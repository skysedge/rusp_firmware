"""
Caller identification: reading +CLIP, and choosing what the bottom line says.

The modem reports the calling number in a +CLIP URC sent alongside each RING,
once AT+CLIP=1 has been applied at boot. That is preferred over the number
field in +CLCC because it arrives with the first ring rather than at the next
poll tick, and because the pollers share a single transaction slot.

Mirrors parse_clip_number() in lara.cpp and the bottom-band selection in
ui_refresh() in rusp_firmware.ino.
"""

import enum

# E.164 allows 15 digits; the leading '+' of an international number makes 16.
CALLER_ID_MAX_LEN = 16

# Shown while ringing when the network gives us no number to show.
CALLER_ID_UNKNOWN = "Unknown"

class CallerLineEvent(enum.Enum):
	"""Things that can hand the bottom line to, or take it from, a caller."""

	RING_STARTED = "ring_started"
	CALL_ENDED = "call_ended"
	CALL_REJECTED = "call_rejected"
	DIAL_DIGIT = "dial_digit"
	CLEAR_DIGIT = "clear_digit"
	DISPLAY_SLEPT = "display_slept"
	OUTBOUND_STARTED = "outbound_started"


# Events after which a caller no longer owns the bottom line. Ending a call is
# deliberately absent: the number has to outlive the call so that "Rejected" or
# "Call ended" is shown next to whoever it refers to.
#
# CLEAR_DIGIT releases the line but CALL_REJECTED does not, even though both
# come from the C button. Pressing C on a ringing call rejects it, and clearing
# the number then would erase the very thing the status refers to; pressing C
# with no call is an edit of the dial buffer, which the user must be able to
# see.
_RELEASING = frozenset({
	CallerLineEvent.DIAL_DIGIT,
	CallerLineEvent.CLEAR_DIGIT,
	CallerLineEvent.DISPLAY_SLEPT,
	CallerLineEvent.OUTBOUND_STARTED,
})


def caller_line_after(active: bool, event: CallerLineEvent) -> bool:
	"""Whether an incoming call still owns the bottom line after `event`.

	Only a new ring claims the line, so an event arriving while it is
	inactive cannot activate it -- an outbound call ending must not
	present an empty caller ID as "Unknown".
	"""
	if event is CallerLineEvent.RING_STARTED:
		return True
	if event in _RELEASING:
		return False
	return active


# The only characters a dialable number can contain.
_DIALABLE = set("+0123456789*#")

_CLIP_PREFIX = "+CLIP:"


def parse_clip_number(line: str) -> str | None:
	"""Extract the calling number from a +CLIP URC line.

	Returns None whenever the line does not carry a number that could
	plausibly be one: a withheld caller (empty quotes), an unrelated URC,
	or a damaged line.

	Damage is not hypothetical here. Hardware flow control left enabled
	once caused the module to gate its own transmitter mid-message, and
	URCs arrived as a lone "R" or "+". Unsolicited output is never
	retransmitted, so a mangled +CLIP cannot be re-requested -- the only
	safe response is to report no caller. Truncating an overlong or
	partial number instead would put a different, real-looking number on
	the panel, which is a worse failure than showing none because the user
	cannot tell it is wrong.
	"""
	if not line:
		return None
	if not line.startswith(_CLIP_PREFIX):
		return None
	return parse_clip_payload(line[len(_CLIP_PREFIX):])


def parse_clip_payload(payload: str) -> str | None:
	"""As parse_clip_number(), for the text following "+CLIP:".

	This split exists because the firmware's byte-oriented URC matcher
	consumes the prefix as it recognises it, so only the payload survives
	to be parsed. lara.cpp mirrors this function, not the one above.
	"""
	payload = payload.lstrip()
	if not payload.startswith('"'):
		return None
	end = payload.find('"', 1)
	if end < 0:
		return None

	number = payload[1:end]
	if not number:
		return None
	if len(number) > CALLER_ID_MAX_LEN:
		return None
	if any(ch not in _DIALABLE for ch in number):
		return None
	return number


def bottom_line_number(
	caller_line_active: bool, caller_id: str | None, dial_buf: str
) -> str | None:
	"""The number the bottom line should render, or None for no number.

	An incoming call outranks the dial buffer for as long as it owns the
	line. The typed digits are only hidden, not discarded, and return when
	caller_line_after() hands the line back.

	caller_id is ignored while the line is inactive, so a number cannot
	label a screen that no longer refers to it. That staleness is the same
	class of fault as the ring-evidence timestamp which, when it survived
	teardown, made every subsequent call look instantly expired and
	stopped the phone alerting at all.
	"""
	if caller_line_active:
		return caller_id or None
	return dial_buf or None


def bottom_line_label(
	caller_line_active: bool, caller_id: str | None
) -> str | None:
	"""A literal word for the bottom line, or None to use the number path.

	Kept separate from bottom_line_number() because the caller returned by
	that function is run through the phone-number formatter. "Unknown" put
	through the same path would be rearranged into punctuation.
	"""
	if caller_line_active and not caller_id:
		return CALLER_ID_UNKNOWN
	return None
