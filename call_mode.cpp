#include <Arduino.h>

#include "call_mode.h"
#include "pins.h"

const char *call_mode_name(CallMode mode)
{
	switch (mode) {
	case CALL_MODE_LOCAL:
		return "LOCAL";
	case CALL_MODE_ALT:
		return "ALT";
	case CALL_MODE_NONLOCAL:
		return "NONLOCAL";
	}
	return "?";
}

CallMode physical_call_mode(void)
{
	if (digitalRead(SW_ALT) == LOW)
		return CALL_MODE_ALT;
	if (digitalRead(SW_LOCAL) == LOW)
		return CALL_MODE_LOCAL;
	return CALL_MODE_NONLOCAL;
}

CallMode effective_call_mode(bool disable_call_type_modes)
{
	if (disable_call_type_modes)
		return CALL_MODE_NONLOCAL;
	return physical_call_mode();
}
