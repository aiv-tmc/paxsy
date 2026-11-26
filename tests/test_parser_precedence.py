import json
import unittest
from pathlib import Path

from paxsy_testlib import ProjectCase, run_paxsy


class ParserPrecedenceTests(ProjectCase):
    def test_mixed_precedence_expression_executes(self):
        project = self.init_demo()
        source = '''
use std::inout;
func main(): Void {
    let a = 2 + 3 * 4;
    let b = (2 + 3) * 4;
    let c = 64 >> 2 + 1;
    let d = 1 + 2 << 2;
    let f = 2 <<< 3;
    let e = 15 & 6 ^ 3 $ 8;
    std::inout::println("{a}|{b}|{c}|{d}|{e}|{f}");
}
'''
        (project / "src" / "main.px").write_text(source)
        result = run_paxsy("build", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = run_paxsy("run", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "14|20|8|12|13|16\n")

    def test_comparison_operators_share_precedence(self):
        project = self.init_demo()
        source = '''
use std::inout;
func main(): Void {
    let a = (1 < 2) == true;
    let b = (3 >= 2) != false;
    if a and b { std::inout::println("ok"); }
}
'''
        (project / "src" / "main.px").write_text(source)
        result = run_paxsy("build", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = run_paxsy("run", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "ok\n")

    def test_nested_if_expression_returns_the_selected_value(self):
        project = self.init_demo()
        source = '''
use std::inout;
func main(): Void {
    let value = if 2 > 1 {
        if 3 > 2 { 10 } else { 20 }
    } else {
        30
    };
    std::inout::println("value={value}");
}
'''
        (project / "src" / "main.px").write_text(source)
        result = run_paxsy("build", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = run_paxsy("run", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "value=10\n")

    def test_char_literal_is_a_numeric_utf8_code_unit(self):
        project = self.init_demo()
        source = '''
use std::inout;
func main(): Void {
    let letter: Char = 'A';
    let code = letter as Int32;
    std::inout::println("code={code}");
}
'''
        (project / "src" / "main.px").write_text(source)
        result = run_paxsy("build", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = run_paxsy("run", cwd=project)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "code=65\n")

    def test_vscode_extension_manifest_and_script_are_valid(self):
        root = Path(__file__).resolve().parent.parent / "editors" / "vscode"
        manifest = json.loads((root / "package.json").read_text())
        self.assertEqual(manifest["contributes"]["languages"][0]["extensions"], [".px"])
        self.assertTrue((root / manifest["main"]).is_file())
        self.assertTrue((root / manifest["contributes"]["grammars"][0]["path"]).is_file())
        self.assertTrue((root / manifest["icon"]).is_file())


if __name__ == "__main__":
    unittest.main()
