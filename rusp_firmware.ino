#include <EnableInterrupt.h>
#include <GxEPD2_BW.h>
#include <SPI.h>

// Firmware version
#define FIRMWARE_VERSION "1.2.5"

/*
 * When 1: ignore local / address-book (ALT) switch for dialing.
 * Digits always append to dial_buf; call button dials dial_buf as typed.
 */
#ifndef DISABLE_CALL_TYPE_MODES
#define DISABLE_CALL_TYPE_MODES 1
#endif
/* When 1: never blank the OLED on idle/status timeouts or mode bumps. */
#ifndef KEEP_OLED_DISPLAY
#define KEEP_OLED_DISPLAY 1
#endif
/* Blank the panel (SSD1362 OFF) after this much input idle time. */
#ifndef OLED_IDLE_TIMEOUT_MS
#define OLED_IDLE_TIMEOUT_MS 20000
#endif

// Define placement new for Arduino (if not already defined)
inline void* operator new(size_t, void* ptr) { return ptr; }

#include <stdarg.h>

#include <string.h>

#include "pins.h"
#include "lara.h"
#include "oled.h"
#include "sd.h"
#include "epd.h"
#include "call_mode.h"
#include "battery.h"
#include "phone_format.h"
#include "pin_scan.h"
#include "chg_hist.h"
#include "last_number.h"

// External declarations for contact data (from epd_contact.cpp)
extern char CName[30];
extern int CNumber[30];
extern int kc;
extern int pg;

// length of dial buffer (max 255 right now since dial_idx is unsigned char)
#define DIAL_BUF_LEN 30
// subtracted from pulse count to get number dialed
#define PULSE_FUDGE 1
// ms to debounce rotary switch by
#define ROTARY_DEBOUNCE_MS 30
// Quiet after last edge before considering a digit commit.
#define PULSES_DONE_MS 200
// If no regular return suffix yet, wait this long before best-effort commit
// (avoids treating a wind pause as end-of-digit).
#define FALLBACK_COMMIT_MS 500
// How many recent rotary falling edges to retain for digit extract.
#define ROTARY_EDGE_HISTORY 48
// Valid digits need >= 2 raw pulses after debounce (PULSE_FUDGE → '1'..'0').
#define MIN_PULSES_FOR_DIGIT 2
// Max raw pulses for one digit ('0' = 11).
#define MAX_PULSES_FOR_DIGIT 11
// Max |new_gap - mean_return_gap| as a percentage of mean (relative cadence).
#define MAX_REL_DEV_PCT 45
// Hall is logged only; not used for digit decisions. Debounce log spam.
#define HALL_LOG_DEBOUNCE_MS 100

// https://en.wikipedia.org/wiki/X_macro
#define FOR_CONTACTS(DO) \
	DO('1', CONTACT1) \
	DO('2', CONTACT2) \
	DO('3', CONTACT3) \
	DO('4', CONTACT4) \
	DO('5', CONTACT5) \
	DO('6', CONTACT6) \
	DO('7', CONTACT7) \
	DO('8', CONTACT8) \
	DO('9', CONTACT9) \
	DO('0', CONTACT0)

// ePaper display object (using static storage, not heap)
// We use a static buffer and placement new to avoid dynamic allocation issues
GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)> *eink = nullptr;

// ugly global variables
//
// Dial model (pulse-only; hall ignored for digits):
// - Every rotary falling edge is timestamped.
// - Wind (finger) has irregular gaps; return (spring) is more uniform.
// - Digit = trailing suffix grown from the end while each earlier gap stays
//   within MAX_REL_DEV_PCT of the mean return gap (relative, not absolute).
// - Quiet PULSES_DONE_MS commits when a regular return is found; otherwise
//   wait FALLBACK_COMMIT_MS then best-effort.
// - Completed digits go on a queue; UI/modem work never disables IRQs.
#define DIAL_DIGIT_QUEUE_LEN 16
#define DIAL_DBG_QUEUE_LEN 64

typedef struct {
	char pulse_count;
	bool hook_held;
} DialDigitEvent;

/* Dial debug events (ISR-safe queue → flushed to SD DIAL.LOG in loop). */
enum DialDbgType : uint8_t {
	DDBG_HALL_SEEN = 1, /* telemetry only — not used for digits */
	DDBG_COMMIT_REG = 5,
	DDBG_COMMIT_FALLBACK = 6,
	DDBG_DIGIT = 8,
	DDBG_QUEUE_DROP = 9,
	DDBG_DISCARD = 10,
};

typedef struct {
	uint32_t t_ms;
	uint8_t type;
	uint8_t pulses;
	char ascii;
	uint16_t dt_ms;
	uint8_t hall_level; /* 0=LOW active, 1=HIGH */
} DialDbgEvent;

volatile DialDbgEvent dial_dbg_q[DIAL_DBG_QUEUE_LEN];
volatile uint8_t dial_dbg_head = 0;
volatile uint8_t dial_dbg_tail = 0;
volatile uint16_t dial_dbg_dropped = 0;

volatile DialDigitEvent dial_digit_q[DIAL_DIGIT_QUEUE_LEN];
volatile uint8_t dial_digit_head = 0;
volatile uint8_t dial_digit_tail = 0;
volatile uint16_t dial_digit_dropped = 0;

// Ring of recent rotary edge timestamps (millis), always recorded.
volatile unsigned long rotary_edge_ms[ROTARY_EDGE_HISTORY];
volatile uint8_t rotary_edge_head = 0;
volatile uint8_t rotary_edge_count = 0;

// Edges with t > last_commit_ms belong to the current/next dial activity.
volatile unsigned long last_commit_ms = 0;
// Hall log debounce only (hall does not affect digit extraction).
volatile unsigned long hall_last_ms = 0;
// True while uncommitted rotary activity is in progress (LED effects).
volatile bool pulsing = false;
// Hook sampled once per activity (first rotary edge).
volatile bool hook_during_dial = false;
volatile bool hook_sampled = false;

// buffer to store the phone number (or other thing) being dialed
char dial_buf[DIAL_BUF_LEN];
// index of the dial string we're on
unsigned char dial_idx = 0;
// whether or not the hook was recently pressed
// TODO: i tried just putting all the hook-handling code in the ISR but somehow
// the chip got mad at me
bool hook = false;
// for debouncing
unsigned long hook_last = 0;
// for tracking hook hold time
bool hook_pressed = false;
unsigned long hook_press_start = 0;
/* Ignore press/release pairs shorter than this (contact bounce). Action runs
 * on release — not on hold duration — so one press cannot dial then cancel. */
#define HOOK_DEBOUNCE_MS 30
// are we ringing the bell
bool ringing = false;
/*
 * True after a successful outbound ATD until hangup / NO CARRIER.
 * Needed because the modem can still report CPAS ready (0) while a call
 * is up — trusting only CPAS==4 re-dials and leaves OLED on DIALING.
 */
bool outbound_call_active = false;
/* Set when CPAS reports in-call (4) during an outbound session. */
bool saw_in_call_cpas = false;
unsigned long last_call_cpas_poll_ms = 0;
#define CALL_CPAS_POLL_MS 1000
/*
 * Consecutive AT+CLCC polls reporting no calls before the session is
 * declared over. Mirrors tools/pulse_monitor/call_end_detect.py.
 * Needed because a refused dial never reaches an active state, so
 * saw_in_call_cpas stays false and nothing else can clear the session.
 */
#define CLCC_ABSENT_LIMIT 3
uint8_t clcc_absent_polls = 0;
// when did ringing start
unsigned long ringing_start = 0;
/*
 * When the modem last proved an incoming call is still ringing, from a RING
 * URC *or* +UCALLSTAT: 1,4. Mirrors is_incoming_ring_evidence() in
 * tools/pulse_monitor/call_end_detect.py.
 *
 * Not RING-only: a live capture recorded a whole incoming call announced by
 * +UCALLSTAT: 1,4 with zero RING URCs, which left this at 0 and silently
 * disabled the expiry check below that is guarded on it.
 *
 * 0 means "no baseline yet". call_session_end() must restore that, or a
 * timestamp left over from an earlier call makes the next ring look
 * instantly stale and the phone stops alerting entirely.
 */
unsigned long last_ring_evidence_ms = 0;
/*
 * Silence that means the caller is gone. Two measured ring cadences (5.5-6 s
 * on this modem), so one dropped URC cannot end a live call — URCs do arrive
 * corrupted here. The previous 5000 ms sat below the real cadence and fired
 * between every pair of rings.
 */
