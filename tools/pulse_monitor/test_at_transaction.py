"""Tests for AT response reading (mirror of lara.cpp lara_at()).

Why these tests exist: the firmware matched final result codes with a
substring scan (expect("OK\\r")) that did not respect line boundaries and did
not consume the response it matched. Two bugs followed:

1. lara_status() read the +CPAS digit plus one byte and left "\\n\\r\\nOK\\r\\n"
   in the RX buffer. The next command's expect("OK\\r") matched that leftover
   and returned success before the command had even been echoed, so
   lara_dial() and lara_hangup() always reported rc=0.
2. lara_on() sets +CMEE=2, so every failure arrives as "+CME ERROR: <text>".
   A matcher looking for "ERROR\\r" never sees it, so failures were reported
   as timeouts after burning the full timeout window.

Failure modes when these regressions return:
- consumed < len(response): a trailing OK survives and satisfies the next
  transaction, so a failed dial reports success.
- "+CME ERROR: 100" classified as AT_TIMEOUT instead of AT_ERROR.
- A quoted "OK" inside a +CLCC row terminating the transaction early, which
  drops the remaining call rows.
- NO CARRIER terminating a non-call command, which steals the URC that tells
  the UI a call ended.
"""

import unittest

from at_transaction import (
	AT_ERROR,
	AT_OK,
	AT_TIMEOUT,
	final_code,
	read_response,
)

# Real transcripts, echo enabled (the modem default until ATE0 is sent).
CPAS_READY = "AT+CPAS\r\r\n+CPAS: 0\r\n\r\nOK\r\n"
CPAS_IN_CALL = "AT+CPAS\r\r\n+CPAS: 4\r\n\r\nOK\r\n"
ATD_VERBOSE_ERROR = "ATD5551234;\r\r\n+CME ERROR: no network service\r\n"
ATD_OK = "ATD5551234;\r\r\nOK\r\n"


class FinalCodeTests(unittest.TestCase):
	def test_ok_and_bare_error(self):
		# Guards the two codes every AT set command can return.
		# Failure: lara_at_set() can never complete.
		self.assertEqual(final_code("OK"), AT_OK)
		self.assertEqual(final_code("ERROR"), AT_ERROR)

	def test_verbose_cme_and_cms_error_are_final(self):
		# Why: +CMEE=2 makes this the ONLY error form the modem emits.
		# Failure: every failed command reports AT_TIMEOUT after the full
		# timeout instead of the reason the modem gave.
		self.assertEqual(final_code("+CME ERROR: 100"), AT_ERROR)
		self.assertEqual(
			final_code("+CME ERROR: no network service"), AT_ERROR
		)
		self.assertEqual(final_code("+CMS ERROR: 500"), AT_ERROR)

	def test_information_lines_are_not_final(self):
		# Why: the response body must not end the transaction.
		# Failure: the trailing OK is left buffered for the next command.
		self.assertIsNone(final_code("+CPAS: 0"))
		self.assertIsNone(final_code('+CLCC: 1,0,0,0,0,"5551234",129'))
		self.assertIsNone(final_code("+CSQ: 17,99"))
		self.assertIsNone(final_code(""))

	def test_line_merely_containing_ok_is_not_final(self):
		# Why: expect("OK\r") scanned for a substring, so any payload
		# containing OK ended the read. Failure: remaining +CLCC rows are
		# dropped and prefer_clcc_stat() sees an incomplete call list.
		self.assertIsNone(final_code('+CLCC: 1,0,0,0,0,"OK",129'))
		self.assertIsNone(final_code('+COPS: 0,0,"OKTEL",2'))

	def test_call_progress_final_only_when_requested(self):
		# Why: NO CARRIER / BUSY terminate ATD and ATA, but for any other
		# command they are unsolicited. Failure: a NO CARRIER URC arriving
		# during AT+CPAS ends that transaction and is consumed as its
		# result, so the UI never learns the call dropped.
		for line in ("NO CARRIER", "BUSY", "NO ANSWER", "NO DIALTONE"):
			self.assertIsNone(final_code(line))
			self.assertEqual(
				final_code(line, call_progress_final=True), AT_ERROR
			)

	def test_ok_still_final_for_call_commands(self):
		# Guards that enabling call-progress codes does not lose OK.
		self.assertEqual(
			final_code("OK", call_progress_final=True), AT_OK
		)


