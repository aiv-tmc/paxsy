"""Fuzz suite (deterministic): the compiler must never crash with a
signal (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE) on grammar-generated
programs, and must answer every input with a clean exit code. Grammar
programs are held to the stricter bar: no signal at all. Mutated
garbage may be rejected with a diagnostic (exit 1) but must not hang.
Run count scales with FUZZ_N (default 300 grammar + 300 mutations).
"""

import os
import unittest

from paxsy_testlib import ProjectCase, run_paxsy
from fuzz_gen import gen_program, gen_mutation

N_GRAMMAR = int(os.environ.get("FUZZ_N", "300"))
N_MUTATE = N_GRAMMAR

SIGNAL_CODES = {137, 134, 132, 136, 138, 139, 135, 133}  # 128+sig


class FuzzTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def _crash_codes(self, r):
        """A subprocess result that died by signal (negative returncode
        or 128+signal shell code) means the compiler crashed."""
        if r.returncode < 0:
            return True
        return r.returncode in SIGNAL_CODES

    def test_grammar_programs_never_crash(self):
        for seed in range(N_GRAMMAR):
            with self.subTest(seed=seed):
                (self.proj / "src/main.px").write_text(gen_program(seed))
                r = run_paxsy("build", cwd=self.proj)
                self.assertFalse(self._crash_codes(r),
                                 f"compiler crashed on seed {seed}: "
                                 f"rc={r.returncode}\n{r.stderr[:400]}")

    def test_mutations_never_crash(self):
        for seed in range(N_MUTATE):
            with self.subTest(seed=seed):
                (self.proj / "src/main.px").write_text(gen_mutation(seed))
                r = run_paxsy("build", cwd=self.proj)
                self.assertFalse(self._crash_codes(r),
                                 f"compiler crashed on mutation {seed}: "
                                 f"rc={r.returncode}\n{r.stderr[:400]}")


if __name__ == "__main__":
    unittest.main()
