"""Standard-library contract tests: libraries are reachable only
through the std package namespace (use std::<lib>;), use directives
naming unknown modules or unknown std members are refused in sema,
and the Paxsy-written std::string helpers work end to end."""

from paxsy_testlib import ProjectCase, run_paxsy


class StdContractTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def build_main(self, source):
        (self.proj / "src/main.px").write_text(source)
        return run_paxsy("build", cwd=self.proj)

    def test_use_std_inout_connects_the_library(self):
        """The seeded demo uses std::inout and builds: the use
        directive passes manifest validation and println runs."""
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
        """The std.toml manifest bounds the std namespace: a
        mistyped library name is refused in sema, not at link time."""
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


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
