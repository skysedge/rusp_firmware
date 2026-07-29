#ifndef RUSP_PIN_SCAN_H
#define RUSP_PIN_SCAN_H

/*
 * Sample named board pins and log levels. Used to find which GPIO moves
 * when USB/charge is plugged (same connector on the RUSP).
 */
void pin_scan_init(void);
void pin_scan_service(unsigned long now_ms);

#endif
