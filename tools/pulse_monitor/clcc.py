"""Parse +CLCC call list entries and decide whether a call is incoming.

Why direction matters: +CLCC reports both the state and the direction of each
call, and state alone cannot distinguish them. State 0 (active) means an
established call whether the phone placed it or answered it. Reading only the
state left the firmware unable to tell an incoming call from its own outgoing
one, so an incoming call never set `ringing` and the bell never flashed.

The call-waiting case is why this is not academic. With a call already up, a
second incoming call is reported as an additional entry in state 5, not as a
RING URC. A capture of that exact situation is in test_clcc.py.

Mirrors parse_clcc_line() / clcc_has_incoming() in lara.cpp.
"""

from __future__ import annotations

from dataclasses import dataclass

CLCC_PREFIX = "+CLCC:"

# 3GPP 27.007 <dir>: 0 mobile originated, 1 mobile terminated.
CLCC_DIR_INCOMING = 1

# 3GPP 27.007 <stat>. Only the two that mean "alerting the user" are listed;
# 0 active, 1 held, 2 dialing and 3 alerting are all either established or
# outgoing and must not ring the bell.
CLCC_STATE_INCOMING = 4
CLCC_STATE_WAITING = 5
CLCC_RINGING_STATES = (CLCC_STATE_INCOMING, CLCC_STATE_WAITING)

# id, dir, stat — the fields needed before the line can be trusted.
CLCC_MIN_FIELDS = 3


@dataclass(frozen=True)
class ClccEntry:
	call_id: int
	direction: int
	state: int


def parse_clcc_line(line: str) -> ClccEntry | None:
	"""Return the entry described by one +CLCC line, or None.

	Returns None rather than raising or guessing for anything that is not a
	complete, numeric +CLCC entry. Truncated input is an observed condition
	on this hardware, not a hypothetical: URCs have arrived as a bare "+U",
	and a fragment that parsed into a partial entry would either invent an
	incoming call or hide a real one.
	"""
	if line is None:
		return None
	text = line.strip()
	if not text.upper().startswith(CLCC_PREFIX):
		return None

	fields = [f.strip() for f in text[len(CLCC_PREFIX):].split(",")]
	if len(fields) < CLCC_MIN_FIELDS:
		return None
	try:
		call_id, direction, state = (int(f) for f in fields[:CLCC_MIN_FIELDS])
	except ValueError:
		return None
	return ClccEntry(call_id=call_id, direction=direction, state=state)


def clcc_has_incoming(entries) -> bool:
	"""True if any entry is a mobile-terminated call alerting the user.

	None entries are skipped so callers can pass parse results straight in
	without filtering; a line that failed to parse is simply not evidence.
	"""
	return any(
		e is not None
		and e.direction == CLCC_DIR_INCOMING
		and e.state in CLCC_RINGING_STATES
		for e in entries
	)
