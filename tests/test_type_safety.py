import unittest

from paxsy_testlib import ProjectCase, run_paxsy


class TypeSafetyTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def write_main(self, source):
        (self.proj / "src/main.px").write_text(source)

    def test_integer_is_not_silently_a_char(self):
        self.write_main("func main(): Void {\n let c: Char = 5;\n}\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("type of the initializer", result.stderr)
        self.assertIn("help:", result.stderr)
        self.assertIn("src/main.px: error:", result.stderr)
        self.assertNotIn("<tyinfer>", result.stderr)
        self.assertIn("       1 | func main()", result.stderr)
        self.assertIn("       3 | }", result.stderr)

    def test_char_literal_matches_char(self):
        self.write_main("func main(): Void { let c: Char = 'A'; }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_integer_to_char_cast(self):
        self.write_main("func main(): Void { let c: Char = 65 as Char; }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_function_argument_type_is_checked(self):
        self.write_main(
            "func takes(c: Char): Void { }\n"
            "func main(): Void { takes(5); }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("function argument type mismatch", result.stderr)
        self.assertIn("expected Char", result.stderr)

    def test_return_type_is_checked(self):
        self.write_main(
            "func get(): Char { return 5; }\n"
            "func main(): Void { }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("return type mismatch", result.stderr)

    def test_assignment_type_is_checked(self):
        self.write_main(
            "func main(): Void { let c: Char = 'A'; c = 5; }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("assignment type mismatch", result.stderr)

    def test_function_arity_is_checked(self):
        self.write_main(
            "func takes(a: Int, b: Int): Void { }\n"
            "func main(): Void { takes(1); }\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expects 2 argument(s)", result.stderr)

    def test_free_accepts_typed_alloc_pointer_without_cast(self):
        self.write_main(
            "func main(): Void {\n"
            "    let p: &Int = alloc Int[2];\n"
            "    defer free p;\n"
            "}\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = run_paxsy("build", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = run_paxsy("run", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_free_statement_requires_semicolon(self):
        self.write_main(
            "func main(): Void {\n"
            "    let p: &Int = alloc Int[2];\n"
            "    free p\n"
            "}\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected ';' after free", result.stderr)

    def test_typed_struct_initializer_without_equals(self):
        self.write_main(
            "use std::inout;\n"
            "static Person = Struct { name: Char[], age: UInt8, };\n"
            "func main(): Void {\n"
            " let Tom: Person {.name=\"Tom\", .age=20};\n"
            " std::inout::println(\"{Tom.name} {Tom.age}\");\n"
            "}\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = run_paxsy("build", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = run_paxsy("run", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "Tom 20\n")

    def test_configured_output_path_is_used_by_build_and_run(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[build]\noutput = "build/bin/demo-app"\n')
        self.write_main('use std::inout;\nfunc main(): Void { std::inout::println("ok"); }\n')
        result = run_paxsy("build", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.proj / "build/bin/demo-app").is_file())
        self.assertFalse((self.proj / "build/demo").exists())
        result = run_paxsy("run", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "ok\n")


if __name__ == "__main__":
    unittest.main()
