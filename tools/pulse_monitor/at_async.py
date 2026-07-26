"""Non-blocking AT transaction engine.

Mirror of the lara_async_* state machine in lara.cpp. at_transaction.py
covers the same framing rules for the blocking path; the classification of a
line as a final result code is imported from there rather than restated, so
the two paths cannot drift apart.

Why it exists: the blocking reader owns loop() for a whole modem round trip,
and for the full timeout when the modem stays quiet. The two periodic
pollers (+CSQ for the signal meter, +CLCC while a call session is up) pay
that cost unprompted, several times a minute, and while they hold the CPU the
bell LED does not toggle and rotary dial pulses queue up. This engine turns
one transaction into a sequence of short service calls instead.

Three invariants carry the risk:

* Every byte the engine consumes is handed to the URC sink. The engine reads
  the same UART the URC matcher reads, and the module never retransmits
  unsolicited output, so a byte the engine keeps to itself is a URC lost.
* The engine consumes nothing unless a transaction is in flight, and stops
  at its final result code. Everything else belongs to the caller's drain.
* A result is delivered exactly once. The +CLCC absence watchdog counts
  delivered results, so a result offered on every iteration would tear down
  a live call within milliseconds.
"""

from __future__ import annotations

from typing import Callable, NamedTuple

from at_transaction import AT_ERROR, AT_OK, AT_TIMEOUT, final_code

STATE_IDLE = "IDLE"
STATE_WAITING = "WAITING"
STATE_DONE = "DONE"

# Longest response line kept intact; longer lines are truncated, never split.
# Only +CSQ and +CLCC run asynchronously and both are far shorter than this.
# Truncation keeps the head of the line, so a fragment can never be mistaken
# for a final result code the way a tail could.
DEFAULT_LINE_MAX = 64


class AsyncResult(NamedTuple):
	"""Outcome of one asynchronous AT transaction.

	rc:    AT_OK, AT_ERROR, or AT_TIMEOUT (no final code before the deadline).
	lines: information lines, filtered by prefix when one was given.
	"""

	rc: str
	lines: list[str]


class AsyncAt:
	"""One AT transaction spread across many service calls.

	urc_sink receives every byte the engine consumes, in order, including
	line terminators. The firmware passes lara_urc_on_byte() here; the
	matcher it drives is byte-oriented and holds state across bytes, so
	forwarding whole lines without their terminators would leave a
	"+UCALLSTAT:" capture permanently open.
	"""

	def __init__(
		self,
		urc_sink: Callable[[str], None] | None = None,
		line_max: int = DEFAULT_LINE_MAX,
	) -> None:
		self._urc_sink = urc_sink
		self._line_max = line_max
		self._state = STATE_IDLE
		self._command: str | None = None
		self._prefix: str | None = None
		self._call_progress_final = False
		self._deadline = 0
		self._echo_pending = False
		self._partial = ""
		self._lines: list[str] = []
		self._rc: str | None = None

	@property
	def state(self) -> str:
		return self._state

	@property
	def busy(self) -> bool:
		"""True while the engine owns the UART or holds an unclaimed result."""
		return self._state != STATE_IDLE

	@property
	def command(self) -> str | None:
		return self._command

	def submit(
		self,
		command: str | None,
		now_ms: int,
		timeout_ms: int,
		prefix: str | None = None,
		call_progress_final: bool = False,
	) -> bool:
		"""Start a transaction. False when the engine is not free.

		Refused while a result is still unclaimed, not just while one is in
		flight: the result belongs to whoever submitted it, and the two
		pollers share this engine.

		This is the only place transaction state is reset. Clearing it on
		completion as well would look safer but makes each clear untestable
		on its own — a dropped reset stays invisible because the other one
		covers for it.
		"""
		if self._state != STATE_IDLE:
			return False
		self._command = command
		self._prefix = prefix.upper() if prefix else None
		self._call_progress_final = call_progress_final
		self._deadline = now_ms + timeout_ms
		self._echo_pending = command is not None
		self._partial = ""
		self._lines = []
		self._rc = None
		self._state = STATE_WAITING
		return True

	def service(self, data: str, now_ms: int) -> int:
		"""Consume what has arrived. Returns the number of bytes taken.

		Bytes are judged before the clock: a reply that landed just before
		the deadline is real data, and discarding it would count a healthy
		poll as an absent one. The deadline only decides what happens once
		the available bytes are exhausted.
		"""
		if self._state != STATE_WAITING:
			return 0

		consumed = 0
		for ch in data:
			consumed += 1
			if self._urc_sink is not None:
				self._urc_sink(ch)
			if ch not in "\r\n":
				# Truncate rather than split. Room is reserved for the
				# terminating NUL the C buffer needs.
				if len(self._partial) + 1 < self._line_max:
					self._partial += ch
				continue

			line = self._partial
			self._partial = ""
			if not line:
				continue
			if self._echo_pending and self._is_echo(line):
				self._echo_pending = False
				continue
			rc = final_code(line, self._call_progress_final)
			if rc is not None:
				self._finish(rc)
				return consumed
			if self._prefix is None or line.upper().startswith(self._prefix):
				self._lines.append(line)

		if now_ms - self._deadline >= 0:
			self._finish(AT_TIMEOUT)
		return consumed

	def take(self) -> AsyncResult | None:
		"""Claim a completed result, freeing the engine. None until then."""
		if self._state != STATE_DONE:
			return None
		self._state = STATE_IDLE
		return AsyncResult(self._rc, self._lines)

	def _finish(self, rc: str) -> None:
		self._rc = rc
		self._state = STATE_DONE

	def _is_echo(self, line: str) -> bool:
		"""True when the modem is echoing "AT<command>" (ATE1 still on)."""
		if self._command is None:
			return False
		return line.strip().upper() == ("AT" + self._command).upper()


def settle(
	engine: AsyncAt,
	read_available: Callable[[], str],
	now: Callable[[], int],
) -> str:
	"""Drive an in-flight transaction to completion, blocking if need be.

	This is what a synchronous command calls before it transmits. A button
	press can fire lara_dial() while a poll is outstanding, and one UART
	cannot carry two unfinished transactions: the poll's reply would still be
	arriving when ATD's matcher started reading, and its OK would be accepted
	as the dial's own result.

	Absorbing the poll rather than discarding it keeps two things true at
	once — the stream is back in sync for the synchronous command, and the
	poller still gets its result, so the +CLCC absence count neither skips
	nor double-counts. Bounded by the deadline the transaction already
	carries, so the worst case is the timeout the caller would have paid
	anyway.
	"""
	while engine.state == STATE_WAITING:
		engine.service(read_available(), now())
	return engine.state


__all__ = [
	"AT_ERROR",
	"AT_OK",
	"AT_TIMEOUT",
	"STATE_DONE",
	"STATE_IDLE",
	"STATE_WAITING",
	"AsyncAt",
	"AsyncResult",
	"settle",
	"final_code",
]
