#include <Arduino.h>
#include <HardwareSerial.h>
#include <stdlib.h>
#include <string.h>

#include "lara.h"

static struct lara_state lara;

/*
 * URCs that arrive while an AT transaction is reading must not be discarded.
 * Stash events here; lara_unsolicited() delivers them.
 */
static bool urc_pending_ringing = false;
static unsigned long urc_pending_last_ring = 0;
static bool urc_pending_call_ended = false;
static int urc_pending_stat = -1;

static void lara_urc_on_byte(char c);
static void lara_async_service(void);
static void lara_async_settle(void);


static char lara_read_byte(void)
{
	char c = (char)lara.s->read();
	lara_urc_on_byte(c);
	return c;
}


/* Longest response line kept intact; longer lines are truncated. */
#define LARA_LINE_LEN 96

enum {
	LARA_FINAL_NONE = 0,
	LARA_FINAL_OK,
	LARA_FINAL_ERROR,
};


/**
 * Classify a complete response line as a final result code.
 *
 * Whole-line comparison is deliberate. The previous substring matcher
 * accepted a quoted "OK" inside a +CLCC row as a terminator, and it never
 * matched the verbose "+CME ERROR: ..." form that +CMEE=2 guarantees is the
 * only error the modem emits.
 *
 * call_progress_final selects the ATD/ATA dialect. For every other command
 * NO CARRIER is unsolicited and must be left to the URC handler, otherwise
 * a dropped call silently becomes some unrelated command's result.
 *
 * Mirrors tools/pulse_monitor/at_transaction.py final_code().
 */
static uint8_t lara_final_code(const char *line, bool call_progress_final)
{
	if (strcasecmp_P(line, PSTR("OK")) == 0)
		return LARA_FINAL_OK;
	if (strcasecmp_P(line, PSTR("ERROR")) == 0)
		return LARA_FINAL_ERROR;
	if (strncasecmp_P(line, PSTR("+CME ERROR:"), 11) == 0
	    || strncasecmp_P(line, PSTR("+CMS ERROR:"), 11) == 0)
		return LARA_FINAL_ERROR;
	if (call_progress_final
	    && (strcasecmp_P(line, PSTR("NO CARRIER")) == 0
	        || strcasecmp_P(line, PSTR("BUSY")) == 0
	        || strcasecmp_P(line, PSTR("NO ANSWER")) == 0
	        || strcasecmp_P(line, PSTR("NO DIALTONE")) == 0))
		return LARA_FINAL_ERROR;
	return LARA_FINAL_NONE;
}


/**
 * Read one complete, non-empty line into buf (NUL-terminated, terminator
 * stripped). Blank CR/LF separators are skipped. Over-long lines are
 * truncated rather than split, so a fragment can never look like a result
 * code. Every byte goes through lara_read_byte() so URCs arriving mid
 * response are still delivered.
 *
 * deadline is an absolute millis() value; the cast keeps the comparison
 * correct across the 49-day rollover.
 */
static bool lara_read_line(char *buf, uint8_t buf_len, unsigned long deadline)
{
	uint8_t len = 0;

	while ((long)(millis() - deadline) < 0) {
		if (!lara.s->available())
			continue;
		char c = lara_read_byte();
		if (c == '\r' || c == '\n') {
			if (len == 0)
				continue;
			buf[len] = '\0';
			return true;
		}
		if (len + 1 < buf_len)
			buf[len++] = c;
	}
	buf[len] = '\0';
	return false;
}


/* True when line is the modem echoing "AT<command>" (ATE1 still in effect). */
static bool lara_line_is_echo(const char *line, const char *command)
{
	if (command == nullptr)
		return false;
	if (strncasecmp_P(line, PSTR("AT"), 2) != 0)
		return false;
	return strcmp(line + 2, command) == 0;
}


/**
 * Read one AT response, consuming everything through its final result code.
 *
 * That consumption is the whole point: lara_status() used to stop after the
 * +CPAS digit and leave "\n\r\nOK\r\n" buffered, which the next command's
 * matcher then accepted as its own result. lara_dial() and lara_hangup()
 * reported success unconditionally as a result.
 *
 * @arg command  text after "AT", used to discard the echo (may be NULL)
 * @arg prefix   information lines to capture (may be NULL)
 * @arg resp     receives the last line matching prefix (may be NULL)
 * Returns 0 on OK, -1 on an error result, -2 on timeout.
 */
static int lara_at_response(
	const char *command, const char *prefix,
	char *resp, uint8_t resp_len,
	bool call_progress_final, unsigned long timeout_ms
)
{
	char line[LARA_LINE_LEN];
	unsigned long deadline = millis() + timeout_ms;
	bool echo_pending = (command != nullptr);
	uint8_t prefix_len = prefix ? (uint8_t)strlen(prefix) : 0;

	if (resp && resp_len)
		resp[0] = '\0';

	while (lara_read_line(line, sizeof(line), deadline)) {
		if (lara.cons)
			lara.cons->println(line);
		if (echo_pending && lara_line_is_echo(line, command)) {
			echo_pending = false;
			continue;
		}
		uint8_t fin = lara_final_code(line, call_progress_final);
		if (fin == LARA_FINAL_OK)
			return LARA_RC_OK;
		if (fin == LARA_FINAL_ERROR)
			return LARA_RC_ERROR;
		if (resp && resp_len && prefix_len
		    && strncasecmp(line, prefix, prefix_len) == 0) {
			strncpy(resp, line, resp_len - 1);
			resp[resp_len - 1] = '\0';
		}
	}
	return LARA_RC_TIMEOUT;
}


