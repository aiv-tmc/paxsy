# Paxsy LLM Integration Guide

This guide is for developers using language models to write Paxsy code, build editor/agent integrations, generate examples, or help maintain the compiler. Its central rule is simple: **ground every generated program in the repository's current grammar and tested behaviour.**

## 1. Canonical sources of truth

When sources disagree, use this order:

1. Compiler implementation and regression tests for what the current version accepts and executes.
2. [Language Specification](../language/language-specification.md) for syntax and semantics.
3. [Toolchain User Manual](../language/toolchain-user-manual.md) for command-line and project behaviour.
4. [Standard Library Reference](../language/standard-library-reference.md) for public APIs.
5. This guide for LLM workflow and prompt structure.

The specification defines syntax and semantics; compiler tests define executable behaviour.

## 2. Minimum context pack for a coding model

For a small source-generation task, provide:

- the relevant section of `../language/language-specification.md`;
- the exact standard-library API declaration or reference section being used;
- the project's `.toml` manifest and relevant `.px` files;
- the compiler version (`paxsy --version`);
- the output of `paxsy check` or the exact compiler diagnostic.

For compiler work, also provide:

- `docs/language-specification.md`;
- the relevant parser/sema/HIR/backend files;
- the closest regression tests;
- a minimal reproducer and expected output.

Do not send only a prose description when exact source declarations or diagnostics are available. LLMs are prone to inventing APIs that sound plausible.

## 3. Feature-grounded generation workflow

Use this sequence for generated Paxsy code:

1. **Identify the construct.** Determine which language feature and standard-library module are required.
2. **Verify support.** Check compiler tests and locate a test or implementation path demonstrating the feature end to end.
3. **Generate the smallest program.** Avoid introducing extra syntax or libraries not required by the task.
4. **Run `paxsy check`.** Treat compiler diagnostics as authoritative evidence; do not “fix” them by inventing syntax.
5. **Run `paxsy build` and execute the result.** Type-checking alone does not prove backend support or runtime correctness.
6. **Add a regression test.** Preserve the accepted syntax and expected behaviour in a minimal project.

A good agent should distinguish three states in its output:

- **Verified:** built and executed successfully with the specified compiler.
- **Type-checked:** accepted by `paxsy check`.
- **Source-matched:** based on design/specification and consistent with compiler tests.

## 4. Paxsy-specific facts an LLM must not guess

- Source files use `.px` and project manifests use TOML.
- Create projects with `paxsy new`; there is no `init` command.
- The compile-time declaration modifier is `static`; `comptime` is an ordinary identifier.
- `static` type aliases and compile-time type parameters are supported. Static scalar constant expansion is conservative and is not a general compile-time interpreter.
- `static Name = func(...) { ... };` is an ordinary runtime function binding, not CTFE; the separate static-function declaration and static blocks are not interpreted.
- The native backend's primary target is x86-64. Other output modes do not imply identical feature coverage.
- `paxsy run` runs the existing build output; run `paxsy build` first. The default `[run].profile` is `native`; custom `[run.<name>]` profiles can launch tools such as QEMU or Docker with an argument array, environment entries, and `{binary}` / `{root}` / `{project}` substitutions. Select one with `paxsy run --profile <name>`. Arguments after `--` are forwarded; `{args}` splices them into the configured argument list. No shell command string is evaluated.
- `paxsy test` compiles test sources and discovers test functions; it does not execute discovered test functions.
- `paxsy bench` measures lexer throughput. Use `make bench-full` for the full compiler/program benchmark.
- Generic parameters marked `static` are compile-time type parameters in the supported monomorphisation path; do not assume arbitrary value-level const generics.

## 5. Suggested system prompt for a Paxsy coding agent

Copy and adapt this block when configuring an agent:

```text
You are generating or reviewing Paxsy code for the compiler version supplied
by the user. Treat the supplied repository and tests as authoritative.

Before using a feature, verify it in the current Language Specification and
compiler tests and the specification. Do not invent standard-library functions, keywords,
operators, attributes, compiler flags, or manifest keys. In particular,
`static` is the compile-time declaration modifier; `comptime` is an ordinary identifier. Static function literals are runtime functions while static blocks are not interpreted.

Use `.px` source files and the supplied TOML project manifest. Prefer the
smallest complete example. Explain any assumptions. Separate verified claims
from specification-only material. When possible, run `paxsy check`, `paxsy build`, and the
resulting executable; report exact commands and diagnostics. Never claim a
program was compiled or tested unless the command actually ran successfully.

When modifying the compiler, follow docs/language-specification.md. A parser
change is not complete without semantic analysis, HIR/backend support,
regression tests, and documentation. Optimisations must preserve observable
semantics and fail conservatively when proof or resources are unavailable.
```

## 6. Structured context for retrieval-augmented generation

For a language server or RAG index, chunk the corpus by semantic unit rather than arbitrary token count:

| Corpus | Recommended chunk key | Metadata |
|---|---|---|
| Language specification | Heading path + code example | `feature`, `syntax`, `status` |
| Standard library | Module + declaration | `module`, `symbol`, `signature` |
| Toolchain manual | Command/configuration subsection | `command`, `manifest_key`, `platform` |
| Compiler source | File + function | `phase`, `symbol`, `calls` |
| Regression tests | Test method | `feature`, `expected_result`, `backend` |

Keep source paths, heading paths, symbol names, and test names in the retrieved context. A chunk that contains a function signature without its module or receiver context is likely to produce incorrect code.

### Suggested retrieval strategy

1. Search exact symbol names first.
2. Retrieve the full declaration and its module context.
3. Retrieve at least one positive test and one negative test.
4. Retrieve the implementation note that matches the feature.
5. Generate code only after those sources agree.

## 7. Compiler-agent workflow

When an LLM is asked to implement a language feature:

1. Ask it to map the feature through lexer, parser, semantic analysis, HIR, type inference, optimiser, and backend.
2. Require it to list the documented edge cases before coding.
3. Require a minimal failing test before implementation where feasible.
4. Make it implement one phase at a time and compile after each structural change.
5. Require `make test` and the relevant sanitizers for the change.
6. Require documentation updates for the affected behaviour.

For optimiser work, insist on a semantic counterexample test, not only a test that the pass reports a rewrite. For CTFE work, require limits for recursion, evaluation steps, and memory before exposing user-controlled compile-time execution.

## 8. Output contract for generated examples

A useful LLM answer about Paxsy should include:

- the `.px` code;
- any required `use` declarations;
- the expected project layout or manifest additions;
- the exact commands to check, build, and run;
- the expected output;
- the compiler version and verification state;
- assumptions outside the documented rules.

Do not label pseudocode as compilable Paxsy. If a requested form is outside the documented language, say so and offer the closest documented form.


## Optimisation configuration

`build.optimization` accepts `0` (off), `1` (basic local/tree passes), `2` (advanced CFG/SSA, GVN and dead-store elimination), or `3` (maximum, including SROA). New projects default to `3`.
