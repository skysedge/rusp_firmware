"""Tests for OLED battery icon fill mapping.

Why these tests exist: top row uses a graphical battery; wrong fill width
misrepresents charge. Digit entry must not show a false Dialing status
(covered in firmware; fill math is tested here).

Failure modes when regressions occur:
- No clamp → fill width out of range.
- Mid charge maps to empty/full icon.
- Split per-column bolt → unreadable flash at mid charge.
"""

import unittest

from battery_ui import (
	LIPO_MAX_MV,
	LIPO_MIN_MV,
	battery_bolt_lit,
	battery_bolt_pixels,
	battery_fill_columns,
	battery_percent_from_mv,
)


class BatteryPercentTests(unittest.TestCase):
	def test_min_is_zero(self):
		# Guards empty-battery floor. Failure: negative percent.
		self.assertEqual(battery_percent_from_mv(LIPO_MIN_MV), 0)
		self.assertEqual(battery_percent_from_mv(LIPO_MIN_MV - 100), 0)

	def test_max_is_hundred(self):
		# Guards full-battery ceiling. Failure: >100%.
		self.assertEqual(battery_percent_from_mv(LIPO_MAX_MV), 100)
		self.assertEqual(battery_percent_from_mv(LIPO_MAX_MV + 100), 100)

	def test_midpoint(self):
		# Guards linear map. Failure: mid voltage not ~50%.
		mid = (LIPO_MIN_MV + LIPO_MAX_MV) // 2
		self.assertEqual(battery_percent_from_mv(mid), 50)


class BatteryFillTests(unittest.TestCase):
	def test_fill_endpoints(self):
		# Guards icon empty/full. Failure: full bar when empty.
		self.assertEqual(battery_fill_columns(0, 10), 0)
		self.assertEqual(battery_fill_columns(100, 10), 10)

	def test_fill_mid(self):
		# Guards proportional fill. Failure: mid charge draws empty/full.
		self.assertEqual(battery_fill_columns(50, 10), 5)


class BatteryBoltTests(unittest.TestCase):
	def test_bolt_is_connected_zigzag(self):
		# Why: flash must read as one glyph, not scattered dots.
		# Failure: too few pixels / not spanning a zigzag.
		pixels = battery_bolt_pixels()
		self.assertGreaterEqual(len(pixels), 20)
		xs = {x for x, _y in pixels}
		ys = {y for _x, y in pixels}
		self.assertTrue(all(2 <= x <= 8 for x, _y in pixels))
		self.assertTrue(all(1 <= y <= 7 for _x, y in pixels))
		self.assertGreaterEqual(len(xs), 5)
		self.assertGreaterEqual(len(ys), 6)

	def test_bolt_one_polarity_vs_level(self):
		# Why: whole flash inverts against level (not per-column).
		# Failure: lit on a full pack → flash blends into the fill.
		self.assertTrue(battery_bolt_lit(0, 10))
		self.assertTrue(battery_bolt_lit(4, 10))
		self.assertFalse(battery_bolt_lit(5, 10))
		self.assertFalse(battery_bolt_lit(10, 10))


if __name__ == "__main__":
	unittest.main()
