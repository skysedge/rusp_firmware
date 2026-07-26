#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "pins.h"

/*
 * Read 1.1V reference against AVcc (Scott Daniels / provideyourown.com).
 * Same approach as the original RotaryUnSmartphone Helpers.ino.
 */
long battery_read_vcc_mv(void)
{
	ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
	delay(2);
	ADCSRA |= _BV(ADSC);
	while (bit_is_set(ADCSRA, ADSC)) {
	}
	uint8_t low = ADCL;
	uint8_t high = ADCH;
	long result = (high << 8) | low;
	if (result == 0)
		return 0;
	return 1125300L / result;
}

int battery_percent_from_mv(long mv)
{
	const int span = LIPO_MAX_MV - LIPO_MIN_MV;
	int pct = (int)(((mv - LIPO_MIN_MV) * 100L) / span);
	if (pct > 100)
		pct = 100;
	if (pct < 0)
		pct = 0;
	return pct;
}

int battery_read_percent(void)
{
	return battery_percent_from_mv(battery_read_vcc_mv());
}

int battery_percent_cached(unsigned long now_ms, unsigned long refresh_ms)
{
	static int cached = -1;
	static unsigned long last_ms = 0;
	if (cached < 0
	    || (now_ms - last_ms) >= refresh_ms
	    || now_ms < last_ms) {
		cached = battery_read_percent();
		last_ms = now_ms;
	}
	return cached;
}

/*
 * Hardware (RUST_Hardware Motherboard PCB):
 *   MCP73831 STAT → D2 cathode (Net-D2-K); D2 anode → series R → VBUS.
 *   Amber D2 is driven only by the charger IC — it is NOT tied to an MCU pin.
 *   ATmega PL5 / Arduino D44 (pins.h CHG_STAT) is unconnected on the PCB
 *   (`unconnected-(U4-PL5-Pad40)`). The schematic netlist's `/chg_stat`→U4.40
 *   was never routed.
 *
 * With INPUT_PULLUP on a floating D44, raw stays 1. Active-HIGH then falsely
 * reports "charging" forever. Active-LOW: floating/high = not charging.
 * To sense STAT in software, jumper Net-(D2-K)/STAT to PL5 (D44); MCP73831
 * STAT is open-drain low while charging.
 */
#ifndef CHG_STAT_ACTIVE_HIGH
#define CHG_STAT_ACTIVE_HIGH 0
#endif

int battery_chg_stat_raw(void)
{
	return digitalRead(CHG_STAT) == HIGH ? 1 : 0;
}

bool battery_is_charging(void)
{
	int raw = battery_chg_stat_raw();
#if CHG_STAT_ACTIVE_HIGH
	return raw != 0;
#else
	return raw == 0;
#endif
}

static void format_meter(char *out, int segments, int pct)
{
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	int filled = (pct * segments + 50) / 100;
	if (filled > segments)
		filled = segments;
	for (int i = 0; i < segments; i++)
		out[i] = (i < filled) ? '#' : '-';
	out[segments] = '\0';
}

char *battery_format_field(char *buf, size_t buflen, int pct, bool charging)
{
	if (buf == nullptr || buflen < 8)
		return buf;
	char meter[8];
	format_meter(meter, 6, pct);
	snprintf_P(
		buf, buflen, PSTR("%s[%s]%d%%"),
		charging ? "+" : "", meter, pct
	);
	return buf;
}