#define RING_EVIDENCE_TIMEOUT_MS 12000
/* Hard cap per ring. Backstop only; reaching it means the above failed. */
#define RING_MAX_MS 30000
// Last effective / physical call-type modes (for change detection + logs).
CallMode prev_mode = CALL_MODE_NONLOCAL;
CallMode prev_phys_mode = CALL_MODE_NONLOCAL;
// OLED status message display
String oled_status_message = "";
unsigned long last_status_message_time = 0;
#define STATUS_MESSAGE_TIMEOUT 2000  // Clear status after 2 seconds
/* Top-row call status (line 1 with battery); dial_buf is line 2. */
char ui_status[24] = "Ready";
#define BATT_UI_REFRESH_MS 5000
/* Poll CHG_STAT often so plug/unplug updates the bolt without waiting 5s. */
#define CHG_UI_POLL_MS 250
#define SIGNAL_UI_REFRESH_MS 10000
volatile bool ui_dirty = false;
/* C button: handled in loop so display-asleep presses can wake without delete. */
volatile bool clear_request = false;
bool incoming_ui_shown = false;
unsigned long last_ui_meter_ms = 0;
/* Last drawn chrome — skip full clear/redraw when nothing changed (anti-flicker). */
static int ui_last_batt_pct = -1;
static int ui_last_signal_bars = -1;
static bool ui_last_charging = false;
static char ui_last_status[24];
static char ui_last_number[DIAL_BUF_LEN];
static bool ui_have_last = false;
static int ui_signal_bars_cached = 0;
static unsigned long ui_signal_last_ms = 0;
/* SSD1362 on/off idle blanking (mirrors tools/pulse_monitor/display_idle.py). */
static bool display_awake = true;
static unsigned long last_ui_activity_ms = 0;
static unsigned long last_chg_poll_ms = 0;
static bool last_chg_charging = false;
// LED effects for rotary dial
unsigned long last_led_toggle = 0;
bool led_effects_state = false;
#define LED_PULSE_INTERVAL 50  // Toggle LEDs every 50ms for visible pulsing
// Visual ring indicator (FILAMENT LED)
unsigned long ring_pattern_start = 0;
bool filament_led_state = false;
unsigned long last_filament_toggle = 0;
#define FILAMENT_PULSE_INTERVAL 50  // Fast pulse during ring (20 Hz)
// Ring pattern timing (milliseconds)
#define RING_ON_DURATION 400        // First "ring" - ON period
#define RING_SHORT_PAUSE 200        // Short pause between rings
#define RING_ON_DURATION2 400       // Second "ring" - ON period
#define RING_LONG_PAUSE 2000        // Long pause before repeat
#define RING_PATTERN_TOTAL (RING_ON_DURATION + RING_SHORT_PAUSE + RING_ON_DURATION2 + RING_LONG_PAUSE)


char pulse2ascii(char pulse_count);

/**
 * Log a call/dial troubleshooting line to Serial and DIAL.LOG.
 *
 * The message and format strings live in flash, not RAM. On AVR a plain
 * string literal is copied into .data at startup and occupies SRAM for the
 * life of the program; with ~40 log sites that added up to well over a
 * kilobyte of the 8 KB total. The _P variants read from flash instead, and
 * the call_log / call_logf macros below wrap the literal in PSTR() so call
 * sites keep their ordinary shape.
 *
 * Consequence to respect: these take flash pointers. Passing a RAM string
 * (a char buffer, a runtime-built message) reads from the wrong address
 * space and prints garbage. Use call_log_ram() for those.
 */
static void call_log_ram(const char *msg)
{
	if (msg == nullptr)
		return;
	Serial.println(msg);
	sd_log_append(msg);
}

static void call_log_P(const char *msg)
{
	if (msg == nullptr)
		return;
	char line[96];
	strncpy_P(line, msg, sizeof(line) - 1);
	line[sizeof(line) - 1] = '\0';
	call_log_ram(line);
}

