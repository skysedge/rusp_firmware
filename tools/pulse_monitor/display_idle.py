"""OLED idle timeout / wake rules.

Display sleeps after OLED_IDLE_TIMEOUT_MS with no input. Any button press
wakes it. Call start (dial/answer) and C-delete require the display to have
been on; hangup always runs.
"""

from __future__ import annotations

OLED_IDLE_TIMEOUT_MS = 20_000


def should_sleep_display(
	*,
	display_awake: bool,
	now_ms: int,
	last_activity_ms: int,
	timeout_ms: int = OLED_IDLE_TIMEOUT_MS,
	charging: bool = False,
) -> bool:
	"""True when an awake display has been idle long enough to blank.

	While charging, never sleep — the docked phone should keep the UI lit.
	"""
	if not display_awake or charging:
		return False
	return (now_ms - last_activity_ms) >= timeout_ms


def clear_should_delete(*, display_awake: bool) -> bool:
	"""C button deletes dial text only when the display is already on."""
	return display_awake
