"""Tests for remote hangup detection used to update the OLED.

Why these tests exist: far-end hangup left the screen on CALLING because
NO CARRIER / disconnect was not recognized reliably.

Failure modes when regressions occur:
- Ignoring +UCALLSTAT status 6 → UI never clears after VoLTE disconnect.
- Treating CPAS ready before ever seeing in-call → false CALL ENDED at dial.
- Requiring only NO CARRIER → misses modems that only emit UCALLSTAT.
- Requiring a RING URC to judge ring liveness → the ringer never stops on a
  network that announces incoming calls with +UCALLSTAT alone.
"""

import unittest

from call_end_detect import (
	CLCC_ABSENT_LIMIT,
	RING_EVIDENCE_TIMEOUT_MS,
	RING_MAX_MS,
	clcc_absent_ends_call,
	cpas_implies_remote_end,
	is_incoming_ring_evidence,
	ring_evidence_expired,
	ucallstat_is_disconnected,
)

# Ring cadence measured on the live modem (see the t=847s..870s capture and
# the t=88.8s incoming call). RING/UCALLSTAT repeats arrived 5.5-6 s apart,
# not the ~3 s the firmware comment originally assumed.
OBSERVED_RING_INTERVAL_MS = 6000


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


class ClccAbsenceTests(unittest.TestCase):
	"""Clear a call session that never became active.

	Why: a dial the network refused still set outbound_call_active, but
	saw_in_call_cpas stayed False, so cpas_implies_remote_end() could never
	fire. The flag stuck true forever — the UI held "Dialing" and the next
	button press hung up instead of dialing. AT+CLCC reporting no rows is
	the authoritative "there is no call", independent of whether one was
	ever answered.
	"""

	def test_sustained_absence_ends_a_never_active_call(self):
		# Failure: False, and the session can only be cleared by the user
		# pressing the call button twice.
		self.assertTrue(
			clcc_absent_ends_call(
				session_active=True,
				consecutive_absent=CLCC_ABSENT_LIMIT,
			)
		)

	def test_absence_below_the_limit_does_not_end(self):
		# Why: CLCC legitimately reports no rows in the gap between ATD's
		# OK and the call leg appearing. Failure: the call is torn down as
		# it starts, so no number can ever be reached.
		for absent in range(CLCC_ABSENT_LIMIT):
			self.assertFalse(
				clcc_absent_ends_call(
					session_active=True,
					consecutive_absent=absent,
				),
				f"ended after {absent} absent polls",
			)

	def test_absence_is_ignored_outside_a_call_session(self):
		# Why: idle polls must not fabricate a call-ended transition.
		# Failure: spurious "Call ended" on an idle phone.
		self.assertFalse(
			clcc_absent_ends_call(
				session_active=False,
				consecutive_absent=CLCC_ABSENT_LIMIT * 10,
			)
		)

	def test_limit_allows_at_least_two_polls_of_grace(self):
		# Why: a limit of 1 would race call setup at the 1 Hz poll rate.
		# Failure: flaky teardown of calls that were connecting fine.
		self.assertGreaterEqual(CLCC_ABSENT_LIMIT, 3)

	def test_ringing_session_is_reconciled_like_an_outbound_one(self):
		# The captured regression: an incoming call raised the ringer from
		# +UCALLSTAT: 1,4, the cancel URC arrived corrupted as a bare "1,"
		# and was dropped, and nothing polled CLCC because the poll was
		# gated on outbound_call_active. The ringer ran ~24 s past the
		# caller hanging up, stopped only by the 30 s cap.
		# Failure: False, and the only thing that can stop a ring whose
		# end URC was lost is RING_MAX_MS.
		self.assertTrue(
			clcc_absent_ends_call(
				session_active=True,
				consecutive_absent=CLCC_ABSENT_LIMIT,
			)
		)


class IncomingRingEvidenceTests(unittest.TestCase):
	"""What counts as proof that an incoming call is still ringing.

	Why: liveness was tracked off the bare RING URC alone. The live capture
	recorded zero RING URCs for a whole incoming call — the network
	announced it with +UCALLSTAT: 1,4 only — so the liveness timestamp
	stayed 0 and the no-URC timeout, guarded on `last_ring_urc > 0`, was
	disabled for the entire call.
	"""

	def test_bare_ring_urc_is_evidence(self):
		# Failure: the classic RING path stops refreshing liveness and every
		# ring is cancelled one timeout after it starts.
		self.assertTrue(
			is_incoming_ring_evidence(ucall_stat=None, saw_ring_urc=True)
		)

	def test_ucallstat_mt_ringing_is_evidence(self):
		# The regression this module exists for. Failure: liveness is never
		# stamped on a UCALLSTAT-only network, so nothing can time the ring
		# out and it runs to the 30 s cap.
		self.assertTrue(
			is_incoming_ring_evidence(ucall_stat=4, saw_ring_urc=False)
		)

	def test_disconnect_is_not_evidence(self):
		# Why: UCALLSTAT 6 is the opposite of liveness. Failure: the
		# disconnect URC refreshes the timer and holds the ringer on.
		self.assertFalse(
			is_incoming_ring_evidence(ucall_stat=6, saw_ring_urc=False)
		)

	def test_outbound_phases_are_not_incoming_evidence(self):
		# Why: dialling/alerting/active belong to an outbound call. Failure:
		# an outgoing call keeps stamping incoming liveness, so a ring that
		# follows it inherits a bogus fresh timestamp.
		for stat in (0, 2, 3, 7):
			self.assertFalse(
				is_incoming_ring_evidence(
					ucall_stat=stat, saw_ring_urc=False
				),
				f"stat {stat} treated as incoming evidence",
			)

	def test_no_urc_at_all_is_not_evidence(self):
		# The null case: a drain that delivered nothing must not refresh
		# liveness. Failure: the ring never times out on an idle stream.
		self.assertFalse(
			is_incoming_ring_evidence(ucall_stat=None, saw_ring_urc=False)
		)


	def test_call_waiting_counts_as_incoming_ring_evidence(self):
		# Why: with a call already up, a second incoming call is announced
		# as +UCALLSTAT: 1,5 (waiting) and never as 1,4 or a RING URC. A
		# live capture showed exactly this when the user called back during
		# a still-active outbound call.
		# Failure: returns False, so `ringing` is never set and the bell
		# and filament LEDs do not flash for the returning call.
		self.assertTrue(
			is_incoming_ring_evidence(ucall_stat=5, saw_ring_urc=False)
		)

	def test_outgoing_alerting_is_not_ring_evidence(self):
		# Why: state 3 is the remote phone ringing during our own outgoing
		# call. Pairs with the case above to pin which states alert us.
		# Failure: the phone rings at itself while placing a call.
		self.assertFalse(
			is_incoming_ring_evidence(ucall_stat=3, saw_ring_urc=False)
		)


