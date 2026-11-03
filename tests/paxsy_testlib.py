"""Shared helpers for the paxsy test suite.

Every test drives the real binary in a temporary directory: it creates
projects, feeds it broken sources, and inspects the exit statuses,
the diagnostics and the files the commands leave behind. No third-party
dependencies are required; the suite runs with the standard library:

    make test

The command under test is ./paxsy by default; override it with the
PAXSY_CMD environment variable, for example:

    PAXSY_CMD=/path/to/paxsy python3 tests/test_codegen.py
"""

import os
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


class ProjectCase(unittest.TestCase):
    """A scratch directory with an initialised 'demo' project inside."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="paxsy-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def init_demo(self):
        """Creates the seeded project and returns its path."""
        r = run_paxsy("init", "demo", cwd=self.tmp)
        self.assertEqual(r.returncode, 0, r.stderr)
        return self.tmp / "demo"
