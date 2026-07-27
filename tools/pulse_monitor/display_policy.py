"""
When the OLED blanks, and what lights it again.

Mirrors the panel sleep/wake branch in loop() in rusp_firmware.ino.
"""

import enum


class DisplayAction(enum.Enum):
	"""What to do to the panel this iteration."""

	WAKE = "wake"
	SLEEP = "sleep"
	NONE = "none"


# What the status line reads when nothing is going on.
IDLE_STATUS = "Ready"


def status_on_sleep(status: str, call_active: bool) -> str:
	"""The status to keep once the panel blanks.

	Blanking ends the episode. It already hands the bottom line back from
	the caller, and the status has to go with it: an outcome that outlives
	the number identifying it left the phone showing "Rejected" on both
	lines -- once as the status, and again because the bottom band echoes
	the status when it has no number to draw -- describing a call the user
	could no longer identify or date.

	A call still in progress is the exception, and waking to "Ready"
	mid-call would say the user had hung up when they had not. A connected
	call now holds the panel awake outright, so the idle timer can no
	longer reach this case; the branch is kept because the hold and this
	function are separate decisions, and dropping the in-call hold would
	otherwise silently reintroduce the wrong status.
	"""
	if call_active:
		return status
	return IDLE_STATUS


def holds_panel_awake(charging: bool, ringing: bool, in_call: bool) -> bool:
	"""Is something currently entitled to keep the panel lit?"""
	return charging or ringing or in_call


def next_idle_ms(
	idle_ms: int,
	elapsed_ms: int,
	charging: bool,
	ringing: bool,
	in_call: bool,
) -> int:
	"""Advance the idle timer, holding it at zero while the panel is held.

	Suppressing sleep without restarting the timer is not enough. The
	timer would keep running underneath the hold, so a call lasting longer
	than the timeout leaves the panel already overdue, and it blanks in
	the same frame the call ends -- taking the "Rejected" status and the
	caller's number with it. Time spent on a call or on the charger is not
	time spent ignoring the phone, so it does not count towards going
	idle.
	"""
	if holds_panel_awake(charging, ringing, in_call):
		return 0
	return idle_ms + elapsed_ms


def display_action(
	display_awake: bool,
	idle_ms: int,
	timeout_ms: int,
	charging: bool,
	ringing: bool,
	in_call: bool,
) -> DisplayAction:
	"""Decide whether to light, blank, or leave the panel alone.

	Charging, ringing, and a connected call each hold the panel awake, and
	each lights it if it is already dark. Ringing is stated explicitly
	rather than left to the idle timer: rings arrive roughly every 5 s
	against a 20 s timeout, so each one would normally count as activity
	and keep the screen lit, but that is coincidence. A ring URC lost to a
	throttled UART -- which this modem has done -- would let the panel
	blank in the middle of a call, and blanking also releases the caller
	line, so the number on the bottom line would be lost with it.

	The connected leg needs its own hold because the ringing one ends the
	instant the call is answered, and a call in progress generates no user
	input at all. That blanked the panel ~20 s into every call: a capture
	of a working outgoing call shows OLED_SLEEP while the modem still
	reported CLCC state 0.

	All three return NONE when the panel is already lit. The panel-on
	sequence forces a full repaint, and re-issuing it every iteration is
	the sort of redundant SPI traffic that has previously blocked the CPU
	long enough to overrun the modem receive buffer.
	"""
	if holds_panel_awake(charging, ringing, in_call):
		return DisplayAction.WAKE if not display_awake else DisplayAction.NONE
	if display_awake and idle_ms >= timeout_ms:
		return DisplayAction.SLEEP
	return DisplayAction.NONE
