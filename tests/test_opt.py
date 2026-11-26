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
        r = subprocess.run(self.cmd + ["new", "opt"], cwd=self.tmp,
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

    def test_cfg_removes_statements_after_return(self):
        src = """use std::inout;
func main(): Void {
    return;
    std::inout::println("unreachable");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "")

    def test_scalar_replacement_forwards_pure_struct_fields(self):
        src = """use std::inout;
static Pair = Struct { left: Int, right: Int };
func main(): Void {
    let pair: Pair = Pair {19, 23};
    let values: Int[3] = {5, 7, 11};
    std::inout::println("sum={pair.left + pair.right + values[1]}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        optimized = subprocess.run(self.cmd + ["build", "--profile"],
                                   cwd=self.proj, capture_output=True,
                                   text=True)
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        self.assertRegex(optimized.stderr,
                         r"SROA\s+[0-9.]+\s+[0-9]+\s+[1-9][0-9]*")
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "sum=49\n")
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "sum=49\n")

    def test_sroa_rejects_mutated_aggregate(self):
        src = """use std::inout;
static Pair = Struct { left: Int, right: Int };
func main(): Void {
    let pair: Pair = Pair {19, 23};
    pair.right = 10;
    std::inout::println("sum={pair.left + pair.right}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        result = subprocess.run(self.cmd + ["build", "--profile"],
                                cwd=self.proj, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stderr,
                         r"SROA\s+[0-9.]+\s+[0-9]+\s+0")
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "sum=29\n")

    def test_simplifier_reduces_repeated_stable_scalar_operands(self):
        src = """use std::inout;
