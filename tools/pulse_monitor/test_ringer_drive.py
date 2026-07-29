"""
Why: incoming RING was rebooting the board; the H-bridge ran 100% of alert
time (LEDs already used the cadence). Failure without this policy: coil
drive stays on during long pauses → brownout / reset on first ring.
"""

import unittest

from ringer_drive import (
	RING_LONG_PAUSE_MS,
	RING_ON_DURATION_MS,
	RING_PATTERN_TOTAL_MS,
	RING_SHORT_PAUSE_MS,
	ring_cadence_on,
	ringer_bridge_outputs,
)


class TestRingerDrive(unittest.TestCase):
	def test_cadence_on_during_first_burst(self):
		self.assertTrue(ring_cadence_on(0))
		self.assertTrue(ring_cadence_on(RING_ON_DURATION_MS - 1))

	def test_cadence_off_during_pauses(self):
		# Short pause between bursts.
		self.assertFalse(ring_cadence_on(RING_ON_DURATION_MS))
		# Long pause after second burst.
		after_second = (
			RING_ON_DURATION_MS
			+ RING_SHORT_PAUSE_MS
			+ RING_ON_DURATION_MS
		)
		self.assertFalse(ring_cadence_on(after_second))
		self.assertFalse(
			ring_cadence_on(RING_PATTERN_TOTAL_MS - RING_LONG_PAUSE_MS)
		)

	def test_bridge_idle_when_cadence_off(self):
		# Failure: returning a driven polarity during pause keeps the coil
		# energized across the 2 s gap and can brown out the rail.
		self.assertEqual(ringer_bridge_outputs(False, True), (0, 0))
		self.assertEqual(ringer_bridge_outputs(False, False), (0, 0))

	def test_bridge_never_both_high(self):
		self.assertEqual(ringer_bridge_outputs(True, True), (1, 0))
		self.assertEqual(ringer_bridge_outputs(True, False), (0, 1))
		for bit in (True, False):
			p, n = ringer_bridge_outputs(True, bit)
			self.assertFalse(p == 1 and n == 1)


if __name__ == "__main__":
	unittest.main()
