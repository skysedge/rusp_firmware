"""Tests for display_policy: when the OLED blanks and when it lights up."""

import unittest

from display_policy import (
	DisplayAction,
	display_action,
	IDLE_STATUS,
	next_idle_ms,
	status_on_sleep,
)

TIMEOUT_MS = 20_000
BEFORE_TIMEOUT_MS = TIMEOUT_MS - 1
WELL_PAST_TIMEOUT_MS = TIMEOUT_MS * 10


def _action(**overrides) -> DisplayAction:
	"""An idle, awake, uneventful phone, with the case under test applied."""
	args = {
		"display_awake": True,
		"idle_ms": 0,
		"timeout_ms": TIMEOUT_MS,
		"charging": False,
		"ringing": False,
		"in_call": False,
	}
	args.update(overrides)
	return display_action(**args)


def _next_idle(**overrides) -> int:
	"""Advance the idle timer on an unheld phone, with the case applied."""
	args = {
		"idle_ms": 0,
		"elapsed_ms": 0,
		"charging": False,
		"ringing": False,
		"in_call": False,
	}
	args.update(overrides)
	return next_idle_ms(**args)


class IncomingCallTests(unittest.TestCase):
	"""An incoming call lights the panel and keeps it lit.

	Ring URCs repeat about every 5 s against a 20 s idle timeout, so the
	panel would usually stay lit merely because each ring counts as
	activity. That is an accident, not a guarantee: this modem has already
	been observed dropping URCs to a throttled UART, and a blanked panel
	also releases the caller line, so the number would be lost with it.
	"""

	def test_ring_wakes_a_sleeping_panel(self):
		# Why: the phone must show who is calling without being touched.
		# Failure: the call is silent and invisible until the user
		# happens to press something.
		self.assertIs(
			_action(display_awake=False, ringing=True),
			DisplayAction.WAKE,
		)

	def test_ring_holds_the_panel_past_the_idle_timeout(self):
		# Why: the guarantee that repeated rings only imply. A long ring,
		# or one whose URCs were dropped, must not blank the screen.
		# Failure: the panel goes dark mid-call and takes the caller ID
		# with it, because sleeping releases the caller line.
		self.assertIs(
			_action(idle_ms=WELL_PAST_TIMEOUT_MS, ringing=True),
			DisplayAction.NONE,
		)

	def test_an_already_lit_panel_is_not_woken_again(self):
		# Why: ringing is evaluated every loop. Re-waking would re-issue
		# the panel-on sequence and force a full repaint each iteration,
		# which is the kind of redundant SPI traffic that has previously
		# overrun the modem UART receive buffer.
		# Failure: continuous repaints for the duration of the ring.
		self.assertIs(
			_action(display_awake=True, ringing=True),
			DisplayAction.NONE,
		)


class ChargingTests(unittest.TestCase):
	"""Charging already held the panel awake; that must not regress."""

	def test_charging_wakes_a_sleeping_panel(self):
		# Why: existing behaviour, kept when ringing joined this branch.
		# Failure: plugging in no longer lights the phone.
		self.assertIs(
			_action(display_awake=False, charging=True),
			DisplayAction.WAKE,
		)

	def test_charging_holds_the_panel_past_the_idle_timeout(self):
		# Why: existing behaviour -- a charging phone stays readable.
		# Failure: the panel blanks while on the cable.
		self.assertIs(
			_action(idle_ms=WELL_PAST_TIMEOUT_MS, charging=True),
			DisplayAction.NONE,
		)


