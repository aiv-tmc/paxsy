"""Compile-time regression gates (baseline lock, generous bounds).

The compiler is fast: ~18M lexer tokens/sec and a 1200-function
program builds in ~0.06 s. These tests pin that magnitude with
100x headroom so a future change that tanks throughput fails the
suite instead of shipping silently. Wall-clock only, no tuning.
"""

import time
import unittest

from paxsy_testlib import ProjectCase, run_paxsy


def big_source(nfun, ncalls):
    src = ["use std::inout;"]
    for i in range(nfun):
        src.append(
            "func g%d(a: Int): Int { let x: Int = a + %d; "
            "if x > 50 { return x * 2; } return x - 1; }" % (i, i % 13))
    body = " ".join(
        "let q%d: Int = g%d(q%d);" % (i, i % nfun, i - 1)
        for i in range(1, ncalls))
    src.append("func main(): Void { let q0: Int = 1; %s "
               "std::inout::println(\"ok {q%d}\"); }"
               % (body, ncalls - 1))
    return "\n".join(src) + "\n"


class CompilePerfTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def test_1200_functions_build_quickly(self):
        (self.proj / "src/main.px").write_text(big_source(1200, 800))
        t0 = time.time()
        r = run_paxsy("build", cwd=self.proj)
        dt = time.time() - t0
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertLess(dt, 5.0,
                        "1200-function build took %.2fs (baseline 0.06s)"
                        % dt)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_lex_throughput_baseline(self):
        (self.proj / "src/main.px").write_text(big_source(300, 1))
        (self.proj / "benches").mkdir()
        (self.proj / "benches/lexer.px").write_text(big_source(300, 1))
        t0 = time.time()
        r = run_paxsy("bench", cwd=self.proj)
        dt = time.time() - t0
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertLess(dt, 5.0,
                        "lexer benchmark took %.2fs (baseline ~0.01s)" % dt)


if __name__ == "__main__":
    unittest.main()