/* Write "AT<command>\r". command may be "" for a bare AT ping. */
static void lara_send_at(const char *command)
{
	/*
	 * Every command in this file transmits through here, which is what
	 * makes it the one place that can guarantee no asynchronous
	 * transaction is still outstanding. See lara_async_settle().
	 */
	lara_async_settle();
	lara.s->write("AT");
	if (command && command[0])
		lara.s->write(command);
	lara.s->write('\r');
	lara.s->flush();
}


/*
 * Non-blocking AT transactions.
 *
 * Only the two periodic pollers use this: +CSQ behind the signal meter, and
 * +CLCC while a call session is up. Those are the transactions nobody asked
 * for. They fire on a timer, and each one held loop() for a modem round
 * trip — or for the full second the timeout allows when the modem stayed
 * quiet. Nothing else ran in that window, so the bell LED stopped toggling
 * and rotary dial pulses waited on it.
 *
 * lara_dial(), lara_answer(), lara_hangup(), lara_status() and all of
 * lara_on() / lara_off() stay synchronous deliberately. Each is one shot and
 * driven by a button or the hook switch, so its stall lands where the user
 * is already waiting for the phone to do the thing they just asked it to do.
 * Converting them would mean rebuilding the hook handler as a state machine
 * to buy latency in the one place nobody can perceive it.
 *
 * Mirrors tools/pulse_monitor/at_async.py.
 */

enum {
	LARA_ASYNC_IDLE = 0,
	LARA_ASYNC_WAITING,	/* command sent, no final result code yet */
	LARA_ASYNC_DONE,	/* result held until its owner claims it */
};

/*
 * Longest asynchronous response line kept intact; longer lines are truncated
 * rather than split, so the tail of one line can never start another and be
 * read as a result code. Both async commands are far shorter than this.
 */
#define LARA_ASYNC_LINE_LEN 64

static struct {
	uint8_t state;
	int8_t rc;
	bool echo_pending;
	uint8_t len;
	/* Literal with static storage duration; compared, never copied. */
	const char *command;
	/*
	 * Per-command handler for information lines. It also identifies the
	 * owner of the transaction, which is what stops one poller claiming
	 * the other's result — the two never share a handler.
	 */
	void (*on_line)(const char *line);
	unsigned long deadline;
	char line[LARA_ASYNC_LINE_LEN];
} lara_async;


/**
 * Start an asynchronous transaction. False when the engine is not free.
 *
 * Refused while a result is unclaimed, not only while one is in flight: the
 * result belongs to whoever submitted it, and both pollers share the engine.
 *
 * This is the only place transaction state is reset. Clearing it on
 * completion as well would look safer, but it makes each reset untestable on
 * its own, because the other one covers for a missing one.
 */
static bool lara_async_submit(
	const char *command, void (*on_line)(const char *line),
	unsigned long timeout_ms
)
{
	if (!lara.s || lara_async.state != LARA_ASYNC_IDLE)
		return false;

	lara_async.command = command;
	lara_async.on_line = on_line;
	lara_async.echo_pending = (command != nullptr);
	lara_async.len = 0;
	lara_send_at(command);
	/* Timed from the last byte out, since lara_send_at() flushes. */
	lara_async.deadline = millis() + timeout_ms;
	lara_async.state = LARA_ASYNC_WAITING;
	return true;
}


/*
 * End the transaction, holding the result for its owner.
 *
 * A disowned transaction goes straight back to IDLE instead: nobody is left
 * to claim its result, and a result nobody can claim would hold the engine
 * for good.
 */
static void lara_async_finish(int8_t rc)
{
	lara_async.rc = rc;
	lara_async.state = lara_async.on_line
		? LARA_ASYNC_DONE : LARA_ASYNC_IDLE;
}


/**
 * Consume whatever has arrived, then judge the clock.
 *
 * Bytes are taken before the deadline is tested because a reply that landed
 * just before it is real data; discarding it would count a healthy poll as
 * an absent one.
 *
 * Every byte goes through lara_read_byte(), which is what hands it to the
 * URC matcher — exactly as the blocking reader does. That matcher is
 * byte-oriented and carries state between bytes, so the engine cannot filter
 * what it forwards: a "+UCALLSTAT:" capture closes on its CR, and
 * unsolicited output is never retransmitted.
 *
 * Reading stops at the final result code. What follows belongs to the drain
 * in lara_unsolicited(), which is where a URC arriving on the heels of the
 * reply has to be seen.
 */
