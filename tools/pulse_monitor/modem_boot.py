"""
Modem power-on policy for lara_on().

When the MCU resets (USB serial DTR) the LARA module often stays powered.
Pulsing CELL_ON then would toggle it off, and +PACSP1 will not be re-sent.
"""


def modem_already_powered(pwr_det_high: bool, at_responds: bool = False) -> bool:
	"""
	True when the module is up: PWR_DET HIGH, or AT responds.

	PWR_DET alone is not trusted — a false LOW plus a CELL_ON pulse toggles
	a running modem off (then CELL_PWR_DET wait times out).
	"""
	return bool(pwr_det_high) or bool(at_responds)


def should_pulse_cell_on(pwr_det_high: bool, at_responds: bool = False) -> bool:
	"""
	Pulse CELL_ON only for a cold start.

	If the modem is already on, a ~1s PWR_ON pulse turns it off (u-blox
	toggle). Skip when PWR_DET is HIGH or a short AT probe succeeds.
	"""
	return not modem_already_powered(pwr_det_high, at_responds)


def should_wait_for_pacsp(pwr_det_high: bool, at_responds: bool = False) -> bool:
	"""
	Wait for the +PACSP1 boot URC only after a cold power-on pulse.

	Skip when the modem was already up (PWR_DET or AT) — URC will not repeat.
	"""
	return should_pulse_cell_on(pwr_det_high, at_responds)


# Commands whose failure means the phone cannot report call state honestly.
# &K0 is here because the u-blox default is &K3 (RTS/CTS hardware flow
# control). The board sets CELL_RTS as an output and never drives it, so the
# module can gate its own transmitter and stop part-way through a message —
# observed as URCs arriving as a lone "R" or "+", and as AT+CSQ timing out.
# Unsolicited output has no retry, so a throttled UART loses calls outright.
BOOT_MANDATORY = ("&K0", "+CMEE=2", "+UCALLSTAT=1")

# Nice to have; the phone still places and receives calls without them.
# +CLIP=1 is here rather than in BOOT_MANDATORY because caller identification
# is a subscription service: a SIM or network that withholds it must not make
# the phone declare itself unfit to boot, since calls place and receive fine.
BOOT_ADVISORY = ("+CLIP=1", "+CLVL=6", "+UEXTDCONF=0,1")


def boot_config_sequence() -> list[str]:
	"""Ordered AT configuration applied once the modem answers.

	Order is load-bearing, not cosmetic:
	  * E0 first, so no later response is polluted by a command echo.
	  * &K0 before anything that depends on the modem talking freely.
	  * +CMEE=2 before the rest, so any later failure is readable text.
	"""
	return ["E0", *BOOT_MANDATORY, *BOOT_ADVISORY]


def boot_command_is_mandatory(command: str) -> bool:
	"""Should lara_on() report failure when this command is refused?"""
	return command in BOOT_MANDATORY


# Carrier profile the module should be provisioned with.
#
# LARA-R6 firmware 02.14 offers only 1 (SIM ICCID select), 90 (Global) and
# 201 (GCF-PTCRB certification). Global applies no operator-specific IMS
# configuration, and a unit left on it was seen to register IMS and receive
# VoLTE calls normally while every outgoing call rang the far end and never
# completed the answer back -- the module held it in "dialing" until the
# network timed it out, so it never opened the audio path. 1 takes the
# operator's configuration from the SIM, which is what Global lacks.
MNO_PROFILE_DESIRED = 1

_MNO_PREFIX = "+UMNOPROF:"


def parse_mno_profile(line: str) -> int | None:
	"""The profile number from a +UMNOPROF query, or None if unreadable."""
	if not line or not line.startswith(_MNO_PREFIX):
		return None
	value = line[len(_MNO_PREFIX):].strip()
	if not value.isdigit():
		return None
	return int(value)


def mno_profile_change_needed(current: int | None) -> bool:
	"""Whether the module needs reprovisioning to the desired profile.

	An unreadable current value is deliberately treated as "leave alone".
	Writing the profile reboots the module, so acting on an unknown value
	risks rebooting, failing the query again, and rebooting forever --
	a phone that never finishes starting up. A modem that cannot answer
	the query has a larger problem than its carrier profile.
	"""
	if current is None:
		return False
	return current != MNO_PROFILE_DESIRED


def mno_profile_commands(profile: int) -> list[str]:
	"""The ordered sequence that provisions `profile`.

	Order is the behaviour, not presentation. The module refuses the write
	while attached to a network, and the new profile only takes effect
	after a reboot. Skipping either step leaves the setting unapplied
	while appearing to have succeeded.

	The reboot discards the session configuration applied before it, so
	the caller must run boot_config_sequence() again afterwards.
	"""
	return ["+COPS=2", f"+UMNOPROF={profile}", "+CFUN=15"]
