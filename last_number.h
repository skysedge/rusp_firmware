#ifndef RUSP_LAST_NUMBER_H
#define RUSP_LAST_NUMBER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Last dialled number, held in EEPROM.
 *
 * RAM does not survive the MCU reset that a USB replug causes, so redial has
 * to be non-volatile to be useful at all. Mirrors the record framing in
 * tools/pulse_monitor/last_number.py.
 */

/* Record the number an ATD was issued for. No-op for a null/empty string. */
void last_number_store(const char *number);

/* True when a valid record exists — i.e. redial has something to offer. */
bool last_number_available(void);

/*
 * Copy the stored number into out (NUL-terminated). Returns false and leaves
 * out untouched when no valid record exists or it would not fit.
 */
bool last_number_load(char *out, uint8_t out_len);

#endif
