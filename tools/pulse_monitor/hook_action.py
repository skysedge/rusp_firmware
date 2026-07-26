"""Resolve what the call button should do given modem + local call state.

Why: after ATD the modem may still report CPAS ready (0) while an outbound
call is up. Firmware that trusts only CPAS==4 will re-dial instead of hang up,
leaving the OLED on DIALING/DIAL OK.

Display gating: hangup always runs; dial/answer only when the display is on.
A press while the display is asleep wakes it but must not start a call.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class HookAction(Enum):
	ANSWER = "ANSWER"
	HANGUP = "HANGUP"
	DIAL = "DIAL"
	REFUSE_EMPTY = "REFUSE_EMPTY"
	UNHANDLED = "UNHANDLED"
	# Display was off: wake only — do not dial or answer.
	WAKE_ONLY = "WAKE_ONLY"
	# Empty buffer, but a previous number is stored: load and show it.
	# Deliberately not a dial — the user has not seen the number yet.
	RECALL_LAST = "RECALL_LAST"


def resolve_hook_action(
	*,
	cpas: str,
	outbound_call_active: bool,
	has_digits: bool,
	display_awake: bool = True,
	ringing: bool = False,
	has_stored_last_number: bool = False,
) -> HookAction:
	"""cpas is the single digit character from AT+CPAS (e.g. '0', '3', '4').

	ringing comes from the RING URC or +UCALLSTAT 4. It is the answer-side
	counterpart to outbound_call_active: both survive a CPAS read that timed
	out ('2' / LARA_UNKNOWN), so neither answering nor hanging up depends on
	one AT transaction succeeding.
	"""
	# Hangup always — even with the display asleep, and even if a stale
	# ringing flag is still set.
	if outbound_call_active or cpas == "4":
		return HookAction.HANGUP

	# Call start (answer / dial) requires an already-awake display.
	if not display_awake:
		return HookAction.WAKE_ONLY

	# Answer is checked before dial: digits left in the buffer must not turn
	# an incoming call into an outgoing one.
	if cpas == "3" or ringing:
		return HookAction.ANSWER
	if cpas == "0":
		if has_digits:
			return HookAction.DIAL
		# Recall shows the number and waits for a second press. Dialling
		# here would place a call to a number the user cannot see until
		# it is already ringing.
		if has_stored_last_number:
			return HookAction.RECALL_LAST
		return HookAction.REFUSE_EMPTY
	return HookAction.UNHANDLED


@dataclass
class DialState:
	"""The two buffers firmware keeps for a number being dialled.

	dial_buf is what ATD sends. shown_digits mirrors oled_dialed_digits,
	which is the only thing the user can actually read — ui_refresh() does
	not render dial_buf. They are written by different code paths
	(handle_completed_digit and show_dialed_digit_on_oled), so they have to
	be kept in step by hand.
	"""

	dial_buf: str = ""
	shown_digits: str = ""


def apply_recall_last(state: DialState, stored_number: str) -> DialState:
	"""Load a stored number into the dial buffer *and* onto the display.

	Both writes are required. Filling dial_buf alone loads the number
	invisibly: nothing on screen changes, so the button looks broken, and
	the second press then dials a number the user never saw — which is the
	risk RECALL_LAST is meant to remove.

	Assigns rather than appends: the display string is normally built one
	digit at a time, and appending here would splice the recalled number
	onto leftover digits and dial the result.

	Raises ValueError on an empty number so a zero-length record surfaces
	as REFUSE_EMPTY instead of silently blanking the screen.
	"""
	if not stored_number:
		raise ValueError("recall requires a stored number")
	return DialState(dial_buf=stored_number, shown_digits=stored_number)
