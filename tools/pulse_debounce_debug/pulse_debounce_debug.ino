/*
 * Rotary pulse debounce debug sketch for ATmega1280/2560 RUSP boards.
 *
 * Flashes in place of production firmware. Logs every SW_ROTARY falling edge
 * (raw) and whether the production-style debounce accepted or rejected it.
 *
 * Serial protocol (115200 8N1; same as rusp_firmware.ino), one event per line:
 *   READY board=atmega2560 baud=115200 debounce_ms=30 done_ms=200
 *   HALL t_ms=<millis>
 *   RAW t_us=<micros> dt_us=<since last raw>
 *   ACCEPTED t_ms=<millis> n=<count> dt_ms=<since last accepted>
 *   REJECTED t_ms=<millis> n=<count> dt_ms=<since last accepted>
 *   DIGIT raw=<n> accepted=<n> ascii=<c> intervals_ms=a,b,c
 *   OVERFLOW dropped=<n>
 *
 * Host: tools/pulse_monitor/pulse_monitor.py
 */

#include <Arduino.h>

// Match pins.h (kept local so this sketch folder is self-contained).
#define SW_ROTARY 2
#define SW_HALL 3
#define LED_STAT 47
#define EN_3V3 34
#define EN_12V 33
#define LL_OE 38

// Match rusp_firmware.ino production constants.
#define PULSE_FUDGE 1
#define ROTARY_DEBOUNCE_MS 30
#define PULSES_DONE_MS 200

#define EDGE_BUF_LEN 64
#define MAX_INTERVALS 16
/* Match production rusp_firmware.ino Serial.begin(115200). */
#define SERIAL_BAUD 115200

typedef struct {
	uint32_t us;
	uint32_t ms;
} EdgeSample;

volatile EdgeSample edge_buf[EDGE_BUF_LEN];
volatile uint8_t edge_head = 0;
volatile uint8_t edge_tail = 0;
volatile uint16_t edge_dropped = 0;

volatile bool hall_pending = false;
volatile uint32_t hall_ms = 0;

volatile bool pulsing = false;

uint32_t last_raw_us = 0;
bool have_raw = false;
uint32_t last_accepted_ms = 0;
bool have_accepted = false;
uint16_t raw_count = 0;
uint16_t accepted_count = 0;
uint16_t intervals_ms[MAX_INTERVALS];
uint8_t interval_count = 0;

char pulse2ascii(uint16_t pulse_count)
{
	int16_t adjusted = (int16_t)pulse_count - PULSE_FUDGE;
	if (adjusted == 10) return '0';
	if (adjusted >= 1 && adjusted <= 9) return (char)('0' + adjusted);
	return '?';
}

void isr_rotary()
{
	if (!pulsing) return;
	uint8_t next = (uint8_t)((edge_head + 1) % EDGE_BUF_LEN);
	if (next == edge_tail) {
		edge_dropped++;
		return;
	}
	/* Capture times in the ISR so serial latency cannot stretch bounce. */
	edge_buf[edge_head].us = micros();
	edge_buf[edge_head].ms = millis();
	edge_head = next;
}

void isr_hall()
{
	hall_ms = millis();
	hall_pending = true;
}

void reset_session()
{
	noInterrupts();
	edge_head = 0;
	edge_tail = 0;
	interrupts();
	have_raw = false;
	have_accepted = false;
	raw_count = 0;
	accepted_count = 0;
	interval_count = 0;
	last_raw_us = 0;
	last_accepted_ms = 0;
}

void begin_dial(uint32_t t_ms)
{
	reset_session();
	pulsing = true;
	Serial.print(F("HALL t_ms="));
	Serial.println(t_ms);
}

void emit_digit()
{
	Serial.print(F("DIGIT raw="));
	Serial.print(raw_count);
	Serial.print(F(" accepted="));
	Serial.print(accepted_count);
	Serial.print(F(" ascii="));
	Serial.print(pulse2ascii(accepted_count));
	Serial.print(F(" intervals_ms="));
	if (interval_count == 0) {
		Serial.println(F("-"));
	} else {
		for (uint8_t i = 0; i < interval_count; i++) {
			if (i) Serial.print(',');
			Serial.print(intervals_ms[i]);
		}
		Serial.println();
	}
	pulsing = false;
	digitalWrite(LED_STAT, LOW);
}

