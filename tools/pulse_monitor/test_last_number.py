"""Tests for the persisted last-dialled number (redial).

Why this exists: pressing the call button with an empty buffer should recall
the previous number instead of refusing. The record lives in EEPROM, so it
survives the USB reset that wipes everything else, and it is read back on a
board whose EEPROM may never have held one.

Failure modes when regressions occur:
- Missing magic check → uninitialised EEPROM (all 0xFF) decodes as a number
  made of 0xFF bytes and the phone dials garbage.
- Missing length validation → a corrupt length reads past the record and
  returns adjacent EEPROM contents as a phone number.
- Storing an empty string → the recall path thinks a number exists and
  loads nothing, leaving the user with no feedback at all.
"""

import unittest

from last_number import (
	LAST_NUMBER_MAGIC,
	LAST_NUMBER_MAX_LEN,
	decode_record,
	encode_record,
)

# Mirrors DIAL_BUF_LEN - 1 in rusp_firmware.ino. Anything longer cannot fit
# the dial buffer it is loaded back into.
DIAL_BUF_USABLE = 29

# Uninitialised AVR EEPROM reads as 0xFF in every byte.
ERASED_EEPROM = [0xFF] * 40


class EncodeTests(unittest.TestCase):
	def test_round_trip_preserves_the_number(self):
		# The core behaviour. Failure: redial dials something other than
		# what was dialled before.
		self.assertEqual(decode_record(encode_record("6315551234")), "6315551234")

	def test_record_starts_with_the_magic_byte(self):
		# Why: the magic is the only thing distinguishing a real record
		# from erased EEPROM. Failure: garbage is treated as a number.
		self.assertEqual(encode_record("5551234")[0], LAST_NUMBER_MAGIC)

	def test_record_carries_its_own_length(self):
		# Why: decode must not rely on a terminator that erased EEPROM
		# would never contain. Failure: reads run past the record.
		record = encode_record("5551234")
		self.assertEqual(record[1], len("5551234"))

	def test_empty_number_is_not_stored(self):
		# The null case. Failure: an empty record is written, recall
		# reports a number exists, and the buffer is loaded with nothing.
		self.assertIsNone(encode_record(""))

	def test_none_is_not_stored(self):
		# Why: the dial buffer can be empty at the call site. Failure:
		# a crash or an empty record written over a good one.
		self.assertIsNone(encode_record(None))

	def test_number_longer_than_the_dial_buffer_is_truncated(self):
		# Why: the recalled value is copied straight back into
		# dial_buf[DIAL_BUF_LEN]. Failure: buffer overrun on load.
		long_number = "9" * (DIAL_BUF_USABLE + 15)
		record = encode_record(long_number)
		self.assertEqual(record[1], LAST_NUMBER_MAX_LEN)
		self.assertEqual(len(decode_record(record)), LAST_NUMBER_MAX_LEN)

	def test_max_length_matches_the_dial_buffer(self):
		# Guards the two constants drifting apart. Failure: either the
		# stored number is silently clipped shorter than it need be, or
		# it no longer fits the buffer it is loaded into.
		self.assertEqual(LAST_NUMBER_MAX_LEN, DIAL_BUF_USABLE)

	def test_non_digit_dial_characters_survive(self):
		# Why: numbers legitimately carry '+' for international dialling
		# and the prepend digits added in LOCAL call mode. Failure:
		# redialling an international number drops the '+' and fails.
		self.assertEqual(decode_record(encode_record("+441632960")), "+441632960")


class DecodeTests(unittest.TestCase):
	def test_erased_eeprom_has_no_number(self):
		# The case every board hits before its first call. Failure: the
		# phone offers to redial 0xFF bytes.
		self.assertIsNone(decode_record(ERASED_EEPROM))

	def test_all_zero_eeprom_has_no_number(self):
		# The other common uninitialised pattern. Failure: recall loads
		# an empty or NUL-filled number.
		self.assertIsNone(decode_record([0x00] * 40))

	def test_wrong_magic_is_rejected(self):
		# Why: another feature's data must never be read as a number.
		# Failure: unrelated EEPROM contents get dialled.
		record = encode_record("5551234")
		record[0] = LAST_NUMBER_MAGIC ^ 0xFF
		self.assertIsNone(decode_record(record))

	def test_zero_length_is_rejected(self):
		# Why: a length of 0 is not a number, it is a corrupt record.
		# Failure: recall claims success and loads an empty buffer.
		self.assertIsNone(decode_record([LAST_NUMBER_MAGIC, 0]))

	def test_length_beyond_the_maximum_is_rejected(self):
		# Why: a corrupt length must not authorise a longer read than the
		# dial buffer can hold. Failure: buffer overrun.
		corrupt = [LAST_NUMBER_MAGIC, LAST_NUMBER_MAX_LEN + 1] + [0x39] * 40
		self.assertIsNone(decode_record(corrupt))

	def test_length_longer_than_available_data_is_rejected(self):
		# Why: a truncated record must not return adjacent EEPROM bytes.
		# Failure: whatever follows the record is dialled.
		self.assertIsNone(decode_record([LAST_NUMBER_MAGIC, 10, 0x35, 0x35]))

	def test_exact_length_record_decodes(self):
		# Boundary: a record filling the buffer exactly must still be
		# valid. Failure: an off-by-one rejects the longest legal number.
		digits = "8" * LAST_NUMBER_MAX_LEN
		self.assertEqual(decode_record(encode_record(digits)), digits)


if __name__ == "__main__":
	unittest.main()
