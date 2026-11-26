#!/usr/bin/env python3
"""Run the memory-sensitive compiler, optimizer and module suites under ASan.

The exhaustive language-conformance corpus is covered by `make test`; running
its hundreds of subprocess-heavy cases under ASan multiplies runtime several
fold without adding equivalent memory-safety coverage. Keep this suite focused
on compiler internals and representative end-to-end paths.
"""
import importlib
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
MODULES = (
    "test_static",
    "test_type_safety",
    "test_modules",
    "test_opt",
)
loader = unittest.defaultTestLoader
suite = unittest.TestSuite()
for name in MODULES:
    suite.addTests(loader.loadTestsFromModule(importlib.import_module(name)))
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
