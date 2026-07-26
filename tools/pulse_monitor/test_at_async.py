"""Tests for the non-blocking AT transaction engine.

Mirrors the lara_async_* state machine in lara.cpp.

Why this module exists: in steady state the only AT traffic is two periodic
pollers (+CSQ for the signal meter, +CLCC while a call session is up). Each
one blocked loop() for the modem round trip, and for a full second when the
modem did not answer. During that stall the bell LED stopped toggling and
rotary dial-pulse processing was delayed. The engine below spreads one
transaction across many loop iterations so neither poller can stall it.

The highest-risk property under test is URC routing. The engine consumes
bytes from the same UART the URC matcher reads, so every byte it takes must
still be handed to the URC sink, and lines that are not this transaction's
response must not terminate it. If that regresses, incoming-call detection
breaks: a RING or NO CARRIER swallowed by a poller is never retransmitted.
"""

import unittest

from at_async import (
	AT_ERROR,
	AT_OK,
	AT_TIMEOUT,
	STATE_DONE,
	STATE_IDLE,
	STATE_WAITING,
	AsyncAt,
	settle,
)

# Transactions the firmware actually issues asynchronously.
CSQ_CMD = "+CSQ"
CSQ_PREFIX = "+CSQ:"
CLCC_CMD = "+CLCC"
CLCC_PREFIX = "+CLCC:"

# Echo is off in production (lara_on sends ATE0), so responses start with the
# blank separator the module emits before an information line.
CSQ_REPLY = "\r\n+CSQ: 21,99\r\n\r\nOK\r\n"
CLCC_TWO_LEGS = (
	'\r\n+CLCC: 1,0,0,0,0,"2345640362",129\r\n'
	'\r\n+CLCC: 2,1,5,0,0,"+12345640362",145\r\n'
	"\r\nOK\r\n"
)

TIMEOUT_MS = 1000
T0 = 10_000

# A line ends on its CR, so the LF of the final code's CRLF is still in the
# buffer when the transaction completes. lara_read_line() behaves the same
# way and the next read skips it as a blank line; here it falls to the URC
# drain instead, which is why it is not part of what the engine consumes.
CSQ_REPLY_CONSUMED = len(CSQ_REPLY) - 1
CSQ_REPLY_TRAILING_LF = "\n"


class Harness:
	"""Engine plus a recording URC sink, wired the way lara.cpp wires it."""

	def __init__(self, line_max=None):
		self.urc = []
		kwargs = {} if line_max is None else {"line_max": line_max}
		self.engine = AsyncAt(urc_sink=self.urc.append, **kwargs)

	@property
	def urc_text(self):
		return "".join(self.urc)


