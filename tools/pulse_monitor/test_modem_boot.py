"""
Why: guards warm-reset modem init — MCU USB reset must not pulse CELL_ON
or wait for +PACSP1 when the module is already powered.
Failure: cold-start path skipped (modem never turns on) or warm path still
pulses/waits (timeout spam / accidental power-off).
"""

import unittest

from modem_boot import (
	MNO_PROFILE_DESIRED,
	mno_profile_change_needed,
	mno_profile_commands,
	parse_mno_profile,
	BOOT_ADVISORY,
	BOOT_MANDATORY,
	boot_command_is_mandatory,
	boot_config_sequence,
	modem_already_powered,
	should_pulse_cell_on,
	should_wait_for_pacsp,
)


class TestBootConfigSequence(unittest.TestCase):
	"""Ordering constraints in the post-power-on AT configuration.

	Why: the modem was left on the u-blox default &K3 (RTS/CTS flow
	control) while the board never drives CELL_RTS. The module gated its
	own transmitter part-way through messages — URCs arrived as a lone "R"
	or "+", AT+CSQ hit its 1 s timeout, and an incoming call was never
	announced, so the phone did not ring.
	"""

	def test_flow_control_is_disabled_before_urcs_are_enabled(self):
		# The regression this exists for. +UCALLSTAT=1 turns on unsolicited
		# output, which has no retry; if the UART can still be throttled at
		# that point, call announcements are lost with no way to recover.
		# Failure: &K0 after (or missing) → incoming calls silently dropped.
		seq = boot_config_sequence()
		self.assertIn("&K0", seq)
		self.assertLess(seq.index("&K0"), seq.index("+UCALLSTAT=1"))

	def test_echo_off_is_first(self):
		# Why: every later response is parsed line by line, and an echoed
		# command is an extra line. Failure: first configured command's
		# reply is misread as the echo of the one before it.
		self.assertEqual(boot_config_sequence()[0], "E0")

	def test_verbose_errors_precede_the_commands_they_describe(self):
		# Why: +CMEE=2 is what turns a bare ERROR into readable text.
		# Failure: a refused +UCALLSTAT=1 reports nothing diagnosable.
		seq = boot_config_sequence()
		self.assertLess(seq.index("+CMEE=2"), seq.index("+UCALLSTAT=1"))

	def test_flow_control_failure_is_mandatory_not_advisory(self):
		# Why: booting to "ready" with a throttled UART is a lie — the
		# phone cannot reliably receive calls. Failure: the error is
		# swallowed and the fault presents as random missed calls.
		self.assertTrue(boot_command_is_mandatory("&K0"))

	def test_audio_tuning_stays_advisory(self):
		# Why: volume and audio-path config must not block booting.
		# Failure: a refused +CLVL=6 reports the modem as broken.
		for command in BOOT_ADVISORY:
			self.assertFalse(boot_command_is_mandatory(command))

	def test_caller_id_reporting_is_enabled(self):
		# Why: without +CLIP=1 the modem never reports who is calling and
		# the bottom line has nothing to show during an incoming call.
		# Failure: every incoming call displays as anonymous.
		self.assertIn("+CLIP=1", boot_config_sequence())

	def test_caller_id_is_advisory_not_mandatory(self):
		# Why: caller ID is a convenience. A network or SIM that refuses
		# it must not make the phone declare itself unfit to boot, since
		# calls still place and receive perfectly well without it.
		# Failure: a refused +CLIP=1 blocks startup entirely.
		self.assertNotIn("+CLIP=1", BOOT_MANDATORY)
		self.assertIn("+CLIP=1", BOOT_ADVISORY)

	def test_every_mandatory_command_is_actually_sent(self):
		# Guards the two lists drifting apart. Failure: a command is
		# declared mandatory but never issued, so the check is dead code.
		seq = boot_config_sequence()
		for command in BOOT_MANDATORY:
			self.assertIn(command, seq)

	def test_advisory_commands_follow_all_mandatory_ones(self):
		# Why: a slow advisory command must not delay the config that
		# decides whether calls work. Failure: mandatory setup is applied
		# late, widening the window where URCs are lost.
		seq = boot_config_sequence()
		last_mandatory = max(seq.index(c) for c in BOOT_MANDATORY)
		for command in BOOT_ADVISORY:
			self.assertGreater(seq.index(command), last_mandatory)


