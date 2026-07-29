#include "chg_hist.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <stdio.h>

/* Keep in sync with tools/pulse_monitor/chg_hist.py */
#ifndef CHG_HIST_MAGIC
#define CHG_HIST_MAGIC 0xC7
#endif
#ifndef CHG_HIST_MAX
#define CHG_HIST_MAX 12
#endif
#ifndef CHG_HIST_EE_BASE
#define CHG_HIST_EE_BASE 16
#endif

/* Layout: magic, head, count, then MAX * 5-byte records. */
#define CHG_HIST_OFF_MAGIC (CHG_HIST_EE_BASE + 0)
#define CHG_HIST_OFF_HEAD (CHG_HIST_EE_BASE + 1)
#define CHG_HIST_OFF_COUNT (CHG_HIST_EE_BASE + 2)
#define CHG_HIST_OFF_RECS (CHG_HIST_EE_BASE + 3)
#define CHG_HIST_REC_SIZE 5

static int last_raw = -1;

static void ee_write_u32(int addr, uint32_t v)
{
	EEPROM.update(addr + 0, (uint8_t)(v & 0xFF));
	EEPROM.update(addr + 1, (uint8_t)((v >> 8) & 0xFF));
	EEPROM.update(addr + 2, (uint8_t)((v >> 16) & 0xFF));
	EEPROM.update(addr + 3, (uint8_t)((v >> 24) & 0xFF));
}

static uint32_t ee_read_u32(int addr)
{
	uint32_t v = EEPROM.read(addr);
	v |= (uint32_t)EEPROM.read(addr + 1) << 8;
	v |= (uint32_t)EEPROM.read(addr + 2) << 16;
	v |= (uint32_t)EEPROM.read(addr + 3) << 24;
	return v;
}

static void chg_hist_reset(void)
{
	EEPROM.update(CHG_HIST_OFF_MAGIC, CHG_HIST_MAGIC);
	EEPROM.update(CHG_HIST_OFF_HEAD, 0);
	EEPROM.update(CHG_HIST_OFF_COUNT, 0);
}

void chg_hist_boot_dump(HardwareSerial *cons)
{
	if (cons == nullptr)
		return;

	if (EEPROM.read(CHG_HIST_OFF_MAGIC) != CHG_HIST_MAGIC)
		chg_hist_reset();

	uint8_t count = EEPROM.read(CHG_HIST_OFF_COUNT);
	uint8_t head = EEPROM.read(CHG_HIST_OFF_HEAD);
	if (count > CHG_HIST_MAX)
		count = 0;
	if (head >= CHG_HIST_MAX)
		head = 0;

	cons->print(F("CHG_HIST n="));
	cons->print(count);
	cons->println(F(" (survives USB reset; empty = no CHG edge on battery)"));

	if (count == 0)
		return;

	/* Oldest first. */
	uint8_t start = (uint8_t)((head + CHG_HIST_MAX - count) % CHG_HIST_MAX);
	for (uint8_t i = 0; i < count; i++) {
		uint8_t idx = (uint8_t)((start + i) % CHG_HIST_MAX);
		int addr = CHG_HIST_OFF_RECS + (int)idx * CHG_HIST_REC_SIZE;
		uint32_t ms = ee_read_u32(addr);
		uint8_t raw = EEPROM.read(addr + 4);
		char line[40];
		snprintf_P(
			line, sizeof(line), PSTR("CHG_HIST t=%lu raw=%u"),
			(unsigned long)ms, (unsigned)raw
		);
		cons->println(line);
	}
}

void chg_hist_note_raw(uint8_t raw, unsigned long ms)
{
	raw = (uint8_t)(raw ? 1 : 0);
	if (last_raw < 0) {
		last_raw = (int)raw;
		return;
	}
	if ((int)raw == last_raw)
		return;
	last_raw = (int)raw;

	if (EEPROM.read(CHG_HIST_OFF_MAGIC) != CHG_HIST_MAGIC)
		chg_hist_reset();

	uint8_t head = EEPROM.read(CHG_HIST_OFF_HEAD);
	uint8_t count = EEPROM.read(CHG_HIST_OFF_COUNT);
	if (head >= CHG_HIST_MAX)
		head = 0;
	if (count > CHG_HIST_MAX)
		count = 0;

	int addr = CHG_HIST_OFF_RECS + (int)head * CHG_HIST_REC_SIZE;
	ee_write_u32(addr, (uint32_t)ms);
	EEPROM.update(addr + 4, raw);

	head = (uint8_t)((head + 1) % CHG_HIST_MAX);
	if (count < CHG_HIST_MAX)
		count++;
	EEPROM.update(CHG_HIST_OFF_HEAD, head);
	EEPROM.update(CHG_HIST_OFF_COUNT, count);
}
