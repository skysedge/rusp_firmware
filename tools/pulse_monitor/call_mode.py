"""Call-type mode resolution for the 1P3T local/alt/nonlocal switch.

When DISABLE_CALL_TYPE_MODES is enabled in firmware, physical switch position
is ignored for dialing behavior: digits accumulate as typed, and the call
button (hook) dials exactly dial_buf — no local prepend, no address book.
"""

from __future__ import annotations

from enum import IntEnum


class CallMode(IntEnum):
	NONLOCAL = 0
	LOCAL = 1
	ALT = 2


def resolve_call_mode(
	alt_active: bool,
	local_active: bool,
	*,
	disable_call_type_modes: bool,
) -> CallMode:
	"""Map switch inputs to effective dialing mode.

	alt_active / local_active mean the corresponding pin is asserted (LOW on
	hardware). ALT takes priority over LOCAL when both are somehow true.
	"""
	if disable_call_type_modes:
		return CallMode.NONLOCAL
	if alt_active:
		return CallMode.ALT
	if local_active:
		return CallMode.LOCAL
	return CallMode.NONLOCAL


def hook_drives_call_actions(mode: CallMode) -> bool:
	"""True when hook answers/hangs/dials; false when reserved for speed-dial."""
	return mode != CallMode.ALT


def allow_local_prepend(mode: CallMode) -> bool:
	return mode == CallMode.LOCAL


def allow_alt_contacts(mode: CallMode) -> bool:
	return mode == CallMode.ALT
