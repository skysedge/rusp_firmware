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
// for tracking hook hold time
bool hook_pressed = false;
unsigned long hook_press_start = 0;
#define HOOK_HOLD_TIME 1000  // Must hold for 1 second
// are we ringing the bell
bool ringing = false;
// when did ringing start
unsigned long ringing_start = 0;
// one of SW_ALT, SW_LOCAL, SW_NONLOCAL (or 0 for uninitialized)
int prev_mode = 0;
// OLED digit feedback display
String oled_dialed_digits = "";
unsigned long last_digit_display_time = 0;
#define DIGIT_DISPLAY_TIMEOUT 3000  // Clear after 3 seconds
// OLED status message display
String oled_status_message = "";
unsigned long last_status_message_time = 0;
#define STATUS_MESSAGE_TIMEOUT 2000  // Clear status after 2 seconds
// LED effects for rotary dial
unsigned long last_led_toggle = 0;
bool led_effects_state = false;
#define LED_PULSE_INTERVAL 50  // Toggle LEDs every 100ms for visible pulsing


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
	delay(200);	// TODO: see above
	digitalWrite(LED_FILAMENT, LOW);
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
	// Rotary dial positions:
	// Dial "1" = 1 pulse → should display '1'
	// Dial "2" = 2 pulses → should display '2'
	// ...
	// Dial "9" = 9 pulses → should display '9'
	// Dial "0" = 10 pulses → should display '0'

	if (pulse_count == 10) return '0';
	if (pulse_count >= 1 && pulse_count <= 9) return pulse_count + '0';
	else return '?';
}


void show_dialed_digit_on_oled(char digit)
{
	// Add digit to display string
	oled_dialed_digits += digit;
	last_digit_display_time = millis();

	// Display immediately on OLED
	oled_enable();
	oled_clear();

	// Show just the accumulated digits (no label needed)
	char digit_buf[DIAL_BUF_LEN];
	oled_dialed_digits.toCharArray(digit_buf, DIAL_BUF_LEN);
	oled_draw_str(digit_buf, 0, 10);

	// Show mode indicator if in alt mode (on second line if needed)
	if (digitalRead(SW_ALT) == LOW) {
		oled_draw_str("(ALT)", 0, 30);
	}

	Serial.print("OLED showing digit: ");
	Serial.println(oled_dialed_digits);
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

	// Display splash screen on startup
	Serial.println("Displaying startup splash screen...");
	epd_splash();
	Serial.println("Startup complete!");
}


void loop()
{
	unsigned long t = millis();

	if (digitalRead(OFFSIGNAL) == LOW) shutdown();

	// Clear OLED digit display after timeout
	if (oled_dialed_digits.length() > 0 &&
	    (t - last_digit_display_time > DIGIT_DISPLAY_TIMEOUT)) {
		oled_dialed_digits = "";
		oled_clear();
	}

	// Clear OLED status message after timeout
	if (oled_status_message.length() > 0 &&
	    (t - last_status_message_time > STATUS_MESSAGE_TIMEOUT)) {
		oled_status_message = "";
		oled_clear();
	}

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
		oled_dialed_digits = "";  // Clear digit display
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
			// Clear the "INCOMING CALL" message
			oled_status_message = "";
			oled_clear();
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

		// CRITICAL: Show the digit on OLED FIRST, before any mode logic
		char entered_digit = pulse2ascii(pulses);
		show_dialed_digit_on_oled(entered_digit);

		// alt (contacts) mode
		if (digitalRead(SW_ALT) == LOW) {
			// Get the number the user entered.
			int n = pulse2ascii(pulses) - '0';  // Convert to actual digit

			// Check if hook was held while dialing (Speed Dial mode)
			bool speed_dial = (digitalRead(SW_HOOK) == LOW);

			if (n >= 0 && n <= 9) {
				if (speed_dial) {
					// Speed dial: Load contact from current page and dial immediately
					// Calculate actual contact line: contacts on page pg, position n
					// Page 1 (pg=1) positions 1-9 = contacts 1-9
					// Page 2 (pg=2) positions 1-9 = contacts 10-18, etc.
					int contact_line = (pg * 9) - 9 + n;

					Serial.print("Speed dial: Loading contact ");
					Serial.print(contact_line);
					Serial.print(" (page ");
					Serial.print(pg);
					Serial.print(", position ");
					Serial.print(n);
					Serial.println(")");

					SDgetContact(contact_line);

					// Convert CNumber[] array to dial_buf string
					dial_idx = 0;
					for (int j = 0; j < kc - 2 && j < DIAL_BUF_LEN - 1; j++) {
						dial_buf[dial_idx++] = CNumber[j] + '0';
					}
					dial_buf[dial_idx] = '\0';

					// Display on OLED
					oled_enable();
					oled_clear();
					oled_draw_str(CName, 0, 20);
					oled_draw_str(dial_buf, 0, 35);

					Serial.print("Speed dialing: ");
					Serial.print(CName);
					Serial.print(" - ");
					Serial.println(dial_buf);

					// Dial immediately
					delay(500);  // Brief delay to show the contact
					lara_dial(dial_buf);

				} else {
					// Regular Alt mode: Show contacts page and load first contact
					Serial.print("Alt mode: Loading contact page ");
					Serial.println(n);

					// Display contacts page on ePaper and remember which page
					pg = epd_displayContacts(n);

					// Load the first contact from this page for potential dialing
					int contact_line = (n == 0) ? 10 : n;
					SDgetContact(contact_line);

					// Convert CNumber[] array to dial_buf string
					dial_idx = 0;
					for (int j = 0; j < kc - 2 && j < DIAL_BUF_LEN - 1; j++) {
						dial_buf[dial_idx++] = CNumber[j] + '0';
					}
					dial_buf[dial_idx] = '\0';

					// Display on OLED
					oled_enable();
					oled_clear();
					oled_draw_str(CName, 0, 20);
					oled_draw_str(dial_buf, 0, 35);

					Serial.print("Loaded contact: ");
					Serial.print(CName);
					Serial.print(" - ");
					Serial.println(dial_buf);
				}
			} else {
				oled_print("INVALID", 0, 30);
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

	// Check if hook button is being held
	if (hook_pressed) {
		// Check if button is still pressed
		if (digitalRead(SW_HOOK) == LOW) {
			// Button still held - check if held long enough
			if ((t - hook_press_start >= HOOK_HOLD_TIME) && !hook) {
				// Held for 1 second - trigger action
				hook = true;
				Serial.println("hook held for 1 second - triggering action");
			}
		} else {
			// Button released before 1 second
			hook_pressed = false;
			if (!hook) {
				Serial.println("hook released too early - ignoring");
			}
		}
	}

	if (hook) {
		Serial.println("hook pressed");
		oled_dialed_digits = "";  // Clear digit display on hook press
		lara_activity stat = lara_status();
		switch (stat) {
		case LARA_RINGING:
			oled_status_message = "ANSWERING";
			last_status_message_time = t;
			oled_print("ANSWERING", 0, 30);
			Serial.println("answering");
			lara_answer();
			break;
		case LARA_CALLING:
			oled_status_message = "HANGING UP";
			last_status_message_time = t;
			oled_print("HANGING UP", 0, 30);
			Serial.println("hanging up");
			lara_hangup();
			break;
		case LARA_READY:
			oled_status_message = "DIALING";
			last_status_message_time = t;
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
		hook_pressed = false;  // Reset the press tracking
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
