"""
Why: guards warm-reset modem init — MCU USB reset must not pulse CELL_ON
or wait for +PACSP1 when the module is already powered.
Failure: cold-start path skipped (modem never turns on) or warm path still
pulses/waits (timeout spam / accidental power-off).
"""

import unittest

from modem_boot import (
	modem_already_powered,
	should_pulse_cell_on,
	should_wait_for_pacsp,
)


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
