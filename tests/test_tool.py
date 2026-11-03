#!/usr/bin/env python3
"""Tooling behaviour: the build/check/run contract of the CLI."""
import os
import subprocess
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

    def test_build_artifacts_layout(self):
        """One build fills the whole tree: the binary at build/ and
        the incremental stamp under build/bin/; nothing else."""
        proj = self.tmp / "demo"
        r = self.paxsy("init", "demo")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.paxsy("build", str(proj))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((proj / "build/demo").is_file())
        self.assertTrue((proj / "build/bin/demo.opt").is_file())
        self.assertFalse((proj / "build/report.txt").exists())
        self.assertFalse((proj / "build/bin/demo.c").exists())
        self.assertEqual(sorted(p.name for p in (proj / "build").iterdir()),
                         ["bin", "demo"])

    def test_diagnostic_format_on_broken_source(self):
        proj = self.tmp / "broken"
        self.paxsy("init", "broken")
        (proj / "src/main.px").write_text("func main( {\n")
        r = self.paxsy("check", str(proj))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("error", r.stderr)

    def test_init_creates_two_files_and_git(self):
        r = self.paxsy("init", "demo")
        self.assertEqual(r.returncode, 0, r.stderr)
        proj = self.tmp / "demo"
        self.assertTrue((proj / "demo.toml").is_file())
        self.assertTrue((proj / "src/main.px").is_file())
        self.assertTrue((proj / ".git").is_dir())

    def test_init_rejects_bad_name_and_existing_dir(self):
        r = self.paxsy("init", "bad name")
        self.assertNotEqual(r.returncode, 0)
        self.paxsy("init", "taken")
        r = self.paxsy("init", "taken")
        self.assertNotEqual(r.returncode, 0)

    def test_init_writes_gitignore(self):
        self.paxsy("init", "demo")
        gi = (self.tmp / "demo/.gitignore").read_text()
        self.assertIn("build/", gi)

    def test_missing_config_is_a_usage_error(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        r = self.paxsy("build", str(empty))
        self.assertEqual(r.returncode, 2)

    def test_seeded_project_checks_clean_and_silent(self):
        self.paxsy("init", "demo")
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

    def test_unknown_command_and_option(self):
        r = self.paxsy("frobnicate")
        self.assertNotEqual(r.returncode, 0)
        r = self.paxsy("--definitely-unknown-option")
        self.assertNotEqual(r.returncode, 0)

    def test_version(self):
        r = self.paxsy("--version")
        self.assertEqual(r.returncode, 0)
        self.assertIn("paxsy", r.stdout)


if __name__ == "__main__":
    unittest.main()
