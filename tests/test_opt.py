#!/usr/bin/env python3
"""Optimizer behaviour on the native build route. Without textual
listings the pass is observed through its verbose rewrite report, the
PAXSY_NO_OPT stamp and the observable behaviour of the binaries."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

SRC = """func classify(v: Int32): Int32 {
    if v < 0 {
        return 0;
    }
    return v * 2;
}

func main(): Void! {
    try std::inout::println(classify(21));
    try std::inout::println(classify(-3));
}
"""

EXPECTED = "42\n0\n"


class OptimizerTests(unittest.TestCase):
    def setUp(self):
        self.cmd = [os.environ.get("PAXSY_CMD", "paxsy")]
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-opt-"))
        r = subprocess.run(self.cmd + ["init", "opt"], cwd=self.tmp,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.proj = self.tmp / "opt"
        (self.proj / "src/main.px").write_text(SRC)

    def build(self, env=None):
        e = dict(os.environ)
        e.pop("PAXSY_NO_OPT", None)
        if env:
            e.update(env)
        return subprocess.run(self.cmd + ["build"], cwd=self.proj,
                              capture_output=True, text=True, env=e)

    def run_bin(self):
        return subprocess.run([str(self.proj / "build/opt")],
                              capture_output=True, text=True)

    def test_optimized_and_unoptimized_binaries_agree(self):
        """Semantics are preserved with and without the pipeline; the
        A/B observable is the program output and the rewrite report."""
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_bin().stdout, EXPECTED)
        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_bin().stdout, EXPECTED)

    def test_optimizer_reports_rewrites_only_when_enabled(self):
        r = subprocess.run(self.cmd + ["build", "-v"], cwd=self.proj,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout, r"\(\d+ rewrites\)\n")
        r = subprocess.run(self.cmd + ["build", "-v"], cwd=self.proj,
                           capture_output=True, text=True,
                           env={**os.environ, "PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("rewrites", r.stdout)

    def test_no_opt_change_without_source_edits_rebuilds(self):
        """The optimisation stamp flips, so the incremental check
        must not short-circuit the second build."""
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("up to date", r.stdout)
        self.assertEqual(self.run_bin().stdout, EXPECTED)


if __name__ == "__main__":
    unittest.main()
