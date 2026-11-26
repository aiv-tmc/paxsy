
import os, subprocess, shutil, sys

PAXSY = os.environ.get("PAXSY_CMD", "/tmp/pt/paxsy")
WORK = "/tmp/specwork"
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)

PASS = []
FAIL = []

def run_case(name, main_body, expected=None, expect_fail=False):
    d = os.path.join(WORK, name.replace("/", "_"))
    os.makedirs(d + "/src", exist_ok=True)
    open(d + "/demo.toml", "w").write('[project]\nname = "demo"\nversion = "0.1.0"\n')
    open(d + "/src/main.px", "w").write(main_body)
    b = subprocess.run([PAXSY, "build"], cwd=d, capture_output=True, text=True, timeout=60)
    if b.returncode != 0:
        if expect_fail:
            PASS.append(name); return
        FAIL.append((name, "build", (b.stdout + b.stderr)[:400])); return
    if expect_fail:
        FAIL.append((name, "expected compile error but built", "")); return
    r = subprocess.run(["./build/demo"], cwd=d, capture_output=True, text=True, timeout=10)
    out = r.stdout
    if expected is None or out == expected:
        PASS.append(name)
    else:
        FAIL.append((name, "run", "rc=%d out=%r want %r" % (r.returncode, out, expected)))

WRAP = 'module demo {\n    use std::inout;\n%s\n}\n'

# ============ TYPES ============
run_case("t/int-widths", WRAP % '''
    func main(): Void! {
        let a: Int8 = 127; let b: Int16 = 32767; let c: Int32 = 2147483647;
        let u: UInt8 = 255;
        try std::inout::println("ok");
    }''', "ok\n")
run_case("t/char-arith", WRAP % '''
    func main(): Void! {
        let c: Char = 'A';
        try std::inout::println("c={c + 1}");
    }''', "c=66\n")
run_case("t/bool-ops", WRAP % '''
    func main(): Void! {
        let a = 1 < 2 and 3 > 2;
        let b = 1 > 2 or 3 > 2;
        let c = not (1 == 1);
        try std::inout::println("a={a} b={b} c={c}");
    }''', "a=1 b=1 c=0\n")
run_case("t/casts", WRAP % '''
    func main(): Void! {
        let x: Int32 = 300;
        let y: Int8 = x as Int8;
        let z = 65 as Char;
        let p = 1234 as UInt64;
        try std::inout::println("y={y} z={z} p={p}");
    }''', "y=44 z=65 p=1234\n")
run_case("t/cast-sign", WRAP % '''
    func main(): Void! {
        let x: Int32 = -1;
        let y: UInt8 = x as UInt8;
        let s: Int8 = x as Int8;
        try std::inout::println("y={y} s={s}");
    }''', "y=255 s=-1\n")
run_case("t/bitwise", WRAP % '''
    func main(): Void! {
        let a = 12 & 10; let b = 12 $ 10; let c = 12 ^ 10;
        let e = 1 << 4; let f = 256 >> 4;
        try std::inout::println("a={a} b={b} c={c} e={e} f={f}");
    }''', "a=8 b=14 c=6 e=16 f=16\n")
run_case("t/mod-div", WRAP % '''
    func main(): Void! {
        try std::inout::println("m={17 % 5} d={17 / 5} n={-17 / 5}");
    }''', "m=2 d=3 n=-3\n")

# ============ CONTROL FLOW ============
run_case("cf/if-else-chain", WRAP % '''
    func main(): Void! {
        let v = 7;
        if v < 5 { try std::inout::println("low"); }
        else if v < 10 { try std::inout::println("mid"); }
        else { try std::inout::println("high"); }
    }''', "mid\n")
run_case("cf/if-value-nested", WRAP % '''
    func main(): Void! {
        let x = if 2 > 1 { if 3 > 2 { 10 } else { 20 } } else { 30 };
        try std::inout::println("x={x}");
    }''', "x=10\n")
run_case("cf/match-or-else", WRAP % '''
    func main(): Void! {
        match 5 {
            case 1 or 5 { try std::inout::println("hit"); }
            else { try std::inout::println("miss"); }
        }
    }''', "hit\n")
run_case("cf/match-expr-arms", WRAP % '''
    func main(): Void! {
        let x: Int32 = match 2 {
            case 1 { 100; }
            case 2 { 200; }
            else { 300; }
        };
        try std::inout::println("x={x}");
    }''', "x=200\n")
run_case("cf/while-nested", WRAP % '''
    func main(): Void! {
        let i: Int32 = 0; let c: Int32 = 0;
        while i < 3 {
            let j: Int32 = 0;
            while j < 3 { c += 1; j += 1; }
            i += 1;
        }
        try std::inout::println("c={c}");
    }''', "c=9\n")
run_case("cf/do-while-false", WRAP % '''
    func main(): Void! {
        let i: Int32 = 0;
        do { i += 1; } while i < 0;
        try std::inout::println("i={i}");
    }''', "i=1\n")
run_case("cf/break-continue", WRAP % '''
    func main(): Void! {
        let i: Int32 = 0; let s: Int32 = 0;
        while i < 10 {
            i += 1;
            if i % 2 == 0 { continue; }
            if i > 7 { break; }
            s += i;
        }
        try std::inout::println("s={s} i={i}");
    }''', "s=16 i=9\n")

