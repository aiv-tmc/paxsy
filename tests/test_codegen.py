"""End-to-end tests for the executable backend: build must produce a
working binary in build/ and run must execute it."""

from paxsy_testlib import ProjectCase, run_paxsy


class CodegenEndToEndTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def test_hello_world_builds_and_runs(self):
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "Hello, World\n")

    def test_module_wrapped_program_builds(self):
        """A program whose declarations sit inside a module wrapper
        lowers every member and resolves the entry point."""
        (self.proj / "src/main.px").write_text(
            "module demo {\n"
            "    use std::inout;\n"
            "\n"
            "    func greet(): Int32 {\n"
            "        return 6 * 7;\n"
            "    }\n"
            "\n"
            "    func main(): Void! {\n"
            "        if greet() == 42 {\n"
            "            try std::inout::println(\"ok\");\n"
            "        } else {\n"
            "            try std::inout::println(\"bad\");\n"
            "        }\n"
            "    }\n"
            "}\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.stdout, "ok\n")

    def test_control_flow_and_calls(self):
        (self.proj / "src/main.px").write_text(
            "func add(a: Int32, b: Int32): Int32 {\n"
            "    let sum = a + b;\n"
            "    return sum;\n"
            "}\n"
            "\n"
            "func main(): Void! {\n"
            "    try std::inout::println(\"start\");\n"
            "    let total = 0;\n"
            "    for 1..5 |i| {\n"
            "        total += i;\n"
            "    }\n"
            "    if add(total, 32) == 42 {\n"
            "        try std::inout::println(\"ok\");\n"
            "    } else {\n"
            "        try std::inout::println(\"bad\");\n"
            "    }\n"
            "}\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout, "start\nok\n")

    def test_error_codes_propagate(self):
        """A T! function returns {value, err}; try unwraps and
        propagates, and main exits with the error code."""
        (self.proj / "src/main.px").write_text(
            "func might_fail(x: Int32): Int32! {\n"
            "    if x > 5 {\n"
            "        return error 7;\n"
            "    }\n"
            "    return x;\n"
            "}\n"
            "\n"
            "func main(): Void! {\n"
            "    let a = try might_fail(1);\n"
            "    let b = try might_fail(9);\n"
            "}\n")
        r = run_paxsy("build", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.stdout, "")
        self.assertEqual(r.returncode, 7)

    def test_match_lowers_to_switch(self):
        (self.proj / "src/main.px").write_text(
            "func classify(x: Int32): Int32 {\n"
            "    match x {\n"
            "    case 1 {\n"
            "        return 10;\n"
            "    }\n"
            "    case 2 or 3 {\n"
            "        return 20;\n"
            "    }\n"
            "    else {\n"
            "        return 0;\n"
            "    }\n"
            "}\n"
            "}\n"
            "\n"
            "func main(): Void! {\n"
            "    if classify(1) == 10 && classify(3) == 20 {\n"
            "        try std::inout::println(\"ok\");\n"
            "    } else {\n"
            "        try std::inout::println(\"bad\");\n"
            "    }\n"
            "}\n")
        r = run_paxsy("build", "--emit-llvm", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.stdout, "ok\n")
        ll = (self.proj / "build/bin/demo.ll").read_text()
        self.assertIn("switch i32", ll)
        self.assertIn("unreachable", ll)   # merge block after all-rets
        self.assertIn("px_res_i32", ll)    # T! ABI structs

    def test_emit_cuda_writes_kernels(self):
        (self.proj / "src/main.px").write_text(
            "func sum_range(): Int32 {\n"
            "    let acc = 0;\n"
            "    for 1..10 |i| {\n"
            "        acc += i;\n"
            "    }\n"
            "    return acc;\n"
            "}\n"
            "\n"
            "func main(): Void! {\n"
            "    if sum_range() == 45 {\n"
            "        try std::inout::println(\"ok\");\n"
            "    } else {\n"
            "        try std::inout::println(\"bad\");\n"
            "    }\n"
            "}\n")
        r = run_paxsy("build", "--emit-cuda", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        cu = (self.proj / "build/bin/demo.cu").read_text()
        self.assertIn("__global__ void px_kernel_0", cu)
        self.assertIn("atomicAdd(px_out, px_i)", cu)
        self.assertIn("px_kernel_0<<<blocks, threads>>>", cu)
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.stdout, "ok\n")

    def test_run_without_build_fails(self):
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.returncode, 1)

    def test_gpu_map_pattern(self):
        """A loop of the form arr[i] = expr(i) over a pointer is a GPU
        map candidate: the CUDA artifact holds a per-thread store
        kernel without atomics, and the host result is verified on
        the CPU by summing the filled array."""
        (self.proj / "src/main.px").write_text(
            "func fill(a: &Int32, n: Int32): Void {\n"
            "    for 0..n |i| {\n"
            "        a[i] = i * 2;\n"
            "    }\n"
            "}\n"
            "\n"
            "func sum_arr(a: &Int32, n: Int32): Int32 {\n"
            "    let acc = 0;\n"
            "    for 0..n |i| { acc += a[i]; }\n"
            "    return acc;\n"
            "}\n"
            "\n"
            "func main(): Void! {\n"
            "    let vals: Int32[5] = {0, 0, 0, 0, 0};\n"
            "    fill(&vals, 5);\n"
            "    if sum_arr(&vals, 5) == 20 {\n"
            "        try std::inout::println(\"ok\");\n"
            "    } else {\n"
            "        try std::inout::println(\"bad\");\n"
            "    }\n"
            "}\n")
        r = run_paxsy("build", "--emit-cuda", cwd=self.proj)
        self.assertEqual(r.returncode, 0, r.stderr)
        cu = (self.proj / "build/bin/demo.cu").read_text()
        self.assertIn("__global__ void px_kernel_0", cu)
        self.assertNotIn("atomicAdd",
                         cu.split("px_kernel_0")[1].split("}")[0])
        r = run_paxsy("run", cwd=self.proj)
        self.assertEqual(r.stdout, "ok\n")


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
