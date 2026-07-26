#ifndef RUSP_CALL_MODE_H
#define RUSP_CALL_MODE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Effective dialing mode for the 1P3T local / alt / nonlocal switch.
 * Mirrored by tools/pulse_monitor/call_mode.py.
 */
enum CallMode : uint8_t {
	CALL_MODE_NONLOCAL = 0,
	CALL_MODE_LOCAL = 1,
	CALL_MODE_ALT = 2,
};

const char *call_mode_name(CallMode mode);
CallMode physical_call_mode(void);
CallMode effective_call_mode(bool disable_call_type_modes);

#endif
