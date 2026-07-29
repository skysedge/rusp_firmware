"""
Physical ringer H-bridge drive policy.

Why: driving RINGER_P/N for the whole alert window (not just the cadence ON
periods) can brown out the board on the first incoming RING. LEDs already
use should_pulse; the coil must match.
"""

from __future__ import annotations

RING_ON_DURATION_MS = 400
RING_SHORT_PAUSE_MS = 200
RING_ON_DURATION2_MS = 400
RING_LONG_PAUSE_MS = 2000
RING_PATTERN_TOTAL_MS = (
	RING_ON_DURATION_MS
	+ RING_SHORT_PAUSE_MS
	+ RING_ON_DURATION2_MS
	+ RING_LONG_PAUSE_MS
)


def ring_cadence_on(pattern_time_ms: int) -> bool:
	"""True during the two ON bursts of the classic double-ring pattern."""
	t = int(pattern_time_ms) % RING_PATTERN_TOTAL_MS
	if t < RING_ON_DURATION_MS:
		return True
	if t < RING_ON_DURATION_MS + RING_SHORT_PAUSE_MS:
		return False
	if t < RING_ON_DURATION_MS + RING_SHORT_PAUSE_MS + RING_ON_DURATION2_MS:
		return True
	return False


def ringer_bridge_outputs(cadence_on: bool, polarity_bit: bool) -> tuple[int, int]:
	"""
	(RINGER_P, RINGER_N) levels.

	When cadence is off both legs are LOW (idle). When on, one leg HIGH —
	never both HIGH (H-bridge shoot-through).
	"""
	if not cadence_on:
		return (0, 0)
	if polarity_bit:
		return (1, 0)
	return (0, 1)
