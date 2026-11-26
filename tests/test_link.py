"""Tests for the self-hosted object/archive emission and the static
linker: --emit-obj produces a relocatable object, --emit-a an
archive, and `paxsy link` turns either into a runnable executable
without any external tool."""

from paxsy_testlib import ProjectCase, run_paxsy


class LinkTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def test_emit_obj_and_link(self):
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + '\n[build]\ntarget = "obj"\n')
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        obj = self.proj / "build/bin/demo.o"
        self.assertTrue(obj.exists())
        self.assertEqual(obj.read_bytes()[:4], b"\x7fELF")
        out = self.proj / "linked"
        r = run_paxsy("link", str(out), "--", str(obj), cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy(str(out), cwd=self.proj)
        self.assertEqual(r.stdout, "Hello, World\n")

    def test_emit_a_and_link_archive(self):
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + '\n[build]\ntarget = "a"\n')
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        arch = self.proj / "build/artifacts/demo.a"
        self.assertTrue(arch.exists())
        self.assertTrue(arch.read_bytes().startswith(b"!<arch>\n"))
        out = self.proj / "linked-a"
        r = run_paxsy("link", str(out), "--", str(arch), cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy(str(out), cwd=self.proj)
        self.assertEqual(r.stdout, "Hello, World\n")

    def test_link_rejects_truncated_elf_without_crashing(self):
        bad = self.proj / "truncated.o"
        bad.write_bytes(b"\x7fELF" + bytes(60))
        out = self.proj / "bad-linked"
        r = run_paxsy("link", str(out), "--", str(bad), cwd=self.proj)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not a 64-bit relocatable object", r.stderr)
        self.assertFalse(out.exists())

    def test_link_rejects_malformed_archive(self):
        bad = self.proj / "malformed.a"
        bad.write_bytes(b"!<arch>\nshort")
        out = self.proj / "bad-archive-linked"
        r = run_paxsy("link", str(out), "--", str(bad), cwd=self.proj)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("malformed archive", r.stderr)
        self.assertFalse(out.exists())

    def test_link_missing_file_fails(self):
        out = self.proj / "nowhere"
        r = run_paxsy("link", str(out), "--",
                      str(self.proj / "missing.o"), cwd=self.proj)
        self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
