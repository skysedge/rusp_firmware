"""
OLED phone-number display formatting.

Why: callers see (234) 567-8901 while dial_buf stays raw for ATD.
Failure modes: wrong grouping, formatting non-NANP lengths, mutating intent
of partial entry (showing parens too early / too late).
"""

import unittest

from phone_format import format_phone_display


class PhoneFormatTests(unittest.TestCase):
	def test_empty_and_partial(self):
		# Why: early digits must stay plain until an area code is complete.
		# Failure: "12" becomes "(12" and jumps around while dialing.
		self.assertEqual(format_phone_display(""), "")
		self.assertEqual(format_phone_display("2"), "2")
		self.assertEqual(format_phone_display("23"), "23")
		self.assertEqual(format_phone_display("234"), "234")

	def test_area_code_then_exchange(self):
		# Why: at 4+ digits show (AAA) prefix; exchange grows before the dash.
		# Failure: 4th digit appears outside parens or dash appears too soon.
		self.assertEqual(format_phone_display("2345"), "(234) 5")
		self.assertEqual(format_phone_display("234567"), "(234) 567")

	def test_full_nanp(self):
		# Why: canonical 10-digit display the UI should show.
		# Failure: missing dash, wrong grouping, or truncated subscriber.
		self.assertEqual(
			format_phone_display("2345678901"),
			"(234) 567-8901",
		)
		self.assertEqual(
			format_phone_display("2345678"),
			"(234) 567-8",
		)

	def test_leading_one(self):
		# Why: optional US trunk/country digit stays outside the parens.
		# Failure: leading 1 absorbed into area code → (123) 456-7890.
		self.assertEqual(
			format_phone_display("12345678901"),
			"1 (234) 567-8901",
		)

	def test_non_nanp_left_raw(self):
		# Why: international / odd lengths must not be force-grouped as NANP.
		# Failure: 12-digit string mangled into bogus (xxx) groups.
		self.assertEqual(
			format_phone_display("441234567890"),
			"441234567890",
		)

	def test_strips_existing_punctuation_for_shape(self):
		# Why: if dial_buf somehow has separators, digit shape still wins.
		# Failure: double-formatting or empty result.
		self.assertEqual(
			format_phone_display("(234)567-8901"),
			"(234) 567-8901",
		)


if __name__ == "__main__":
	unittest.main()
