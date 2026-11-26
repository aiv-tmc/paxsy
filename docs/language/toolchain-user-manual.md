# Paxsy Toolchain User Manual

## 1. Purpose and scope

Paxsy is a systems-programming language and a single-executable toolchain. The `paxsy` program provides the compiler driver, project commands, package-management commands, a native linker, source formatter, quality checker, debugger, disassembler, and recovery utilities. The standard library is distributed as Paxsy source and is compiled as part of a project build.

This manual describes the behaviour implemented by the current toolchain. In particular, `paxsy test` performs test-source compilation and test-function discovery; it does **not** execute discovered test functions. Likewise, `paxsy bench` measures lexer throughput, not runtime performance of compiled programs. These distinctions are intentional and important when using the commands in automation.

The compiler's normal native code-generation target is x86-64. It emits Linux ELF executables and can emit Windows PE32+ images. Other output modes include relocatable objects, static archives, LLVM-like textual output, MLIR, and CUDA source. The availability of an output mode does not imply that every backend has identical language-feature coverage.

## 2. Installation and building the compiler

### 2.1 Build from a source checkout

The repository uses a Makefile and a C99 compiler. Parallel builds are supported (`make -j$(nproc)`); generated per-source dependency files make incremental rebuilds selective, so changing a header recompiles only the translation units that include it:

```sh
make
make test
```

The resulting executable is `./paxsy`. `make test` builds an isolated test copy and runs the repository's regression suites. Additional developer checks are available:

```sh
make test-asan
make test-ubsan
make test-fuzz
```

`make clean` removes the compiler executable and its object directory (`bin/`). It does not remove source files or project data.

### 2.2 Install a release

The repository's `install.sh` downloads a release archive from `https://paxsy.org/releases/`, builds it, installs the `paxsy` executable, installs the standard library when it is included in the release tree, and optionally installs Vim syntax support.

```sh
curl -fsSL https://paxsy.org/install.sh | sh
```

The installer supports these environment variables:

| Variable | Meaning | Default |
|---|---|---|
| `INSTALL_PATH` | Directory receiving the `paxsy` executable | `/usr/local/bin` |
| `VERSION` | Release version to download; selects `paxsy-<VERSION>.tar.gz` | latest release archive |
| `NO_VIM` | Set to `1` to skip Vim support files | `0` |
| `VIMFILES` | Vim runtime directory | `/usr/share/vim/vimfiles` |

The installer may use `sudo` when the destination is not writable. It requires a C build toolchain, `tar`, and either `curl` or `wget`.

### 2.3 Verify the installation

```sh
paxsy --version
paxsy --help
paxsy --license
```

`--version` prints the toolchain version and release state. `--help` prints the command list. `--license` identifies the Apache License, Version 2.0 and links to its canonical text; the source distribution includes the complete `LICENSE` and `NOTICE` files.

## 3. Project layout and first build

### 3.1 Create a project

```sh
paxsy new hello_world
cd hello_world
```

Project names must use lowercase letters, digits, and underscores. The command creates a project directory containing:

```text
hello_world/
├── .gitignore
├── hello_world.toml
└── src/
 └── main.px
```

The starter source imports `std::inout` and defines `main`. Git initialisation is attempted when enabled and available. Use `--no-git` to disable Git integration for the project-creation command. Ordinary top-level declarations are private to their source file during semantic analysis; declarations placed inside a named `module` are project-wide and can be imported from another file with `use`.

### 3.2 Build, check, and run

```sh
paxsy check
paxsy build
paxsy run
```

- `check` parses and type-checks the project but does not emit a program.
- `build` runs the full frontend, HIR optimisation, and selected code-generation pipeline.
- `run` executes the already-built program. It does not implicitly build it; run `paxsy build` first.

A path may be supplied after the command. If omitted, the current directory is used:

```sh
paxsy build path/to/project
paxsy check path/to/project
paxsy run path/to/project -- --input data.bin
```

For `run`, arguments after the literal `--` are forwarded to the selected runtime without being interpreted by Paxsy. The process exit status is returned by `paxsy run`.

### Configurable run profiles

Projects can define a default runtime and named execution profiles in their TOML file. The default `native` profile executes the built program directly. A named profile uses `[run.<name>]` and starts its `command` with an argument vector; no shell is involved.

```toml
[run]
profile = "qemu"

[run.qemu]
command = "qemu-system-x86_64"
args = ["-m", "2048", "-cdrom", "{binary}", "-boot", "d"]

[run.docker]
command = "docker"
args = ["run", "--rm", "-v", "{root}:/workspace", "-w", "/workspace", "my-image:latest"]
working_dir = "{root}"
env = ["PAXSY_PROJECT={project}", "PAXSY_MODE=development"]
```

Select a profile with `paxsy run --profile docker -- --help`. The project default can be overridden per invocation. This is the same `--profile` spelling used by `paxsy build --profile` for compiler timings; the meaning is selected by the command. Profile fields:

- `command` (required): executable name or path, resolved using `PATH` when it has no slash.
- `args` (optional): array of individual arguments. `{binary}`, `{root}`, and `{project}` expand to the build output path, project directory, and project name. `{args}` splices through arguments after `--`; if omitted, forwarded arguments are appended.
- `working_dir` (optional): child working directory; defaults to the project root and supports the same substitutions.
- `env` (optional): array of `KEY=VALUE` entries applied to the child process; values support the same substitutions.

