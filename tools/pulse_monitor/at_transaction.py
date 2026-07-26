"""Read an AT command response up to its final result code.

Mirror of lara_at() in lara.cpp. Kept pure (operates on a complete buffer)
so the framing rules can be tested without a modem; the firmware carries the
millisecond timeout that decides when the buffer has run out.

Two rules matter more than the parsing itself:

* A transaction consumes everything through its final result code. Leaving
  the trailing OK buffered is what made the *next* command's matcher succeed
  instantly against the previous command's response.
* Final result codes are whole lines, never substrings. A quoted "OK" inside
  a +CLCC row is payload, not a terminator.
"""

from __future__ import annotations

from typing import NamedTuple

AT_OK = "OK"
AT_ERROR = "ERROR"
AT_TIMEOUT = "TIMEOUT"

# 3GPP 27.007 final result codes that apply to any command.
_FINAL_OK = "OK"
_FINAL_ERROR = "ERROR"
# +CMEE=2 makes this the only error form the modem emits, so a matcher that
# looks for a bare "ERROR" line never fires.
_ERROR_PREFIXES = ("+CME ERROR:", "+CMS ERROR:")
# Result codes for call-originating commands (ATD, ATA, ATO). For every other
# command these are unsolicited and must be left to the URC handler.
_CALL_PROGRESS = ("NO CARRIER", "BUSY", "NO ANSWER", "NO DIALTONE")


class AtResponse(NamedTuple):
	"""Outcome of one AT transaction.

	rc:       AT_OK, AT_ERROR, or AT_TIMEOUT (no final code in the buffer).
	lines:    information lines, filtered by prefix when one was given.
	consumed: characters taken from the buffer. On success this always
	          includes the final result code and its line terminator.
	"""

	rc: str
	lines: list[str]
	consumed: int


def final_code(line: str, call_progress_final: bool = False) -> str | None:
	"""Classify a complete response line as a final result code.

	Returns AT_OK, AT_ERROR, or None when the line is information or a URC.
	call_progress_final selects the ATD/ATA dialect in which NO CARRIER and
	friends end the transaction.
	"""
	text = line.strip().upper()
	if not text:
		return None
	if text == _FINAL_OK:
		return AT_OK
	if text == _FINAL_ERROR:
		return AT_ERROR
	for prefix in _ERROR_PREFIXES:
		if text.startswith(prefix):
			return AT_ERROR
	if call_progress_final and text in _CALL_PROGRESS:
		return AT_ERROR
	return None


def read_response(
	data: str,
	command: str | None = None,
	prefix: str | None = None,
	call_progress_final: bool = False,
) -> AtResponse:
	"""Consume one AT response from data.

	command, when given, suppresses the echo line the modem sends while
	ATE1 is in effect. prefix filters the information lines that are kept;
	None keeps them all.
	"""
	echo = command.strip() if command else None
	want = prefix.upper() if prefix else None
	lines: list[str] = []
	line_start = 0
	i = 0
	n = len(data)

	while i < n:
		ch = data[i]
		if ch not in "\r\n":
			i += 1
			continue

		line = data[line_start:i]
		# Consume the terminator, pairing CR with a following LF so the
		# caller is never handed a dangling half of a CRLF.
		i += 1
		if ch == "\r" and i < n and data[i] == "\n":
			i += 1
		line_start = i

		text = line.strip()
		if not text:
			continue
		if echo is not None and text == echo:
			echo = None
			continue

		rc = final_code(text, call_progress_final)
		if rc is not None:
			return AtResponse(rc, lines, i)

		if want is None or text.upper().startswith(want):
			lines.append(text)

	# No final result code arrived. Everything seen is drained so a partial
	# line cannot be re-parsed as the head of the next response.
	return AtResponse(AT_TIMEOUT, lines, n)
