#!/usr/bin/env python3
"""Capture the firmware console, surviving USB re-enumeration.

Distinct from pulse_monitor.py, which parses dial pulses into sessions. This
records the console verbatim, which is what hardware debugging of call flow
needs.

Runs unbounded by default. The capture this replaces carried a hardcoded
2400-second deadline and exited silently on reaching it, so a long hardware
test produced a log that simply stopped part-way with nothing to say why. A
bounded window is now something a caller opts into, and every terminal state
names itself.

Serial access is injected rather than imported so the retry and reconnect
behaviour can be tested without hardware; see test_console_capture.py.
"""

from __future__ import annotations

import argparse
import enum
import sys
import time
from typing import Callable, Optional

DEFAULT_BAUD = 115200
RECONNECT_POLL_SECONDS = 0.5
READ_SIZE = 4096


class CaptureResult(enum.Enum):
	"""Why capture stopped. Every value must be explicable by describe_end."""

	LIMIT_REACHED = "limit_reached"
	ITERATIONS_EXHAUSTED = "iterations_exhausted"
	INTERRUPTED = "interrupted"


def describe_end(result: CaptureResult) -> str:
	"""Human-readable reason a capture ended.

	A log that stops mid-session is ambiguous between a finished window, a
	crash, and an unplugged board. Naming the terminal state removes the
	guesswork when reading the log later.
	"""
	reasons = {
		CaptureResult.LIMIT_REACHED:
			"requested time limit reached",
		CaptureResult.ITERATIONS_EXHAUSTED:
			"iteration budget exhausted (test harness only)",
		CaptureResult.INTERRUPTED:
			"interrupted by the operator",
	}
	return reasons[result]


def capture(
	find_port: Callable[[], Optional[str]],
	open_port: Callable[[str, int], object],
	write: Callable[[str], None],
	sleep: Callable[[float], None],
	now: Callable[[], float],
	limit_seconds: Optional[float] = None,
	max_iterations: Optional[int] = None,
	baud: int = DEFAULT_BAUD,
	read_size: int = READ_SIZE,
	poll_seconds: float = RECONNECT_POLL_SECONDS,
) -> CaptureResult:
	"""Stream the console until a limit is hit or the operator interrupts.

	limit_seconds of None means run indefinitely, which is the intended
	behaviour for hardware debugging: the operator decides when the test is
	over, not the tool.

	max_iterations exists so tests can bound the loop without a wall clock.
	It has no production use.

	A detach or a failed open is a wait-and-retry, never a return. The board
	re-enumerates under a new device name when it resets, and a capture that
	died on that would miss precisely the reboot it was started to observe.
	"""
	started = now()
	iterations = 0
	port = None
	attached = None

	while True:
		if max_iterations is not None and iterations >= max_iterations:
			return CaptureResult.ITERATIONS_EXHAUSTED
		iterations += 1

		# Sampled unconditionally rather than inside the limit check, so
		# the clock advances on every pass including while waiting for a
		# port. Guarding the call behind the limit test short-circuits it
		# away in the unbounded case and freezes elapsed time.
		elapsed = now() - started
		if limit_seconds is not None and elapsed >= limit_seconds:
			return CaptureResult.LIMIT_REACHED

		if port is None:
			name = find_port()
			if name is None:
				sleep(poll_seconds)
				continue
			try:
				port = open_port(name, baud)
			except OSError:
				# The device node can exist before it is openable, and
				# something else may still hold it after a reset.
				port = None
				sleep(poll_seconds)
				continue
			if name != attached:
				attached = name
				write("\n=== ATTACHED %s ===\n" % name)

		try:
			data = port.read(read_size)
		except OSError:
			write("\n=== DETACHED ===\n")
			try:
				port.close()
			except OSError:
				pass
			port = None
			continue

		if data:
			# Replacement rather than strict: line noise is normal on a
			# serial link and must not end the capture.
			write(data.decode("utf-8", "replace"))


def main(argv: list[str] | None = None) -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--port", default=None, help="Serial device")
	parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
	parser.add_argument(
		"--seconds", type=float, default=None,
		help="Stop after this long. Omit to run until interrupted."
	)
	args = parser.parse_args(argv)

	import serial
	from pulse_monitor import default_port

	def find_port() -> Optional[str]:
		return args.port or default_port()

	def open_port(name: str, baud: int) -> object:
		return serial.Serial(name, baud, timeout=0.2)

	def write(text: str) -> None:
		sys.stdout.write(text)
		sys.stdout.flush()

	try:
		result = capture(
			find_port=find_port,
			open_port=open_port,
			write=write,
			sleep=time.sleep,
			now=time.monotonic,
			limit_seconds=args.seconds,
			baud=args.baud,
		)
	except KeyboardInterrupt:
		result = CaptureResult.INTERRUPTED

	write("\n=== CAPTURE ENDED: %s ===\n" % describe_end(result))
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
