"""Tests for clear_action.

The C button deletes a dial digit. While a call is ringing there is nothing
worth deleting and an obvious thing to want instead, so it rejects the call.
These tests pin that precedence and, just as importantly, pin that it does not
leak into the cases where deleting is still the right answer.
"""

import unittest

from clear_action import ClearAction, resolve_clear_action


class RingingTests(unittest.TestCase):
	def test_rejects_incoming_call(self):
		# Why: the feature itself.
		# Failure: DELETE_DIGIT, and the phone keeps ringing while the
		# buffer silently loses a digit the user wanted to keep.
		self.assertEqual(
			resolve_clear_action(ringing=True, display_awake=True),
			ClearAction.REJECT_CALL,
		)

	def test_rejects_from_a_dark_display(self):
		# Why: the panel sleeps while a call rings, so requiring it to be
		# awake would make rejecting take two presses -- the same defect
		# that made answering take two. The user is responding to a phone
		# that is audibly ringing, so the press cannot be accidental in the
		# way a dial press can.
		# Failure: WAKE_ONLY, and the caller keeps ringing.
		self.assertEqual(
			resolve_clear_action(ringing=True, display_awake=False),
			ClearAction.REJECT_CALL,
		)

	def test_reject_outranks_the_prepend_guard(self):
		# Why: the prepend guard refuses deletions that would eat into a
		# dialling prefix. That guard is about editing a number and must
		# not suppress a rejection.
		# Failure: REFUSE_DELETE, so C does nothing at all on a ringing
		# phone whenever a local prefix is in the buffer.
		self.assertEqual(
			resolve_clear_action(
				ringing=True, display_awake=True, in_local_prepend=True
			),
			ClearAction.REJECT_CALL,
		)


class NotRingingTests(unittest.TestCase):
	def test_deletes_a_digit_when_awake(self):
		# Why: the button's original job must survive the change.
		# Failure: the dial buffer becomes uneditable.
		self.assertEqual(
			resolve_clear_action(ringing=False, display_awake=True),
			ClearAction.DELETE_DIGIT,
		)

	def test_wakes_only_when_display_asleep(self):
		# Why: a press on a dark screen should reveal the number before
		# altering it, otherwise a digit vanishes unseen.
		# Failure: a digit is deleted the user never saw.
		self.assertEqual(
			resolve_clear_action(ringing=False, display_awake=False),
			ClearAction.WAKE_ONLY,
		)

	def test_refuses_to_delete_into_the_prepend(self):
		# Why: the dialling prefix is not user-entered and deleting into it
		# produces a number that cannot be dialled.
		# Failure: the prefix erodes one press at a time.
		self.assertEqual(
			resolve_clear_action(
				ringing=False, display_awake=True, in_local_prepend=True
			),
			ClearAction.REFUSE_DELETE,
		)

	def test_asleep_outranks_the_prepend_guard(self):
		# Why: waking is about what the user can see and applies before any
		# decision about what to edit.
		# Failure: REFUSE_DELETE leaves the screen dark with no feedback.
		self.assertEqual(
			resolve_clear_action(
				ringing=False, display_awake=False, in_local_prepend=True
			),
			ClearAction.WAKE_ONLY,
		)


class ExhaustivenessTests(unittest.TestCase):
	def test_every_input_combination_resolves(self):
		# Why: the resolver must be total. A combination falling through to
		# None makes the button dead in a state nobody enumerated.
		# Failure: AssertionError naming the unhandled combination.
		for ringing in (True, False):
			for awake in (True, False):
				for prepend in (True, False):
					with self.subTest(
						ringing=ringing, awake=awake, prepend=prepend
					):
						action = resolve_clear_action(
							ringing=ringing,
							display_awake=awake,
							in_local_prepend=prepend,
						)
						self.assertIsInstance(action, ClearAction)


if __name__ == "__main__":
	unittest.main()
