"""Lint, archive and frontend-recovery end-to-end tests."""

from paxsy_testlib import ProjectCase, run_paxsy


class QualityTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def test_clippy_header_warning_and_strict(self):
        (self.proj / "src/main.px").write_text(
            "func main(): Void {\n    pass;\n}\n")   # headerless
        r = run_paxsy("clippy", cwd=self.proj)
        self.assertEqual(r.returncode, 0)
        self.assertIn("file-header", r.stderr)
        self.assertEqual(run_paxsy("clippy", "--strict",
                                   cwd=self.proj).returncode, 1)

    def test_clippy_wextra_enables_magic_numbers(self):
        main = self.proj / "src/main.px"
        main.write_text(main.read_text() + "func f(): Void { pass; }\n")
        main.write_text(main.read_text().replace(
            "func f(): Void { pass; }",
            "func f(): Void { let x = 5; let y = 42; pass; }"))
        self.assertNotIn("magic number",
                         run_paxsy("clippy", cwd=self.proj).stderr)
        self.assertIn("magic number",
                      run_paxsy("clippy", "--Wextra",
                                cwd=self.proj).stderr)

    def test_tree_bench_meta_config_doc(self):
        out = run_paxsy("tree", cwd=self.proj).stdout
        self.assertIn("main.px", out)
        self.assertNotIn("build", out)          # generated trees hide
        self.assertNotIn(".gitignore", out)     # dotfiles hide
        self.assertIn("tokens/sec",
                      run_paxsy("bench", cwd=self.proj).stdout)
        self.assertIn("demo", run_paxsy("meta", cwd=self.proj).stdout)
        run_paxsy("config", "build.optimization", "0", cwd=self.proj)
        self.assertEqual(run_paxsy("config", "build.optimization",
                                   cwd=self.proj).stdout.strip(), "0")
        run_paxsy("doc", cwd=self.proj)
        self.assertIn("API reference",
                      (self.proj / "doc.md").read_text())


class ArtifactTests(ProjectCase):
    def test_state_archive_is_deterministic_ustar(self):
        proj = self.init_demo()
        r = run_paxsy("state", cwd=proj)
        self.assertEqual(r.returncode, 0)
        archive = proj / "build/demo.a"
        with archive.open("rb") as f:
            f.seek(257)
            self.assertEqual(f.read(5), b"ustar")


class FrontendRecoveryTests(ProjectCase):
    """Regression tests for the frontend hardening: statement-level
    error recovery, numeric literal validation and deterministic
    archive emission."""

    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def test_multiple_syntax_errors_in_one_file(self):
        """The parser recovers at statement boundaries, so a file with
        two broken declarations surfaces both errors in one run."""
        (self.proj / "src/main.px").write_text(
            "func main(): Void {\n    pass;\n}\n"
            "func broken( {\n    let x = ;\n}\n"
            "func also_broken( {\n    pass;\n}\n")
        r = run_paxsy("check", cwd=self.proj)
        self.assertEqual(r.returncode, 1)
        self.assertGreaterEqual(r.stderr.count("error:"), 2)
        self.assertLess(r.stderr.count("error:"), 6)

    def test_numeric_literal_validation(self):
        (self.proj / "src/main.px").write_text(
            "func main(): Void {\n    pass;\n}\n"
            "func bad(): Void {\n"
            "    let a = 0b12;\n"
            "    let b = 1__2;\n"
            "    let c = 0x1.2;\n"
            "}\n")
        r = run_paxsy("check", cwd=self.proj)
        self.assertEqual(r.returncode, 1)
        self.assertIn("invalid digit '2'", r.stderr)
        self.assertIn("consecutive '_' separators", r.stderr)
        self.assertIn("hexadecimal float requires a 'p' exponent",
                      r.stderr)

    def test_state_archive_is_deterministic(self):
        """Two consecutive state runs must produce byte-identical
        archives: source discovery sorts its file list, so the tar
        member order is stable."""
        self.assertEqual(run_paxsy("state", cwd=self.proj).returncode, 0)
        first = (self.proj / "build/demo.a").read_bytes()
        self.assertEqual(run_paxsy("state", cwd=self.proj).returncode, 0)
        second = (self.proj / "build/demo.a").read_bytes()
        self.assertEqual(first, second)
        self.assertEqual(first[257:262], b"ustar")


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