static void lara_async_service(void)
{
	if (lara_async.state != LARA_ASYNC_WAITING)
		return;

	/*
	 * The deadline below is evaluated even with no serial attached, so a
	 * transaction can always reach an end state. lara_async_settle() spins
	 * on this function and would not otherwise be guaranteed to return.
	 */
	while (lara.s && lara.s->available()) {
		char c = lara_read_byte();
		if (c != '\r' && c != '\n') {
			if (lara_async.len + 1 < LARA_ASYNC_LINE_LEN)
				lara_async.line[lara_async.len++] = c;
			continue;
		}
		if (lara_async.len == 0)
			continue;
		lara_async.line[lara_async.len] = '\0';
		lara_async.len = 0;
		if (lara.cons)
			lara.cons->println(lara_async.line);
		if (lara_async.echo_pending
		    && lara_line_is_echo(lara_async.line, lara_async.command)) {
			lara_async.echo_pending = false;
			continue;
		}
		/*
		 * Never the call-progress dialect: neither +CSQ nor +CLCC can
		 * produce NO CARRIER, so one arriving here is a dropped call
		 * announcing itself and belongs to the URC path alone.
		 */
		uint8_t fin = lara_final_code(lara_async.line, false);
		if (fin != LARA_FINAL_NONE) {
			lara_async_finish(
				fin == LARA_FINAL_OK
					? LARA_RC_OK : LARA_RC_ERROR
			);
			return;
		}
		if (lara_async.on_line)
			lara_async.on_line(lara_async.line);
	}

	/* Cast keeps the comparison correct across the 49-day rollover. */
	if ((long)(millis() - lara_async.deadline) >= 0)
		lara_async_finish(LARA_RC_TIMEOUT);
}


/**
 * Claim a completed result, freeing the engine. False until then.
 *
 * on_line names the owner. A poller that asks for a result belonging to the
 * other one is told there is nothing to take, so neither can consume the
 * other's reply or free the engine out from under it.
 */
static bool lara_async_take(void (*on_line)(const char *line), int8_t *rc_out)
{
	if (lara_async.state != LARA_ASYNC_DONE || lara_async.on_line != on_line)
		return false;
	lara_async.state = LARA_ASYNC_IDLE;
	if (rc_out)
		*rc_out = lara_async.rc;
	return true;
}


/**
 * Give up ownership of the outstanding transaction, without waiting for it.
 *
 * The reply is still read to completion — the stream has to stay in step for
 * whatever reads next — but nothing is done with it and the result is
 * dropped instead of being offered to a caller that no longer has a use for
 * it. Any result already sitting unclaimed goes the same way.
 */
static void lara_async_disown(void)
{
	if (lara_async.state == LARA_ASYNC_DONE)
		lara_async.state = LARA_ASYNC_IDLE;
	lara_async.on_line = nullptr;
}


/**
 * Drive an in-flight transaction to completion, blocking if it has to.
 *
 * Every synchronous send calls this. One UART cannot carry two unfinished
 * transactions: a poll's reply would still be arriving when ATD's matcher
 * started reading, and its OK would be accepted as the dial's own result —
 * precisely the failure lara_at_response() was written to end.
 *
 * Absorbing the poll rather than discarding it keeps two things true at
 * once. The stream is back in sync for the synchronous command, and the
 * poller still receives its result, so the +CLCC absence count neither skips
 * a poll nor counts one twice. Bounded by the deadline that transaction
 * already carries, so the worst case is the wait it would have cost anyway.
 */
static void lara_async_settle(void)
{
	while (lara_async.state == LARA_ASYNC_WAITING)
		lara_async_service();
}


/* Send "AT<command>" and read its response. See lara_at_response(). */
static int lara_at(
	const char *command, const char *prefix,
	char *resp, uint8_t resp_len, unsigned long timeout_ms
)
{
	lara_send_at(command);
	return lara_at_response(
		command, prefix, resp, resp_len, false, timeout_ms
	);
}


static int lara_apply_boot_config(void);
static bool lara_mno_profile_needs_change(void);
static int lara_set_mno_profile(void);
#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

/* As lara_at(), for the call commands where NO CARRIER / BUSY are results. */
static int lara_at_call(const char *command, unsigned long timeout_ms)
{
	lara_send_at(command);
	return lara_at_response(command, nullptr, nullptr, 0, true, timeout_ms);
}


int lara_at_set(const char *command, unsigned long timeout)
{
	int rc = lara_at(command, nullptr, nullptr, 0, timeout);
	if (rc != LARA_RC_OK) {
		lara.cons->print(F("LARA: failed to AT"));
		lara.cons->print(command);
		lara.cons->println(
			rc == LARA_RC_ERROR ? F(" (ERROR)") : F(" (TIMEOUT)")
		);
		return -1;
	}
	/* datasheet tells us to delay >20 ms after receiving a final result */
	delay(25);
	return 0;
}


/**
 * Is the module answering AT yet? Silence is not reported as a failure.
 *
 * Both callers probe a module that is expected not to answer — one that may
 * still be powered off, and one part-way through a reboot. Routing those
 * through lara_at_set() printed a failure line for every unanswered attempt,
 * so a provisioning boot that was working correctly reported nine
 * consecutive timeouts before saying "ready".
 */
static bool lara_answers_at(unsigned long timeout_ms)
{
	return lara_at("", nullptr, nullptr, 0, timeout_ms) == LARA_RC_OK;
}


