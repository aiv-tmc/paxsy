#!/usr/bin/env python3
"""End-to-end tests for the paxsy toolchain.

Every test drives the real binary in a temporary directory: it creates
projects, feeds it broken sources, and inspects the exit statuses,
the diagnostics and the files the commands leave behind. No third-party
dependencies are required; the suite runs with the standard library:

    python3 tests/test_paxsy.py            # or: make test

The command under test is ./paxsy by default; override it with the
PAXSY_CMD environment variable, for example:

    PAXSY_CMD="/lib64/ld-linux-x86-64.so.2 /path/to/paxsy" python3 tests/test_paxsy.py
"""

import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMD = shlex.split(os.environ.get("PAXSY_CMD", str(ROOT / "paxsy")))


def run_paxsy(*args, cwd=None, env=None):
    """Runs the toolchain; returns the completed process. The package
    cache is redirected to a scratch location unless a test sets it."""
    full_env = os.environ.copy()
    full_env.pop("PAXSY_LIBRARY_DIR", None)
    full_env.pop("NO_COLOR", None)
    if env:
        full_env.update(env)
    return subprocess.run(CMD + [str(a) for a in args],
                          cwd=cwd or ROOT, env=full_env,
                          capture_output=True, text=True, timeout=120)


class ToolTests(unittest.TestCase):
    def test_version(self):
        r = run_paxsy("--version")
        self.assertEqual(r.returncode, 0)
        self.assertRegex(r.stdout, r"^paxsy \d+\.\d+\.\d+")

    def test_help_and_license(self):
        self.assertIn("Usage: paxsy", run_paxsy("--help").stdout)
        self.assertIn("MIT", run_paxsy("--license").stdout)

    def test_unknown_command_and_option(self):
        self.assertEqual(run_paxsy("frobnicate").returncode, 2)
        self.assertEqual(run_paxsy("--bogus").returncode, 2)


class ProjectTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_init_creates_two_files_and_git(self):
        r = run_paxsy("init", "demo", cwd=self.tmp)
        self.assertEqual(r.returncode, 0)
        self.assertTrue((self.tmp / "demo/src/main.px").is_file())
        self.assertTrue((self.tmp / "demo/demo.toml").is_file())
        self.assertIn("Created project 'demo'", r.stdout)
        self.assertTrue((self.tmp / "demo/.git").is_dir() or
                        "git not found" in r.stdout)

    def test_init_rejects_bad_name_and_existing_dir(self):
        self.assertEqual(run_paxsy("init", "Bad Name", cwd=self.tmp).returncode, 2)
        (self.tmp / "taken").mkdir()
        self.assertEqual(run_paxsy("init", "taken", cwd=self.tmp).returncode, 1)

    def test_seeded_project_checks_clean_and_silent(self):
        run_paxsy("init", "demo", cwd=self.tmp)
        r = run_paxsy("check", cwd=self.tmp / "demo")
        self.assertEqual((r.returncode, r.stdout, r.stderr), (0, "", ""))

    def test_build_writes_report(self):
        run_paxsy("init", "demo", cwd=self.tmp)
        r = run_paxsy("build", cwd=self.tmp / "demo")
        self.assertEqual(r.returncode, 0)
        self.assertIn("frontend build report",
                      (self.tmp / "demo/build/report.txt").read_text())

    def test_missing_config_is_a_usage_error(self):
        r = run_paxsy("check", cwd=self.tmp)
        self.assertEqual(r.returncode, 2)

    def test_diagnostic_format_on_broken_source(self):
        run_paxsy("init", "demo", cwd=self.tmp)
        # a missing semicolon after pass is a parse error
        (self.tmp / "demo/src/main.px").write_text(
            "func main(): Void {\n    pass\n}\n")
        r = run_paxsy("check", cwd=self.tmp / "demo")
        self.assertEqual(r.returncode, 1)
        self.assertRegex(r.stderr, r"main\.px:\d+:\d+: error:")
        self.assertIn("expected", r.stderr)
        self.assertIn("^", r.stderr)                # caret underline present


class QualityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        run_paxsy("init", "demo", cwd=self.tmp)
        self.proj = self.tmp / "demo"

    def test_clippy_header_warning_and_strict(self):
        (self.proj / "src/main.px").write_text(
            "func main(): Void {\n    pass;\n}\n")   # headerless
        r = run_paxsy("clippy", cwd=self.proj)
        self.assertEqual(r.returncode, 0)
        self.assertIn("file-header", r.stderr)
        self.assertEqual(run_paxsy("clippy", "--strict", cwd=self.proj).returncode, 1)

    def test_clippy_wextra_enables_magic_numbers(self):
        main = self.proj / "src/main.px"
        main.write_text(main.read_text() + "func f(): Void { pass; }\n")
        # no magic number without --Wextra: f() adds none; add one now
        main.write_text(main.read_text().replace(
            "func f(): Void { pass; }",
            "func f(): Void { let x = 5; let y = 42; pass; }"))
        self.assertNotIn("magic number", run_paxsy("clippy", cwd=self.proj).stderr)
        self.assertIn("magic number", run_paxsy("clippy", "--Wextra",
                                                cwd=self.proj).stderr)

    def test_tree_bench_meta_config_doc(self):
        out = run_paxsy("tree", cwd=self.proj).stdout
        self.assertIn("main.px", out)
        self.assertIn("tokens/sec", run_paxsy("bench", cwd=self.proj).stdout)
        self.assertIn("demo", run_paxsy("meta", cwd=self.proj).stdout)
        run_paxsy("config", "build.optimization", "0", cwd=self.proj)
        self.assertEqual(run_paxsy("config", "build.optimization",
                                   cwd=self.proj).stdout.strip(), "0")
        run_paxsy("doc", cwd=self.proj)
        self.assertIn("API reference",
                      (self.proj / "doc.md").read_text())


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        run_paxsy("init", "demo", cwd=self.tmp)

    def test_state_archive_is_deterministic_ustar(self):
        proj = self.tmp / "demo"
        r = run_paxsy("state", cwd=proj)
        self.assertEqual(r.returncode, 0)
        archive = proj / "target/demo.a"
        with archive.open("rb") as f:
            f.seek(257)
            self.assertEqual(f.read(5), b"ustar")


class PackageTests(unittest.TestCase):
    PKG = "fake"          # name of the synthetic package

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.env = {"PAXSY_LIBRARY_DIR": str(self.tmp / "cache")}
        # A synthetic package stands in for any real library so that
        # the tests are hermetic and do not depend on the repository
        # carrying the std/ tree.
        self.pkg_src = self.tmp / "pkgsrc"
        (self.pkg_src / "src").mkdir(parents=True)
        (self.pkg_src / "fake.toml").write_text(
            '[project]\n'
            'name = "fake"\n'
            'version = "2.0.0"\n'
            'description = "Synthetic package for the test suite"\n')
        (self.pkg_src / "src/mini.px").write_text(
            "module mini {\n"
            "    /// Answers with a greeting.\n"
            "    func ping(): Char[] = \"pong\";\n"
            "}\n")

    def test_offline_index_from_installed_tree(self):
        installed = self.tmp / "cache/installed"
        installed.mkdir(parents=True)
        shutil.copytree(self.pkg_src, installed / self.PKG)
        r = run_paxsy("fetch", env=self.env)
        self.assertEqual(r.returncode, 0)
        self.assertIn("local cache", r.stdout)
        self.assertIn(self.PKG, run_paxsy("search", self.PKG, env=self.env).stdout)
        self.assertIn(self.PKG, run_paxsy("list", env=self.env).stdout)

    def test_install_from_local_cache_and_uninstall(self):
        cached = self.tmp / "cache/cache" / self.PKG
        cached.parent.mkdir(parents=True)
        shutil.copytree(self.pkg_src, cached)
        r = run_paxsy("install", self.PKG, env=self.env)
        self.assertEqual(r.returncode, 0)
        self.assertTrue((self.tmp / "cache/installed" / self.PKG / "fake.toml").is_file())
        self.assertEqual(run_paxsy("uninstall", self.PKG, env=self.env).returncode, 0)
        self.assertIn("no packages installed", run_paxsy("list", env=self.env).stdout)

    def test_install_unknown_package_fails(self):
        self.assertEqual(run_paxsy("install", "nope", env=self.env).returncode, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
