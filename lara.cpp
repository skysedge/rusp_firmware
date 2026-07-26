#include <Arduino.h>
#include <HardwareSerial.h>
#include <stdlib.h>
#include <string.h>

#include "lara.h"

static struct lara_state lara;

/*
 * URCs that arrive while expect()/AT helpers are reading must not be
 * discarded. Stash events here; lara_unsolicited() delivers them.
 */
static bool urc_pending_ringing = false;
static unsigned long urc_pending_last_ring = 0;
static bool urc_pending_call_ended = false;
static int urc_pending_stat = -1;

static void lara_urc_on_byte(char c);


static char lara_read_byte(void)
{
	char c = (char)lara.s->read();
	lara_urc_on_byte(c);
	return c;
}


/* expect lara to send a specific string (every byte still feeds URC parser) */
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
			char c = lara_read_byte();
			if (c == str[i])
				i += 1;
			else
				i = 0;
		}
	}
	return 0;
}


/* expect lara to send one of several specific strings
 * @arg indices: array of unsigned zeros that is used to track matching
 * returns 0 if error or the index of the matched string, starting at 1
 */
static unsigned multiexpect(
	unsigned str_count, unsigned *indices, char **strs,
	unsigned long timeout
){
	unsigned long t0 = millis();
	do {
		if (lara.s->available()) {
			char c = lara_read_byte();
			for (unsigned i = 0; i < str_count; i++) {
				if (!strs[i][indices[i]])
					return 1 + i;
				if (c == strs[i][indices[i]])
					indices[i] += 1;
				else
					indices[i] = 0;
			}
		}
	} while (millis() - t0 < timeout);
	lara.cons->println("LARA: timeout while multiexpecting {");
	for (unsigned i = 0; i < str_count; i++) {
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
	/* datasheet tells us to delay >20 ms after receiving a final result */
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

	/*
	 * Warm MCU reset: modem often stays powered. A CELL_ON pulse toggles it
	 * off. Trust PWR_DET HIGH, but if PWR_DET is LOW still probe AT before
	 * pulsing — a false LOW + pulse was observed to kill the modem
	 * (CELL_PWR_DET timeout, blank UI). See modem_boot.py.
	 */
	unsigned long t0 = millis();
	lara.s->begin(115200);
	while (!*lara.s) {
		if (millis() - t0 > timeout) {
			lara.cons->println(
				"LARA: timeout waiting for serial to begin"
			);
			return -1;
		}
	}

	bool already_on = digitalRead(CELL_PWR_DET) == HIGH;
	bool pulsed = false;
	if (!already_on) {
		/* Short AT probe — ignore errors; success means skip pulse. */
		if (lara_at_set("", 400) == 0) {
			already_on = true;
			lara.cons->println(
				"LARA: AT ok with PWR_DET low (skip CELL_ON pulse)"
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
					"LARA: timeout waiting for CELL_PWR_DET"
				);
				return -1;
			}
		}
	} else if (digitalRead(CELL_PWR_DET) == HIGH) {
		lara.cons->println(
			"LARA: already powered (skip CELL_ON / +PACSP1 wait)"
		);
	}

	/* Cold start only: module emits +PACSP1 once after power-up. */
	if (pulsed)
		expect("+PACSP1\r", timeout);

	/* enable verbose errors */
	lara_at_set("+CMEE=2", 1000);
	/* maximum call volume */
	lara_at_set("+CLVL=6", 1000);
	/* enable codec autoconfiguration on next boot */
	lara_at_set("+UEXTDCONF=0,1", 1000);
	/* Voice call status URCs (+UCALLSTAT: <id>,<stat>) */
	lara_at_set("+UCALLSTAT=1", 1000);

	lara.cons->println("LARA: ready");
	return 0;
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


/* Feed one modem byte into URC matchers; stash results in module pending. */
static void lara_urc_on_byte(char c)
{
	static const char *ring_pat = "RING\r";
	static const char *ring_pat_n = "RING\n";
	static const char *nocarr_r = "NO CARRIER\r";
	static const char *nocarr_n = "NO CARRIER\n";
	static const char *ucs_pat = "+UCALLSTAT:";
	static uint8_t ring_i = 0, ring_n_i = 0, noc_r_i = 0, noc_n_i = 0;
	static uint8_t ucs_i = 0;
	static bool ucs_capturing = false;
	static char ucs_buf[24];
	static uint8_t ucs_len = 0;

	if (ucs_capturing) {
		if (c == '\r' || c == '\n') {
			ucs_buf[ucs_len] = '\0';
			ucs_capturing = false;
			int st = parse_ucallstat_stat(ucs_buf);
			if (st >= 0) {
				urc_pending_stat = st;
				if (st == 6)
					urc_pending_call_ended = true;
			}
			ucs_len = 0;
		} else if (ucs_len + 1 < sizeof(ucs_buf)) {
			ucs_buf[ucs_len++] = c;
		}
		return;
	}

	if (lara_pat_advance(ring_pat, &ring_i, c)
	    || lara_pat_advance(ring_pat_n, &ring_n_i, c)) {
		urc_pending_ringing = true;
		urc_pending_last_ring = millis();
	}
	if (lara_pat_advance(nocarr_r, &noc_r_i, c)
	    || lara_pat_advance(nocarr_n, &noc_n_i, c)) {
		urc_pending_call_ended = true;
	}
	if (lara_pat_advance(ucs_pat, &ucs_i, c)) {
		ucs_capturing = true;
		ucs_len = 0;
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
	/*
	 * Drain the whole modem RX buffer each loop so multi-byte URCs are not
	 * stretched across many iterations (and so NO CARRIER is not missed).
	 */
	while (lara.s && lara.s->available()) {
		char c = (char)lara.s->read();
		if (lara.cons)
			lara.cons->write(c);
		lara_urc_on_byte(c);
	}
	lara_urc_deliver(ringing, last_ring_time, call_ended, ucall_stat);
}


lara_activity lara_status()
{
	lara.s->write("AT+CPAS\r");
	lara.s->flush();
	if (expect("+CPAS: ", 1000) != 0)
		return LARA_UNKNOWN;
	unsigned long t0 = millis();
	while (!lara.s->available()) {
		if (millis() - t0 > 1000)
			return LARA_UNKNOWN;
	}
	lara_activity ret = (lara_activity)lara_read_byte();
	t0 = millis();
	while (!lara.s->available()) {
		if (millis() - t0 > 200)
			break;
	}
	if (lara.s->available())
		lara_read_byte(); /* clear trailing \r (or leftover) */
	return ret;
}


/**
 * Parse +CLCC <stat> (3rd CSV field). Mirrors call_phase.parse_clcc_stat.
 * Returns -1 if the line is not a usable +CLCC row.
 */
static int parse_clcc_stat_line(const char *line)
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
			return -1;
	}
	const char *p = line + strlen(prefix);
	while (*p == ' ' || *p == '\t')
		p++;
	/* id */
	while (*p && *p != ',')
		p++;
	if (*p != ',')
		return -1;
	p++;
	/* dir */
	while (*p && *p != ',')
		p++;
	if (*p != ',')
		return -1;
	p++;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p < '0' || *p > '9')
		return -1;
	return *p - '0';
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


