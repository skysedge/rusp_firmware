"""Tests for call-type mode bypass and effective dialing mode.

Why these tests exist: local/ALT switch paths can leave the phone in a weird
dialing state (prepend, contacts, hook stolen for speed-dial). Bypass must
make the call button dial exactly what was typed (dial_buf), regardless of
switch position.

Failure modes when regressions occur:
- Bypass ignored → ALT still intercepts digits / hook.
- Priority wrong → LOCAL wins over ALT when both asserted.
- Hook policy wrong → hook ignored in nonlocal, or still used for speed-dial
  when modes are disabled.
"""

import unittest

from call_mode import (
	CallMode,
	allow_alt_contacts,
	allow_local_prepend,
	hook_drives_call_actions,
	resolve_call_mode,
)


class ResolveCallModeTests(unittest.TestCase):
	def test_nonlocal_when_neither_switch(self):
		# Guards default throw. Failure: defaults to LOCAL/ALT by mistake.
		self.assertEqual(
			resolve_call_mode(False, False, disable_call_type_modes=False),
			CallMode.NONLOCAL,
		)

	def test_local_when_local_switch(self):
		# Guards local prepend path. Failure: LOCAL never selected.
		self.assertEqual(
			resolve_call_mode(False, True, disable_call_type_modes=False),
			CallMode.LOCAL,
		)

	def test_alt_when_alt_switch(self):
		# Guards address-book path. Failure: ALT never selected.
		self.assertEqual(
			resolve_call_mode(True, False, disable_call_type_modes=False),
			CallMode.ALT,
		)

	def test_alt_priority_over_local(self):
		# Guards impossible dual-assert. Failure: LOCAL wins and prepends.
		self.assertEqual(
			resolve_call_mode(True, True, disable_call_type_modes=False),
			CallMode.ALT,
		)

	def test_bypass_forces_nonlocal_for_all_switch_positions(self):
		# Guards DISABLE_CALL_TYPE_MODES. Failure: physical ALT/LOCAL still win.
		cases = (
			(False, False),
			(False, True),
			(True, False),
			(True, True),
		)
		for alt, local in cases:
			with self.subTest(alt=alt, local=local):
				self.assertEqual(
					resolve_call_mode(
						alt, local, disable_call_type_modes=True
					),
					CallMode.NONLOCAL,
				)


class ModePolicyTests(unittest.TestCase):
	def test_hook_drives_call_except_in_alt(self):
		# Guards hook/speed-dial split. Failure: hook never dials, or ALT dials.
		self.assertTrue(hook_drives_call_actions(CallMode.NONLOCAL))
		self.assertTrue(hook_drives_call_actions(CallMode.LOCAL))
		self.assertFalse(hook_drives_call_actions(CallMode.ALT))

	def test_bypass_mode_allows_hook_and_blocks_special_paths(self):
		# Guards bypass side effects. Failure: still prepends or opens contacts.
		mode = resolve_call_mode(True, True, disable_call_type_modes=True)
		self.assertTrue(hook_drives_call_actions(mode))
		self.assertFalse(allow_local_prepend(mode))
		self.assertFalse(allow_alt_contacts(mode))


if __name__ == "__main__":
	unittest.main()
