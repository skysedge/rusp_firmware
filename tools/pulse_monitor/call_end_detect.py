"""Detect remote call end from modem URCs / CPAS transitions.

Why: OLED stayed on CALLING after the far end hung up because firmware
only looked for NO CARRIER (fragile one-byte URC match) and ignored
+UCALLSTAT disconnect (6) and CPAS returning to ready after in-call.
"""

from __future__ import annotations


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
