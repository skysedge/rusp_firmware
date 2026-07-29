#include "pin_scan.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "chg_hist.h"
#include "pins.h"
#include "sd.h"

/* Modem GPIO (also in lara.h — listed here to avoid dual CELL_ON defines). */
#ifndef PIN_SCAN_NET_STAT
#define PIN_SCAN_NET_STAT A5
#define PIN_SCAN_CELL_CTS A3
#define PIN_SCAN_CELL_RTS A4
#define PIN_SCAN_CELL_RESET A1
#define PIN_SCAN_CELL_PWR_DET A2
#endif

#ifndef PIN_SCAN_MS
#define PIN_SCAN_MS 250
#endif

/* Full snapshots on Serial only (throttled). SD gets PINCHG edges. */
#ifndef PIN_SCAN_FULL_MS
#define PIN_SCAN_FULL_MS 5000
#endif

/*
 * Held in flash, not RAM. An array of `const char *name` puts both the
 * pointer table and every name string into .data, which on this part is
 * SRAM. Inline char arrays plus PROGMEM move the whole thing to flash at the
 * cost of padding each name to the longest one.
 *
 * PIN_NAME_LEN must fit the longest name below ("LAMBDA", "NONLOC",
 * "OFFSIG", "EPDRST" are 6) plus a terminator. A name that does not fit is
 * silently truncated by the initialiser, so grow this if you add a longer
 * one.
 */
#define PIN_NAME_LEN 7

struct PinWatch {
	char name[PIN_NAME_LEN];
	uint8_t pin;
};

/*
 * Every named digital pin on the board map. USB and the charge port share
 * one connector — whichever sense line exists should appear here as a edge
 * when the cable is plugged or unplugged.
 *
 * PINCHG edges go to PINS.LOG on SD (and Serial). Full PINS snapshots are
 * Serial-only — 1 Hz SD open/close of every pin stalled the console.
 */
static const PinWatch WATCH[] PROGMEM = {
	{"CHG", CHG_STAT},
	{"ROT", SW_ROTARY},
	{"HALL", SW_HALL},
	{"HOOK", SW_HOOK},
	{"C", SW_C},
	{"ALPHA", SW_ALPHA},
	{"BETA", SW_BETA},
	{"LAMBDA", SW_LAMBDA},
	{"FN", SW_FN},
	{"LOCAL", SW_LOCAL},
	{"ALT", SW_ALT},
	{"NONLOC", SW_NONLOCAL},
	{"OFFSIG", OFFSIGNAL},
	{"NET", PIN_SCAN_NET_STAT},
	{"CTS", PIN_SCAN_CELL_CTS},
	{"RTS", PIN_SCAN_CELL_RTS},
	{"CRST", PIN_SCAN_CELL_RESET},
	{"CPWR", PIN_SCAN_CELL_PWR_DET},
	{"BUSY", EPD_BUSY},
	{"LSTAT", LED_STAT},
	{"LFIL", LED_FILAMENT},
	{"LBELL", LED_BELL},
	{"L1A", LED1A},
	{"L2A", LED2A},
	{"L2R", LED2R},
	{"L3A", LED3A},
	{"L3R", LED3R},
	{"L4A", LED4A},
	{"L4R", LED4R},
	{"L5A", LED5A},
	{"L5R", LED5R},
	{"RP", RINGER_P},
	{"RN", RINGER_N},
	{"RELAY", RELAY_OFF},
	{"LLOE", LL_OE},
	{"E3V3", EN_3V3},
	{"E12V", EN_12V},
	{"EAMP", EN_OUTAMP},
	{"CELL", CELL_ON},
	{"SDCS", CHIPSELECT},
	{"EPDCS", EPD_CS},
	{"EPDDC", EPD_DC},
	{"EPDRST", EPD_RST},
};

static const unsigned WATCH_N = sizeof(WATCH) / sizeof(WATCH[0]);
static uint8_t last_level[sizeof(WATCH) / sizeof(WATCH[0])];
static bool have_last = false;
static unsigned long last_sample_ms = 0;
static unsigned long last_full_ms = 0;

static uint8_t pin_level(uint8_t pin)
{
	return digitalRead(pin) == HIGH ? 1u : 0u;
}

static void pin_scan_emit_serial(const char *line)
{
	if (line == nullptr || line[0] == '\0')
		return;
	Serial.println(line);
}

