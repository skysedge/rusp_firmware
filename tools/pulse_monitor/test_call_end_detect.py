"""Tests for remote hangup detection used to update the OLED.

Why these tests exist: far-end hangup left the screen on CALLING because
NO CARRIER / disconnect was not recognized reliably.

Failure modes when regressions occur:
- Ignoring +UCALLSTAT status 6 → UI never clears after VoLTE disconnect.
- Treating CPAS ready before ever seeing in-call → false CALL ENDED at dial.
- Requiring only NO CARRIER → misses modems that only emit UCALLSTAT.
"""

import unittest

from call_end_detect import cpas_implies_remote_end, ucallstat_is_disconnected


class UcallstatTests(unittest.TestCase):
	def test_status_6_is_disconnected(self):
		# Guards UCALLSTAT disconnect. Failure: remote hangup ignored.
		self.assertTrue(ucallstat_is_disconnected("1,6"))
		self.assertTrue(ucallstat_is_disconnected("1,6,1"))

	def test_active_and_dialling_not_disconnected(self):
		# Guards false end during call setup. Failure: CALL ENDED while ringing.
		self.assertFalse(ucallstat_is_disconnected("1,0"))
		self.assertFalse(ucallstat_is_disconnected("1,2"))
		self.assertFalse(ucallstat_is_disconnected("1,3"))


class CpasTransitionTests(unittest.TestCase):
	def test_ready_after_in_call_ends_outbound(self):
		# Guards CPAS poll backup. Failure: OLED stays CALLING after hangup.
		self.assertTrue(
			cpas_implies_remote_end(
				outbound_active=True,
				saw_in_call_cpas=True,
				cpas="0",
			)
		)

	def test_ready_before_in_call_does_not_end(self):
		# Guards sticky CPAS=0 after ATD. Failure: ends call immediately.
		self.assertFalse(
			cpas_implies_remote_end(
				outbound_active=True,
				saw_in_call_cpas=False,
				cpas="0",
			)
		)


if __name__ == "__main__":
	unittest.main()