/* Wait for an unsolicited line starting with prefix. True if it arrived. */
static bool lara_wait_line(const char *prefix, unsigned long timeout_ms)
{
	char line[LARA_LINE_LEN];
	unsigned long deadline = millis() + timeout_ms;
	uint8_t prefix_len = (uint8_t)strlen(prefix);

	while (lara_read_line(line, sizeof(line), deadline)) {
		if (lara.cons)
			lara.cons->println(line);
		if (strncasecmp(line, prefix, prefix_len) == 0)
			return true;
	}
	return false;
}


/*
 * Ping until the modem answers OK. PWR_DET only says the module has power;
 * this is what proves the UART is usable before any config is attempted.
 */
static bool lara_sync(uint8_t attempts)
{
	for (uint8_t i = 0; i < attempts; i++) {
		if (lara_at("", nullptr, nullptr, 0, 500) == LARA_RC_OK)
			return true;
	}
	return false;
}


int lara_on(
	HardwareSerial *serial, HardwareSerial *console, unsigned long timeout
)
{
	if (console == nullptr) return -1;
	lara.s = serial;
	lara.cons = console;
	lara.cons->println(F("LARA: initializing"));

	pinMode(CELL_ON, OUTPUT);
	pinMode(NET_STAT, INPUT);
	pinMode(CELL_CTS, INPUT);
	pinMode(CELL_RTS, OUTPUT);
	/*
	 * Assert RTS (active low) before the first byte. The module boots on
	 * &K3 and only sees &K0 once it has answered, so until then this is
	 * what stops it gating its transmitter mid-message.
	 */
	digitalWrite(CELL_RTS, LOW);
	pinMode(CELL_RESET, OUTPUT);
	pinMode(CELL_PWR_DET, INPUT);

	/*
	 * Warm MCU reset: modem often stays powered. A CELL_ON pulse toggles it
	 * off. Trust PWR_DET HIGH, but if PWR_DET is LOW still probe AT before
	 * pulsing — a false LOW + pulse was observed to kill the modem
	 * (CELL_PWR_DET timeout, blank UI). See modem_boot.py.
	 */
	unsigned long t0 = millis();
	lara.s->begin(115200);

	bool already_on = digitalRead(CELL_PWR_DET) == HIGH;
	bool pulsed = false;
	if (!already_on) {
		/* Short AT probe — silence is expected; success means skip pulse. */
		if (lara_answers_at(400)) {
			already_on = true;
			lara.cons->println(
				F("LARA: AT ok with PWR_DET low (skip pulse)")
			);
		}
	}

	if (!already_on) {
		pulsed = true;
		digitalWrite(CELL_ON, HIGH);
		delay(1200);
		digitalWrite(CELL_ON, LOW);
		t0 = millis();
		while (digitalRead(CELL_PWR_DET) != HIGH) {
			if (millis() - t0 > timeout) {
				lara.cons->println(
					F("LARA: timeout on CELL_PWR_DET")
				);
				return -1;
			}
		}
	} else if (digitalRead(CELL_PWR_DET) == HIGH) {
		lara.cons->println(
			F("LARA: already powered (skip pulse / +PACSP1)")
		);
	}

	/* Cold start only: module emits +PACSP1 once after power-up. */
	if (pulsed)
		lara_wait_line("+PACSP1", timeout);

	if (!lara_sync(3)) {
		lara.cons->println(F("LARA: no AT response after power-up"));
		return -1;
	}

	int rc = lara_apply_boot_config();

	/*
	 * Provision the carrier profile, then re-apply the configuration the
	 * reboot discarded. Checked after the config above because the query
	 * needs &K0 already applied — a throttled UART truncates the reply,
	 * and an unreadable profile is deliberately left alone.
	 */
	if (lara_mno_profile_needs_change()) {
		if (lara_set_mno_profile() == 0)
			rc = lara_apply_boot_config();
		else
			rc = -1;
	}

	lara.cons->println(
		rc == 0 ? F("LARA: ready") : F("LARA: config incomplete")
	);
	return rc;
}


/*
 * Apply the session configuration. Re-runnable, because a carrier profile
 * change reboots the module and takes all of this with it.
 *
 * Mirrors boot_config_sequence() in tools/pulse_monitor/modem_boot.py.
 */
static int lara_apply_boot_config(void)
{
	/* Echo off — responses carry only what the modem has to say. */
	lara_at_set("E0", LARA_AT_TIMEOUT_MS);

	/*
	 * Mandatory config. See boot_config_sequence() in
	 * tools/pulse_monitor/modem_boot.py for the ordering constraints.
	 *
	 * &K0 first: the u-blox default is &K3 (RTS/CTS hardware flow
	 * control), but CELL_RTS is configured as an output and never driven,
	 * so the module can gate its own transmitter mid-message. That was
	 * observed as URCs arriving as a lone "R" or "+" and AT+CSQ hitting
	 * its 1 s timeout. Unsolicited output is never retried, so a throttled
	 * UART loses incoming calls outright.
	 *
	 * +CMEE=2 turns failures into readable text, and +UCALLSTAT=1 is the
	 * only thing that drives the call-phase UI. If any of these is refused
	 * the phone cannot report call state honestly, so say so rather than
	 * booting to a "Modem ready" that is not true.
	 */
	int rc = 0;
	if (lara_at_set("&K0", LARA_AT_TIMEOUT_MS) != 0)
		rc = -1;
	if (lara_at_set("+CMEE=2", LARA_AT_TIMEOUT_MS) != 0)
		rc = -1;
	if (lara_at_set("+UCALLSTAT=1", LARA_AT_TIMEOUT_MS) != 0)
		rc = -1;

	/*
	 * Advisory: none of these is required to place or receive a call.
	 * +CLIP=1 asks the network to identify callers. It is a subscription
	 * service, so a SIM or network that withholds it must not fail boot.
	 */
	lara_at_set("+CLIP=1", LARA_AT_TIMEOUT_MS);
	lara_at_set("+CLVL=6", LARA_AT_TIMEOUT_MS);
	/* Takes effect on the module's next power-up, not this one. */
	lara_at_set("+UEXTDCONF=0,1", LARA_AT_TIMEOUT_MS);

	return rc;
}