static void pin_scan_emit_sd(const char *line)
{
	if (line == nullptr || line[0] == '\0')
		return;
	sd_pins_log_append(line);
}

static void pin_scan_log_line(const char *tag, bool to_sd)
{
	/*
	 * Emit compact name=0/1 groups. Each Serial/SD line is self-contained
	 * (re-tag on wrap) so a partial USB read never looks like "OHOOK=".
	 */
	char chunk[96];
	size_t used = 0;
	int n = snprintf_P(chunk, sizeof(chunk), PSTR("t=%lu %s"), (unsigned long)millis(), tag);
	if (n < 0)
		return;
	used = (size_t)n;

	for (unsigned i = 0; i < WATCH_N; i++) {
		char piece[16];
		PinWatch w;
		memcpy_P(&w, &WATCH[i], sizeof(w));
		int pn = snprintf_P(
			piece, sizeof(piece), PSTR(" %s=%u"),
			w.name, (unsigned)pin_level(w.pin)
		);
		if (pn < 0)
			continue;
		if (used + (size_t)pn + 1 >= sizeof(chunk)) {
			pin_scan_emit_serial(chunk);
			if (to_sd)
				pin_scan_emit_sd(chunk);
			n = snprintf_P(
				chunk, sizeof(chunk), PSTR("t=%lu %s"),
				(unsigned long)millis(), tag
			);
			if (n < 0)
				return;
			used = (size_t)n;
		}
		memcpy(chunk + used, piece, (size_t)pn + 1);
		used += (size_t)pn;
	}
	if (used > 0) {
		pin_scan_emit_serial(chunk);
		if (to_sd)
			pin_scan_emit_sd(chunk);
	}
}

void pin_scan_init(void)
{
	have_last = false;
	last_sample_ms = 0;
	last_full_ms = 0;
	/* Serial snapshot only — SD open/close here wedged the card for sd cat. */
	pin_scan_log_line("PINS", false);
	for (unsigned i = 0; i < WATCH_N; i++)
		last_level[i] = pin_level(
			(uint8_t)pgm_read_byte(&WATCH[i].pin)
		);
	have_last = true;
}

void pin_scan_service(unsigned long now_ms)
{
	if (last_sample_ms != 0
	    && (now_ms - last_sample_ms) < PIN_SCAN_MS
	    && now_ms >= last_sample_ms) {
		return;
	}
	last_sample_ms = now_ms;

	bool changed = !have_last;
	char delta[96];
	size_t dused = 0;
	delta[0] = '\0';

	for (unsigned i = 0; i < WATCH_N; i++) {
		PinWatch w;
		memcpy_P(&w, &WATCH[i], sizeof(w));
		uint8_t v = pin_level(w.pin);
		if (have_last && v != last_level[i]) {
			changed = true;
			char piece[24];
			int pn = snprintf_P(
				piece, sizeof(piece), PSTR(" %s:%u>%u"),
				w.name,
				(unsigned)last_level[i], (unsigned)v
			);
			if (pn > 0 && dused + (size_t)pn + 1 < sizeof(delta)) {
				memcpy(delta + dused, piece, (size_t)pn + 1);
				dused += (size_t)pn;
			}
		}
		last_level[i] = v;
	}
	have_last = true;

	if (changed && delta[0] != '\0') {
		char line[120];
		snprintf_P(
			line, sizeof(line), PSTR("t=%lu PINCHG%s"),
			(unsigned long)now_ms, delta
		);
		pin_scan_emit_serial(line);
		/*
		 * SD only for CHG edges — EPD BUSY and other chatter was
		 * open/close-thrashing the card until sd cat hung.
		 */
		if (strstr(delta, " CHG:") != nullptr) {
			/* EEPROM history survives the USB reset that kills Serial. */
			uint8_t chg_now = pin_level(CHG_STAT);
			chg_hist_note_raw(chg_now, now_ms);
			pin_scan_emit_sd(line);
			pin_scan_log_line("PINS", true);
		}
		last_full_ms = now_ms;
		return;
	}

	if (last_full_ms == 0
	    || (now_ms - last_full_ms) >= PIN_SCAN_FULL_MS
	    || now_ms < last_full_ms) {
		last_full_ms = now_ms;
		/* Idle snapshots: Serial only — avoid SD thrash. */
		pin_scan_log_line("PINS", false);
	}
}
