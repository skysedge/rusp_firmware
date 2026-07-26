"""
Display formatting for dialed numbers on the OLED.

Keeps the dial buffer as raw digits for ATD; only the on-screen string
is grouped as NANP: (234) 567-8901.

Progressive rules (digit count after stripping non-digits):
  1–3  → plain digits
  4–6  → (AAA) BBB…
  7–10 → (AAA) BBB-CCCC
  11 with leading 1 → 1 (AAA) BBB-CCCC
Otherwise return the raw string unchanged (international / odd lengths).
"""

from __future__ import annotations


def _digits_only(raw: str) -> str:
	return "".join(c for c in raw if c.isdigit())


def format_phone_display(raw: str) -> str:
	"""
	Format a dial buffer for OLED display.

	Empty input stays empty. Non-digit characters in raw are ignored when
	deciding the NANP shape; the formatted result is rebuilt from digits.
	"""
	if raw is None or raw == "":
		return ""

	digits = _digits_only(raw)
	if digits == "":
		return raw

	# US country code + NANP
	if len(digits) == 11 and digits[0] == "1":
		rest = format_phone_display(digits[1:])
		return f"1 {rest}" if rest else "1"

	n = len(digits)
	if n <= 3:
		return digits
	if n <= 6:
		return f"({digits[:3]}) {digits[3:]}"
	if n <= 10:
		return f"({digits[:3]}) {digits[3:6]}-{digits[6:]}"

	# Longer than NANP: leave raw so international numbers stay readable.
	return raw
