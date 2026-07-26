"""Tests for call-button action while an outbound call may still look READY.

Why these tests exist: re-pressing call after dial must hang up even when
CPAS has not moved to 4. Otherwise the UI stays on DIALING/DIAL OK.
Display-asleep presses must wake without starting a call; hangup still works.

Failure modes when regressions occur:
- Trusting only CPAS==4 → second press re-dials (screen shows DIALING again).
- Ignoring inbound RING (3) → answer path lost.
- Empty buffer still dials → ATD with empty string.
- Dial/answer while display asleep → call starts from a dark screen.
- Hangup blocked while asleep → cannot end an active call.
"""

import unittest

from hook_action import (
	DialState,
	HookAction,
	apply_recall_last,
	resolve_hook_action,
)


class RecallLastNumberTests(unittest.TestCase):
	"""Pressing call with an empty buffer offers the previous number.

	Why: the press loads the stored number and shows it, but must not
	dial. Dialling straight from an empty-buffer press would place a call
	the user never confirmed — the number they get is whatever was dialled
	last, which they cannot see until it is already ringing. A second
	press then dials it through the ordinary path.
	"""

	def test_empty_buffer_with_a_stored_number_recalls_it(self):
		# The feature. Failure: REFUSE_EMPTY, and redial does nothing.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=False,
				has_stored_last_number=True,
			),
			HookAction.RECALL_LAST,
		)

	def test_recall_does_not_dial_on_the_same_press(self):
		# Why: the recalled number must be visible and confirmable before
		# it is called. Failure: one press on an empty buffer silently
		# places a call to an unseen number.
		self.assertNotEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=False,
				has_stored_last_number=True,
			),
			HookAction.DIAL,
		)

	def test_second_press_dials_the_recalled_number(self):
		# Models the state after recall filled the buffer. Failure: the
		# recalled number can be shown but never dialled.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=True,
				has_stored_last_number=True,
			),
			HookAction.DIAL,
		)

	def test_empty_buffer_without_a_stored_number_still_refuses(self):
		# The null case, and the behaviour on a phone that has never
		# dialled. Failure: the firmware tries to recall nothing and
		# reports success, so the user gets no "No number" feedback.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=False,
				has_stored_last_number=False,
			),
			HookAction.REFUSE_EMPTY,
		)

	def test_recall_never_happens_from_a_dark_display(self):
		# Why: the whole point of recall is seeing the number before
		# dialling, which is impossible with the screen off. The existing
		# wake-only rule must win. Failure: a press on a sleeping phone
		# silently arms a call to an unseen number.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=False,
				display_awake=False,
				has_stored_last_number=True,
			),
			HookAction.WAKE_ONLY,
		)

	def test_recall_never_preempts_answering_an_incoming_call(self):
		# Why: an empty buffer is the normal state when a call arrives.
		# Failure: the call button recalls the last number instead of
		# answering, and the phone cannot be answered at all.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=False,
				ringing=True,
				has_stored_last_number=True,
			),
			HookAction.ANSWER,
		)

	def test_recall_never_preempts_hanging_up(self):
		# Why: hangup outranks everything. Failure: pressing call during
		# an active call recalls a number instead of ending the call.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=True,
				has_digits=False,
				has_stored_last_number=True,
			),
			HookAction.HANGUP,
		)

	def test_stored_number_is_irrelevant_when_digits_are_present(self):
		# Why: a half-dialled number must never be replaced by the stored
		# one. Failure: the user's in-progress digits are overwritten.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=True,
				has_stored_last_number=False,
			),
			HookAction.DIAL,
		)