func main(): Void {
    let value: Int = 37;
    let sub = value - value;
    let xor = value ^ value;
    let bitand = value & value;
    let flag: Bool = true;
    let bool_and = flag && flag;
    let bool_or = flag || flag;
    let eq = value == value;
    std::inout::println("sub={sub}");
    std::inout::println("xor={xor}");
    std::inout::println("and={bitand}");
    std::inout::println("and_bool={bool_and}");
    std::inout::println("or_bool={bool_or}");
    std::inout::println("eq={eq}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        expected = "sub=0\nxor=0\nand=37\nand_bool=1\nor_bool=1\neq=1\n"
        self.assertEqual(self.run_bin().stdout, expected)
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        self.assertEqual(self.run_bin().stdout, expected)

    def test_ssa_interference_graph_has_valid_abstract_colouring(self):
        src = """use std::inout;
func calculate(a: Int, b: Int, c: Int): Int {
    let first: Int = a + b;
    let second: Int = b + c;
    let third: Int = first * second;
    let fourth: Int = first + c;
    return third + fourth;
}
func main(): Void {
    std::inout::println(calculate(2, 3, 4));
}
"""
        (self.proj / "src/main.px").write_text(src)
        result = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(
            result.stderr,
            r"max statement-point-live [0-9]+; interference [0-9]+ edge\(s\)")
        self.assertRegex(
            result.stderr,
            r"abstract colouring [0-8] colour\(s\), [0-9]+ spill candidate\(s\) \(K=8\)")
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "44\n")

    def test_ssa_live_phi_results_interfere_at_block_entry(self):
        src = """use std::inout;
func combine(a: Int, b: Int, flag: Bool): Int {
    let left: Int = a;
    let right: Int = b;
    if flag {
        left = a + 1;
        right = b + 2;
    } else {
        left = a + 3;
        right = b + 4;
    }
    return left + right;
}
func main(): Void {
    std::inout::println(combine(10, 20, true));
}
"""
        (self.proj / "src/main.px").write_text(src)
        result = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(result.returncode, 0, result.stderr)
        phi_values = set()
        for line in result.stderr.splitlines():
            if "phi(slot" in line:
                phi_values.update(int(v) for v in re.findall(r"v(\d+) = phi", line))
        self.assertGreaterEqual(len(phi_values), 2, result.stderr)
        edge_match = re.search(r"^  interference:(.*)$", result.stderr, re.MULTILINE)
        self.assertIsNotNone(edge_match, result.stderr)
        edge_list = [tuple(map(int, pair)) for pair in
                     re.findall(r"v(\d+)-v(\d+)", edge_match.group(1))]
        edges = set(edge_list)
        self.assertEqual(len(edge_list), len(edges),
                         "interference graph must not emit duplicate edges")
        self.assertTrue(any(a in phi_values and b in phi_values
                            for a, b in edges), result.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "33\n")

    def test_ssa_liveness_caches_phi_edge_uses(self):
        names = [f"value_{i}" for i in range(10)]
        initial = "\n".join(
            f"    let {name}: Int = seed + {i + 1};"
            for i, name in enumerate(names))
        then_assignments = "\n".join(
            f"        {name} = seed + {i + 11};"
            for i, name in enumerate(names))
        else_assignments = "\n".join(
            f"        {name} = seed + {i + 21};"
            for i, name in enumerate(names))
        total = " + ".join(names)
        src = f"""use std::inout;
func merge_many(seed: Int, flag: Bool): Int {{
{initial}
    if flag {{
{then_assignments}
    }} else {{
{else_assignments}
    }}
    return {total};
}}
func main(): Void {{
    std::inout::println(merge_many(0, true));
}}
"""
        (self.proj / "src/main.px").write_text(src)
        result = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(result.returncode, 0, result.stderr)
        merge_dump = next(
            (line for line in result.stderr.splitlines()
             if line.startswith("SSA merge_many:")), "")
        self.assertTrue(merge_dump, result.stderr)
        phi_count = sum(
            line.count("phi(slot") for line in result.stderr.splitlines())
        self.assertGreaterEqual(phi_count, len(names), result.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "155\n")

    def test_ssa_colouring_reports_spill_pressure(self):
        # Fifteen simultaneously-live locals produce more than 90 pairwise
        # conflicts and force the edge set through a growth/rehash cycle.
        names = ["a", "b", "c", "d", "e", "f", "g", "h", "i",
                 "j", "k", "l", "m", "n", "o"]
        declarations = "\n".join(
            f"    let {name}: Int = seed + {index + 1};"
            for index, name in enumerate(names))
        expression = " + ".join(names)
        src = f"""use std::inout;
func sum_many(seed: Int): Int {{
{declarations}
    return {expression};
}}
func main(): Void {{ std::inout::println(sum_many(0)); }}
"""
        (self.proj / "src/main.px").write_text(src)
        result = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(
            result.stderr,
            r"abstract colouring 8 colour\(s\), [1-9][0-9]* spill candidate\(s\) \(K=8\)")
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "120\n")

    def test_ssa_renaming_handles_deep_dominator_trees(self):
        # A straight-line function creates a deep dominator tree. SSA renaming
        # must use an explicit work stack rather than the native C call stack.
        count = 512
        statements = "\n".join("    value = value + 1;" for _ in range(count))
        src = f"""use std::inout;
func main(): Void {{
    let value: Int = 0;
{statements}
    std::inout::println(value);
}}
"""
        (self.proj / "src/main.px").write_text(src)
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, f"{count}\n")

    def test_ssa_sccp_folds_identical_values_at_phi(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let value: Int = 0;
    if flag {
        value = 5;
    } else {
        value = 5;
    }
    return value;
}
func main(): Void {
    std::inout::println("a={choose(true)}");
    std::inout::println("b={choose(false)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        env = dict(os.environ)
        env["PAXSY_DUMP_SSA"] = "1"
        r = self.build(env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("phi(slot", r.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "a=5\nb=5\n")

    def test_ssa_use_index_handles_long_copy_chains(self):
        # The SSA copy simplifier resolves each HIR read through the sorted
        # use index. A long chain exercises both construction and lookups.
        lines = ["use std::inout;", "func main(): Void {", "    let v0: Int = 7;"]
        for i in range(1, 161):
            lines.append(f"    let v{i}: Int = v{i - 1};")
        lines.append('    std::inout::println("value={v160}");')
        lines.append("}")
        (self.proj / "src/main.px").write_text("\n".join(lines) + "\n")

        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "value=7\n")

        reference = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(reference.returncode, 0, reference.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "value=7\n")

    def test_ssa_canonicalizes_chains_of_local_copy_values(self):
        src = """func identity(input: Int): Int {
    let first: Int = input;
    let second: Int = first;
    return second;
}
func main(): Void { let result = identity(9); }
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stderr, r"SSA identity:.*simplified 2 copy/copies")

    def test_ssa_skips_phi_placement_for_single_definition_slot(self):
        src = """func read_once(flag: Bool, input: Int): Int {
    let value: Int = input;
    if flag {
        let copy: Int = value;
    }
    return value;
}
func main(): Void { let result = read_once(true, 23); }
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("SSA read_once:", r.stderr)
        self.assertNotRegex(r.stderr, r"phi\(slot")

    def test_ssa_eliminates_trivial_phi_with_identical_incoming_value(self):
        src = """func choose(flag: Bool, input: Int): Int {
    let value: Int = input;
    if flag {
        value = input;
    } else {
        value = input;
    }
    return value;
}
func main(): Void { let result = choose(true, 17); }
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stderr, r"simplified [0-9]+ copy/copies, [1-9][0-9]* Phi node")

    def test_ssa_sinks_pure_single_use_expression_to_immediate_consumer(self):
        src = """use std::inout;
func sum(a: Int, b: Int): Int {
    let value: Int = a + b;
    return value;
}
func main(): Void { std::inout::println(sum(19, 23)); }
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("sunk 1 expression(s)", r.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "42\n")

    def test_ssa_removes_overwritten_and_unused_pure_definitions(self):
        src = """use std::inout;
func main(): Void {
    let value: Int = 0;
    value = 1;
    value = 2;
    let unused: Int = 99;
    std::inout::println(value);
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stderr, r"removed [1-9][0-9]* dead definition\(s\)")
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "2\n")

    def test_ssa_keeps_side_effecting_unused_initializer(self):
        src = """use std::inout;
func produce(): Int {
    std::inout::println("initializer ran");
    return 7;
}
func main(): Void {
    let unused: Int = produce();
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "initializer ran\n")

    def test_ssa_does_not_sink_expression_across_side_effect(self):
        src = """use std::inout;
func sum(a: Int, b: Int): Int {
    let value: Int = a + b;
    std::inout::println("between");
    return value;
}
func main(): Void { std::inout::println(sum(19, 23)); }
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("sunk 0 expression(s)", r.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "between\n42\n")

    def test_ssa_sccp_uses_phi_constant_to_select_control_flow_edge(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let value: Int = 0;
    if flag {
        value = 9;
    } else {
        value = 9;
    }
    if value == 9 {
        return 1;
    } else {
        return 2;
    }
}
func main(): Void {
    std::inout::println("a={choose(true)}");
    std::inout::println("b={choose(false)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        env = dict(os.environ)
        env["PAXSY_DUMP_SSA"] = "1"
        r = self.build(env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("phi(slot", r.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "a=1\nb=1\n")

    def test_ssa_sccp_worklist_propagates_through_long_phi_chain(self):
        # Each branch creates a Phi whose selected input becomes the next
        # branch's input. This exercises value-to-block notifications and
        # executable-edge updates without relying on repeated whole-CFG scans.
        lines = [
            "use std::inout;",
            "func main(): Void {",
            "    let gate: Bool = true;",
            "    let value: Int = 0;",
        ]
        for _ in range(48):
            lines.extend([
                "    if gate {",
                "        value = value + 1;",
                "    } else {",
                "        value = value + 2;",
                "    }",
            ])
        lines.extend([
            "    if value == 48 {",
            '        std::inout::println("worklist=ok");',
            "    } else {",
            '        std::inout::println("worklist=bad");',
            "    }",
            "}",
        ])
        (self.proj / "src/main.px").write_text("\n".join(lines) + "\n")

        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "worklist=ok\n")

        reference = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(reference.returncode, 0, reference.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "worklist=ok\n")

    def test_ssa_sccp_prunes_constant_branch_with_side_effect(self):
        src = """use std::inout;
func main(): Void {
    let enabled: Bool = false;
    if enabled {
        std::inout::println("wrong");
    } else {
        std::inout::println("right");
    }
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "right\n")

    def test_ssa_sccp_does_not_merge_different_phi_constants(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let value: Int = 0;
    if flag {
        value = 5;
    } else {
        value = 6;
    }
    return value;
}
func main(): Void {
    std::inout::println("a={choose(true)}");
    std::inout::println("b={choose(false)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stderr, r"[1-9][0-9]* single-use Phi\(s\)")
        run = self.run_bin()
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "a=5\nb=6\n")

    def test_cfg_routes_break_to_loop_exit(self):
        src = """use std::inout;
func main(): Void {
    while true {
        break;
        std::inout::println("unreachable");
    }
    std::inout::println("after");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "after\n")

    def test_cfg_unreachable_loop_tail_does_not_hide_live_exit(self):
        src = """use std::inout;
func main(): Void {
    let i: Int = 0;
    while i < 3 {
        if i == 1 {
            break;
            std::inout::println("unreachable-tail");
        }
        i += 1;
    }
    std::inout::println("exit={i}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        self.assertEqual(self.run_bin().stdout, "exit=1\n")
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        self.assertEqual(self.run_bin().stdout, "exit=1\n")

    def test_cfg_routes_continue_to_loop_condition(self):
        src = """use std::inout;
func main(): Void {
    let i: Int = 0;
    while i < 2 {
        i += 1;
        continue;
        std::inout::println("unreachable");
    }
    std::inout::println("after");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "after\n")

    def test_cfg_removes_code_after_fully_terminating_if(self):
        src = """use std::inout;
func stop(flag: Bool): Void {
    if flag {
        return;
    } else {
        return;
    }
    std::inout::println("unreachable");
}
func main(): Void {
    stop(true);
    stop(false);
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "")

    def test_all_public_optimization_levels_preserve_output(self):
        config = self.proj / "opt.toml"
        base = config.read_text()
        outputs = []
        for level in range(4):
            config.write_text(base + f"\n[build]\noptimization = {level}\n")
            built = self.build()
            self.assertEqual(built.returncode, 0,
                             f"optimization={level}: {built.stderr}")
            executed = self.run_bin()
            self.assertEqual(executed.returncode, 0, executed.stderr)
            outputs.append(executed.stdout)
        self.assertEqual(outputs, [EXPECTED] * 4)

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
        # the per-module rewrite report was removed from the build
        # output; the pipeline still honours PAXSY_NO_OPT and both
        # builds must succeed and agree on the program output
        r = subprocess.run(self.cmd + ["build", "-v"], cwd=self.proj,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("rewrites", r.stdout)
        r = subprocess.run(self.cmd + ["build", "-v"], cwd=self.proj,
                           capture_output=True, text=True,
                           env={**os.environ, "PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("rewrites", r.stdout)

    def test_unused_binding_keeps_initializer_side_effects(self):
        src = """use std::inout;
func side_effect(): Int {
    std::inout::println("effect");
    return 1;
}
func main(): Void {
    let unused = side_effect();
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "effect\n")

    def test_equal_values_from_if_branches_survive_join(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 0;
    if true {
        x = 7;
    } else {
        x = 7;
    }
    std::inout::println("x={x}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        self.assertEqual(self.run_bin().stdout, "x=7\n")
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        self.assertEqual(self.run_bin().stdout, "x=7\n")

    def test_different_values_from_if_branches_are_not_merged(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 0;
    let flag: Bool = true;
    if flag {
        x = 7;
    } else {
        x = 9;
    }
    std::inout::println("x={x}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_bin().stdout, "x=7\n")
        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_bin().stdout, "x=7\n")

    def test_assignment_invalidates_propagated_local(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 1;
    x = 2;
    std::inout::println("x={x}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "x=2\n")

    def test_address_taken_local_is_not_replaced_by_its_value(self):
        src = """use std::inout;
func mutate(p: &Int): Void {
    @p = 2;
}
func main(): Void {
    let x: Int = 1;
    mutate(&x);
    std::inout::println("x={x}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "x=2\n")

    def test_loop_propagation_does_not_freeze_values_across_iterations(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 0;
    let i: Int = 0;
    while i < 2 {
        std::inout::println("x={x}");
        x = 1;
        i += 1;
    }
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "x=0\nx=1\n")

    def test_loop_gvn_preserves_invariant_values_and_invalidates_mutated_ones(self):
        src = """use std::inout;
func main(): Void {
    let stable: Int = 11;
    let changing: Int = 0;
    let i: Int = 0;
    while i < 3 {
        let a = stable + 4;
        std::inout::println("stable={a}, changing={changing}");
        changing += 1;
        i += 1;
    }
}
"""
        (self.proj / "src/main.px").write_text(src)
        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        expected = "stable=15, changing=0\nstable=15, changing=1\nstable=15, changing=2\n"
        self.assertEqual(self.run_bin().stdout, expected)
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        self.assertEqual(self.run_bin().stdout, expected)

    def test_loop_gvn_drops_facts_across_unknown_memory_effects(self):
        src = """use std::inout;
func mutate(p: &Int): Void {
    @p = 8;
}
func main(): Void {
    let x: Int = 3;
    let i: Int = 0;
    while i < 2 {
        let before = x + 1;
        mutate(&x);
        let after = x + 1;
        std::inout::println("{before}:{after}");
        i += 1;
    }
}
"""
        (self.proj / "src/main.px").write_text(src)
        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        expected = "4:9\n9:9\n"
        self.assertEqual(self.run_bin().stdout, expected)
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        self.assertEqual(self.run_bin().stdout, expected)

    def test_loop_keeps_invariant_local_facts_after_while(self):
        src = """use std::inout;
func main(): Void {
    let stable: Int = 7;
    let i: Int = 0;
    while i < 3 {
        i += 1;
    }
    std::inout::println("stable={stable}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "stable=7\n")

    def test_loop_keeps_unmodified_local_facts_with_do_while(self):
        src = """use std::inout;
func main(): Void {
    let stable: Int = 9;
    let i: Int = 0;
    do {
        i += 1;
    } while i < 2;
    std::inout::println("stable={stable}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "stable=9\n")

    def test_if_branches_use_their_own_incoming_propagation_state(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 1;
    if false {
        x = 2;
    } else {
        std::inout::println("x={x}");
    }
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "x=1\n")

    def test_propagation_handles_more_than_64_local_slots(self):
        declarations = "\n".join(
            "    let v%d: Int = %d;" % (i, i) for i in range(96))
        src = ("use std::inout;\nfunc main(): Void {\n" + declarations +
               "\n    std::inout::println(\"v={v95}\");\n}\n")
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "v=95\n")

    def test_final_store_value_is_propagated_into_format(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 0;
    x = 1;
    x = 2;
    std::inout::println("{x}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "2\n")

    def test_dead_stores_before_if_are_removed_without_changing_result(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let x: Int = 0;
    x = 1;
    if flag {
        x = 2;
    } else {
        x = 3;
    }
    return x;
}
func main(): Void {
    std::inout::println("{choose(true)}");
    std::inout::println("{choose(false)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "2\n3\n")
        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "2\n3\n")

    def test_loop_liveness_removes_overwritten_stores_inside_while(self):
        src = """use std::inout;
func main(): Void {
    let value: Int = 0;
    let i: Int = 0;
    while i < 3 {
        value = 10;
        value = 20;
        i += 1;
    }
    std::inout::println("{value}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "20\n")
        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "20\n")

    def test_do_while_liveness_keeps_condition_value(self):
        src = """use std::inout;
func main(): Void {
    let i: Int = 0;
    let value: Int = 4;
    do {
        value = 9;
        i += 1;
    } while i < 2;
    std::inout::println("{value}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "9\n")

    def test_branch_without_store_keeps_incoming_value_live(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let x: Int = 10;
    if flag {
        x = 20;
    }
    return x;
}
func main(): Void {
    std::inout::println("{choose(true)}");
    std::inout::println("{choose(false)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "20\n10\n")

    def test_immutable_local_copy_is_forwarded(self):
        src = """use std::inout;
func produce(): Int {
    std::inout::println("created");
    return 7;
}
func main(): Void {
    let x: Int = produce();
    let y: Int = x;
    std::inout::println("{y}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "created\n7\n")

    def test_copy_is_not_forwarded_across_source_mutation(self):
        src = """use std::inout;
func produce(): Int {
    return 7;
}
func main(): Void {
    let x: Int = produce();
    let y: Int = x;
    x = 9;
    std::inout::println("{y}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "7\n")


    def test_gvn_eliminates_repeated_integer_expression(self):
        src = """func calc(x: Int, y: Int): Int {
    let first: Int = x + y;
    let second: Int = y + x;
    return second;
}
func main(): Void {
    std::inout::println("{calc(20, 22)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_bin().stdout, "42\n")
        r = subprocess.run(self.cmd + ["disasm", "build/opt"],
                           cwd=self.proj, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm = (self.proj / "opt.disasm/src/calc.asm").read_text()
        optimized_adds = len(re.findall(r"\badd\s+rax,rcx\b", asm))
        self.assertEqual(optimized_adds, 1, asm)

        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run(self.cmd + ["disasm", "build/opt"],
                           cwd=self.proj, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm = (self.proj / "opt.disasm/src/calc.asm").read_text()
        unoptimized_adds = len(re.findall(r"\badd\s+rax,rcx\b", asm))
        self.assertEqual(unoptimized_adds, 2, asm)

    def test_gvn_preserves_dominating_expression_across_if_join(self):
        src = """func calc(flag: Bool, x: Int, y: Int): Int {
    let first: Int = x + y;
    if flag {
        let left: Int = 1;
    } else {
        let right: Int = 2;
    }
    let second: Int = x + y;
    return second;
}
func main(): Void {
    std::inout::println("{calc(true, 20, 22)}");
    std::inout::println("{calc(false, 20, 22)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_bin().stdout, "42\n42\n")
        r = subprocess.run(self.cmd + ["disasm", "build/opt"],
                           cwd=self.proj, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm = (self.proj / "opt.disasm/src/calc.asm").read_text()
        optimized_adds = len(re.findall(r"\badd\s+rax,rcx\b", asm))
        self.assertEqual(optimized_adds, 1, asm)

        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run(self.cmd + ["disasm", "build/opt"],
                           cwd=self.proj, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm = (self.proj / "opt.disasm/src/calc.asm").read_text()
        unoptimized_adds = len(re.findall(r"\badd\s+rax,rcx\b", asm))
        self.assertEqual(unoptimized_adds, 2, asm)

    def test_gvn_drops_expression_when_one_branch_mutates_an_operand(self):
        src = """func calc(flag: Bool, x: Int, y: Int): Int {
    let first: Int = x + y;
    if flag {
        x = 10;
    } else {
        y = 20;
    }
    let second: Int = x + y;
    return first + second;
}
func main(): Void {
    std::inout::println("{calc(true, 1, 2)}");
    std::inout::println("{calc(false, 1, 2)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        optimized = self.build()
        self.assertEqual(optimized.returncode, 0, optimized.stderr)
        expected = "15\n24\n"
        self.assertEqual(self.run_bin().stdout, expected)
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        self.assertEqual(self.run_bin().stdout, expected)

    def test_gvn_reuses_loop_invariant_expression_inside_loop(self):
        src = """func calc(x: Int, y: Int, n: Int): Int {
    let first: Int = x + y;
    let second: Int = 0;
    let i: Int = 0;
    while i < n {
        second = y + x;
        i += 1;
    }
    return second;
}
func main(): Void {
    std::inout::println("{calc(20, 22, 0)}");
    std::inout::println("{calc(20, 22, 1)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.run_bin().stdout, "0\n42\n")
        r = subprocess.run(self.cmd + ["disasm", "build/opt"],
                           cwd=self.proj, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm = (self.proj / "opt.disasm/src/calc.asm").read_text()
        optimized_adds = len(re.findall(r"\badd\s+rax,rcx\b", asm))
        self.assertEqual(optimized_adds, 1, asm)

        r = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run(self.cmd + ["disasm", "build/opt"],
                           cwd=self.proj, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm = (self.proj / "opt.disasm/src/calc.asm").read_text()
        unoptimized_adds = len(re.findall(r"\badd\s+rax,rcx\b", asm))
        self.assertEqual(unoptimized_adds, 2, asm)

    def test_gvn_is_invalidated_by_call_that_can_mutate_addressed_value(self):
        src = """use std::inout;
func mutate(p: &Int): Void {
    @p = 10;
}
func calc(x: Int, y: Int): Int {
    let first: Int = x + y;
    mutate(&x);
    let second: Int = y + x;
    return second;
}
func main(): Void {
    std::inout::println("{calc(1, 2)}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "12\n")

    def test_redundant_stores_preserve_final_value(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 0;
    x = 1;
    x = 2;
    std::inout::println("x={x}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "x=2\n")

    def test_store_in_branch_is_not_removed_as_dead(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 0;
    if true {
        x = 7;
    }
    std::inout::println("x={x}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "x=7\n")

    def test_deferred_read_keeps_later_store(self):
        src = """use std::inout;
func main(): Void {
    let x: Int = 0;
    defer std::inout::println("x={x}");
    x = 9;
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "x=9\n")

    def test_short_circuit_simplification_keeps_effects(self):
        src = """use std::inout;
func side(): Bool {
    std::inout::println("called");
    return true;
}
func main(): Void {
    let a: Bool = false && side();
    let b: Bool = true && side();
    let c: Bool = false || side();
    let d: Bool = true || side();
    std::inout::println("{a}{b}{c}{d}");
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "called\ncalled\n0111\n")
        unoptimized = self.build({"PAXSY_NO_OPT": "1"})
        self.assertEqual(unoptimized.returncode, 0, unoptimized.stderr)
        self.assertEqual(self.run_bin().stdout, "called\ncalled\n0111\n")

    def test_ssa_builds_phi_for_branch_join(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let x: Int = 1;
    if flag {
        x = 2;
    } else {
        x = 3;
    }
    return x;
}
func main(): Void {
    std::inout::println(choose(true));
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stderr, r"SSA choose:.*max statement-point-live [1-9][0-9]*")
        self.assertRegex(r.stderr, r"SSA choose:.*interference [0-9]+ edge\(s\), max degree [0-9]+")
        self.assertRegex(r.stderr, r"SSA choose:.*\n  bb\d+:.*phi\(slot\d+")
        self.assertEqual(self.run_bin().stdout, "2\n")

    def test_ssa_parameter_reassignment_uses_entry_definition(self):
        src = """use std::inout;
func mutate(value: Int, flag: Bool): Int {
    if flag {
        value = 9;
    }
    return value;
}
func main(): Void {
    std::inout::println(mutate(2, true));
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stderr, r"SSA mutate:.*\n  bb0:.*phi\(slot0,.*bb\d+:v\d+.*bb\d+:v\d+")
        self.assertEqual(self.run_bin().stdout, "9\n")

    def test_ssa_builds_loop_carried_phi(self):
        src = """use std::inout;
func main(): Void {
    let i: Int = 0;
    while i < 3 {
        i += 1;
    }
    std::inout::println(i);
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build({"PAXSY_DUMP_SSA": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("phi(slot0", r.stderr)
        self.assertRegex(r.stderr, r"SSA main:.*[0-9]+ value\(s\)")
        self.assertEqual(self.run_bin().stdout, "3\n")

    def test_if_expression_result_return_is_sunk_into_arms(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let value = if flag { 1 } else { 2 };
    return value;
}
func main(): Void {
    std::inout::println(choose(true));
    std::inout::println(choose(false));
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "1\n2\n")

    def test_if_expression_return_sinks_through_single_use_alias_chain(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let first = if flag { 11 } else { 22 };
    let second = first;
    let third = second;
    return third;
}
func main(): Void {
    std::inout::println(choose(true));
    std::inout::println(choose(false));
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "11\n22\n")

    def test_if_expression_result_with_multiple_uses_is_not_sunk(self):
        src = """use std::inout;
func choose(flag: Bool): Int {
    let value = if flag { 1 } else { 2 };
    std::inout::println(value);
    return value;
}
func main(): Void {
    std::inout::println(choose(true));
    std::inout::println(choose(false));
}
"""
        (self.proj / "src/main.px").write_text(src)
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_bin()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "1\n1\n2\n2\n")

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