The runtime profile does not build the project: run `paxsy build` first. External runtimes such as QEMU and Docker must be installed separately. The `native` profile retains the original direct-execution behaviour.

A successful `build` or `check` is normally quiet. Use `--verbose` for additional progress output. `--profile` on `build` reports wall-clock time for source loading, lexing, parsing, project-wide semantic analysis, lowering, HIR typing, optimisation, and code generation. Frontend timings are summed across source files; the later whole-project stages are reported once. The profile also breaks HIR optimisation down by SROA, CFG/SSA, propagation, GVN, constant folding, strength reduction, dead-store elimination, DCE, and simplification. Each pass reports CPU time, invocation count, and number of HIR changes; these pass times are nested inside the overall optimisation time and must not be added to the stage total. Profiling is opt-in and does not change the generated program. `--quiet` suppresses optional progress messages. The build driver uses a content-based freshness stamp incorporating relevant compiler, configuration, source, target, and optimisation inputs; a fresh build may return without recompiling. `--profile` forces compilation so the measurements are not hidden by a cache hit. Requests for textual backend output bypass the ordinary freshness shortcut.

### 3.3 Reproducible full-language benchmark

The repository includes one multi-source stress project at `benches/full_language/`. It is intentionally a single stable corpus rather than a collection of unrelated microbenchmarks. It combines module imports and generic specialisation with structs, enums, unions, tuples, arrays, slices, pointers, numeric casts, floating-point expressions, error handling, `defer`, inline assembly, `match`, `while`, `do/while`, range and collection iteration, and a large generated family of arithmetic/control-flow functions. The repeated functions exercise expression reuse, branch merges, loop-carried values, dead stores, and dead pure expressions.

Run it with:

```sh
make bench-full
# or, with a chosen compiler and iteration count:
PAXSY_CMD=./paxsy python3 benches/bench_full_language.py --iterations 7
```

The harness builds the same source tree in two isolated temporary copies: an internal no-optimisation reference (used only to validate semantics) and the compiler's maximum optimisation level (`3`). It reports median build wall time, stage/pass timings, direct executable runtime, and executable size. Both binaries must produce identical output or the benchmark fails. The default is five measured builds and five runtime samples per mode; one warm-up build is excluded. Set `--keep` to retain temporary projects for inspection.

Compile-time evaluation supports static type aliases, static type parameters, and scalar static constant expressions after project-wide HIR typing. Static function declarations and static blocks are not executed as compile-time programs (a `static` function-literal binding is an ordinary runtime function). `static` is the only compile-time keyword; `comptime` is an ordinary identifier. See Language Specification for the exact boundary.


### 3.4 Source discovery

Project source files use the `.px` extension and are collected from `src/` in stable path order. A new project contains `src/mod.px` as its conventional module entry point: place shared `module` declarations there and keep the executable `main` in `src/main.px`. The entry point is a normal Paxsy source file, not a separate manifest language. By default, source discovery is non-recursive. Set `[source].recursive = true` to include nested directories. The older `[build].recursive` key is still accepted as a fallback when `[source].recursive` is absent.

Additional source trees can be attached with one `[include.<name>]` table per dependency. The `path` is relative to the project root (or may be absolute), and may name either a `.px` file or a directory. Directories are traversed recursively. A missing path is an error unless `optional = true` is set. Included sources participate in the same semantic analysis and standard-library dependency scan as project sources:

```toml
[include.geometry]
path = "vendor/geometry/src"

[include.platform]
path = "vendor/platform/src"
optional = true
```

The table name is a descriptive dependency key; module identity still comes from `module` declarations in the included `.px` files. Include paths are added consistently to `build`, `check`, `run`, `clippy`, and other project commands that share source discovery.

The project root is the directory containing the project TOML file. The configuration filename is conventionally `<project>.toml`, where `<project>` matches `[project].name`.

Numeric iteration ranges require both endpoints: `for 0..10 |i| { ... }`, `for 0..<10 |i| { ... }`, or `for 0..=10 |i| { ... }`. Open-ended ranges such as `..10` and `3..` are rejected. This restriction does not remove open-ended array slices such as `items[2..]` or `items[..3]`.
The native backend represents `Real32` memory cells as IEEE-754 binary32 in structures, arrays, and addressed memory, converting to its binary64 register representation for arithmetic. Stores and explicit conversions preserve the binary32 rounding point. `Real` and `Real64` remain binary64.

`routine { ... }` and statement-form `async { ... }` lower to the same HIR scheduling operation.

## 4. Project configuration

Paxsy reads a TOML file from the project root. `paxsy new` creates the `[project]` section; other settings can be added as needed. The configuration is the persistent place for build and tool policies. Command-line arguments select a command and its path; the supported global CLI switches are intentionally small.

### 4.1 Project identity

```toml
[project]
name = "hello_world"
version = "0.1.0"
description = "A Paxsy application"
authors = ["Example Author"]
```

`name` determines the default output basename. `version`, `description`, and `authors` are project metadata used by project information and package workflows. If `name` is absent, the configuration filename is used as a fallback in relevant commands.

