"""
Find functions that call themselves directly.

On an ATmega2560 with roughly 3.2 KB left for heap and stack, an
unconditional self-call exhausts the stack within a few hundred frames. The
board stops responding and only a power cycle clears it, and because the
console dies with everything else there is no log to diagnose it from.

avr-gcc 7.3.0 -- the version the Arduino AVR core pins -- predates
-Winfinite-recursion, which arrived in GCC 12 under -Wall. Until the toolchain
moves, nothing in the build catches this, so it is checked here.

Only unconditional direct self-calls are reported: a call sitting at the top
level of a function's body, so that reaching the function means recursing.
Recursion guarded by a condition is left alone because it is usually bounded
and legitimate -- format_phone_display() recurses once for an 11-digit number
beginning with 1, and the recursive call cannot recurse again. Reporting those
would make the check noisy, and a noisy check gets switched off.

Mutual recursion between two functions is not detected, and would need a call
graph.
"""

import argparse
import re
import sys

# A definition at file scope: return type, name, parameter list, then the
# opening brace. Anchored to column zero so nested blocks and calls are not
# mistaken for definitions. The brace may sit on the following line, which is
# the style used throughout this firmware.
_DEFINITION = re.compile(
	r"^(?![\s#])"          # column zero, not a preprocessor line
	r"[A-Za-z_][\w \t\*&]*?"  # return type and any qualifiers
	r"\b([A-Za-z_]\w*)"    # the function's name
	r"\s*\([^;{}]*\)"      # its parameter list
	r"[\w \t]*\s*\{",      # trailing specifiers, then the opening brace
	re.M,
)


def _blank_comments_and_strings(source: str) -> str:
	"""Replace comment and string contents with spaces, preserving offsets.

	A function's own name routinely appears in the comment describing it
	and in log messages it emits. Matching those would flag exactly the
	well-documented code this project prefers, and the cheapest way to
	silence the check would be to delete the documentation.

	Offsets are preserved so that positions found afterwards still line up
	with the original text, which is what line numbers are counted from.
	"""
	out = []
	i = 0
	end = len(source)
	while i < end:
		two = source[i:i + 2]
		if two == "/*":
			close = source.find("*/", i + 2)
			close = end if close < 0 else close + 2
			out.append(_spaces_like(source[i:close]))
			i = close
		elif two == "//":
			close = source.find("\n", i)
			close = end if close < 0 else close
			out.append(_spaces_like(source[i:close]))
			i = close
		elif source[i] in "\"'":
			quote = source[i]
			j = i + 1
			while j < end and source[j] != quote:
				j += 2 if source[j] == "\\" else 1
			j = min(j + 1, end)
			out.append(_spaces_like(source[i:j]))
			i = j
		else:
			out.append(source[i])
			i += 1
	return "".join(out)


def _spaces_like(text: str) -> str:
	"""Blank `text`, keeping its newlines so line numbering is unchanged."""
	return "".join("\n" if c == "\n" else " " for c in text)


def _body_after(source: str, brace_index: int) -> str:
	"""The text between a function's opening brace and its match."""
	depth = 0
	for i in range(brace_index, len(source)):
		if source[i] == "{":
			depth += 1
		elif source[i] == "}":
			depth -= 1
			if depth == 0:
				return source[brace_index + 1:i]
	return source[brace_index + 1:]


def _top_level_only(body: str) -> str:
	"""Blank everything nested inside braces, preserving offsets.

	What remains is the code that runs on every path through the function,
	which is where a self-call is unconditionally fatal.
	"""
	out = []
	depth = 0
	for char in body:
		if char == "{":
			depth += 1
			out.append(" ")
		elif char == "}":
			depth -= 1
			out.append(" ")
		elif depth > 0:
			out.append("\n" if char == "\n" else " ")
		else:
			out.append(char)
	return "".join(out)


def find_self_recursive(source: str) -> list[tuple[str, int]]:
	"""Names and 1-based definition lines of unconditional self-callers.

	The name is matched on word boundaries so that a call to a longer name
	sharing the prefix -- ui_show() invoking ui_show_incoming() -- is not
	reported.
	"""
	cleaned = _blank_comments_and_strings(source)
	found = []
	for match in _DEFINITION.finditer(cleaned):
		name = match.group(1)
		body = _top_level_only(_body_after(cleaned, match.end() - 1))
		if re.search(r"\b" + re.escape(name) + r"\s*\(", body):
			line = cleaned.count("\n", 0, match.start()) + 1
			found.append((name, line))
	return found


def main(argv: list[str] | None = None) -> int:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("paths", nargs="+", help="source files to scan")
	args = parser.parse_args(argv)

	offenders = []
	for path in args.paths:
		with open(path, encoding="utf-8", errors="replace") as handle:
			for name, line in find_self_recursive(handle.read()):
				offenders.append(f"{path}:{line}: {name}() calls itself")

	if offenders:
		print("Infinite recursion (stack overflow at runtime):")
		for line in offenders:
			print(f"  {line}")
		return 1
	print(f"Recursion check OK: {len(args.paths)} files, no self-calls")
	return 0


if __name__ == "__main__":
	sys.exit(main())
