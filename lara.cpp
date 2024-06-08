#include <Arduino.h>
#include <HardwareSerial.h>

#include "lara.h"

static struct lara_state lara;


// expect lara to send a specific string
static int expect(char *str, unsigned long timeout)
{
	unsigned i = 0;
	unsigned long t0 = millis();
	while (str[i]) {
		if (millis() - t0 > timeout) {
			lara.cons->print("LARA: timeout while expecting ");
			lara.cons->println(str);
			return -1;
		}
		if (lara.s->available()) {
			if (lara.s->read() == str[i]) i += 1;
			else i = 0;
		}
	}
	return 0;
}


// expect lara to send one of several specific strings
// @arg indices: array of unsigned zeros that is used to track matching
// returns 0 if error or the index of the matched string, starting at 1
static unsigned multiexpect(
	unsigned str_count, unsigned *indices, char **strs,
	unsigned long timeout
){
	unsigned long t0 = millis();
	do {
		// TODO: we do a whole lot of spinning in this file tbh.
		// it may be possible with some clever asynchronous thinking to
		// find a way to move the available() check for this and other
		// functions into the main event loop so that other phone
		// features aren't blocked while the modem is thinking
		if (lara.s->available()) {
			char c = lara.s->read();
			for (unsigned i=0; i<str_count; i++) {
				if (!strs[i][indices[i]]) return 1 + i;
				if (c == strs[i][indices[i]]) indices[i] += 1;
				else indices[i] = 0;
			}
		}
	} while (millis() - t0 < timeout);
	lara.cons->println("LARA: timeout while multiexpecting {");
	for (unsigned i=0; i<str_count; i++) {
		lara.cons->println(strs[i]);
	}
	lara.cons->println("}");
	return 0;
}


int lara_at_set(char *command, unsigned long timeout)
{
	lara.s->write("AT");
	lara.s->write(command);
	lara.s->write('\r');
	lara.s->flush();
	unsigned indices[] = {0, 0};
	char *strs[] = {"OK\r", "ERROR\r"};
	if (multiexpect(2, indices, strs, timeout) != 1) {
		lara.cons->print("LARA: failed to AT");
		lara.cons->println(command);
		return -1;
	}
	// datasheet tells us to delay >20 ms after receiving a final result
	delay(25);
	return 0;
}


int lara_on(
	HardwareSerial *serial, HardwareSerial *console, unsigned long timeout
)
{
	if (!*console) return -1;
	lara.s = serial;
	lara.cons = console;
	lara.cons->println("LARA: initializing");

	pinMode(CELL_ON, OUTPUT);
	pinMode(NET_STAT, INPUT);
	pinMode(CELL_CTS, INPUT);
	pinMode(CELL_RTS, OUTPUT);
	pinMode(CELL_RESET, OUTPUT);
	pinMode(CELL_PWR_DET, INPUT);

	// power on
	digitalWrite(CELL_ON, HIGH);
	delay(1000);
	digitalWrite(CELL_ON, LOW);
	unsigned long t0 = millis();
	while (digitalRead(CELL_PWR_DET) != HIGH) {
		if (millis() - t0 > timeout) {
			lara.cons->println(
				"LARA: timeout waiting for CELL_PWR_DET"
			);
			return -1;
		}
	}

	// initialize lara
	lara.s->begin(115200);
	while (!*lara.s) {
		if (millis() - t0 > timeout) {
			lara.cons->println(
				"LARA: timeout waiting for serial to begin"
			);
			return -1;
		}
	}

	// lara sends us this on startup
	expect("+PACSP1\r", timeout);

	// enable verbose errors
	lara_at_set("+CMEE=2", 1000);
	// maximum call volume
	lara_at_set("+CLVL=6", 1000);
	// enable codec autoconfiguration on next boot
	lara_at_set("+UEXTDCONF=0,1", 1000);
	// enable URCs for registration
	//lara_at_set("+CREG=2", 1000);
	// enable URCs for voice call status
	//lara_at_set("+UCALLSTAT=1", 1000);

	lara.cons->println("LARA: ready");
	return 0;
}


void lara_unsolicited(bool *ringing)
{
	// we use a similar strategy to multiexpect() but make our parsing
	// static because we cannot be busy-waiting here
	static unsigned indices[] = {0};
	static unsigned str_count = 1;
	static char *strs[] = {"RING\r"};
	if (lara.s->available()) {
		char c = lara.s->read();
		// passthrough
		lara.cons->write(c);
		// parse
		for (unsigned i=0; i<str_count; i++) {
			if (!strs[i][indices[i]]) {
				switch (i) {
				case 0:
					*ringing = true;
					break;
				default:
					break;
				}
				// now reset indices
				for (unsigned j=0; j<str_count; j++) {
					indices[j] = 0;
				}
			}
			if (c == strs[i][indices[i]]) indices[i] += 1;
			else indices[i] = 0;
		}
	}
	// passthrough
	if (lara.cons->available()) lara.s->write(lara.cons->read());
}


lara_activity lara_status()
{
	lara.s->write("AT+CPAS\r");
	lara.s->flush();
	expect("+CPAS: ", 1000);
	while (!lara.s->available()) {};
	lara_activity ret = lara.s->read();
	while (!lara.s->available()) {};
	lara.s->read();	// clear the \r
	return ret;
}


int lara_answer()
{
	lara.s->write("ATA\r");
	lara.s->flush();
	// TODO: error handling
	return 0;
}


int lara_hangup()
{
	lara.s->write("AT+CHUP\r");
	lara.s->flush();
	return expect("OK\r", 1000);
}


int lara_dial(char *dial_string)
{
	lara.s->write("ATD");
	// TODO: figure out what to do if the string has no null terminator
	// (right now we just stop at 32)
	for (unsigned i=0; i<32; i++) {
		if (!dial_string[i]) break;
		lara.s->write(dial_string[i]);
	}
	lara.s->write(";\r");
	lara.s->flush();
	return expect("OK\r", 1000);
}


int lara_off(unsigned long timeout)
{
	lara.s->write("AT+CPWROFF\r");
	lara.s->flush();
	unsigned long t0 = millis();
	while (digitalRead(CELL_PWR_DET) != LOW) {
		if (millis() - t0 > timeout) {
			lara.cons->println(
				"LARA: timeout waiting for CELL_PWR_DET == LOW"
			);
			return -1;
		}
	}
	lara.s->end();
	return 0;
}