class ReadResponseTests(unittest.TestCase):
	def test_cpas_consumes_through_final_ok(self):
		# THE headline regression. lara_status() left "\n\r\nOK\r\n"
		# buffered. Failure: consumed < len(CPAS_READY), and the remainder
		# contains an OK that the next transaction will match.
		got = read_response(CPAS_READY, command="AT+CPAS", prefix="+CPAS:")
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.lines, ["+CPAS: 0"])
		self.assertEqual(got.consumed, len(CPAS_READY))
		self.assertEqual(CPAS_READY[got.consumed :], "")

	def test_stale_ok_cannot_satisfy_the_next_command(self):
		# Why: this is the exact sequence the hook handler runs —
		# lara_status() then lara_dial() with nothing draining in between.
		# Failure: the second transaction returns AT_OK from the first
		# command's trailing OK, so a dial that never happened reports
		# success and outbound_call_active sticks true forever.
		stream = CPAS_READY + ATD_VERBOSE_ERROR
		first = read_response(stream, command="AT+CPAS", prefix="+CPAS:")
		self.assertEqual(first.rc, AT_OK)

		rest = stream[first.consumed :]
		self.assertNotIn("OK", rest)

		second = read_response(rest, command="ATD5551234;")
		self.assertEqual(second.rc, AT_ERROR)
		self.assertEqual(second.consumed, len(rest))

	def test_echo_line_is_not_captured_as_information(self):
		# Why: echo is on until ATE0 is sent, so "AT+CPAS" precedes every
		# response. Failure: the echo is returned as a response line and
		# parsed as data.
		got = read_response(CPAS_IN_CALL, command="AT+CPAS")
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.lines, ["+CPAS: 4"])

	def test_verbose_error_is_error_not_timeout(self):
		# Why: see module docstring item 2. Failure: AT_TIMEOUT, and the
		# caller cannot distinguish "modem said no" from "modem is dead".
		got = read_response(ATD_VERBOSE_ERROR, command="ATD5551234;")
		self.assertEqual(got.rc, AT_ERROR)
		self.assertEqual(got.consumed, len(ATD_VERBOSE_ERROR))

	def test_dial_success(self):
		# Guards the happy path so the error tests are not vacuous.
		got = read_response(ATD_OK, command="ATD5551234;")
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.lines, [])
		self.assertEqual(got.consumed, len(ATD_OK))

	def test_urc_interleaved_with_response_does_not_terminate(self):
		# Why: a RING can land between the echo and the result code.
		# Failure: RING is treated as the response body or as a final
		# code, and +CPAS: 0 is never returned.
		stream = "AT+CPAS\r\r\nRING\r\n\r\n+CPAS: 0\r\n\r\nOK\r\n"
		got = read_response(stream, command="AT+CPAS", prefix="+CPAS:")
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.lines, ["+CPAS: 0"])
		self.assertEqual(got.consumed, len(stream))

	def test_no_carrier_during_plain_command_is_left_for_urc_handling(self):
		# Why: NO CARRIER is only a result code for ATD/ATA. Failure: the
		# CPAS transaction ends on it, the OK is left buffered, and the
		# next command matches that stale OK.
		stream = "AT+CPAS\r\r\nNO CARRIER\r\n\r\n+CPAS: 0\r\n\r\nOK\r\n"
		got = read_response(stream, command="AT+CPAS", prefix="+CPAS:")
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.lines, ["+CPAS: 0"])
		self.assertEqual(got.consumed, len(stream))

	def test_no_carrier_terminates_a_dial(self):
		# Why: ATD to an unreachable number ends with NO CARRIER and no OK.
		# Failure: the dial blocks for the whole timeout, then reports
		# AT_TIMEOUT, and outbound_call_active is set from a dead call.
		stream = "ATD5551234;\r\r\nNO CARRIER\r\n"
		got = read_response(
			stream, command="ATD5551234;", call_progress_final=True
		)
		self.assertEqual(got.rc, AT_ERROR)
		self.assertEqual(got.consumed, len(stream))

	def test_multiple_information_lines_are_all_captured(self):
		# Why: +CLCC returns one row per call leg and prefer_clcc_stat()
		# needs all of them. Failure: only the first row survives, so an
		# active leg behind an alerting leg is missed.
		stream = (
			"AT+CLCC\r"
			'\r\n+CLCC: 1,0,3,0,0,"5551234",129\r\n'
			'\r\n+CLCC: 2,1,0,0,0,"5555678",129\r\n'
			"\r\nOK\r\n"
		)
		got = read_response(stream, command="AT+CLCC", prefix="+CLCC:")
		self.assertEqual(
			got.lines,
			[
				'+CLCC: 1,0,3,0,0,"5551234",129',
				'+CLCC: 2,1,0,0,0,"5555678",129',
			],
		)
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.consumed, len(stream))

	def test_no_calls_returns_ok_with_no_lines(self):
		# Why: AT+CLCC with no calls is a bare OK. Failure: treated as an
		# error, so the call-ended watchdog never fires.
		stream = "AT+CLCC\r\r\nOK\r\n"
		got = read_response(stream, command="AT+CLCC", prefix="+CLCC:")
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.lines, [])

	def test_truncated_response_times_out_and_drains(self):
		# Why: a modem that dies mid-response must not leave a partial
		# line that the next transaction parses. Failure: consumed stops
		# short and the fragment corrupts the following command.
		stream = "AT+CPAS\r\r\n+CPAS: 0\r\n"
		got = read_response(stream, command="AT+CPAS", prefix="+CPAS:")
		self.assertEqual(got.rc, AT_TIMEOUT)
		self.assertEqual(got.consumed, len(stream))

	def test_empty_input_times_out_consuming_nothing(self):
		# Why: the null case — no bytes at all from a powered-off modem.
		# Failure: an exception, or a false AT_OK.
		got = read_response("", command="AT+CPAS")
		self.assertEqual(got.rc, AT_TIMEOUT)
		self.assertEqual(got.lines, [])
		self.assertEqual(got.consumed, 0)

	def test_prefix_none_captures_every_information_line(self):
		# Why: callers that want the raw body pass no prefix. Failure:
		# body lines are silently dropped.
		got = read_response(CPAS_READY, command="AT+CPAS")
		self.assertEqual(got.lines, ["+CPAS: 0"])

	def test_response_without_echo_is_handled(self):
		# Why: after ATE0 the echo disappears. Failure: the parser depends
		# on an echo line being present and mis-frames the first line.
		stream = "\r\n+CPAS: 0\r\n\r\nOK\r\n"
		got = read_response(stream, command="AT+CPAS", prefix="+CPAS:")
		self.assertEqual(got.rc, AT_OK)
		self.assertEqual(got.lines, ["+CPAS: 0"])
		self.assertEqual(got.consumed, len(stream))


if __name__ == "__main__":
	unittest.main()
