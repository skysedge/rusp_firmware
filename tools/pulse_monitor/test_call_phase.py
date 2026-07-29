"""Tests for UCALLSTAT / CLCC → call-phase OLED labels.

Why these tests exist: the UI must distinguish dialing, remote ringing,
answered/in-call, and incoming — not lump everything as CALLING. Answered
detection also depends on parsing URCs that may include spaces, and on
AT+CLCC polls when a URC was swallowed by an AT expect.

Failure modes when regressions occur:
- Alerting (3) labeled In call → looks answered while still ringing.
- Connected/active not mapped → stays on Dialing/Ringing after answer.
- Space-padded UCALLSTAT ignored → answer URC never updates OLED.
- CLCC active not preferred → stays Ringing while already talking.
"""

import unittest

from call_phase import (
	call_phase_label,
	parse_clcc_stat,
	parse_ucallstat_stat,
	prefer_clcc_stat,
)


class CallPhaseTests(unittest.TestCase):
	def test_outbound_progression(self):
		# Guards MO phases. Failure: Ringing/In call labels wrong.
		self.assertEqual(call_phase_label(2), "Dialing")
		self.assertEqual(call_phase_label(3), "Ringing")
		self.assertEqual(call_phase_label(7), "In call")
		self.assertEqual(call_phase_label(0), "In call")

	def test_incoming_and_end(self):
		# Guards MT ring + disconnect. Failure: Incoming/ended lost.
		self.assertEqual(call_phase_label(4), "Incoming")
		self.assertEqual(call_phase_label(6), "Call ended")

	def test_ignored_codes(self):
		# Guards hold/waiting. Failure: spurious status text.
		self.assertIsNone(call_phase_label(1))
		self.assertIsNone(call_phase_label(5))


class UcallstatParseTests(unittest.TestCase):
	def test_compact_and_spaced(self):
		# Why: modem may emit '+UCALLSTAT: 1, 7'. Failure: answer ignored.
		self.assertEqual(parse_ucallstat_stat("1,7"), 7)
		self.assertEqual(parse_ucallstat_stat("1,0"), 0)
		self.assertEqual(parse_ucallstat_stat(" 1, 0"), 0)
		self.assertEqual(parse_ucallstat_stat("1,6,1"), 6)

	def test_bad_payload(self):
		# Why: garbage must not invent a phase. Failure: random UI flip.
		self.assertIsNone(parse_ucallstat_stat(""))
		self.assertIsNone(parse_ucallstat_stat("1"))


class ClccParseTests(unittest.TestCase):
	def test_active_alerting_dialling(self):
		# Why: CLCC is the backup when UCALLSTAT answer is swallowed.
		# Failure: cannot tell answered from ringing via poll.
		self.assertEqual(
			parse_clcc_stat('+CLCC: 1,0,0,0,0,"5551234",129'),
			0,
		)
		self.assertEqual(
			parse_clcc_stat('+CLCC: 1,0,3,0,0,"5551234",129'),
			3,
		)
		self.assertEqual(
			parse_clcc_stat('+CLCC: 1,0,2,0,0,"5551234",129'),
			2,
		)

	def test_prefer_active_over_alerting(self):
		# Why: multiparty / duplicate lines — answered must win.
		# Failure: UI stays Ringing while a leg is already active.
		self.assertEqual(prefer_clcc_stat([3, 0]), 0)
		self.assertEqual(prefer_clcc_stat([2, 3]), 3)
		self.assertIsNone(prefer_clcc_stat([]))


if __name__ == "__main__":
	unittest.main()