### 4.2 Build settings

```toml
[build]
target = "elf"
optimization = 3
output = "build/hello_world"

[source]
recursive = false
```

| Key | Meaning | Supported values / behaviour |
|---|---|---|
| `build.target` | Select the native target or request a backend artifact | `elf`, `native`, `exe`, `linux-x86_64`, `x86_64-linux-gnu`, `pe`, `windows-x86_64`, `x86_64-windows-gnu`, `obj`, `o`, `a`, `static`, `lib`, `llvm`, `mlir`, `cuda` |
| `build.optimization` | HIR optimisation effort | `0` (off), `1` (basic), `2` (advanced), `3` (maximum). |
| `build.output` | Native executable output path | Relative to the project root unless an absolute path is supplied. Defaults to the project build output. |
| `source.recursive` | Search nested source directories | Boolean; defaults to `false`. |

The compiler exposes four optimisation tiers: `0` disables HIR optimisation, `1` enables inexpensive local/tree passes, `2` adds CFG/SSA and global value numbering, and `3` enables the full pipeline including scalar replacement of aggregates. New projects default to `3`. The optimiser uses an aggressive bounded fixed-point pipeline and includes CFG reachability, scalar propagation, value-numbering, constant folding, strength reduction, dead-store analysis, and dead-code elimination. For supported scalar-local control flow, the CFG pass constructs and verifies SSA values and explicit Phi nodes. Its sparse conditional constant propagation (SCCP) tracks both the `UNDEFINED / CONSTANT / OVERDEFINED` value lattice and executable CFG edges. A constant `if` or `while` condition activates only its selected edge; Phi nodes merge values only from executable incoming edges. Proven constant local reads are rewritten as HIR integer constants, and subsequent HIR passes fold expressions and remove unreachable branches. The supported scalar types are `Bool`, `Char`, and integer types. Real and pointer locals, unsupported operations, representation-changing casts, and values affected by unmodelled memory/control-flow semantics remain outside this rewrite. SCCP is therefore real edge-sensitive SCCP for the supported scalar subset, not a claim that all HIR constructs or the backend have migrated to SSA.

### 4.3 Additional artifacts

```toml
[artifacts]
object = false
archive = false
pe = false
llvm = false
mlir = false
cuda = false
```

These booleans request additional outputs during `build`:

| Key | Output |
|---|---|
| `object` | ELF relocatable object under `build/bin/` |
| `archive` | Static archive under `build/artifacts/` |
| `pe` | Windows PE32+ executable |
| `llvm` | LLVM-style textual unit under `build/artifacts/` |
| `mlir` | MLIR textual unit under `build/artifacts/` |
| `cuda` | CUDA source under `build/artifacts/` |

`build.target` can select a native executable target or request an additional artifact. The PE target is emitted as the primary PE executable; object, archive, LLVM, MLIR, and CUDA selections still follow the build driver's native-output flow and should not be interpreted as a promise that the ELF executable is suppressed. A native build also writes its relocatable object to `build/bin/<project>.o`. Textual LLVM, MLIR, and CUDA outputs are placed in `build/artifacts/`. CUDA output generation does not by itself guarantee that a CUDA compiler or GPU runtime is installed.

### 4.4 Diagnostics and quality policy

```toml
[quality]
max_errors = 20
extra_warnings = false
warnings_as_errors = false
ignore_warnings = false
```

- `max_errors` is the diagnostic storage limit and must be between `1` and `10000`.
- `extra_warnings` enables the extended warning set where supported.
- `warnings_as_errors` makes warnings fail the relevant command.
- `ignore_warnings` suppresses warnings.

The warning policy is read by build/check, test discovery, benchmark analysis, and clippy. When both warning suppression and warning-as-error policies are enabled, suppressed warnings are not available to promote.

### 4.5 Tests and benchmarks

```toml
[test]
path = "tests"

[bench]
path = "benches"
```

`test.path` selects the directory scanned by `paxsy test`. The command recursively finds `.px` test sources, analyses them, and discovers functions whose names begin with `test_`. It does not generate machine code for the test functions or execute them yet.

`bench.path` selects the directory scanned by `paxsy bench`. The current benchmark command measures lexer throughput on `.px` files and reports token counts and median timings.

### 4.6 Debugger settings

```toml
[debug]
enabled = true
breakpoints = ["main"]
run = false
regs = false
stepi = 0
trace_memory = false
trace_registers = false
trace_all = false
trace_file = "build/debug.trace"
```

The integrated debugger reads its settings from `[debug]`. `enabled = false` prevents starting it. `breakpoints` is a comma-separated list in the TOML value and supports up to 16 entries. Additional keys control run-on-start, register display, instruction stepping, tracing, and the trace output path. Exact support depends on the active native debugging backend and host operating system.

### 4.7 Git automation

```toml
[git]
enabled = true
auto_init = true
```

