"""Determinism suite: repeated builds of the same source must be
bit-identical (same size, same hash), and the emitted binaries must
not depend on build cache state (fresh vs incremental)."""

import hashlib
import os
import shutil
import unittest

from paxsy_testlib import ProjectCase, run_paxsy

CASES = {
    "hello": "use std::inout;\nfunc main(): Void { std::inout::println(\"hi\"); }\n",
    "compute": ("use std::inout;\n"
                "func fib(n: Int): Int { if n < 2 { return n; } return fib(n-1) + fib(n-2); }\n"
                "func main(): Void { std::inout::println(\"f={fib(10)}\"); }\n"),
    "real": ("use std::inout;\n"
             "func main(): Void { let x: Real = 1.5; std::inout::println(\"v={(x * 2.0) as Int}\"); }\n"),
    "struct": ("use std::inout;\nstatic P = Struct { x: Int, y: Int };\n"
               "func main(): Void { let p: P = P {1, 2}; std::inout::println(\"v={p.x + p.y}\"); }\n"),
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


class DeterminismTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def test_repeated_builds_are_bit_identical(self):
        for name, src in CASES.items():
            with self.subTest(case=name):
                (self.proj / "src/main.px").write_text(src)
                hashes = []
                sizes = []
                for _ in range(3):
                    r = run_paxsy("build", cwd=self.proj)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    exe = self.proj / "build" / "demo"
                    self.assertTrue(exe.exists())
                    hashes.append(sha256(exe))
                    sizes.append(exe.stat().st_size)
                self.assertEqual(len(set(hashes)), 1,
                                 f"non-deterministic binary: {hashes}")
                self.assertEqual(len(set(sizes)), 1)

    def test_fresh_vs_incremental_identical(self):
        src = CASES["compute"]
        (self.proj / "src/main.px").write_text(src)
        shutil.rmtree(self.proj / "build", ignore_errors=True)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        fresh = sha256(self.proj / "build" / "demo")
        # touch the source and rebuild incrementally
        (self.proj / "src/main.px").write_text(src)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        incr = sha256(self.proj / "build" / "demo")
        self.assertEqual(fresh, incr, "incremental build differs from fresh build")
        shutil.rmtree(self.proj / "build", ignore_errors=True)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        rebuilt = sha256(self.proj / "build" / "demo")
        self.assertEqual(fresh, rebuilt, "rebuild after clean differs")


if __name__ == "__main__":
    unittest.main()
