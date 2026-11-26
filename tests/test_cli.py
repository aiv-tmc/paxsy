"""CLI conformance suite: the documented command surface exists, the
formatter obeys its safety contract, and the debugger command
answers honestly in both modes."""

import os
import unittest
from pathlib import Path

from paxsy_testlib import ProjectCase, run_paxsy

U = "use std::inout;\n"

DOCUMENTED_COMMANDS = [
    "new", "new", "build", "check", "run", "test", "bench", "clean",
    "tree", "clippy", "fmt", "debug", "fetch", "search", "install",
    "uninstall", "update", "add", "remove", "list", "publish",
    "meta", "config", "state", "shared", "disasm", "recovery", "link",
]


class CliSurfaceTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()


    def test_run_profile_executes_command_with_expansion_and_environment(self):
        import shutil
        printenv = shutil.which("printenv")
        self.assertIsNotNone(printenv)
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() +
            "\n[run.envcheck]\n"
            f'command = "{printenv}"\n'
            'args = ["PAXSY_PROFILE_VALUE"]\n'
            'env = ["PAXSY_PROFILE_VALUE=from-profile"]\n'
            'working_dir = "{root}"\n')
        result = run_paxsy("run", "--profile", "envcheck", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "from-profile")

    def test_run_profile_expands_project_and_forwards_arguments(self):
        import shutil
        printf = shutil.which("printf")
        self.assertIsNotNone(printf)
        built = run_paxsy("build", cwd=self.proj)
        self.assertEqual(built.returncode, 0, built.stderr)
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() +
            "\n[run.print]\n"
            f'command = "{printf}"\n'
            'args = ["%s\\n", "{project}", "{args}"]\n')
        result = run_paxsy("run", "--profile", "print", "--", "first", "second", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["demo", "first", "second"])

    def test_run_profile_option_requires_a_name(self):
        result = run_paxsy("run", "--profile", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("run --profile requires a profile name", result.stderr)

    def test_legacy_run_profile_option_is_rejected(self):
        result = run_paxsy("run", "--run-profile", "envcheck", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown option '--run-profile'", result.stderr)

    def test_run_profile_rejects_missing_command(self):
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + "\n[run.broken]\nargs = []\n")
        result = run_paxsy("run", "--profile", "broken", cwd=self.proj)
        self.assertEqual(result.returncode, 2)
        self.assertIn("requires [run.broken].command", result.stderr)

    def test_new_is_the_only_project_creation_command(self):
        created = run_paxsy("new", "only_new", "--no-git", cwd=self.tmp)
        self.assertEqual(created.returncode, 0, created.stderr)
        self.assertTrue((self.tmp / "only_new/src/main.px").is_file())
        self.assertTrue((self.tmp / "only_new/src/mod.px").is_file())
        removed = run_paxsy("init", "legacy", cwd=self.tmp)
        self.assertNotEqual(removed.returncode, 0)
        self.assertFalse((self.tmp / "legacy").exists())
        help_text = run_paxsy("--help", cwd=self.tmp)
        self.assertIn("new <name>", help_text.stdout)
        self.assertNotIn("init <name>", help_text.stdout)

    def test_unknown_names_inside_interpolation_point_to_the_expression(self):
        (self.proj / "src/main.px").write_text(
            'use std::inout;\n'
            'func main(): Void {\n'
            '    std::inout::println("{missing_value} {another_missing}");\n'
            '}\n')
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown variable or function 'missing_value'", result.stderr)
        self.assertRegex(result.stderr, r"(?m)^\s*3 \|.*missing_value")
        self.assertNotRegex(result.stderr, r"(?m)^\s*1 \| use std::inout")
        self.assertIn("}\n\nsrc/main.px: error:", result.stderr)

    def test_build_profile_reports_compiler_stages(self):
        result = run_paxsy("build", "--profile", cwd=self.proj)
        self.assertEqual(result.returncode, 0, result.stderr)
        for stage in ("source loading", "lexing", "parsing", "semantics",
                      "lowering", "HIR typing", "optimization",
                      "code generation", "measured total"):
            self.assertIn(stage, result.stderr)
        # Profiling deliberately bypasses the normal fresh-build shortcut.
        repeated = run_paxsy("build", "--profile", cwd=self.proj)
        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertIn("Paxsy compilation profile", repeated.stderr)

    def test_backend_diagnostics_are_separated_from_frontend_summary(self):
        (self.proj / "src/main.px").write_text(
            'use std::inout;\n'
            'func main(): Void {\n'
            '    std::inout::println("one")\n'
            '    std::inout::println("two");\n'
            '}\n')
        result = run_paxsy("build", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected ';' after expression", result.stderr)
        self.assertIn("\n1 error, 0 warnings\n\n<nemit>: error:", result.stderr)

    def test_missing_semicolon_help_points_to_previous_statement(self):
        (self.proj / "src/main.px").write_text(
            'use std::inout;\n'
            'func main(): Void {\n'
            '    std::inout::println("first")\n'
            '    std::inout::println("second");\n'
            '}\n')
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected ';' after expression", result.stderr)
        self.assertIn("help: Insert ';' at the end of the previous statement.", result.stderr)
        self.assertRegex(result.stderr, r"(?m)^\s*3 \|.*first")

    def test_alloc_and_free_are_highlighted_as_native_memory_keywords(self):
        import json
        syntax = json.loads((Path(__file__).resolve().parent.parent /
                             "editors/vscode/syntaxes/px.tmLanguage.json").read_text())
        root_patterns = syntax["patterns"]
        alloc = next(p for p in root_patterns
                     if p.get("match") == r"\balloc\b")
        free = next(p for p in root_patterns
                    if p.get("match") == r"\bfree\b")
        self.assertEqual(alloc["name"], "keyword.operator.px")
        self.assertEqual(free["name"], "keyword.control.px")
        self.assertNotIn("alloc", syntax["repository"]["builtins"]["match"])
        self.assertNotIn("free", syntax["repository"]["builtins"]["match"])

    def test_free_requires_semicolon_even_inside_defer(self):
        (self.proj / "src/main.px").write_text(
            "func main(): Void {\n"
            "    let p: &Int = alloc Int[2];\n"
            "    defer free p\n"
            "}\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected ';' after free", result.stderr)

    @unittest.skipUnless(__import__("shutil").which("git"), "git is not installed")
    def test_git_automation_creates_work_branch_and_safe_commit(self):
        import subprocess
        subprocess.run(["git", "config", "user.name", "Paxsy Test"], cwd=self.proj, check=True)
        subprocess.run(["git", "config", "user.email", "paxsy-test@example.invalid"], cwd=self.proj, check=True)
        r = run_paxsy("build", "--verbose", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        branch = subprocess.check_output(
            ["git", "branch", "--show-current"], cwd=self.proj, text=True).strip()
        self.assertEqual(branch, "paxsy/demo")
        subject = subprocess.check_output(
            ["git", "log", "-1", "--pretty=%s"], cwd=self.proj, text=True).strip()
        self.assertEqual(subject, "build: demo (4 files)")
        self.assertEqual(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=self.proj, text=True), "")

    def test_free_requires_semicolon_even_inside_defer(self):
        (self.proj / "src/main.px").write_text(
            "func main(): Void {\n"
            "    let p: &Int = alloc Int[2];\n"
            "    defer free p\n"
            "}\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected ';' after free", result.stderr)

    @unittest.skipUnless(__import__("shutil").which("git"), "git is not installed")
    def test_new_no_git_disables_repository_and_persists_policy(self):
        r = run_paxsy("new", "without_git", "--no-git", cwd=self.tmp)
        self.assertEqual(r.returncode, 0, r.stderr)
        project = self.tmp / "without_git"
        self.assertIn("enabled = false", (project / "without_git.toml").read_text())
        self.assertFalse((project / ".git").exists())

    def test_free_requires_semicolon_even_inside_defer(self):
        (self.proj / "src/main.px").write_text(
            "func main(): Void {\n"
            "    let p: &Int = alloc Int[2];\n"
            "    defer free p\n"
            "}\n")
        result = run_paxsy("check", cwd=self.proj)
        self.assertEqual(result.returncode, 1)
        self.assertIn("expected ';' after free", result.stderr)

    @unittest.skipUnless(__import__("shutil").which("git"), "git is not installed")
    def test_git_master_switch_disables_automation(self):
        import subprocess
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + "\n[git]\nenabled = false\n")
        subprocess.run(["git", "config", "user.name", "Paxsy Test"], cwd=self.proj, check=True)
        subprocess.run(["git", "config", "user.email", "paxsy-test@example.invalid"], cwd=self.proj, check=True)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        branch = subprocess.check_output(
            ["git", "branch", "--show-current"], cwd=self.proj, text=True).strip()
        self.assertIn(branch, ("master", "main"))
        head = subprocess.run(["git", "rev-parse", "--verify", "HEAD"],
                              cwd=self.proj, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
        self.assertNotEqual(head.returncode, 0)

    def test_doc_command_is_not_part_of_the_cli(self):
        help_result = run_paxsy("--help", cwd=self.proj)
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertNotIn("doc [path]", help_result.stdout)
        result = run_paxsy("doc", cwd=self.proj)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown command", result.stderr)

    def test_help_lists_every_documented_command(self):
        r = run_paxsy("--help", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        for cmd in DOCUMENTED_COMMANDS:
            self.assertIn(cmd, r.stdout, f"--help does not list {cmd}")

    def test_fmt_reformats_and_preserves_semantics(self):
        (self.proj / "src/main.px").write_text(
            "use std::inout;\n"
            "func main(): Void { let x: Int = 5; "
            "if x > 3 { std::inout::println(\"big\"); } "
            "else { std::inout::println(\"small\"); } }\n")
        r = run_paxsy("fmt", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        formatted = (self.proj / "src/main.px").read_text()
        self.assertIn("if x > 3 {\n", formatted)
        self.assertIn("} else {\n", formatted)
        self.assertNotIn("  \n", formatted)          # no trailing ws
        self.assertTrue(formatted.endswith("\n"))
        self.assertNotIn("\n\n\n", formatted)
        # semantics preserved: still compiles and runs
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "big\n")
        # idempotency
        r = run_paxsy("fmt", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "")   # success is silent

    def test_fmt_check_mode(self):
        (self.proj / "src/main.px").write_text(
            U + "func main(): Void { let x: Int = 5; "
            "std::inout::println(\"x={x}\"); }\n")
        r = run_paxsy("fmt", "--check", cwd=self.proj)
        self.assertEqual(r.returncode, 1, "check mode must report diffs")
        r = run_paxsy("fmt", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("fmt", "--check", cwd=self.proj)
        self.assertEqual(r.returncode, 0, "clean tree passes check mode")

    def test_fmt_never_loses_source(self):
        # A file whose layout the formatter cannot prove token-safe
        # must be left untouched, never truncated.
        tricky = ("/* block comment */\n"
                  "use std::inout;\n"
                  "func main(): Void { /* inline */ "
                  "std::inout::println(\"ok\"); }\n")
        (self.proj / "src/main.px").write_text(tricky)
        r = run_paxsy("fmt", cwd=self.proj)
        self.assertIn(r.returncode, (0, 1))
        self.assertNotIn("left unchanged", r.stdout + r.stderr)
        self.assertNotIn("paxsy:", r.stdout + r.stderr)
        after = (self.proj / "src/main.px").read_text()
        self.assertIn("block comment", after)   # comment survived
        self.assertIn("inline", after)

    def test_recovery_restores_original_source_byte_for_byte(self):
        (self.proj / "src/main.px").write_text(
            "func classify(x: Int): Int { if x > 5 { return 1; } "
            "return 2; }\n"
            "func main(): Void { let v: Int = classify(7); "
            "if v == 1 { pass; } }\n")
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + "\n[recovery]\nembed_source = true\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("recovery", "build/demo", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        px = (self.proj / "build/demo.recovered/src/main.px").read_text()
        self.assertEqual(px, (self.proj / "src/main.px").read_text())
        # Exact recovery round-trips the original source byte-for-byte.
        (self.proj / "src/main.px").write_text(px)
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_recovery_restores_original_println_source(self):
        (self.proj / "src/main.px").write_text(
            U + "func main(): Void { std::inout::println(\"hi\"); }\n")
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + "\n[recovery]\nembed_source = true\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("recovery", "build/demo", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        px = (self.proj / "build/demo.recovered/src/main.px").read_text()
        self.assertEqual(px, (self.proj / "src/main.px").read_text())

    def test_recovery_without_argument_uses_project_output(self):
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + "\n[recovery]\nembed_source = true\n")
        r = run_paxsy("recovery", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.proj / "build/demo.recovered/src/main.px").read_text(),
                         (self.proj / "src/main.px").read_text())

    def test_recovery_keeps_disassembly_intermediate_files_private(self):
        import shutil
        binary = self.proj / "foreign-recovery"
        shutil.copy2("/bin/true", binary)
        r = run_paxsy("recovery", str(binary), cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.proj / "foreign-recovery.recovered/src").is_dir())
        self.assertFalse((self.proj / "foreign-recovery.disasm").exists())
        self.assertFalse((self.proj / "foreign-recovery.recovered/.recovery-asm").exists())

    def test_recovery_of_foreign_binary_is_honest_about_assembly(self):
        import shutil
        binary = self.proj / "foreign program"
        shutil.copy2("/bin/true", binary)
        r = run_paxsy("recovery", str(binary), cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Recovered project:", r.stdout)
        recovered_project = self.proj / "foreign program.recovered"
        recovered = recovered_project / "src"
        self.assertTrue(recovered.is_dir())
        recovered_files = list(recovered.glob("*.px"))
        self.assertTrue(recovered_files)
        self.assertIn("asm \"", recovered_files[0].read_text())
        recovered_config = recovered_project / "foreign_program.toml"
        self.assertTrue(recovered_config.is_file())
        self.assertTrue((recovered_project / "tests/recovered_smoke.px").is_file())
        self.assertTrue((recovered_project / "benches/recovered_smoke.px").is_file())
        self.assertFalse((recovered_project / "RECOVERY.md").exists())
        self.assertFalse((recovered_project / "std").exists())
        checked = run_paxsy("check", str(recovered_project), cwd=self.proj)
        self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_recovery_restores_multiple_source_files(self):
        main = self.proj / "src/main.px"
        main.write_text("func main(): Void { helpers::helper(); }\n")
        helper = self.proj / "src/helper.px"
        helper.write_text("module helpers { func helper(): Void { pass; } }\n")
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() + "\n[recovery]\nembed_source = true\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("recovery", "build/demo", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        restored = self.proj / "build/demo.recovered"
        self.assertEqual((restored / "src/main.px").read_text(), main.read_text())
        self.assertEqual((restored / "src/helper.px").read_text(), helper.read_text())
        self.assertFalse((restored / "RECOVERY.md").exists())
        self.assertTrue((restored / "demo.toml").is_file())
        self.assertTrue((restored / "tests/recovered_smoke.px").is_file())
        self.assertTrue((restored / "benches/recovered_smoke.px").is_file())

    def test_disasm_annotates_the_binary(self):
        (self.proj / "src/main.px").write_text(
            U + "func main(): Void { std::inout::println(\"hi\"); }\n")
        r = run_paxsy("disasm", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm_dir = self.proj / "demo.disasm/src"
        self.assertTrue(asm_dir.is_dir())
        main_asm = (asm_dir / "main.asm").read_text()
        self.assertIn("<main>", main_asm)
        self.assertNotIn("prologue", main_asm)
        self.assertIn("call", main_asm)
        self.assertRegex(main_asm, r"(?m)^  000000000040[0-9a-f]+")

    def test_disasm_has_one_simple_default_view(self):
        (self.proj / "src/main.px").write_text(
            "func helper(x: Int): Int { return x + 2; }\n"
            "func main(): Void { let x: Int = helper(40); }\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("disasm", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm_dir = self.proj / "demo.disasm/src"
        self.assertTrue((asm_dir / "helper.asm").is_file())
        self.assertTrue((asm_dir / "main.asm").is_file())
        self.assertIn("add", (asm_dir / "helper.asm").read_text())

    def test_disasm_rejects_presentation_flags(self):
        r = run_paxsy("disasm", "--function", "main", cwd=self.proj)
        self.assertEqual(r.returncode, 2)
        self.assertIn("unknown option", r.stderr)

    def test_disasm_passes_unusual_binary_paths_without_a_shell(self):
        import shutil
        built = self.proj / "build/demo"
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        odd = self.proj / "build" / "quoted ' ; touch PAXSY_DISASM_INJECTED"
        shutil.copy2(built, odd)
        r = run_paxsy("disasm", str(odd), cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        asm_dir = self.proj / "quoted ' ; touch PAXSY_DISASM_INJECTED.disasm/src"
        self.assertTrue(asm_dir.is_dir())
        self.assertFalse((self.proj / "PAXSY_DISASM_INJECTED").exists())

    def test_disasm_uses_configured_build_output(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[build]\noutput = "out/custom_demo"\n')
        r = run_paxsy("disasm", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.proj / "out/custom_demo").is_file())
        self.assertTrue((self.proj / "custom_demo.disasm/src/main.asm").is_file())

    def test_init_generates_compact_active_configuration(self):
        config = (self.proj / "demo.toml").read_text()
        self.assertLess(len(config), 3000)
        self.assertIn('[project]', config)
        self.assertIn('name = "demo"', config)
        self.assertIn('version = "0.1.0"', config)
        self.assertNotIn('[build]', config)
        self.assertNotIn('[source]', config)
        self.assertNotIn('[artifacts]', config)
        self.assertNotIn('[quality]', config)
        self.assertNotIn('[recovery]', config)
        self.assertNotIn('[debug]', config)
        self.assertNotIn('[git]', config)
        self.assertFalse((self.proj / 'tests').exists())
        self.assertFalse((self.proj / 'benches').exists())

    def test_debug_builtin_session(self):
        (self.proj / "src/main.px").write_text(
            U + "func main(): Void { std::inout::println(\"hi\"); }\n")
        cfg = self.proj / "demo.toml"
        cfg.write_text(cfg.read_text() +
                       '\n[debug]\nbreakpoints = "main"\nregs = true\nrun = true\n')
        import subprocess
        p = subprocess.run(
            [os.environ["PAXSY_CMD"], "debug"], cwd=self.proj,
            capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("breakpoint 1", p.stdout)
        self.assertIn("rip=0x", p.stdout)          # register dump
        self.assertIn("exited with code 0", p.stdout)

    def test_formatter_is_silent_on_success(self):
        r = run_paxsy("fmt", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "")
        self.assertEqual(r.stderr, "")

    def test_help_points_to_project_config_without_debug_flags(self):
        r = run_paxsy("--help", cwd=self.proj)
        self.assertEqual(r.returncode, 0)
        self.assertNotIn("--trace-memory", r.stdout)
        self.assertNotIn("--function", r.stdout)
        self.assertIn("project TOML", r.stdout)
        self.assertNotIn("--builtin", r.stdout)
        self.assertNotIn("--emit-exe", r.stdout)

    def test_debug_breakpoint_without_run_stops_at_the_breakpoint(self):
        (self.proj / "src/main.px").write_text(
            U + "func main(): Void { std::inout::println(\"stop\"); }\n")
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[debug]\nbreakpoints = "main"\nregs = true\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("breakpoint 1", r.stdout)
        self.assertIn("rip=0x", r.stdout)
        self.assertNotIn("exited with code 0", r.stdout)

    def test_debug_builds_native_executable_even_when_project_default_is_pe(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[build]\ntarget = "windows-x86_64"\n'
                          'output = "build/demo.exe"\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        binary = (self.proj / "build/demo.exe").read_bytes()
        self.assertEqual(binary[:4], b"\x7fELF")

    def test_debug_configuration_supplies_breakpoint_and_trace_defaults(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[debug]\ntrace_memory = true\n'
                          'trace_file = "configured.trace"\n'
                          'breakpoints = "main"\nregs = true\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("breakpoint 1", r.stdout)
        self.assertIn("rip=0x", r.stdout)
        self.assertIn("memory read", (self.proj / "configured.trace").read_text())

    def test_trace_file_is_configured_in_project_toml(self):
        trace = self.proj / "debug.trace"
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[debug]\nbreakpoints = "main"\n'
                          'trace_memory = true\ntrace_file = "' + str(trace) + '"\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(trace.is_file())
        self.assertIn("memory read", trace.read_text())

    def test_debug_rejects_unknown_breakpoint(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[debug]\nbreakpoints = "missing_function"\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 2)
        self.assertIn("no symbol 'missing_function'", r.stderr)

    def test_debug_rejects_invalid_project_step_count(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() + '\n[debug]\nstepi = "many"\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 2)
        self.assertIn("invalid [debug].stepi", r.stderr)

    def test_source_recursive_policy_is_configured_once(self):
        nested = self.proj / "src/nested/broken.px"
        nested.parent.mkdir()
        nested.write_text("func broken(: Void {\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() + '\n[source]\nrecursive = true\n')
        r = run_paxsy("build", cwd=self.proj)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("broken.px", r.stderr + r.stdout)

    def test_debug_can_be_disabled_in_project_configuration(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() + '\n[debug]\nenabled = false\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 2)
        self.assertIn("disabled by [debug].enabled", r.stderr)
        self.assertFalse((self.proj / "build/demo").exists())

    def test_debug_controls_are_configured_in_toml_not_flags(self):
        r = run_paxsy("debug", "--stepi", "not-a-number", cwd=self.proj)
        self.assertEqual(r.returncode, 2)
        self.assertIn("unknown option", r.stderr)

    def test_debug_memory_trace_setting_writes_a_trace_file(self):
        config = self.proj / "demo.toml"
        config.write_text(config.read_text() +
                          '\n[debug]\ntrace_memory = true\n'
                          'trace_registers = true\ntrace_file = "trace.log"\n'
                          'breakpoints = "main"\nrun = true\n')
        r = run_paxsy("debug", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        trace = self.proj / "trace.log"
        self.assertTrue(trace.is_file())
        contents = trace.read_text()
        self.assertIn("memory read", contents)
        self.assertIn("memory write", contents)
        self.assertIn("registers rip=", contents)
        self.assertIn("r15=", contents)
        self.assertIn("before=0x", contents)
        self.assertIn("after=0x", contents)


if __name__ == "__main__":
    unittest.main()

