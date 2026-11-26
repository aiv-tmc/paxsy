<div align="center">

<img src="logo.png" alt="Paxsy" width="180" />

# Paxsy

### A systems language. A compiler you can inspect. A toolchain you can build on.

[![CI](https://github.com/aiv-tmc/paxsy/actions/workflows/ci.yml/badge.svg)](https://github.com/aiv-tmc/paxsy/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)
[![Language: C99](https://img.shields.io/badge/Compiler-C99-informational.svg)](src/)

[Documentation](docs/ToolchainUserManual.md) · [Language specification](docs/LanguageSpecification.md) · [Contributing](docs/LanguageDeveloperGuide.md) · [LLM guide](docs/LLMIntegrationGuide.md) · [Website](https://paxsy.org)

</div>

---

Paxsy is a statically typed systems programming language with a native compiler and an integrated developer toolchain. It is designed around a practical idea: **make program structure explicit, make compilation observable, and keep the path from source code to machine code understandable.**

The repository contains the compiler, project and package commands, a standard library, native code generation, optimisation passes, debugging and inspection tools, and editor support. The compiler itself is written in C99, so contributors can follow the implementation from lexer to backend without first learning a large compiler framework.

> [!IMPORTANT]
> Paxsy 1.0.0 is the stable baseline release. The documentation set is frozen for this release: the [language specification](docs/LanguageSpecification.md), [toolchain manual](docs/ToolchainUserManual.md), and contributor references describe the accepted 1.0.0 contract. The implementation status and tests identify backend and feature boundaries; do not assume every specified construct is implemented by every backend.

## Why Paxsy?

- **Systems-level control** — explicit types, pointers, allocation, aggregate layout, and native code generation.
- **A coherent toolchain** — one `paxsy` executable provides project creation, build, check, package management, formatting, linting, debugging, disassembly, and recovery workflows.
- **An optimisation pipeline you can inspect** — HIR passes, CFG/SSA analysis, profiling, reproducible stress benchmarks, and regression tests live in the same repository.
- **A language designed for real projects** — modules, generic specialisation, error-carrying types, `defer`, aggregate types, and concurrency constructs are part of the language design.
- **Hackable compiler architecture** — a compact C99 implementation with distinct lexer, parser, semantic analysis, HIR, optimisation, and backend layers.
- **Open collaboration** — Apache License 2.0, developer documentation, and a dedicated guide for AI-assisted language work.

## Get started

### Build Paxsy from source

Requirements: a C99 compiler, GNU Make, and a POSIX-like build environment.

```sh
git clone https://github.com/aiv-tmc/paxsy.git
cd paxsy
make -j"$(nproc)"
make test
```

The compiler is produced as `./paxsy`. Install it with:

```sh
sudo make install
```

For sanitizer and stress checks, see the [Toolchain User Manual](docs/ToolchainUserManual.md#installation-and-building-the-compiler).

### Create and run a project

```sh
paxsy new hello
cd hello
paxsy check
paxsy build
paxsy run
```

`paxsy new` creates the project and starter source. There is no `init` command. The generated project uses a TOML manifest and `.px` source files.

### A small Paxsy example

```paxsy
use std::inout;

static GREETING = "Hello from Paxsy";

func twice(value: Int32): Int32 {
    return value * 2;
}

func main(): Void! {
    let answer: Int32 = twice(21);
    try std::inout::println("{GREETING}: {answer}");
}
```

The example illustrates the language's surface style; consult the specification and tests for exact feature and backend coverage.

## Toolchain at a glance

| Command | Purpose |
|---|---|
| `paxsy new` | Create a project scaffold |
| `paxsy check` | Parse, resolve, and type-check without producing an executable |
| `paxsy build` | Compile and link the configured target |
| `paxsy run` | Run the configured native, QEMU, Docker, or custom runtime profile |
| `paxsy test` | Discover and validate test sources/functions |
| `paxsy bench` | Measure source lexer throughput |
| `paxsy fmt` | Format Paxsy sources |
| `paxsy clippy` | Run code-quality checks |
| `paxsy debug` | Start the integrated debugger |
| `paxsy disasm` | Inspect generated machine code |
| `paxsy fetch/search/install/add` | Work with packages |

> [!NOTE]
> `paxsy test` currently compiles test sources and discovers test functions; it does not yet execute discovered test functions. `paxsy bench` currently measures lexer throughput. The full compiler stress benchmark is run separately with `make bench-full`.

## Optimisation and performance work

Paxsy exposes four HIR optimisation levels: `0` disables optimisation, `1` enables inexpensive local passes, `2` adds CFG/SSA and global value numbering, and `3` enables the full pipeline including scalar replacement. New projects use level `3` by default. The optimiser includes constant folding, propagation, global value numbering, strength reduction, dead-store elimination, dead-code elimination, CFG simplification, conservative scalar replacement, and SSA-oriented analyses. The project also contains a multi-source full-language benchmark that compares the maximum optimisation pipeline with an internal no-optimisation reference and verifies output equivalence.

```sh
# Per-stage and per-pass compiler timings
paxsy build --profile

# Select a configured runtime environment (native, qemu, docker, ...)
paxsy run --profile qemu

# Full compiler / generated-program stress corpus
make bench-full

# Regression and undefined-behaviour checks
make test
make test-ubsan
```

Performance claims should be backed by a repeatable benchmark run on the target machine. The benchmark harness reports build time, runtime, and executable size; it does not turn one machine's results into universal claims.

## Compile-time features: current boundary

Paxsy uses `static` for compile-time declarations and type-level facilities. `comptime` is no longer a keyword and can be used as an ordinary identifier. The current implementation supports static type aliases, compile-time type parameters and monomorphisation, and substitution/folding of eligible scalar constant expressions. Unused scalar constant storage is removed; address-taken constants retain storage. Expansion is bounded and conservative: it avoids runtime effects, diagnoses cycles, and leaves unsupported expressions intact.

> [!WARNING]
> `static` functions and `static` blocks are present in parts of the language design/parser but are not a general-purpose compile-time interpreter yet. `comptime` is an ordinary identifier, not a compile-time modifier. See [Compile-time evaluation status](docs/LanguageDeveloperGuide.md#compile-time-evaluation-status) before relying on compile-time execution in a library API.

## Documentation

| Document | Audience | What it covers |
|---|---|---|
| [Toolchain User Manual](docs/ToolchainUserManual.md) | Paxsy users | Install, projects, commands, configuration, packages, builds, debugging |
| [Language Specification](docs/LanguageSpecification.md) | Language users | Syntax, types, expressions, control flow, memory, modules, semantics |
| [Language Developer Guide](docs/LanguageDeveloperGuide.md) | Compiler and language contributors | Architecture, invariants, pass ordering, tests, extension workflow, implementation status |
| [LLM Integration Guide](docs/LLMIntegrationGuide.md) | LLM users and tool builders | Grounded prompting, context files, feature verification, code-generation workflow |
| [Standard Library Reference](docs/StandardLibraryReference.md) | Library users | Public modules, APIs, and conventions |
| [Code Style Guide](docs/CodeStyleGuide.md) | Contributors | Source formatting and conventions |
| [Benchmark guide](benches/README.md) | Performance contributors | Benchmark corpus and measurement methodology |

## Editor support

- **Visual Studio Code:** `editors/vscode` contains the language extension source and syntax configuration. Package it with `editors/vscode/package.sh` after installing `@vscode/vsce`.
- **Vim:** syntax highlighting and file detection are in `editors/vim`.

## Contributing

Contributions are welcome, especially focused changes accompanied by tests and documentation. Before changing syntax or semantics, read the [Language Developer Guide](docs/LanguageDeveloperGuide.md). For language proposals, include examples, expected diagnostics, edge cases, and a note about backend implications.

A useful contribution is not merely a parser acceptance test: it should demonstrate the feature through semantic analysis, HIR lowering, code generation, and observable runtime behaviour wherever those stages apply.

## Project status

Paxsy 1.0.0 is the frozen language/compiler baseline. The native compiler's primary target is x86-64; output formats and language-feature coverage vary by backend. The compiler's own tests and the frozen manuals are the authoritative record of current behaviour. Post-release work is intentionally limited to `debug` optimisation and the separately tracked future disassembly/source-recovery programme; see [documentation freeze policy](docs/README.md) and [post-release tooling issue](issues/POST_RELEASE_DEBUG_DISASM_RECOVERY.md).

## License

Paxsy is distributed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for attribution information.