class RingEvidenceExpiryTests(unittest.TestCase):
	"""When silence means the caller is gone.

	Why: the threshold was 5000 ms against a comment claiming RING repeats
	every ~3 s. Measured cadence is 5.5-6 s, so the timeout fired between
	every pair of rings, cancelling and re-raising the ringer once per
	cycle and resetting the OLED to "Ready" each time.
	"""

	def test_one_cadence_of_silence_is_not_expiry(self):
		# Directly reproduces the observed stutter. Failure: True, and the
		# ringer drops out for ~0.5-1 s before every single ring.
		self.assertFalse(
			ring_evidence_expired(
				now_ms=100_000 + OBSERVED_RING_INTERVAL_MS,
				last_evidence_ms=100_000,
			)
		)

	def test_tolerates_exactly_one_dropped_urc(self):
		# Why: URCs are demonstrably lost on this hardware (the capture has
		# "AT: 1,3" and " 1,7" with the +UCALLSTAT prefix eaten). Losing one
		# repeat must not end a live ring. Failure: a single dropped URC
		# silences a call that is still ringing.
		self.assertFalse(
			ring_evidence_expired(
				now_ms=100_000 + 2 * OBSERVED_RING_INTERVAL_MS - 1,
				last_evidence_ms=100_000,
			)
		)

	def test_two_missed_cadences_expire(self):
		# Why: the timeout must still do its job. Failure: silence never
		# ends the ring and only the 30 s cap stops it.
		self.assertTrue(
			ring_evidence_expired(
				now_ms=100_000 + RING_EVIDENCE_TIMEOUT_MS + 1,
				last_evidence_ms=100_000,
			)
		)

	def test_unstamped_session_never_expires(self):
		# The zero case. A ring that has not yet stamped evidence has no
		# baseline to measure against; the previous code's `> 0` guard is
		# what silently disabled the whole check. Failure: a ring is
		# cancelled the instant it starts because 0 looks infinitely old.
		self.assertFalse(
			ring_evidence_expired(now_ms=500_000, last_evidence_ms=0)
		)

	def test_stale_timestamp_from_a_prior_call_is_cleared_not_reused(self):
		# Why: last_ring_urc was never reset at session end, so a new call
		# raised by UCALLSTAT 4 was measured against a timestamp minutes
		# old and cancelled in the same loop iteration that raised it.
		# call_session_end() must zero it; zero must then mean "no
		# baseline". Failure: the second and later incoming calls never
		# alert.
		cleared_at_session_end = 0
		self.assertFalse(
			ring_evidence_expired(
				now_ms=900_000,
				last_evidence_ms=cleared_at_session_end,
			)
		)

	def test_millis_wraparound_does_not_expire_a_live_ring(self):
		# Why: millis() wraps at ~49.7 days and the firmware stores raw
		# unsigned longs. Failure: at the wrap boundary a ringing call is
		# torn down because now_ms appears to precede the last evidence.
		self.assertFalse(
			ring_evidence_expired(now_ms=10, last_evidence_ms=4_294_967_000)
		)

	def test_evidence_timeout_stays_below_the_hard_cap(self):
		# Why: RING_MAX_MS is a last-resort backstop, not the mechanism.
		# Failure: the two collapse and the ringer routinely runs the full
		# 30 s past a cancelled call, which is the reported symptom.
		self.assertLess(RING_EVIDENCE_TIMEOUT_MS, RING_MAX_MS)

	def test_evidence_timeout_exceeds_the_measured_cadence(self):
		# Why: this invariant is what the 5000 ms value violated. Failure:
		# any future retune that drops below the real ring interval
		# silently reintroduces the per-ring stutter.
		self.assertGreater(RING_EVIDENCE_TIMEOUT_MS, OBSERVED_RING_INTERVAL_MS)


if __name__ == "__main__":
	unittest.main()
