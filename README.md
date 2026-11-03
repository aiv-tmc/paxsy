# Paxsy

The Paxsy toolchain: a systems programming language with a fully
self-hosted build pipeline. One program — `paxsy` — is the compiler,
the build orchestrator and the package manager. It reads Paxsy
sources, analyses them, and turns them into runnable artifacts with
its own machine-code emitter: no external compiler, assembler or
linker is invoked at any step, and programs link no runtime library.

## Highlights

* **Self-hosted native backend.** The compiler lowers the typed HIR
  to x86-64 machine code in-process, emits the start-up sequence,
  lays out the ELF image and writes a static executable. Object
  files (`--emit-obj`), archives (`--emit-a`), PE32+ images
  (`--emit-exe`) and LLVM/MLIR/CUDA listings are produced by the
  same driver. `paxsy link` joins emitted objects with the
  toolchain's own static linker.
* **No builtin functions.** The standard library is written in Paxsy
  itself. Console output, string handling and process control are
  implemented with inline assembly and raw syscalls; a compiled
  program carries no runtime archive and no hidden entry points.
* **Inline assembly.** An `asm "…"` statement embeds x86-64
  instructions directly into a function. The built-in assembler
  supports the portable core set (moves, ALU ops, shifts, division,
  stack operations, control transfer, `syscall`), resolves labels
  through the shared fixup machinery and reports unknown mnemonics
  with the exact source segment.
* **Explicit module system.** Sources declare plain modules
  (`module geometry { … }`); package sources (the standard library)
  are qualified with the package name automatically, so a library
  file declares `module inout { … }` and programs reach it as
  `std::inout`. `use std::inout;` is validated against the package
  manifest; `use inout;` is refused unless the project declares such
  a module itself.
* **Package manager built in.** `paxsy install`, `add`, `remove`,
  `update`, `search`, `fetch` and `list` manage libraries from the
  repository at paxsy.org with a local-cache fallback.
* **Deterministic, incremental builds.** Source discovery is
  deterministic, rebuilds are freshness-checked against every input
  plus the compiler itself, and `PAXSY_NO_OPT=1` is the A/B switch
  for the optimisation pipeline.

## Quick start

Build the toolchain from the repository:

```sh
git clone https://github.com/aiv-tmc/paxsy.git
cd paxsy
make            # produces ./paxsy
make test       # stages ./paxsy with lib/std and runs the suite
sudo make install   # or: sudo cp paxsy /usr/local/bin/paxsy
```

Create and run a project:

```sh
paxsy new hello
cd hello
paxsy build
paxsy run
```

The seeded program prints a greeting through the standard library:

```px
use std::inout;

/// Prints a greeting and exits.
func main(): Void {
    std::inout::println("Hello, World");
}
```

## The standard library

The library is a Paxsy package carried under `lib/std` (and installed
next to the compiler under `lib/paxsy/std`). Its manifest `std.toml`
lists the libraries a `use` directive may name; only the libraries a
program actually references are compiled.

| Module        | Entries                              |
|---------------|--------------------------------------|
| `std::inout`  | `println(s: &Int8): Void!`           |
| `std::string` | `strlen`, `strcpy`, `streq`          |

Every entry point is an ordinary Paxsy function implemented with
inline assembly; the compiler knows none of them by name.

## Inline assembly

```px
func println(s: &Int8): Void! {
    asm "mov r8, rdi ; xor ecx, ecx ; pxs_len: xor edx, edx ; …";
}
```

Instructions are separated by `;` or by `\n` escapes; labels end
with `:` and may share a segment with an instruction; mnemonics are
case-insensitive; register names accept both the 64-bit spelling
(`rcx`) and the classic 32-bit spelling (`ecx`) because every
operation is 64-bit. Anything the assembler cannot encode is a hard
error that names the offending segment.

## Targets and artifacts

`paxsy build` always produces the native executable at
`build/<name>`. Extra artifacts are opt-in:

```sh
paxsy build --emit-obj        # build/bin/<name>.o  (ELF relocatable)
paxsy build --emit-a          # build/bin/<name>.a  (ar archive)
paxsy build --emit-exe        # build/bin/<name>.exe (PE32+)
paxsy build --emit-llvm       # build/bin/<name>.ll
paxsy build --emit-mlir       # build/bin/<name>.mlir
paxsy build --emit-cuda       # build/bin/<name>.cu
paxsy link out.elf -- a.o b.o # static link without external tools
```

## Configuration

Every build-affecting setting lives in `<project>.toml`. Common
fields:

```toml
[project]
name = "hello"
version = "0.1.0"

[build]
recursive = false      # descend into subdirectories of src/
```

Package dependencies are recorded in `[dependencies]` and resolved
against the package cache (`$PAXSY_LIBRARY_DIR`, default
`~/.paxsy`).

## Development

```sh
make              # strict C99 build (-Wall -Wextra)
make test         # full Python test suite against the staged binary
make test-asan    # AddressSanitizer rebuild + suite
```

Continuous integration (`.github/workflows/ci.yml`) runs the suite
under gcc and clang, ASan/UBSan/valgrind jobs, a self-hosted
link smoke test and a release-tarball artifact on every push; tags
publish a release through `.github/workflows/release.yml`.

See `docs/cli.md` for the full command reference and
`docs/ToolchainUserManual.md` for the complete language and toolchain
manual.

## License

MIT. See `LICENSE`.
