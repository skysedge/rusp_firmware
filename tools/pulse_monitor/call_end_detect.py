"""Detect remote call end from modem URCs / CPAS transitions.

Why: OLED stayed on CALLING after the far end hung up because firmware
only looked for NO CARRIER (fragile one-byte URC match) and ignored
+UCALLSTAT disconnect (6) and CPAS returning to ready after in-call.

Ring liveness lives here too, because "is this incoming call still
ringing" is the same question as "has this session ended". A live capture
showed the modem announcing a whole incoming call with +UCALLSTAT: 1,4 and
no RING URC at all, so anything that keys off RING alone cannot answer it.
"""

from __future__ import annotations

from call_phase import UCALL_RINGING_MT

# Consecutive AT+CLCC polls reporting no calls before a session is declared
# over. At the 1 Hz poll rate this is ~3 s of grace, enough to cover the gap
# between ATD returning OK and the call leg appearing in CLCC.
CLCC_ABSENT_LIMIT = 3

# Silence that means the caller is gone. Measured repeat interval on the
# live modem is 5.5-6 s, so this is two cadences: it tolerates exactly one
# dropped URC, which matters because URCs demonstrably arrive corrupted
# (the capture holds "AT: 1,3" and " 1,7" with the +UCALLSTAT prefix eaten).
# The previous 5000 ms was below the real cadence and fired between every
# pair of rings.
RING_EVIDENCE_TIMEOUT_MS = 12000

# Hard cap on a single ring, independent of any URC. Backstop only: if this
# is what stops the ringer, URC handling or CLCC reconciliation has failed.
RING_MAX_MS = 30000


def ucallstat_is_disconnected(payload: str) -> bool:
	"""payload is the text after '+UCALLSTAT:' (e.g. '1,6' or '1,6,1')."""
	parts = [p.strip() for p in payload.strip().split(",")]
	if len(parts) < 2:
		return False
	return parts[1] == "6"


def cpas_implies_remote_end(
	*,
	outbound_active: bool,
	saw_in_call_cpas: bool,
	cpas: str,
) -> bool:
	"""After we have seen CPAS=4, a return to ready means the call ended."""
	return outbound_active and saw_in_call_cpas and cpas == "0"


def clcc_absent_ends_call(
	*,
	session_active: bool,
	consecutive_absent: int,
	limit: int = CLCC_ABSENT_LIMIT,
) -> bool:
	"""End a session that AT+CLCC says has no calls.

	session_active covers ringing as well as outbound calls. Polling only
	outbound calls is what let a ringer run ~24 s past the caller hanging
	up: the cancel URC was corrupted, and nothing re-checked whether the
	call still existed. The same poll already tears an outbound call down
	correctly when its end URC is lost.

	Unlike cpas_implies_remote_end() this does not require the call to have
	been active first, which is the case it exists for: a dial the network
	refused set the session flag without ever reaching an active state, so
	nothing could clear it and the UI stayed on "Dialing" forever.
	"""
	return session_active and consecutive_absent >= limit


def is_incoming_ring_evidence(
	*,
	ucall_stat: int | None,
	saw_ring_urc: bool,
) -> bool:
	"""Does this URC drain prove an incoming call is still ringing?

	+UCALLSTAT: 1,4 counts because some networks announce an incoming call
	with it and never send a bare RING. Keying liveness off RING alone left
	the timestamp unset for the entire call, which disabled the expiry
	check that is guarded on it.
	"""
	if saw_ring_urc:
		return True
	return ucall_stat == UCALL_RINGING_MT


def ring_evidence_expired(
	*,
	now_ms: int,
	last_evidence_ms: int,
	timeout_ms: int = RING_EVIDENCE_TIMEOUT_MS,
) -> bool:
	"""Has an incoming call gone silent long enough to be considered gone?

	last_evidence_ms of 0 means no evidence has been stamped yet, so there
	is no baseline to measure and the ring cannot be judged stale. Session
	teardown must reset it to 0 for exactly this reason: a leftover
	timestamp from a previous call made the next ring look instantly
	expired and the phone stopped alerting altogether.

	now_ms earlier than last_evidence_ms means millis() wrapped (~49.7
	days). Treat that as fresh rather than tearing down a live call.
	"""
	if last_evidence_ms == 0:
		return False
	if now_ms < last_evidence_ms:
		return False
	return now_ms - last_evidence_ms > timeout_ms