class CompletionTests(unittest.TestCase):
	def test_response_completes_normally(self):
		# Why: the base case the two pollers depend on. The result must
		# carry the information line and the OK must end the transaction.
		# Failure: rc is AT_TIMEOUT or lines is empty, so the signal meter
		# reads 0 bars forever.
		h = Harness()
		self.assertTrue(
			h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		)
		consumed = h.engine.service(CSQ_REPLY, T0 + 5)
		self.assertEqual(consumed, CSQ_REPLY_CONSUMED)
		self.assertEqual(h.engine.state, STATE_DONE)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(result.lines, ["+CSQ: 21,99"])
		self.assertEqual(h.engine.state, STATE_IDLE)

	def test_multi_line_response_collects_every_information_line(self):
		# Why: +CLCC emits one row per call leg, and the call-waiting fix
		# depends on seeing the second row.
		# Failure: only the last row survives, so a return call arriving
		# while another call is up never sets `ringing`.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		h.engine.service(CLCC_TWO_LEGS, T0 + 5)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(
			result.lines,
			[
				'+CLCC: 1,0,0,0,0,"2345640362",129',
				'+CLCC: 2,1,5,0,0,"+12345640362",145',
			],
		)

	def test_result_is_unavailable_until_the_final_code_arrives(self):
		# Why: the poller reads the result on the iteration it lands. A
		# result offered early would be acted on with missing rows.
		# Failure: take() returns a partial +CLCC list and the call-state
		# UI flips on incomplete data.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		h.engine.service(CLCC_TWO_LEGS[:40], T0 + 1)
		self.assertEqual(h.engine.state, STATE_WAITING)
		self.assertIsNone(h.engine.take())

	def test_error_final_codes_end_the_transaction(self):
		# Why: +CMEE=2 makes "+CME ERROR: ..." the only error form the
		# modem emits, and a bare ERROR is still legal.
		# Failure: a refused command burns the full timeout instead of
		# reporting the modem's answer on the next iteration.
		for reply in ("\r\nERROR\r\n", "\r\n+CME ERROR: 100\r\n"):
			with self.subTest(reply=reply):
				h = Harness()
				h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS)
				h.engine.service(reply, T0 + 1)
				self.assertEqual(h.engine.take().rc, AT_ERROR)

	def test_blank_separator_lines_are_skipped(self):
		# Why: the module frames every line with CRLF on both sides, so a
		# reader that treats an empty line as a line sees phantom entries.
		# Failure: empty strings appear in lines, or an empty line is
		# classified and ends the transaction.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS)
		h.engine.service("\r\n\r\n\r\n\r\nOK\r\n", T0 + 1)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(result.lines, [])

	def test_echo_line_is_suppressed_once(self):
		# Why: ATE0 is sent at boot but is not guaranteed to have taken
		# effect (lara_at_set ignores its own failure for E0).
		# Failure: "AT+CSQ" is collected as an information line and the
		# +CSQ parser reads the echo instead of the reply.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=None)
		h.engine.service("AT+CSQ\r\r\n+CSQ: 21,99\r\n\r\nOK\r\n", T0 + 1)
		self.assertEqual(h.engine.take().lines, ["+CSQ: 21,99"])


class TimeoutTests(unittest.TestCase):
	def test_silence_past_the_deadline_reports_timeout(self):
		# Why: the modem going quiet is the case that used to cost a full
		# second of loop() stall. It must now resolve without blocking.
		# Failure: state stays WAITING forever and the poller never fires
		# again, so the CLCC absence watchdog can never end a dead call.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		self.assertEqual(h.engine.service("", T0 + TIMEOUT_MS - 1), 0)
		self.assertEqual(h.engine.state, STATE_WAITING)
		h.engine.service("", T0 + TIMEOUT_MS)
		self.assertEqual(h.engine.state, STATE_DONE)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_TIMEOUT)
		self.assertEqual(result.lines, [])

	def test_timeout_result_is_delivered_exactly_once(self):
		# Why: the CLCC watchdog increments clcc_absent_polls on every
		# delivered result. A result delivered on every loop iteration
		# would reach CLCC_ABSENT_LIMIT in milliseconds and tear down a
		# live call.
		# Failure: the second take() returns a result and the session ends
		# three loop iterations after one missed poll.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS)
		h.engine.service("", T0 + TIMEOUT_MS)
		self.assertIsNotNone(h.engine.take())
		self.assertIsNone(h.engine.take())
		self.assertIsNone(h.engine.take())

	def test_data_already_in_the_buffer_beats_the_deadline(self):
		# Why: bytes are consumed before the clock is judged. A response
		# that arrived just before the deadline is real data; discarding it
		# would count a healthy call as an absent poll.
		# Failure: rc is AT_TIMEOUT even though the whole reply was
		# present, and clcc_absent_polls climbs during a normal call.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		h.engine.service(CSQ_REPLY, T0 + TIMEOUT_MS + 500)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(result.lines, ["+CSQ: 21,99"])

	def test_a_timed_out_partial_line_does_not_fuse_with_the_next_reply(self):
		# Why: a timeout can land mid-line, and the rest of that line then
		# arrives during the next transaction. A fragment left in the
		# assembly buffer fuses with those bytes into a line that still
		# matches the prefix, so the corruption is not merely dropped — it
		# is read as a valid reading from a reply that was abandoned.
		#
		# The values are chosen so the fused line is well formed: the
		# truncated "+CSQ: 2" plus the late "1,99" reads as "+CSQ: 21,99",
		# a strong signal, while the reply actually belonging to this
		# transaction reports 7. Nothing downstream could tell them apart.
		# Failure: lines carries the fused entry ahead of the real one and
		# the meter shows four bars on a weak signal.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		h.engine.service("\r\n+CSQ: 2", T0 + 1)
		h.engine.service("", T0 + TIMEOUT_MS)
		self.assertEqual(h.engine.take().rc, AT_TIMEOUT)

		h.engine.submit(CSQ_CMD, T0 + TIMEOUT_MS, TIMEOUT_MS, prefix=CSQ_PREFIX)
		h.engine.service(
			"1,99\r\n\r\n+CSQ: 7,99\r\n\r\nOK\r\n", T0 + TIMEOUT_MS + 1
		)
		self.assertEqual(h.engine.take().lines, ["+CSQ: 7,99"])


