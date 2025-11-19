#include <EnableInterrupt.h>
#include <GxEPD2_BW.h>
#include <SPI.h>

// Define placement new for Arduino (if not already defined)
inline void* operator new(size_t, void* ptr) { return ptr; }

#include "pins.h"
#include "lara.h"
#include "oled.h"
#include "sd.h"
#include "epd.h"

// length of dial buffer (max 255 right now since dial_idx is unsigned char)
#define DIAL_BUF_LEN 30
// subtracted from pulse count to get number dialed
#define PULSE_FUDGE 1
// ms to debounce rotary switch by
#define ROTARY_DEBOUNCE_MS 30
// ms after no pulses have been received when we decide the number is done
#define PULSES_DONE_MS 500

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
static uint8_t eink_buffer[sizeof(GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)>)];
static GxEPD2_BW<GxEPD2_290_flex, MAX_HEIGHT(GxEPD2_290_flex)> *eink = nullptr;
static bool eink_constructed = false;

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
	oled_print(dial_buf, 0, 30);
}


char pulse2ascii(char pulse_count)
{
	pulse_count -= PULSE_FUDGE;
	if (pulse_count == 10) pulse_count = 0;
	if ((unsigned char)pulse_count < 10) return pulse_count + 0x30;
	else return '?';
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
	oled_print("STARTING", 0, 40);

	Serial.println("hello! turning LARA on");
	digitalWrite(LED_STAT, HIGH);
	lara_on(&Serial1, &Serial, 10000);
	digitalWrite(LED_STAT, LOW);

	oled_clear();
	digitalWrite(LED_BELL, LOW);
	
	// Note: ePaper will be initialized after a delay in the main loop
}


void loop()
{
	unsigned long t = millis();

	if (digitalRead(OFFSIGNAL) == LOW) shutdown();
	
	// Splash screen on SW_ALPHA button press
	static unsigned long alpha_last = 0;
	if (digitalRead(SW_ALPHA) == LOW && (t - alpha_last > 1000)) {
		alpha_last = t;
		Serial.println("Displaying splash screen...");
		epd_splash();
	}

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
		oled_print("INCOMING CALL", 0, 20);
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
			
			// Display contacts page on ePaper
			int n = pulse2ascii(pulses) - '0';  // Convert to actual digit
			if (n >= 0 && n <= 9) {
				Serial.print("Displaying contacts page ");
				Serial.println(n);
				pg = epd_displayContacts(n);
			}

			// Match the number to the contact list for dialing
			switch (contact_idx) {
			#define CASE_CONTACT(c, f) \
			case c: \
				strcpy(dial_buf, sd_##f()); \
				oled_print(dial_buf, 0, 30); \
				dial_idx = strlen(sd_##f()); \
				break;
			FOR_CONTACTS(CASE_CONTACT)
			default:
				oled_print("NOT FOUND", 0, 30);
			}
		// Check whether the dial index is below maximum capacity.
		} else if (dial_idx < DIAL_BUF_LEN - 1) {
			// first digit in local (prepend) mode
			if (digitalRead(SW_LOCAL) == LOW
			&& dial_idx < strlen(sd_PREPEND())) {
				strcpy(dial_buf, sd_PREPEND());
				oled_print(dial_buf, 0, 30);
				dial_idx = strlen(sd_PREPEND());
			}
			// Get the number the user entered.
			dial_buf[dial_idx] = pulse2ascii(pulses);
			// Increment the dial index, and set the next value to
			// the null byte.
			dial_buf[++dial_idx] = 0;
			// Print the dial buffer.
			oled_print(dial_buf, 0, 30);
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
			oled_print("ANSWERING", 0, 30);
			Serial.println("answering");
			lara_answer();
			break;
		case LARA_CALLING:
			oled_print("HANGING UP", 0, 30);
			Serial.println("hanging up");
			lara_hangup();
			break;
		case LARA_READY:
			oled_print("DIALING", 0, 30);
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