# ============ FUNCTIONS ============
run_case("f/recursion", WRAP % '''
    func fact(n: Int32): Int32 {
        if n <= 1 { return 1; }
        return n * fact(n - 1);
    }
    func main(): Void! { try std::inout::println("f={fact(5)}"); }''', "f=120\n")
run_case("f/multi-args", WRAP % '''
    func add3(a: Int32, b: Int32, c: Int32): Int32 { return a + b + c; }
    func add7(a: Int32, b: Int32, c: Int32, d: Int32, e: Int32, f: Int32, g: Int32): Int32 {
        return a + b + c + d + e + f + g;
    }
    func main(): Void! { try std::inout::println("a={add3(1,2,3)} b={add7(1,2,3,4,5,6,7)}"); }''', "a=6 b=28\n")
run_case("f/early-return", WRAP % '''
    func f(x: Int32): Int32 {
        if x > 0 { return 1; }
        return 2;
    }
    func main(): Void! { try std::inout::println("f={f(5)} {f(-5)}"); }''', "f=1 2\n")

# ============ STRINGS ============
run_case("s/strlen-streq", WRAP % '''
    func main(): Void! {
        let e: Char[] = "";
        let abc: Char[] = "abc";
        let ab: Char[] = "ab";
        let cd: Char[] = "cd";
        try std::inout::println("{std::string::strlen(e)}-{std::string::strlen(abc)}-{std::string::streq(ab,ab)}-{std::string::streq(ab,cd)}");
    }''', "0-3-1-0\n")

# ============ TUPLES ============
run_case("tp/basic", WRAP % '''
    func main(): Void! {
        let t = {1, "a", 3};
        try std::inout::println("{t[0]}|{t[1]}|{t[2]}");
    }''', "1|a|3\n")
run_case("tp/destructure-typed", WRAP % '''
    func main(): Void! {
        let (x: Int32, s: Char[]) = {9, "z"};
        try std::inout::println("{x}|{s}");
    }''', "9|z\n")

# ============ STRUCTS ============
run_case("st/positional", WRAP % '''
    static P = Struct { x: Int32, y: Int32 };
    func main(): Void! {
        let p = P { 3, 4 };
        try std::inout::println("{p.x},{p.y}");
    }''', "3,4\n")
run_case("st/named-partial", WRAP % '''
    static P = Struct { x: Int32, y: Int32 };
    func main(): Void! {
        let p = P { .y = 4 };
        try std::inout::println("{p.x},{p.y}");
    }''', "0,4\n")
run_case("st/pass-return", WRAP % '''
    static P = Struct { x: Int32, y: Int32 };
    func swap(p: P): P { return P { .x = p.y, .y = p.x }; }
    func main(): Void! {
        let q = swap(P { .x = 1, .y = 2 });
        try std::inout::println("{q.x},{q.y}");
    }''', "2,1\n")

# ============ ENUM ============
run_case("en/basic", WRAP % '''
    static E = Enum { A, B, C };
    func main(): Void! {
        let e = E.B;
        let n: Int32 = e as Int32;
        try std::inout::println("n={n}");
    }''', "n=1\n")

# ============ MEMORY ============
run_case("m/alloc-free", WRAP % '''
    use std::heap;
    func main(): Void! {
        let p = alloc Int32;
        if p == 0 { return error 1; }
        @p = 42;
        try std::inout::println("v={@p}");
        free p;
    }''', "v=42\n")
run_case("m/alloc-array", WRAP % '''
    use std::heap;
    func main(): Void! {
        let a = alloc Int32[4];
        @(a + 0) = 7;
        @(a + 1) = 8;
        try std::inout::println("v={@(a + 0)} {@(a + 1)}");
        free a;
    }''', "v=7 8\n")
run_case("m/defer", WRAP % '''
    func main(): Void! {
        let a: Int32 = 0;
        {
            defer { a += 10; }
            a += 1;
        }
        try std::inout::println("a={a}");
    }''', "a=11\n")

# ============ EDGE ============
run_case("e/deep-expr", WRAP % '''
    func main(): Void! {
        let x = 1 + 2 * 3 - 4 / 2 + (5 - 1) * 2;
        try std::inout::println("x={x}");
    }''', "x=13\n")
run_case("e/deep-recursion", WRAP % '''
    func deep(n: Int32): Int32 {
        if n == 0 { return 0; }
        return deep(n - 1) + 1;
    }
    func main(): Void! { try std::inout::println("d={deep(200)}"); }''', "d=200\n")
run_case("e/interp-call", WRAP % '''
    func f(): Int32 { return 21; }
    func main(): Void! { try std::inout::println("x={f() * 2}"); }''', "x=42\n")
run_case("e/global-const", WRAP % '''
    static G = 10;
    func main(): Void! { try std::inout::println("g={G}"); }''', "g=10\n")
run_case("e/interp-in-cond", WRAP % '''
    func main(): Void! {
        let x = 5;
        if x == 5 { try std::inout::println("eq {x}"); }
    }''', "eq 5\n")
run_case("e/interp-multi", WRAP % '''
    func main(): Void! {
        let a = 1; let b = 2; let c = 3;
        try std::inout::println("{a}{b}{c}|{a + b + c}");
    }''', "123|6\n")

print("PASS %d / %d" % (len(PASS), len(PASS) + len(FAIL)))
for name, phase, info in FAIL:
    print("FAIL %s [%s]: %s" % (name, phase, info))