class UrcRoutingTests(unittest.TestCase):
	"""The engine reads the UART the URC matcher also reads.

	Every byte it takes has to reach the URC sink, or the URC is gone: the
	module never retransmits unsolicited output.
	"""

	def test_urc_interleaved_with_the_response_is_forwarded(self):
		# Why: an incoming call announced while a +CLCC poll is in flight
		# is the exact sequence that must survive. RING is not a +CLCC row
		# and not a final code, so the engine's own filters drop it.
		# Failure: urc_text lacks "RING", the bell never flashes, and the
		# call is missed outright.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		stream = '\r\n+CLCC: 1,1,4,0,0,"+12345640362",145\r\n\r\nRING\r\n\r\nOK\r\n'
		h.engine.service(stream, T0 + 1)
		self.assertIn("RING\r\n", h.urc_text)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(
			result.lines, ['+CLCC: 1,1,4,0,0,"+12345640362",145']
		)

	def test_every_consumed_byte_reaches_the_urc_sink(self):
		# Why: the URC matcher in lara.cpp is byte-oriented and holds state
		# across bytes, so it cannot tolerate a gap. Forwarding whole lines
		# but not the terminators would break "+UCALLSTAT:" capture, which
		# ends on the CR.
		# Failure: urc_text is shorter than what was consumed and a
		# +UCALLSTAT payload is never closed, so the call phase never
		# updates.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		consumed = h.engine.service(CLCC_TWO_LEGS, T0 + 1)
		self.assertEqual(h.urc_text, CLCC_TWO_LEGS[:consumed])

	def test_ucallstat_urc_inside_the_response_is_forwarded_intact(self):
		# Why: +UCALLSTAT is the only thing driving the call-phase UI, and
		# it arrives unprompted, including while a +CLCC poll is in flight.
		# Failure: the payload reaches the sink without its terminator, the
		# matcher never completes, and "Call ended" is never shown.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		h.engine.service("\r\n+UCALLSTAT: 1,6\r\n\r\nOK\r\n", T0 + 1)
		self.assertIn("+UCALLSTAT: 1,6\r\n", h.urc_text)
		self.assertEqual(h.engine.take().lines, [])

	def test_call_progress_codes_do_not_end_a_non_call_transaction(self):
		# Why: for anything other than ATD/ATA, NO CARRIER is unsolicited.
		# Treating it as this command's result both ends the poll early and
		# steals the URC that tells the UI the call dropped.
		# Failure: the +CLCC transaction reports AT_ERROR at "NO CARRIER",
		# the trailing OK is left to satisfy the next command, and the call
		# never leaves the "In call" state.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		h.engine.service("\r\nNO CARRIER\r\n\r\nOK\r\n", T0 + 1)
		self.assertIn("NO CARRIER\r\n", h.urc_text)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)

	def test_bytes_after_the_final_code_are_left_for_the_urc_drain(self):
		# Why: the engine must stop at its final code so the loop's own
		# drain owns whatever follows. Consuming past it would hide a URC
		# that arrived immediately after the reply, and take it out of the
		# console passthrough as well.
		# Failure: consumed equals the whole buffer, the trailing RING is
		# attributed to a transaction that had already ended, and the drain
		# sees nothing.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		stream = CSQ_REPLY + "\r\nRING\r\n"
		consumed = h.engine.service(stream, T0 + 1)
		self.assertEqual(consumed, CSQ_REPLY_CONSUMED)
		self.assertEqual(
			stream[consumed:], CSQ_REPLY_TRAILING_LF + "\r\nRING\r\n"
		)
		self.assertNotIn("RING", h.urc_text)

	def test_the_final_codes_trailing_lf_is_left_for_the_drain(self):
		# Why: a line ends on its CR, so the LF that follows the final
		# result code is not part of the transaction. Absorbing it would
		# mean peeking at a byte that may not have arrived, which is a
		# reason to keep reading after the transaction is over — exactly
		# what must not happen.
		# Failure: the engine keeps consuming while DONE, and a URC that
		# arrives in the gap before take() is swallowed.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		consumed = h.engine.service(CSQ_REPLY, T0 + 1)
		self.assertEqual(CSQ_REPLY[consumed:], CSQ_REPLY_TRAILING_LF)
		self.assertEqual(h.engine.service(CSQ_REPLY_TRAILING_LF, T0 + 2), 0)

	def test_an_idle_engine_consumes_nothing(self):
		# Why: between transactions the UART belongs to the URC drain. An
		# engine that ate bytes while idle would silently compete with it.
		# Failure: consumed is non-zero, and every URC arriving between
		# polls disappears into a transaction that does not exist.
		h = Harness()
		self.assertEqual(h.engine.state, STATE_IDLE)
		self.assertEqual(h.engine.service("\r\nRING\r\n", T0), 0)
		self.assertEqual(h.urc_text, "")

	def test_a_claimed_result_stops_the_engine_consuming(self):
		# Why: after the final code the engine is DONE but the owner may
		# not have taken the result yet. It must not keep reading.
		# Failure: URCs arriving in the gap between completion and take()
		# are swallowed.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		h.engine.service(CSQ_REPLY, T0 + 1)
		self.assertEqual(h.engine.state, STATE_DONE)
		self.assertEqual(h.engine.service("\r\nRING\r\n", T0 + 2), 0)
		self.assertEqual(h.urc_text, CSQ_REPLY[:CSQ_REPLY_CONSUMED])


