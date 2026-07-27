"""Tests for recursion: finding functions that call themselves."""

import unittest

from recursion import find_self_recursive


class DetectionTests(unittest.TestCase):
	"""Direct self-recursion is unsurvivable on this part.

	With roughly 3.2 KB left for heap and stack, a function that always
	calls itself overruns the stack in a few hundred frames. The phone
	stops responding entirely and only a reset clears it, so there is no
	log to diagnose it from. avr-gcc 7.3.0 predates -Winfinite-recursion
	(GCC 12), which is why this check exists at all.
	"""

	def test_finds_an_unconditional_self_call(self):
		# Why: the shape of the real defect -- ui_show_incoming() was
		# edited into calling itself instead of ui_set_status().
		# Failure: the build passes and the phone hangs on the next
		# incoming call, with nothing on the console to explain it.
		src = (
			"static void ui_show_incoming(void)\n"
			"{\n"
			"\tcaller_line_active = true;\n"
			"\tui_show_incoming();\n"
			"}\n"
		)
		self.assertEqual(
			find_self_recursive(src), [("ui_show_incoming", 1)]
		)

	def test_reports_every_offender_not_just_the_first(self):
		# Why: a scripted edit can rewrite several definitions at once,
		# which is exactly how the real one happened.
		# Failure: one is fixed, the build goes green, and the rest ship.
		src = (
			"static void a(void)\n{\n\ta();\n}\n"
			"static void b(void)\n{\n\tb();\n}\n"
		)
		self.assertEqual(find_self_recursive(src), [("a", 1), ("b", 5)])

	def test_finds_a_self_call_after_other_statements(self):
		# Why: the call is rarely the first statement. Body extraction
		# must balance braces rather than stop at the first '}', or a
		# self-call following any nested block is missed.
		# Failure: the real defect goes unreported, since its self-call
		# sat below an assignment.
		src = (
			"static void poll(void)\n"
			"{\n"
			"\tif (ready) {\n"
			"\t\tarm();\n"
			"\t}\n"
			"\tpoll();\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [("poll", 1)])

	def test_finds_a_self_call_in_a_function_taking_arguments(self):
		# Why: most functions take parameters; matching only "(void)"
		# would inspect almost nothing.
		# Failure: the check silently covers a fraction of the codebase
		# and reports success regardless.
		src = (
			"static int depth(int n)\n"
			"{\n"
			"\treturn depth(n - 1);\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [("depth", 1)])


class FalsePositiveTests(unittest.TestCase):
	"""A noisy check gets disabled, so silence on clean code matters."""

	def test_ignores_a_function_that_calls_something_else(self):
		# Why: the overwhelmingly common case must stay silent.
		# Failure: every build fails and the check is removed.
		src = (
			"static void ui_show_incoming(void)\n"
			"{\n"
			"\tui_set_status(\"Incoming\");\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [])

	def test_ignores_a_call_to_a_longer_name_sharing_the_prefix(self):
		# Why: ui_show() calling ui_show_incoming() is not recursion.
		# Matching on the bare name without a word boundary would flag it.
		# Failure: legitimate helper pairs are reported as fatal bugs.
		src = (
			"static void ui_show(void)\n"
			"{\n"
			"\tui_show_incoming();\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [])

	def test_ignores_the_name_inside_a_comment(self):
		# Why: this codebase documents heavily, and a docstring naming
		# the function it describes is normal.
		# Failure: well-commented functions are flagged, so the fix is to
		# delete the comment -- the check would actively harm the source.
		src = (
			"static void poll(void)\n"
			"{\n"
			"\t/* poll() is driven from loop(). */\n"
			"\t// see poll() above\n"
			"\tread();\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [])

	def test_ignores_the_name_inside_a_string_literal(self):
		# Why: log lines routinely name the function they report from.
		# Failure: adding a log message makes the build fail.
		src = (
			"static void poll(void)\n"
			"{\n"
			"\tcall_log(\"poll() ran\");\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [])

	def test_ignores_a_prototype_that_is_not_a_definition(self):
		# Why: headers and forward declarations have no body to inspect.
		# Failure: a declaration followed anywhere by a matching call
		# reads as recursion.
		src = "static void poll(void);\nstatic void run(void)\n{\n\tpoll();\n}\n"
		self.assertEqual(find_self_recursive(src), [])

	def test_ignores_recursion_guarded_by_a_condition(self):
		# Why: format_phone_display() genuinely calls itself, but only
		# for an 11-digit number starting with 1, and the recursive call
		# sees 10 digits so it cannot recurse again. Bounded recursion is
		# legitimate; only a call on every path is certain death.
		# Failure: working code is reported as a fatal bug, the check is
		# judged untrustworthy, and it gets deleted.
		src = (
			"void format_phone_display(const char *raw)\n"
			"{\n"
			"\tif (n == 11 && digits[0] == '1') {\n"
			"\t\tformat_phone_display(digits + 1);\n"
			"\t\treturn;\n"
			"\t}\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [])

	def test_ignores_a_self_call_inside_a_loop(self):
		# Why: same reasoning for the other nesting construct.
		# Failure: iterative helpers that recurse per item are flagged.
		src = (
			"static void walk(void)\n"
			"{\n"
			"\tfor (i = 0; i < n; i++) {\n"
			"\t\twalk();\n"
			"\t}\n"
			"}\n"
		)
		self.assertEqual(find_self_recursive(src), [])

	def test_empty_source_yields_nothing(self):
		# Why: the null case -- an empty or header-only file.
		# Failure: an exception aborts the build for a harmless file.
		self.assertEqual(find_self_recursive(""), [])


if __name__ == "__main__":
	unittest.main()
