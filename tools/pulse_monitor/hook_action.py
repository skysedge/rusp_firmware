"""Resolve what the call button should do given modem + local call state.

Why: after ATD the modem may still report CPAS ready (0) while an outbound
call is up. Firmware that trusts only CPAS==4 will re-dial instead of hang up,
leaving the OLED on DIALING/DIAL OK.

Display gating: hangup always runs; dial/answer only when the display is on.
A press while the display is asleep wakes it but must not start a call.
"""

from __future__ import annotations

from enum import Enum


class HookAction(Enum):
	ANSWER = "ANSWER"
	HANGUP = "HANGUP"
	DIAL = "DIAL"
	REFUSE_EMPTY = "REFUSE_EMPTY"
	UNHANDLED = "UNHANDLED"
	# Display was off: wake only — do not dial or answer.
	WAKE_ONLY = "WAKE_ONLY"


def resolve_hook_action(
	*,
	cpas: str,
	outbound_call_active: bool,
	has_digits: bool,
	display_awake: bool = True,
) -> HookAction:
	"""cpas is the single digit character from AT+CPAS (e.g. '0', '3', '4')."""
	# Hangup always — even with the display asleep.
	if outbound_call_active or cpas == "4":
		return HookAction.HANGUP

	# Call start (answer / dial) requires an already-awake display.
	if not display_awake:
		return HookAction.WAKE_ONLY

	if cpas == "3":
		return HookAction.ANSWER
	if cpas == "0":
		return HookAction.DIAL if has_digits else HookAction.REFUSE_EMPTY
	return HookAction.UNHANDLED
