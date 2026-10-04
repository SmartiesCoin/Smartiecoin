#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Regression checks for low-level Qt module dependency boundaries."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2] / "src"


def includes(module):
    # Like circular-dependencies.py, merge the header and implementation.
    return {
        include
        for suffix in (".h", ".cpp")
        for include in re.findall(r"^#include <(.*)>", (ROOT / (module + suffix)).read_text(encoding="utf8"), re.M)
    }


class QtDependencyBoundaries(unittest.TestCase):
    def test_font_registry_does_not_depend_on_guiutil(self):
        self.assertNotIn("qt/guiutil.h", includes("qt/guiutil_font"))

    def test_guiutil_does_not_own_startup_options(self):
        dependencies = includes("qt/guiutil")
        self.assertNotIn("qt/optionsmodel.h", dependencies)
        self.assertNotIn("qt/appearancewidget.h", dependencies)


if __name__ == "__main__":
    unittest.main()
