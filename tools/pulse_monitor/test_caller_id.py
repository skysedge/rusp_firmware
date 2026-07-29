"""Tests for caller_id: +CLIP parsing and bottom-line precedence."""

import unittest

from caller_id import (
	CALLER_ID_MAX_LEN,
	CallerLineEvent,
	caller_line_after,
	CALLER_ID_UNKNOWN,
	bottom_line_label,
	bottom_line_number,
	parse_clip_number,
)


class ParseClipTests(unittest.TestCase):
	"""Extracting the caller's number from a +CLIP URC.

	Guards the display against the URC corruption this modem has already
	been observed producing: a throttled UART delivered "RING" as a lone
	"R", so a half-arrived +CLIP must not paint fragments on the panel.
	"""

	def test_parses_national_number(self):
		# Why: the ordinary case, type 129, as sent alongside RING.
		# Failure: no number reaches the bottom line and the caller is
		# shown as anonymous even though the network identified them.
		self.assertEqual(
			parse_clip_number('+CLIP: "2345640362",129,,,,0'),
			"2345640362",
		)

	def test_keeps_leading_plus_on_international(self):
		# Why: type 145 numbers carry a '+' that the display formatter
		# needs to recognise the number as international.
		# Failure: the '+' is stripped and the number reads as national.
		self.assertEqual(
			parse_clip_number('+CLIP: "+12345640362",145,,,,0'),
			"+12345640362",
		)

	def test_tolerates_absent_space_after_colon(self):
		# Why: the space after the colon is not guaranteed by the spec.
		# Failure: every caller ID is dropped on a modem that omits it.
		self.assertEqual(
			parse_clip_number('+CLIP:"2345640362",129'),
			"2345640362",
		)

	def test_withheld_number_yields_none(self):
		# Why: a caller who blocks their ID sends empty quotes. There is
		# no number, and inventing a placeholder would be a lie about who
		# is calling.
		# Failure: an empty string is treated as a number and the bottom
		# line goes blank rather than falling back to the status text.
		self.assertIsNone(parse_clip_number('+CLIP: "",128,,,,1'))

	def test_missing_quotes_yields_none(self):
		# Why: a truncated URC can end before the closing quote.
		# Failure: the rest of the buffer is shown as a phone number.
		self.assertIsNone(parse_clip_number('+CLIP: "234564036'))

	def test_non_dialable_characters_yield_none(self):
		# Why: a garbled URC can put arbitrary bytes inside the quotes.
		# Only the dialable set is a plausible number.
		# Failure: junk such as "23R\x00456" is rendered on the panel.
		self.assertIsNone(parse_clip_number('+CLIP: "23R456",129'))

	def test_overlong_number_yields_none(self):
		# Why: E.164 caps at 15 digits; anything past the buffer is a
		# framing error, not a real caller.
		# Failure: the number is silently truncated and a wrong caller is
		# displayed, which is worse than showing none.
		too_long = "9" * (CALLER_ID_MAX_LEN + 1)
		self.assertIsNone(parse_clip_number(f'+CLIP: "{too_long}",129'))

	def test_max_length_number_is_accepted(self):
		# Why: the boundary itself must fit, not be rejected off-by-one.
		# Failure: legitimate long international numbers are discarded.
		at_limit = "9" * CALLER_ID_MAX_LEN
		self.assertEqual(
			parse_clip_number(f'+CLIP: "{at_limit}",145'),
			at_limit,
		)

	def test_unrelated_urc_yields_none(self):
		# Why: every modem byte passes the matcher; only +CLIP is ours.
		# Failure: another URC's payload is mistaken for a caller.
		self.assertIsNone(parse_clip_number('+UCALLSTAT: 1,4'))

	def test_empty_input_yields_none(self):
		# Why: the null case. A capture closed by a stray CR is empty.
		# Failure: an exception in the URC path, which runs for every
		# byte the modem sends.
		self.assertIsNone(parse_clip_number(""))


