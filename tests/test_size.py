"""Code-size golden regression and dead-instruction scans.

.text section sizes of canonical programs are pinned byte-exact:
any change to the emitter, the optimiser or the standard library that
adds a single instruction is a test failure. The scans additionally
reject the dead patterns the peephole must never leave behind:
two immediate moves to the same register in a row (the first one
carries no value) and register self-moves.
"""

import re
import subprocess
import unittest

from paxsy_testlib import ProjectCase, run_paxsy

U = "use std::inout;\n"

CASES = {
    "hello": U + "func main(): Void { std::inout::println(\"hi\"); }\n",
    "arith": U + "func main(): Void { let x: Int = 6; let y: Int = x * 7 + 2; "
                 "std::inout::println(\"y={y}\"); }\n",
    "fib":   U + "func fib(n: Int): Int { if n < 2 { return n; } "
                 "return fib(n-1) + fib(n-2); }\n"
                 "func main(): Void { std::inout::println(\"f={fib(10)}\"); }\n",
    "real":  U + "func main(): Void { let x: Real = 1.5; "
                 "let y: Real = x * 2.0 + 0.25; "
                 "std::inout::println(\"v={(y as Int)}\"); }\n",
    "struct": U + "static P = Struct { x: Int, y: Int };\n"
                  "func main(): Void { let p: P = P {1, 2}; p.y = 10; "
                  "std::inout::println(\"v={p.x + p.y}\"); }\n",
    "view":  U + "func main(): Void { let a: Int[5] = {1,2,3,4,5}; "
                 "let v: Int[..] = a[1..3]; "
                 "std::inout::println(\"n={v.len}\"); }\n",
}

# Exact .text sizes (bytes) of the canonical programs above. A change
# here means the compiler emits different code: review the delta, and
# only update the table when the new code is strictly better.
GOLDEN_TEXT_SIZES = {
    "hello": 1112,
    "arith": 1502,
    "fib": 1619,
    "real": 1566,
    "struct": 1626,
    "view": 1638,
}


def text_size(path):
    out = subprocess.run(["readelf", "-S", str(path)],
                           capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "] .text" in line:
            return int(lines[i + 1].split()[0], 16)
    raise AssertionError(".text section not found")


def text_disasm(path):
    out = subprocess.run(["objdump", "-d", str(path)],
                         capture_output=True, text=True, check=True).stdout
    return [l for l in out.splitlines() if "\t" in l]


class SizeAndDeadCodeTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def test_text_size_golden(self):
        for name, src in CASES.items():
            with self.subTest(case=name):
                (self.proj / "src/main.px").write_text(src)
                r = run_paxsy("build", cwd=self.proj)
                self.assertEqual(r.returncode, 0, r.stderr)
                got = text_size(self.proj / "build" / "demo")
                self.assertEqual(got, GOLDEN_TEXT_SIZES[name],
                                 f".text grew or shrank: {got} != "
                                 f"{GOLDEN_TEXT_SIZES[name]} for {name}")

    def test_unreachable_functions_are_not_emitted(self):
        src = ("func unused_a(): Int { return 1; }\n"
               "func unused_b(): Int { return 2; }\n"
               "func used(): Int { return 3; }\n"
               "func main(): Void { let x: Int = used(); }\n")
        (self.proj / "src/main.px").write_text(src)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        symbols = subprocess.run(
            ["readelf", "-s", str(self.proj / "build/demo")],
            capture_output=True, text=True, check=True).stdout
        self.assertIn("used", symbols)
        self.assertIn("main", symbols)
        self.assertNotIn("unused_a", symbols)
        self.assertNotIn("unused_b", symbols)

    def test_inline_asm_call_keeps_target_function(self):
        src = ("module lib {\n"
               "    func asm_target(): Void { return; }\n"
               "    func unused_target(): Void { return; }\n"
               "}\n"
               "func main(): Void { asm \"entry: call lib::asm_target\"; }\n")
        (self.proj / "src/main.px").write_text(src)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        symbols = subprocess.run(
            ["readelf", "-s", str(self.proj / "build/demo")],
            capture_output=True, text=True, check=True).stdout
        self.assertIn("lib::asm_target", symbols)
        self.assertNotIn("lib::unused_target", symbols)

    def test_no_dead_immediate_rewrite(self):
        # Two immediate writes to the same register in a row: the
        # first is dead code that must have been eliminated.
        src = CASES["arith"]
        (self.proj / "src/main.px").write_text(src)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        ins = text_disasm(self.proj / "build" / "demo")
        imm_re = re.compile(
            r"mov\s+\$(-?0x[0-9a-f]+|-?\d+),%r(ax|bx|cx|dx|di|si|bp|sp|8|9|10|11|12|13|14|15)\b")
        seq_re = re.compile(r"mov\s+%r(ax|bx|cx|dx|di|si|bp|sp|8|9|10|11|12|13|14|15),\s*%r\1\b")
        prev = None
        prev_seq = None
        for line in ins:
            m = imm_re.search(line)
            if m and prev is not None:
                pm, pline = prev
                self.assertFalse(
                    pm.group(2) == m.group(2),
                    f"dead immediate rewrite of %{m.group(2)}:\n"
                    f"  {pline.strip()}\n  {line.strip()}")
            prev = (m, line) if m else None
            if seq_re.search(line):
                self.assertIsNot(prev_seq, line,
                                 f"self-move left in code: {line.strip()}")

    def test_semantics_preserved_by_peephole(self):
        # Programs whose codegen historically carried redundant moves
        # must still produce the same results with the peephole on.
        checks = [
            (CASES["arith"], "y=44\n"),
            (CASES["fib"], "f=55\n"),
            (CASES["real"], "v=3\n"),
        ]
        for src, expected in checks:
            (self.proj / "src/main.px").write_text(src)
            r = run_paxsy("build", cwd=self.proj)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = run_paxsy("run", cwd=self.proj)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, expected)


if __name__ == "__main__":
    unittest.main()
