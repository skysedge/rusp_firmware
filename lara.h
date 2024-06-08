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

// state of the lara system
struct lara_state {
	// serial port lara is talking on
	HardwareSerial *s;
	// serial port the console is on
	HardwareSerial *cons;
};

// execute an AT set command (a command that can only return OK or ERROR)
int lara_at_set(char *command, unsigned timeout);

// initialize the modem
int lara_on(HardwareSerial *serial, HardwareSerial *console, unsigned timeout);

// pass lara's serial port through to the console and vice versa
void lara_passthrough();

// dial a phone number
int lara_dial(char *dialstring);

// power off
int lara_off(unsigned timeout);

#endif	// include guard

