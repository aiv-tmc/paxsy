#!/usr/bin/env python3
"""Tooling behaviour: the build/check/run contract of the CLI."""
import os
import subprocess
import shutil
import tempfile
import unittest
from pathlib import Path


class ProjectTests(unittest.TestCase):
    def setUp(self):
        self.cmd = [os.environ.get("PAXSY_CMD", "paxsy")]
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-test-"))

    def paxsy(self, *args, cwd=None):
        return subprocess.run(self.cmd + list(args),
                                cwd=cwd or self.tmp,
                                capture_output=True, text=True)

    def test_recovery_accepts_binary_paths_with_shell_quotes(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        built = self.paxsy("build", str(proj))
        self.assertEqual(built.returncode, 0, built.stderr)
        quoted = self.tmp / "binary ' with spaces"
        shutil.copy2(proj / "build/demo", quoted)
        recovered = self.paxsy("recovery", str(quoted))
        self.assertEqual(recovered.returncode, 0, recovered.stderr)
        recovered_src = self.tmp / "binary ' with spaces.recovered" / "src"
        self.assertTrue(recovered_src.is_dir())
        recovered_files = list(recovered_src.glob("*.px"))
        self.assertTrue(recovered_files)
        self.assertIn("asm \"", recovered_files[0].read_text())

    def test_windows_target_emits_only_the_pe_executable(self):
        proj = self.tmp / "windows_demo"
        initialized = self.paxsy("new", "windows_demo")
        self.assertEqual(initialized.returncode, 0, initialized.stderr)
        config = proj / "windows_demo.toml"
        config.write_text(config.read_text() +
                          '\n[build]\ntarget = "windows-x86_64"\n'
                          'output = "build/windows_demo.exe"\n')
        built = self.paxsy("build", str(proj))
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertTrue((proj / "build/windows_demo.exe").is_file())
        self.assertTrue((proj / "build/bin/windows_demo.o").is_file())
        self.assertFalse((proj / "build/windows_demo").exists())

    def test_recovery_rejects_non_elf_input_cleanly(self):
        bad = self.tmp / "not-an-elf"
        bad.write_bytes(b"this is not an executable")
        recovered = self.paxsy("recovery", str(bad))
        self.assertNotEqual(recovered.returncode, 0)
        self.assertFalse(Path(str(bad) + ".px").exists())

    def test_unimplemented_target_fails_with_specific_diagnostic(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        config = proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[build]\ntarget = "linux-aarch64"\n')
        r = self.paxsy("build", str(proj))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not implemented", r.stderr)
        self.assertFalse((proj / "build/demo").exists())

    def test_configured_target_is_part_of_incremental_build_identity(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        native = self.paxsy("build", str(proj))
        self.assertEqual(native.returncode, 0, native.stderr)
        config = proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[build]\ntarget = "windows-x86_64"\n'
                          'output = "build/demo.exe"\n')
        pe = self.paxsy("build", str(proj))
        self.assertEqual(pe.returncode, 0, pe.stderr)
        self.assertTrue((proj / "build/demo.exe").is_file())

    def test_build_artifacts_layout(self):
        """One build fills the whole tree: the binary at build/ and
        the incremental stamp under build/bin/; nothing else."""
        proj = self.tmp / "demo"
        r = self.paxsy("new", "demo")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.paxsy("build", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((proj / "build/demo").is_file())
        self.assertFalse((proj / "build/report.txt").exists())
        self.assertFalse((proj / "build/bin/demo.c").exists())
        visible = sorted(p.name for p in (proj / "build").iterdir()
                         if not p.name.startswith("."))
        self.assertEqual(visible, ["bin", "demo"])
        objects = list((proj / "build/bin").iterdir())
        self.assertTrue(objects)
        self.assertTrue(all(p.suffix == ".o" for p in objects))
        self.assertFalse(any(p.suffix in {".exe", ".elf"} for p in
                             (proj / "build/bin").iterdir()))

    def test_diagnostics_use_location_first_layout_and_body_context(self):
        proj = self.tmp / "diagnostics"
        self.paxsy("new", "diagnostics")
        (proj / "src/main.px").write_text(
            "func calc(): Int! { return 1; }\n"
            "func main(): Void! {\n"
            "    let value = try calc();\n"
            "}\n")
        result = self.paxsy("clippy", str(proj))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("src/main.px: warning: [try-catch]", result.stderr)
        self.assertIn("       2 | func main()", result.stderr)
        self.assertIn("       4 | }", result.stderr)
        self.assertNotIn("paxsy: ", result.stderr)
        self.assertNotIn("\x1b[", result.stderr)

    def test_banner_keeps_the_logo_shadow(self):
        result = self.paxsy()
        self.assertIn("paxsy", result.stdout)
        self.assertIn("░░", result.stdout)

    def test_diagnostic_format_on_broken_source(self):
        proj = self.tmp / "broken"
        self.paxsy("new", "broken")
        (proj / "src/main.px").write_text("func main( {\n")
        r = self.paxsy("check", str(proj))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("error", r.stderr)

    def test_new_creates_project_layout_and_git(self):
        r = self.paxsy("new", "demo")
        self.assertEqual(r.returncode, 0, r.stderr)
        proj = self.tmp / "demo"
        self.assertTrue((proj / "demo.toml").is_file())
        self.assertTrue((proj / "src/main.px").is_file())
        self.assertTrue((proj / "src/mod.px").is_file())
        self.assertFalse((proj / "tests").exists())
        self.assertFalse((proj / "benches").exists())
        self.assertTrue((proj / ".git").is_dir())

    def test_new_rejects_bad_name_and_existing_dir(self):
        r = self.paxsy("new", "bad name")
        self.assertNotEqual(r.returncode, 0)
        self.paxsy("new", "taken")
        r = self.paxsy("new", "taken")
        self.assertNotEqual(r.returncode, 0)

    def test_new_writes_gitignore(self):
        self.paxsy("new", "demo")
        gi = (self.tmp / "demo/.gitignore").read_text()
        self.assertIn("build/", gi)

    def test_missing_config_is_a_usage_error(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        r = self.paxsy("build", str(empty))
        self.assertEqual(r.returncode, 2)

    def test_seeded_project_checks_clean_and_silent(self):
        self.paxsy("new", "demo")
        r = self.paxsy("check", str(self.tmp / "demo"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "")
        self.assertEqual(r.stderr, "")


class ToolTests(unittest.TestCase):
    def setUp(self):
        self.cmd = [os.environ.get("PAXSY_CMD", "paxsy")]
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-test-"))

    def paxsy(self, *args):
        return subprocess.run(self.cmd + list(args), cwd=self.tmp,
                              capture_output=True, text=True)

    def test_help_and_license(self):
        r = self.paxsy("--help")
        self.assertEqual(r.returncode, 0)
        self.assertIn("paxsy", r.stdout)
        license_info = self.paxsy("--license")
        self.assertEqual(license_info.returncode, 0)
        self.assertIn("Apache License, Version 2.0", license_info.stdout)
        self.assertIn("apache.org/licenses/LICENSE-2.0", license_info.stdout)

    def test_unknown_command_and_option(self):
        r = self.paxsy("frobnicate")
        self.assertNotEqual(r.returncode, 0)
        r = self.paxsy("--definitely-unknown-option")
        self.assertNotEqual(r.returncode, 0)

    def test_version(self):
        r = self.paxsy("--version")
        self.assertEqual(r.returncode, 0)
        self.assertIn("paxsy", r.stdout)

    def test_custom_test_and_bench_directories(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        config = proj / "demo.toml"
        text = config.read_text()
        text += '\n[test]\npath = "quality/tests"\n'
        text += '\n[bench]\npath = "performance"\n'
        config.write_text(text)
        tests = proj / "quality/tests"
        benches = proj / "performance"
        tests.mkdir(parents=True)
        benches.mkdir(parents=True)
        (tests / "basic_test.px").write_text(
            "func test_basic(): Void { pass; }\n")
        (benches / "lexer.px").write_text(
            "func sample(): Void { pass; }\n")
        r = self.paxsy("test", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("test_basic", r.stdout)
        self.assertIn("running test discovery", r.stdout)
        self.assertNotIn("\x1b[", r.stdout)
        r = self.paxsy("bench", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("tokens/sec", r.stdout)
        self.assertIn("bench result:", r.stdout)
        self.assertNotIn("\x1b[", r.stdout)

    def test_git_autocommit_is_enabled_by_default(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        subprocess.run(["git", "config", "user.name", "Paxsy Test"],
                       cwd=proj, check=True)
        subprocess.run(["git", "config", "user.email", "paxsy-test@example.invalid"],
                       cwd=proj, check=True)
        r = self.paxsy("build", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        head = subprocess.run(["git", "rev-parse", "--verify", "HEAD"],
                              cwd=proj, capture_output=True, text=True)
        self.assertEqual(head.returncode, 0, head.stderr)
        subject = subprocess.run(["git", "log", "-1", "--format=%s"],
                                 cwd=proj, capture_output=True, text=True,
                                 check=True).stdout.strip()
        self.assertEqual(subject, "build: demo (4 files)")

    def test_git_autocommit_can_be_disabled(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        subprocess.run(["git", "config", "user.name", "Paxsy Test"],
                       cwd=proj, check=True)
        subprocess.run(["git", "config", "user.email", "paxsy-test@example.invalid"],
                       cwd=proj, check=True)
        config = proj / "demo.toml"
        config.write_text(config.read_text() + '\n[git]\nauto_commit = false\n')
        r = self.paxsy("build", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        head = subprocess.run(["git", "rev-parse", "--verify", "HEAD"],
                              cwd=proj, capture_output=True, text=True)
        self.assertNotEqual(head.returncode, 0)

    def test_git_autocommit_does_not_commit_unrelated_untracked_files(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        subprocess.run(["git", "config", "user.name", "Paxsy Test"],
                       cwd=proj, check=True)
        subprocess.run(["git", "config", "user.email", "paxsy-test@example.invalid"],
                       cwd=proj, check=True)
        config = proj / "demo.toml"
        config.write_text(config.read_text() + '\n[git]\nauto_commit = true\n')
        (proj / "private-notes.txt").write_text("not a build input\n")
        r = self.paxsy("build", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        status = subprocess.run(["git", "status", "--porcelain"],
                                cwd=proj, capture_output=True, text=True,
                                check=True).stdout
        self.assertIn("private-notes.txt", status)
        committed = subprocess.run(["git", "show", "--pretty=", "--name-only", "HEAD"],
                                   cwd=proj, capture_output=True, text=True,
                                   check=True).stdout
        self.assertIn("src/main.px", committed)
        self.assertNotIn("private-notes.txt", committed)

    def test_git_autocommit_skips_when_user_has_staged_changes(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        subprocess.run(["git", "config", "user.name", "Paxsy Test"],
                       cwd=proj, check=True)
        subprocess.run(["git", "config", "user.email", "paxsy-test@example.invalid"],
                       cwd=proj, check=True)
        config = proj / "demo.toml"
        config.write_text(config.read_text() + '\n[git]\nauto_commit = true\n')
        staged = proj / "staged.txt"
        staged.write_text("user staged this\n")
        subprocess.run(["git", "add", "staged.txt"], cwd=proj, check=True)
        r = self.paxsy("build", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        head = subprocess.run(["git", "rev-parse", "--verify", "HEAD"],
                              cwd=proj, capture_output=True, text=True)
        self.assertNotEqual(head.returncode, 0)
        self.assertIn("staged.txt", subprocess.run(
            ["git", "status", "--porcelain"], cwd=proj,
            capture_output=True, text=True, check=True).stdout)

    def test_git_autocommit_uses_project_policy(self):
        proj = self.tmp / "demo"
        self.paxsy("new", "demo")
        subprocess.run(["git", "config", "user.name", "Paxsy Test"],
                       cwd=proj, check=True)
        subprocess.run(["git", "config", "user.email", "paxsy-test@example.invalid"],
                       cwd=proj, check=True)
        config = proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[git]\nauto_commit = true\n'
                          'commit_message = "release: verified build"\n')
        r = self.paxsy("build", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        subject = subprocess.run(["git", "log", "-1", "--format=%s"],
                                 cwd=proj, capture_output=True, text=True,
                                 check=True).stdout.strip()
        self.assertEqual(subject, "release: verified build")


if __name__ == "__main__":
    unittest.main()
