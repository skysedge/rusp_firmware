"""Enforce a static RAM budget on the built firmware.

Why this exists: the ATmega2560 has 8192 bytes of SRAM shared between globals
(.data + .bss), the heap, and the stack. Static usage reached 85% without
anyone noticing, leaving roughly 1.2 KB for everything dynamic. That is the
condition behind intermittent faults that get attributed to anything but
memory, because the symptom (a dead rotary dial, a corrupted buffer) is far
from the cause.

A budget that fails the build turns that slow creep into an immediate, named
failure. The limits are meant to be ratcheted down as savings land, never
raised to accommodate a new global without a deliberate decision.

Reads `avr-size -A` output rather than the Arduino CLI summary because the
per-section numbers are what the budget is written against.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass

# Sections that occupy SRAM at run time. .data is copied from flash into RAM
# at startup (this is where non-PROGMEM string literals end up); .bss is
# zeroed. Everything else in the ELF — .text, .comment, .debug_* — lives in
# flash or in the file only.
RAM_SECTIONS = (".data", ".bss")

# ATmega2560 SRAM.
SRAM_BYTES = 8192


class SectionParseError(Exception):
	"""Raised when the size output cannot be read.

	Deliberately an error rather than a zero default: a section that silently
	reads as 0 makes every budget pass, so a broken parse would present as a
	green build and nobody would look again.
	"""


@dataclass(frozen=True)
class Budget:
	"""Per-section ceilings, in bytes. A build at exactly the limit passes."""

	data_max: int
	bss_max: int


@dataclass(frozen=True)
class BudgetViolation:
	section: str
	actual: int
	limit: int

	@property
	def overrun(self) -> int:
		return self.actual - self.limit


def parse_section_sizes(avr_size_output: str) -> dict[str, int]:
	"""Map section name to size from `avr-size -A` output.

	Returns every section found, not just the RAM ones, so callers can report
	flash usage too. Raises SectionParseError if any RAM section is absent,
	since that means the output is not what we think it is.
	"""
	sections: dict[str, int] = {}
	for line in avr_size_output.splitlines():
		fields = line.split()
		if len(fields) < 2 or not fields[0].startswith("."):
			continue
		try:
			sections[fields[0]] = int(fields[1])
		except ValueError:
			continue

	missing = [s for s in RAM_SECTIONS if s not in sections]
	if missing:
		raise SectionParseError(
			f"no {', '.join(missing)} in size output; "
			"is this an avr-size -A dump of a linked ELF?"
		)
	return sections


def check_budget(
	sections: dict[str, int], budget: Budget
) -> list[BudgetViolation]:
	"""Return every ceiling the build exceeds, in RAM_SECTIONS order.

	Reports all violations rather than the first so two overruns take one
	rebuild to find, not two.
	"""
	limits = {".data": budget.data_max, ".bss": budget.bss_max}
	return [
		BudgetViolation(
			section=name, actual=sections[name], limit=limits[name]
		)
		for name in RAM_SECTIONS
		if sections[name] > limits[name]
	]


def format_report(
	violations: list[BudgetViolation],
	sram_bytes: int = SRAM_BYTES,
	used: int | None = None,
) -> str:
	"""Human-readable result.

	The passing report states remaining headroom on purpose: a build that is
	under budget but trending toward the cap is the thing worth seeing early.
	"""
	if violations:
		lines = ["RAM budget exceeded:"]
		lines += [
			f"  {v.section}: {v.actual} bytes, limit {v.limit} "
			f"(over by {v.overrun})"
			for v in violations
		]
		lines.append(
			"Raise the limit only as a deliberate decision — it exists to "
			"make growth visible."
		)
		return "\n".join(lines)

	if used is None:
		return "RAM budget OK"
	free = sram_bytes - used
	pct = (used * 100) // sram_bytes
	return (
		f"RAM budget OK: {used} of {sram_bytes} bytes used ({pct}%), "
		f"{free} free for heap and stack"
	)


def _read_sizes(avr_size: str, elf: str) -> str:
	result = subprocess.run(
		[avr_size, "-A", elf],
		capture_output=True,
		text=True,
		check=False,
	)
	if result.returncode != 0:
		raise SectionParseError(
			f"{avr_size} -A {elf} failed: {result.stderr.strip()}"
		)
	return result.stdout


def main(argv: list[str] | None = None) -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("elf", help="path to the linked .elf")
	parser.add_argument("--avr-size", required=True, help="avr-size binary")
	parser.add_argument("--data-max", type=int, required=True)
	parser.add_argument("--bss-max", type=int, required=True)
	parser.add_argument("--sram", type=int, default=SRAM_BYTES)
	args = parser.parse_args(argv)

	sections = parse_section_sizes(_read_sizes(args.avr_size, args.elf))
	violations = check_budget(
		sections, Budget(data_max=args.data_max, bss_max=args.bss_max)
	)
	used = sections[".data"] + sections[".bss"]
	print(format_report(violations, sram_bytes=args.sram, used=used))
	return 1 if violations else 0


if __name__ == "__main__":
	sys.exit(main())
