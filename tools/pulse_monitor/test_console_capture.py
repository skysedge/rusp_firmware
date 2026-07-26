"""Tests for console_capture.

The capture this replaces exited silently after a hardcoded 40 minutes and
took its reason with it, so a hardware test that ran long simply produced a
log that stopped mid-session. These tests pin the two properties that failure
depended on: capture runs unbounded unless a limit is asked for, and a limit
that is asked for is honoured and announced.
"""

import unittest

from console_capture import CaptureResult, capture, describe_end


class FakePort:
	"""Serial stand-in. Raises OSError on the reads listed in fail_at."""

	def __init__(self, chunks, fail_at=()):
		self.chunks = list(chunks)
		self.fail_at = set(fail_at)
		self.reads = 0
		self.closed = False

	def read(self, _size):
		index = self.reads
		self.reads += 1
		if index in self.fail_at:
			raise OSError("device detached")
		if self.chunks:
			return self.chunks.pop(0)
		return b""

	def close(self):
		self.closed = True


class Harness:
	"""Drives capture() with a virtual clock and scripted port availability."""

	def __init__(self, ports, tick=0.1):
		# ports: list of (name, FakePort) or None for "no port present"
		self.ports = list(ports)
		self.tick = tick
		self.time = 0.0
		self.written = []
		self.opened = []
		self.slept = []

	def find_port(self):
		"""None entries model "board not plugged in yet" and are consumed,
		so a scripted absence advances instead of repeating forever."""
		if not self.ports:
			return None
		if self.ports[0] is None:
			self.ports.pop(0)
			return None
		return self.ports[0][0]

	def open_port(self, name, baud):
		entry = self.ports.pop(0)
		self.opened.append((name, baud))
		return entry[1]

	def write(self, text):
		self.written.append(text)

	def sleep(self, seconds):
		self.slept.append(seconds)
		self.time += seconds

	def now(self):
		self.time += self.tick
		return self.time

	def text(self):
		return "".join(self.written)


class LimitTests(unittest.TestCase):
	def test_no_limit_runs_until_the_port_list_is_exhausted(self):
		"""Guards the regression that motivated this module.

		The old capture stopped at a hardcoded 2400 seconds. With no limit
		requested, capture must not stop on a clock at all -- if a deadline
		leaks back in, the virtual clock here races past 2400 within a few
		hundred iterations and the payload written after that point goes
		missing, failing the content assertion below.
		"""
		port = FakePort([b"early"] + [b""] * 50000 + [b"late"])
		harness = Harness([("/dev/tty0", port)], tick=1.0)

		result = capture(
			find_port=harness.find_port,
			open_port=harness.open_port,
			write=harness.write,
			sleep=harness.sleep,
			now=harness.now,
			limit_seconds=None,
			max_iterations=60000,
		)

		self.assertGreater(harness.time, 2400)
		self.assertIn("early", harness.text())
		self.assertIn("late", harness.text())
		self.assertEqual(result, CaptureResult.ITERATIONS_EXHAUSTED)

	def test_explicit_limit_stops_capture(self):
		"""A caller that asks for a bounded window must get one."""
		port = FakePort([b"x"] * 10000)
		harness = Harness([("/dev/tty0", port)], tick=1.0)

		result = capture(
			find_port=harness.find_port,
			open_port=harness.open_port,
			write=harness.write,
			sleep=harness.sleep,
			now=harness.now,
			limit_seconds=5.0,
			max_iterations=10000,
		)

		self.assertEqual(result, CaptureResult.LIMIT_REACHED)
		self.assertLess(harness.time, 20)

	def test_end_reason_is_reported_for_every_result(self):
		"""The old script ended with no usable reason.

		Every terminal state must describe itself, so a truncated log can be
		told apart from a completed one without reading the source.
		"""
		for result in CaptureResult:
			with self.subTest(result=result):
				self.assertTrue(describe_end(result).strip())


class ReconnectTests(unittest.TestCase):
	def test_detach_reopens_rather_than_exiting(self):
		"""A USB re-enumeration must not end the capture.

		If the OSError propagates or breaks the loop, nothing after the
		detach is captured and "after" is absent from the output.
		"""
		first = FakePort([b"before"], fail_at=[1])
		second = FakePort([b"after"])
		harness = Harness([("/dev/tty0", first), ("/dev/tty1", second)])

		capture(
			find_port=harness.find_port,
			open_port=harness.open_port,
			write=harness.write,
			sleep=harness.sleep,
			now=harness.now,
			limit_seconds=None,
			max_iterations=40,
		)

		self.assertIn("before", harness.text())
		self.assertIn("after", harness.text())
		self.assertEqual(len(harness.opened), 2)
		self.assertTrue(first.closed)

	def test_absent_port_waits_instead_of_failing(self):
		"""Starting before the board is plugged in must be survivable."""
		port = FakePort([b"hello"])
		harness = Harness([None, None, ("/dev/tty0", port)])

		capture(
			find_port=harness.find_port,
			open_port=harness.open_port,
			write=harness.write,
			sleep=harness.sleep,
			now=harness.now,
			limit_seconds=None,
			max_iterations=40,
		)

		self.assertIn("hello", harness.text())
		self.assertGreaterEqual(len(harness.slept), 2)

	def test_open_failure_is_retried_not_fatal(self):
		"""The port node can exist before it is openable."""
		attempts = []

		def open_port(name, baud):
			attempts.append(name)
			if len(attempts) < 3:
				raise OSError("busy")
			return FakePort([b"ok"])

		harness = Harness([])
		harness.find_port = lambda: "/dev/tty0"
		harness.open_port = open_port

		capture(
			find_port=harness.find_port,
			open_port=harness.open_port,
			write=harness.write,
			sleep=harness.sleep,
			now=harness.now,
			limit_seconds=None,
			max_iterations=40,
		)

		self.assertGreaterEqual(len(attempts), 3)
		self.assertIn("ok", harness.text())


class DecodeTests(unittest.TestCase):
	def test_invalid_utf8_is_replaced_not_raised(self):
		"""Line noise on a serial link is normal and must not kill capture.

		A strict decode raises UnicodeDecodeError here and nothing is
		written at all.
		"""
		port = FakePort([b"a\xffb"])
		harness = Harness([("/dev/tty0", port)])

		capture(
			find_port=harness.find_port,
			open_port=harness.open_port,
			write=harness.write,
			sleep=harness.sleep,
			now=harness.now,
			limit_seconds=None,
			max_iterations=10,
		)

		text = harness.text()
		self.assertIn("a", text)
		self.assertIn("b", text)

	def test_attach_is_announced_once_per_port_name(self):
		"""Re-announcing an unchanged port would bury the log in banners."""
		port = FakePort([b"x"] * 5)
		harness = Harness([("/dev/tty0", port)])

		capture(
			find_port=harness.find_port,
			open_port=harness.open_port,
			write=harness.write,
			sleep=harness.sleep,
			now=harness.now,
			limit_seconds=None,
			max_iterations=10,
		)

		self.assertEqual(harness.text().count("/dev/tty0"), 1)


if __name__ == "__main__":
	unittest.main()