Git initialisation and post-build snapshots are best-effort project conveniences. The supported policy keys are `enabled`, `auto_init`, `auto_branch`, `branch_name`, `auto_commit`, `commit_on_build`, `include_src`, `include_tests`, `include_benches`, `include_readme`, `include_gitignore`, `include_config`, and `commit_message`. The automatic branch name defaults to `paxsy/{project}` and the commit message defaults to `build: {project} ({files} files)`; `{project}` and `{files}` are expanded. The automatic commit stages only the explicitly enabled project paths and skips when the Git index already contains staged changes. These conveniences do not replace normal version-control workflows. `--no-git` applies to `new`; `PAXSY_NO_GIT` disables post-build Git automation.

### 4.8 Recovery metadata

```toml
[recovery]
embed_source = false
```

When enabled, the native ELF build attempts to embed source metadata so that `paxsy recovery` can restore original source files from a compatible Paxsy-produced binary. This increases artifact size and should be enabled only when source recovery is desired.

### 4.10 TOML section inventory

The current project configuration uses these sections:

- `[project]` — project name, version, description, authors.
- `[build]` — target, optimisation level, output paths.
- `[source]` — recursive source discovery (with legacy `[build].recursive` fallback).
- `[artifacts]` — optional object, archive, PE, LLVM, MLIR, and CUDA outputs.
- `[quality]` — diagnostic limit and warning policy.
- `[test]` and `[bench]` — source directories for test and benchmark discovery.
- `[debug]` — debugger enablement, breakpoints, stepping, registers, and trace options.
- `[git]` — repository initialisation and post-build commit policy.
- `[recovery]` — embedded source metadata policy.
- `[dependencies]` — package/version entries added by `paxsy add`.

Package manifests are separate from project configuration. The standard-library module registry is discovered by scanning `lib/std/src/**/*.px` for `module` declarations; `lib/std/std.toml` now contains package metadata only and no longer enumerates modules. Project `[include.<name>]` tables attach additional source files/directories, and new projects contain `src/mod.px` as the conventional source-level module entry point. Module identity continues to be defined by source `module` declarations; ordinary top-level declarations are file-local, while named module declarations are project-wide. SSA Phi-edge expression sinking is conservative: Phi-edge uses are counted and analysed, and expressions are moved onto individual incoming edges only when the preconditions hold.

### 4.9 Reading and changing settings

```sh
paxsy config build.optimization
paxsy config build.optimization 2
paxsy meta
```

`config` reads or writes one `section.key` in the project TOML in the current directory. Strings should be passed as ordinary text (Paxsy quotes them); numbers and booleans are written as TOML scalars. `meta` prints the project's identity, target, and optimisation setting.

## 5. Compilation model

A normal build proceeds through these stages:

1. **Source discovery** — collect `.px` files under `src/` according to the recursion setting.
2. **Lexing and parsing** — turn source text into tokens and syntax trees.
3. **Project semantic analysis** — resolve names and modules and check types across the collected project sources.
4. **HIR lowering** — lower typed syntax into Paxsy's typed high-level intermediate representation.
5. **Optimisation** — run bounded optimisation rounds. CFG analysis computes reachability and dominators; supported scalar-local CFGs also receive SSA value numbering and explicit Phi placement. Other HIR passes perform propagation, GVN, folding, strength reduction, store elimination, and DCE.
6. **Emission** — the selected backend writes executable or intermediate artifacts.

The native x86-64 backend emits machine code in-process. The ordinary Linux output is a statically linked ELF executable. The PE backend writes a Windows PE32+ image. LLVM-style, MLIR, and CUDA emitters are separate output routes and should be treated as generated representations rather than promises of identical native-backend behaviour.

`PAXSY_NO_OPT=1` disables the HIR optimisation pipeline for the current invocation. It is useful for comparing optimised and unoptimised program behaviour during compiler development. It is not a project configuration setting.

## 6. Command reference

### 6.1 Everyday commands

| Command | Purpose |
|---|---|
| `paxsy new <name>` | Create a project and starter source. |
| `paxsy build [path]` | Compile and emit configured artifacts. |
| `paxsy check [path]` | Parse and type-check without emitting a binary. |
| `paxsy run [path] [-- args...]` | Run the configured runtime profile; forward arguments after `--`. |
| `paxsy test [path]` | Analyse test sources and discover `test_*` functions; does not run them. |
| `paxsy bench [path]` | Measure lexer throughput over the configured benchmark directory. |
| `paxsy fmt [path]` | Format source files in place. |
| `paxsy fmt [path] --check` | Check formatting without rewriting; non-clean formatting returns failure. |
| `paxsy clippy [path]` | Run lexical/style quality checks. |
| `paxsy clippy [path] --fix` | Apply safe whitespace fixes. |
| `paxsy debug [path]` | Build a native debug executable and launch the integrated debugger. |
| `paxsy disasm [path-or-binary]` | Write annotated disassembly output. |
| `paxsy recovery [path-or-binary]` | Recover embedded sources or create best-effort assembly-backed source wrappers. |
| `paxsy clean [path]` | Remove generated `build/`, `target/`, and `debug/` trees. |
| `paxsy tree [path]` | Display the project layout. |

`fmt` follows the formatter's mechanical layout rules and preserves comments and literals. `clippy` is a source-quality check, not a replacement for parsing, semantic analysis, or the language specification. Review files after `--fix`; only the fixes explicitly reported by the command should be expected.

### 6.2 Package and project commands

