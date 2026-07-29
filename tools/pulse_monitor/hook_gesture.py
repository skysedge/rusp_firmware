"""Call-button gesture: act once on release, not on hold duration.

Why: a 1s hold meant dialing on hold, then keeping the button down could
re-trigger hangup. Release-edge + debounce fires exactly one action per press.
"""

from __future__ import annotations

HOOK_DEBOUNCE_MS = 30


def hook_fires_on_release(
	*,
	hook_pressed: bool,
	pin_released: bool,
	held_ms: int,
	debounce_ms: int = HOOK_DEBOUNCE_MS,
) -> bool:
	"""True when this loop iteration should run dial/answer/hangup once."""
	return hook_pressed and pin_released and held_ms >= debounce_ms
