"""Map modem call-status codes to OLED call-phase labels.

+UCALLSTAT and +CLCC share the same <stat> values for 0..5; UCALLSTAT
also adds 6 (disconnected) and 7 (connected).
"""

from __future__ import annotations

# u-blox +UCALLSTAT / 3GPP +CLCC <stat> (LARA AT commands manual)
UCALL_ACTIVE = 0
UCALL_HOLD = 1
UCALL_DIALLING = 2
UCALL_ALERTING = 3  # MO: remote ringing
UCALL_RINGING_MT = 4  # MT: incoming
UCALL_WAITING = 5
UCALL_DISCONNECTED = 6
UCALL_CONNECTED = 7


def call_phase_label(ucall_stat: int) -> str | None:
	"""Return OLED status text, or None if the code is ignored."""
	return {
		UCALL_DIALLING: "Dialing",
		UCALL_ALERTING: "Ringing",
		UCALL_RINGING_MT: "Incoming",
		UCALL_ACTIVE: "In call",
		UCALL_CONNECTED: "In call",
		UCALL_DISCONNECTED: "Call ended",
	}.get(ucall_stat)


def parse_ucallstat_stat(payload: str) -> int | None:
	"""
	Parse <stat> from text after '+UCALLSTAT:'.
	Accepts spaces: '1,7', ' 1, 0', '1,6,1'.
	"""
	parts = [p.strip() for p in payload.strip().split(",")]
	if len(parts) < 2 or not parts[1].isdigit():
		return None
	return int(parts[1])


def parse_clcc_stat(line: str) -> int | None:
	"""
	Parse call <stat> (3rd field) from a '+CLCC: ...' information line.
	Example: '+CLCC: 1,0,0,0,0,\"5551234\",129' → 0 (active).
	"""
	text = line.strip()
	prefix = "+CLCC:"
	if not text.upper().startswith(prefix):
		return None
	body = text[len(prefix) :].strip()
	parts = [p.strip() for p in body.split(",")]
	if len(parts) < 3 or not parts[2].isdigit():
		return None
	return int(parts[2])


def prefer_clcc_stat(stats: list[int]) -> int | None:
	"""
	Pick the most informative call state when several +CLCC lines exist.
	Active wins over alerting over dialling over incoming.
	"""
	if not stats:
		return None
	for wanted in (
		UCALL_ACTIVE,
		UCALL_ALERTING,
		UCALL_DIALLING,
		UCALL_RINGING_MT,
		UCALL_WAITING,
		UCALL_HOLD,
	):
		if wanted in stats:
			return wanted
	return stats[0]