class FramingTests(unittest.TestCase):
	def test_line_split_byte_by_byte_across_service_calls(self):
		# Why: a service call sees only what the UART buffer holds, so a
		# line routinely arrives in pieces. Assembly state has to persist.
		# Failure: each fragment is treated as a line, "+CS" and "Q: 21,99"
		# both fail to parse, and the meter never updates.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		for i, ch in enumerate(CSQ_REPLY):
			h.engine.service(ch, T0 + i)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(result.lines, ["+CSQ: 21,99"])
		self.assertEqual(h.urc_text, CSQ_REPLY[:CSQ_REPLY_CONSUMED])

	def test_crlf_split_across_service_calls_yields_one_line(self):
		# Why: the split landing between CR and LF is the awkward boundary.
		# The LF must be absorbed as an empty line, not emitted.
		# Failure: an empty information line appears, or the LF is prefixed
		# onto the next line making it unparseable.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		h.engine.service("\r\n+CSQ: 21,99\r", T0 + 1)
		h.engine.service("\n\r\nOK\r\n", T0 + 2)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(result.lines, ["+CSQ: 21,99"])

	def test_final_code_split_across_service_calls(self):
		# Why: the terminator itself can be split, and until its line
		# terminator lands the transaction is not over.
		# Failure: "O" then "K" are classified separately and the
		# transaction times out on a reply that did arrive.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS)
		h.engine.service("\r\nO", T0 + 1)
		self.assertEqual(h.engine.state, STATE_WAITING)
		h.engine.service("K", T0 + 2)
		self.assertEqual(h.engine.state, STATE_WAITING)
		h.engine.service("\r\n", T0 + 3)
		self.assertEqual(h.engine.take().rc, AT_OK)

	def test_over_long_line_is_truncated_not_split(self):
		# Why: splitting an over-long line lets its tail start a new line,
		# and a tail that happens to read "OK" then terminates the
		# transaction on payload. Truncation keeps the head, which for
		# these commands can never look like a final result code.
		#
		# The first row below is built so that splitting at the limit
		# leaves exactly "OK" as the tail — that is the only input shape
		# that tells truncation and splitting apart, since a tail of any
		# other text is discarded by the prefix filter either way.
		#
		# 15 characters survive, not 16: the C buffer reserves its last
		# byte for the terminating NUL.
		# Failure: lines holds only the first row and the transaction ends
		# on the fragment, so the second call leg is lost and the real OK
		# is left buffered for the next command to match.
		h = Harness(line_max=16)
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		stream = "\r\n+CLCC: 1,1,4,0,OK\r\n\r\n+CLCC: 2,1,5,0,\r\n\r\nOK\r\n"
		consumed = h.engine.service(stream, T0 + 1)
		result = h.engine.take()
		self.assertEqual(result.rc, AT_OK)
		self.assertEqual(
			result.lines, ["+CLCC: 1,1,4,0,", "+CLCC: 2,1,5,0,"]
		)
		self.assertEqual(consumed, len(stream) - 1)

	def test_prefix_filters_information_lines(self):
		# Why: the pollers ask for one prefix each, and unrelated
		# information lines must not be handed back as the reply.
		# Failure: "+CBC: 0,80,3900" is parsed as an RSSI.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		h.engine.service(
			"\r\n+CBC: 0,80,3900\r\n\r\n+CSQ: 21,99\r\n\r\nOK\r\n", T0 + 1
		)
		self.assertEqual(h.engine.take().lines, ["+CSQ: 21,99"])

	def test_no_prefix_keeps_every_information_line(self):
		# Why: the zero-filter case has to be distinguishable from "filter
		# matched nothing".
		# Failure: passing no prefix silently drops the whole response.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=None)
		h.engine.service("\r\n+CBC: 0,80,3900\r\n\r\nOK\r\n", T0 + 1)
		self.assertEqual(h.engine.take().lines, ["+CBC: 0,80,3900"])


