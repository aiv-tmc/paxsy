"""Language-conformance suite: every construct of the specification
is compiled, executed and checked against golden stdout/exit codes.
Each positive case runs twice - with the optimisation pipeline and
with PAXSY_NO_OPT=1 - because both paths must produce identical
program semantics."""

import os
import unittest

from paxsy_testlib import ProjectCase, run_paxsy

U = "use std::inout;\n"

# (name, source, expected stdout)
POSITIVE = [
    # let declarations: all three specification forms
    ("let_annotated", U + "func main(): Void { let x: Int = 5; std::inout::println(\"x={x}\"); }\n", "x=5\n"),
    ("let_inferred", U + "func main(): Void { let x = 42; std::inout::println(\"x={x}\"); }\n", "x=42\n"),
    ("let_no_init", U + "func main(): Void { let x: Int; x = 7; std::inout::println(\"x={x}\"); }\n", "x=7\n"),
    ("let_multi", U + "func main(): Void { let (a: Int32 = 5, b: Int32 = 10); std::inout::println(\"s={a + b}\"); }\n", "s=15\n"),

    # arithmetic, precedence, all operators
    ("arith_precedence", U + "func main(): Void { std::inout::println(\"v={1 + 2 * 3}\"); }\n", "v=7\n"),
    ("arith_mod", U + "func main(): Void { std::inout::println(\"v={17 % 5}\"); }\n", "v=2\n"),
    ("arith_bits", U + "func main(): Void { std::inout::println(\"v={(6 & 3) + (6 $ 3) + (6 ^ 3)}\"); }\n", "v=14\n"),
    ("arith_shifts", U + "func main(): Void { std::inout::println(\"v={(3 << 2) + (16 >> 2)}\"); }\n", "v=16\n"),
    ("arith_unary", U + "func main(): Void { std::inout::println(\"v={-5 - 5}\"); }\n", "v=-10\n"),
    # Paxsy's deliberately distinct bitwise-OR spelling and operator aliases.
    ("bitwise_or_precedence", U + "func main(): Void { let v: Int32 = 1 $ 2 ^ 3 & 1; std::inout::println(\"v={v}\"); }\n", "v=3\n"),
    ("bitwise_or_left_assoc", U + "func main(): Void { let v: Int32 = 1 $ 2 $ 4; std::inout::println(\"v={v}\"); }\n", "v=7\n"),
    ("shift_left_alias", U + "func main(): Void { let v: Int32 = 3 <<< 2; std::inout::println(\"v={v}\"); }\n", "v=12\n"),
    ("logical_symbol_aliases", U + "func main(): Void { if true && (false || true) { std::inout::println(\"ok\"); } }\n", "ok\n"),
    ("bitwise_compound_assignments", U + "func main(): Void { let mut_dummy: Int32 = 6; mut_dummy &= 3; mut_dummy $= 8; mut_dummy ^= 2; mut_dummy <<= 1; mut_dummy >>= 1; mut_dummy >>>= 1; std::inout::println(\"v={mut_dummy}\"); }\n", "v=4\n"),

    # logic: full truth tables, non-vacuous operands
    ("and_tt", U + "func main(): Void { if true and true { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "T\n"),
    ("and_ft", U + "func main(): Void { if false and true { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "F\n"),
    ("or_tt", U + "func main(): Void { if true or true { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "T\n"),
    ("or_tf", U + "func main(): Void { if true or false { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "T\n"),
    ("or_ft", U + "func main(): Void { if false or true { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "T\n"),
    ("or_ff", U + "func main(): Void { if false or false { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "F\n"),
    ("not_v", U + "func main(): Void { if not (1 > 2) { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "T\n"),
    ("logic_word_ops", U + "func main(): Void { let a: Bool = true and false; let b: Bool = a or true; if b { std::inout::println(\"T\"); } }\n", "T\n"),

    # bitwise not on Bool via not, comparisons chained by and
    ("cmp_chain", U + "func main(): Void { let x: Int = 5; if x >= 5 and x <= 10 { std::inout::println(\"in\"); } }\n", "in\n"),

    # control flow: if/else, while, do-while, for ranges, labels
    ("if_else", U + "func main(): Void { let x: Int = 5; if x > 3 { std::inout::println(\"big\"); } else { std::inout::println(\"small\"); } }\n", "big\n"),
    ("while_loop", U + "func main(): Void { let i: Int = 0; while i < 3 { i = i + 1; } std::inout::println(\"i={i}\"); }\n", "i=3\n"),
    ("do_while", U + "func main(): Void { let i: Int = 0; do { i = i + 1; } while i < 3; std::inout::println(\"i={i}\"); }\n", "i=3\n"),
    ("do_while_break", U + "func main(): Void { let i: Int = 0; do { i = i + 1; if i > 2 { break; } } while true; std::inout::println(\"i={i}\"); }\n", "i=3\n"),
    ("for_range", U + "func main(): Void { let s: Int = 0; for 0..4 |i| { s = s + i; } std::inout::println(\"s={s}\"); }\n", "s=6\n"),
    ("for_inclusive", U + "func main(): Void { let s: Int = 0; for 0..=4 |i| { s = s + i; } std::inout::println(\"s={s}\"); }\n", "s=10\n"),
    ("for_exclusive", U + "func main(): Void { let s: Int = 0; for 0..<4 |i| { s = s + i; } std::inout::println(\"s={s}\"); }\n", "s=6\n"),
    ("for_continue", U + "func main(): Void { let c: Int = 0; for 0..5 |i| { if i == 2 { continue; } c = c + 1; } std::inout::println(\"c={c}\"); }\n", "c=4\n"),
    ("label_break", U + "func main(): Void { let i: Int = 0; outer: while true { i = i + 1; if i > 3 { break outer; } } std::inout::println(\"i={i}\"); }\n", "i=4\n"),
    ("label_continue", U + "func main(): Void { let i: Int = 0; let c: Int = 0; outer: while i < 5 { i = i + 1; if i == 3 { continue outer; } c = c + 1; } std::inout::println(\"c={c}\"); }\n", "c=4\n"),
    ("label_nested", U + "func main(): Void { let n: Int = 0; outer: for 0..3 |i| { inner: for 0..3 |j| { n = n + 1; if j == 1 { break outer; } } } std::inout::println(\"n={n}\"); }\n", "n=2\n"),

    # functions: recursion, multiple args, shadowing, dead lets
    ("func_recursion", U + "func fact(n: Int): Int { if n <= 1 { return 1; } return n * fact(n - 1); }\nfunc main(): Void { std::inout::println(\"f={fact(5)}\"); }\n", "f=120\n"),
    ("func_early_return", U + "func f(x: Int): Int { if x > 0 { return 1; } return -1; }\nfunc main(): Void { std::inout::println(\"v={f(5)}\"); }\n", "v=1\n"),
    ("dead_let_keeps_slots", U + "func main(): Void { let unused: Int = 1; let y: Int = 2; std::inout::println(\"y={y}\"); }\n", "y=2\n"),
    ("dead_let_chain", U + "func main(): Void { let unused: Int = 1; let y: Int = 2; let z: Int = y + 1; std::inout::println(\"z={z}\"); }\n", "z=3\n"),

    # arrays: fixed size, unsized with literal, indexing, stores
    ("array_fixed", U + "func main(): Void { let a: Int[3] = {5, 6, 7}; std::inout::println(\"v={a[1]}\"); }\n", "v=6\n"),
    ("array_unsized", U + "func main(): Void { let a: Int[] = {5, 6, 7}; std::inout::println(\"v={a[2]}\"); }\n", "v=7\n"),
    ("array_store", U + "func main(): Void { let a: Int[3] = {10, 20, 30}; a[1] = 99; std::inout::println(\"v={a[0] + a[1] + a[2]}\"); }\n", "v=139\n"),
    ("array_partial_init", U + "func main(): Void { let a: Int[4] = {1, 2}; std::inout::println(\"v={a[0] + a[1] + a[2] + a[3]}\"); }\n", "v=3\n"),
    ("array_len", U + "func main(): Void { let a: Int[4] = {1, 2, 3, 4}; std::inout::println(\"n={a.len}\"); }\n", "n=4\n"),
    ("for_array", U + "func main(): Void { let a: Int[3] = {5, 6, 7}; let s: Int = 0; for a |x| { s = s + x; } std::inout::println(\"s={s}\"); }\n", "s=18\n"),
    ("for_array_break", U + "func main(): Void { let a: Int[5] = {1, 2, 3, 4, 5}; let s: Int = 0; for a |x| { if x > 3 { break; } s = s + x; } std::inout::println(\"s={s}\"); }\n", "s=6\n"),

    # tuples and destructuring, mixed element types
    ("tuple_index", U + "func main(): Void { let t = {1, 2}; std::inout::println(\"v={t[0] + t[1]}\"); }\n", "v=3\n"),
    ("tuple_destructure", U + "func main(): Void { let (c, d) = {1, 2}; std::inout::println(\"v={c + d}\"); }\n", "v=3\n"),
    ("tuple_mixed", U + "func main(): Void { let (c, d) = {2.0, 1}; if d == 1 { std::inout::println(\"ok\"); } }\n", "ok\n"),
    ("tuple_mixed_rev", U + "func main(): Void { let (c, d) = {1, 2.0}; if c == 1 { std::inout::println(\"ok\"); } }\n", "ok\n"),

    # pointers: address-of, deref, pointer arithmetic, difference
    ("pointer_basic", U + "func main(): Void { let x: Int = 5; let p: &Int = &x; @p = 9; std::inout::println(\"x={x}\"); }\n", "x=9\n"),
    ("pointer_arith", U + "func main(): Void { let a: Int[3] = {10, 20, 30}; let p: &Int = &a[0]; std::inout::println(\"v={@(p + 2)}\"); }\n", "v=30\n"),
    ("pointer_diff", U + "func main(): Void { let a: Int[3] = {10, 20, 30}; let p: &Int = &a[0]; let q: &Int = &a[2]; let d: Int = q - p; std::inout::println(\"d={d}\"); }\n", "d=2\n"),
    ("byte_pointer_index", U + "func main(): Void { let s: &Int8 = \"abc\"; if s[1] == 98 { std::inout::println(\"ok\"); } }\n", "ok\n"),

    # structs
    ("struct_literal", U + "static Point = Struct { x: Int, y: Int };\nfunc main(): Void { let p: Point = Point {1, 2}; std::inout::println(\"v={p.x + p.y}\"); }\n", "v=3\n"),
    ("struct_named_init", U + "static Point = Struct { x: Int, y: Int };\nfunc main(): Void { let p: Point = Point {.y = 2, .x = 1}; std::inout::println(\"v={p.x + p.y}\"); }\n", "v=3\n"),
    ("struct_field_store", U + "static P = Struct { x: Int, y: Int };\nfunc main(): Void { let p: P = P {1, 2}; p.y = 10; std::inout::println(\"v={p.x + p.y}\"); }\n", "v=11\n"),

    # enums, with and without underlying type
    ("enum_plain", U + "static E = Enum { A, B, C };\nfunc main(): Void { let s: E = E.C; std::inout::println(\"v={s as Int}\"); }\n", "v=2\n"),
    ("enum_typed", U + "static E = Enum Int32 { Ready = 1, Running = 2 };\nfunc main(): Void { let s: E = E.Running; std::inout::println(\"v={s as Int}\"); }\n", "v=2\n"),
    ("enum_compare", U + "static E = Enum Int32 { Ready = 1, Running = 2 };\nfunc main(): Void { let s: E = E.Ready; if s == 1 { std::inout::println(\"one\"); } }\n", "one\n"),

    # unions
    ("union_basic", U + "static U = Union { i: Int32, r: Real64 };\nfunc main(): Void { let u: U; u.i = 7; std::inout::println(\"v={u.i}\"); }\n", "v=7\n"),

    # match statement and expression
    ("match_stmt", U + "func main(): Void { let x: Int = 2; match x { case 1 { std::inout::println(\"one\"); } case 2 { std::inout::println(\"two\"); } else { std::inout::println(\"other\"); } } }\n", "two\n"),
    ("match_else", U + "func main(): Void { let x: Int = 9; match x { case 1 { std::inout::println(\"one\"); } else { std::inout::println(\"other\"); } } }\n", "other\n"),
    ("match_expr", U + "func main(): Void { let x: Int = 1; let s: &Int8 = match x { case 1 { \"one\"; } else { \"other\"; } }; std::inout::println(s); }\n", "one\n"),
    ("match_expr_no_tail_semicolon", U + "func main(): Void { let x: Int = 1; let s: &Int8 = match x { case 1 { \"one\" } else { \"other\" } }; std::inout::println(s); }\n", "one\n"),

    # casts between numeric types
    ("cast_widen", U + "func main(): Void { let x: Int32 = 5; let y: Int64 = x as Int64; std::inout::println(\"v={y}\"); }\n", "v=5\n"),
    ("cast_narrow", U + "func main(): Void { let x: Int64 = 300; let y: Int32 = x as Int32; std::inout::println(\"v={y}\"); }\n", "v=300\n"),

    # Real arithmetic and comparisons, verified through non-zero values
    ("real_add", U + "func main(): Void { let a: Real = 1.5; let x: Real = a + 2.25; std::inout::println(\"v={x as Int}\"); }\n", "v=3\n"),
    ("real_sub", U + "let g: Real = 3.0;\nfunc main(): Void { let x: Real = g - 1.0; std::inout::println(\"v={x as Int}\"); }\n", "v=2\n"),
    ("real_mul", U + "let g: Real = 3.0;\nfunc main(): Void { let x: Real = g * 4.0; std::inout::println(\"v={x as Int}\"); }\n", "v=12\n"),
    ("real_div", U + "let g: Real = 3.0;\nfunc main(): Void { let x: Real = g / 2.0; std::inout::println(\"v={x as Int}\"); }\n", "v=1\n"),
    ("real_cmp_true", U + "let g: Real = 3.0;\nfunc main(): Void { if g > 2.0 and g < 4.0 and g != 2.5 and g >= 3.0 and g <= 3.0 { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "T\n"),
    ("real_cmp_false", U + "let g: Real = 3.0;\nfunc main(): Void { if g < 2.0 or g == 2.5 { std::inout::println(\"T\"); } else { std::inout::println(\"F\"); } }\n", "F\n"),
    ("real_neg", U + "func main(): Void { let x: Real = -2.5; if x < 0.0 { std::inout::println(\"neg\"); } }\n", "neg\n"),
    ("real_int_cast", U + "func main(): Void { let i: Int = 7; let r: Real = i as Real; std::inout::println(\"v={r as Int}\"); }\n", "v=7\n"),
    ("real_frac_neq", U + "func main(): Void { let x: Real = 0.1 + 0.2; if x != 0.3 { std::inout::println(\"neq\"); } }\n", "neq\n"),

    # global variables, including Real-typed globals
    ("global_int", U + "let g: Int = 42;\nfunc main(): Void { std::inout::println(\"g={g}\"); }\n", "g=42\n"),
    ("global_real", U + "let g: Real = 3.5;\nfunc main(): Void { std::inout::println(\"v={g as Int}\"); }\n", "v=3\n"),
    ("global_array", U + "let a: Int[3] = {1, 2, 3};\nfunc main(): Void { std::inout::println(\"v={a[0] + a[1] + a[2]}\"); }\n", "v=6\n"),

    # pass statement and empty blocks
    ("pass_stmt", U + "func main(): Void { pass; std::inout::println(\"ok\"); }\n", "ok\n"),
    ("empty_block", U + "func main(): Void { { } std::inout::println(\"ok\"); }\n", "ok\n"),

    # static: all declaration forms
    ("static_typed", U + "static N: Int = 5;\nfunc main(): Void { std::inout::println(\"n={N}\"); }\n", "n=5\n"),
    ("static_let", U + "static let M = 7;\nfunc main(): Void { std::inout::println(\"m={M}\"); }\n", "m=7\n"),
    ("static_let_typed", U + "static let M: Int32 = 7;\nfunc main(): Void { std::inout::println(\"m={M}\"); }\n", "m=7\n"),
    ("static_expr", U + "static BASE = 100;\nfunc main(): Void { std::inout::println(\"v={BASE + 23}\"); }\n", "v=123\n"),
    ("static_stmt", U + "func main(): Void { static K = 9; std::inout::println(\"k={K}\"); }\n", "k=9\n"),
    ("static_struct", U + "static Point = Struct { x: Int, y: Int };\nfunc main(): Void { let p: Point = Point {1, 2}; std::inout::println(\"v={p.x + p.y}\"); }\n", "v=3\n"),
    ("static_enum", U + "static E = Enum { A, B };\nfunc main(): Void { let e: E = E.B; std::inout::println(\"e={e as Int}\"); }\n", "e=1\n"),
    ("static_union", U + "static U = Union { i: Int32, r: Real64 };\nfunc main(): Void { let u: U; u.i = 7; std::inout::println(\"u={u.i}\"); }\n", "u=7\n"),
    ("static_func", U + "static Double = func(x: Int): Int { return x * 2; };\nfunc main(): Void { std::inout::println(\"d={Double(4)}\"); }\n", "d=8\n"),

    # catch: handler yields the operand type or diverges
    ("catch_diverge", U + "func f(a: Int, b: Int): Int! { return try a + b catch |e| { return 0; }; }\nfunc main(): Void! { std::inout::println(\"r={f(2, 3)}\"); }\n", "r=5\n"),
    ("errval_bind", U + "func g(): Int! { return 42; }\nfunc main(): Void! { let r: Int! = g(); std::inout::println(\"r={r}\"); }\n", "r=42\n"),

    # blocks are full statements; bodies are one statement
    ("if_single_body", U + "func main(): Void { let x: Int = 5; if x > 3 std::inout::println(\"big\"); else std::inout::println(\"small\"); }\n", "big\n"),
    ("else_if_chain", U + "func main(): Void { let x: Int = 2; if x > 3 { std::inout::println(\"a\"); } else if x > 1 { std::inout::println(\"b\"); } else { std::inout::println(\"c\"); } }\n", "b\n"),
    ("while_single_body", U + "func main(): Void { let i: Int = 0; while i < 3 i = i + 1; std::inout::println(\"i={i}\"); }\n", "i=3\n"),
    ("for_single_body", U + "func main(): Void { let s: Int = 0; for 0..4 |i| s = s + i; std::inout::println(\"s={s}\"); }\n", "s=6\n"),
    ("block_statement", U + "func main(): Void { { let q: Int = 7; std::inout::println(\"q={q}\"); } }\n", "q=7\n"),

    # views (slices): closed, exclusive, open-ended, indexing, iteration
    ("view_len", U + "func main(): Void { let a: Int[5] = {1,2,3,4,5}; let v: Int[..] = a[1..3]; std::inout::println(\"n={v.len}\"); }\n", "n=3\n"),
    ("view_elems", U + "func main(): Void { let a: Int[5] = {1,2,3,4,5}; let v: Int[..] = a[1..3]; std::inout::println(\"v={v[0] + v[1] + v[2]}\"); }\n", "v=9\n"),
    ("view_open_hi", U + "func main(): Void { let a: Int[5] = {1,2,3,4,5}; let v: Int[..] = a[2..]; std::inout::println(\"n={v.len}\"); }\n", "n=3\n"),
    ("view_open_lo", U + "func main(): Void { let a: Int[5] = {1,2,3,4,5}; let v: Int[..] = a[..3]; std::inout::println(\"n={v.len}\"); }\n", "n=4\n"),
    ("view_exclusive", U + "func main(): Void { let a: Int[5] = {1,2,3,4,5}; let v: Int[..] = a[1..<3]; std::inout::println(\"n={v.len}\"); }\n", "n=2\n"),
    ("view_for", U + "func main(): Void { let a: Int[5] = {1,2,3,4,5}; let v: Int[..] = a[1..3]; let s: Int = 0; for v |x| { s = s + x; } std::inout::println(\"s={s}\"); }\n", "s=9\n"),
    ("view_string", U + "func main(): Void { let s: &Int8 = \"hello\"; let v: Int8[..] = s[1..3]; let c: Int = v[0]; std::inout::println(\"c={c}\"); }\n", "c=101\n"),

    # implicit numeric conversion materialization: let, return, store
    ("let_int_to_real", U + "func main(): Void { let x: Real = 5; std::inout::println(\"v={x as Int}\"); }\n", "v=5\n"),
    ("return_int_to_real", U + "func f(): Real { return 5; }\nfunc main(): Void { std::inout::println(\"v={f() as Int}\"); }\n", "v=5\n"),
    ("store_int_to_real", U + "func main(): Void { let x: Real = 1.0; x = 5; std::inout::println(\"v={x as Int}\"); }\n", "v=5\n"),
    ("store_field_real", U + "static P = Struct { r: Real, i: Int };\nfunc main(): Void { let p: P = P {0.0, 0}; p.r = 3; std::inout::println(\"v={p.r as Int}\"); }\n", "v=3\n"),
    ("deref_store_real", U + "func main(): Void { let x: Real = 1.0; let p: &Real = &x; @p = 9; std::inout::println(\"v={x as Int}\"); }\n", "v=9\n"),
    ("real32_struct_array", U + "static Pair = Struct { a: Real32, b: Real32, c: Int };\nfunc main(): Void { let p: Pair = Pair {1.5, 2.5, 9}; p.b = 7.25; let a: Real32[3] = {1.5, 2.5, 3.5}; a[1] = 8.5; std::inout::println(\"v={p.a as Int},{p.b as Int},{p.c},{a[0] as Int},{a[1] as Int},{a[2] as Int}\"); }\n", "v=1,7,9,1,8,3\n"),
    ("real32_rounding", U + "func main(): Void { let x: Real32 = 1.00000005; std::inout::println(\"v={(x * 100000000.0) as Int}\"); }\n", "v=100000000\n"),
    ("catch_body_trailing_semicolon", U + "func add(a: Int, b: Int): Int { return a + b catch |e| { error \"{e}\"; }; }\nfunc main(): Void { std::inout::println(\"v={add(1, 2)}\"); }\n", "v=3\n"),

    # Adversarial expression forms: if-expressions in assignments,
    # nested conditional expressions, tuple type annotations and
    # typed destructuring from an existing tuple value.
    ("if_expr_assignment_true", U + "func main(): Void { let flag: Bool = true; let value: Int = if flag { 17; } else { 29; }; std::inout::println(\"v={value}\"); }\n", "v=17\n"),
    ("if_expr_assignment_false", U + "func main(): Void { let flag: Bool = false; let value: Int = if flag { 17; } else { 29; }; std::inout::println(\"v={value}\"); }\n", "v=29\n"),
    ("if_expr_assignment_no_tail_semicolon", U + "func main(): Void { let value: Int = if true { 17 } else { 29 }; std::inout::println(\"v={value}\"); }\n", "v=17\n"),
    ("if_expr_nested", U + "func main(): Void { let a: Bool = true; let b: Bool = false; let v: Int = if a { if b { 1; } else { 2; } } else { 3; }; std::inout::println(\"v={v}\"); }\n", "v=2\n"),
    ("tuple_typed_value_destructure", U + "func main(): Void { let n: (Int, Int) = {1, 2}; let (a, b): Int = n; std::inout::println(\"v={a},{b}\"); }\n", "v=1,2\n"),
    ("tuple_typed_destructure_literal", U + "func main(): Void { let (a, b): Int = {7, 11}; std::inout::println(\"v={a + b}\"); }\n", "v=18\n"),
    ("catch_expression_value", U + "func value(): Int! { return 12; }\nfunc main(): Void { let result: Int = value() catch |e| { 99; }; std::inout::println(\"v={result}\"); }\n", "v=12\n"),
    ("catch_runtime_error_fallback", U + "func fail(): Int! { return error 5; }\nfunc main(): Void { let value: Int = fail() catch |code| { std::inout::println(\"error={code}\"); 7; }; std::inout::println(\"value={value}\"); }\n", "error=5\nvalue=7\n"),
    ("catch_runtime_error_binding", U + "func fail(): Int! { return error 5; }\nfunc main(): Void { let value: Int = fail() catch |code| { code; }; std::inout::println(\"value={value}\"); }\n", "value=5\n"),
    ("catch_nested_expression", U + "func value(): Int! { return 8; }\nfunc main(): Void { let n: Int = (value() catch |e| { 3; }) + 2; std::inout::println(\"v={n}\"); }\n", "v=10\n"),
    ("if_expr_branch_side_effects", U + "func main(): Void { let n: Int = if true { std::inout::println(\"then\"); 4; } else { std::inout::println(\"else\"); 9; }; std::inout::println(\"v={n}\"); }\n", "then\nv=4\n"),

    # inline assembly statement
    ("inline_asm", U + "func main(): Void { asm \"nop\"; std::inout::println(\"ok\"); }\n", "ok\n"),
]

# (name, source, fragment that the diagnostic must contain)
NEGATIVE = [
    ("plus_on_strings", U + "func main(): Void { let s = \"a\" + \"b\"; std::inout::println(s); }\n", "not defined"),
    ("plus_on_arrays", U + "func main(): Void { let a: Int[2] = {1, 2}; let b: Int[2] = {3, 4}; let c = a + b; std::inout::println(\"x\"); }\n", "not defined"),
    ("break_outside_loop", U + "func main(): Void { break; }\n", "outside of a loop"),
    ("continue_outside_loop", U + "func main(): Void { continue; }\n", "outside of a loop"),
    ("unknown_label", U + "func main(): Void { while true { break nowhere; } }\n", "unknown label"),
    ("undeclared_var", U + "func main(): Void { std::inout::println(\"v={nope}\"); }\n", "unknown variable or function"),
    ("type_mismatch", U + "func main(): Void { let x: Int = \"hello\"; std::inout::println(\"x\"); }\n", "type"),
    ("open_range_lower", U + "func main(): Void { let x = ..10; }\n", "expected"),
    ("open_range_upper", U + "func main(): Void { let x = 1..; }\n", "range requires an ending value"),
    ("for_open_range", U + "func main(): Void { for 3.. |i| { pass; } }\n", "ending value"),
    ("tuple_annotation_wrong_arity", U + "func main(): Void { let n: (Int, Int) = {1, 2, 3}; }\n", "tuple literal element count"),
    ("tuple_annotation_wrong_element_type", U + "func main(): Void { let n: (Int, Bool) = {1, 2}; }\n", "tuple element is not assignable"),
    ("return_requires_semicolon", "func main(): Void { return }\n", "';' after return"),
    ("return_value_requires_semicolon", "func value(): Int { return 42 }\nfunc main(): Void { pass; }\n", "';' after return"),
    ("catch_binding_outside_scope", U + "func fail(): Int! { return error 5; }\nfunc main(): Void { let value: Int = fail() catch |code| { 7; }; std::inout::println(\"code={code}\"); }\n", "unknown variable or function"),
]


class LanguageConformanceTests(ProjectCase):
    def setUp(self):
        super().setUp()
        self.proj = self.init_demo()

    def _build_and_run(self, src, no_opt=False):
        (self.proj / "src/main.px").write_text(src)
        env = dict(os.environ)
        if no_opt:
            env["PAXSY_NO_OPT"] = "1"
        r = run_paxsy("build", cwd=self.proj, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = run_paxsy("run", cwd=self.proj, env=env)
        return r

    @staticmethod
    def _make(name, src, expected):
        def test(self):
            for no_opt in (False, True):
                r = self._build_and_run(src, no_opt)
                self.assertEqual(r.returncode, 0, f"PAXSY_NO_OPT={no_opt}: {r.stderr}{r.stdout}")
                self.assertEqual(r.stdout, expected,
                                 f"PAXSY_NO_OPT={no_opt}")
        return test

    @staticmethod
    def _make_negative(name, src, fragment):
        def test(self):
            (self.proj / "src/main.px").write_text(src)
            r = run_paxsy("build", cwd=self.proj)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn(fragment, r.stderr + r.stdout)
        return test


# Large lexical scope: exercises the lowering scope hash index, including
# growth past several table capacities and shadowing in a nested scope.
_many_scope_decls = "\n".join(
    f"let local_{i}: Int32 = {i + 1};" for i in range(64)
)
_many_scope_sum = " + ".join(f"local_{i}" for i in range(64))
POSITIVE.append((
    "large_local_scope_hash",
    U + "func main(): Void {\n" + _many_scope_decls +
    "\nfor 0..1 |scope_iteration| { let nested: Int32 = scope_iteration; }\n" +
    f"std::inout::println(\"sum={{{_many_scope_sum}}}\");\n" +
    "}\n",
    "sum=2080\n",
))

for _name, _src, _expected in POSITIVE:
    setattr(LanguageConformanceTests, "test_pos_" + _name,
            LanguageConformanceTests._make(_name, _src, _expected))
for _name, _src, _fragment in NEGATIVE:
    setattr(LanguageConformanceTests, "test_neg_" + _name,
            LanguageConformanceTests._make_negative(_name, _src, _fragment))

if __name__ == "__main__":
    unittest.main()
