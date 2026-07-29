#include "last_number.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <string.h>

/* Keep in sync with tools/pulse_monitor/last_number.py */
#define LAST_NUMBER_MAGIC 0x4E
#define LAST_NUMBER_MAX_LEN 29

/*
 * chg_hist owns 16..78 (base 16, 3 header bytes + 12 * 5-byte records).
 * Start clear of it; the ATmega2560 has 4096 bytes of EEPROM.
 */
#define LAST_NUMBER_EE_BASE 80
#define LAST_NUMBER_OFF_MAGIC (LAST_NUMBER_EE_BASE + 0)
#define LAST_NUMBER_OFF_LEN (LAST_NUMBER_EE_BASE + 1)
#define LAST_NUMBER_OFF_CHARS (LAST_NUMBER_EE_BASE + 2)


/*
 * Length of the stored number, or 0 when there is no usable record.
 *
 * Both checks are load-bearing. Erased EEPROM reads as 0xFF everywhere and a
 * zeroed one as 0x00, so without the magic a board that has never placed a
 * call would offer to redial a number made of 0xFF bytes. Without the length
 * bound a corrupt record would read past itself and return whatever follows
 * it in EEPROM.
 */
static uint8_t last_number_stored_len(void)
{
	if (EEPROM.read(LAST_NUMBER_OFF_MAGIC) != LAST_NUMBER_MAGIC)
		return 0;

	uint8_t len = EEPROM.read(LAST_NUMBER_OFF_LEN);
	if (len < 1 || len > LAST_NUMBER_MAX_LEN)
		return 0;

	return len;
}


void last_number_store(const char *number)
{
	if (number == nullptr || number[0] == '\0')
		return;

	uint8_t len = (uint8_t)strnlen(number, LAST_NUMBER_MAX_LEN);
	if (len == 0)
		return;

	/*
	 * update() only touches bytes that changed, so redialling the same
	 * number repeatedly costs no EEPROM write cycles.
	 */
	EEPROM.update(LAST_NUMBER_OFF_MAGIC, LAST_NUMBER_MAGIC);
	EEPROM.update(LAST_NUMBER_OFF_LEN, len);
	for (uint8_t i = 0; i < len; i++)
		EEPROM.update(LAST_NUMBER_OFF_CHARS + i, (uint8_t)number[i]);
}


bool last_number_available(void)
{
	return last_number_stored_len() > 0;
}


bool last_number_load(char *out, uint8_t out_len)
{
	if (out == nullptr || out_len == 0)
		return false;

	uint8_t len = last_number_stored_len();
	if (len == 0 || len > (uint8_t)(out_len - 1))
		return false;

	for (uint8_t i = 0; i < len; i++)
		out[i] = (char)EEPROM.read(LAST_NUMBER_OFF_CHARS + i);
	out[len] = '\0';
	return true;
}