class ConnectedCallTests(unittest.TestCase):
	"""A call that is up holds the panel lit for as long as it lasts.

	Ringing already held the panel, but the hold ended the moment the
	call was answered, so the panel blanked ~20 s into every call. A
	capture of a working outgoing call shows exactly that: OLED_SLEEP at
	t=56146 with the modem still reporting CLCC state 0. Blanking also
	releases the caller line, so the number went with it, and the call
	summary shown on hanging up had nothing left to identify.
	"""

	def test_a_connected_call_holds_the_panel_past_the_idle_timeout(self):
		# Why: the reported behaviour -- the screen went dark mid-call
		# because nothing but user input reset the idle timer, and a call
		# in progress involves no input at all.
		# Failure: the panel blanks OLED_IDLE_TIMEOUT_MS into any call
		# lasting longer than that, which is most of them.
		self.assertIs(
			_action(idle_ms=WELL_PAST_TIMEOUT_MS, in_call=True),
			DisplayAction.NONE,
		)

	def test_a_connected_call_wakes_a_dark_panel(self):
		# Why: the hold has to survive the handover from ringing. If it
		# only suppressed sleep, a call answered after the panel had
		# already blanked would stay dark for its whole duration.
		# Failure: answering from a dark screen leaves it dark.
		self.assertIs(
			_action(display_awake=False, in_call=True),
			DisplayAction.WAKE,
		)

	def test_an_already_lit_panel_is_not_woken_again(self):
		# Why: this branch runs every loop iteration for the length of
		# the call. Re-waking re-issues the panel-on sequence and forces
		# a full repaint, and that redundant SPI traffic has previously
		# blocked the CPU long enough to overrun the modem UART.
		# Failure: continuous repaints for the whole call.
		self.assertIs(
			_action(display_awake=True, in_call=True),
			DisplayAction.NONE,
		)

	def test_the_panel_blanks_normally_once_the_call_ends(self):
		# Why: the hold must be scoped to the call and nothing more,
		# otherwise the phone never blanks again after its first call.
		# Failure: the OLED stays lit indefinitely post-call.
		self.assertIs(
			_action(idle_ms=WELL_PAST_TIMEOUT_MS, in_call=False),
			DisplayAction.SLEEP,
		)


class IdleTests(unittest.TestCase):
	"""Ordinary blanking, which the two hold conditions override."""

	def test_idle_past_the_timeout_sleeps(self):
		# Why: the reason the timeout exists -- an untouched phone must
		# blank to save the panel and power.
		# Failure: the OLED stays lit indefinitely.
		self.assertIs(
			_action(idle_ms=TIMEOUT_MS),
			DisplayAction.SLEEP,
		)

	def test_idle_short_of_the_timeout_does_nothing(self):
		# Why: the boundary. One millisecond early must not blank, or the
		# effective timeout is not the one documented.
		# Failure: the panel blanks sooner than OLED_IDLE_TIMEOUT_MS.
		self.assertIs(
			_action(idle_ms=BEFORE_TIMEOUT_MS),
			DisplayAction.NONE,
		)

	def test_a_sleeping_idle_panel_is_left_alone(self):
		# Why: the null case. Nothing is happening and the panel is
		# already off, so re-issuing sleep every loop is pure waste.
		# Failure: the panel-off sequence is sent on every iteration.
		self.assertIs(
			_action(display_awake=False, idle_ms=WELL_PAST_TIMEOUT_MS),
			DisplayAction.NONE,
		)

	def test_zero_idle_time_does_nothing(self):
		# Why: the zero case -- activity just happened.
		# Failure: an off-by-one comparison blanks the panel the instant
		# the user touches it.
		self.assertIs(_action(idle_ms=0), DisplayAction.NONE)


