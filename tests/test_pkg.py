"""Package-manager end-to-end tests with a synthetic local package."""

import shutil
import tempfile
import unittest
from pathlib import Path

from paxsy_testlib import run_paxsy


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
        # A complete cache entry carries the completion marker with
        # its version; a directory without it counts as partial.
        (cached / ".paxsy-pkg").write_text("1.0.0\n")
        r = run_paxsy("install", self.PKG, env=self.env)
        self.assertEqual(r.returncode, 0)
        self.assertTrue((self.tmp / "cache/installed" / self.PKG / "fake.toml").is_file())
        self.assertEqual(run_paxsy("uninstall", self.PKG, env=self.env).returncode, 0)
        self.assertIn("no packages installed", run_paxsy("list", env=self.env).stdout)

    def test_install_unknown_package_fails(self):
        self.assertEqual(run_paxsy("install", "nope", env=self.env).returncode, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
