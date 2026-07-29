#!/usr/bin/env python3
"""Serial host for the pulse_debounce_debug MCU sketch.

Usage:
  python3 pulse_monitor.py --port /dev/cu.usbmodem1101
  python3 pulse_monitor.py --port /dev/ttyACM0 --baud 9600
"""

from __future__ import annotations

import argparse
import sys
import time

from pulse_analysis import DialSession, parse_line


def default_port() -> str:
	import glob

	mac = sorted(glob.glob("/dev/cu.usbmodem*"))
	if mac:
		return mac[0]
	linux = sorted(glob.glob("/dev/ttyACM*"))
	if linux:
		return linux[0]
	return "/dev/cu.usbmodem1101"


def format_event(event: dict, session: DialSession) -> str:
	event_type = event["type"]
	if event_type == "READY":
		return (
			f"READY board={event.get('board')} baud={event.get('baud')} "
			f"debounce_ms={event.get('debounce_ms')} done_ms={event.get('done_ms')}"
		)
	if event_type == "HALL":
		return f"HALL t_ms={event.get('t_ms')}  (dial started)"
	if event_type == "RAW":
		return f"  RAW     t_us={event.get('t_us')} dt_us={event.get('dt_us')}"
	if event_type == "ACCEPTED":
		return (
			f"  ACCEPTED n={event.get('n')} t_ms={event.get('t_ms')} "
			f"dt_ms={event.get('dt_ms')}"
		)
	if event_type == "REJECTED":
		return (
			f"  REJECTED n={event.get('n')} t_ms={event.get('t_ms')} "
			f"dt_ms={event.get('dt_ms')}  (inside debounce)"
		)
	if event_type == "OVERFLOW":
		return f"OVERFLOW dropped={event.get('dropped')}  (raise baud or slow dial)"
	if event_type == "DIGIT":
		clusters = session.bounce_clusters
		cluster_txt = (
			", ".join(
				f"{c['edges']} edges / {c['span_us']} us" for c in clusters
			)
			or "none"
		)
		return (
			f"DIGIT ascii={event.get('ascii')} "
			f"raw={event.get('raw')} accepted={event.get('accepted')} "
			f"intervals_ms={event.get('intervals_ms')}\n"
			f"  bounce_clusters=[{cluster_txt}] "
			f"rejected={session.rejected_count}"
		)
	return str(event)


def run_monitor(port: str, baud: int) -> int:
	try:
		import serial
	except ImportError:
		print(
			"pyserial is required. From tools/pulse_monitor:\n"
			"  python3 -m venv .venv\n"
			"  .venv/bin/pip install -r requirements.txt\n"
			"  .venv/bin/python pulse_monitor.py --port " + port,
			file=sys.stderr,
		)
		return 1

	session = DialSession()
	print(f"Opening {port} at {baud} baud...", flush=True)
	with serial.Serial(port, baud, timeout=0.2) as ser:
		# Allow MCU reset after USB open (ATmega USB-serial toggles DTR).
		time.sleep(0.5)
		ser.reset_input_buffer()
		print("Listening. Dial the rotary; Ctrl+C to quit.\n", flush=True)
		buf = ""
		while True:
			chunk = ser.read(256).decode("ascii", errors="replace")
			if not chunk:
				continue
			buf += chunk
			while "\n" in buf:
				line, buf = buf.split("\n", 1)
				event = parse_line(line)
				if event is None:
					continue
				session.handle(event)
				print(format_event(event, session), flush=True)
				if event["type"] == "DIGIT":
					print("-" * 60, flush=True)


def main(argv: list[str] | None = None) -> int:
	parser = argparse.ArgumentParser(
		description="Monitor rotary pulse debounce debug output from the MCU."
	)
	parser.add_argument("--port", default=default_port(), help="Serial device")
	parser.add_argument("--baud", type=int, default=9600, help="Serial baud")
	args = parser.parse_args(argv)
	try:
		return run_monitor(args.port, args.baud)
	except KeyboardInterrupt:
		print("\nStopped.")
		return 0
	except Exception as exc:
		print(f"Error: {exc}", file=sys.stderr)
		return 1


if __name__ == "__main__":
	sys.exit(main())
