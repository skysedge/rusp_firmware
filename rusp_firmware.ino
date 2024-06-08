#include <EnableInterrupt.h>

#include "lara.h"

// PINS #{{{
#define ENABLE_GxEPD2_GFX 0
#define SW_ROTARY 2
#define LED_STAT 47
#define LED_FILAMENT 46
#define LED_BELL 4
#define LED1A 8
#define LED2A 6
#define LED2R 16
#define LED3A 17
#define LED3R 40
#define LED4A 42
#define LED4R A6
#define LED5A A7
#define LED5R A8
#define RINGER_P 21
#define RINGER_N 20
#define OFFSIGNAL 27
#define RELAY_OFF 35
#define LL_OE 38
#define EN_3V3 34
#define EN_12V 33
#define EN_OUTAMP A15
#define CELL_ON A0
#define CHIPSELECT 24
#define SW_C 15
#define SW_HOOK 14
#define SW_ALPHA 10
#define SW_BETA 12
#define SW_LAMBDA 11
#define SW_FN 13
#define SW_LOCAL 32
#define SW_ALT 31
#define SW_NONLOCAL 30
#define SW_HALL 3
#define CHG_STAT 44
// #}}}

// length of dial buffer (max 255 right now since dial_idx is a char)
#define DIAL_BUF_LEN 30
// subtracted from pulse count to get number dialed
#define PULSE_FUDGE 1

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
char dial_idx = 0;
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


// Interrupt Service Routines for the rotary dial's "pulse" switch, which is a
// tied-to-GND normally-open limit switch that rolls against a cam
void isr_rotary()
{
	// only count if the hall triggered
	if (!pulsing) return;
	// debounce
	unsigned long pulse_cur = millis();
	if (pulse_cur - pulse_last > 30) {
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


char pulse2ascii(char pulse_count)
{
	pulse_count -= PULSE_FUDGE;
	if (pulse_count == 10) pulse_count = 0;
	if ((unsigned char)pulse_count < 10) return pulse_count + 0x30;
	else return '?';
}


void setup()
{
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

	Serial.begin(115200);
	digitalWrite(LL_OE, HIGH);
	digitalWrite(EN_3V3, HIGH);

	delay(2000);

	Serial.println("hello! turning LARA on");
	digitalWrite(LED_STAT, HIGH);
	lara_on(&Serial1, &Serial, 10000);
	digitalWrite(LED_STAT, LOW);

	digitalWrite(EN_12V, HIGH);
	digitalWrite(LED_BELL, LOW);
}


void loop()
{
	if (digitalRead(OFFSIGNAL) == LOW) shutdown();
	if (digitalRead(SW_ALPHA) == LOW) ringing = true;

	lara_unsolicited(&ringing);

	unsigned long t = millis();

	if (ringing) {
		if (t - ringing_start < 2000) {
			if (t & 0b01000000) {
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
		digitalWrite(LED_FILAMENT, LOW);
		ringing_start = t;
	}

	// if it's been a while since the last pulse we counted, assume that
	// number is done being entered
	if (pulses && t - pulse_last > 600) {
		disableInterrupt(SW_ROTARY);
		disableInterrupt(SW_HALL);
		dial_buf[dial_idx] = pulse2ascii(pulses);
		pulsing = false;
		pulses = 0;
		dial_idx += 1;
		dial_buf[dial_idx] = 0;
		if (dial_idx >= DIAL_BUF_LEN - 1) dial_idx = 0;
		Serial.print("entered: ");
		Serial.println(dial_buf);
		enableInterrupt(SW_ROTARY, isr_rotary, FALLING);
		enableInterrupt(SW_HALL, isr_hall, FALLING);
	}

	if (hook) {
		Serial.println("hook pressed");
		lara_activity stat = lara_status();
		switch (stat) {
		case LARA_RINGING:
			Serial.println("answering");
			lara_answer();
			break;
		case LARA_CALLING:
			Serial.println("hanging up");
			lara_hangup();
			break;
		case LARA_READY:
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
	Serial.println("shutdown called; waiting for cell powerdown");
	lara_off(5000);
	Serial.end();
	digitalWrite(RELAY_OFF, HIGH);
	// POWER KILLED
}

