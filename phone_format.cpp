#include "phone_format.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/**
 * Format a dial buffer for OLED display.
 *
 * Progressive NANP grouping (digit count after stripping non-digits):
 *   1–3  → plain digits
 *   4–6  → (AAA) BBB…
 *   7–10 → (AAA) BBB-CCCC
 *   11 with leading 1 → 1 (AAA) BBB-CCCC
 * Otherwise copy raw unchanged (international / odd lengths).
 *
 * Keeping raw for non-NANP lengths avoids inventing a false local number
 * from an international string.
 */
void format_phone_display(const char *raw, char *out, size_t out_size)
{
	if (out == nullptr || out_size == 0)
		return;
	out[0] = '\0';
	if (raw == nullptr || raw[0] == '\0')
		return;

	char digits[32];
	size_t n = 0;
	for (size_t i = 0; raw[i] != '\0' && n + 1 < sizeof(digits); i++) {
		if (isdigit((unsigned char)raw[i]))
			digits[n++] = raw[i];
	}
	digits[n] = '\0';

	if (n == 0) {
		strncpy(out, raw, out_size - 1);
		out[out_size - 1] = '\0';
		return;
	}

	if (n == 11 && digits[0] == '1') {
		char rest[24];
		format_phone_display(digits + 1, rest, sizeof(rest));
		if (rest[0] == '\0')
			snprintf(out, out_size, "1");
		else
			snprintf(out, out_size, "1 %s", rest);
		return;
	}

	if (n <= 3) {
		snprintf(out, out_size, "%s", digits);
		return;
	}
	if (n <= 6) {
		snprintf(
			out, out_size, "(%c%c%c) %s",
			digits[0], digits[1], digits[2], digits + 3
		);
		return;
	}
	if (n <= 10) {
		char exchange[4];
		char subscriber[8];
		memcpy(exchange, digits + 3, 3);
		exchange[3] = '\0';
		memcpy(subscriber, digits + 6, n - 6);
		subscriber[n - 6] = '\0';
		snprintf(
			out, out_size, "(%c%c%c) %s-%s",
			digits[0], digits[1], digits[2],
			exchange, subscriber
		);
		return;
	}

	strncpy(out, raw, out_size - 1);
	out[out_size - 1] = '\0';
}