class HookActionTests(unittest.TestCase):
	def test_hangup_when_outbound_active_even_if_cpas_ready(self):
		# Guards stale CPAS after ATD. Failure: action is DIAL, OLED stays dialing.
		self.assertEqual(
			resolve_hook_action(
				cpas="0", outbound_call_active=True, has_digits=True
			),
			HookAction.HANGUP,
		)

	def test_hangup_when_cpas_calling(self):
		# Guards normal in-call CPAS. Failure: hangup never selected.
		self.assertEqual(
			resolve_hook_action(
				cpas="4", outbound_call_active=False, has_digits=True
			),
			HookAction.HANGUP,
		)

	def test_hangup_even_when_display_asleep(self):
		# Why: end-call must work from a blanked display.
		# Failure: WAKE_ONLY leaves the call up.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=True,
				has_digits=True,
				display_awake=False,
			),
			HookAction.HANGUP,
		)

	def test_answer_when_ringing(self):
		# Guards inbound answer. Failure: hangup/dial steals the button.
		self.assertEqual(
			resolve_hook_action(
				cpas="3", outbound_call_active=False, has_digits=False
			),
			HookAction.ANSWER,
		)

	def test_answer_when_display_asleep(self):
		# Why: the display sleeps while a call rings, so requiring it to be
		# awake made answering take two presses -- one to wake, one to
		# answer. Measured on hardware as a 4 s gap between the two.
		# The awake rule exists to stop a stray press placing a call; a
		# stray press on a ringing phone only answers one already offered.
		# Failure: WAKE_ONLY, and the caller keeps ringing.
		self.assertEqual(
			resolve_hook_action(
				cpas="3",
				outbound_call_active=False,
				has_digits=False,
				display_awake=False,
			),
			HookAction.ANSWER,
		)

	def test_answer_when_display_asleep_via_ringing_flag(self):
		# Why: ringing survives a CPAS read that timed out, so answering
		# from sleep must not depend on CPAS having succeeded.
		# Failure: WAKE_ONLY whenever CPAS is unknown during a ring.
		self.assertEqual(
			resolve_hook_action(
				cpas="2",
				outbound_call_active=False,
				has_digits=False,
				display_awake=False,
				ringing=True,
			),
			HookAction.ANSWER,
		)

	def test_still_no_dial_when_display_asleep_and_not_ringing(self):
		# Why: relaxing the awake rule for answering must not relax it for
		# dialling, which is the case it was written for.
		# Failure: ATD fires from a dark screen on a pocket press.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=True,
				display_awake=False,
			),
			HookAction.WAKE_ONLY,
		)

	def test_dial_when_ready_idle_with_digits(self):
		# Guards first place-call. Failure: refused or hangup with no call.
		self.assertEqual(
			resolve_hook_action(
				cpas="0", outbound_call_active=False, has_digits=True
			),
			HookAction.DIAL,
		)

	def test_no_dial_when_display_asleep(self):
		# Why: first call-button press on a dark display wakes only.
		# Failure: ATD fires before the user can see the number.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=True,
				display_awake=False,
			),
			HookAction.WAKE_ONLY,
		)

	def test_refuse_empty_when_ready_idle(self):
		# Guards empty ATD. Failure: dials empty buffer.
		self.assertEqual(
			resolve_hook_action(
				cpas="0", outbound_call_active=False, has_digits=False
			),
			HookAction.REFUSE_EMPTY,
		)