| Command | Purpose |
|---|---|
| `paxsy fetch` | Refresh the package index. |
| `paxsy search <term>` | Search the local package index. |
| `paxsy install <pkg> [version]` | Install a package into the user's Paxsy package area. |
| `paxsy uninstall <pkg>` | Remove an installed package. |
| `paxsy update <pkg> [version]` | Refresh an installed package. |
| `paxsy add <pkg> [version]` | Add a dependency to the current project's package configuration. |
| `paxsy remove <pkg>` | Remove a dependency from the current project's package configuration. |
| `paxsy list` | List installed packages. |
| `paxsy publish [path]` | Assemble a versioned distributable project tree under `dist/`. |
| `paxsy meta [path]` | Print project metadata. |
| `paxsy config <section.key> [value]` | Read or update one TOML setting. |
| `paxsy state [path]` | Emit a frontend state archive (`.a`). |
| `paxsy shared [path]` | Emit a shared frontend archive (`.sfa`). |
| `paxsy link <output> -- <objects...>` | Link Paxsy-compatible object files into an executable. |

The `state` and `shared` commands are frontend archive operations; they are not aliases for static-library linking. The `link` command is the native object-linking workflow.

### 6.3 Global options

The command-line parser recognises:

- `-h`, `--help` — show command help.
- `-v`, `--version` — show version information.
- `-l`, `--license` — show the license.
- `--verbose` — show additional progress information.
- `--profile` — report per-stage compilation timings for `build`.
- `-q`, `--quiet` — suppress optional progress output.
- `--no-git` — disable Git integration for `new`.
- `--check` — formatter check mode; valid with `fmt`.
- `--fix` — safe-fix mode; valid with `clippy`.

Options may appear before or after positional arguments. A literal `--` ends Paxsy option parsing; remaining arguments are forwarded by commands such as `run`. Unsupported flags are rejected rather than silently ignored. Build and quality policy should be configured in TOML rather than assumed to be available as ad-hoc command-line switches.

### 6.4 Exit statuses

The common convention is `0` for success, `1` for a command that ran but failed, and `2` for an invalid invocation or missing project/configuration input. `run` returns the program's exit status when execution succeeds. Automation should rely on the status code rather than parsing human-readable progress text.

## 7. Output layout

A typical native project build produces:

```text
project/
├── build/
│ ├── <project>       # configured native executable
│ ├── .paxsy-hash      # incremental build stamp
│ ├── report.txt       # where produced by artifact workflows
│ ├── bin/
│ │ └── <project>.o    # relocatable object
│ └── artifacts/
│    ├── <project>.ll    # optional LLVM-style output
│    ├── <project>.mlir   # optional MLIR output
│    ├── <project>.cu    # optional CUDA output
│    └── <project>.a    # optional archive output
├── src/
└── <project>.toml
```

The exact executable name and location follow `[project].name`, `[build].output`, and the selected target. Additional output files exist only when requested. `clean` removes generated build and debug directories; it does not delete source files or the project TOML.

## 8. Linking object files

To emit an object, select `build.target = "obj"` or enable `[artifacts].object`. The object is written under `build/bin/`. Then link one or more compatible objects:

```sh
paxsy link build/program -- build/bin/module_a.o build/bin/module_b.o
```

Use `--` to separate the output operand from the object list. The native linker resolves global symbols and relocations and writes a static x86-64 executable. Duplicate global definitions, unsupported object formats, or unresolved references cause the link to fail. The toolchain's object and linker formats are not intended as a universal replacement for every platform linker or foreign object ABI.

## 9. Package management

The package manager stores packages in a per-user directory. `PAXSY_LIBRARY_DIR` overrides the default root `~/.paxsy`. The root contains cache and installed package trees. Package metadata and source files are read from the package manifest and `src/` tree.

Typical workflow:

```sh
paxsy search json
paxsy install json
paxsy add json
paxsy list
```

- `search` uses the locally available package index.
- `install` fetches or reuses a package and installs it for the current user. When run inside a project, it can also record the dependency.
- `add` and `remove` edit the current project's dependency table.
- `update` replaces an installed package with a refreshed copy.
- `uninstall` removes an installed package.
- `fetch` refreshes the index.

The package repository is hosted under `https://paxsy.org/packages/`. Downloads use `curl` or `wget`. `PAXSY_OFFLINE=1` disables network downloads; a usable local cache can still be used. `PAXSY_LIBRARY_DIR` selects the package root and should be set consistently for the compiler and package commands.

Module imports are resolved through the project's declared module/package context and the standard library. Local modules can be nested; each level extends the qualified path, and nested modules can be imported as a namespace or by individual member:

```px
module graphics {
 module geometry {
    func area(width: Int, height: Int): Int {
      return width * height;
    }
 }
}

use graphics::geometry;
use std::inout;

func main(): Void {
 std::inout::println("{area(8, 5)}");
}
```

The equivalent single-member import is `use graphics::geometry::area;`. Module declarations with the same qualified name may be split across source files; their declarations are collected into the same namespace. A repeated declaration of the same member is still a duplicate definition.

Consult [standard-library-reference.md](standard-library-reference.md) for standard-library modules and APIs. Consult [language-specification.md](language-specification.md) for language syntax and semantics.

