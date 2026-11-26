"""Standard-library contract tests: libraries are reachable only
through the std package namespace (use std::<lib>;), use directives
naming unknown modules or unknown std members are refused in sema,
and the Paxsy-written std::string helpers work end to end."""

from pathlib import Path

from paxsy_testlib import ProjectCase, run_paxsy


class StdContractTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def build_main(self, source):
        (self.proj / "src/main.px").write_text(source)
        return run_paxsy("build", cwd=self.proj)

    def test_std_module_registry_is_source_discovered(self):
        manifest = Path(__file__).resolve().parents[1] / "lib/std/std.toml"
        content = manifest.read_text()
        self.assertIn('[package]', content)
        self.assertNotIn('\n[modules]\n', content)

    def test_use_std_inout_connects_the_library(self):
        """The seeded demo uses std::inout and builds: the use
        directive resolves the module declaration discovered in std sources and println runs."""
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_std_string_strlen(self):
        r = self.build_main(
            "use std::string;\n"
            "\n"
            "func main(): Void! {\n"
            "    if std::string::strlen(\"hello\") == 5 &&\n"
            "       std::string::strlen(\"\") == 0 {\n"
            "        try std::inout::println(\"ok\");\n"
            "    } else {\n"
            "        try std::inout::println(\"bad\");\n"
            "    }\n"
            "}\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.stdout, "ok\n")

    def test_std_string_streq(self):
        r = self.build_main(
            "use std::string;\n"
            "\n"
            "func main(): Void! {\n"
            "    if std::string::streq(\"abc\", \"abc\") == 1 &&\n"
            "       std::string::streq(\"abc\", \"abd\") == 0 {\n"
            "        try std::inout::println(\"ok\");\n"
            "    } else {\n"
            "        try std::inout::println(\"bad\");\n"
            "    }\n"
            "}\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.stdout, "ok\n")

    def test_memory_copy_uses_word_and_tail_paths(self):
        r = self.build_main(
            "use std::mem;\n"
            "use std::inout;\n"
            "func main(): Void! {\n"
            "    let source: UInt64 = 123456789;\n"
            "    let target: UInt64 = 0;\n"
            "    std::mem::copy((&target) as &Void, (&source) as &Void, 8);\n"
            "    try std::inout::println(\"{target}\");\n"
            "}\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "123456789\n")

    def test_string_search_helpers_cover_empty_and_mismatch_cases(self):
        r = self.build_main(
            "use std::string;\n"
            "use std::inout;\n"
            "func main(): Void! {\n"
            "    let a = std::string::find_substring(\"abracadabra\", \"cada\");\n"
            "    let b = std::string::find_substring(\"abc\", \"\");\n"
            "    let c = std::string::find_substring(\"abc\", \"xyz\");\n"
            "    try std::inout::println(\"{a}|{b}|{c}\");\n"
            "}\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "4|0|3\n")

    def test_sizeof_and_alignof_are_compile_time_builtins(self):
        r = self.build_main(
            "use std::inout;\n"
            "func main(): Void! {\n"
            "    let size = sizeof(Int32);\n"
            "    let alignment = alignof(Int32);\n"
            "    try std::inout::println(\"{size}|{alignment}\");\n"
            "}\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "4|4\n")

    def test_address_of_local_matches_pointer_parameter(self):
        r = self.build_main(
            "use std::inout;\n"
            "func increment(value: &Int32): Void {\n"
            "    @value = @value + 1;\n"
            "}\n"
            "func main(): Void! {\n"
            "    let value: Int32 = 4;\n"
            "    increment(&value);\n"
            "    try std::inout::println(\"{value}\");\n"
            "}\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "5\n")

    def test_unknown_module_is_a_build_error(self):
        """A use directive naming a module that exists nowhere - not
        in the program, not in std, not among installed packages -
        stops the build in sema."""
        r = self.build_main(
            "use nowhere;\n"
            "\n"
            "func main(): Void! {\n"
            "}\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown module", r.stderr)

    def test_unknown_std_member_is_a_build_error(self):
        """The std source tree bounds the namespace: a
        mistyped module name is refused in sema, not at link time."""
        r = self.build_main(
            "use std::typo;\n"
            "\n"
            "func main(): Void! {\n"
            "}\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has no member", r.stderr)

    def test_library_without_std_is_a_build_error(self):
        """There is no top-level inout module: connecting a library
        without going through std is refused."""
        r = self.build_main(
            "use inout;\n"
            "\n"
            "func main(): Void! {\n"
            "}\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown module", r.stderr)

    def test_unknown_qualified_std_function_is_a_semantic_error(self):
        r = self.build_main(
            "func main(): Void { std::inout::missing_function(); }\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown function or value", r.stderr)
        self.assertNotIn("unknown global", r.stderr)

    def test_missing_bare_symbol_after_local_import_is_a_name_error(self):
        r = self.build_main(
            "module math { func add(a: Int, b: Int): Int { return a + b; } }\n"
            "use math;\n"
            "func main(): Void { missing_function(); }\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown variable or function 'missing_function'", r.stderr)
        self.assertNotIn("unknown module", r.stderr)

    def test_missing_member_in_known_local_module_is_a_member_error(self):
        r = self.build_main(
            "module math { func add(a: Int, b: Int): Int { return a + b; } }\n"
            "func main(): Void { math::missing_function(); }\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown function or value 'math::missing_function'", r.stderr)

    def test_unknown_bare_identifier_is_not_reported_as_a_module_error(self):
        r = self.build_main(
            "func main(): Void { missing_function(); }\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unknown variable or function 'missing_function'", r.stderr)
        self.assertNotIn("unknown module", r.stderr)



if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
