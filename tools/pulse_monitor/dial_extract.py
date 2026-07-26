"""Pulse-only dial digit extraction (mirrors rusp_firmware.ino).

Hall is not used. Wind (finger) has irregular intervals; return (spring) is
more uniform. Digit detection grows a trailing suffix from the end of the
activity: each earlier pulse is included only while its gap stays close to
the mean gap of the pulses already accepted (relative regularity).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple

from pulse_analysis import ROTARY_DEBOUNCE_MS, pulse_to_ascii

# Quiet after last edge before considering a commit.
PULSES_DONE_MS = 200
# If no regular return yet, wait this long before best-effort fallback
# (avoids committing irregular wind during a pause before return).
FALLBACK_COMMIT_MS = 500
# Valid raw pulse counts after debounce (less the non-digit edge, '1'..'0').
MIN_PULSES_FOR_DIGIT = 2
MAX_PULSES_FOR_DIGIT = 11
# Max |new_gap - mean_return_gap| as a percentage of mean. Relative cadence.
MAX_REL_DEV_PCT = 45


@dataclass(frozen=True)
class DigitExtractResult:
	pulses: int
	ascii: str
	source: str  # "regular_suffix" | "best_effort" | "none"
	rel_dev_pct: int


def debounce_times(
	edge_times_ms: Sequence[int],
	debounce_ms: int = ROTARY_DEBOUNCE_MS,
) -> List[int]:
	"""Accepted edge timestamps (first always kept; later need > debounce_ms)."""
	out: List[int] = []
	for t in edge_times_ms:
		if not out or (t - out[-1]) > debounce_ms:
			out.append(t)
	return out


def max_rel_dev_pct(edge_times_ms: Sequence[int]) -> int:
	"""Max |iv - mean| / mean as a percentage. 0 for a single interval."""
	if len(edge_times_ms) < 2:
		return 999
	if len(edge_times_ms) == 2:
		return 0
	intervals = [
		edge_times_ms[i] - edge_times_ms[i - 1]
		for i in range(1, len(edge_times_ms))
	]
	mean = sum(intervals) // len(intervals)
	if mean <= 0:
		return 999
	worst = 0
	for iv in intervals:
		pct = (abs(iv - mean) * 100) // mean
		if pct > worst:
			worst = pct
	return worst


def grow_return_suffix(
	accepted: Sequence[int],
	max_pct: int = MAX_REL_DEV_PCT,
) -> Optional[Tuple[List[int], int]]:
	"""Grow a return suffix from the end using relative gap agreement.

	Starts at the last pulse and prepends earlier pulses while the new gap
	stays within max_pct of the mean gap of the suffix so far. Finger-wind
	gaps typically disagree with the spring mean and stop the walk.
	"""
	n = len(accepted)
	if n < MIN_PULSES_FOR_DIGIT:
		return None

	start = n - 1
	while start > 0 and (n - start) < MAX_PULSES_FOR_DIGIT:
		prev = start - 1
		new_gap = accepted[start] - accepted[prev]
		if new_gap <= 0:
			break
		suffix_len = n - start
		if suffix_len == 1:
			# First interval seeds the return; no mean to compare yet.
			start = prev
			continue
		# Mean of intervals already in the suffix (accepted[start .. n)).
		interval_sum = accepted[n - 1] - accepted[start]
		interval_count = suffix_len - 1
		mean = interval_sum // interval_count
		if mean <= 0:
			break
		pct = (abs(new_gap - mean) * 100) // mean
		if pct > max_pct:
			break
		start = prev

	suffix = list(accepted[start:])
	if len(suffix) < MIN_PULSES_FOR_DIGIT:
		return None
	if len(suffix) > MAX_PULSES_FOR_DIGIT:
		suffix = suffix[-MAX_PULSES_FOR_DIGIT:]
	return suffix, max_rel_dev_pct(suffix)


def find_regular_return_suffix(
	edge_times_ms: Sequence[int],
	max_pct: int = MAX_REL_DEV_PCT,
) -> Optional[Tuple[List[int], int]]:
	"""Return suffix grown from the end with relative regularity.

	A 2-pulse suffix (digit '1') is only trusted when earlier pulses were
	rejected as wind — a lone pair cannot be distinguished from wind yet.
	"""
	accepted = debounce_times(edge_times_ms)
	grown = grow_return_suffix(accepted, max_pct=max_pct)
	if grown is None:
		return None
	suffix, pct = grown
	if len(suffix) > 2 and pct > max_pct:
		return None
	if len(suffix) == 2 and len(accepted) == 2:
		# Lone pair: wait for fallback quiet / best_effort.
		return None
	return grown


def find_best_effort_suffix(
	edge_times_ms: Sequence[int],
) -> Optional[Tuple[List[int], int]]:
	"""Grow with a looser relative cap, then pick lowest-dev valid length trim."""
	accepted = debounce_times(edge_times_ms)
	grown = grow_return_suffix(accepted, max_pct=100)
	if grown is None:
		return None
	suffix, _ = grown
	best: Optional[Tuple[List[int], int]] = None
	# Consider trailing trims of the grown suffix (still a suffix of activity).
	for cut in range(0, len(suffix) - MIN_PULSES_FOR_DIGIT + 1):
		cand = suffix[cut:]
		if len(cand) > MAX_PULSES_FOR_DIGIT:
			continue
		pct = max_rel_dev_pct(cand)
		if best is None or pct < best[1] or (
			pct == best[1] and len(cand) > len(best[0])
		):
			best = (cand, pct)
	return best


def should_commit_after_quiet(
	quiet_ms: int,
	has_regular_return: bool,
	pulses_done_ms: int = PULSES_DONE_MS,
	fallback_ms: int = FALLBACK_COMMIT_MS,
) -> bool:
	"""Commit when quiet and a regular return is ready, else after fallback."""
	if quiet_ms <= pulses_done_ms:
		return False
	if has_regular_return:
		return True
	return quiet_ms > fallback_ms


def extract_digit_after_quiet(
	edge_times_ms: Sequence[int],
	allow_best_effort: bool = True,
) -> DigitExtractResult:
	"""Extract digit from a finished activity (hall ignored)."""
	if not edge_times_ms:
		return DigitExtractResult(0, "?", "none", 999)

	regular = find_regular_return_suffix(edge_times_ms)
	if regular is not None:
		suffix, pct = regular
		pulses = len(suffix)
		ascii = pulse_to_ascii(pulses)
		if ascii != "?":
			return DigitExtractResult(pulses, ascii, "regular_suffix", pct)

	if allow_best_effort:
		effort = find_best_effort_suffix(edge_times_ms)
		if effort is not None:
			suffix, pct = effort
			pulses = len(suffix)
			ascii = pulse_to_ascii(pulses)
			if ascii != "?":
				return DigitExtractResult(pulses, ascii, "best_effort", pct)

	accepted = debounce_times(edge_times_ms)
	return DigitExtractResult(
		len(accepted),
		"?",
		"none",
		max_rel_dev_pct(accepted) if len(accepted) >= 2 else 999,
	)