int lara_clcc_stat(void)
{
	if (!lara.s)
		return -1;

	lara.s->write("AT+CLCC\r");
	lara.s->flush();

	int stats[6];
	unsigned nstats = 0;
	char line[96];
	uint8_t len = 0;
	unsigned long t0 = millis();
	bool got_ok = false;

	while (!got_ok && (millis() - t0) < 1000) {
		if (!lara.s->available())
			continue;
		char c = lara_read_byte();
		if (lara.cons)
			lara.cons->write(c);
		if (c == '\r' || c == '\n') {
			if (len == 0)
				continue;
			line[len] = '\0';
			len = 0;
			if (strcmp(line, "OK") == 0) {
				got_ok = true;
				break;
			}
			if (strcmp(line, "ERROR") == 0)
				return -1;
			int st = parse_clcc_stat_line(line);
			if (st >= 0 && nstats < (unsigned)(sizeof(stats) / sizeof(stats[0])))
				stats[nstats++] = st;
		} else if (len + 1 < sizeof(line)) {
			line[len++] = c;
		} else {
			len = 0; /* overflow — resync on next newline */
		}
	}

	if (!got_ok)
		return -1;
	return prefer_clcc_stat(stats, nstats);
}


int lara_answer()
{
	lara.s->write("ATA\r");
	lara.s->flush();
	/* TODO: error handling */
	return 0;
}


int lara_hangup()
{
	lara.s->write("AT+CHUP\r");
	lara.s->flush();
	return expect("OK\r", 1000);
}


int lara_signal_rssi(void)
{
	if (!lara.s)
		return 99;
	lara.s->write("AT+CSQ\r");
	lara.s->flush();
	if (expect("+CSQ: ", 1000) != 0)
		return 99;

	char digits[4];
	uint8_t n = 0;
	unsigned long t0 = millis();
	while (n < 3 && (millis() - t0) < 500) {
		if (!lara.s->available())
			continue;
		char c = lara_read_byte();
		if (c >= '0' && c <= '9')
			digits[n++] = c;
		else
			break;
	}
	digits[n] = '\0';

	/* Discard rest of the CSQ line, then OK. */
	t0 = millis();
	while ((millis() - t0) < 300) {
		if (!lara.s->available())
			continue;
		char c = lara_read_byte();
		if (c == '\n')
			break;
	}
	expect("OK\r", 500);

	if (n == 0)
		return 99;
	return atoi(digits);
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


int lara_dial(char *dial_string)
{
	if (lara.cons) {
		lara.cons->print("LARA: ATD");
		if (dial_string) {
			for (unsigned i = 0; i < 32; i++) {
				if (!dial_string[i]) break;
				lara.cons->write(dial_string[i]);
			}
		}
		lara.cons->println(";");
	}
	lara.s->write("ATD");
	/* Stop at 32 if the string has no null terminator. */
	if (dial_string) {
		for (unsigned i = 0; i < 32; i++) {
			if (!dial_string[i]) break;
			lara.s->write(dial_string[i]);
		}
	}
	lara.s->write(";\r");
	lara.s->flush();
	int rc = expect("OK\r", 1000);
	if (lara.cons) {
		lara.cons->print("LARA: ATD result=");
		lara.cons->println(rc == 0 ? "OK" : "TIMEOUT/FAIL");
	}
	return rc;
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