class TestModemBoot(unittest.TestCase):
	def test_cold_start_pulses_and_waits_pacsp(self):
		# PWR_DET low and no AT → need power pulse and boot URC.
		self.assertFalse(modem_already_powered(False, False))
		self.assertTrue(should_pulse_cell_on(False, False))
		self.assertTrue(should_wait_for_pacsp(False, False))

	def test_warm_mcu_reset_skips_pulse_and_pacsp(self):
		# PWR_DET already high → modem up; +PACSP1 will not reappear.
		self.assertTrue(modem_already_powered(True, False))
		self.assertFalse(should_pulse_cell_on(True, False))
		self.assertFalse(should_wait_for_pacsp(True, False))

	def test_pwr_det_false_low_but_at_ok_skips_pulse(self):
		# Why: false PWR_DET LOW + pulse toggles modem off (OLED/side-LED
		# symptom window after a bad boot). Failure: still pulses.
		self.assertTrue(modem_already_powered(False, True))
		self.assertFalse(should_pulse_cell_on(False, True))
		self.assertFalse(should_wait_for_pacsp(False, True))


if __name__ == "__main__":
	unittest.main()


class MnoProfileTests(unittest.TestCase):
	"""Carrier profile provisioning.

	A LARA-R6 shipped on the generic "Global" profile (90) carries no
	operator-specific IMS configuration. Observed on an Ultra Mobile
	(T-Mobile US) SIM: IMS registers, incoming VoLTE calls connect with
	audio, but outgoing calls ring the far end and never complete the
	answer back, so the module holds them in "dialing" until the network
	times them out. Profile 1 reads the operator from the SIM instead.

	The value lives in the module's non-volatile memory and survives power
	cycles, so this is provisioning, not configuration: check first and
	write only when wrong, or every boot would reboot the modem.
	"""

	def test_parses_the_profile_from_the_query(self):
		# Why: the whole decision rests on this number.
		# Failure: a readable profile looks unreadable, so a misprovisioned
		# device is never corrected.
		self.assertEqual(parse_mno_profile("+UMNOPROF: 90"), 90)

	def test_parses_without_a_space_after_the_colon(self):
		# Why: the space is not guaranteed by the response format.
		# Failure: every device reads as unknown and none is corrected.
		self.assertEqual(parse_mno_profile("+UMNOPROF:1"), 1)

	def test_unreadable_response_yields_none(self):
		# Why: a truncated or unrelated line must not parse as a number.
		# Failure: garbage becomes a profile value and provokes a write.
		self.assertIsNone(parse_mno_profile("+UMNOPROF:"))
		self.assertIsNone(parse_mno_profile("OK"))
		self.assertIsNone(parse_mno_profile(""))

	def test_correct_profile_needs_no_change(self):
		# Why: the steady state for every already-provisioned device.
		# Failure: the modem reboots on every single startup, dropping
		# registration and adding tens of seconds to boot.
		self.assertFalse(mno_profile_change_needed(MNO_PROFILE_DESIRED))

	def test_global_profile_needs_changing(self):
		# Why: the measured state of the affected device.
		# Failure: the outgoing-call fault is never corrected.
		self.assertTrue(mno_profile_change_needed(90))

	def test_certification_profile_needs_changing(self):
		# Why: 201 is a lab profile; a device left on it in the field is
		# just as wrong as one on Global.
		# Failure: a test-configured unit ships broken.
		self.assertTrue(mno_profile_change_needed(201))

	def test_unreadable_profile_is_left_alone(self):
		# Why: if the query failed we do not know the current value.
		# Writing anyway means rebooting the module, re-querying, failing
		# again, and rebooting forever.
		# Failure: a modem that cannot answer the query is stuck in a
		# reboot loop and the phone never finishes starting up.
		self.assertFalse(mno_profile_change_needed(None))

	def test_change_deregisters_before_writing_and_reboots_after(self):
		# Why: the write is rejected while attached to a network, and it
		# only takes effect after a module reboot. Order is the whole
		# behaviour here.
		# Failure: the command is refused, or it is accepted but never
		# applied, leaving outgoing calls broken while appearing fixed.
		self.assertEqual(
			mno_profile_commands(MNO_PROFILE_DESIRED),
			["+COPS=2", f"+UMNOPROF={MNO_PROFILE_DESIRED}", "+CFUN=15"],
		)
