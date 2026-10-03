#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Regression tests for positional format argument counting."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


LINTER = Path(__file__).with_name("run-lint-format-strings.py")
SPEC = importlib.util.spec_from_file_location("format_lint", LINTER)
assert SPEC is not None and SPEC.loader is not None
FORMAT_LINT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FORMAT_LINT)


class FormatStringsTest(unittest.TestCase):
    def test_repeated_positions(self):
        self.assertEqual(FORMAT_LINT.count_format_specifiers(
            "%1$s %2$s %3$s %4$s %2$s %1$s"), 4)

    def test_positional_width_and_precision(self):
        self.assertEqual(FORMAT_LINT.count_format_specifiers("%3$*1$.*2$f %3$f"), 3)

    def test_escaped_percent_and_sequential_stars(self):
        self.assertEqual(FORMAT_LINT.count_format_specifiers("%%1$s %*.*f %s"), 4)

    def test_cli_rejects_mixed_argument_modes(self):
        cases = [
            ("%1$s %1$s %s", "a, b"),
            ("%2$s %s", "a, b, c"),
            ("%2$*s", "a, b, c"),
            ("%2$.*s", "a, b, c"),
            ("%s %2$s", "a, b, c"),
            ("%*2$s", "a, b, c"),
            ("%.*2$s", "a, b, c"),
            ("%3$*1$.*f", "a, b, c, d"),
        ]
        for fmt, args in cases:
            with self.subTest(fmt=fmt), tempfile.TemporaryDirectory() as tmp:
                source = Path(tmp) / "format.cpp"
                source.write_text(f'strprintf("{fmt}", {args});\n', encoding="utf-8")
                result = subprocess.run([sys.executable, str(LINTER), "strprintf", str(source)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("Mixed positional and sequential format arguments", result.stdout)
                self.assertIn(str(source), result.stdout)
                self.assertEqual(result.stderr, "")

    def test_cli_accepts_valid_argument_modes(self):
        cases = [
            ("%1$s %1$s", "a"),
            ("%3$*1$.*2$f %3$f", "a, b, c"),
            ("%2$*1$s", "a, b"),
            ("%2$.*1$s", "a, b"),
            ("%%1$s %*.*f %s", "a, b, c, d"),
            ("%% %1$s %%", "a"),
            ("%s %d", "a, b"),
        ]
        for fmt, args in cases:
            with self.subTest(fmt=fmt), tempfile.TemporaryDirectory() as tmp:
                source = Path(tmp) / "format.cpp"
                source.write_text(f'strprintf("{fmt}", {args});\n', encoding="utf-8")
                result = subprocess.run([sys.executable, str(LINTER), "strprintf", str(source)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout + result.stderr, "")

    def test_cli_rejects_missing_and_extra_arguments(self):
        for args, expected in [("a, b", 0), ("a", 1), ("a, b, c", 1)]:
            with self.subTest(args=args), tempfile.TemporaryDirectory() as tmp:
                source = Path(tmp) / "format.cpp"
                source.write_text(f'strprintf("%2$s %1$s %2$s", {args});\n', encoding="utf-8")
                result = subprocess.run([sys.executable, str(LINTER), "strprintf", str(source)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