static void call_logf_P(const char *fmt, ...)
{
	char line[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf_P(line, sizeof(line), fmt, ap);
	va_end(ap);
	call_log_ram(line);
}

#define call_log(msg) call_log_P(PSTR(msg))
#define call_logf(fmt, ...) call_logf_P(PSTR(fmt), __VA_ARGS__)


/**
 * Clear all state for the current call session.
 *
 * Every teardown path must clear the same fields, including the CLCC
 * absence counter — a count left over from the previous call would make the
 * next session declare itself over early.
 */
static void call_session_end(const char *reason)
{
	outbound_call_active = false;
	saw_in_call_cpas = false;
	ringing = false;
	incoming_ui_shown = false;
	clcc_absent_polls = 0;
	last_ring_evidence_ms = 0;
	call_logf(
		"t=%lu CALL_SESSION_END %s",
		(unsigned long)millis(), reason ? reason : "?"
	);
}


/**
 * Start tracking a call just placed or answered. Only called after the modem
 * has acknowledged ATD/ATA, so a refused call never opens a session.
 */
static void call_session_begin(const char *reason)
{
	outbound_call_active = true;
	saw_in_call_cpas = false;
	ringing = false;
	incoming_ui_shown = false;
	clcc_absent_polls = 0;
	last_ring_evidence_ms = 0;
	last_call_cpas_poll_ms = millis();
	call_logf(
		"t=%lu CALL_SESSION_BEGIN %s",
		(unsigned long)millis(), reason ? reason : "?"
	);
}


static int ui_signal_bars_now(unsigned long now_ms)
{
	if (ui_signal_last_ms == 0
	    || (now_ms - ui_signal_last_ms) >= SIGNAL_UI_REFRESH_MS
	    || now_ms < ui_signal_last_ms) {
		int rssi = lara_signal_rssi();
		ui_signal_bars_cached = lara_signal_bars(rssi);
		ui_signal_last_ms = now_ms;
	}
	return ui_signal_bars_cached;
}

static void ui_wake(void);
static void ui_note_activity(void);

/**
 * Redraw post-boot UI (signal left, battery right, bottom message/number).
 * Skips redraw when content is unchanged — full oled_clear() is what
 * caused visible flicker on the periodic battery timer.
 * While the panel is asleep, skip SPI draws (wake forces a full refresh).
 */
static void ui_refresh(void)
{
	if (!display_awake)
		return;

	unsigned long now = millis();
	int pct = battery_percent_cached(now, BATT_UI_REFRESH_MS);
	bool charging = battery_is_charging();
	int bars = ui_signal_bars_now(now);
	const char *raw =
		(dial_idx > 0 && dial_buf[0] != '\0') ? dial_buf : "";
	char formatted[40];
	if (raw[0] != '\0')
		format_phone_display(raw, formatted, sizeof(formatted));
	else
		formatted[0] = '\0';

	if (ui_have_last
	    && pct == ui_last_batt_pct
	    && charging == ui_last_charging
	    && bars == ui_last_signal_bars
	    && strcmp(ui_status, ui_last_status) == 0
	    && strcmp(raw, ui_last_number) == 0) {
		return;
	}

	oled_show_ui(
		ui_status,
		formatted[0] ? formatted : nullptr,
		pct, charging, bars
	);

	ui_last_batt_pct = pct;
	ui_last_charging = charging;
	ui_last_signal_bars = bars;
	strncpy(ui_last_status, ui_status, sizeof(ui_last_status) - 1);
	ui_last_status[sizeof(ui_last_status) - 1] = '\0';
	strncpy(ui_last_number, raw, sizeof(ui_last_number) - 1);
	ui_last_number[sizeof(ui_last_number) - 1] = '\0';
	ui_have_last = true;
}

static void ui_sleep(void)
{
	if (!display_awake)
		return;
	display_awake = false;
	oled_display_off();
	call_logf("t=%lu OLED_SLEEP", (unsigned long)millis());
}

static void ui_wake(void)
{
	last_ui_activity_ms = millis();
	if (display_awake) {
		ui_refresh();
		return;
	}
	display_awake = true;
	oled_display_on();
	ui_have_last = false;
	call_logf("t=%lu OLED_WAKE", (unsigned long)millis());
	ui_refresh();
}

/** Reset idle timer; wake the panel if it was blanked. */
static void ui_note_activity(void)
{
	last_ui_activity_ms = millis();
	if (!display_awake)
		ui_wake();
}

/*
 * Status text lives in flash for the same reason the log strings do. Takes a
 * flash pointer: a RAM string passed here reads from the wrong address space
 * and renders garbage on the panel.
 */
static void ui_set_status_P(const char *status)
{
	if (status == nullptr) {
		ui_status[0] = '\0';
	} else {
		strncpy_P(ui_status, status, sizeof(ui_status) - 1);
		ui_status[sizeof(ui_status) - 1] = '\0';
	}
	ui_refresh();
}

#define ui_set_status(status) ui_set_status_P(PSTR(status))

/**
 * Enqueue a dial debug event. Interrupts must already be masked / in ISR.
 */
void dial_dbg_push_locked(
	uint8_t type, uint8_t pulse_count, char ascii, uint16_t dt_ms
)
{
	uint8_t next = (uint8_t)((dial_dbg_head + 1) % DIAL_DBG_QUEUE_LEN);
	if (next == dial_dbg_tail) {
		dial_dbg_dropped++;
		return;
	}
	dial_dbg_q[dial_dbg_head].t_ms = millis();
	dial_dbg_q[dial_dbg_head].type = type;
	dial_dbg_q[dial_dbg_head].pulses = pulse_count;
	dial_dbg_q[dial_dbg_head].ascii = ascii;
	dial_dbg_q[dial_dbg_head].dt_ms = dt_ms;
	dial_dbg_q[dial_dbg_head].hall_level =
		(digitalRead(SW_HALL) == LOW) ? 0 : 1;
	dial_dbg_head = next;
}


/**
 * Flush queued dial debug lines to SD card file DIAL.LOG.
 */
void dial_dbg_flush()
{
	char line[96];
	while (true) {
		noInterrupts();
		if (dial_dbg_tail == dial_dbg_head) {
			uint16_t dropped = dial_dbg_dropped;
			dial_dbg_dropped = 0;
			interrupts();
			if (dropped) {
				snprintf_P(
					line, sizeof(line),
					PSTR("t=%lu DROP dbg_q=%u"),
					(unsigned long)millis(),
					(unsigned)dropped
				);
				sd_log_append(line);
			}
			break;
		}
		DialDbgEvent ev;
		ev.t_ms = dial_dbg_q[dial_dbg_tail].t_ms;
		ev.type = dial_dbg_q[dial_dbg_tail].type;
		ev.pulses = dial_dbg_q[dial_dbg_tail].pulses;
		ev.ascii = dial_dbg_q[dial_dbg_tail].ascii;
		ev.dt_ms = dial_dbg_q[dial_dbg_tail].dt_ms;
		ev.hall_level = dial_dbg_q[dial_dbg_tail].hall_level;
		dial_dbg_tail = (uint8_t)((dial_dbg_tail + 1) % DIAL_DBG_QUEUE_LEN);
		interrupts();

		/*
		 * Flash pointers, printed with %S (capital) rather than %s.
		 * Mixing the two silently prints garbage, so the format strings
		 * below and these assignments must change together.
		 */
		const char *name = PSTR("?");
		switch (ev.type) {
		case DDBG_HALL_SEEN: name = PSTR("HALL_SEEN"); break;
		case DDBG_COMMIT_REG: name = PSTR("COMMIT_REG"); break;
		case DDBG_COMMIT_FALLBACK:
			name = PSTR("COMMIT_FALLBACK");
			break;
		case DDBG_DIGIT: name = PSTR("DIGIT"); break;
		case DDBG_QUEUE_DROP: name = PSTR("DIGIT_Q_DROP"); break;
		case DDBG_DISCARD: name = PSTR("DISCARD"); break;
		default: break;
		}
		if (ev.ascii) {
			snprintf_P(
				line, sizeof(line),
				PSTR("t=%lu %S pulses=%u ascii=%c dt=%u hall=%u"),
				(unsigned long)ev.t_ms, name,
				(unsigned)ev.pulses, ev.ascii,
				(unsigned)ev.dt_ms, (unsigned)ev.hall_level
			);
		} else {
			snprintf_P(
				line, sizeof(line),
				PSTR("t=%lu %S pulses=%u dt=%u hall=%u"),
				(unsigned long)ev.t_ms, name,
				(unsigned)ev.pulses,
				(unsigned)ev.dt_ms, (unsigned)ev.hall_level
			);
		}
		sd_log_append(line);
		Serial.println(line);
	}
}


/**
 * Enqueue a completed dial digit.
 * Caller must run with interrupts masked (ISR context, or noInterrupts()).
 * Returns false if the queue was full (digit dropped — should not happen at
 * human dial rates with DIAL_DIGIT_QUEUE_LEN=16).
 */
bool dial_digit_enqueue(char pulse_count, bool hook_held)
{
	uint8_t next = (uint8_t)((dial_digit_head + 1) % DIAL_DIGIT_QUEUE_LEN);
	if (next == dial_digit_tail) {
		dial_digit_dropped++;
		dial_dbg_push_locked(DDBG_QUEUE_DROP, pulse_count, 0, 0);
		return false;
	}
	dial_digit_q[dial_digit_head].pulse_count = pulse_count;
	dial_digit_q[dial_digit_head].hook_held = hook_held;
	dial_digit_head = next;
	return true;
}


void rotary_edge_push(unsigned long t_ms)
{
	rotary_edge_ms[rotary_edge_head] = t_ms;
	rotary_edge_head = (uint8_t)((rotary_edge_head + 1) % ROTARY_EDGE_HISTORY);
	if (rotary_edge_count < ROTARY_EDGE_HISTORY) {
		rotary_edge_count++;
	}
}


/**
 * Sample hook once per dial activity for ALT speed-dial.
 * Interrupts must already be masked.
 */
void dial_sample_hook_once_locked()
{
	if (!hook_sampled) {
		hook_during_dial = (digitalRead(SW_HOOK) == LOW);
		hook_sampled = true;
	}
}


/**
 * Newest rotary edge timestamp with t > last_commit_ms, or 0 if none.
 * Interrupts must already be masked.
 */
unsigned long dial_newest_edge_after_commit_locked()
{
	uint8_t count = rotary_edge_count;
	if (count == 0) return 0;
	uint8_t oldest = (uint8_t)((rotary_edge_head + ROTARY_EDGE_HISTORY - count)
		% ROTARY_EDGE_HISTORY);
	unsigned long newest = 0;
	for (uint8_t i = 0; i < count; i++) {
		unsigned long t_ms = rotary_edge_ms[(oldest + i) % ROTARY_EDGE_HISTORY];
		if (t_ms > last_commit_ms) newest = t_ms;
	}
	return newest;
}


/**
 * Copy activity edges (t > last_commit_ms) into out[]; return count.
 * Interrupts must already be masked.
 */
uint8_t dial_copy_activity_edges_locked(unsigned long *out, uint8_t out_len)
{
	uint8_t count = rotary_edge_count;
	uint8_t oldest = (uint8_t)((rotary_edge_head + ROTARY_EDGE_HISTORY - count)
		% ROTARY_EDGE_HISTORY);
	uint8_t n = 0;
	for (uint8_t i = 0; i < count && n < out_len; i++) {
		unsigned long t_ms = rotary_edge_ms[(oldest + i) % ROTARY_EDGE_HISTORY];
		if (t_ms > last_commit_ms) {
			out[n++] = t_ms;
		}
	}
	return n;
}


/**
 * Debounce raw edges into accepted[]; return accepted count.
 */
uint8_t dial_debounce_times(
	const unsigned long *times, uint8_t n,
	unsigned long *accepted, uint8_t accepted_len
)
{
	uint8_t out_n = 0;
	for (uint8_t i = 0; i < n && out_n < accepted_len; i++) {
		if (out_n == 0
		    || (times[i] - accepted[out_n - 1]) > ROTARY_DEBOUNCE_MS) {
			accepted[out_n++] = times[i];
		}
	}
	return out_n;
}


/**
 * Max |iv - mean| / mean as a percentage for accepted[0..n).
 * Single interval (2 pulses) → 0. Fewer than 2 pulses → 999.
 */
uint16_t dial_max_rel_dev_pct(const unsigned long *accepted, uint8_t n)
{
	if (n < 2) return 999;
	if (n == 2) return 0;
	unsigned long sum = 0;
	uint8_t count = (uint8_t)(n - 1);
	for (uint8_t i = 1; i < n; i++) {
		sum += accepted[i] - accepted[i - 1];
	}
	unsigned long mean = sum / count;
	if (mean == 0) return 999;
	uint16_t worst = 0;
	for (uint8_t i = 1; i < n; i++) {
		unsigned long iv = accepted[i] - accepted[i - 1];
		unsigned long dev = (iv > mean) ? (iv - mean) : (mean - iv);
		uint16_t pct = (uint16_t)((dev * 100UL) / mean);
		if (pct > worst) worst = pct;
	}
	return worst;
}


/**
 * Grow return suffix from the end. Sets *start_out to first index in accepted[].
 * Returns pulse count of the suffix, or 0 if too short.
 * Mirrors tools/pulse_monitor/dial_extract.py grow_return_suffix().
 */
uint8_t dial_grow_return_suffix(
	const unsigned long *accepted, uint8_t n,
	uint8_t max_pct, uint8_t *start_out
)
{
	if (n < MIN_PULSES_FOR_DIGIT) return 0;
	uint8_t start = (uint8_t)(n - 1);
	while (start > 0 && (uint8_t)(n - start) < MAX_PULSES_FOR_DIGIT) {
		uint8_t prev = (uint8_t)(start - 1);
		unsigned long new_gap = accepted[start] - accepted[prev];
		if (new_gap == 0) break;
		uint8_t suffix_len = (uint8_t)(n - start);
		if (suffix_len == 1) {
			start = prev;
			continue;
		}
		unsigned long interval_sum = accepted[n - 1] - accepted[start];
		uint8_t interval_count = (uint8_t)(suffix_len - 1);
		unsigned long mean = interval_sum / interval_count;
		if (mean == 0) break;
		unsigned long dev = (new_gap > mean) ? (new_gap - mean) : (mean - new_gap);
		uint16_t pct = (uint16_t)((dev * 100UL) / mean);
		if (pct > max_pct) break;
		start = prev;
	}
	uint8_t pulses = (uint8_t)(n - start);
	if (pulses < MIN_PULSES_FOR_DIGIT) return 0;
	if (pulses > MAX_PULSES_FOR_DIGIT) {
		start = (uint8_t)(n - MAX_PULSES_FOR_DIGIT);
		pulses = MAX_PULSES_FOR_DIGIT;
	}
	*start_out = start;
	return pulses;
}


/**
 * True when pulse count maps to a dial digit '0'..'9'.
 */
bool dial_pulses_valid(char pulses)
{
	char ascii = pulse2ascii(pulses);
	return ascii != '?'
		&& pulses >= MIN_PULSES_FOR_DIGIT
		&& pulses <= MAX_PULSES_FOR_DIGIT;
}


/**
 * Pulse-only digit extract. Mirrors tools/pulse_monitor/dial_extract.py.
 * prefer_regular: if true, only accept a trusted regular suffix (else
 * best-effort). Sets *has_regular if a trusted regular return exists.
 */
char dial_extract_pulses(
	const unsigned long *times,
	uint8_t n,
	bool allow_best_effort,
	bool *has_regular,
	uint8_t *dbg_type,
	char *ascii_out
)
{
	unsigned long accepted[ROTARY_EDGE_HISTORY];
	uint8_t an;
	uint8_t start = 0;
	uint8_t pulses;

	*has_regular = false;
	if (n == 0) {
		*dbg_type = DDBG_DISCARD;
		*ascii_out = '?';
		return 0;
	}

	an = dial_debounce_times(times, n, accepted, ROTARY_EDGE_HISTORY);
	pulses = dial_grow_return_suffix(accepted, an, MAX_REL_DEV_PCT, &start);
	if (pulses >= MIN_PULSES_FOR_DIGIT) {
		uint16_t pct = dial_max_rel_dev_pct(accepted + start, pulses);
		bool lone_pair = (pulses == 2 && an == 2);
		bool regular_ok = !lone_pair
			&& (pulses == 2 || pct <= MAX_REL_DEV_PCT)
			&& dial_pulses_valid((char)pulses);
		if (regular_ok) {
			*has_regular = true;
			*dbg_type = DDBG_COMMIT_REG;
			*ascii_out = pulse2ascii((char)pulses);
			return (char)pulses;
		}
	}

	if (!allow_best_effort) {
		*dbg_type = DDBG_DISCARD;
		*ascii_out = '?';
		return 0;
	}

	/* Best effort: grow with loose cap, pick lowest-dev valid trailing trim. */
	pulses = dial_grow_return_suffix(accepted, an, 100, &start);
	if (pulses < MIN_PULSES_FOR_DIGIT) {
		*dbg_type = DDBG_DISCARD;
		*ascii_out = '?';
		return 0;
	}
	{
		uint8_t best_pulses = 0;
		uint16_t best_pct = 999;
		uint8_t base = start;
		uint8_t base_n = pulses;
		for (uint8_t cut = 0;
		     cut + MIN_PULSES_FOR_DIGIT <= base_n;
		     cut++) {
			uint8_t cand_n = (uint8_t)(base_n - cut);
			if (cand_n > MAX_PULSES_FOR_DIGIT) continue;
			uint16_t pct = dial_max_rel_dev_pct(
				accepted + base + cut, cand_n
			);
			if (pct < best_pct
			    || (pct == best_pct && cand_n > best_pulses)) {
				if (dial_pulses_valid((char)cand_n)) {
					best_pct = pct;
					best_pulses = cand_n;
				}
			}
		}
		if (best_pulses >= MIN_PULSES_FOR_DIGIT) {
			*dbg_type = DDBG_COMMIT_FALLBACK;
			*ascii_out = pulse2ascii((char)best_pulses);
			return (char)best_pulses;
		}
	}

	*dbg_type = DDBG_DISCARD;
	*ascii_out = '?';
	return 0;
}


// Always record the edge. Hall never gates rotary capture.
void isr_rotary()
{
	unsigned long now = millis();
	rotary_edge_push(now);
	dial_sample_hook_once_locked();
}


/**
 * Hall telemetry only — must not affect digit extraction.
 */
void isr_hall()
{
	unsigned long now = millis();
	if (hall_last_ms != 0 && (now - hall_last_ms) <= HALL_LOG_DEBOUNCE_MS) {
		return;
	}
	hall_last_ms = now;
	dial_dbg_push_locked(DDBG_HALL_SEEN, 0, 0, 0);
}


/**
 * When activity has been quiet long enough, extract and enqueue a digit.
 */
void poll_dial_digit_completion()
{
	unsigned long activity[ROTARY_EDGE_HISTORY];
	uint8_t n;
	unsigned long last_edge;
	unsigned long now;
	unsigned long quiet_ms;
	bool has_regular = false;
	bool allow_best_effort;

	noInterrupts();
	now = millis();
	last_edge = dial_newest_edge_after_commit_locked();

	if (last_edge == 0) {
		pulsing = false;
		interrupts();
		return;
	}

	pulsing = true;
	quiet_ms = now - last_edge;
	if (quiet_ms <= PULSES_DONE_MS) {
		interrupts();
		return;
	}

	n = dial_copy_activity_edges_locked(activity, ROTARY_EDGE_HISTORY);
	bool hook_held = hook_during_dial;

	/*
	 * Probe for a trusted regular return without committing best-effort yet.
	 * If none, wait until FALLBACK_COMMIT_MS so a wind pause is not a digit.
	 */
	uint8_t dbg_type = DDBG_DISCARD;
	char ascii = '?';
	char pulse_count = dial_extract_pulses(
		activity, n, false, &has_regular, &dbg_type, &ascii
	);
	allow_best_effort = (quiet_ms > FALLBACK_COMMIT_MS);
	if (!has_regular && !allow_best_effort) {
		interrupts();
		return;
	}
	if (!has_regular && allow_best_effort) {
		pulse_count = dial_extract_pulses(
			activity, n, true, &has_regular, &dbg_type, &ascii
		);
	}

	uint16_t dt = (uint16_t)quiet_ms;
	if (pulse_count >= MIN_PULSES_FOR_DIGIT && ascii != '?') {
		dial_dbg_push_locked(dbg_type, pulse_count, ascii, dt);
		dial_digit_enqueue(pulse_count, hook_held);
	} else if (allow_best_effort) {
		dial_dbg_push_locked(DDBG_DISCARD, pulse_count, ascii, dt);
	} else {
		interrupts();
		return;
	}

	last_commit_ms = last_edge;
	hook_sampled = false;
	hook_during_dial = false;
	pulsing = false;
	interrupts();
}


// Forward declaration — definition follows pulse2ascii / display helpers.
void handle_completed_digit(char pulse_count, bool speed_dial_hook);


/**
 * Drain queued digits. Safe to call while the user keeps dialing: capture
 * stays in ISRs and this only performs UI/modem side effects.
 */
void process_dial_digit_queue()
{
	while (true) {
		// Commit any burst that went quiet while previous UI work blocked.
		poll_dial_digit_completion();
		noInterrupts();
		if (dial_digit_tail == dial_digit_head) {
			interrupts();
			break;
		}
		char pulse_count = dial_digit_q[dial_digit_tail].pulse_count;
		bool hook_held = dial_digit_q[dial_digit_tail].hook_held;
		dial_digit_tail = (uint8_t)((dial_digit_tail + 1) % DIAL_DIGIT_QUEUE_LEN);
		interrupts();
		handle_completed_digit(pulse_count, hook_held);
	}
}


void isr_hook()
{
	unsigned long hook_cur = millis();
	if (hook_cur - hook_last > 30) {
		hook_last = hook_cur;
		// Track when button was pressed, don't trigger action yet
		if (!hook_pressed) {
			hook_pressed = true;
			hook_press_start = hook_cur;
		}
	}
}


void isr_clear()
{
	/*
	 * Defer to loop: when the display is asleep, C must wake without
	 * deleting (display_idle.clear_should_delete).
	 */
	clear_request = true;
}


char pulse2ascii(char pulse_count)
{
	// Apply PULSE_FUDGE to correct for off-by-one error
	// The rotary mechanism counts one extra pulse
	pulse_count = pulse_count - PULSE_FUDGE;

	// Rotary dial positions:
	// Dial "1" = 2 pulses (after fudge: 1) → should display '1'
	// Dial "2" = 3 pulses (after fudge: 2) → should display '2'
	// ...
	// Dial "9" = 10 pulses (after fudge: 9) → should display '9'
	// Dial "0" = 11 pulses (after fudge: 10) → should display '0'

	if (pulse_count == 10) return '0';
	if (pulse_count >= 1 && pulse_count <= 9) return pulse_count + '0';
	else return '?';
}





/**
 * Apply one completed dial digit (UI / mode / call side effects).
 *
 * Rotary and hall interrupts remain enabled for the whole function so further
 * digits are counted and queued even while OLED, SD, or modem work blocks.
 * Blocking calls are bracketed with poll_dial_digit_completion() so a quiet
 * burst that ends mid-wait is committed promptly.
 */
void handle_completed_digit(char pulse_count, bool speed_dial_hook)
{
	char entered_digit = pulse2ascii(pulse_count);
	CallMode mode = effective_call_mode(DISABLE_CALL_TYPE_MODES);
	CallMode phys = physical_call_mode();
	noInterrupts();
	dial_dbg_push_locked(DDBG_DIGIT, pulse_count, entered_digit, 0);
	interrupts();
	poll_dial_digit_completion();

	call_logf(
		"t=%lu DIGIT digit=%c pulses=%u phys=%s eff=%s hook_held=%u buf=%s",
		(unsigned long)millis(), entered_digit, (unsigned)pulse_count,
		call_mode_name(phys), call_mode_name(mode),
		(unsigned)speed_dial_hook, dial_buf
	);

	/* Address-book / speed-dial path (disabled when DISABLE_CALL_TYPE_MODES). */
	if (mode == CALL_MODE_ALT) {
		int n = entered_digit - '0';
		bool speed_dial = speed_dial_hook;

		call_logf(
			"t=%lu ALT n=%d speed_dial=%u",
			(unsigned long)millis(), n, (unsigned)speed_dial
		);

		if (n >= 0 && n <= 9) {
			if (speed_dial) {
				int contact_line = (pg * 9) - 9 + n;

				SDgetContact(contact_line);
				poll_dial_digit_completion();

				dial_idx = 0;
				for (int j = 0; j < kc && j < DIAL_BUF_LEN - 1; j++) {
					dial_buf[dial_idx++] = CNumber[j] + '0';
				}
				dial_buf[dial_idx] = '\0';

				oled_enable();
				oled_clear();
				oled_draw_str(CName, 0, 20);
				oled_draw_str(dial_buf, 0, 35);

				call_logf(
					"t=%lu SPEED_DIAL contact=%d name=%s num=%s",
					(unsigned long)millis(), contact_line,
					CName, dial_buf
				);

				delay(500);
				poll_dial_digit_completion();
				last_number_store(dial_buf);
				int rc = lara_dial(dial_buf, sizeof(dial_buf));
				call_logf(
					"t=%lu SPEED_DIAL_ATD rc=%d",
					(unsigned long)millis(), rc
				);
				poll_dial_digit_completion();
			} else {
				pg = epd_displayContacts(n);
				poll_dial_digit_completion();

				int contact_line = (n == 0) ? 10 : n;
				SDgetContact(contact_line);
				poll_dial_digit_completion();

				dial_idx = 0;
				for (int j = 0; j < kc && j < DIAL_BUF_LEN - 1; j++) {
					dial_buf[dial_idx++] = CNumber[j] + '0';
				}
				dial_buf[dial_idx] = '\0';

				oled_enable();
				oled_clear();
				oled_draw_str(CName, 0, 20);
				oled_draw_str(dial_buf, 0, 35);

				call_logf(
					"t=%lu CONTACT_PAGE page=%d line=%d name=%s num=%s",
					(unsigned long)millis(), n, contact_line,
					CName, dial_buf
				);
			}
		} else {
			oled_print("INVALID", 0, 30);
			call_log("ALT_INVALID digit");
		}
		return;
	}

	if (dial_idx >= DIAL_BUF_LEN - 1) {
		call_log("BUF_FULL digit ignored");
		return;
	}

	if (mode == CALL_MODE_LOCAL && dial_idx < strlen(sd_PREPEND())) {
		strcpy(dial_buf, sd_PREPEND());
		dial_idx = strlen(sd_PREPEND());
		call_logf(
			"t=%lu LOCAL_PREPEND %s",
			(unsigned long)millis(), dial_buf
		);
	}
	dial_buf[dial_idx] = entered_digit;
	dial_buf[++dial_idx] = 0;
	/*
	 * Entering digits is not "Dialing" — that status is only for ATD.
	 * Keep Ready / CALL ENDED / etc.; number lives on OLED row 2.
	 * Digits count as activity (wake + reset 20s idle timer).
	 */
	ui_note_activity();
	ui_refresh();
	call_logf("t=%lu BUF %s", (unsigned long)millis(), dial_buf);
}


void effects_leds_on()
{
	// Turn on all effects LEDs
	digitalWrite(LED1A, HIGH);
	digitalWrite(LED2A, HIGH);
	digitalWrite(LED2R, HIGH);
	digitalWrite(LED3A, HIGH);
	digitalWrite(LED3R, HIGH);
	digitalWrite(LED4A, HIGH);
	digitalWrite(LED4R, HIGH);
	digitalWrite(LED5A, HIGH);
	digitalWrite(LED5R, HIGH);
}


void effects_leds_off()
{
	// Turn off all effects LEDs
	digitalWrite(LED1A, LOW);
	digitalWrite(LED2A, LOW);
	digitalWrite(LED2R, LOW);
	digitalWrite(LED3A, LOW);
	digitalWrite(LED3R, LOW);
	digitalWrite(LED4A, LOW);
	digitalWrite(LED4R, LOW);
	digitalWrite(LED5A, LOW);
	digitalWrite(LED5R, LOW);
}


void setup()
{
	Serial.begin(115200);

	// Print firmware version
	Serial.print(F("RUSP Firmware v"));
	Serial.println(FIRMWARE_VERSION);

	pinMode(LED_STAT, OUTPUT);
	pinMode(LED_FILAMENT, OUTPUT);
	pinMode(LED_BELL, OUTPUT);
	pinMode(LED1A, OUTPUT);
	pinMode(LED2A, OUTPUT);
	pinMode(LED2R, OUTPUT);
	pinMode(LED3A, OUTPUT);
	pinMode(LED3R, OUTPUT);
	pinMode(LED4A, OUTPUT);
	pinMode(LED4R, OUTPUT);
	pinMode(LED5A, OUTPUT);
	pinMode(LED5R, OUTPUT);
	pinMode(RINGER_P, OUTPUT);
	pinMode(RINGER_N, OUTPUT);
	pinMode(RELAY_OFF, OUTPUT);
	pinMode(LL_OE, OUTPUT);
	pinMode(EN_3V3, OUTPUT);
	pinMode(EN_12V, OUTPUT);
	pinMode(EN_OUTAMP, OUTPUT);
	pinMode(CELL_ON, OUTPUT);
	pinMode(CHIPSELECT, OUTPUT);
	pinMode(SW_ROTARY, INPUT_PULLUP);
	pinMode(SW_C, INPUT_PULLUP);
	pinMode(SW_HOOK, INPUT_PULLUP);
	pinMode(SW_ALPHA, INPUT_PULLUP);
	pinMode(SW_BETA, INPUT_PULLUP);
	pinMode(SW_LAMBDA, INPUT_PULLUP);
	pinMode(SW_FN, INPUT_PULLUP);
	pinMode(SW_LOCAL, INPUT_PULLUP);
	pinMode(SW_ALT, INPUT_PULLUP);
	pinMode(SW_NONLOCAL, INPUT_PULLUP);
	pinMode(SW_HALL, INPUT_PULLUP);
	pinMode(OFFSIGNAL, INPUT_PULLUP);
	/*
	 * Intended CHG sense (D44/PL5) is unconnected on the PCB — D2/STAT never
	 * reaches the MCU. Pull-up + active-LOW ⇒ idle unless a STAT jumper is
	 * added (see battery.cpp).
	 */
	pinMode(CHG_STAT, INPUT_PULLUP);

	// call ISR on rotary switch falling edge (internally pulled up)
	enableInterrupt(SW_ROTARY, isr_rotary, FALLING);
	enableInterrupt(SW_HALL, isr_hall, FALLING);
	enableInterrupt(SW_HOOK, isr_hook, FALLING);
	enableInterrupt(SW_C, isr_clear, FALLING);

	digitalWrite(EN_12V, HIGH);
	digitalWrite(EN_3V3, HIGH);
	digitalWrite(LL_OE, HIGH);

	char ver_line[24];
	snprintf_P(ver_line, sizeof(ver_line), PSTR("RUSP v%s"), FIRMWARE_VERSION);

	// SD before OLED: SD.begin drops SPI clock for the card.
	sd_init(&Serial);

	oled_init();
	/* Centered boot messages (original style) — not the icon chrome. */
	oled_print_status(ver_line, "Booting");
	delay(400);

	if (sd_is_ready()) {
		oled_print_status("SD card", "Ready");
		char banner[80];
		snprintf_P(
			banner, sizeof(banner),
			PSTR("t=%lu BOOT rusp_v%s dial_debug=1"),
			(unsigned long)millis(), FIRMWARE_VERSION
		);
		sd_log_append(banner);
		sd_log_append(
			"# HALL_SEEN|COMMIT_REG|COMMIT_FALLBACK|DISCARD|DIGIT"
		);
	} else {
		oled_print_status("SD card", "Not found");
	}
	delay(500);

	oled_print_status("Cellular", "Starting modem");
	Serial.println(F("hello! turning LARA on"));
	digitalWrite(LED_STAT, HIGH);
	int modem_rc = lara_on(&Serial1, &Serial, 10000);
	digitalWrite(LED_STAT, LOW);
	if (modem_rc == 0) {
		oled_print_status("Cellular", "Modem ready");
	} else {
		oled_print_status("Cellular", "Modem error");
	}
	delay(500);

	oled_print_status("E-ink", "Splash screen");
	Serial.println(F("Displaying startup splash screen..."));
	epd_splash();
	sd_recover_spi();
	oled_reclaim_spi();

	Serial.println(F("Startup complete!"));
	chg_hist_boot_dump(&Serial);
	{
		int chg0 = battery_chg_stat_raw();
		Serial.print(F("CHG at boot raw="));
		Serial.println(chg0);
		chg_hist_note_raw((uint8_t)chg0, millis());
	}
	Serial.println(F("SD over serial: sd help | sd ls | sd cat DIAL.LOG"));
	if (sd_is_ready()) {
		char ready_line[48];
		snprintf_P(
			ready_line, sizeof(ready_line),
			PSTR("t=%lu READY"), (unsigned long)millis()
		);
		sd_log_append(ready_line);
	}
	delay(400);
	call_logf(
		"t=%lu CONFIG disable_call_types=%d keep_oled=%d batt=%d%%",
		(unsigned long)millis(),
		(int)DISABLE_CALL_TYPE_MODES,
		(int)KEEP_OLED_DISPLAY,
		battery_read_percent()
	);
	/* Ensure side / filament LEDs are off after boot (no dim stuck glow). */
	digitalWrite(LED_STAT, LOW);
	digitalWrite(LED_FILAMENT, LOW);
	digitalWrite(LED_BELL, LOW);
	digitalWrite(LED1A, LOW);
	digitalWrite(LED2A, LOW);
	digitalWrite(LED2R, LOW);
	digitalWrite(LED3A, LOW);
	digitalWrite(LED3R, LOW);
	digitalWrite(LED4A, LOW);
	digitalWrite(LED4R, LOW);
	digitalWrite(LED5A, LOW);
	digitalWrite(LED5R, LOW);
	/* Idle chrome: signal | battery + "Ready to dial". */
	ui_have_last = false;
	display_awake = true;
	last_ui_activity_ms = millis();
	ui_set_status("Ready");
	pin_scan_init();
}


/**
 * Read console lines. "sd ..." talks to the SD card; anything else is forwarded
 * to the modem as an AT line (replaces former char-wise LARA passthrough).
 */
void service_console_serial()
{
	static char line[64];
	static uint8_t len = 0;

	while (Serial.available()) {
		char c = (char)Serial.read();
		if (c == '\r')
			continue;
		if (c == '\n') {
			line[len] = '\0';
			if (len > 0) {
				if (!sd_handle_serial_command(&Serial, line)) {
					/* Forward to LARA as a complete line. */
					Serial1.write(line);
					Serial1.write('\r');
				}
			}
			len = 0;
		} else if (len + 1 < sizeof(line)) {
			line[len++] = c;
		} else {
			/* Overflow — drop the line. */
			len = 0;
		}
	}
}


void loop()
{
	// Highest-priority dial path after ISRs: commit quiet bursts, then drain
	// the digit queue before slower UI/modem work.
	poll_dial_digit_completion();
	process_dial_digit_queue();
	dial_dbg_flush();
	service_console_serial();

	unsigned long t = millis();

	/* Dump / diff all named GPIOs — find which line moves with USB/charge. */
	pin_scan_service(t);

	if (digitalRead(OFFSIGNAL) == LOW) shutdown();

	/*
	 * C button: wake always; delete only if the display was already on
	 * (tools/pulse_monitor/display_idle.py).
	 */
	if (clear_request) {
		noInterrupts();
		clear_request = false;
		interrupts();
		const bool was_awake = display_awake;
		ui_wake();
		if (was_awake) {
			if (!DISABLE_CALL_TYPE_MODES
			    && prev_mode == CALL_MODE_LOCAL
			    && dial_idx <= strlen(sd_PREPEND())) {
				/* refuse to delete into the prepend */
			} else {
				if (dial_idx > 0)
					dial_idx -= 1;
				else
					dial_idx = 0;
				dial_buf[dial_idx] = 0;
			}
			ui_have_last = false;
			ui_refresh();
			call_logf(
				"t=%lu CLEAR buf=%s",
				(unsigned long)millis(), dial_buf
			);
		} else {
			call_logf(
				"t=%lu CLEAR_WAKE_ONLY",
				(unsigned long)millis()
			);
		}
	}

	if (ui_dirty) {
		noInterrupts();
		ui_dirty = false;
		interrupts();
		ui_refresh();
	}
	/*
	 * Re-sample meters on a timer, but ui_refresh() no-ops unless values
	 * changed — avoids blanking the OLED every few seconds (flicker).
	 */
	if (display_awake && (t - last_ui_meter_ms >= BATT_UI_REFRESH_MS)) {
		last_ui_meter_ms = t;
		ui_refresh();
	}

	/*
	 * Charger status edge (see battery_is_charging polarity). Log the raw
	 * pin so plug tests can confirm whether pin 44 moves with the amber LED.
	 */
	if (last_chg_poll_ms == 0
	    || (t - last_chg_poll_ms) >= CHG_UI_POLL_MS
	    || t < last_chg_poll_ms) {
		last_chg_poll_ms = t;
		int chg_raw = battery_chg_stat_raw();
		bool charging_now = battery_is_charging();
		if (charging_now != last_chg_charging) {
			call_logf(
				"t=%lu CHG %s raw=%d",
				(unsigned long)millis(),
				charging_now ? "ACTIVE" : "IDLE",
				chg_raw
			);
			chg_hist_note_raw((uint8_t)(chg_raw ? 1 : 0), t);
			last_chg_charging = charging_now;
			if (charging_now) {
				ui_wake();
			} else if (display_awake) {
				ui_have_last = false;
				ui_refresh();
			}
		}
	}

	/*
	 * Blank after OLED_IDLE_TIMEOUT_MS with no input, but keep the panel
	 * awake while actively charging. (Full battery + cable still plugged
	 * reports IDLE — CHRG only means charge cycle active.)
	 */
	if (battery_is_charging()) {
		if (!display_awake)
			ui_wake();
	} else if (display_awake
	           && (t - last_ui_activity_ms >= OLED_IDLE_TIMEOUT_MS)) {
		ui_sleep();
	}

#if !KEEP_OLED_DISPLAY
	// Clear OLED status message after timeout
	if (oled_status_message.length() > 0 &&
	    (t - last_status_message_time > STATUS_MESSAGE_TIMEOUT)) {
		oled_status_message = "";
		oled_clear();
	}
#endif

	// Pulse effects LEDs while rotary dial is turning
	if (pulsing) {
		// Toggle LEDs at regular intervals for pulsing effect
		if (t - last_led_toggle >= LED_PULSE_INTERVAL) {
			last_led_toggle = t;
			led_effects_state = !led_effects_state;
			if (led_effects_state) {
				effects_leds_on();
			} else {
				effects_leds_off();
			}
		}
	} else {
		// Not pulsing - ensure LEDs are off
		if (led_effects_state) {
			effects_leds_off();
			led_effects_state = false;
		}
	}

	// Track physical vs effective call-type mode (bypass forces NONLOCAL).
	CallMode phys_mode = physical_call_mode();
	CallMode cur_mode = effective_call_mode(DISABLE_CALL_TYPE_MODES);
	if (phys_mode != prev_phys_mode) {
		call_logf(
			"t=%lu MODE_PHYS %s->%s eff=%s",
			(unsigned long)millis(),
			call_mode_name(prev_phys_mode),
			call_mode_name(phys_mode),
			call_mode_name(cur_mode)
		);
		prev_phys_mode = phys_mode;
	}
	if (cur_mode != prev_mode) {
		call_logf(
			"t=%lu MODE_EFF %s->%s buf_was=%s",
			(unsigned long)millis(),
			call_mode_name(prev_mode),
			call_mode_name(cur_mode),
			dial_buf
		);
		/*
		 * With call-type modes disabled, physical switch changes must not
		 * wipe digits — call button dials whatever was typed.
		 */
		if (!DISABLE_CALL_TYPE_MODES) {
			dial_idx = 0;
			dial_buf[dial_idx] = 0;
#if !KEEP_OLED_DISPLAY
			oled_clear();
#endif
		}
		prev_mode = cur_mode;
	}

	bool call_ended_urc = false;
	int ucall_stat = -1;
	const bool was_ringing = ringing;
	lara_unsolicited(
		&ringing, &last_ring_evidence_ms, &call_ended_urc, &ucall_stat
	);
	const bool rang_this_drain = ringing && !was_ringing;
	/*
	 * End first, then apply the latest phase. One drain can deliver a
	 * disconnect and the ring of the *next* call together; ending
	 * afterwards cleared the ringing flag that call had just set.
	 *
	 * The stashed URCs carry no ordering, so a ring seen in the same drain
	 * is kept: a spurious ring is cleared by the 5 s no-URC timeout below,
	 * whereas a dropped one silently loses an incoming call.
	 */
	if (call_ended_urc) {
		call_session_end("NO CARRIER or UCALLSTAT:6");
		ui_set_status("Call ended");
		if (rang_this_drain)
			ringing = true;
	}
	/*
	 * Drive call-phase UI from +UCALLSTAT (see call_phase.py):
	 * 2 Dialing, 3 Ringing (far end), 4 Incoming, 0/7 In call, 6 ended.
	 */
	if (ucall_stat >= 0) {
		call_logf(
			"t=%lu UCALLSTAT %d",
			(unsigned long)millis(), ucall_stat
		);
		switch (ucall_stat) {
		case 2: /* dialling */
			outbound_call_active = true;
			ui_set_status("Dialing");
			break;
		case 3: /* MO alerting — remote ringing */
			outbound_call_active = true;
			ui_set_status("Ringing");
			break;
		case 4: /* MT ringing */
			ringing = true;
			/*
			 * Counts as ring liveness. Some networks announce an
			 * incoming call with this and never send a bare RING;
			 * without stamping here the expiry check below has no
			 * baseline and can never end the ring.
			 */
			last_ring_evidence_ms = t;
			ui_set_status("Incoming");
			incoming_ui_shown = true;
			break;
		case 0: /* active */
		case 7: /* connected */
			outbound_call_active = true;
			saw_in_call_cpas = true;
			ringing = false;
			incoming_ui_shown = false;
			ui_set_status("In call");
			break;
		case 6: /* disconnected */
			call_session_end("UCALLSTAT:6");
			ui_set_status("Call ended");
			break;
		default:
			break;
		}
	}

	/*
	 * Poll AT+CLCC while a call is up. CPAS:4 means "call in progress"
	 * (dialling or answered) so it cannot drive "In call". CLCC <stat>
	 * 0 = active (answered). Also catches answer if +UCALLSTAT was
	 * swallowed during an earlier AT expect (now also stashed).
	 */
	/*
	 * Ringing is polled too, not just outbound. An incoming call whose
	 * cancel URC was lost had nothing to reconcile against and the ringer
	 * ran ~24 s past the caller hanging up, stopped only by RING_MAX_MS.
	 * The same poll already tears an outbound call down correctly when its
	 * end URC goes missing. See call_end_detect.clcc_absent_ends_call().
	 */
	if ((outbound_call_active || ringing)
	    && (t - last_call_cpas_poll_ms >= CALL_CPAS_POLL_MS)) {
		last_call_cpas_poll_ms = t;
		int clcc = lara_clcc_stat();
		if (clcc >= 0)
			clcc_absent_polls = 0;
		else if (clcc_absent_polls < CLCC_ABSENT_LIMIT)
			clcc_absent_polls++;
		call_logf(
			"t=%lu CLCC %d absent=%u",
			(unsigned long)millis(), clcc,
			(unsigned)clcc_absent_polls
		);
		if (clcc == 0) {
			saw_in_call_cpas = true;
			ringing = false;
			incoming_ui_shown = false;
			ui_set_status("In call");
		} else if (clcc == 3) {
			ui_set_status("Ringing");
		} else if (clcc == 2) {
			ui_set_status("Dialing");
		} else if (clcc == 4) {
			ringing = true;
			last_ring_evidence_ms = t;
			incoming_ui_shown = true;
			ui_set_status("Incoming");
		} else if (clcc_absent_polls >= CLCC_ABSENT_LIMIT) {
			/*
			 * CLCC says there is no call. This does not require the
			 * call to have been active first — that condition is
			 * exactly what left a refused dial stuck on "Dialing".
			 */
			call_session_end("CLCC reports no calls");
			ui_set_status("Call ended");
		}
	}

	if (ringing) {
		// Start ringing timer on first ring
		if (ringing_start == 0 || t < ringing_start) {
			ringing_start = t;
		}

		/*
		 * Caller gone: no ring evidence for RING_EVIDENCE_TIMEOUT_MS.
		 * Zero means nothing has been stamped yet, so there is no
		 * baseline to measure; t < last means millis() wrapped, which
		 * must not tear down a live call. Mirrors
		 * call_end_detect.ring_evidence_expired().
		 */
		if (last_ring_evidence_ms > 0 && t >= last_ring_evidence_ms
		    && (t - last_ring_evidence_ms > RING_EVIDENCE_TIMEOUT_MS)) {
			call_session_end("no ring evidence");
			ui_set_status("Ready");
		}

		/* Backup safety only — reaching this means the above failed. */
		if (ringing && (t - ringing_start > RING_MAX_MS)) {
			call_session_end("RING_TIMEOUT");
			ui_set_status("Ready");
		}
	}

	if (ringing) {
		if (!incoming_ui_shown) {
			ui_set_status("Incoming");
			incoming_ui_shown = true;
		}

		// Visual ring pattern on FILAMENT and BELL LEDs (operate together)
		// Pattern: ON (pulsing), short pause, ON (pulsing), long pause, repeat
		unsigned long pattern_time = (t - ringing_start) % RING_PATTERN_TOTAL;
		bool should_pulse = false;

		if (pattern_time < RING_ON_DURATION) {
			// First "ring" - pulsing ON
			should_pulse = true;
		} else if (pattern_time < RING_ON_DURATION + RING_SHORT_PAUSE) {
			// Short pause - OFF
			should_pulse = false;
		} else if (pattern_time < RING_ON_DURATION + RING_SHORT_PAUSE + RING_ON_DURATION2) {
			// Second "ring" - pulsing ON
			should_pulse = true;
		} else {
			// Long pause - OFF
			should_pulse = false;
		}

		// Handle pulsing during "ON" periods for BOTH LEDs
		if (should_pulse) {
			if (t - last_filament_toggle >= FILAMENT_PULSE_INTERVAL) {
				last_filament_toggle = t;
				filament_led_state = !filament_led_state;
				// Control both LEDs together
				digitalWrite(LED_FILAMENT, filament_led_state ? HIGH : LOW);
				digitalWrite(LED_BELL, filament_led_state ? HIGH : LOW);
			}
		} else {
			// OFF period - both LEDs off
			digitalWrite(LED_FILAMENT, LOW);
			digitalWrite(LED_BELL, LOW);
			filament_led_state = false;
		}

		// Physical ringer
		if (t & 0b00100000) {
			digitalWrite(RINGER_P, HIGH);
			digitalWrite(RINGER_N, LOW);
		} else {
			digitalWrite(RINGER_P, LOW);
			digitalWrite(RINGER_N, HIGH);
		}
	} else {
		digitalWrite(RINGER_P, LOW);
		digitalWrite(RINGER_N, LOW);
		digitalWrite(LED_BELL, LOW);
		digitalWrite(LED_FILAMENT, LOW);
		filament_led_state = false;
		ringing_start = t;
		incoming_ui_shown = false;
	}

	// Re-check completion late in the loop as well: long work above may have
	// spanned a quiet window that started mid-iteration.
	poll_dial_digit_completion();
	process_dial_digit_queue();

	/*
	 * Hook / call button: one action per press, on release (after debounce).
	 * Matches tools/pulse_monitor/hook_gesture.py — holding longer must not
	 * dial and then hang up in the same press.
	 * Action choice: tools/pulse_monitor/hook_action.py (outbound_call_active
	 * forces hangup even when CPAS still reports ready).
	 */
	const bool hook_drives_call =
		(effective_call_mode(DISABLE_CALL_TYPE_MODES) != CALL_MODE_ALT);
	if (hook_pressed && hook_drives_call) {
		if (digitalRead(SW_HOOK) == HIGH) {
			unsigned long held_ms = t - hook_press_start;
			hook_pressed = false;
			if (held_ms >= HOOK_DEBOUNCE_MS && !hook) {
				hook = true;
				call_logf(
					"t=%lu HOOK_RELEASE held_ms=%lu outbound=%u buf=%s",
					(unsigned long)millis(), held_ms,
					(unsigned)outbound_call_active, dial_buf
				);
			} else {
				call_logf(
					"t=%lu HOOK_BOUNCE held_ms=%lu",
					(unsigned long)millis(), held_ms
				);
			}
		}
	}

	if (hook && hook_drives_call) {
		/*
		 * Capture awake state before wake: hangup always runs; dial/answer
		 * only if the display was already on (hook_action.py).
		 */
		const bool was_awake = display_awake;
		ui_wake();

		lara_activity stat = lara_status();
		const bool has_digits = (dial_idx > 0 && dial_buf[0] != '\0');
		const bool do_hangup =
			(outbound_call_active || stat == LARA_CALLING);
		call_logf(
			"t=%lu HOOK_ACTION cpas=%c outbound=%u awake=%u buf=%s",
			(unsigned long)millis(), (char)stat,
			(unsigned)outbound_call_active,
			(unsigned)was_awake, dial_buf
		);

		if (do_hangup) {
			/*
			 * Hang up even if the display was asleep.
			 * Prefer local outbound flag over CPAS: after ATD the
			 * modem often still reports ready (0) while the call is up.
			 */
			ui_set_status("Ending");
			call_logf(
				"t=%lu CALL_HANGUP cpas=%c outbound=%u",
				(unsigned long)millis(), (char)stat,
				(unsigned)outbound_call_active
			);
			int rc = lara_hangup();
			call_session_end("hook hangup");
			call_logf(
				"t=%lu CALL_HANGUP_DONE rc=%d",
				(unsigned long)millis(), rc
			);
			if (rc == LARA_RC_OK)
				ui_set_status("Call ended");
			else
				ui_set_status("Hangup fail");
		} else if (!was_awake) {
			/* Wake only — do not dial or answer from a dark display. */
			call_log("HOOK_WAKE_ONLY");
		} else if (stat == LARA_RINGING || ringing) {
			/*
			 * `ringing` is the answer-side fallback for a CPAS read
			 * that timed out. Hangup already trusts
			 * outbound_call_active the same way; without this a
			 * visibly ringing phone could not be answered at all.
			 * See tools/pulse_monitor/hook_action.py.
			 */
			ui_set_status("Answering");
			int rc = lara_answer();
			call_logf(
				"t=%lu CALL_ANSWER rc=%d cpas=%c",
				(unsigned long)millis(), rc, (char)stat
			);
			if (rc == LARA_RC_OK) {
				call_session_begin("ATA");
				ui_set_status("In call");
			} else {
				call_session_end("ATA failed");
				ui_set_status("Answer fail");
			}
		} else if (stat == LARA_READY) {
			if (!has_digits) {
				/*
				 * Redial: load the previous number and show it,
				 * but do not dial. The user has not seen it yet
				 * — dialling here would place a call to a
				 * number they only discover once it rings. A
				 * second press takes the ordinary dial path.
				 *
				 * No status change, so it looks exactly like a
				 * hand-dialled number (see handle_completed_digit).
				 * See tools/pulse_monitor/hook_action.py.
				 */
				if (last_number_load(dial_buf, sizeof(dial_buf))) {
					dial_idx = (unsigned char)strlen(dial_buf);
					ui_note_activity();
					ui_refresh();
					call_logf(
						"t=%lu REDIAL_RECALL num=%s",
						(unsigned long)millis(),
						dial_buf
					);
				} else {
					ui_set_status("No number");
					call_log("DIAL_REFUSED empty buffer");
				}
			} else {
				ui_set_status("Dialing");
				/*
				 * Stored on attempt, not on success: a number
				 * the network refused is exactly the one worth
				 * being able to retry.
				 */
				last_number_store(dial_buf);
				call_logf(
					"t=%lu DIAL_START num=%s",
					(unsigned long)millis(), dial_buf
				);
				int rc = lara_dial(dial_buf, sizeof(dial_buf));
				call_logf(
					"t=%lu DIAL_DONE rc=%d",
					(unsigned long)millis(), rc
				);
				if (rc == LARA_RC_OK) {
					call_session_begin("ATD");
					ui_set_status("Dialing");
				} else {
					call_session_end("ATD failed");
					if (rc == LARA_RC_ERROR)
						ui_set_status("Dial rejected");
					else
						ui_set_status("No response");
				}
			}
		} else {
			call_logf(
				"t=%lu HOOK_UNHANDLED cpas=%c outbound=%u",
				(unsigned long)millis(), (char)stat,
				(unsigned)outbound_call_active
			);
			ui_set_status("Busy");
		}
		hook = false;
		hook_pressed = false;
	} else if (hook && !hook_drives_call) {
		call_log("HOOK_IGNORED alt speed-dial mode");
		hook = false;
		hook_pressed = false;
	}
}


void shutdown()
{
	oled_print("GOODBYE", 0, 30);
	Serial.println(F("shutdown called; waiting for cell powerdown"));
	lara_off(5000);
	Serial.end();
	digitalWrite(EN_12V, LOW);
	digitalWrite(RELAY_OFF, HIGH);
	// POWER KILLED
}
