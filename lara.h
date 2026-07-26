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

/* 3GPP 27.007 +CLCC <dir> and <stat> values used to spot an incoming call. */
#define LARA_CLCC_DIR_INCOMING 1
#define LARA_CLCC_STATE_INCOMING 4
#define LARA_CLCC_STATE_WAITING 5

/*
 * The two periodic pollers. Both are asynchronous, and deliberately have no
 * blocking form: they fire on a timer rather than on anything the user did,
 * so a blocking one is a stall nobody asked for. The bell LED stops toggling
 * and rotary dial pulses are delayed for its duration. See the async engine
 * comment in lara.cpp for why the button-driven commands are not the same
 * case.
 *
 * Both share one transaction slot, so a *_start() returns false when the
 * other poller — or a result still unclaimed — holds it; the caller retries.
 * The matching *_take() returns true on exactly one call per completed
 * transaction, whether it ended in a reply, an error or a timeout, which is
 * what lets a caller count polls rather than loop iterations.
 *
 * Serviced from lara_unsolicited(), so the loop needs no extra pump.
 */

/*
 * Poll the call list. stat_out receives the preferred <stat> (0 active,
 * 2 dialling, 3 alerting, 4 incoming, …), or -1 if no calls / error.
 *
 * incoming_out (may be NULL) is set when any entry is a mobile-terminated
 * call that is alerting — state 4, or state 5 when another call is already
 * up. Call waiting is the only announcement a return call gets while an
 * earlier call is still active.
 */
bool lara_clcc_poll_start(void);
bool lara_clcc_poll_take(int *stat_out, bool *incoming_out);

/*
 * Abandon an outstanding call-state poll. Call when the call session it was
 * asked about is torn down or replaced, so its reply cannot be applied to a
 * different session.
 */
void lara_clcc_poll_cancel(void);

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
 * Poll AT+CSQ. rssi_out receives 0..31, or 99 if unknown / error.
 * Maps to OLED bars via lara_signal_bars(). See lara_clcc_poll_start().
 */
bool lara_csq_poll_start(void);
bool lara_csq_poll_take(int *rssi_out);

/* Map CSQ RSSI to 0..4 bars (99/≤0 → 0). */
int lara_signal_bars(int rssi);

// power off
int lara_off(unsigned long timeout);

#endif	// include guard

