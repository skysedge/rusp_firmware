"""Persisted last-dialled number, for redial from an empty dial buffer.

Mirror of last_number.cpp. Kept pure (operates on a byte list) so the record
framing can be tested without EEPROM; the firmware supplies the actual reads
and writes.

The record is deliberately self-describing rather than NUL-terminated.
Erased AVR EEPROM reads as 0xFF in every byte and a freshly zeroed one reads
as 0x00, so a decoder that trusts the stored bytes would hand back a "phone
number" on a board that has never placed a call. The magic byte says a record
was written, and the length byte bounds the read so a corrupt record cannot
return whatever happens to sit after it in EEPROM.
"""

from __future__ import annotations

# Arbitrary but fixed; distinguishes a written record from erased EEPROM.
LAST_NUMBER_MAGIC = 0x4E

# Mirrors DIAL_BUF_LEN - 1 in rusp_firmware.ino. The recalled value is copied
# straight back into dial_buf, so it can never be allowed to exceed it.
LAST_NUMBER_MAX_LEN = 29


def encode_record(number: str | None) -> list[int] | None:
	"""Bytes to write, or None when there is nothing worth storing.

	Returning None for an empty number matters: writing a zero-length
	record would make the recall path report that a number exists and then
	load nothing, which reads to the user as the button doing nothing.
	"""
	if not number:
		return None
	text = number[:LAST_NUMBER_MAX_LEN]
	return [LAST_NUMBER_MAGIC, len(text)] + [ord(c) for c in text]


def decode_record(data: list[int]) -> str | None:
	"""Number held in these EEPROM bytes, or None if absent or corrupt."""
	if len(data) < 2:
		return None
	if data[0] != LAST_NUMBER_MAGIC:
		return None
	length = data[1]
	if length < 1 or length > LAST_NUMBER_MAX_LEN:
		return None
	if length > len(data) - 2:
		return None
	return "".join(chr(b) for b in data[2 : 2 + length])