## 10. Debugging

`paxsy debug [path]` builds the project using the native debug target and launches the integrated debugger. The debugger uses the generated symbol information and supports the debugger commands exposed by the current native debugging implementation. Configuration is read from `[debug]`, including breakpoints, register display, stepping, and trace settings.

A practical workflow is:

```sh
paxsy check
paxsy debug
```

The debug command rebuilds the project before launching the session so that the executable corresponds to the current sources. Debugging is host- and backend-dependent; do not assume that a Linux `ptrace` workflow is available on every operating system.

## 11. Disassembly and recovery

`paxsy disasm <path-or-binary>` writes a directory of per-symbol assembly listings. The output is intended for inspection of emitted machine code and includes instruction addresses and bytes where available.

`paxsy recovery <path-or-binary>` has two modes of usefulness:

1. If a compatible Paxsy-produced binary contains embedded recovery metadata, the original source files can be restored.
2. Otherwise, the tool can write best-effort source wrappers containing decoded assembly statements.

The second mode is not a lossless decompiler. Machine code does not preserve original comments, source formatting, complete type information, variable names, or reliable high-level control flow. Treat recovered assembly wrappers as an inspection and migration aid, not as equivalent source code.

## 12. State and shared frontend archives

`paxsy state` and `paxsy shared` analyse project sources and package frontend representations into their respective archive formats. The archive commands are useful for preserving or transferring frontend state; they do not emit a runnable native executable. Use `paxsy build` for normal executable generation and `paxsy link` for linking native object files.

## 13. Environment variables

| Variable | Effect |
|---|---|
| `PAXSY_LIBRARY_DIR` | Package cache and installed-package root; default `~/.paxsy`. |
| `PAXSY_OFFLINE` | Any non-empty value other than `0` disables package network downloads. |
| `PAXSY_NO_OPT` | Any non-empty value disables the HIR optimisation loop for the current build. |
| `PAXSY_NO_GIT` | Disables post-build Git automation. |
| `PAXSY_BENCH_ITERATIONS` | Number of benchmark samples; integer from `1` to `101`, default `7`. |
| `PAXSY_DUMP_SSA` | Developer diagnostic: prints scalar-local SSA blocks, definitions, uses, Phi inputs, copy/Phi simplification counts, single-use Phi counts, sunk-expression counts, and removed dead-definition counts. Not a stable user-facing output format. |
| `PAXSY_TDBG` | Developer diagnostic: prints selected type-inference binding and assignment details. |
| `PAXSY_JOBS` | Worker-pool concurrency limit; a positive integer overrides the automatic worker count. |
| `NO_COLOR` | Disables coloured output where supported. |

The installer variables `INSTALL_PATH`, `VERSION`, `NO_VIM`, and `VIMFILES` are documented in the installation section.

## 14. Compiler development and optimisation status

The optimiser operates on typed HIR. CFG analysis constructs basic blocks for the modelled structured control-flow constructs, computes reachable blocks, immediate dominators, dominator-tree children, and dominance frontiers, and verifies those structures before rewriting unreachable HIR. Before pruning, it builds a pointer-sorted statement-to-reachability index; nested HIR statements are then classified with binary search instead of rescanning every CFG block for each statement.

The scalar-local SSA builder creates explicit SSA values and Phi records for supported function control flow. It includes parameter values, local definitions and uses, iterated dominance-frontier Phi placement, dominator-tree renaming, and structural verification of unique definitions and predecessor-specific Phi inputs. Phi placement keeps a dense per-block/per-slot membership index, avoiding repeated linear scans of a block's Phi vector. Dominance-frontier construction uses per-runner generation stamps to deduplicate each target block in constant time instead of rescanning the accumulated frontier list; the canonical frontier vectors and their ordering remain unchanged. Dominator verification walks the dominator tree once from the entry and checks parent/child consistency and reachable-block coverage in linear time; it no longer climbs the immediate-dominator chain independently for every block, which was quadratic on deep CFGs. The same tree receives iterative, half-open DFS intervals, making SSA dominance queries constant-time instead of walking immediate-dominator chains; iterative traversal also avoids recursion on deeply nested CFGs. It also skips the dominance-frontier worklist for slots defined in fewer than two reachable blocks, since those slots cannot require a merge Phi. Sparse conditional constant propagation tracks the `UNDEFINED / CONSTANT / OVERDEFINED` lattice together with executable CFG edges. A constant branch activates only its selected edge, and Phi nodes merge only values from executable incoming edges. SCCP uses a value-to-consumer-block dependency index and a deduplicated block worklist, so only blocks affected by a changed SSA value or a newly executable incoming edge are revisited. The lattice is monotone and finite, allowing the worklist to run to quiescence without an arbitrary iteration cap. Proven constant local reads are rewritten as HIR constants; CFG pruning then removes unreachable branches.

