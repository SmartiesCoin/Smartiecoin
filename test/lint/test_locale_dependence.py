# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Keep the BDB import-pointer exception narrower than ordinary references."""

import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "lint_locale_dependence", Path(__file__).with_name("lint-locale-dependence.py"))
assert SPEC is not None and SPEC.loader is not None
LINT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LINT)


class LocaleDependenceTests(unittest.TestCase):
    def check_line(self, line, expected):
        with patch.object(LINT, "find_locale_dependent_function_uses", return_value=[line]):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as result:
                LINT.main()
        self.assertEqual(result.exception.code, expected, line)

    def test_import_pointer_definitions_are_not_calls(self):
        for name, argument in [("snprintf", "..."), ("vsnprintf", "va_list")]:
            with self.subTest(name=name):
                self.check_line(
                    f"src/wallet/bdb.cpp:int (*__imp__{name})(char*, size_t, const char*, {argument}) = {name};", 0)

    def test_real_calls_and_references_still_fail(self):
        for name, argument in [("snprintf", "..."), ("vsnprintf", "va_list")]:
            alias = f"int (*__imp__{name})(char*, size_t, const char*, {argument}) = {name};"
            for source in [
                f"{name}(buffer, size, format, args);",
                f"auto pointer = {name};",
                f"auto pointer = &{name};",
                f"__imp__{name}(buffer, size, format, args); {name}(buffer, size, format, args);",
                f"{name}(buffer, size, format, args); " + alias,
                alias + f" {name}(buffer, size, format, args);",
                alias + " auto pointer = printf;",
                alias.replace(f"= {name};", f"= &{name};"),
                alias.replace(" = ", "  = "),
                alias.replace(f"= {name};", f"= {name}(buffer, size, format, args);"),
                alias.replace("__imp__", "ordinary_"),
                alias.replace(f"= {name};", "= printf;"),
            ]:
                with self.subTest(source=source):
                    self.check_line("src/wallet/bdb.cpp:" + source, 1)
            self.check_line("src/other.cpp:" + alias, 1)

    def test_integer_and_float_to_string_remain_detected(self):
        for argument in ["n", "1.25"]:
            self.check_line(f"src/sapling/sapling_transaction.h: return std::to_string({argument});", 1)
