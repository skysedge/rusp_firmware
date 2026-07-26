"""Battery icon fill helpers for the OLED top row.

Why: line 1 is status + a graphical battery (not ASCII #/% clutter).
Percent mapping matches the original RUSP Helpers.ino LiPo curve.
"""

from __future__ import annotations

LIPO_MAX_MV = 4150
LIPO_MIN_MV = 3500


def battery_percent_from_mv(
	mv: int,
	*,
	min_mv: int = LIPO_MIN_MV,
	max_mv: int = LIPO_MAX_MV,
) -> int:
	span = max_mv - min_mv
	if span <= 0:
		return 0
	pct = ((mv - min_mv) * 100) // span
	if pct > 100:
		return 100
	if pct < 0:
		return 0
	return pct


def battery_fill_columns(pct: int, inner_cols: int) -> int:
	"""How many inner body columns to fill for pct (0..100)."""
	if pct < 0:
		pct = 0
	if pct > 100:
		pct = 100
	if inner_cols <= 0:
		return 0
	filled = (pct * inner_cols + 50) // 100
	if filled > inner_cols:
		filled = inner_cols
	return filled


def battery_bolt_pixels() -> tuple[tuple[int, int], ...]:
	"""
	Classic lightning-flash pixels relative to the battery icon top-left.

	12×10 body; flash sits in the inner well. One polarity for the whole
	glyph (see battery_bolt_lit) so the shape stays readable.
	"""
	# Dense, recognizable zigzag (not a sparse sprinkle of dots).
	return (
		(5, 1), (6, 1), (7, 1), (8, 1),
		(4, 2), (5, 2), (6, 2), (7, 2),
		(2, 3), (3, 3), (4, 3), (5, 3), (6, 3), (7, 3),
		(5, 4), (6, 4), (7, 4),
		(3, 5), (4, 5), (5, 5), (6, 5),
		(2, 6), (3, 6), (4, 6),
		(3, 7), (4, 7),
	)


def battery_bolt_lit(filled_inner_cols: int, inner_cols: int = 10) -> bool:
	"""
	Whole-flash polarity inverted against overall battery level.

	Lit (bright) when the pack is under half full; dark when half or more
	is filled — so the flash is never split mid-glyph across the fill edge.
	"""
	if inner_cols <= 0:
		return True
	return filled_inner_cols * 2 < inner_cols