class ArbitrationTests(unittest.TestCase):
	def test_submit_is_refused_while_a_transaction_is_in_flight(self):
		# Why: one UART, one response stream. A second command sent before
		# the first finishes makes both replies unattributable.
		# Failure: submit returns True, the second command's deadline and
		# prefix overwrite the first, and the first reply is parsed as the
		# second's.
		h = Harness()
		self.assertTrue(
			h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS, prefix=CSQ_PREFIX)
		)
		self.assertFalse(
			h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		)
		self.assertEqual(h.engine.command, CSQ_CMD)
		h.engine.service(CSQ_REPLY, T0 + 1)
		self.assertEqual(h.engine.take().lines, ["+CSQ: 21,99"])

	def test_submit_is_refused_while_a_result_is_unclaimed(self):
		# Why: the result belongs to whoever submitted it. Overwriting it
		# with a new transaction loses a poll result the caller is about to
		# read on this very iteration.
		# Failure: the CLCC poller's reply is destroyed by the signal
		# meter's submit and the poll counts as absent.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		h.engine.service(CLCC_TWO_LEGS, T0 + 1)
		self.assertFalse(h.engine.submit(CSQ_CMD, T0 + 2, TIMEOUT_MS))
		self.assertEqual(len(h.engine.take().lines), 2)

	def test_back_to_back_transactions_do_not_leak_state(self):
		# Why: the pollers alternate on one engine, so a second submit must
		# start from a clean slate.
		# Failure: the previous transaction's lines are appended to, so the
		# +CSQ result still contains the +CLCC rows.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		h.engine.service(CLCC_TWO_LEGS, T0 + 1)
		self.assertEqual(len(h.engine.take().lines), 2)

		self.assertTrue(
			h.engine.submit(CSQ_CMD, T0 + 2, TIMEOUT_MS, prefix=CSQ_PREFIX)
		)
		h.engine.service(CSQ_REPLY, T0 + 3)
		second = h.engine.take()
		self.assertEqual(second.rc, AT_OK)
		self.assertEqual(second.lines, ["+CSQ: 21,99"])

	def test_submit_is_accepted_again_after_a_timeout_is_claimed(self):
		# Why: a timed-out transaction must not wedge the engine, or one
		# missed reply disables both pollers for the rest of the session.
		# Failure: submit keeps returning False and the signal meter and
		# call-state poll are dead until reboot.
		h = Harness()
		h.engine.submit(CSQ_CMD, T0, TIMEOUT_MS)
		h.engine.service("", T0 + TIMEOUT_MS)
		self.assertEqual(h.engine.take().rc, AT_TIMEOUT)
		self.assertTrue(h.engine.submit(CLCC_CMD, T0 + TIMEOUT_MS, TIMEOUT_MS))