class BottomLineTests(unittest.TestCase):
	"""Which number the bottom line shows.

	An incoming call outranks a half-typed dial buffer: the digits are
	still held and reappear once the call clears, whereas the caller's
	number is only knowable while the phone is ringing.
	"""

	def test_active_line_prefers_caller_id_over_dial_buffer(self):
		# Why: the whole point of the feature -- the caller must be
		# visible even if the user was mid-dial when the call arrived.
		# Failure: the bottom line keeps showing the typed digits and the
		# caller is never identified.
		self.assertEqual(
			bottom_line_number(
				caller_line_active=True, caller_id="2345640362", dial_buf="555"
			),
			"2345640362",
		)

	def test_active_line_without_caller_id_offers_no_number(self):
		# Why: a withheld caller has no number to format. The label path
		# supplies the wording instead, so this must not fall through to
		# the dial buffer and label an unrelated call with typed digits.
		# Failure: the user's own half-dialled number is presented as the
		# identity of the incoming caller.
		self.assertIsNone(
			bottom_line_number(
				caller_line_active=True, caller_id=None, dial_buf="555"
			)
		)

	def test_inactive_line_shows_dial_buffer(self):
		# Why: normal dialling must be unaffected by this feature.
		# Failure: digits do not appear as they are dialled.
		self.assertEqual(
			bottom_line_number(
				caller_line_active=False, caller_id=None, dial_buf="5551234"
			),
			"5551234",
		)

	def test_caller_id_is_dropped_once_line_is_released(self):
		# Why: the number is scoped to the call. A caller ID left over
		# from a finished call must not label the next screen -- the same
		# staleness class of bug as the ring-evidence timestamp that
		# stopped the phone alerting.
		# Failure: the bottom line still shows the last caller after the
		# call ends, over top of the user's own dial buffer.
		self.assertEqual(
			bottom_line_number(
				caller_line_active=False, caller_id="2345640362", dial_buf="555"
			),
			"555",
		)

	def test_withheld_caller_is_labelled_unknown(self):
		# Why: a ringing call with no number still needs to say something
		# on the bottom line. The label is a literal word, kept separate
		# from the number path because a phone-number formatter applied
		# to "Unknown" would mangle it into punctuation.
		# Failure: the bottom line is blank, or the word is run through
		# the formatter and rendered as garbage.
		self.assertEqual(
			bottom_line_label(caller_line_active=True, caller_id=None),
			CALLER_ID_UNKNOWN,
		)

	def test_identified_caller_needs_no_label(self):
		# Why: when a number is known it occupies the bottom line, and a
		# label alongside it would overwrite the caller.
		# Failure: "Unknown" is drawn over a caller that was identified.
		self.assertIsNone(
			bottom_line_label(caller_line_active=True, caller_id="2345640362")
		)

	def test_no_label_when_line_is_inactive(self):
		# Why: the label belongs to an incoming call only. Left standing,
		# it would sit over the dial buffer during normal use.
		# Failure: "Unknown" appears on an idle phone.
		self.assertIsNone(
			bottom_line_label(caller_line_active=False, caller_id=None)
		)

	def test_idle_with_empty_buffer_shows_nothing(self):
		# Why: the null case -- nothing dialled, nothing ringing.
		# Failure: an empty string is treated as a number to render, and
		# the bottom band stops falling back to the status text.
		self.assertIsNone(
			bottom_line_number(
				caller_line_active=False, caller_id=None, dial_buf=""
			)
		)


if __name__ == "__main__":
	unittest.main()


class CallerLineLifetimeTests(unittest.TestCase):
	"""How long an incoming call's identity keeps the bottom line.

	The number outlives the call itself so that "Rejected" or "Call ended"
	is shown next to whoever it refers to. Everything that replaces the
	bottom line with something else hands it back.
	"""

	def test_call_ending_keeps_the_caller_on_screen(self):
		# Why: the point of this behaviour -- after rejecting, the number
		# must stay beside the "Rejected" status.
		# Failure: the bottom line reverts to the dial buffer the instant
		# the call tears down and the user never sees who they rejected.
		self.assertTrue(
			caller_line_after(True, CallerLineEvent.CALL_ENDED)
		)

	def test_new_ring_takes_the_line(self):
		# Why: a second call must claim the line from the first.
		# Failure: the previous caller's number labels the new call.
		self.assertTrue(
			caller_line_after(False, CallerLineEvent.RING_STARTED)
		)

	def test_dialling_a_digit_releases_the_line(self):
		# Why: once the user starts dialling, the digits are what they
		# need to see.
		# Failure: dialled digits are invisible because a finished call's
		# number is still occupying the bottom line.
		self.assertFalse(
			caller_line_after(True, CallerLineEvent.DIAL_DIGIT)
		)

	def test_deleting_a_digit_releases_the_line(self):
		# Why: C used as a delete key is the user editing the buffer, so
		# the buffer must become visible.
		# Failure: the buffer is edited invisibly behind a stale caller.
		self.assertFalse(
			caller_line_after(True, CallerLineEvent.CLEAR_DIGIT)
		)

	def test_rejecting_does_not_release_the_line(self):
		# Why: C is overloaded. Pressing it on a ringing call rejects and
		# must keep the number; only its delete role hands the line back.
		# Failure: rejecting clears the very number it was meant to leave
		# on screen -- the exact request this behaviour came from.
		self.assertTrue(
			caller_line_after(True, CallerLineEvent.CALL_REJECTED)
		)

	def test_display_sleeping_releases_the_line(self):
		# Why: a blanked panel ends the episode; waking should show the
		# phone's normal idle state, not a call from some time ago.
		# Failure: yesterday's caller greets the user on wake.
		self.assertFalse(
			caller_line_after(True, CallerLineEvent.DISPLAY_SLEPT)
		)

	def test_placing_a_call_releases_the_line(self):
		# Why: an outbound call has no caller ID, and the number being
		# dialled belongs on the bottom line instead.
		# Failure: dialling out shows the last person who rang in.
		self.assertFalse(
			caller_line_after(True, CallerLineEvent.OUTBOUND_STARTED)
		)

	def test_call_ending_does_not_activate_an_idle_line(self):
		# Why: the null case. An outbound call ending must not hand the
		# bottom line to a caller ID that was never populated.
		# Failure: "Unknown" appears after every outgoing call.
		self.assertFalse(
			caller_line_after(False, CallerLineEvent.CALL_ENDED)
		)