class RingFallbackTests(unittest.TestCase):
	"""Answering must not depend on a readable CPAS response.

	Hangup already falls back to outbound_call_active when CPAS is stale or
	lost. Answering had no equivalent: it required cpas == '3' exactly, so a
	CPAS read that timed out (LARA_UNKNOWN, '2') left a visibly and audibly
	ringing phone unanswerable. `ringing` is set by the RING URC and by
	+UCALLSTAT 4, both of which survive a failed AT transaction.
	"""

	def test_answer_when_ring_seen_but_cpas_unreadable(self):
		# Failure: UNHANDLED — the button does nothing while it rings.
		self.assertEqual(
			resolve_hook_action(
				cpas="2",
				outbound_call_active=False,
				has_digits=False,
				ringing=True,
			),
			HookAction.ANSWER,
		)

	def test_ring_beats_dial_when_digits_are_buffered(self):
		# Why: ordering. A number left in the buffer must not turn an
		# incoming call into an outgoing one.
		# Failure: DIAL — ATD fires while the phone is ringing.
		self.assertEqual(
			resolve_hook_action(
				cpas="0",
				outbound_call_active=False,
				has_digits=True,
				ringing=True,
			),
			HookAction.ANSWER,
		)

	def test_ring_does_not_override_hangup(self):
		# Why: a stale ringing flag alongside a live call must still end
		# the call. Failure: ATA on top of an active call.
		self.assertEqual(
			resolve_hook_action(
				cpas="4",
				outbound_call_active=True,
				has_digits=False,
				ringing=True,
			),
			HookAction.HANGUP,
		)

	def test_ring_answers_from_a_dark_display(self):
		# Why: the panel sleeps while a call rings, so gating answer on it
		# made picking up take two presses. Covers the ringing fallback
		# specifically, where CPAS ('2') gave no usable answer.
		# Failure: WAKE_ONLY, and the caller keeps ringing.
		self.assertEqual(
			resolve_hook_action(
				cpas="2",
				outbound_call_active=False,
				has_digits=False,
				ringing=True,
				display_awake=False,
			),
			HookAction.ANSWER,
		)

	def test_unknown_cpas_without_ring_is_still_unhandled(self):
		# Why: the fallback must not answer calls that do not exist.
		# Failure: ATA whenever a CPAS read times out.
		self.assertEqual(
			resolve_hook_action(
				cpas="2",
				outbound_call_active=False,
				has_digits=True,
				ringing=False,
			),
			HookAction.UNHANDLED,
		)


class RecallDisplayTests(unittest.TestCase):
	"""A recalled number must be visible, not just loaded.

	Firmware keeps the dialled number in two places: dial_buf, which ATD
	sends, and the OLED digit string, which is what the user actually reads.
	show_dialed_digit_on_oled() appends to the second while the dial path
	fills the first, so they are easy to let drift apart.
	"""

	def test_recall_fills_both_the_dial_buffer_and_the_display(self):
		# Why: the first version wrote dial_buf only. The number loaded
		# silently — the screen did not change, so RECALL_LAST looked
		# like a dead button, and the second press dialled a number the
		# user had never seen. That is the hazard RECALL_LAST exists to
		# avoid, so the display write is part of the behaviour, not
		# cosmetic.
		# Failure: shown_digits stays "" while dial_buf holds the number.
		state = apply_recall_last(DialState(), "5551234")
		self.assertEqual(state.dial_buf, "5551234")
		self.assertEqual(state.shown_digits, "5551234")

	def test_recall_replaces_rather_than_appends_to_the_display(self):
		# Why: the only existing writer of the display string appends one
		# digit at a time. Reusing that habit here concatenates the
		# recalled number onto whatever was left on screen.
		# Failure: shown_digits becomes "9995551234" and the user dials
		# a number that is not the one stored.
		state = apply_recall_last(DialState(dial_buf="999", shown_digits="999"), "5551234")
		self.assertEqual(state.dial_buf, "5551234")
		self.assertEqual(state.shown_digits, "5551234")

	def test_recall_keeps_the_two_buffers_identical(self):
		# Why: what is dialled must be what was shown. Any divergence
		# means the user reads one number and the modem calls another.
		# Failure: the two fields differ.
		for number in ("1", "5551234", "+15551234567", "0" * 29):
			with self.subTest(number=number):
				state = apply_recall_last(DialState(), number)
				self.assertEqual(state.dial_buf, state.shown_digits)
				self.assertEqual(state.dial_buf, number)

	def test_empty_stored_number_is_not_a_recall(self):
		# Why: an empty string passes a naive truth test in neither
		# language, but guarding here documents that a zero-length record
		# must reach REFUSE_EMPTY instead of blanking the screen and
		# leaving the user with no feedback at all.
		# Failure: both buffers are cleared and nothing is displayed.
		with self.assertRaises(ValueError):
			apply_recall_last(DialState(), "")


if __name__ == "__main__":
	unittest.main()
