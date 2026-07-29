"""Tests for CSQ → OLED signal bar count.

Why these tests exist: left icon must reflect modem RSSI without false
full/empty bars.

Failure modes when regressions occur:
- Treating 99 (unknown) as full signal.
- Threshold drift → mid RSSI shows 0 or 4 bars.
"""

import unittest

from signal_bars import signal_bars_from_rssi


class SignalBarsTests(unittest.TestCase):
	def test_unknown_and_zero(self):
		# Guards no-service display. Failure: shows full bars when unknown.
		self.assertEqual(signal_bars_from_rssi(99), 0)
		self.assertEqual(signal_bars_from_rssi(0), 0)

	def test_thresholds(self):
		# Guards bar steps. Failure: wrong height for a given RSSI.
		self.assertEqual(signal_bars_from_rssi(1), 1)
		self.assertEqual(signal_bars_from_rssi(8), 2)
		self.assertEqual(signal_bars_from_rssi(15), 3)
		self.assertEqual(signal_bars_from_rssi(22), 4)
		self.assertEqual(signal_bars_from_rssi(31), 4)


if __name__ == "__main__":
	unittest.main()