/*
 * Carrier profile the module should be provisioned with.
 *
 * LARA-R6 firmware 02.14 offers only 1 (SIM ICCID select), 90 (Global) and
 * 201 (GCF-PTCRB certification). Global applies no operator-specific IMS
 * configuration, and a unit left on it was seen to register IMS and receive
 * VoLTE calls normally while every outgoing call rang the far end and never
 * completed the answer back — the module held it in "dialing" until the
 * network timed it out, so it never opened the audio path. 1 takes the
 * operator's configuration from the SIM, which is what Global lacks.
 *
 * The value lives in the module's non-volatile memory and survives power
 * cycles, so this is provisioning rather than configuration: read it, and
 * write only when wrong. Writing unconditionally would reboot the modem on
 * every startup, dropping registration to rewrite a correct value.
 *
 * Mirrors the MNO profile helpers in tools/pulse_monitor/modem_boot.py.
 */
#define LARA_MNO_PROFILE_DESIRED 1

/* A module reboot takes far longer than any ordinary command. */
#define LARA_REBOOT_TIMEOUT_MS 40000UL

static bool lara_mno_profile_needs_change(void)
{
	char resp[24];

	if (lara_at(
		"+UMNOPROF?", "+UMNOPROF:", resp, sizeof(resp),
		LARA_AT_TIMEOUT_MS
	) != 0)
		return false;

	const char *value = resp + strlen("+UMNOPROF:");
	while (*value == ' ' || *value == '\t')
		value++;
	if (*value < '0' || *value > '9')
		return false;

	/*
	 * An unreadable profile is left alone on purpose. Writing reboots the
	 * module, so acting on a value we could not read risks rebooting,
	 * failing the query again and rebooting forever — a phone that never
	 * finishes starting up. A modem that cannot answer this has a larger
	 * problem than its carrier profile.
	 */
	int current = atoi(value);
	if (current == LARA_MNO_PROFILE_DESIRED)
		return false;

	lara.cons->print(F("LARA: MNO profile "));
	lara.cons->print(current);
	lara.cons->println(F(" — reprovisioning"));
	return true;
}


/*
 * Write the profile and reboot the module so it takes effect.
 *
 * The module refuses the write while attached to a network, hence the
 * deregister first. Returns 0 once the module answers AT again.
 */
static int lara_set_mno_profile(void)
{
	lara_at_set("+COPS=2", LARA_AT_TIMEOUT_MS);
	if (lara_at_set("+UMNOPROF=" STRINGIFY(LARA_MNO_PROFILE_DESIRED),
	                LARA_AT_TIMEOUT_MS) != 0) {
		lara.cons->println(F("LARA: MNO profile write refused"));
		return -1;
	}
	lara_at_set("+CFUN=15", LARA_AT_TIMEOUT_MS);

	lara.cons->println(F("LARA: module rebooting for MNO profile"));
	unsigned long deadline = millis() + LARA_REBOOT_TIMEOUT_MS;
	while (millis() < deadline) {
		delay(500);
		if (lara_answers_at(400))
			return 0;
	}
	lara.cons->println(F("LARA: module did not return after reboot"));
	return -1;
}


/* Advance a literal pattern matcher. True when ch completes the pattern. */
static bool lara_pat_advance(const char *pat, uint8_t *idx, char ch)
{
	if (ch == pat[*idx]) {
		(*idx)++;
		if (pat[*idx] == '\0') {
			*idx = 0;
			return true;
		}
		return false;
	}
	*idx = (ch == pat[0]) ? 1 : 0;
	return false;
}


/**
 * Parse <stat> from text after '+UCALLSTAT:'.
 * Accepts spaces: "1,7", " 1, 0", "1,6,1". Mirrors call_phase.py.
 */
static int parse_ucallstat_stat(const char *payload)
{
	while (*payload == ' ' || *payload == '\t')
		payload++;
	const char *comma = strchr(payload, ',');
	if (!comma)
		return -1;
	comma++;
	while (*comma == ' ' || *comma == '\t')
		comma++;
	if (*comma < '0' || *comma > '9')
		return -1;
	int st = *comma - '0';
	char next = comma[1];
	if (next != '\0' && next != ',' && next != ' ' && next != '\t')
		return -1;
	return st;
}


/* Empty when the network gave no number for the call now ringing. */
static char lara_clip_number[LARA_CALLER_ID_MAX_LEN + 1] = "";

