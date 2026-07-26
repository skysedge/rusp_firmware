"""Tests for pulse-only spring-return digit extraction.

Why these tests exist: hall-based windows corrupted digits (wind counted in,
late hall truncated). Extraction must use relative return regularity only.

Failure modes when regressions occur:
- Counting wind+return → digit too high (2→4, 5→0 in DIAL.LOG).
- Committing irregular wind on short quiet → false digit before return.
- Requiring exact ~100 ms cadence → rejects real spring jitter.
- Picking a short regular mid-train over the full return suffix → truncate.
"""

import unittest

from dial_extract import (
	FALLBACK_COMMIT_MS,
	MAX_REL_DEV_PCT,
	MIN_PULSES_FOR_DIGIT,
	PULSES_DONE_MS,
	debounce_times,
	extract_digit_after_quiet,
	find_regular_return_suffix,
	max_rel_dev_pct,
	should_commit_after_quiet,
)


def _burst(start_ms: int, pulses: int, period_ms: int = 100) -> list:
	return [start_ms + i * period_ms for i in range(pulses)]


def _jittered_return(start_ms: int, pulses: int, periods_ms: list) -> list:
	"""Return burst with explicit inter-pulse periods (relative regularity)."""
	assert len(periods_ms) == pulses - 1
	t = start_ms
	out = [t]
	for p in periods_ms:
		t += p
		out.append(t)
	return out


class DebounceTests(unittest.TestCase):
	def test_rejects_bounce(self):
		# Guards ROTARY_DEBOUNCE_MS. Failure: bounce inflates pulse count.
		self.assertEqual(debounce_times([1000, 1010, 1100, 1200]), [1000, 1100, 1200])


class RegularityTests(unittest.TestCase):
	def test_uniform_return_is_regular(self):
		# Guards spring-like uniform spacing. Failure: rejects good returns.
		edges = _burst(1000, 10)
		self.assertEqual(max_rel_dev_pct(edges), 0)
		self.assertIsNotNone(find_regular_return_suffix(edges))

	def test_relative_jitter_still_regular(self):
		# Guards relative (not absolute) cadence: 80/100/120 around mean 100
		# is 20% — under MAX_REL_DEV_PCT. Failure: requires exact 100 ms.
		edges = _jittered_return(1000, 4, [80, 100, 120])
		self.assertLessEqual(max_rel_dev_pct(edges), MAX_REL_DEV_PCT)
		self.assertIsNotNone(find_regular_return_suffix(edges))

	def test_irregular_wind_not_regular_suffix_alone_when_merged(self):
		# Guards: irregular wind + uniform return → suffix is return only.
		wind = [1000, 1180, 1450, 1600]  # scattered finger timing
		ret = _burst(1800, 6)  # digit 5
		result = extract_digit_after_quiet(wind + ret)
		self.assertEqual(result.source, "regular_suffix")
		self.assertEqual(result.ascii, "5")
		self.assertEqual(result.pulses, 6)


class ExtractDigitTests(unittest.TestCase):
	def test_return_only_digits(self):
		# Guards clean return-only trains. Digit '1' (2 pulses) uses
		# best_effort — a lone pair is ambiguous with wind until fallback.
		for pulses, ascii in [(6, "5"), (10, "9"), (11, "0")]:
			with self.subTest(ascii=ascii):
				r = extract_digit_after_quiet(_burst(1000, pulses))
				self.assertEqual(r.ascii, ascii)
				self.assertEqual(r.source, "regular_suffix")
		r1 = extract_digit_after_quiet(_burst(1000, 2))
		self.assertEqual(r1.ascii, "1")
		self.assertEqual(r1.source, "best_effort")

	def test_digit_one_after_rejected_wind_is_regular(self):
		# Guards: when grow rejects wind and leaves 2 return pulses, that
		# '1' is trusted immediately (not a lone wind pair).
		wind = [1000, 1300, 1600]
		ret = _burst(1900, 2)
		r = extract_digit_after_quiet(wind + ret, allow_best_effort=False)
		self.assertEqual(r.ascii, "1")
		self.assertEqual(r.source, "regular_suffix")

	def test_merged_wind_return_like_log_two_as_four(self):
		# Guards DIAL.LOG failure mode: dial 2 → 5 pulses (wind+return) → '4'.
		# Irregular wind 2 + regular return 3 must yield '2', not '4'.
		wind = [1000, 1250]  # slow irregular
		ret = _burst(1400, 3)  # digit 2
		# Whole train debounced length 5 → would be '4' if counted raw.
		self.assertEqual(len(debounce_times(wind + ret)), 5)
		result = extract_digit_after_quiet(wind + ret)
		self.assertEqual(result.ascii, "2")
		self.assertEqual(result.pulses, 3)

	def test_merged_wind_return_five_not_zero(self):
		# Guards dial 5 → 11 pulses → '0' when wind counted in.
		wind = [1000, 1200, 1500, 1700, 1950]
		ret = _burst(2100, 6)
		self.assertGreaterEqual(len(debounce_times(wind + ret)), 11)
		result = extract_digit_after_quiet(wind + ret)
		self.assertEqual(result.ascii, "5")
		self.assertEqual(result.pulses, 6)

	def test_late_hall_style_full_return_still_found(self):
		# Guards: even if analysis starts mid-train, longest regular suffix
		# is the full uniform return (former 9→6 truncate case).
		ret = _burst(1000, 10)
		result = extract_digit_after_quiet(ret)
		self.assertEqual(result.ascii, "9")
		self.assertEqual(result.pulses, 10)

	def test_does_not_prefer_short_prefix_over_full_return(self):
		# Guards longest-suffix rule: a short regular slice must not beat
		# the full return (would map 9→ smaller digit).
		ret = _burst(1000, 10)
		found = find_regular_return_suffix(ret)
		self.assertIsNotNone(found)
		self.assertEqual(len(found[0]), 10)

	def test_discards_single_pulse(self):
		# Guards MIN_PULSES. Failure: noise becomes '?'.
		result = extract_digit_after_quiet([1000], allow_best_effort=False)
		self.assertEqual(result.source, "none")
		self.assertLess(result.pulses, MIN_PULSES_FOR_DIGIT)

	def test_best_effort_lone_digit_one(self):
		# Guards fallback path for a lone 2-pulse train (digit 1): not marked
		# regular_suffix (ambiguous with wind) but best_effort still yields '1'.
		result = extract_digit_after_quiet(_burst(1000, 2), allow_best_effort=True)
		self.assertEqual(result.source, "best_effort")
		self.assertEqual(result.ascii, "1")


class CommitTimingTests(unittest.TestCase):
	def test_regular_return_commits_after_pulses_done(self):
		# Guards snappy commit once spring return looks regular.
		self.assertFalse(should_commit_after_quiet(PULSES_DONE_MS, True))
		self.assertTrue(should_commit_after_quiet(PULSES_DONE_MS + 1, True))

	def test_irregular_waits_for_fallback(self):
		# Guards: wind pause must not commit before return can arrive.
		self.assertFalse(
			should_commit_after_quiet(PULSES_DONE_MS + 1, False)
		)
		self.assertFalse(
			should_commit_after_quiet(FALLBACK_COMMIT_MS, False)
		)
		self.assertTrue(
			should_commit_after_quiet(FALLBACK_COMMIT_MS + 1, False)
		)


if __name__ == "__main__":
	unittest.main()
