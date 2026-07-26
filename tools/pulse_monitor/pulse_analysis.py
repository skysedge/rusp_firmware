"""Pure analysis helpers for the rotary pulse debounce debug protocol.

Matches production timing from rusp_firmware.ino:
- ROTARY_DEBOUNCE_MS uses strict greater-than
- PULSE_FUDGE and digit mapping match pulse2ascii()
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional

PULSE_FUDGE = 1
ROTARY_DEBOUNCE_MS = 30
PULSES_DONE_MS = 200

_KNOWN_TYPES = frozenset(
	{"READY", "HALL", "RAW", "ACCEPTED", "REJECTED", "DIGIT", "OVERFLOW"}
)


def pulse_to_ascii(pulse_count: int) -> str:
	"""Map a raw accepted pulse count to a dial digit, matching production."""
	adjusted = pulse_count - PULSE_FUDGE
	if adjusted == 10:
		return "0"
	if 1 <= adjusted <= 9:
		return str(adjusted)
	return "?"


def replay_debounce(
	edge_times_ms: List[int],
	debounce_ms: int = ROTARY_DEBOUNCE_MS,
) -> List[int]:
	"""Replay production-style debounce on session-relative edge times.

	The first edge is always accepted (models a fresh dial after hall with no
	prior accepted pulse in-session). Later edges are accepted only when
	``t - last_accepted > debounce_ms``, matching the firmware comparison.
	"""
	accepted: List[int] = []
	for t in edge_times_ms:
		if not accepted or (t - accepted[-1]) > debounce_ms:
			accepted.append(t)
	return accepted


def _parse_value(key: str, raw: str) -> Any:
	# Keep dial digit as a character even when it is '0'..'9'.
	if key == "ascii":
		return raw
	if "," in raw and all(part.lstrip("-").isdigit() for part in raw.split(",")):
		return [int(part) for part in raw.split(",") if part != ""]
	if raw.lstrip("-").isdigit():
		return int(raw)
	return raw


def parse_line(line: str) -> Optional[Dict[str, Any]]:
	"""Parse one protocol line into a dict, or None if not a known event."""
	text = line.strip()
	if not text:
		return None
	parts = text.split()
	event_type = parts[0]
	if event_type not in _KNOWN_TYPES:
		return None
	event: Dict[str, Any] = {"type": event_type}
	for part in parts[1:]:
		if "=" not in part:
			continue
		key, value = part.split("=", 1)
		event[key] = _parse_value(key, value)
	return event


class DialSession:
	"""Accumulate one dial (hall → edges → digit) and detect bounce clusters."""

	def __init__(
		self,
		debounce_ms: int = ROTARY_DEBOUNCE_MS,
		done_ms: int = PULSES_DONE_MS,
	) -> None:
		self.debounce_ms = debounce_ms
		self.done_ms = done_ms
		self.raw_edge_count = 0
		self.accepted_count = 0
		self.rejected_count = 0
		self.bounce_clusters: List[Dict[str, int]] = []
		self._cluster_start_us: Optional[int] = None
		self._cluster_last_us: Optional[int] = None
		self._cluster_edges = 0
		self._last_raw_us: Optional[int] = None

	def reset(self) -> None:
		self.raw_edge_count = 0
		self.accepted_count = 0
		self.rejected_count = 0
		self.bounce_clusters = []
		self._cluster_start_us = None
		self._cluster_last_us = None
		self._cluster_edges = 0
		self._last_raw_us = None

	def _flush_cluster(self) -> None:
		if (
			self._cluster_edges >= 2
			and self._cluster_start_us is not None
			and self._cluster_last_us is not None
		):
			self.bounce_clusters.append(
				{
					"edges": self._cluster_edges,
					"span_us": self._cluster_last_us - self._cluster_start_us,
				}
			)
		self._cluster_start_us = None
		self._cluster_last_us = None
		self._cluster_edges = 0

	def _note_raw(self, t_us: int) -> None:
		debounce_us = self.debounce_ms * 1000
		if self._last_raw_us is None:
			self._cluster_start_us = t_us
			self._cluster_last_us = t_us
			self._cluster_edges = 1
		else:
			dt_us = t_us - self._last_raw_us
			if dt_us < debounce_us:
				if self._cluster_edges == 0:
					self._cluster_start_us = self._last_raw_us
					self._cluster_edges = 1
				self._cluster_last_us = t_us
				self._cluster_edges += 1
			else:
				self._flush_cluster()
				self._cluster_start_us = t_us
				self._cluster_last_us = t_us
				self._cluster_edges = 1
		self._last_raw_us = t_us
		self.raw_edge_count += 1

	def handle(self, event: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
		"""Update session state from a parsed event. Returns DIGIT events."""
		if event is None:
			return None
		event_type = event["type"]
		if event_type == "HALL":
			self.reset()
			return event
		if event_type == "RAW":
			self._note_raw(int(event["t_us"]))
			return event
		if event_type == "ACCEPTED":
			self.accepted_count = int(event.get("n", self.accepted_count + 1))
			return event
		if event_type == "REJECTED":
			self.rejected_count += 1
			return event
		if event_type == "DIGIT":
			self._flush_cluster()
			return event
		return event