class SettleTests(unittest.TestCase):
	"""settle() is what a synchronous command calls before it transmits.

	A button press can fire lara_dial() while a poll is outstanding. Letting
	the two overlap would leave the poll's reply in the buffer for the dial's
	matcher to accept as its own result.
	"""

	def test_settle_drives_an_in_flight_transaction_to_completion(self):
		# Why: the reply is already on the wire; absorbing it is what keeps
		# the stream in sync for the synchronous command that follows.
		# Failure: settle returns with the reply unread, and ATD's matcher
		# takes the poll's OK as its own — a refused dial reports success.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		chunks = iter([CLCC_TWO_LEGS[:20], CLCC_TWO_LEGS[20:]])
		clock = iter([T0 + 1, T0 + 2])
		state = settle(
			h.engine,
			lambda: next(chunks, ""),
			lambda: next(clock, T0 + 3),
		)
		self.assertEqual(state, STATE_DONE)
		self.assertEqual(len(h.engine.take().lines), 2)

	def test_settle_gives_up_at_the_transaction_deadline(self):
		# Why: the modem may never answer. settle must be bounded by the
		# deadline the transaction already carries, not spin forever.
		# Failure: a button press with a dead modem hangs the firmware.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		ticks = iter(range(T0, T0 + TIMEOUT_MS + 1, 100))
		state = settle(h.engine, lambda: "", lambda: next(ticks, T0 + TIMEOUT_MS))
		self.assertEqual(state, STATE_DONE)
		self.assertEqual(h.engine.take().rc, AT_TIMEOUT)

	def test_settle_on_an_idle_engine_is_a_no_op(self):
		# Why: every synchronous send calls settle, and almost always
		# nothing is in flight. It must not read the UART in that case.
		# Failure: settle consumes bytes belonging to the URC drain on
		# every synchronous command.
		h = Harness()
		reads = []

		def read():
			reads.append(1)
			return "\r\nRING\r\n"

		self.assertEqual(settle(h.engine, read, lambda: T0), STATE_IDLE)
		self.assertEqual(reads, [])
		self.assertEqual(h.urc_text, "")

	def test_settle_preserves_an_unclaimed_result(self):
		# Why: the poller still has to see the result of a transaction that
		# completed just as a button was pressed, or the poll silently
		# vanishes instead of counting.
		# Failure: settle clears the result and clcc_absent_polls neither
		# resets nor increments for that poll.
		h = Harness()
		h.engine.submit(CLCC_CMD, T0, TIMEOUT_MS, prefix=CLCC_PREFIX)
		h.engine.service(CLCC_TWO_LEGS, T0 + 1)
		self.assertEqual(settle(h.engine, lambda: "", lambda: T0 + 2), STATE_DONE)
		self.assertEqual(len(h.engine.take().lines), 2)


if __name__ == "__main__":
	unittest.main()
