#include <EnableInterrupt.h>

#include "pins.h"
#include "lara.h"
#include "oled.h"
#include "sd.h"

// length of dial buffer (max 255 right now since dial_idx is unsigned char)
#define DIAL_BUF_LEN 30
// subtracted from pulse count to get number dialed
#define PULSE_FUDGE 1
// ms to debounce rotary switch by
#define ROTARY_DEBOUNCE_MS 30
// ms after no pulses have been received when we decide the number is done
#define PULSES_DONE_MS 500
// battery max in mV
#define LIPOMAX 4150
// battery min in mV (consider 2750? failsafe cutoff is 2500)
#define LIPOMIN 3500

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

// ugly global variables
// should we be counting pulses?
bool pulsing = false;
// how many pulses have we counted since we started counting?
char pulses = 0;
// what was the time of the last pulse?
unsigned long pulse_last = 0;
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
// are we ringing the bell
bool ringing = false;
// when did ringing start
unsigned long ringing_start = 0;
// one of SW_ALT, SW_LOCAL, SW_NONLOCAL (or 0 for uninitialized)
int prev_mode = 0;
// alternate every 0x2000 ms
bool clk2000h = true;


// Interrupt Service Routines for the rotary dial's "pulse" switch, which is a
// tied-to-GND normally-open limit switch that rolls against a cam
void isr_rotary()
{
	// only count if the hall triggered
	if (!pulsing) return;
	// debounce
	unsigned long pulse_cur = millis();
	if (pulse_cur - pulse_last > ROTARY_DEBOUNCE_MS) {
		digitalWrite(LED_STAT, HIGH);
		digitalWrite(LED5A, HIGH);
		digitalWrite(LED5R, HIGH);
		pulse_last = pulse_cur;
		pulses += 1;
		// TODO: we really should not be stopping everything for the LED
		// flashes lol
		delay(10);
		digitalWrite(LED_STAT, LOW);
		digitalWrite(LED5A, LOW);
		digitalWrite(LED5R, LOW);
	}
}


void isr_hall()
{
	digitalWrite(LED_FILAMENT, HIGH);
	pulsing = true;
	pulses = 0;
	delay(10);	// TODO: see above
	digitalWrite(LED_FILAMENT, LOW);
}


void isr_hook()
{
	unsigned long hook_cur = millis();
	if (hook_cur - hook_last > 30) {
		hook_last = hook_cur;
		hook = true;
	}
}


void isr_clear()
{
	// if in local mode, refuse to delete into the prepend
	if (prev_mode == SW_LOCAL && dial_idx <= strlen(sd_PREPEND()))
		return;
	// Decrement the dial index without underflow.
	if (dial_idx > 0) dial_idx -= 1;
	else dial_idx = 0;
	// Null-terminate the dial buffer.
	dial_buf[dial_idx] = 0;
	// Print the new dial buffer.
	oled_print(dial_buf, 0, 48);
}


char pulse2ascii(char pulse_count)
{
	pulse_count -= PULSE_FUDGE;
	if (pulse_count == 10) pulse_count = 0;
	if ((unsigned char)pulse_count < 10) return pulse_count + 0x30;
	else return '?';
}


// thanks to
// https://provideyourown.com/2012/
//     secret-arduino-voltmeter-measure-battery-voltage/
long vcc() {
	// read 1.1V reference against AVcc
	// set the reference to Vcc and the measurement to the internal 1.1V
	// reference
	ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
	delay(2); // wait for Vref to settle
	ADCSRA |= _BV(ADSC); // start conversion
	while (bit_is_set(ADCSRA,ADSC)); // measuring

	uint8_t low  = ADCL; // must read ADCL first - it then locks ADCH
	uint8_t high = ADCH; // unlocks both

	long result = (high<<8) | low;
	// calculate Vcc (in mV); 1125300 = 1.1*1023*1000
	result = 1125300L / result;
	return result; // Vcc in millivolts
}


// convert charge in mV (reported from vcc()) to percent
unsigned char battery_percent() {
	int chargeLevel;
	int lipo_range = LIPOMAX - LIPOMIN;

	int lipo = ((vcc() - LIPOMIN) * 100) / (LIPOMAX - LIPOMIN);
	if (lipo > 100) return 100;
	if (lipo < 0) return 0;
	return (unsigned char)lipo;
}


// write the decimal representation of an unsigned char to a string,
// right-aligned with last digit at `str_end - 1`
void decimal_uchar_flush_right(char *str_end, unsigned char x) {
	unsigned char hundreds = x / 100;
	x -= 100 * hundreds;
	unsigned char tens = x / 10;
	x -= 10 * tens;
	str_end[-1] = 0x30 + x;
	if (tens || hundreds) str_end[-2] = 0x30 + tens;
	if (hundreds) str_end[-3] = 0x30 + hundreds;
}


void setup()
{
	Serial.begin(115200);

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
	pinMode(CHG_STAT, INPUT);

	digitalWrite(LED_BELL, HIGH);

	// call ISR on rotary switch falling edge (internally pulled up)
	enableInterrupt(SW_ROTARY, isr_rotary, FALLING);
	enableInterrupt(SW_HALL, isr_hall, FALLING);
	enableInterrupt(SW_HOOK, isr_hook, FALLING);
	enableInterrupt(SW_C, isr_clear, FALLING);

	digitalWrite(EN_12V, HIGH);
	digitalWrite(EN_3V3, HIGH);
	digitalWrite(LED_BELL, HIGH);
	digitalWrite(LL_OE, HIGH);

	// this needs to be before oled_init because it sets the SPI speed to
	// very low
	sd_init(&Serial);

	oled_init();
	oled_print("STARTING", 0, 48);

	Serial.println("hello! turning LARA on");
	digitalWrite(LED_STAT, HIGH);
	lara_on(&Serial1, &Serial, 10000);
	digitalWrite(LED_STAT, LOW);

	oled_clear();
	digitalWrite(LED_BELL, LOW);
}


