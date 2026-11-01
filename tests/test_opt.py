#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Optimizer end-to-end tests for the paxsy HIR pipeline.

The suite builds seeded projects twice - once through the default
pipeline and once with PAXSY_NO_OPT=1, which disables the HIR
optimizer inside the code generation driver - and compares the
generated C units. The comparisons assert the visible effects of each
pass: branch folding collapses a constant conditional into one arm,
strength reduction turns a multiplication by a power of two into a
shift, and dead-code elimination removes an unused pure binding. Pure
literal arithmetic is folded earlier, in the AST folder, so it is not
exercised here. Binary behaviour is covered by test_paxsy.py; these
tests pin down the optimisation contract itself, so a regression in
opt_fold, opt_strength or opt_dce fails here even when the produced
program stays correct.
"""

import os
import subprocess
import tempfile
import unittest

PAXSY = os.environ.get("PAXSY_CMD", "../paxsy")


def run(argv, cwd=None, env=None):
    merged = dict(os.environ)
    if env:
        merged.update(env)
    return subprocess.run(argv, cwd=cwd, env=merged,
                          capture_output=True, text=True)


class OptimizerEndToEndTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        init = run([PAXSY, "init", "opt"], cwd=self.tmp.name)
        self.assertEqual(init.returncode, 0, init.stderr)
        self.proj = os.path.join(self.tmp.name, "opt")

    def build_c(self, source, env=None):
        with open(os.path.join(self.proj, "src", "main.px"), "w") as fh:
            fh.write(source)
        build = run([PAXSY, "build"], cwd=self.proj, env=env)
        self.assertEqual(build.returncode, 0, build.stderr)
        with open(os.path.join(self.proj, "target", "opt.c")) as fh:
            return fh.read()

    def test_branch_folding_collapses_constant_if(self):
        src = ("func main(): Void! {\n"
               "    if 2 < 3 {\n"
               "        try std::inout::println(1);\n"
               "    } else {\n"
               "        try std::inout::println(2);\n"
               "    }\n"
               "}\n")
        optimized = self.build_c(src)
        folded = self.build_c(src, env={"PAXSY_NO_OPT": "1"})
        self.assertIn("println__i(1)", optimized)
        self.assertNotIn("println__i(2)", optimized)
        self.assertIn("println__i(2)", folded)

    def test_strength_reduction_mul_to_shift(self):
        src = ("func scale(a: Int32): Int32 {\n"
               "    return a * 8;\n"
               "}\n"
               "func main(): Void! {\n"
               "    try std::inout::println(scale(5));\n"
               "}\n")
        optimized = self.build_c(src)
        self.assertIn("<< (3)", optimized)

    def test_dead_let_is_removed(self):
        src = ("func main(): Void! {\n"
               "    let x = 1;\n"
               "    let unused = x + 1;\n"
               "    try std::inout::println(x);\n"
               "}\n")
        optimized = self.build_c(src)
        unoptimized = self.build_c(src, env={"PAXSY_NO_OPT": "1"})
        self.assertNotIn("+ (1)", optimized)
        self.assertIn("+ (1)", unoptimized)


if __name__ == "__main__":
    unittest.main()
