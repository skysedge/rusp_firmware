"""Tests for +CLCC line parsing and incoming-call detection.

Mirrors parse_clcc_line() / clcc_has_incoming() in lara.cpp.
"""

import unittest

from clcc import ClccEntry, clcc_has_incoming, parse_clcc_line

# Captured verbatim from a live session. The device was in an outbound call
# (id 1) when the user called back, so the network delivered the return call
# as call waiting (id 2, direction 1, state 5).
OUTBOUND_ACTIVE = '+CLCC: 1,0,0,0,0,"2345640362",129'
INBOUND_WAITING = '+CLCC: 2,1,5,0,0,"+12345640362",145'
INBOUND_RINGING = '+CLCC: 1,1,4,0,0,"+12345640362",145'


class ParseTests(unittest.TestCase):
	def test_outbound_active_line_reports_direction_and_state(self):
		# Why: direction was previously discarded, which is what made an
		# incoming call indistinguishable from an outgoing one.
		# Failure: direction is absent, so callers cannot tell MO from MT.
		entry = parse_clcc_line(OUTBOUND_ACTIVE)
		self.assertEqual(entry, ClccEntry(call_id=1, direction=0, state=0))

	def test_waiting_inbound_line_is_parsed(self):
		# Why: this exact line was in the capture when the light failed to
		# flash. State 5 is "waiting" and direction 1 is mobile-terminated.
		# Failure: returns None or direction 0, and the callback is treated
		# as part of the existing outbound call.
		entry = parse_clcc_line(INBOUND_WAITING)
		self.assertEqual(entry, ClccEntry(call_id=2, direction=1, state=5))

	def test_ringing_inbound_line_is_parsed(self):
		# Why: state 4 is the ordinary incoming case, when no other call is
		# up. It must be recognised by the same path as 5.
		# Failure: only call waiting alerts, plain incoming calls do not.
		entry = parse_clcc_line(INBOUND_RINGING)
		self.assertEqual(entry, ClccEntry(call_id=1, direction=1, state=4))

	def test_truncated_lines_are_rejected(self):
		# Why: the same capture contained "+U" and "1,0" where full URCs
		# should have been, so truncated input is a real, observed case —
		# not a hypothetical. A partial line must never be read as a call.
		# Failure: a fragment parses into a bogus entry and either invents
		# an incoming call or masks a real one.
		for line in ("+U", "1,0", "+CLCC:", "+CLCC: 2", "+CLCC: 2,1", ""):
			with self.subTest(line=line):
				self.assertIsNone(parse_clcc_line(line))

	def test_unrelated_lines_are_rejected(self):
		# Why: the reader sees every line, including other URCs and final
		# result codes.
		# Failure: "OK" or a +CSQ reply is parsed as call state.
		for line in ("OK", "+CSQ: 30,99", "RING", "+CME ERROR: unknown"):
			with self.subTest(line=line):
				self.assertIsNone(parse_clcc_line(line))

	def test_non_numeric_fields_are_rejected(self):
		# Why: a corrupted line can be the right shape with garbage in it.
		# Failure: an exception escapes into the URC read loop.
		self.assertIsNone(parse_clcc_line('+CLCC: x,y,z,0,0,"1",129'))


class IncomingDetectionTests(unittest.TestCase):
	def test_waiting_call_alongside_active_call_counts_as_incoming(self):
		# Why: this is the reported bug. With an outbound call still up, the
		# returning call arrives as a second entry in state 5, and the phone
		# must still alert.
		# Failure: returns False, `ringing` stays clear, and the bell and
		# filament LEDs never flash.
		entries = [
			parse_clcc_line(OUTBOUND_ACTIVE),
			parse_clcc_line(INBOUND_WAITING),
		]
		self.assertTrue(clcc_has_incoming(entries))

	def test_ordinary_incoming_call_counts(self):
		# Why: the plain case must not regress while fixing call waiting.
		# Failure: normal incoming calls stop alerting.
		self.assertTrue(clcc_has_incoming([parse_clcc_line(INBOUND_RINGING)]))

	def test_outbound_call_alone_is_not_incoming(self):
		# Why: an outgoing call must never light the bell. Direction is the
		# only thing separating these, which is why it has to be parsed.
		# Failure: the phone rings at itself for its own outgoing calls.
		self.assertFalse(clcc_has_incoming([parse_clcc_line(OUTBOUND_ACTIVE)]))

	def test_connected_inbound_call_is_not_still_incoming(self):
		# Why: once an incoming call is answered its state becomes 0. It
		# must stop being treated as ringing or the bell never stops.
		# Failure: the LEDs keep flashing throughout the answered call.
		self.assertFalse(
			clcc_has_incoming([ClccEntry(call_id=1, direction=1, state=0)])
		)

	def test_empty_list_is_not_incoming(self):
		# Why: the zero case. No calls means nothing to alert about.
		# Failure: an empty CLCC reply rings the phone.
		self.assertFalse(clcc_has_incoming([]))

	def test_none_entries_are_ignored(self):
		# Why: callers pass parse results straight through, and truncated
		# lines parse to None. Those must be skipped, not crash.
		# Failure: a TypeError inside the modem read loop.
		self.assertFalse(clcc_has_incoming([None, None]))
		self.assertTrue(
			clcc_has_incoming([None, parse_clcc_line(INBOUND_WAITING)])
		)


if __name__ == "__main__":
	unittest.main()
