#!/usr/bin/env python3
"""Regression tests for compile-time type parameters and aliases."""
import unittest
import subprocess
from paxsy_testlib import ProjectCase, run_paxsy


class StaticTests(ProjectCase):
    def test_static_constant_expansion(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "static BASE = 6;\n"
            "static VALUE = BASE * 7;\n"
            "func value(): Int { return VALUE; }\n"
            "func main(): Void { std::inout::println(value()); }\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)
        executed = subprocess.run([str(project / "build/demo")],
                                  capture_output=True, text=True)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        self.assertEqual(executed.stdout, "42\n")

    def test_static_type_parameter(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "func identity(static T, value: T): T { return value; }\n"
            "func main(): Void { let value: Int32 = identity(Int32, 42); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_comptime_is_no_longer_a_reserved_keyword(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "func main(): Void { let comptime: Int = 42; }\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_global_type_alias_is_a_type(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "static T = Int;\n"
            "func main(): Void { let value: T = 17; }\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertTrue((project / "build/demo").is_file())

    def test_explicit_static_type_argument(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "func calc(static T, a, b): T { return a + b; }\n"
            "func main(): Void { let value: Int = calc(Int, 10, 20); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertTrue((project / "build/demo").is_file())

    def test_auto_static_type_inference_and_override(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "func calc(a, b, static T=Auto): T { return a + b; }\n"
            "func main(): Void {\n"
            " let first: Int = calc(10, 20);\n"
            " let second: Int = calc(10, 20, Int);\n"
            "}\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_generic_specialization_keeps_declaring_module_scope(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "module helpers {\n"
            "    func increment(value: Int32): Int32 { return value + 1; }\n"
            "}\n"
            "module numbers {\n"
            "    use helpers;\n"
            "    func apply(static T, value: T): T { return increment(value); }\n"
            "}\n"
            "func main(): Void { let result: Int32 = numbers::apply(Int32, 41); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_catch_handler_error_is_a_diverging_path(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "func calc(static T, a, b): T {\n"
            " return a + b catch |e| { error \"{e}\"; };\n"
            "}\n"
            "func main(): Void { let value: Int = calc(Int, 1, 2); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_auto_infers_type_from_typed_variables(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "func calc(a: T, b: T, static T=Auto): T { return a + b; }\n"
            "func main(): Void {\n"
            " let first: Real64 = 1.5;\n"
            " let second: Real64 = 2.5;\n"
            " let total: Real64 = calc(first, second);\n"
            "}\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_forward_type_alias_chain_resolves(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "static First = Second;\n"
            "static Second = Int;\n"
            "func main(): Void { let value: First = 9; }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_static_value_alias_is_not_mistaken_for_a_type(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "static BASE = 7;\n"
            "static VALUE = BASE;\n"
            "func main(): Void { let value: Int = VALUE; }\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_static_integer_expressions_expand_across_use_sites(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "static BASE = 6;\n"
            "static SCALE = BASE * 7 + 0;\n"
            "func result(): Int { return SCALE; }\n"
            "func main(): Void { std::inout::println(result()); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)
        executed = subprocess.run([str(project / "build/demo")],
                                  capture_output=True, text=True)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        self.assertEqual(executed.stdout, "42\n")
    def test_static_constant_expansion_is_independent_of_runtime_optimisation(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "static BASE = 9;\n"
            "static VALUE = BASE * 5 - 3;\n"
            "func result(): Int { return VALUE; }\n"
            "func main(): Void { std::inout::println(result()); }\n")
        built = run_paxsy("build", project, cwd=self.tmp,
                          env={"PAXSY_NO_OPT": "1"})
        self.assertEqual(built.returncode, 0, built.stderr)
        executed = subprocess.run([str(project / "build/demo")],
                                  capture_output=True, text=True)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        self.assertEqual(executed.stdout, "42\n")

    def test_cyclic_static_constants_are_diagnosed(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "static FIRST: Int = SECOND;\n"
            "static SECOND: Int = FIRST;\n"
            "func main(): Void { let value: Int = FIRST; }\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertNotEqual(checked.returncode, 0)
        self.assertIn("cyclic static constant reference", checked.stderr)

    def test_unused_static_constants_do_not_leave_global_storage(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "static BASE = 6;\n"
            "static SCALE = BASE * 7;\n"
            "func value(): Int { return SCALE; }\n"
            "func main(): Void { std::inout::println(value()); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

        baseline = self.tmp / "baseline"
        created = run_paxsy("new", "baseline", cwd=self.tmp)
        self.assertEqual(created.returncode, 0, created.stderr)
        (baseline / "src/main.px").write_text(
            "use std::inout;\n"
            "func value(): Int { return 42; }\n"
            "func main(): Void { std::inout::println(value()); }\n")
        built = run_paxsy("build", baseline, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

        def data_size(binary):
            output = subprocess.run(["readelf", "-SW", str(binary)],
                                    capture_output=True, text=True, check=True).stdout
            for line in output.splitlines():
                fields = line.split()
                if len(fields) > 6 and fields[2] == ".data":
                    return int(fields[6], 16)
            self.fail(".data section not found")

        self.assertEqual(data_size(project / "build/demo"),
                         data_size(baseline / "build/baseline"))

    def test_static_float_expression_is_evaluated_at_compile_time(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "static HALF = 1.5;\n"
            "static TOTAL = HALF + 2.5;\n"
            "func value(): Int64 { return TOTAL as Int64; }\n"
            "func main(): Void { std::inout::println(value()); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)
        executed = subprocess.run([str(project / "build/demo")],
                                  capture_output=True, text=True)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        self.assertEqual(executed.stdout, "4\n")

    def test_static_constants_expand_across_named_modules(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "module constants { static BASE = 21; }\n"
            "use constants;\n"
            "static VALUE = BASE * 2;\n"
            "func value(): Int { return VALUE; }\n"
            "func main(): Void { std::inout::println(value()); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)
        executed = subprocess.run([str(project / "build/demo")],
                                  capture_output=True, text=True)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        self.assertEqual(executed.stdout, "42\n")

    def test_taking_static_constant_address_preserves_storage(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "static VALUE = 42;\n"
            "func value(): Int8 {\n"
            " let pointer: &Int8 = &VALUE;\n"
            " return @pointer;\n"
            "}\n"
            "func main(): Void { std::inout::println(\"{value()}\"); }\n")
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)
        executed = subprocess.run([str(project / "build/demo")],
                                  capture_output=True, text=True)
        self.assertEqual(executed.returncode, 0, executed.stderr)
        self.assertEqual(executed.stdout, "42\n")
    def test_user_generic_catch_example_is_accepted(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "use std::inout;\n"
            "func calc(static T, n1, n2): T {\n"
            "    return n1 + n2 catch |e| { error \"{e}\"; };\n"
            "}\n"
            "func main(): Void {\n"
            "    let num = 5;\n"
            "    let result: Int = calc(Int, num, 1);\n"
            "    std::inout::println(\"{result}\");\n"
            "}\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        built = run_paxsy("build", project, cwd=self.tmp)
        self.assertEqual(built.returncode, 0, built.stderr)

    def test_global_alias_and_local_generic_parameter_are_distinct_scopes(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "static Number = Int;\n"
            "func calc(static T, a, b): T { return a + b; }\n"
            "func main(): Void { let result: Number = calc(Int, 10, 20); }\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_static_parameter_cannot_redeclare_global_name(self):
        project = self.init_demo()
        (project / "src/main.px").write_text(
            "static T = Int;\n"
            "func calc(static T, a, b): T { return a + b; }\n"
            "func main(): Void { let value: Int = calc(Int, 1, 2); }\n")
        checked = run_paxsy("check", project, cwd=self.tmp)
        self.assertNotEqual(checked.returncode, 0)
        self.assertIn("static parameter 'T' conflicts", checked.stderr)


if __name__ == "__main__":
    unittest.main()
