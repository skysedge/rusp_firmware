#ifndef RUSP_PHONE_FORMAT_H
#define RUSP_PHONE_FORMAT_H

#include <stddef.h>

/*
 * Format dial_buf for OLED display as NANP: (234) 567-8901.
 * Mirrored by tools/pulse_monitor/phone_format.py.
 * dial_buf itself stays raw for ATD.
 *
 * Writes into out (NUL-terminated). Always succeeds; if out_size is 0 this
 * is a no-op. Truncates only if out_size cannot hold the result.
 */
void format_phone_display(const char *raw, char *out, size_t out_size);

#endif