class IdleTimerTests(unittest.TestCase):
	"""The idle timer while something is holding the panel awake.

	A hold that only suppresses sleep leaves the timer running underneath
	it, so the moment the hold lifts the panel is already overdue and
	blanks at once. Restarting the timer for as long as the hold lasts
	gives the user a full window afterwards.
	"""

	def test_ringing_keeps_the_idle_timer_at_zero(self):
		# Why: time spent ringing is not time spent ignoring the phone.
		# Failure: the timer runs during the call and expires under it.
		self.assertEqual(
			_next_idle(
				idle_ms=WELL_PAST_TIMEOUT_MS,
				elapsed_ms=100,
				ringing=True,
			),
			0,
		)

	def test_charging_keeps_the_idle_timer_at_zero(self):
		# Why: same reasoning for the pre-existing hold, so unplugging
		# does not blank the panel instantly.
		# Failure: the screen dies the moment the cable comes out.
		self.assertEqual(
			_next_idle(
				idle_ms=WELL_PAST_TIMEOUT_MS,
				elapsed_ms=100,
				charging=True,
			),
			0,
		)

	def test_a_connected_call_keeps_the_idle_timer_at_zero(self):
		# Why: a call outlasting the 20 s timeout must still leave a full
		# window when it ends, so the outcome and the caller's number are
		# readable after hanging up.
		# Failure: the timer expires silently under the call and the
		# panel blanks in the frame the call ends.
		self.assertEqual(
			_next_idle(
				idle_ms=WELL_PAST_TIMEOUT_MS,
				elapsed_ms=100,
				in_call=True,
			),
			0,
		)

	def test_a_long_ring_leaves_a_full_window_once_it_ends(self):
		# Why: the regression this guards. A 25 s ring outlasts the 20 s
		# timeout, so rejecting at the end must still leave the panel lit
		# to show "Rejected" and the caller's number.
		# Failure: the reject screen appears and vanishes in one frame,
		# because the timer expired silently during the ring.
		ring_duration_ms = TIMEOUT_MS + 5_000
		idle_ms = _next_idle(elapsed_ms=ring_duration_ms, ringing=True)
		self.assertIs(
			_action(idle_ms=idle_ms, ringing=False),
			DisplayAction.NONE,
		)

	def test_a_long_call_leaves_a_full_window_once_it_ends(self):
		# Why: the regression this guards, for the connected leg rather
		# than the ringing one. A 3 min call far outlasts the timeout, so
		# hanging up must still leave the panel lit to show "Call ended"
		# and who it was with.
		# Failure: the call summary appears and vanishes in one frame.
		call_duration_ms = TIMEOUT_MS * 9
		idle_ms = _next_idle(elapsed_ms=call_duration_ms, in_call=True)
		self.assertIs(
			_action(idle_ms=idle_ms, in_call=False),
			DisplayAction.NONE,
		)

	def test_the_timer_accumulates_when_nothing_holds_the_panel(self):
		# Why: the timeout must still work in the ordinary case.
		# Failure: the panel never blanks, because the timer is pinned.
		self.assertEqual(
			_next_idle(idle_ms=1_000, elapsed_ms=500),
			1_500,
		)


class StatusOnSleepTests(unittest.TestCase):
	"""What the status line says after the panel has blanked.

	Sleeping already hands the bottom line back from the caller. The
	status has to go with it: a call outcome that outlives the number
	identifying it leaves the phone reporting "Rejected" on both lines,
	with nothing to say who was rejected or how long ago.
	"""

	def test_sleeping_clears_a_finished_call_outcome(self):
		# Why: the reported defect. Waking after rejecting a call showed
		# "Rejected" on both lines, describing a call already gone.
		# Failure: a stale outcome greets the user on every wake until
		# something else happens to overwrite it.
		self.assertEqual(
			status_on_sleep("Rejected", call_active=False), IDLE_STATUS
		)

	def test_sleeping_during_a_live_call_keeps_the_status(self):
		# Why: the panel blanks on the idle timer while a call is up --
		# the user is holding the phone to their ear, not touching it.
		# Waking must still report the call.
		# Failure: an active call reads as "Ready", so the user believes
		# they have hung up when they have not.
		self.assertEqual(
			status_on_sleep("In call", call_active=True), "In call"
		)

	def test_sleeping_when_already_idle_changes_nothing(self):
		# Why: the null case -- an untouched phone blanking normally.
		# Failure: needless status churn marks the panel dirty and forces
		# a full repaint on every wake.
		self.assertEqual(
			status_on_sleep(IDLE_STATUS, call_active=False), IDLE_STATUS
		)

	def test_sleeping_clears_a_non_call_status_too(self):
		# Why: "No number" and similar transient messages are just as
		# stale after a blank as a call outcome is.
		# Failure: a message about something the user did minutes ago is
		# still on screen when they come back.
		self.assertEqual(
			status_on_sleep("No number", call_active=False), IDLE_STATUS
		)


if __name__ == "__main__":
	unittest.main()
