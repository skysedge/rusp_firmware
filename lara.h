#ifndef RUSP_LARA_H
#define RUSP_LARA_H_

#include <HardwareSerial.h>

// pin definitions
#define CELL_ON A0
#define NET_STAT A5
#define CELL_CTS A3
#define CELL_RTS A4
#define CELL_RESET A1
#define CELL_PWR_DET A2


typedef char lara_activity;
enum {
	LARA_READY	= '0',
	LARA_UNAVAIL	= '1',
	LARA_UNKNOWN	= '2',
	LARA_RINGING	= '3',
	LARA_CALLING	= '4',
	LARA_ASLEEP	= '5',
};

// state of the lara system
struct lara_state {
	// serial port lara is talking on
	HardwareSerial *s;
	// serial port the console is on
	HardwareSerial *cons;
};

// execute an AT set command (a command that can only return OK or ERROR)
int lara_at_set(char *command, unsigned long timeout);

// initialize the modem
int lara_on(
	HardwareSerial *serial, HardwareSerial *console, unsigned long timeout
);

// handle any unsolicited result codes that may have arrived
// also passes lara's serial port through to the console and vice versa
void lara_unsolicited(bool *ringing);

// check if in a call, etc.
lara_activity lara_status();

// answer an incoming call
int lara_answer();

// hang up on a current call
int lara_hangup();

// dial a phone number
int lara_dial(char *dial_string);

// power off
int lara_off(unsigned long timeout);

#endif	// include guard

