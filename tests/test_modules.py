"""Project-wide module resolution and import diagnostics."""

from paxsy_testlib import ProjectCase, run_paxsy


class ModuleTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def write(self, name, source):
        (self.proj / "src" / name).write_text(source)

    def test_include_table_adds_external_source_directory(self):
        vendor = self.proj / "vendor" / "geometry" / "src"
        vendor.mkdir(parents=True)
        (vendor / "geometry.px").write_text(
            "module geometry { func area(w: Int, h: Int): Int { return w * h; } }\n")
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() +
                       "\n[include.geometry]\npath = \"vendor/geometry/src\"\n")
        self.write("main.px", "use std::inout;\n"
                   'func main(): Void { std::inout::println("{geometry::area(6, 7)}"); }\n')
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "42\n")

    def test_missing_required_include_fails_source_discovery(self):
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() +
                       "\n[include.missing]\npath = \"vendor/missing\"\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source discovery failed", result.stderr)

    def test_missing_optional_include_is_ignored(self):
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() +
                       "\n[include.optional_dep]\npath = \"vendor/optional\"\noptional = true\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_new_module_entrypoint_can_declare_shared_modules(self):
        (self.proj / "src" / "mod.px").write_text(
            "module arithmetic { func twice(x: Int): Int { return x * 2; } }\n")
        self.write("main.px", "use std::inout;\n"
                   'func main(): Void { std::inout::println("{arithmetic::twice(21)}"); }\n')
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "42\n")

    def test_include_table_requires_path(self):
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + "\n[include.incomplete]\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source discovery failed", result.stderr)

    def test_module_declared_in_another_source_file(self):
        self.write("math.px",
                   "module math { func add(a: Int, b: Int): Int { return a + b; } }\n")
        self.write("main.px",
                   "use std::inout;\n"
                   "func main(): Void { std::inout::println(\"{math::add(10, 20)}\"); }\n")
        checked = run_paxsy("check", cwd=self.proj)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "30\n")

    def test_ordinary_top_level_declarations_are_file_local(self):
        self.write("helpers.px",
                   "func private_helper(): Int { return 7; }\n")
        self.write("main.px",
                   "func main(): Void { private_helper(); }\n")
        checked = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(checked.returncode, 0)
        self.assertIn("unknown variable or function 'private_helper'", checked.stderr)

    def test_module_members_remain_visible_across_source_files(self):
        self.write("math.px",
                   "module math { func add(a: Int, b: Int): Int { return a + b; } }\n")
        self.write("main.px",
                   "use std::inout;\n"
                   'func main(): Void { std::inout::println("{math::add(2, 3)}"); }\n')
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "5\n")

    def test_module_can_be_split_across_source_files(self):
        self.write("math_add.px",
                   "module math { func add(a: Int, b: Int): Int { return a + b; } }\n")
        self.write("math_sub.px",
                   "module math { func sub(a: Int, b: Int): Int { return a - b; } }\n")
        self.write("main.px",
                   "use std::inout;\n"
                   "func main(): Void { std::inout::println(\"{math::add(9, 4)} {math::sub(9, 4)}\"); }\n")
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.stdout, "13 5\n")

    def test_generic_function_in_another_module_is_specialized(self):
        self.write("math.px",
                   "module math { func calc(static T, value): T { "
                   "let first = value + value; let second = first * value; "
                   "return second - value; } }\n")
        self.write("main.px", "use std::inout;\n"
                   "use math;\n"
                   "func main(): Void { std::inout::println(\"{calc(Int, 100)}\"); }\n")
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "19900\n")

    def test_struct_type_and_initializer_from_another_module(self):
        self.write("types.px",
                   "module types { static Person = Struct { name: Char[], age: UInt8, }; }\n")
        self.write("main.px",
                   "use std::inout;\n"
                   "func main(): Void {\n"
                   " let Tom: types::Person {.name=\"Tom\", .age=20};\n"
                   " std::inout::println(\"{Tom.name} {Tom.age}\");\n"
                   "}\n")
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.stdout, "Tom 20\n")

    def test_nested_module_can_be_imported_as_a_namespace(self):
        self.write("nested.px",
                   "module outer { module inner { "
                   "func add(a: Int, b: Int): Int { return a + b; } "
                   "} }\n")
        self.write("main.px",
                   "use std::inout;\n"
                   "use outer::inner;\n"
                   'func main(): Void { std::inout::println(\"{add(8, 5)}\"); }\n')
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.stdout, "13\n")

    def test_nested_module_member_can_be_imported_directly(self):
        self.write("nested.px",
                   "module outer { module inner { "
                   "func sub(a: Int, b: Int): Int { return a - b; } "
                   "} }\n")
        self.write("main.px",
                   "use std::inout;\n"
                   "use outer::inner::sub;\n"
                   'func main(): Void { std::inout::println(\"{sub(9, 4)}\"); }\n')
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.stdout, "5\n")

    def test_long_nested_module_path_is_not_truncated_during_import(self):
        names = ["namespace_alpha", "namespace_bravo", "namespace_charlie", "namespace_delta"]
        path = "::".join(names)
        nested = "func value(): Int { return 42; }"
        for name in reversed(names):
            nested = f"module {name} {{ {nested} }}"
        self.write("nested.px", nested + "\n")
        self.write("main.px",
                   "use std::inout;\n"
                   f"use {path};\n"
                   'func main(): Void { std::inout::println("{value()}"); }\n')
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = run_paxsy("run", cwd=self.proj)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "42\n")

    def test_unknown_member_is_reported_as_module_member_error(self):
        self.write("math.px", "module math { func add(a: Int, b: Int): Int { return a + b; } }\n")
        self.write("main.px", "func main(): Void { math::missing(); }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown function or value 'math::missing'", result.stderr)
        self.assertNotIn("unknown module 'math'", result.stderr)

    def test_repeated_external_import_is_safe_and_idempotent(self):
        self.write("main.px",
                   "use std::inout;\n"
                   "use std::inout;\n"
                   "func main(): Void { std::inout::println(\"ok\"); }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_local_import_conflict_has_a_use_site_diagnostic(self):
        self.write("math.px", "module math { func add(a: Int, b: Int): Int { return a + b; } }\n")
        self.write("main.px",
                   "func add(): Void { }\n"
                   "use math;\n"
                   "func main(): Void { }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("import of 'add' conflicts", result.stderr)
        self.assertIn("src/main.px:", result.stderr)


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
