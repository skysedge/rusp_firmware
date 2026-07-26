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
