# Paxsy

**Paxsy** is a statically-typed, compiled systems programming language designed for software that must be fast, predictable, and safe — without relying on a garbage collector. It targets native machine code and gives the programmer direct control over memory layout, allocation, and CPU behaviour, while providing modern facilities for error handling, compile-time evaluation, and concurrency.

Paxsy is built for operating system kernels, device drivers, embedded firmware, real-time controllers, game engines, network servers, and any domain where the cost of every allocation and every branch must be understood.

## Features

- **Native performance** — compiles to machine code with no hidden runtime, no tracing collector, and no interpreter.
- **Determinism** — the same source, compiled with the same options, produces the same machine code and the same memory layout on every machine.
- **Memory safety without GC** — explicit ownership conventions, zero-pointer checks, and a type system that distinguishes values from errors prevent use-after-free, double-free, and zero-pointer dereference.
- **Errors as values** — no exceptions, no `panic`. Every failure is a typed `T!` value, propagated with `try` or handled with `catch`.
- **Two concurrency models** — lightweight cooperative `routine`s and parallel native `async` threads, both communicating through typed `chan`nels. No shared mutable state, no data races by construction.
- **First-class compile-time evaluation** — constants, types, functions, and blocks evaluated by the compiler, so array sizes, lookup tables, and configuration values are computed once at build time.
- **Compact static typing** — type families (`Int`, `UInt`, `Real`, `Char`) and `Auto` let the compiler pick the smallest suitable representation, fixed permanently at first assignment.

## Quick Start

Build the toolchain from source and install it:

```
$ make
$ ./install.sh
```

Create a new project, build it, and run it:

```
$ paxsy init myapp
Created project 'myapp' with src/, lib/, include/, myapp.toml, and a Git repository.
$ cd myapp
$ paxsy build
$ paxsy run
```

The compiler is `paxsy`; the package manager is `paxsy-lib`. Every project is configured by a single file, `<project>.toml`, which is the single source of truth for the build.

## A Taste of Paxsy

Resource safety is enforced by convention and visible in the types — every acquisition is paired with a `defer` that releases it:

```
func copy_data(src_path: Char[], dst_path: Char[]): Void! {
    let src = try std::file::open(src_path);
    defer std::file::close(src);

    let dst = try std::file::create(dst_path);
    defer std::file::close(dst);

    let buf: &Int8 = alloc Int8[1024];
    if buf == 0 {
        return error 1;
    }
    defer free buf;

    // ...
}
```

Concurrency is message-passing, not shared memory:

```
let jobs: chan Job = chan 16;

routine {
    for 1..100 |i| {
        jobs <- Job {.id = i, .payload = i};
    }
    close jobs;
};
```

## Documentation

The full documentation lives in the [project wiki](https://github.com/aiv-tmc/paxsy/wiki):

| Document | Contents |
| --- | --- |
| [Language Specification](https://github.com/aiv-tmc/paxsy/wiki/Language-Specification) | The authoritative description of the language: syntax, semantics, data types, memory management, `comptime`, modules, concurrency, inline assembly. |
| [Standard Library Reference](https://github.com/aiv-tmc/paxsy/wiki/Standard-Library-Reference) | Every `std::` module: console I/O, strings, Unicode, dynamic arrays, math, RNG, regex, time, files, paths, directories, processes, networking, HTTP, URLs, JSON, serialisation, hashing, crypto, databases, logging, windows, input, graphics, images, audio, threads, sync, compression, terminal. |
| [Toolchain User Manual](https://github.com/aiv-tmc/paxsy/wiki/Toolchain-User-Manual) | The compiler and build orchestrator, the `<project>.toml` configuration file, target formats, all commands, the `paxsy-lib` package manager, environment variables, and debugging support. |
| [Code Style Guide](https://github.com/aiv-tmc/paxsy/wiki/Code-Style-Guide) | The conventions used across the standard library, the compiler, and user projects: formatting, naming, declarations, error handling, resource safety, and anti-patterns. |

## Example

A small packet-processing program demonstrating structs, enums, `comptime` constants, `defer`, error handling, and `match`:

```
comptime MAX_PACKET_SIZE = 1500;
comptime HEADER_SIZE = 4;

comptime PacketType = Enum {
    Data = 0,
    Ack = 1,
    Error = 2,
};

func parse_packet(raw: &UInt8, len: Int32): Packet! {
    if raw == 0 {
        return error ParseError.ZeroPointer;
    }
    if len < HEADER_SIZE {
        return error ParseError.TooShort;
    }
    // ...
}

func main(): Void! {
    let raw: UInt8[8] = {0, 0, 5, 0, 10, 20, 30, 40};
    let pkt = try parse_packet(&raw[0], 8);
    defer free pkt.payload;
    print_summary(pkt);
}
```

## Current Status

The toolchain is under active development. Notable limitations of the current implementation:

- AArch64 is fully supported; x86-64 is under development.
- The integrated debugger supports single-threaded programs only.
- Shared library production on Windows is not yet implemented.
- The package manager's remote mirrors are not yet operational; it uses a local package cache.
- Some configuration fields (e.g. `[scripts]` hooks) are recognised but not yet executed.

## Design Principles

1. **Readability over cleverness** — code is read far more often than it is written.
2. **Explicit is better than implicit** — types, errors, and ownership are stated, not inferred from hidden state.
3. **Fail fast and visibly** — errors are detected at their source and reported as values.
4. **Minimalism** — abstractions are introduced to solve real problems, not to reorganise code.

## License

Distributed under the MIT License. See `LICENSE` for details.
