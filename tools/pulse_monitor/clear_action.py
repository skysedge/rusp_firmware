"""Resolve what the C (clear) button should do.

C deletes the last dial digit. While a call is ringing there is nothing worth
deleting and an obvious thing to want instead, so it rejects the call.

Precedence, highest first:

1. Ringing -> reject. Checked before the display gate because the panel sleeps
   while a call rings; gating on it would make rejecting take two presses, the
   same defect that made answering take two. A press cannot be accidental in
   the way a dial press can -- the phone is audibly ringing and the user is
   responding to it.
2. Display asleep -> wake only. A digit deleted from a dark screen vanishes
   before the user has seen the number.
3. Deletion would eat into the dialling prefix -> refuse. The prefix is not
   user-entered, and a number missing it cannot be dialled.
4. Otherwise delete one digit.

Mirrors the clear_request handler in rusp_firmware.ino.
"""

from __future__ import annotations

from enum import Enum


class ClearAction(Enum):
	# Reject the incoming call (AT+CHUP).
	REJECT_CALL = "REJECT_CALL"
	DELETE_DIGIT = "DELETE_DIGIT"
	# Display was off: wake only — do not edit the buffer.
	WAKE_ONLY = "WAKE_ONLY"
	# Deleting here would consume the dialling prefix.
	REFUSE_DELETE = "REFUSE_DELETE"


def resolve_clear_action(
	*,
	ringing: bool,
	display_awake: bool = True,
	in_local_prepend: bool = False,
) -> ClearAction:
	"""ringing comes from the RING URC, +UCALLSTAT 4/5, or a +CLCC entry
	reporting a mobile-terminated call that is alerting.

	in_local_prepend is true when the buffer holds no more than the dialling
	prefix, so the next deletion would start consuming it.
	"""
	if ringing:
		return ClearAction.REJECT_CALL
	if not display_awake:
		return ClearAction.WAKE_ONLY
	if in_local_prepend:
		return ClearAction.REFUSE_DELETE
	return ClearAction.DELETE_DIGIT