/* The only characters a dialable number can contain. */
static bool lara_is_dialable(char c)
{
	return (c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#';
}

/**
 * Parse the calling number from the text after '+CLIP:'.
 *
 * Writes lara_clip_number and returns true only for a number that could
 * plausibly be one. A withheld caller sends empty quotes, and a damaged line
 * may end before the closing quote or carry arbitrary bytes inside it; both
 * leave the stored number empty.
 *
 * Damage is not hypothetical. Hardware flow control left enabled once let the
 * module gate its own transmitter mid-message and URCs arrived as a lone "R".
 * Unsolicited output is never retransmitted, so a mangled +CLIP cannot be
 * re-requested. Truncating an overlong number to fit would put a different,
 * real-looking number on the panel, which is worse than showing none because
 * the user cannot tell that it is wrong.
 *
 * Mirrors tools/pulse_monitor/caller_id.py parse_clip_payload().
 */
static bool parse_clip_number(const char *payload)
{
	lara_clip_number[0] = '\0';

	while (*payload == ' ' || *payload == '\t')
		payload++;
	if (*payload != '"')
		return false;
	payload++;

	uint8_t len = 0;
	while (payload[len] != '"') {
		if (payload[len] == '\0')
			return false;
		if (!lara_is_dialable(payload[len]))
			return false;
		if (len >= LARA_CALLER_ID_MAX_LEN)
			return false;
		len++;
	}
	if (len == 0)
		return false;

	memcpy(lara_clip_number, payload, len);
	lara_clip_number[len] = '\0';
	return true;
}


const char *lara_caller_id(void)
{
	return lara_clip_number;
}


/* Feed one modem byte into URC matchers; stash results in module pending. */
static void lara_urc_on_byte(char c)
{
	static const char *ring_pat = "RING\r";
	static const char *ring_pat_n = "RING\n";
	static const char *nocarr_r = "NO CARRIER\r";
	static const char *nocarr_n = "NO CARRIER\n";
	static const char *ucs_pat = "+UCALLSTAT:";
	static const char *clip_pat = "+CLIP:";
	static uint8_t ring_i = 0, ring_n_i = 0, noc_r_i = 0, noc_n_i = 0;
	static uint8_t ucs_i = 0, clip_i = 0;

	/*
	 * Both captured URCs run from their prefix to end of line, and the
	 * modem cannot interleave two lines on one wire, so a single buffer
	 * serves both. cap_kind records whose payload is being collected.
	 * The prefix itself is consumed by the matcher and never lands here.
	 */
	enum { CAP_NONE, CAP_UCALLSTAT, CAP_CLIP };
	static uint8_t cap_kind = CAP_NONE;
	static char cap_buf[28];
	static uint8_t cap_len = 0;

	if (cap_kind != CAP_NONE) {
		if (c == '\r' || c == '\n') {
			cap_buf[cap_len] = '\0';
			if (cap_kind == CAP_UCALLSTAT) {
				int st = parse_ucallstat_stat(cap_buf);
				if (st >= 0) {
					urc_pending_stat = st;
					if (st == 6)
						urc_pending_call_ended = true;
				}
			} else {
				parse_clip_number(cap_buf);
			}
			cap_kind = CAP_NONE;
			cap_len = 0;
		} else if (cap_len + 1 < sizeof(cap_buf)) {
			cap_buf[cap_len++] = c;
		}
		return;
	}

	if (lara_pat_advance(ring_pat, &ring_i, c)
	    || lara_pat_advance(ring_pat_n, &ring_n_i, c)) {
		urc_pending_ringing = true;
		urc_pending_last_ring = millis();
		/*
		 * Each RING is followed by its own +CLIP in the same burst, so
		 * dropping the stored number here and letting that +CLIP put it
		 * back keeps the number tied to the call now ringing. Without
		 * this, a withheld caller arriving after an identified one
		 * sends no +CLIP, and the previous caller's number would be
		 * displayed as the identity of the new one.
		 */
		lara_clip_number[0] = '\0';
	}
	if (lara_pat_advance(nocarr_r, &noc_r_i, c)
	    || lara_pat_advance(nocarr_n, &noc_n_i, c)) {
		urc_pending_call_ended = true;
	}
	if (lara_pat_advance(ucs_pat, &ucs_i, c)) {
		cap_kind = CAP_UCALLSTAT;
		cap_len = 0;
	}
	if (lara_pat_advance(clip_pat, &clip_i, c)) {
		cap_kind = CAP_CLIP;
		cap_len = 0;
	}
}


static void lara_urc_deliver(
	bool *ringing, unsigned long *last_ring_time,
	bool *call_ended, int *ucall_stat
)
{
	if (urc_pending_ringing) {
		if (ringing)
			*ringing = true;
		if (last_ring_time)
			*last_ring_time = urc_pending_last_ring;
		urc_pending_ringing = false;
	}
	if (urc_pending_call_ended) {
		if (call_ended)
			*call_ended = true;
		urc_pending_call_ended = false;
	}
	if (urc_pending_stat >= 0) {
		if (ucall_stat)
			*ucall_stat = urc_pending_stat;
		urc_pending_stat = -1;
	}
}


void lara_unsolicited(
	bool *ringing, unsigned long *last_ring_time,
	bool *call_ended, int *ucall_stat
)
{
	lara_async_service();

	/*
	 * Drain the whole modem RX buffer each loop so multi-byte URCs are not
	 * stretched across many iterations (and so NO CARRIER is not missed).
	 *
	 * The drain and the async engine read the same UART, so only one of
	 * them may hold it. While a transaction is in flight the bytes are its
	 * reply: taking them here would strand the poller until its deadline
	 * and leave the stream out of step. Nothing is lost by deferring,
	 * because lara_async_service() hands every byte it takes to the same
	 * URC matcher this loop feeds.
	 */
	while (lara.s && lara_async.state != LARA_ASYNC_WAITING
	       && lara.s->available()) {
		char c = (char)lara.s->read();
		if (lara.cons)
			lara.cons->write(c);
		lara_urc_on_byte(c);
	}
	lara_urc_deliver(ringing, last_ring_time, call_ended, ucall_stat);
}


/* Skip "<prefix>" then any blanks, returning the first value character. */
static const char *lara_payload_after(const char *line, const char *prefix)
{
	const char *p = line + strlen(prefix);
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}


lara_activity lara_status()
{
	char resp[24];

	if (!lara.s)
		return LARA_UNKNOWN;
	if (lara_at("+CPAS", "+CPAS:", resp, sizeof(resp), LARA_AT_TIMEOUT_MS)
	    != LARA_RC_OK)
		return LARA_UNKNOWN;

	/* <pas> is a single digit 0..5 (3GPP 27.007 §8.1). */
	const char *p = lara_payload_after(resp, "+CPAS:");
	if (*p < '0' || *p > '5')
		return LARA_UNKNOWN;
	return (lara_activity)*p;
}


/**
 * Parse +CLCC <stat> (3rd CSV field). Mirrors call_phase.parse_clcc_stat.
 * Returns -1 if the line is not a usable +CLCC row.
 */
/*
 * Parse one "+CLCC: <id>,<dir>,<stat>,..." entry.
 *
 * Direction is captured, not skipped. <stat> alone cannot tell an incoming
 * call from an outgoing one — state 0 is an established call either way — so
 * reading only the state left the firmware unable to recognise a call it had
 * not placed, and the bell never flashed for it.
 *
 * Returns false for anything incomplete. Truncated lines are an observed
 * condition on this UART, and a fragment accepted as a call would either
 * invent an incoming call or mask a real one.
 *
 * Mirrors parse_clcc_line() in tools/pulse_monitor/clcc.py.
 */
static bool parse_clcc_line(const char *line, int *dir_out, int *stat_out)
{
	while (*line == ' ' || *line == '\t')
		line++;
	static const char prefix[] = "+CLCC:";
	for (unsigned i = 0; prefix[i] != '\0'; i++) {
		char a = line[i];
		char b = prefix[i];
		if (a >= 'a' && a <= 'z')
			a = (char)(a - 'a' + 'A');
		if (a != b)
			return false;
	}
	const char *p = line + strlen(prefix);

	/* id */
	while (*p == ' ' || *p == '\t')
		p++;
	while (*p && *p != ',')
		p++;
	if (*p != ',')
		return false;
	p++;

	/* dir */
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p < '0' || *p > '9')
		return false;
	int dir = *p - '0';
	while (*p && *p != ',')
		p++;
	if (*p != ',')
		return false;
	p++;

	/* stat */
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p < '0' || *p > '9')
		return false;

	if (dir_out)
		*dir_out = dir;
	if (stat_out)
		*stat_out = *p - '0';
	return true;
}


