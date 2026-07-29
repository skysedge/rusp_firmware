#ifndef RUSP_CHG_HIST_H
#define RUSP_CHG_HIST_H

#include <HardwareSerial.h>
#include <stdint.h>

/*
 * Non-volatile CHG_STAT edge log.
 * USB unplug kills Serial; USB replug resets the MCU — so plug edges never
 * appear as PINCHG on a live console. Edges while on battery are stored here
 * and dumped at the next boot (see tools/pulse_monitor/chg_hist.py).
 */
void chg_hist_boot_dump(HardwareSerial *cons);
void chg_hist_note_raw(uint8_t raw, unsigned long ms);

#endif
