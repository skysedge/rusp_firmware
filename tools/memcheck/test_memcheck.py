"""Tests for the firmware RAM budget check.

Every test states the regression it guards and how that regression shows up,
so a failure here can be told apart from an unrelated breakage.
"""

import unittest

from memcheck import (
	Budget,
	BudgetViolation,
	SectionParseError,
	check_budget,
	format_report,
	parse_section_sizes,
)

# Real `avr-size -A` output for the ATmega2560 build, trimmed of the debug
# sections that appear after .note. Shared so the expected numbers below have
# a single source.
AVR_SIZE_OUTPUT = """\
/Users/x/Library/Caches/arduino/sketches/ABC/rusp_firmware.ino.elf  :
section                      size      addr
.data                        2982   8389120
.text                       57818         0
.bss                         4027   8392102
.comment                       17         0
.note.gnu.avr.deviceinfo       64         0
.debug_aranges                464         0
.debug_info                100011         0
"""

# ATmega2560 SRAM. Globals, heap and stack all live here.
SRAM_BYTES = 8192


class ParseTests(unittest.TestCase):
	def test_extracts_ram_sections_from_avr_size_output(self):
		# Why: the budget is meaningless if the numbers it reads are wrong.
		# Failure: sizes come back as 0 or pick up .text, so the check
		# either always passes or always fails.
		sections = parse_section_sizes(AVR_SIZE_OUTPUT)
		self.assertEqual(sections[".data"], 2982)
		self.assertEqual(sections[".bss"], 4027)
		self.assertEqual(sections[".text"], 57818)

	def test_debug_sections_are_parsed_but_do_not_pollute_ram_totals(self):
		# Why: .debug_info is 100 KB and dwarfs everything. If it were ever
		# folded into the RAM total the check would be nonsense.
		# Failure: a RAM total in the tens of thousands on an 8 KB part.
		sections = parse_section_sizes(AVR_SIZE_OUTPUT)
		self.assertEqual(
			sections[".data"] + sections[".bss"], 7009
		)
		self.assertIn(".debug_info", sections)

	def test_missing_ram_section_is_an_error_not_a_zero(self):
		# Why: returning 0 for an absent section would make any budget pass,
		# turning a broken parse into a silent green build — worse than a
		# loud failure because nobody would look again.
		# Failure: no exception, and check_budget reports success.
		with self.assertRaises(SectionParseError):
			parse_section_sizes("section  size  addr\n.text  100  0\n")

	def test_empty_input_is_an_error(self):
		# Why: an empty capture (avr-size missing, wrong path) must not be
		# read as a zero-byte firmware.
		# Failure: silently passes the budget.
		with self.assertRaises(SectionParseError):
			parse_section_sizes("")


class BudgetTests(unittest.TestCase):
	def test_build_within_budget_reports_no_violations(self):
		# Why: the pass case must be reachable, or the gate blocks all work.
		# Failure: a conforming build cannot be committed.
		sections = {".data": 1000, ".bss": 3000}
		self.assertEqual(
			check_budget(sections, Budget(data_max=1200, bss_max=4100)), []
		)

	def test_oversized_data_is_reported(self):
		# Why: .data is the string-literal section this whole effort targets.
		# Failure: strings drift back into RAM without the build objecting.
		sections = {".data": 2982, ".bss": 3000}
		violations = check_budget(
			sections, Budget(data_max=1200, bss_max=4100)
		)
		self.assertEqual(
			violations,
			[BudgetViolation(section=".data", actual=2982, limit=1200)],
		)

	def test_oversized_bss_is_reported(self):
		# Why: .bss holds the big buffers; a new one must not slip in.
		# Failure: an 880-byte buffer is added and nothing notices.
		sections = {".data": 1000, ".bss": 4500}
		violations = check_budget(
			sections, Budget(data_max=1200, bss_max=4100)
		)
		self.assertEqual(
			violations,
			[BudgetViolation(section=".bss", actual=4500, limit=4100)],
		)

	def test_both_sections_over_are_reported_together(self):
		# Why: reporting only the first violation means two rebuild cycles
		# to discover two problems.
		# Failure: a single-element list, and the second overrun surfaces
		# only after the first is fixed.
		sections = {".data": 2982, ".bss": 4500}
		violations = check_budget(
			sections, Budget(data_max=1200, bss_max=4100)
		)
		self.assertEqual(len(violations), 2)
		self.assertEqual(
			[v.section for v in violations], [".data", ".bss"]
		)

	def test_exactly_at_the_limit_passes(self):
		# Why: the boundary decides whether a budget set to the current
		# measured size is immediately red. Chosen so a ratcheted budget
		# equal to actual usage is a pass, not a failure.
		# Failure: setting the budget to the achieved size fails the build.
		sections = {".data": 1200, ".bss": 4100}
		self.assertEqual(
			check_budget(sections, Budget(data_max=1200, bss_max=4100)), []
		)

	def test_one_byte_over_the_limit_fails(self):
		# Why: pairs with the boundary test above to pin the comparison as
		# "greater than" rather than "greater or equal".
		# Failure: off-by-one lets usage creep past the ceiling.
		sections = {".data": 1201, ".bss": 4100}
		self.assertEqual(
			len(check_budget(sections, Budget(data_max=1200, bss_max=4100))),
			1,
		)

	def test_zero_sizes_are_within_any_budget(self):
		# Why: the empty/zero case. A build with no globals is legal and
		# must not trip the gate.
		# Failure: a spurious violation on a trivially small build.
		self.assertEqual(
			check_budget(
				{".data": 0, ".bss": 0}, Budget(data_max=0, bss_max=0)
			),
			[],
		)


class ReportTests(unittest.TestCase):
	def test_report_names_the_section_and_both_numbers(self):
		# Why: the failure message is the whole value of the gate. Without
		# the overrun amount the next step is guesswork.
		# Failure: an opaque "budget exceeded" with nothing actionable.
		report = format_report(
			[BudgetViolation(section=".data", actual=2982, limit=1200)],
			sram_bytes=SRAM_BYTES,
		)
		self.assertIn(".data", report)
		self.assertIn("2982", report)
		self.assertIn("1200", report)
		self.assertIn("1782", report)  # overrun

	def test_report_for_a_clean_build_states_headroom(self):
		# Why: the passing report is what tells us how much room is left,
		# which is the number this whole exercise is about.
		# Failure: a silent pass that hides usage trending toward the cap.
		report = format_report([], sram_bytes=SRAM_BYTES, used=7009)
		self.assertIn("7009", report)
		self.assertIn("1183", report)  # 8192 - 7009


if __name__ == "__main__":
	unittest.main()
