#ifndef RUSP_BATTERY_H
#define RUSP_BATTERY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Single-cell LiPo display range (matches original RUSP Helpers.ino). */
#define LIPO_MAX_MV 4150
#define LIPO_MIN_MV 3500

/* Measure board Vcc in mV via the AVR 1.1V bandgap. */
long battery_read_vcc_mv(void);

/* Map mV → 0..100% using LIPO_MIN/MAX. */
int battery_percent_from_mv(long mv);

/* Read Vcc and return clamped percent. */
int battery_read_percent(void);

/*
 * Cached percent; refreshes at most every refresh_ms (and on first call).
 * Avoids delay(2) ADC settle on every OLED redraw.
 */
int battery_percent_cached(unsigned long now_ms, unsigned long refresh_ms);

/*
 * True while the charger status line indicates an active charge cycle.
 * Polarity: CHG_STAT_ACTIVE_HIGH in battery.cpp (board-dependent).
 * Pin must be INPUT_PULLUP (open-drain). Raw level: battery_chg_stat_raw().
 */
bool battery_is_charging(void);

/* Raw digitalRead(CHG_STAT): 0 = LOW, 1 = HIGH. */
int battery_chg_stat_raw(void);

/* Fill buf with e.g. "[####--]85%" or "+[####--]85%". Returns buf. */
char *battery_format_field(char *buf, size_t buflen, int pct, bool charging);

#endif
