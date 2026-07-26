#ifndef RUSP_LARA_H
#define RUSP_LARA_H

#include <HardwareSerial.h>

// pin definitions
#define CELL_ON A0
#define NET_STAT A5
#define CELL_CTS A3
#define CELL_RTS A4
#define CELL_RESET A1
#define CELL_PWR_DET A2


/*
 * Outcome of one AT transaction. OK is 0 so existing `rc == 0` checks and
 * the int-returning helpers below keep their meaning.
 */
enum {
	LARA_RC_OK	= 0,
	LARA_RC_ERROR	= -1,	/* modem answered ERROR / +CME ERROR: ... */
	LARA_RC_TIMEOUT	= -2,	/* no final result code before the deadline */
};

/* Per-command timeouts. Only the failure path waits the whole budget. */
#define LARA_AT_TIMEOUT_MS	1000
#define LARA_DIAL_TIMEOUT_MS	5000
#define LARA_ANSWER_TIMEOUT_MS	5000
#define LARA_HANGUP_TIMEOUT_MS	5000

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
int lara_at_set(const char *command, unsigned long timeout);

// initialize the modem
int lara_on(
	HardwareSerial *serial, HardwareSerial *console, unsigned long timeout
);

/*
 * Handle URCs and pass modem→console bytes through.
 * If call_ended is non-NULL, set *call_ended on NO CARRIER or UCALLSTAT 6.
 * If ucall_stat is non-NULL, set it to the latest +UCALLSTAT <stat> (0..7),
 * or leave unchanged when no UCALLSTAT URC was seen this call.
 */
void lara_unsolicited(
	bool *ringing, unsigned long *last_ring_time,
	bool *call_ended, int *ucall_stat
);

// check if in a call, etc.
lara_activity lara_status();

/*
 * Query AT+CLCC and return the preferred call <stat> (0 active, 2 dialling,
 * 3 alerting, 4 incoming, …), or -1 if no calls / error.
 * Used to detect answered calls when +UCALLSTAT was swallowed by an AT wait.
 */
int lara_clcc_stat(void);

// answer an incoming call
int lara_answer();

// hang up on a current call
int lara_hangup();

/*
 * Dial a NUL-terminated number. buf_len is the caller's array size and
 * bounds the scan, so an unterminated buffer cannot be over-read.
 * Returns LARA_RC_OK / LARA_RC_ERROR / LARA_RC_TIMEOUT.
 */
int lara_dial(const char *dial_string, uint8_t buf_len);

/*
 * Query AT+CSQ. Returns RSSI 0..31, or 99 if unknown / error.
 * Maps to OLED bars via lara_signal_bars().
 */
int lara_signal_rssi(void);

/* Map CSQ RSSI to 0..4 bars (99/≤0 → 0). */
int lara_signal_bars(int rssi);

// power off
int lara_off(unsigned long timeout);

#endif	// include guard

