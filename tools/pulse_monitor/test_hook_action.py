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

from hook_action import HookAction, resolve_hook_action


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

	def test_no_answer_when_display_asleep(self):
		# Why: answering is call start — requires display on.
		# Failure: ATA while the panel is still dark.
		self.assertEqual(
			resolve_hook_action(
				cpas="3",
				outbound_call_active=False,
				has_digits=False,
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


if __name__ == "__main__":
	unittest.main()