/** Prefer active over alerting over dialling (call_phase.prefer_clcc_stat). */
static int prefer_clcc_stat(const int *stats, unsigned n)
{
	if (n == 0)
		return -1;
	static const int order[] = {0, 3, 2, 4, 5, 1};
	for (unsigned o = 0; o < sizeof(order) / sizeof(order[0]); o++) {
		for (unsigned i = 0; i < n; i++) {
			if (stats[i] == order[o])
				return stats[i];
		}
	}
	return stats[0];
}


/*
 * +CLCC reports one information line per call leg, so rows accumulate here
 * as they arrive. Reset by lara_clcc_poll_start().
 */
static int lara_async_clcc_stats[6];
static uint8_t lara_async_clcc_nstats;
static bool lara_async_clcc_incoming;


/* Collect one +CLCC row. Mirrors clcc_has_incoming() in clcc.py. */
static void lara_clcc_async_line(const char *line)
{
	int dir = 0;
	int st = 0;

	if (!parse_clcc_line(line, &dir, &st))
		return;
	if (dir == LARA_CLCC_DIR_INCOMING
	    && (st == LARA_CLCC_STATE_INCOMING
	        || st == LARA_CLCC_STATE_WAITING))
		lara_async_clcc_incoming = true;
	if (lara_async_clcc_nstats < (uint8_t)(sizeof(lara_async_clcc_stats)
	                                       / sizeof(lara_async_clcc_stats[0])))
		lara_async_clcc_stats[lara_async_clcc_nstats++] = st;
}


bool lara_clcc_poll_start(void)
{
	if (!lara_async_submit(
		"+CLCC", lara_clcc_async_line, LARA_AT_TIMEOUT_MS
	))
		return false;
	lara_async_clcc_nstats = 0;
	lara_async_clcc_incoming = false;
	return true;
}


