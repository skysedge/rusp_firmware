"""Tests for call-button release-edge gesture.

Why these tests exist: hold-to-act dialed on press-hold, then a long hold
could cancel immediately after dial. Action must be one-shot on release.

Failure modes when regressions occur:
- Fire while still held → dial then hangup in one press.
- Fire on bounce (< debounce) → spurious dial/hangup.
- Require long hold → tap feels dead.
"""

import unittest

from hook_gesture import HOOK_DEBOUNCE_MS, hook_fires_on_release


class HookGestureTests(unittest.TestCase):
	def test_fires_on_release_after_debounce(self):
		# Guards release-edge action. Failure: no action until 1s hold.
		self.assertTrue(
			hook_fires_on_release(
				hook_pressed=True,
				pin_released=True,
				held_ms=HOOK_DEBOUNCE_MS,
			)
		)

	def test_does_not_fire_while_still_held(self):
		# Guards dial-then-cancel. Failure: action while pin still low.
		self.assertFalse(
			hook_fires_on_release(
				hook_pressed=True,
				pin_released=False,
				held_ms=2000,
			)
		)

	def test_ignores_bounce_shorter_than_debounce(self):
		# Guards contact bounce. Failure: fires at held_ms=0/10.
		self.assertFalse(
			hook_fires_on_release(
				hook_pressed=True,
				pin_released=True,
				held_ms=HOOK_DEBOUNCE_MS - 1,
			)
		)

	def test_ignores_release_when_not_tracking_press(self):
		# Guards spurious HIGH reads. Failure: fires without press.
		self.assertFalse(
			hook_fires_on_release(
				hook_pressed=False,
				pin_released=True,
				held_ms=100,
			)
		)


if __name__ == "__main__":
	unittest.main()