void loop()
{
	unsigned long t = millis();

	if (digitalRead(OFFSIGNAL) == LOW) shutdown();
	// TODO: this is for debug purposes
	if (digitalRead(SW_ALPHA) == LOW) ringing = true;

	// figure out which throw the 1p3t switch is on
	int cur_mode;
	if (digitalRead(SW_ALT) == LOW) cur_mode = SW_ALT;
	else if (digitalRead(SW_LOCAL) == LOW) cur_mode = SW_LOCAL;
	else cur_mode = SW_NONLOCAL;
	if (cur_mode != prev_mode) {
		// clear dial buffer if switched to new mode
		dial_idx = 0;
		dial_buf[dial_idx] = 0;
		prev_mode = cur_mode;
		oled_clear();
	}

	lara_unsolicited(&ringing);

	if (ringing) {
		oled_print("INCOMING CALL", 0, 48);
		if (t - ringing_start < 2000) {
			if (t & 0b00100000) {
				digitalWrite(RINGER_P, HIGH);
				digitalWrite(RINGER_N, LOW);
				digitalWrite(LED_BELL, HIGH);
			} else {
				digitalWrite(RINGER_P, LOW);
				digitalWrite(RINGER_N, HIGH);
				digitalWrite(LED_BELL, LOW);
			}
		} else {
			ringing = false;
		}
	} else {
		digitalWrite(RINGER_P, LOW);
		digitalWrite(RINGER_N, LOW);
		digitalWrite(LED_BELL, LOW);
		ringing_start = t;
	}

	// if it's been a while since the last pulse we counted, assume that
	// number is done being entered
	if (pulses && t - pulse_last > PULSES_DONE_MS) {
		disableInterrupt(SW_ROTARY);
		disableInterrupt(SW_HALL);

		// alt (contacts) mode
		if (digitalRead(SW_ALT) == LOW) {
			// Get the number the user entered.
			char contact_idx = pulse2ascii(pulses);

			// Match the number to the contact list.
			switch (contact_idx) {
			#define CASE_CONTACT(c, f) \
			case c: \
				strcpy(dial_buf, sd_##f()); \
				oled_print(dial_buf, 0, 48); \
				dial_idx = strlen(sd_##f()); \
				break;
			FOR_CONTACTS(CASE_CONTACT)
			default:
				oled_print("NOT FOUND", 0, 48);
			}
		// Check whether the dial index is below maximum capacity.
		} else if (dial_idx < DIAL_BUF_LEN - 1) {
			// first digit in local (prepend) mode
			if (digitalRead(SW_LOCAL) == LOW
			&& dial_idx < strlen(sd_PREPEND())) {
				strcpy(dial_buf, sd_PREPEND());
				oled_print(dial_buf, 0, 48);
				dial_idx = strlen(sd_PREPEND());
			}
			// Get the number the user entered.
			dial_buf[dial_idx] = pulse2ascii(pulses);
			// Increment the dial index, and set the next value to
			// the null byte.
			dial_buf[++dial_idx] = 0;
			// Print the dial buffer.
			oled_print(dial_buf, 0, 48);
			Serial.print("entered: ");
			Serial.println(dial_buf);
		}

		// Reset the rotary dial variables.
		pulsing = false;
		pulses = 0;

		enableInterrupt(SW_ROTARY, isr_rotary, FALLING);
		enableInterrupt(SW_HALL, isr_hall, FALLING);
	}

	if (hook) {
		Serial.println("hook pressed");
		lara_activity stat = lara_status();
		switch (stat) {
		case LARA_RINGING:
			oled_print("ANSWERING", 0, 48);
			Serial.println("answering");
			lara_answer();
			break;
		case LARA_CALLING:
			oled_print("HANGING UP", 0, 48);
			Serial.println("hanging up");
			lara_hangup();
			break;
		case LARA_READY:
			oled_print("DIALING", 0, 48);
			Serial.println("dialing");
			lara_dial(dial_buf);
			break;
		default:
			Serial.print("don't know how to handle status ");
			Serial.write(stat);
			Serial.println();
		}
		hook = false;
	}

	// every 8192 ms
	if (!!(t & 0x2000) != clk2000h) {
		clk2000h = !clk2000h;
		Serial.print("Vcc: ");
		Serial.print(vcc());
		Serial.println();
		char status[] = "    % net       % bat";
		decimal_uchar_flush_right(status + 16, battery_percent());
		// TODO: bat percent
		// TODO: separate statusline from regular line
		oled_print(status, 0, 20);
	}
}


void shutdown()
{
	oled_print("GOODBYE", 0, 30);
	Serial.println("shutdown called; waiting for cell powerdown");
	lara_off(5000);
	Serial.end();
	digitalWrite(EN_12V, LOW);
	digitalWrite(RELAY_OFF, HIGH);
	// POWER KILLED
}