void setup()
{
	pinMode(SW_ROTARY, INPUT_PULLUP);
	pinMode(SW_HALL, INPUT_PULLUP);
	pinMode(LED_STAT, OUTPUT);
	pinMode(EN_3V3, OUTPUT);
	pinMode(EN_12V, OUTPUT);
	pinMode(LL_OE, OUTPUT);

	digitalWrite(LED_STAT, LOW);
	digitalWrite(EN_12V, HIGH);
	digitalWrite(EN_3V3, HIGH);
	digitalWrite(LL_OE, HIGH);

	Serial.begin(SERIAL_BAUD);
	while (!Serial && millis() < 2000) {
		/* wait briefly for USB-serial on hosts that enumerate slowly */
	}

	attachInterrupt(digitalPinToInterrupt(SW_ROTARY), isr_rotary, FALLING);
	attachInterrupt(digitalPinToInterrupt(SW_HALL), isr_hall, FALLING);

	Serial.print(F("READY board=atmega2560 baud="));
	Serial.print(SERIAL_BAUD);
	Serial.print(F(" debounce_ms="));
	Serial.print(ROTARY_DEBOUNCE_MS);
	Serial.print(F(" done_ms="));
	Serial.println(PULSES_DONE_MS);
}

void loop()
{
	if (hall_pending) {
		noInterrupts();
		uint32_t t_ms = hall_ms;
		hall_pending = false;
		interrupts();
		begin_dial(t_ms);
	}

	noInterrupts();
	uint16_t dropped = edge_dropped;
	edge_dropped = 0;
	interrupts();
	if (dropped) {
		Serial.print(F("OVERFLOW dropped="));
		Serial.println(dropped);
	}

	while (true) {
		noInterrupts();
		if (edge_tail == edge_head) {
			interrupts();
			break;
		}
		/* Copy fields explicitly; cannot copy a volatile struct by value. */
		uint32_t t_us = edge_buf[edge_tail].us;
		uint32_t t_ms = edge_buf[edge_tail].ms;
		edge_tail = (uint8_t)((edge_tail + 1) % EDGE_BUF_LEN);
		interrupts();
		uint32_t dt_us = 0;
		if (have_raw) {
			dt_us = t_us - last_raw_us;
		}
		last_raw_us = t_us;
		have_raw = true;
		raw_count++;

		Serial.print(F("RAW t_us="));
		Serial.print(t_us);
		Serial.print(F(" dt_us="));
		Serial.println(dt_us);

		bool accept = false;
		uint32_t dt_ms = 0;
		if (!have_accepted) {
			accept = true;
			dt_ms = 0;
		} else {
			dt_ms = t_ms - last_accepted_ms;
			/* Strict '>' matches rusp_firmware isr_rotary. */
			accept = dt_ms > ROTARY_DEBOUNCE_MS;
		}

		if (accept) {
			if (have_accepted && interval_count < MAX_INTERVALS) {
				intervals_ms[interval_count++] = (uint16_t)dt_ms;
			}
			accepted_count++;
			last_accepted_ms = t_ms;
			have_accepted = true;
			digitalWrite(LED_STAT, HIGH);
			Serial.print(F("ACCEPTED t_ms="));
			Serial.print(t_ms);
			Serial.print(F(" n="));
			Serial.print(accepted_count);
			Serial.print(F(" dt_ms="));
			Serial.println(dt_ms);
			digitalWrite(LED_STAT, LOW);
		} else {
			Serial.print(F("REJECTED t_ms="));
			Serial.print(t_ms);
			Serial.print(F(" n="));
			Serial.print(accepted_count);
			Serial.print(F(" dt_ms="));
			Serial.println(dt_ms);
		}
	}

	if (pulsing && accepted_count > 0
	    && (millis() - last_accepted_ms) > PULSES_DONE_MS) {
		emit_digit();
	}
}
