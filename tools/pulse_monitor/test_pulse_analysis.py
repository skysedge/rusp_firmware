"""Tests for rotary pulse debounce debug analysis.

Why these tests exist: the host tool must correctly interpret the MCU serial
protocol and reproduce production debounce decisions so bounce vs real pulses
can be distinguished while tuning ROTARY_DEBOUNCE_MS.

Failure modes when regressions occur:
- Mis-parsed lines hide events (event counts drop / fields become None).
- Wrong debounce replay accepts bounce as pulses or drops real pulses.
- Wrong digit mapping (non-digit edge/0) reports the wrong ASCII digit.
"""

import unittest

from pulse_analysis import (
	ROTARY_NONDIGIT_EDGES,
	ROTARY_DEBOUNCE_MS,
	DialSession,
	parse_line,
	pulse_to_ascii,
	replay_debounce,
)


class ParseLineTests(unittest.TestCase):
	def test_parses_ready_banner(self):
		# Guards protocol handshake fields used to confirm the debug sketch
		# is running (not production firmware). Failure: fields stay None and
		# the host cannot assert debounce_ms matches the sketch.
		event = parse_line(
			"READY board=atmega1280 baud=9600 debounce_ms=30 done_ms=200"
		)
		self.assertEqual(event["type"], "READY")
		self.assertEqual(event["board"], "atmega1280")
		self.assertEqual(event["baud"], 9600)
		self.assertEqual(event["debounce_ms"], 30)
		self.assertEqual(event["done_ms"], 200)

	def test_parses_raw_edge(self):
		# Guards microsecond edge telemetry. Failure: dt_us lost so bounce
		# clusters cannot be measured.
		event = parse_line("RAW t_us=123456 dt_us=87")
		self.assertEqual(event["type"], "RAW")
		self.assertEqual(event["t_us"], 123456)
		self.assertEqual(event["dt_us"], 87)

	def test_parses_accepted_and_rejected(self):
		# Guards accept/reject classification from the MCU. Failure: rejected
		# bounce is counted as accepted (or dropped), skewing pulse totals.
		accepted = parse_line("ACCEPTED t_ms=1000 n=1 dt_ms=0")
		rejected = parse_line("REJECTED t_ms=1005 n=1 dt_ms=5")
		self.assertEqual(accepted["type"], "ACCEPTED")
		self.assertEqual(accepted["n"], 1)
		self.assertEqual(rejected["type"], "REJECTED")
		self.assertEqual(rejected["dt_ms"], 5)

	def test_parses_digit_summary(self):
		# Guards end-of-digit report. Failure: raw/accepted mismatch is hidden
		# and the operator cannot see that debounce changed the count.
		event = parse_line(
			"DIGIT raw=12 accepted=11 ascii=0 intervals_ms=98,101,99"
		)
		self.assertEqual(event["type"], "DIGIT")
		self.assertEqual(event["raw"], 12)
		self.assertEqual(event["accepted"], 11)
		self.assertEqual(event["ascii"], "0")
		self.assertEqual(event["intervals_ms"], [98, 101, 99])

	def test_ignores_blank_and_unknown(self):
		# Guards noisy serial. Failure: blank lines become fake events and
		# pollute session stats.
		self.assertIsNone(parse_line(""))
		self.assertIsNone(parse_line("   "))
		self.assertIsNone(parse_line("hello world"))


class PulseToAsciiTests(unittest.TestCase):
	def test_maps_production_pulse_counts(self):
		# Guards the same mapping as rusp_firmware pulse2ascii.
		# Failure: dialed digit display disagrees with production firmware.
		self.assertEqual(ROTARY_NONDIGIT_EDGES, 1)
		cases = [
			(2, "1"),
			(3, "2"),
			(10, "9"),
			(11, "0"),
			(1, "?"),
			(0, "?"),
			(12, "?"),
		]
		for pulses, expected in cases:
			with self.subTest(pulses=pulses):
				self.assertEqual(pulse_to_ascii(pulses), expected)


class ReplayDebounceTests(unittest.TestCase):
	def test_rejects_edges_inside_debounce_window(self):
		# Why: bounce edges a few ms apart must not become extra pulses.
		# Failure without debounce: three edges become three accepted pulses.
		edge_times_ms = [0, 5, 12, 100]
		accepted = replay_debounce(edge_times_ms, debounce_ms=ROTARY_DEBOUNCE_MS)
		self.assertEqual(accepted, [0, 100])

	def test_accepts_edges_just_outside_debounce_window(self):
		# Why: production uses strict '>' not '>='. At exactly 30 ms the edge
		# is still rejected; at 31 ms it is accepted.
		# Failure: off-by-one in comparison flips accept/reject at the boundary.
		self.assertEqual(
			replay_debounce([0, 30], debounce_ms=30),
			[0],
		)
		self.assertEqual(
			replay_debounce([0, 31], debounce_ms=30),
			[0, 31],
		)

	def test_empty_and_single_edge(self):
		# Why: idle / first-edge cases must not crash or invent pulses.
		self.assertEqual(replay_debounce([], debounce_ms=30), [])
		self.assertEqual(replay_debounce([42], debounce_ms=30), [42])


class DialSessionTests(unittest.TestCase):
	def test_tracks_bounce_clusters_and_digit(self):
		# Why: a real dial with bounce should report raw > accepted and the
		# digit from accepted count. Failure: cluster detection misses the
		# bounce group or digit uses raw count.
		session = DialSession(debounce_ms=30, done_ms=200)
		session.handle(parse_line("HALL t_ms=1000"))
		session.handle(parse_line("RAW t_us=1000000 dt_us=0"))
		session.handle(parse_line("ACCEPTED t_ms=1000 n=1 dt_ms=0"))
		session.handle(parse_line("RAW t_us=1005000 dt_us=5000"))
		session.handle(parse_line("REJECTED t_ms=1005 n=1 dt_ms=5"))
		session.handle(parse_line("RAW t_us=1100000 dt_us=95000"))
		session.handle(parse_line("ACCEPTED t_ms=1100 n=2 dt_ms=100"))
		digit = session.handle(
			parse_line("DIGIT raw=3 accepted=2 ascii=1 intervals_ms=100")
		)
		self.assertEqual(digit["ascii"], "1")
		self.assertEqual(session.raw_edge_count, 3)
		self.assertEqual(session.accepted_count, 2)
		self.assertEqual(session.rejected_count, 1)
		self.assertEqual(len(session.bounce_clusters), 1)
		self.assertEqual(session.bounce_clusters[0]["edges"], 2)
		self.assertEqual(session.bounce_clusters[0]["span_us"], 5000)


if __name__ == "__main__":
	unittest.main()