The SSA analysis also computes block `live-in` and `live-out` sets over SSA value IDs using bit vectors and a reverse-RPO fixed-point solver. Phi-edge operand bitsets are precomputed per predecessor before the fixed-point phase, avoiding repeated scans of every successor Phi on each worklist visit. Bitset population counts use compiler-provided population-count intrinsics. Phi results are treated as definitions in their destination block, while each Phi incoming value is added only on its matching predecessor edge; this avoids the common error of making every Phi operand live on every incoming path. Interference construction then starts from each block's `live-out` set and walks its statement-level SSA events backwards: statement definitions are processed before their RHS/condition uses, Phi results are processed at block entry, and implicit parameter definitions are processed at function entry. A definition interferes only with values live at that definition point; dead definitions therefore add no artificial graph pressure. Phi results are simultaneous definitions at block entry, so conflicts among all live Phi results are recorded before any of those results are removed from the live set. The same live-at-definition rule applies to function parameters. This avoids turning every block-boundary set into a clique and removes conflicts caused solely by values being live at different points in the same block. The event order is statement-granular, not machine-instruction-granular, and unsupported or multi-event HIR forms must remain conservative before this analysis can drive a real allocator. Duplicate undirected edges are sorted and removed. The reported peak-live metric is sampled at statement event boundaries (block live-out, after the statement's collected uses, and after Phi/parameter definitions are unwound), rather than only at CFG block boundaries. `PAXSY_DUMP_SSA=1` reports this peak, the unique edge count, maximum graph degree, and the concrete `vN-vM` interference pairs for debugging. The graph is also passed through a deterministic optimistic simplify/select colouring probe with an abstract budget of eight colours. Before colouring, copy and trivial-Phi aliases are canonicalised and exact def-use counts are computed; ordinary reads and predecessor-specific Phi operands both contribute to the estimated cost. Low-degree vertices are simplified first; when none are available, a lazy priority heap selects a low estimated-use-cost spill candidate using those counts rather than treating every value as equally cheap. Reverse selection assigns non-conflicting abstract colours, and every resulting edge is revalidated. `PAXSY_DUMP_SSA=1` reports the number of colours used and spill candidates. The colouring probe is deterministic and abstract; physical allocation, ABI register constraints, call clobbers, instruction-level live ranges, register classes, and Phi parallel-copy resolution belong to the backend allocation path.

The SSA renamer uses a compact change log: each block records only the local slots it changes and restores those values when leaving the dominator subtree. Its dominator-tree traversal is iterative and uses an explicit frame stack, preserving per-subtree rollback checkpoints without consuming one native C stack frame per CFG block; this keeps very deep, generated straight-line functions from exhausting the process stack. This also avoids allocating and copying a full local-state vector for every CFG block. The SSA value simplifier also builds a sorted node-to-value use index once after dominator-tree renaming. Copy folding and SCCP then resolve HIR local reads with binary search instead of repeatedly scanning every block and use. After copy/Phi alias canonicalisation it builds a second sorted value-to-ordinary-use index; single-use expression sinking can locate a candidate's sole ordinary consumer with binary search instead of rescanning every block for every definition. Def-use counts still include both ordinary reads and Phi-edge operands, so a Phi-only consumer or a value with multiple total consumers cannot be mistaken for a single ordinary use. Index allocation failures disable the SSA rewrite path conservatively. Def-use counts include ordinary reads and Phi-edge operands; the pass canonicalizes chains of scalar copy values and eliminates trivial Phi nodes whose non-self incoming values resolve to one identical SSA value. It also identifies non-trivial Phi values with one consumer. A conservative single-use expression-sinking rewrite moves a small set of pure integer expressions from a `let` into its sole immediate-successor use and removes the now-redundant declaration. It requires a direct CFG edge, a dominating definition, exactly one ordinary consumer, and a side-effect-free, non-trapping expression. A separate dead-definition rewrite removes scalar `let`/store statements only when the corresponding non-aliased SSA value has no consumers and the initializer is proven pure. This removes overwritten initial values and unused pure scalar bindings, but deliberately does not erase alias definitions: the current backend still executes HIR slots rather than SSA values. Calls, memory operations, division, modulo, and any case crossing a branch or intervening statement are left untouched. Phi-edge consumers are analysed; edge-specific rewrites are applied only when the conservative preconditions hold.

SROA runs once before the iterative pass pipeline rather than repeating its whole-function use scan in every fixpoint round.

HIR lowering keeps each lexical scope's local bindings in a canonical growable vector and maintains a per-scope open-addressed hash index for name-to-slot lookup. The index grows at a bounded load factor and is discarded in favour of the original reverse linear scan if allocation fails. Scope push/pop owns and releases its index independently, so nested scopes cannot leak bindings into their parents. This accelerates large functions with many local names without changing slot numbering or shadowing resolution. The `large_local_scope_hash` language regression and the full-language benchmark exercise scopes larger than several hash-table capacities; the benchmark includes a 512-local function whose bindings are read in reverse order.

A separate HIR control-flow simplification removes redundant result locals in the conservative pattern `let result = if condition { ... } else { ... }; return result;`. It follows an immediately adjacent chain of single-use local aliases and turns the selected arm's value into a direct return. Every link must have exactly one syntactic read, and no intervening statement may be crossed, so selected-arm evaluation and side-effect ordering are preserved. Multiple-use locals, missing else arms, unsupported arm shapes, non-adjacent aliases, and cases where safety cannot be proven are left unchanged.

