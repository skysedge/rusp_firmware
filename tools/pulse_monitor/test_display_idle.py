"""Tests for OLED idle blanking and C-button delete gating.

Why: after 20s with no input the panel must sleep; C must wake without
deleting when asleep; C deletes only when already awake.

Failure modes when regressions occur:
- Sleep never triggers → display stays on forever.
- Sleep while already asleep → redundant off commands / state thrash.
- C deletes while asleep → digits vanish on wake press.
"""

import unittest

from display_idle import clear_should_delete, should_sleep_display


class DisplayIdleTests(unittest.TestCase):
	def test_sleeps_after_20s_idle(self):
		# Guards timeout. Failure: still awake at 20s.
		self.assertTrue(
			should_sleep_display(
				display_awake=True,
				now_ms=20_000,
				last_activity_ms=0,
			)
		)
		self.assertFalse(
			should_sleep_display(
				display_awake=True,
				now_ms=19_999,
				last_activity_ms=0,
			)
		)

	def test_no_sleep_when_already_asleep(self):
		# Guards double-sleep. Failure: treats asleep as needing sleep again.
		self.assertFalse(
			should_sleep_display(
				display_awake=False,
				now_ms=60_000,
				last_activity_ms=0,
			)
		)

	def test_no_sleep_while_charging(self):
		# Why: docked/charging phone should keep the OLED on.
		# Failure: blanks after 20s even on the charger.
		self.assertFalse(
			should_sleep_display(
				display_awake=True,
				now_ms=60_000,
				last_activity_ms=0,
				charging=True,
			)
		)


class ClearButtonTests(unittest.TestCase):
	def test_delete_only_when_awake(self):
		# Why: first C press on a dark display is wake-only.
		# Failure: dial_buf shortened while the user only meant to wake.
		self.assertTrue(clear_should_delete(display_awake=True))
		self.assertFalse(clear_should_delete(display_awake=False))


if __name__ == "__main__":
	unittest.main()