bool lara_clcc_poll_take(int *stat_out, bool *incoming_out)
{
	int8_t rc;

	if (!lara_async_take(lara_clcc_async_line, &rc))
		return false;
	/*
	 * Rows seen before the final code still count, whatever that code
	 * turned out to be: a reply cut short by a timeout is still evidence
	 * of the call it described. <stat> is reported only for a reply that
	 * completed, because preferring among a partial set of legs can pick
	 * the wrong one.
	 */
	if (incoming_out)
		*incoming_out = lara_async_clcc_incoming;
	if (stat_out)
		*stat_out = (rc == LARA_RC_OK)
			? prefer_clcc_stat(
				lara_async_clcc_stats, lara_async_clcc_nstats
			)
			: -1;
	return true;
}


void lara_clcc_poll_cancel(void)
{
	/*
	 * A reply describes the session that was up when it was asked for.
	 * Once that session is torn down or replaced, applying it would
	 * repaint the previous call's state over the current one — a call just
	 * answered reported as still ringing, for instance.
	 *
	 * Ownership is checked first so tearing down a call cannot throw away
	 * the signal meter's reading: a +CSQ can be in flight when a session
	 * begins.
	 */
	if (lara_async.on_line == lara_clcc_async_line)
		lara_async_disown();
}


int lara_answer()
{
	if (!lara.s)
		return LARA_RC_TIMEOUT;
	return lara_at_call("A", LARA_ANSWER_TIMEOUT_MS);
}


int lara_hangup()
{
	if (!lara.s)
		return LARA_RC_TIMEOUT;
	return lara_at("+CHUP", nullptr, nullptr, 0, LARA_HANGUP_TIMEOUT_MS);
}


/* Async +CSQ result, valid once lara_csq_poll_take() reports it. */
static int lara_async_rssi;


static void lara_csq_async_line(const char *line)
{
	if (strncasecmp_P(line, PSTR("+CSQ:"), 5) != 0)
		return;
	/* "+CSQ: <rssi>,<ber>" — 99 already means "not known" to callers. */
	const char *p = lara_payload_after(line, "+CSQ:");
	if (*p >= '0' && *p <= '9')
		lara_async_rssi = atoi(p);
}


bool lara_csq_poll_start(void)
{
	if (!lara_async_submit("+CSQ", lara_csq_async_line, LARA_AT_TIMEOUT_MS))
		return false;
	lara_async_rssi = 99;
	return true;
}


bool lara_csq_poll_take(int *rssi_out)
{
	int8_t rc;

	if (!lara_async_take(lara_csq_async_line, &rc))
		return false;
	if (rssi_out)
		*rssi_out = (rc == LARA_RC_OK) ? lara_async_rssi : 99;
	return true;
}


int lara_signal_bars(int rssi)
{
	/* Matches tools/pulse_monitor/signal_bars.py */
	if (rssi == 99 || rssi <= 0)
		return 0;
	if (rssi >= 22)
		return 4;
	if (rssi >= 15)
		return 3;
	if (rssi >= 8)
		return 2;
	return 1;
}


int lara_dial(const char *dial_string, uint8_t buf_len)
{
	char cmd[40];
	uint8_t n = 0;

	if (!lara.s)
		return LARA_RC_TIMEOUT;
	if (dial_string == nullptr || buf_len == 0 || dial_string[0] == '\0')
		return LARA_RC_ERROR;

	/*
	 * Bounded by the caller's array so an unterminated buffer cannot be
	 * over-read; the old code scanned 32 bytes of a 30-byte dial_buf.
	 */
	cmd[n++] = 'D';
	for (uint8_t i = 0; i < buf_len && dial_string[i]; i++) {
		if ((unsigned)(n + 2) >= sizeof(cmd)) {
			if (lara.cons)
				lara.cons->println(F("LARA: dial string too long"));
			return LARA_RC_ERROR;
		}
		cmd[n++] = dial_string[i];
	}
	cmd[n++] = ';';	/* voice call */
	cmd[n] = '\0';

	if (lara.cons) {
		lara.cons->print(F("LARA: AT"));
		lara.cons->println(cmd);
	}
	int rc = lara_at_call(cmd, LARA_DIAL_TIMEOUT_MS);
	if (lara.cons) {
		lara.cons->print(F("LARA: ATD rc="));
		lara.cons->println(rc);
	}
	return rc;
}


int lara_off(unsigned long timeout)
{
	if (!lara.s)
		return -1;

	/*
	 * Wait for the final result before watching the pin: +CPWROFF can be
	 * refused (ERROR) and the pin would then never drop, burning the whole
	 * timeout with no indication of why.
	 */
	int rc = lara_at("+CPWROFF", nullptr, nullptr, 0, LARA_AT_TIMEOUT_MS);
	if (rc != LARA_RC_OK && lara.cons) {
		lara.cons->print(F("LARA: +CPWROFF rc="));
		lara.cons->println(rc);
	}

	unsigned long t0 = millis();
	while (digitalRead(CELL_PWR_DET) != LOW) {
		if (millis() - t0 > timeout) {
			lara.cons->println(
				F("LARA: timeout on CELL_PWR_DET == LOW")
			);
			lara.s->end();
			return -1;
		}
	}
	lara.s->end();
	return 0;
}