SSA construction and SCCP support scalar `Bool`, `Char`, and integer locals. The builder conservatively declines constructs whose implicit control-flow/value semantics are not represented in this model, including range-for induction variables, do-while lowering, match-arm bindings, defer, labelled control flow, address-taken locals, inline assembly, error-control constructs, and concurrency constructs. Declining SSA is conservative; it does not mean those source constructs are rejected by the compiler.

The HIR GVN pass indexes reusable expressions in a per-function hash table (power-of-two bucket array with collision chains keyed by structural expression hash), rather than scanning every local slot for each lookup. It also maintains a reverse dependency index from each read local slot to the expression holders that depend on it, so a normal slot write invalidates only affected facts instead of recursively inspecting every saved expression. The dependency index is rebuilt alongside the hash index after branch and loop snapshot restoration. If index allocation fails, GVN falls back to conservative full scans rather than risking stale facts. Slot invalidation unlinks facts from both indexes; branch and loop state restoration rebuilds the indexes from the restored fact set. Structural equality remains the collision check, so hash collisions cannot cause incorrect substitutions. Expression safety and structural hashes are computed bottom-up during the expression walk, reusing each child's result instead of recursively rescanning every candidate subtree. Barrier detection is likewise performed once at each expression root; the result applies to the entire subtree. This avoids quadratic repeated walks on long, left-associated expressions while retaining the same conservative barrier policy. The HIR GVN pass performs structural value numbering for safe integer-like expressions (`Bool`, `Char`, signed and unsigned integers). It recognizes commutative operand order, tracks the local slot holding each reusable result, and invalidates facts when a holder or an operand changes. Facts cross an `if` join only when every branch preserves the same expression in the same holder; loop handling retains only proven loop-invariant facts. Calls, indirect memory operations, and nested stores are barriers. In particular, stores can be nested inside `HK_EXPR_STMT` rather than appearing as top-level `HK_STORE` statements, so the expression walker must treat a nested store as a barrier too. A regression test compares optimized and unoptimized results when opposite branches mutate different operands.

The simplifier performs a conservative form of scalar replacement for local struct literals. When a local is initialized from a pure struct literal and every use of that local is a read of a known field, field reads are forwarded directly to the corresponding literal values. Taking the aggregate address, reading the whole aggregate, writing any field, or encountering an unset/unknown field cancels the rewrite. Aggregate literals are considered pure only when every element is pure, allowing DCE to remove the now-unused aggregate binding without dropping side effects. Constant-index reads from pure array literals are forwarded similarly. This is intentionally narrower than general SROA: escaping aggregates, mutable fields, dynamic indices, and partially unknown layouts remain materialized.

The HIR simplifier also folds constant left operands of short-circuit Boolean expressions (`false && rhs`, `true && rhs`, `false || rhs`, and `true || rhs`) while preserving whether `rhs` is evaluated. It also applies repeated-operand identities to stable scalar values: `x - x` and `x ^ x` become zero, while `x & x` becomes `x`, and repeated Boolean operands in `x && x` and `x || x` collapse to `x`; the existing self-comparison rules fold equality and ordering comparisons. These rewrites are restricted to identical local/parameter/constant nodes with a supported scalar type, avoiding assumptions about calls, memory reads, or values that may change between evaluations.

The native and textual emitters consume HIR. GVN and the optimisation passes use the IR forms documented in compiler source and regression tests. Memory SSA, aggregate/pointer SSA, Phi-aware code motion, and backend lowering are compiler-development topics recorded with the corresponding tests.

For compiler development, run the regression suite after changing optimisation code:

```sh
make test
make test-asan
make test-ubsan
make test-fuzz
```

`PAXSY_DUMP_SSA=1` is useful when checking whether a branch join or loop-carried scalar local receives a Phi record. Its textual format is diagnostic and may change between releases.

## 15. Example end-to-end workflow

```sh
paxsy new sample_app
cd sample_app

# Edit src/main.px and add source files under src/.
paxsy check
paxsy fmt
paxsy clippy
paxsy build
paxsy run -- --help

# Inspect project settings and outputs.
paxsy meta
paxsy config build.optimization
paxsy tree

# Analyse test sources and discover test_* declarations.
paxsy test

# Measure lexer throughput for .px files under benches/.
paxsy bench

# Remove generated outputs while preserving project sources.
paxsy clean
```

## 16. Related documentation

- [Language Specification](language-specification.md) — syntax, types, control flow, and language semantics.
- [Code Style Guide](code-style-guide.md) — source conventions and formatting expectations.
- [Standard Library Reference](standard-library-reference.md) — standard-library modules and public APIs.

This manual is the primary user-facing reference for installing and operating the Paxsy toolchain. When command behaviour changes, update this document and the relevant tests together; do not replace it with a second, competing command reference.


SSA liveness uses bit sets and a deduplicated dependency worklist. Phi operands are attached to predecessor edges. Their bitsets are precomputed per predecessor before the fixed-point solver, avoiding repeated scans of every successor Phi on each worklist visit; predecessor blocks are revisited only when a successor's live-in set changes. Interference edges are derived by a reverse statement-event walk from each block's live-out set, rather than by making all boundary sets cliques. This remains diagnostic groundwork; physical allocation and spills are handled by the backend path.
