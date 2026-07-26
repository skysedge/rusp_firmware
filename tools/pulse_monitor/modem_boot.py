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
BOOT_ADVISORY = ("+CLVL=6", "+UEXTDCONF=0,1")


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
