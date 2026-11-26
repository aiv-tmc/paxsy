"""Deterministic grammar-based fuzz generator for the Paxsy suite.

Generates well-formed programs (weighted grammar over the language
constructs) and mutated near-valid programs. Randomness comes from a
seeded PRNG, so a failing seed reproduces exactly.
"""

import random

INT_TYPES = ["Int", "Int32", "Int64", "UInt32"]
BINOPS = ["+", "-", "*", "/", "%", "&", "$", "^", "<<", ">>"]
CMPOPS = ["<", "<=", ">", ">=", "==", "!="]
LITS = ["0", "1", "2", "3", "5", "7", "10", "42", "127"]


class Gen:
    def __init__(self, rng):
        self.r = rng
        self.nfun = 0
        self.vars = []

    def expr(self, depth=0):
        r = self.r.random()
        if depth > 3 or r < 0.25:
            if self.vars and self.r.random() < 0.5:
                return self.r.choice(self.vars)
            return self.r.choice(LITS)
        if r < 0.4:
            return "(%s %s %s)" % (self.expr(depth + 1),
                                   self.r.choice(BINOPS), self.expr(depth + 1))
        if r < 0.5:
            return "(%s %s %s)" % (self.expr(depth + 1),
                                   self.r.choice(CMPOPS), self.expr(depth + 1))
        if r < 0.6:
            return "(%s and %s)" % (self.expr(depth + 1), self.expr(depth + 1))
        if r < 0.7:
            return "(not %s)" % self.expr(depth + 1)
        if r < 0.8:
            return "%s(%s)" % (self.r.choice(self.calls),
                               self.expr(depth + 1))
        return self.r.choice(LITS)

    def stmt(self, depth=0):
        r = self.r.random()
        if depth > 2 or r < 0.2:
            v = "v%d" % len(self.vars)
            self.vars.append(v)
            return "let %s: Int = %s;" % (v, self.expr())
        if r < 0.35:
            return "if %s { %s } else { %s }" % (
                self.expr(), self.stmt(depth + 1), self.stmt(depth + 1))
        if r < 0.5:
            return "for 0..%d |i| { let t: Int = %s; }" % (
                self.r.randint(1, 5), self.expr(depth + 1))
        if r < 0.65 and self.vars:
            return "%s = %s;" % (self.r.choice(self.vars), self.expr())
        if r < 0.75:
            return "while false { pass; }"
        if r < 0.85:
            return "match %s { case 1 { pass; } else { pass; } }" % self.expr()
        if r < 0.92:
            return "let arr%d: Int[%d] = {%s};" % (
                len(self.vars), self.r.randint(1, 4),
                ",".join(self.r.choice(LITS) for _ in range(self.r.randint(1, 4))))
        return "pass;"

    def function(self):
        self.nfun += 1
        name = "f%d" % self.nfun
        saved_calls = self.calls
        self.calls = saved_calls + [name]
        self.vars = []
        body = " ".join(self.stmt() for _ in range(self.r.randint(1, 4)))
        ret = "return %s;" % self.expr()
        fn = ("func %s(a: Int, b: Int): Int { let r0: Int = a + b; %s %s }"
              % (name, body, ret))
        self.calls = saved_calls
        return name, fn

    def program(self):
        self.calls = ["fib"]
        parts = [
            "use std::inout;",
            "func fib(n: Int): Int { if n < 2 { return n; } "
            "return fib(n-1) + fib(n-2); }",
        ]
        nfun = self.r.randint(1, 3)
        calls = []
        for _ in range(nfun):
            name, fn = self.function()
            parts.append(fn)
            calls.append(name)
        self.vars = []
        main_body = " ".join(self.stmt() for _ in range(self.r.randint(2, 5)))
        call_lines = " ".join("let c%d: Int = %s(%s, %s);" % (
            i, c, self.expr(), self.expr()) for i, c in enumerate(calls))
        parts.append("func main(): Void { %s %s "
                     "std::inout::println(\"done\"); }" % (main_body, call_lines))
        return "\n".join(parts) + "\n"


def mutate(src, rng):
    """Random byte-level mutation: truncation, splicing, token spam."""
    ops = rng.randint(1, 4)
    b = bytearray(src.encode())
    for _ in range(ops):
        if not b:
            break
        op = rng.random()
        if op < 0.3:
            del b[rng.randrange(len(b)):]
        elif op < 0.6:
            i = rng.randrange(len(b))
            b[i:i] = bytes([rng.randint(32, 126)])
        elif op < 0.8:
            i = rng.randrange(len(b))
            b[i] = rng.randint(32, 126)
        else:
            i = rng.randrange(len(b))
            del b[i]
    return b.decode(errors="ignore")


def gen_program(seed):
    return Gen(random.Random(seed)).program()


def gen_mutation(seed):
    return mutate(gen_program(seed), random.Random(seed ^ 0x5DEECE66D))
