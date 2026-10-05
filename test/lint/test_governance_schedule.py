#!/usr/bin/env python3
"""Regression guard for the governance schedule's leaf dependency boundary."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class GovernanceScheduleDependencies(unittest.TestCase):
    def test_validators_use_leaf_not_classes(self):
        text = (ROOT / 'src/governance/validators.cpp').read_text(encoding='utf8')
        self.assertNotIn('#include <governance/classes.h>', text)
        self.assertIn('#include <governance/superblock_schedule.h>', text)
        self.assertNotIn('CSuperblock::', text)

    def test_schedule_is_a_leaf(self):
        for suffix in ('h', 'cpp'):
            path = ROOT / f'src/governance/superblock_schedule.{suffix}'
            self.assertTrue(path.exists(), f'Missing schedule module: {path}')
            text = path.read_text(encoding='utf8')
            for dependency in re.findall(r'^#include [<"]([^>"]+)', text, re.MULTILINE):
                self.assertNotIn(dependency, ('governance/classes.h', 'governance/object.h',
                                               'governance/validators.h'))


if __name__ == '__main__':
    unittest.main()
