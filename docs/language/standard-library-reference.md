## Preface

The Paxsy standard library provides the essential building blocks from which programs are constructed. It covers console input and output, string and Unicode manipulation, dynamic arrays, low-level memory operations, mathematics, randomness, regular expressions, timekeeping, the filesystem, the operating-system interface, networking, data formats, cryptography, database connectivity, logging, framebuffer graphics, audio, low-level threading and synchronisation, compression, and terminal control. Pure language facilities are usable across targets; operating-system and device modules expose the platform interfaces named in their descriptions. Application-specific integrations and database-engine drivers are supplied as packages.

Every entity in the library is reachable through the `std::` namespace. A program that wishes to print to the console calls `std::inout::println`, and a program that wishes to read a file calls `std::file::read_file`. The `use` directive may bring a module or a single symbol into the current scope so that the fully qualified name is not required at every call site, but this reference always writes fully qualified names so that each function's origin is unambiguous.

Functions that can fail return a type suffixed with `!`, and the error value is an integer code. The library does not define a single global error enumeration; instead, each module declares constants for the errors that its functions can produce. A caller who wishes to distinguish between errors compares the error code with the relevant constants, and a caller who wishes to propagate the error uses `try`. A caller who wishes to handle the error locally uses `catch`.

Every owning resource created by the library has a corresponding release operation. A file handle opened with `std::file::File.open` is closed with `std::file::File.close`; a graphics context created with `std::graphics::create_context` is destroyed with `std::graphics::destroy_context`; a dynamic array created with `std::array::make` is released with `std::array::destroy`. The idiomatic way to ensure that a resource is released on every exit path, including error paths, is to schedule the release with a `defer` statement placed immediately after the resource is acquired.

Opaque handle types such as those returned by file, socket, and graphics functions are declared with `static` as type aliases for pointers to opaque internal types; the internal types are not exported and their contents are not part of the public interface.

All prose, all code, and all code comments in the library and in this reference are written in English. Identifiers use the naming conventions of the language: `snake_case` for functions and variables, `PascalCase` for types, and `SCREAMING_SNAKE_CASE` for compile-time constants.

## Module Overview

The following table lists every module in the standard library, the responsibility that it fulfils, and the modules that it depends upon.

| Module | Responsibility | Dependencies |
| --- | --- | --- |
| `std::inout` | Console input and output | `std::sys`, `std::heap` |
| `std::console` | Formatted terminal output | `std::sys`, `std::string`, `std::fmt` |
| `std::string` | String manipulation | `std::mem` |
| `std::str` | Owning heap string | `std::heap`, `std::string`, `std::mem` |
| `std::unicode` | UTF-8 validation and decoding | — |
| `std::bytes` | Growable byte buffer | `std::heap`, `std::string` |
| `std::array` | Growable typed array | `std::heap` |
| `std::mem` | Low-level memory operations | — |
| `std::heap` | Heap allocator (heap, arena, pool) | `std::sys`, `std::mem` |
| `std::bit` | Bit manipulation | — |
| `std::math` | Scalar mathematics | — |
| `std::trig` | Trigonometry | `std::math` |
| `std::linalg` | Vectors, matrices, quaternions, transformations | `std::math`, `std::trig` |
| `std::camera` | Camera and projection matrices | `std::linalg` |
| `std::random` | Pseudorandom numbers and sampling | `std::sys`, `std::time` |
| `std::regex` | Regular expressions | `std::heap`, `std::string` |
| `std::time` | Clocks, sleeping, and measurement | `std::sys` |
| `std::datetime` | Calendar dates and times | `std::str`, `std::time` |
| `std::file` | File operations and locking | `std::sys` |
| `std::fs` | High-level filesystem operations | `std::file`, `std::heap`, `std::string` |
| `std::path` | Path manipulation | `std::string`, `std::mem` |
| `std::dir` | Directory listing | `std::sys` |
| `std::dirs` | Directory creation and removal | `std::sys` |
| `std::io` | Buffered readers and writers | `std::sys`, `std::heap`, `std::mem` |
| `std::poll` | Descriptor polling | `std::sys`, `std::heap` |
| `std::stat` | File metadata | `std::sys` |
| `std::sys` | System calls | — |
| `std::err` | Error code names and messages | — |
| `std::args` | Command-line arguments | `std::sys` |
| `std::env` | Environment variables | `std::sys` |
| `std::process` | Child processes | `std::sys` |
| `std::pipe` | Pipes and captured output | `std::sys`, `std::heap`, `std::process` |
| `std::lock` | Advisory file locks | `std::sys` |
| `std::rlimit` | Resource limits | `std::sys` |
| `std::sched` | CPU affinity | `std::sys` |
| `std::sysrand` | System randomness | `std::sys` |
| `std::net` | TCP, UDP, DNS, and socket timeouts | `std::sys`, `std::heap` |
| `std::http` | HTTP client | `std::net`, `std::heap`, `std::string`, `std::fmt` |
| `std::url` | URL parsing and serialisation | `std::heap`, `std::string`, `std::mem` |
| `std::json` | JSON parsing | `std::heap`, `std::string` |
| `std::json_write` | JSON serialisation | `std::heap`, `std::string`, `std::bytes` |
| `std::serialize` | Portable binary encoding of supported value types | `std::mem` |
| `std::hash` | Checksums (CRC-32, Adler-32) | — |
| `std::sha256` | SHA-256 digest | `std::mem` |
| `std::sha512` | SHA-512 digest | `std::mem` |
| `std::hmac` | HMAC-SHA-256 | `std::heap`, `std::sha256` |
| `std::pbkdf2` | Password-based key derivation | `std::heap`, `std::hmac` |
| `std::db` | Database connectivity | — |
| `std::log` | Application logging | `std::sys`, `std::string` |
| `std::window` | Off-screen framebuffer surfaces | `std::heap`, `std::graphics` |
| `std::input` | Keyboard, mouse, and gamepad events | `std::sys`, `std::time` |
| `std::graphics` | Framebuffer rendering | `std::sys` |
| `std::image` | Image creation and BMP import/export | `std::heap`, `std::file`, `std::sys` |
| `std::audio` | Audio playback | `std::sys` |
| `std::thread` | Low-level native threads | `std::sys`, `std::heap` |
| `std::atomic` | Atomic operations and memory ordering | — |
| `std::sync` | Mutex | `std::atomic` |
| `std::inflate` | DEFLATE decompression | `std::mem` |
| `std::term` | Terminal raw mode | — |
| `std::test` | Test assertions | — |
| `std::fmt` | Number formatting and parsing | — |
| `std::csv` | CSV parsing and writing | `std::string`, `std::str` |
| `std::encoding` | Hexadecimal, Base64, and percent encoding | `std::heap`, `std::string` |
| `std::ini` | INI configuration parsing | `std::map`, `std::mem` |
| `std::template` | String template rendering | `std::map`, `std::fmt` |
| `std::text` | Text processing: split, join, trim, replace | `std::heap`, `std::str`, `std::string` |
| `std::list` | Doubly linked list (typed) | `std::heap` |
| `std::deque` | Double-ended queue (typed) | `std::heap` |
| `std::ring` | Fixed-capacity byte ring | `std::heap`, `std::mem` |
| `std::pqueue` | Priority queue (typed, comparator) | `std::heap` |
| `std::map` | String-keyed hash map (typed values) | `std::heap`, `std::mem`, `std::string` |
| `std::set` | String hash set | `std::heap`, `std::mem`, `std::string` |
| `std::algorithm` | Generic algorithms over views | — |

Binary serialization is provided by `std::serialize`. Text encodings such as hexadecimal, Base64, and URL percent encoding are provided by `std::encoding`.

The operating-system modules expose the Linux system interfaces described by their APIs. Device modules operate on the corresponding Linux device interfaces: framebuffer rendering uses `/dev/fb0`, input uses evdev, and audio uses OSS-compatible `/dev/dsp`. Their functions return the operating-system error code when a device or permission is unavailable. Database access is driver-based: packages implement the `std::db` interface for their database engine.

## Console Input and Output

The `std::inout` module provides access to the program's standard input, standard output, and standard error streams. These streams are the program's default channels for text-based interaction with the user and with the surrounding environment. The public API is stable across the supported targets; the implementation maps each operation to the target operating system's standard descriptors.

The functions in this module are the standard way to display information to the user and to read information from the user. Each call performs a direct write with a short-write retry loop; programs that emit many small writes should use `std::io::Writer` to batch them.

### Functions

#### `std::inout::print`

Signature: `std::inout::print(message: Char[]) -> Void!`

Writes the string `message` to standard output without a trailing newline. The characters of the string are emitted in order until the null terminator is reached. The function returns a `Void` value on success and an error on failure.

Parameters: `message` is the string to write. It must be a null-terminated `Char[]`.

Returns: a `Void` value on success.

Errors: `std::inout::ERR_WRITE` is returned if the underlying write operation fails, which can happen if the output stream is closed or if an I/O error occurs.

Example: the following function writes a greeting to standard output:

```
func greet(name: Char[]): Void! {
 try std::inout::print("Hello, ");
 try std::inout::print(name);
 try std::inout::print("!");
}
```

Ownership: the caller retains ownership of the string `message`. The function does not modify it.

#### `std::inout::println`

Signature: `std::inout::println(message: Char[]) -> Void!`

Writes the string `message` to standard output followed by a single newline character. The behaviour is otherwise identical to `std::inout::print`.

Parameters: `message` is the string to write.

Returns: a `Void` value on success.

Errors: `std::inout::ERR_WRITE` is returned if the write operation fails.

Example: the following function prints a formatted message:

```
func report(count: Int32): Void! {
 try std::inout::println("Found {count} items.");
}
```

#### `std::inout::readln`

Signature: `std::inout::readln() -> Char[]!`

Reads a line from standard input and returns it as a null-terminated `Char[]`. The returned string does not include the newline character that terminated the line. If the end of the input is reached before any characters are read, an error is returned.

Parameters: none.

Returns: a newly allocated `Char[]` containing the line that was read. The caller owns the returned string and is responsible for releasing it.

Errors: `std::inout::ERR_EOF` is returned if the end of the input is reached before any characters are read. `std::inout::ERR_READ` is returned if the underlying read operation fails.

Example: the following function reads a name from the user and greets the user:

```
func interactive_greeting(): Void! {
 try std::inout::print("Enter your name: ");
 let name = try std::inout::readln();
 defer free name;
 try std::inout::println("Hello, {name}!");
}
```

Ownership: the caller owns the returned string and must release it with `free` when it is no longer needed.

#### `std::inout::read`

Signature: `std::inout::read() -> Char[]!`

Reads all available input from standard input until the end of the stream and returns it as a null-terminated `Char[]`. This function is useful for reading piped input or for reading a file that has been redirected to standard input.

Parameters: none.

Returns: a newly allocated `Char[]` containing all available input.

Errors: `std::inout::ERR_READ` is returned if the underlying read operation fails.

Example: the following function reads all input and prints its length:

```
func count_input(): Void! {
 let data = try std::inout::read();
 defer free data;
 let n = std::string::len(data);
 try std::inout::println("Read {n} characters.");
}
```

Ownership: the caller owns the returned string and must release it with `free` when it is no longer needed.

## String Manipulation

The `std::string` module provides functions for working with null-terminated strings of `Char` values. The functions in this module treat strings as sequences of code units and do not assume any particular character encoding for the operations that do not require one. Functions that produce a new string write into a caller-supplied buffer and report error 36 (`ENAMETOOLONG`) when the buffer is too small, so callers never have to guess output sizes. Case conversion is ASCII-only; Unicode-aware processing belongs to the `std::unicode` module.

Unless a function is explicitly described as modifying its argument, the functions in this module are pure: they read their arguments and return a new value without modifying the original.

### Functions

#### `std::string::len`

Signature: `std::string::len(s: Char[]) -> UInt64`

Returns the number of code units in the string `s`, excluding the null terminator. For a string of UTF-8 code units, this is the number of bytes, which is not necessarily the number of characters.

Parameters: `s` is the string whose length is to be measured.

Returns: the number of code units before the null terminator.

Example: the following function checks whether a string is empty:

```
func is_empty(s: Char[]): Bool {
 return std::string::len(s) == 0;
}
```

#### `std::string::copy`

Signature: `std::string::copy(dst: Char[], src: Char[], cap: UInt64) -> UInt64!`

Copies `src`, terminator included, into `dst`.

Parameters: `dst` is the destination buffer; `src` is the string to copy; `cap` is the destination capacity in bytes.

Returns: the number of bytes copied.

Errors: error 36 is returned when `cap` is too small for the string and its terminator; `dst` is left unmodified.

Example: the following function copies a path into a fixed buffer:

```
func save_path(dst: Char[], src: Char[]): UInt64! {
 return try std::string::copy(dst, src, 4096);
}
```

#### `std::string::concat`

Signature: `std::string::concat(out: Char[], a: Char[], b: Char[], cap: UInt64) -> UInt64!`

Writes the concatenation of `a` and `b`, NUL-terminated, into `out`.

Parameters: `out` is the destination buffer; `a` and `b` are the strings to join; `cap` is the destination capacity.

Returns: the written length.

Errors: error 36 is returned when `cap` cannot hold the result.

Example: the following function joins two names with a slash:

```
func join_name(out: Char[], a: Char[], b: Char[]): UInt64! {
 return try std::string::concat(out, a, b, 4096);
}
```

#### `std::string::compare`

Signature: `std::string::compare(a: Char[], b: Char[]) -> Int32`

Compares two strings lexicographically. The comparison is performed code unit by code unit, treating bytes as unsigned. The function returns a negative value if `a` is less than `b`, zero if the two strings are equal, and a positive value if `a` is greater than `b`.

Parameters: `a` and `b` are the strings to compare.

Returns: a negative, zero, or positive integer according to the ordering.

Example: the following function reports the ordering of two strings:

```
func order(a: Char[], b: Char[]): Void {
 if std::string::compare(a, b) > 0 {
    // a comes after b
 }
}
```

#### `std::string::find_substring`

Signature: `std::string::find_substring(haystack: Char[], needle: Char[]) -> UInt64`

Returns the offset of the first occurrence of `needle` within `haystack`. The search is performed over code units and may split a multi-byte character if the needle is not aligned to a character boundary. An empty needle matches at offset zero.

Parameters: `haystack` is the string to search within; `needle` is the string to search for.

Returns: the zero-based offset of the first occurrence, or the length of `haystack` when the needle is absent. The result is always a valid insertion or split point.

Example: the following function reports whether a substring is present:

```
func contains(text: Char[], word: Char[]): Bool {
 return std::string::find_substring(text, word) < std::string::len(text);
}
```

#### `std::string::find_char`

Signature: `std::string::find_char(s: Char[], ch: Char) -> UInt64`

Returns the offset of the first occurrence of `ch` in `s`, or the length of `s` when the byte is absent.

Parameters: `s` is the string to search; `ch` is the byte to find.

Returns: the offset of the first occurrence, or the length of `s`.

Example: the following function finds the first comma in a line:

```
func comma_at(line: Char[]): UInt64 {
 return std::string::find_char(line, ',');
}
```

#### `std::string::starts_with`

Signature: `std::string::starts_with(s: Char[], prefix: Char[]) -> Bool`

Returns `true` if the string `s` begins with the string `prefix`. An empty prefix is considered to match any string.

Parameters: `s` is the string to test; `prefix` is the prefix to test for.

Returns: `true` if `s` starts with `prefix`, `false` otherwise.

Example: the following function checks whether a path is absolute on a Unix-like system:

```
func is_absolute(path: Char[]): Bool {
 return std::string::starts_with(path, "/");
}
```

#### `std::string::ends_with`

Signature: `std::string::ends_with(s: Char[], suffix: Char[]) -> Bool`

Returns `true` if the string `s` ends with the string `suffix`. An empty suffix is considered to match any string.

Parameters: `s` is the string to test; `suffix` is the suffix to test for.

Returns: `true` if `s` ends with `suffix`, `false` otherwise.

Example: the following function checks whether a file name has a particular extension:

```
func has_extension(name: Char[], ext: Char[]): Bool {
 return std::string::ends_with(name, ext);
}
```

#### `std::string::slice_into`

Signature: `std::string::slice_into(s: Char[], start: UInt64, count: UInt64, out: Char[], cap: UInt64) -> UInt64!`

Writes the `count` code units starting at offset `start` of `s`, NUL-terminated, into `out`.

Parameters: `s` is the source string; `start` is the starting offset; `count` is the number of code units; `out` is the destination buffer; `cap` is its capacity.

Returns: the written length.

Errors: error 36 is returned when the range runs past the end of `s` or when `cap` is too small.

Example: the following function extracts the first three characters of a string:

```
func prefix3(s: Char[], out: Char[]): UInt64! {
 return try std::string::slice_into(s, 0, 3, out, 64);
}
```

#### `std::string::trim_into`

Signature: `std::string::trim_into(s: Char[], out: Char[], cap: UInt64) -> UInt64!`

Writes `s` with leading and trailing ASCII whitespace removed, NUL-terminated, into `out`. Whitespace is the six-byte ASCII set: space, tab, newline, carriage return, vertical tab, and form feed.

Parameters: `s` is the string to trim; `out` is the destination buffer; `cap` is its capacity.

Returns: the written length.

Errors: error 36 is returned when `cap` is too small.

Example: the following function trims a line of input before processing it:

```
func process_line(line: Char[], out: Char[]): UInt64! {
 return try std::string::trim_into(line, out, 4096);
}
```

#### `std::string::to_upper_into`

Signature: `std::string::to_upper_into(s: Char[], out: Char[], cap: UInt64) -> UInt64!`

Writes the ASCII-uppercased form of `s` into `out`, NUL-terminated. Bytes above `0x7F` pass through unchanged.

Parameters: `s` is the string to convert; `out` is the destination buffer; `cap` is its capacity.

Returns: the written length.

Errors: error 36 is returned when `cap` is too small.

Example: the following function uppercases a command name:

```
func shout(s: Char[], out: Char[]): UInt64! {
 return try std::string::to_upper_into(s, out, 256);
}
```

#### `std::string::to_lower_into`

Signature: `std::string::to_lower_into(s: Char[], out: Char[], cap: UInt64) -> UInt64!`

Writes the ASCII-lowercased form of `s` into `out`, NUL-terminated. Bytes above `0x7F` pass through unchanged.

Parameters: `s` is the string to convert; `out` is the destination buffer; `cap` is its capacity.

Returns: the written length.

Errors: error 36 is returned when `cap` is too small.

Example: the following function normalises a command name to lowercase:

```
func normalise_command(cmd: Char[], out: Char[]): UInt64! {
 return try std::string::to_lower_into(cmd, out, 256);
}
```

## Number Formatting and Parsing

The `std::fmt` module formats numbers into caller-supplied buffers and
parses numbers out of strings. String interpolation covers most
formatting needs directly; `std::fmt` is for writing into fixed buffers,
for building protocols field by field, and for the parsing direction
that interpolation does not cover.

### Constants

Error 22 (`std::fmt::ERR_INVALID_NUMBER`) reports malformed input and
error 34 (`std::fmt::ERR_OUT_OF_RANGE`) reports a value that does not
fit the target type; `std::fmt::ERR_BUFFER_TOO_SMALL` (36) is returned
by the fixed-precision real formatter when the buffer is too small.

### Functions

#### `std::fmt::u64_dec`, `std::fmt::i64_dec`, `std::fmt::u64_hex`

Signatures: `std::fmt::u64_dec(value: UInt64, buf: Char[]) -> UInt64`,
`std::fmt::i64_dec(value: Int64, buf: Char[]) -> UInt64`,
`std::fmt::u64_hex(value: UInt64, buf: Char[]) -> UInt64`

Write the decimal or hexadecimal representation of `value` into `buf`
and return the digit count. The decimal buffers need 20 bytes, the
hexadecimal buffer 16; the hexadecimal form is lowercase and not
prefixed.

#### `std::fmt::real_to_str`, `std::fmt::real_to_str_fixed`

Signatures: `std::fmt::real_to_str(value: Real64, buf: Char[]) -> UInt64`,
`std::fmt::real_to_str_fixed(value: Real64, digits: UInt64, buf: Char[]) -> UInt64!`

`real_to_str` writes the shortest string that reads back to `value`;
the buffer needs 32 bytes. `real_to_str_fixed` writes exactly `digits`
digits after the decimal point, rounding half to even.

Errors: `std::fmt::ERR_BUFFER_TOO_SMALL` from `real_to_str_fixed`.

#### `std::fmt::parse_u64_dec`, `std::fmt::parse_i64_dec`, `std::fmt::parse_u64_hex`, `std::fmt::parse_real`

Signatures: `std::fmt::parse_u64_dec(src: Char[]) -> UInt64!`,
`std::fmt::parse_i64_dec(src: Char[]) -> Int64!`,
`std::fmt::parse_u64_hex(src: Char[]) -> UInt64!`,
`std::fmt::parse_real(src: Char[]) -> Real64!`

Parse a number from the start of `src`. Leading whitespace is not
skipped; trailing characters after a well-formed number are not an
error, so the caller can continue parsing where the number ends.

Errors: `std::fmt::ERR_INVALID_NUMBER` on malformed input;
`std::fmt::ERR_OUT_OF_RANGE` on overflow.

Example:

```
func render_seq(seq: UInt64, buf: Char[]): UInt64 {
 return std::fmt::u64_dec(seq, buf);
}
```

## Owning Strings

The `std::string` module operates on borrowed `Char[]` values and
caller-supplied buffers, which is the right discipline for embedded and
kernel code. The `std::str` module adds `Str`, the owning,
heap-allocated, growable string that ordinary programs build text with.
A `Str` owns its backing allocation, tracks its length, and always keeps
a zero byte at `data[len]`, so it interoperates with every function that
takes a `Char[]`: pass `s.data`, or the borrow returned by `as_chars`.

### Types

```
static Str = Struct {
 data: &Char,   // backing allocation
 len: UInt64,   // number of UTF-8 bytes
 cap: UInt64,   // payload capacity in bytes, excluding terminator
};
```

### Constants

`std::str::ERR_OUT_OF_MEMORY` (12) is returned when a growth allocation
fails; `std::str::ERR_OUT_OF_RANGE` (34) is returned when a view range
runs past the end of the string.

### Functions

#### `std::str::make`, `std::str::destroy`

Signatures: `std::str::make() -> Str`,
`std::str::destroy(s: &Str) -> Void`

`make` returns an empty string; `destroy` releases the backing
allocation and returns the value to the state produced by `make`. The
caller of `make` owns the value and schedules `destroy` with `defer`,
or with `errdefer` when a function hands the string out on success.

#### `std::str::from`, `std::str::clone`

Signatures: `std::str::from(s: Char[]) -> Str!`,
`std::str::clone(s: &Str) -> Str!`

Return a new owning string containing a copy of the content.

Errors: `std::str::ERR_OUT_OF_MEMORY`.

Ownership: the caller owns the result and releases it with `destroy`.

#### `std::str::reserve`

Signature: `std::str::reserve(s: &Str, want: UInt64) -> Void!`

Ensures the string can hold `want` UTF-8 bytes, doubling the backing
allocation until it suffices. The allocation reserves one additional
byte for the trailing zero; `cap` excludes that terminator. The length is unchanged.

Errors: `std::str::ERR_OUT_OF_MEMORY`.

#### `std::str::append`, `std::str::append_str`

Signatures: `std::str::append(s: &Str, suffix: Char[]) -> Void!`,
`std::str::append_str(s: &Str, suffix: &Str) -> Void!`

Append borrowed content to the end of the string. The terminator at
`data[len]` is maintained.

Errors: `std::str::ERR_OUT_OF_MEMORY`.

#### `std::str::concat`

Signature: `std::str::concat(a: Char[], b: Char[]) -> Str!`

Returns a new owning string containing `a` followed by `b`.

Ownership: the caller owns the result and releases it with `destroy`.

#### `std::str::compare`

Signature: `std::str::compare(a: &Str, b: &Str) -> Int32`

Compares two strings lexicographically over code units, treating bytes
as unsigned. Returns a negative value when `a` orders before `b`, zero
when they are equal, and a positive value otherwise.

#### `std::str::find_substring`

Signature: `std::str::find_substring(s: &Str, needle: Char[]) -> UInt64`

Returns the offset of the first occurrence of `needle`, or `s.len` when
it is absent.

#### `std::str::clear`

Signature: `std::str::clear(s: &Str) -> Void`

Sets `len` to zero while keeping the backing allocation for reuse. The string's zero terminator is restored at `data[0]`.

#### `std::str::as_chars`, `std::str::view`

Signatures: `std::str::as_chars(s: &Str) -> Char[]`,
`std::str::view(s: &Str, range) -> Char[]`

`as_chars` borrows the whole content as a NUL-terminated `Char[]`;
`view` borrows the window selected by `range`, also NUL-terminated at
the end of the window. Both alias the backing storage: the borrow stays
valid until the string is mutated or destroyed. No copy is made.

Errors: `std::str::ERR_OUT_OF_RANGE` from `view` when the range runs
past the end.

Example:

```
func summarise(path: Char[]): std::str::Str! {
 let blob = try std::file::read_file(path);
 defer free blob;

 let summary = std::str::from(std::bytes::as_str(&blob.data));
 errdefer std::str::destroy(&summary);
 try std::str::append(&summary, " (");
 try std::str::append(&summary, "bytes)");
 return summary;
}
```

Ownership: every value produced by `from`, `clone`, and `concat` must
reach exactly one `destroy`.

## Unicode Processing

The `std::unicode` module provides functions for validating, decoding, counting, and re-encoding UTF-8 text, together with ASCII case helpers. The functions treat strings as sequences of code units and report error 22 (`UNICODE_ERR_INVALID`) when the input is not valid UTF-8 in the place where validity is required.

### Types

```
static CodePoint = UInt32;
```

The `CodePoint` type is an alias for `UInt32`. It represents a Unicode code point, which is a value in the range 0 through 0x10FFFF, excluding the surrogate range 0xD800 through 0xDFFF.

### Functions

#### `std::unicode::validate`

Signature: `std::unicode::validate(s: Char[], len: UInt64) -> Bool`

Reports whether the `len` bytes at `s` form a valid UTF-8 sequence.

Parameters: `s` is the text; `len` is its length in bytes.

Returns: `true` when the text is valid UTF-8.

Example: the following function rejects invalid input before processing:

```
func checked(s: Char[], len: UInt64): Bool {
 return std::unicode::validate(s, len);
}
```

#### `std::unicode::is_ascii`

Signature: `std::unicode::is_ascii(s: Char[], len: UInt64) -> Bool`

Reports whether every byte of the `len` bytes at `s` is below `0x80`.

Parameters: `s` is the text; `len` is its length in bytes.

Returns: `true` when the text is pure ASCII.

Example: the following function selects the fast path for ASCII text:

```
func fast_path(s: Char[], len: UInt64): Bool {
 return std::unicode::is_ascii(s, len);
}
```

#### `std::unicode::decode_at`

Signature: `std::unicode::decode_at(s: Char[], len: UInt64, pos: &UInt64) -> CodePoint!`

Decodes one code point at the byte offset `*pos` and advances `*pos` past its bytes.

Parameters: `s` is the text; `len` is its length in bytes; `pos` points to the current byte offset, which is advanced on success.

Returns: the decoded code point.

Errors: `std::unicode::ERR_INVALID` (22) is returned when the bytes at `*pos` do not form a valid UTF-8 sequence or the offset reaches the end of the text.

Example: the following function counts code points without allocating:

```
func count_cps(s: Char[], len: UInt64): UInt64! {
 let n: UInt64 = 0;
 let pos: UInt64 = 0;
 while pos < len {
    try std::unicode::decode_at(s, len, &pos);
    n += 1;
 }
 return n;
}
```

#### `std::unicode::count`

Signature: `std::unicode::count(s: Char[], len: UInt64) -> UInt64!`

Returns the number of code points in the `len` bytes at `s`.

Parameters: `s` is the text; `len` is its length in bytes.

Returns: the code point count.

Errors: `std::unicode::ERR_INVALID` (22) is returned when the text is not valid UTF-8.

Example: the following function reports the character count of a string:

```
func char_count(s: Char[], len: UInt64): UInt64! {
 return try std::unicode::count(s, len);
}
```

#### `std::unicode::byte_offset_of`

Signature: `std::unicode::byte_offset_of(s: Char[], len: UInt64, n: UInt64) -> UInt64!`

Returns the byte offset of the `n`-th code point of the text.

Parameters: `s` is the text; `len` is its length in bytes; `n` is the code point index.

Returns: the byte offset at which the `n`-th code point starts, or `len` when the text holds fewer than `n` code points.

Errors: `std::unicode::ERR_INVALID` (22) is returned when the text is not valid UTF-8.

Example: the following function drops the first character of a string:

```
func drop_first(s: Char[], len: UInt64): UInt64! {
 return try std::unicode::byte_offset_of(s, len, 1);
}
```

#### `std::unicode::encode`

Signature: `std::unicode::encode(cp: CodePoint, out: Char[]) -> UInt64!`

Encodes one code point as UTF-8 into `out`, which must have room for up to four bytes.

Parameters: `cp` is the code point; `out` is the destination buffer.

Returns: the number of bytes written, from one to four.

Errors: `std::unicode::ERR_INVALID` (22) is returned when `cp` is outside the valid range or is a surrogate.

Example: the following function writes a check mark:

```
func write_check(out: Char[]): UInt64! {
 return try std::unicode::encode(0x2713, out);
}
```

#### `std::unicode::upper_ascii` and `std::unicode::lower_ascii`

Signatures: `std::unicode::upper_ascii(c: Char) -> Char` and `std::unicode::lower_ascii(c: Char) -> Char`

Map one ASCII letter between `a-z` and `A-Z`; every other byte is returned unchanged.

Parameters: `c` is the byte.

Returns: the mapped byte.

Example: the following function compares two ASCII letters case-insensitively:

```
func same_letter(a: Char, b: Char): Bool {
 return std::unicode::upper_ascii(a) == std::unicode::upper_ascii(b);
}
```

## Text Processing

The `std::string` module answers questions about a string; the
`std::text` module transforms strings: splitting, joining, trimming,
case conversion, replacement, repetition, and padding. Functions that
produce a new string return an owning `std::str::Str`; the caller
releases it with `std::str::destroy`, usually scheduled with `defer` or
`errdefer`. Case conversion is ASCII-only; Unicode-aware processing
belongs to `std::unicode`.

### Constants

`std::text::ERR_OUT_OF_MEMORY` (12) is returned when an allocation
fails. Errors produced by a caller-supplied callback function are propagated
unchanged by `split_each`.

### Functions

#### `std::text::split_each`

Signature: `std::text::split_each(s: Char[], sep: Char[], f: func(part: Char[]): Void!) -> Void!`

Calls `f` once for each part of `s` between occurrences of `sep`. An
empty `sep` is a compile-time error. No allocation is performed by the
split itself; a part is valid only for the duration of the call.

Errors: any error returned by `f` stops the iteration and propagates.

Example:

```
try std::text::split_each(line, ",", |field| {
 return try std::inout::println("field: {field}");
});
```

#### `std::text::split`

Signature: `std::text::split(s: Char[], sep: Char[]) -> std::array::Array!`

Returns an `std::array::Array` whose element type is `std::str::Str`, holding the parts. The element type is fixed to `Str` by this function; each element owns its own backing allocation.
Convenience over `split_each` for callers that need random access.

Errors: `std::text::ERR_OUT_OF_MEMORY`.

Ownership: the caller owns the array and every element; each element
must reach `std::str::destroy`, and the array must reach
`std::array::destroy`.

#### `std::text::join`

Signature: `std::text::join(parts: std::str::Str[..], sep: Char[]) -> Str!`

Returns a new owning string holding the parts separated by `sep`.

Errors: `std::text::ERR_OUT_OF_MEMORY`.

#### `std::text::trim`, `std::text::to_upper`, `std::text::to_lower`

Signatures: `std::text::trim(s: Char[]) -> Str!`,
`std::text::to_upper(s: Char[]) -> Str!`,
`std::text::to_lower(s: Char[]) -> Str!`

Return a new owning string with leading and trailing ASCII whitespace
removed (`trim`) or with ASCII case converted (`to_upper`, `to_lower`).
Bytes above `0x7F` pass through unchanged.

#### `std::text::replace`

Signature: `std::text::replace(s: Char[], from: Char[], to: Char[]) -> Str!`

Returns a new owning string in which every occurrence of `from` is
replaced by `to`.

#### `std::text::repeat`

Signature: `std::text::repeat(s: Char[], n: UInt64) -> Str!`

Returns a new owning string containing `s` concatenated `n` times.

#### `std::text::pad_start`, `std::text::pad_end`

Signatures: `std::text::pad_start(s: Char[], width: UInt64, fill: Char) -> Str!`,
`std::text::pad_end(s: Char[], width: UInt64, fill: Char) -> Str!`

Pad the string with `fill` on the left or the right until it is `width`
characters long; a string that is already at least `width` characters is
copied unchanged.

Example:

```
let clean = try std::text::trim(raw);
defer std::str::destroy(&clean);
let loud = try std::text::to_upper(std::str::as_chars(&clean));
defer std::str::destroy(&loud);
try std::inout::println("{std::str::as_chars(&loud)}");
```

## Binary-to-Text Encodings

The `std::encoding` module converts between raw bytes and the three
textual encodings a program meets most often: hexadecimal, Base64, and
percent-encoding for URL components. Each encode function returns an
owning `std::str::Str`; each decode function validates its input and
reports error 22 on malformed text. (URL parsing and serialisation live
in `std::url`; only the component encoding lives here.)

### Constants

`std::encoding::ERR_INVALID_DATA` (22) is returned by the decode
functions on malformed input; `std::encoding::ERR_OUT_OF_MEMORY` (12) is
returned when an allocation fails.

### Functions

#### `std::encoding::hex_encode`, `std::encoding::hex_decode`

Signatures: `std::encoding::hex_encode(data: &Void, len: UInt64) -> Str!`,
`std::encoding::hex_decode(text: Char[]) -> Str!`

`hex_encode` returns the lowercase hexadecimal representation of the
bytes. `hex_decode` accepts uppercase and lowercase input and returns
the decoded bytes as a string value whose `len` is the byte count.

Errors: `std::encoding::ERR_INVALID_DATA` when the input length is odd
or a character is not a hex digit; `std::encoding::ERR_OUT_OF_MEMORY`.

#### `std::encoding::base64_encode`, `std::encoding::base64_decode`

Signatures: `std::encoding::base64_encode(data: &Void, len: UInt64) -> Str!`,
`std::encoding::base64_decode(text: Char[]) -> Str!`

Standard Base64 (RFC 4648, with `+` and `/`). `base64_decode` requires
canonical padding and rejects characters outside the alphabet.

#### `std::encoding::url_encode`, `std::encoding::url_decode`

Signatures: `std::encoding::url_encode(component: Char[]) -> Str!`,
`std::encoding::url_decode(text: Char[]) -> Str!`

Percent-encoding for a single URL component: bytes outside the
unreserved set `A-Z a-z 0-9 - _ . ~` become `%` followed by two
uppercase hex digits. `url_decode` requires every `%` to be followed by
exactly two hex digits; any other byte, including `+`, is copied
unchanged.

Errors: `std::encoding::ERR_INVALID_DATA` from `url_decode` on a
malformed escape.

Ownership: the caller owns every returned string and releases it with
`std::str::destroy`.

Example:

```
let digest: Char[32];
std::sha256::sha256(data, len, &digest[0]);
let hex = try std::encoding::hex_encode(&digest[0], 32);
defer std::str::destroy(&hex);
try std::inout::println("sha256: {std::str::as_chars(&hex)}");
```

## Byte Buffers

The `std::bytes` module provides `ByteBuffer`, a growable array of
bytes whose backing store lives on the heap and doubles geometrically as
bytes are appended. Amortised doubling keeps every append constant-time
on average and bounds the wasted space to half of the capacity. Byte
buffers are the right container for byte-oriented input and output —
accumulating text, holding decoded content, feeding the JSON writer —
while typed element storage lives in `std::array`.

The buffer stores raw bytes because the type of the data is frequently
not known at compile time; typed containers can be built on top by
storing fixed-size records as byte ranges, or by using the typed
containers `std::array`, `std::list`, `std::map`, `std::set`,
`std::deque`, `std::ring`, and `std::pqueue`.

The length/capacity split follows the usual convention: `len` is the
number of bytes the caller may read, `cap` is the payload capacity of
the current heap allocation excluding the trailing zero byte, and
`len` never exceeds `cap`. The allocation contains at least `cap + 1`
bytes whenever the buffer is exposed as a NUL-terminated string. The
backing store is managed through the allocator's `realloc`, so growth
preserves the stored bytes without an explicit copy.

### Types

```
static ByteBuffer = Struct {
 data: &Char,
 len: UInt64,
 cap: UInt64,
};
```

### Constants

`std::bytes::ERR_OUT_OF_MEMORY` (12) is returned when a growth
allocation fails; `std::bytes::ERR_OUT_OF_RANGE` (34) is returned when
an index is not less than `len`.

### Functions

#### `std::bytes::make`

Signature: `std::bytes::make() -> ByteBuffer`

Returns an empty buffer. No memory is allocated until the first
operation that needs capacity.

#### `std::bytes::reserve`

Signature: `std::bytes::reserve(v: &ByteBuffer, want: UInt64) -> Void!`

Ensures the buffer can hold `want` bytes, growing the backing allocation
by doubling until it suffices. The length is unchanged; this is useful
when the final size is known in advance.

Errors: `std::bytes::ERR_OUT_OF_MEMORY`.

#### `std::bytes::push`

Signature: `std::bytes::push(v: &ByteBuffer, b: Char) -> Void!`

Appends a single byte to the end of the buffer.

#### `std::bytes::append`

Signature: `std::bytes::append(v: &ByteBuffer, src: Char[], count: UInt64) -> Void!`

Appends `count` bytes copied from `src`.

#### `std::bytes::append_string`

Signature: `std::bytes::append_string(v: &ByteBuffer, s: Char[]) -> Void!`

Appends the string `s`, terminator excluded, and stores a zero byte at
offset `len` without counting it, so the content doubles as a
NUL-terminated string of length `len`.

#### `std::bytes::byte_at`, `std::bytes::set_byte`

Signatures: `std::bytes::byte_at(v: &ByteBuffer, i: UInt64) -> Char`,
`std::bytes::set_byte(v: &ByteBuffer, i: UInt64, b: Char) -> Void!`

Read or overwrite the byte at index `i`.

Errors: `std::bytes::ERR_OUT_OF_RANGE` from `set_byte` when `i` is not
less than `len`.

#### `std::bytes::as_str`

Signature: `std::bytes::as_str(v: &ByteBuffer) -> Char[]`

Borrows the content of the buffer as a string. The buffer always keeps
a zero byte at offset `len`, so the returned pointer is a valid `Char[]`
of length `len` for as long as the buffer is not mutated or destroyed.
No copy is made.

#### `std::bytes::clear`

Signature: `std::bytes::clear(v: &ByteBuffer) -> Void`

Sets `len` to zero while keeping the backing allocation for reuse. Element bytes are discarded without invoking destructors.

#### `std::bytes::destroy`

Signature: `std::bytes::destroy(v: &ByteBuffer) -> Void`

Releases the backing allocation and returns the buffer to the state
produced by `make`. The buffer must not be used afterwards except
through `make`-style reassignment.

Example:

```
func build_report(parts: Char[3][]): std::bytes::ByteBuffer! {
 let v = std::bytes::make();
 errdefer std::bytes::destroy(&v);
 try std::bytes::reserve(&v, 4096);
 for parts |part| {
    try std::bytes::append_string(&v, part);
    try std::bytes::push(&v, '\n');
 }
 return v; // success: ownership passes to the caller
}
```

Ownership: the caller of `make` owns the buffer; `destroy` (usually
scheduled with `defer` or `errdefer`) releases it.

## Typed Arrays

### Generic Container Value Semantics

The typed containers (`std::array`, `std::list`, `std::deque`,
`std::pqueue`, and the value portion of `std::map`) store bytewise copies
of their element values. They preserve the bit representation and
alignment of `T`; they do not discover or invoke user-defined destructors.
A container may therefore hold scalars, plain records, and non-owning
handles directly. When an element contains an owning pointer or resource
handle, the program must define whether ownership is transferred into
the container or remains with an external owner, and must release each
resource exactly once before the corresponding value is discarded.


The typed-container API provides generic aggregate layout and runtime
field access. Use `std::array`, `std::list`, `std::deque`, `std::pqueue`,
`std::map`, fixed-size language arrays, and `std::bytes` according to the
ownership and lifetime rules of each container.

The `std::array` module provides `Array`, a growable array that stores
elements of any type inline, using the compile-time builtins `sizeof`
and `alignof` (see the language specification) for element size and
alignment. Elements are copied as their in-memory value representation;
container operations do not invoke destructors or recursively release
resources referenced by an element. Values containing owning pointers
or handles must therefore follow an explicit transfer/lifetime policy
chosen by the caller. `clear` and `destroy` release container storage,
not resources referenced by stored values; remove/pop operations return
the stored value representation to the caller. It
is the typed counterpart of `std::bytes` and the default choice for an
ordered collection whose size is not known at compile time. For
containers with specialised access patterns, see `std::list`,
`std::deque`, `std::ring`, and `std::pqueue`.

### Types

```
static Array = Struct {
 data: &Void,    // element storage
 len: UInt64,    // number of elements
 cap: UInt64,    // element capacity of the backing allocation
 elem_size: UInt64, // sizeof(T), recorded at creation
 elem_align: UInt64, // alignof(T), recorded at creation
};
```

Element `i` of an array created with element type `T` occupies the byte
range `[i * elem_size, (i + 1) * elem_size)` of `data`. The allocation
is aligned to `elem_align`, which is `alignof(T)`. The functions below
perform this arithmetic on the caller's behalf, and `slice` returns a
view that does.

### Constants

`std::array::ERR_OUT_OF_MEMORY` (12) is returned when a growth
allocation fails; `std::array::ERR_OUT_OF_RANGE` (34) is returned when
an index is out of bounds or an operation requires a non-empty array.

### Functions

#### `std::array::make`

Signature: `std::array::make(static T) -> Array`

Returns an empty array that stores elements of type `T`. No memory is
allocated until the first operation that needs capacity.

#### `std::array::from`

Signature: `std::array::from(static T, items: T[..]) -> Array!`

Returns a new array containing a copy of the elements of the view
`items`.

Errors: `std::array::ERR_OUT_OF_MEMORY`.

#### `std::array::reserve`

Signature: `std::array::reserve(v: &Array, want: UInt64) -> Void!`

Ensures the array can hold `want` elements, doubling the backing
allocation until it suffices. The length is unchanged.

Errors: `std::array::ERR_OUT_OF_MEMORY`.

#### `std::array::push`, `std::array::pop`

Signatures: `std::array::push(static T, v: &Array, item: T) -> Void!`,
`std::array::pop(static T, v: &Array) -> T!`

Append an element to the end, or remove and return the last element.

Errors: `std::array::ERR_OUT_OF_MEMORY` from `push`;
`std::array::ERR_OUT_OF_RANGE` from `pop` when the array is empty.

#### `std::array::get`, `std::array::set`

Signatures: `std::array::get(static T, v: &Array, i: UInt64) -> T!`,
`std::array::set(static T, v: &Array, i: UInt64, item: T) -> Void!`

Read or overwrite the element at index `i`.

Errors: `std::array::ERR_OUT_OF_RANGE` when `i` is not less than `len`.

#### `std::array::insert`, `std::array::remove`

Signatures:
`std::array::insert(static T, v: &Array, i: UInt64, item: T) -> Void!`,
`std::array::remove(static T, v: &Array, i: UInt64) -> T!`

Insert an element before index `i` (which may equal `len`), shifting the
following elements up, or remove and return the element at index `i`,
shifting the following elements down.

Errors: `std::array::ERR_OUT_OF_MEMORY` from `insert`;
`std::array::ERR_OUT_OF_RANGE` when `i` is greater than `len` (insert)
or not less than `len` (remove).

#### `std::array::slice`

Signature: `std::array::slice(static T, v: &Array, range) -> T[..]`

Returns a view of the selected live elements without copying. The view
is invalidated by any operation that mutates the array or its backing
allocation.

#### `std::array::clear`, `std::array::destroy`

Signatures: `std::array::clear(v: &Array) -> Void`,
`std::array::destroy(v: &Array) -> Void`

`clear` sets `len` to zero while keeping the backing allocation for
reuse; `destroy` releases the allocation and returns the array to the
state produced by `make`. Neither operation invokes element destructors.

Example:

```
func top_scores(raw: Int32[..]): std::array::Array! {
 let scores = std::array::make(Int32);
 errdefer std::array::destroy(&scores);
 for raw |s| {
    if s >= 0 {
      try std::array::push(Int32, &scores, s);
    }
 }
 if scores.len > 1 {
    std::algorithm::sort(Int32, std::array::slice(
      Int32, &scores, 0..(scores.len - 1)), |a, b| a > b);
 }
 return scores;
}
```

Ownership: the caller of `make` owns the array; `destroy` (usually
scheduled with `defer` or `errdefer`) releases it.

## Linked Lists

The `std::list` module provides `List`, a doubly linked list storing
elements of any type inline, using `sizeof` for the element size. A list
offers constant-time insertion and removal at both ends and at a known
position, at the cost of pointer chasing on indexed access; prefer
`std::array` or `std::deque` when indexed access dominates.

### Types

```
static List = Struct {
 head: &Node,
 tail: &Node,
 len: UInt64,
 elem_size: UInt64,
 elem_align: UInt64,
};

static Node = Struct {
 prev: &Node,
 next: &Node,
 data: &Void, // elem_size bytes holding one element
};
```

### Constants

`std::list::ERR_OUT_OF_MEMORY` (12) is returned when a node allocation
fails; `std::list::ERR_OUT_OF_RANGE` (34) is returned when an index is
not less than `len` or an operation requires a non-empty list.

### Functions

#### `std::list::make`, `std::list::make_in_pool`, `std::list::destroy`

Signatures: `std::list::make(static T) -> List`,
`std::list::make_in_pool(static T, pool: &std::heap::Pool) -> List`,
`std::list::destroy(l: &List) -> Void`

Return an empty list storing elements of type `T`. Nodes of a list made
with `make` come from the heap; nodes of a list made with `make_in_pool`
come from the supplied `std::heap::Pool`, which must outlive the list
and whose `block_size` and alignment must be sufficient for the node header and an aligned `T` payload. The list records both `sizeof(T)` and `alignof(T)`.

`destroy` releases every node (heap lists only) and returns the list to
the state produced by `make`.

#### `std::list::push_back`, `std::list::push_front`, `std::list::pop_back`, `std::list::pop_front`

Signatures:
`std::list::push_back(static T, l: &List, v: T) -> Void!`,
`std::list::push_front(static T, l: &List, v: T) -> Void!`,
`std::list::pop_back(static T, l: &List) -> T!`,
`std::list::pop_front(static T, l: &List) -> T!`

Append or remove elements at the two ends.

Errors: `std::list::ERR_OUT_OF_MEMORY` from the push functions;
`std::list::ERR_OUT_OF_RANGE` from the pop functions when the list is
empty.

#### `std::list::front`, `std::list::back`, `std::list::get`

Signatures: `std::list::front(static T, l: &List) -> T!`,
`std::list::back(static T, l: &List) -> T!`,
`std::list::get(static T, l: &List, i: UInt64) -> T!`

Read the first element, the last element, or the element at index `i`.
Indexed access walks the list and takes time proportional to `i`.

Errors: `std::list::ERR_OUT_OF_RANGE`.

#### `std::list::insert`, `std::list::remove_at`

Signatures:
`std::list::insert(static T, l: &List, i: UInt64, v: T) -> Void!`,
`std::list::remove_at(static T, l: &List, i: UInt64) -> T!`

Insert before index `i` (which may equal `len`) or remove and return
the element at index `i`.

Errors: `std::list::ERR_OUT_OF_MEMORY` from `insert`;
`std::list::ERR_OUT_OF_RANGE` when `i` is greater than `len` (insert)
or not less than `len` (remove_at).

#### `std::list::clear`

Signature: `std::list::clear(l: &List) -> Void`

Removes every node (heap lists only) and resets `len` to zero. Element bytes are discarded without invoking destructors.

Ownership: the caller of `make` owns the list; `destroy` (usually
scheduled with `defer` or `errdefer`) releases it.

Example:

```
let queue = std::list::make(Job);
defer std::list::destroy(&queue);
try std::list::push_back(Job, &queue, first);
while queue.len > 0 {
 let job = try std::list::pop_front(Job, &queue);
 run(job);
}
```

## Double-Ended Queues

The `std::deque` module provides `Deque`, a growable circular buffer
storing elements of any type inline. It offers constant-time push and
pop at both ends and constant-time indexed access, at slightly more
per-element overhead than `std::array`.

### Types

```
static Deque = Struct {
 data: &Void,
 head: UInt64,    // index of the first element
 len: UInt64,
 cap: UInt64,
 elem_size: UInt64,
 elem_align: UInt64,
};
```

### Constants

`std::deque::ERR_OUT_OF_MEMORY` (12) is returned when a growth
allocation fails; `std::deque::ERR_OUT_OF_RANGE` (34) is returned when
an index is not less than `len` or an operation requires a non-empty
deque.

### Functions

#### `std::deque::make`, `std::deque::destroy`

Signatures: `std::deque::make(static T) -> Deque`,
`std::deque::destroy(d: &Deque) -> Void`

#### `std::deque::push_back`, `std::deque::push_front`, `std::deque::pop_back`, `std::deque::pop_front`

Signatures mirror the `std::list` push and pop functions with `Deque`
in place of `List`. Errors: 12 from the push functions, 34 from the pop
functions when the deque is empty.

#### `std::deque::get`, `std::deque::set`

Signatures: `std::deque::get(static T, d: &Deque, i: UInt64) -> T!`,
`std::deque::set(static T, d: &Deque, i: UInt64, v: T) -> Void!`

Read or overwrite the element at index `i` in constant time.
Errors: `std::deque::ERR_OUT_OF_RANGE`.

#### `std::deque::slice`

Signature: `std::deque::slice(static T, d: &Deque, range) -> T[..]`

Returns a view of the selected live elements without copying. The view
is invalidated by any operation that mutates the deque.

#### `std::deque::clear`

Signature: `std::deque::clear(d: &Deque) -> Void`

Sets `len` to zero while keeping the backing allocation for reuse. Element bytes are discarded without invoking destructors.

Ownership: the caller of `make` owns the deque; `destroy` (usually
scheduled with `defer` or `errdefer`) releases it.

## Byte Rings

The `std::ring` module provides `ByteRing`, a fixed-capacity FIFO of
bytes. The ring never allocates after creation and never grows: a push
on a full ring reports `ERR_WOULD_BLOCK` rather than blocking, which
makes rings the right buffer between a producer and a consumer that must
not allocate — network and pipe plumbing, serial ports, audio buffers.

### Types

```
static ByteRing = Struct {
 data: &Char,
 read_pos: UInt64, // index of the oldest byte
 len: UInt64,
 cap: UInt64,
};
```

### Constants

`std::ring::ERR_OUT_OF_MEMORY` (12) is returned when the backing block
cannot be allocated; `std::ring::ERR_WOULD_BLOCK` (11) is returned by
the single-byte push and pop when the ring is full or empty;
`std::ring::ERR_INVALID_SIZE` (22) is returned when `cap` is zero.

### Functions

#### `std::ring::make`, `std::ring::destroy`

Signatures: `std::ring::make(cap: UInt64) -> ByteRing!`,
`std::ring::destroy(r: &ByteRing) -> Void`

Allocate or release the backing block.

#### `std::ring::push`, `std::ring::pop`

Signatures: `std::ring::push(r: &ByteRing, b: Char) -> Void!`,
`std::ring::pop(r: &ByteRing) -> Char!`

Push a byte onto the back or pop the oldest byte from the front.

Errors: `std::ring::ERR_WOULD_BLOCK` when the ring is full (`push`) or
empty (`pop`).

#### `std::ring::write`, `std::ring::read`

Signatures: `std::ring::write(r: &ByteRing, src: Char[], count: UInt64) -> UInt64`,
`std::ring::read(r: &ByteRing, dst: Char[], count: UInt64) -> UInt64`

Transfer up to `count` bytes (limited by the free or available space)
and return the number transferred. The bulk forms never fail.

#### `std::ring::clear`

Signature: `std::ring::clear(r: &ByteRing) -> Void`

Sets the ring length to zero; the capacity is unchanged.

Ownership: the caller of `make` owns the ring; `destroy` (usually
scheduled with `defer`) releases the backing block.

Example:

```
let buf = try std::ring::make(4096);
defer std::ring::destroy(&buf);
try std::ring::push(&buf, b);
let n = std::ring::read(&buf, scratch, 4096);
```

## Priority Queues

The `std::pqueue` module provides `PQueue`, a binary-heap priority queue
storing elements of any type inline, ordered by a caller-supplied
comparator. The comparator is fixed when the queue is made and
determines which end `pop` returns: with `less`, `pop` returns the
smallest element.

### Types

```
static PQueue = Struct {
 data: &Void,
 len: UInt64,
 cap: UInt64,
 elem_size: UInt64,
 elem_align: UInt64,
 less: func(a: &Void, b: &Void): Bool, // internal form of the comparator
};
```

### Constants

`std::pqueue::ERR_OUT_OF_MEMORY` (12) is returned when a growth
allocation fails; `std::pqueue::ERR_OUT_OF_RANGE` (34) is returned when
an operation requires a non-empty queue.

### Functions

#### `std::pqueue::make`, `std::pqueue::destroy`

Signatures:
`std::pqueue::make(static T, less: func(a: T, b: T): Bool) -> PQueue`,
`std::pqueue::destroy(q: &PQueue) -> Void`

Return an empty queue ordered by `less`.

#### `std::pqueue::push`

Signature: `std::pqueue::push(static T, q: &PQueue, v: T) -> Void!`

Insert an element. Errors: `std::pqueue::ERR_OUT_OF_MEMORY`.

#### `std::pqueue::peek`, `std::pqueue::pop`

Signatures: `std::pqueue::peek(static T, q: &PQueue) -> T!`,
`std::pqueue::pop(static T, q: &PQueue) -> T!`

Read, or remove and return, the extremal element according to the
comparator.

Errors: `std::pqueue::ERR_OUT_OF_RANGE` when the queue is empty.

#### `std::pqueue::clear`

Signature: `std::pqueue::clear(q: &PQueue) -> Void`

Sets `len` to zero while keeping the backing allocation for reuse. Element bytes are discarded without invoking destructors.

Ownership: the caller of `make` owns the queue; `destroy` (usually
scheduled with `defer` or `errdefer`) releases it.

Example:

```
let q = std::pqueue::make(Task, |a, b| a.deadline < b.deadline);
defer std::pqueue::destroy(&q);
try std::pqueue::push(Task, &q, task);
let next = try std::pqueue::pop(Task, &q);
```

## Hash Maps

The `std::map` module provides `Map`, a hash table keyed by strings with
values of any type stored inline. Lookups average constant time. The map
grows geometrically; iteration order is unspecified.

### Types

```
static Map = Struct {
 buckets: &Void,
 count: UInt64,
 capacity: UInt64,
 value_size: UInt64,
 value_align: UInt64,
};
```

### Constants

`std::map::ERR_OUT_OF_MEMORY` (12) is returned when a growth allocation
fails; `std::map::ERR_KEY_NOT_FOUND` (2) is returned by `get` when the
key is absent.

### Functions

#### `std::map::make`, `std::map::destroy`

Signatures: `std::map::make(static T) -> Map`,
`std::map::destroy(m: &Map) -> Void`

#### `std::map::put`

Signature: `std::map::put(static T, m: &Map, key: Char[], value: T) -> Void!`

Inserts the key and a bytewise copy of `value`, replacing the stored value when the key is already present. Keys are copied into the map. The map does not run a destructor for replaced or removed values; callers manage resources referenced by values explicitly.

Errors: `std::map::ERR_OUT_OF_MEMORY`.

#### `std::map::get`, `std::map::has`, `std::map::remove`

Signatures: `std::map::get(static T, m: &Map, key: Char[]) -> T!`,
`std::map::has(m: &Map, key: Char[]) -> Bool`,
`std::map::remove(m: &Map, key: Char[]) -> Bool`

`get` returns the value for `key`; `has` reports whether the key is
present; `remove` removes the entry and reports whether it was present.

Errors: `std::map::ERR_KEY_NOT_FOUND` from `get`.

#### `std::map::each`

Signature: `std::map::each(static T, m: &Map, f: func(key: Char[], value: T): Void!) -> Void!`

Calls `f` once for every entry. Iteration order is unspecified and may
change after any mutation. Errors returned by `f` stop the iteration and
propagate.

#### `std::map::clear`

Signature: `std::map::clear(m: &Map) -> Void`

Removes every entry and releases its copied key storage while keeping the bucket array for reuse. Stored values are bytewise records; no value destructor is invoked.

Ownership: the caller of `make` owns the map; `destroy` releases the map and its key/bucket storage. Values stored in the map are shallow bytewise copies, and resources referenced by those values remain the caller's responsibility.

Example:

```
let cfg = std::map::make(Int32);
defer std::map::destroy(&cfg);
try std::map::put(Int32, &cfg, "port", 8080);
let port = try std::map::get(Int32, &cfg, "port");
try std::map::each(Int32, &cfg, |key, value| {
 return try std::inout::println("{key}={value}");
});
```

## Hash Sets

The `std::set` module provides `Set`, a hash table of strings with the
same performance profile as `std::map`. Use it for membership tests and
deduplication.

### Constants

`std::set::ERR_OUT_OF_MEMORY` (12) is returned when a growth allocation
fails.

### Functions

#### `std::set::make`, `std::set::destroy`

Signatures: `std::set::make() -> Set`,
`std::set::destroy(s: &Set) -> Void`

#### `std::set::add`, `std::set::has`, `std::set::remove`

Signatures: `std::set::add(s: &Set, key: Char[]) -> Bool`,
`std::set::has(s: &Set, key: Char[]) -> Bool`,
`std::set::remove(s: &Set, key: Char[]) -> Bool`

`add` inserts the key and returns `true` when it was not already
present; keys are copied into the set. `remove` returns `true` when the
key was present.

Errors: `std::set::ERR_OUT_OF_MEMORY` from `add`.

#### `std::set::each`, `std::set::clear`

Signatures:
`std::set::each(s: &Set, f: func(key: Char[]): Void!) -> Void!`,
`std::set::clear(s: &Set) -> Void`

`each` visits every element; iteration order is unspecified. `clear`
removes every element while keeping the bucket array for reuse.

Ownership: the caller of `make` owns the set; `destroy` (usually
scheduled with `defer`) releases it.

## Generic Algorithms

The `std::algorithm` module provides the classical elementwise
algorithms over views. Every function works on any element type via a
compile-time parameter and operates in place on the view — no
allocation, no copying of the container. Ordering-sensitive algorithms
take a comparator function; the natural order for the arithmetic types is
available as `std::algorithm::natural` when natural ordering is sufficient.

### Constants

`std::algorithm::ERR_OUT_OF_RANGE` (34) is returned when a position
argument exceeds the length of the view.

### Functions

#### `std::algorithm::sort`

Signature: `std::algorithm::sort(static T, items: T[..], less: func(a: T, b: T): Bool) -> Void`

Sorts the view in place so that adjacent elements satisfy `less`.
Introspective quicksort: O(n log n) comparisons, in-place.

#### `std::algorithm::binary_search`

Signature: `std::algorithm::binary_search(static T, items: T[..], key: T, less: func(a: T, b: T): Bool) -> UInt64`

Returns the index of an element equal to `key` when the view is sorted
according to `less`, or `items.len` when it is absent.

#### `std::algorithm::find`

Signature: `std::algorithm::find(static T, items: T[..], pred: func(v: T): Bool) -> UInt64`

Returns the index of the first element for which `pred` is true, or
`items.len` when there is none.

#### `std::algorithm::count`

Signature: `std::algorithm::count(static T, items: T[..], pred: func(v: T): Bool) -> UInt64`

Returns the number of elements for which `pred` is true.

#### `std::algorithm::reverse`, `std::algorithm::fill`

Signatures: `std::algorithm::reverse(static T, items: T[..]) -> Void`,
`std::algorithm::fill(static T, items: T[..], v: T) -> Void`

Reverse the order of the elements, or overwrite every element with `v`.

#### `std::algorithm::rotate`

Signature: `std::algorithm::rotate(static T, items: T[..], mid: UInt64) -> Void!`

Rotates the view so that the element at `mid` becomes the first
element. Errors: `std::algorithm::ERR_OUT_OF_RANGE` when `mid` exceeds
`items.len`.

#### `std::algorithm::partition`

Signature: `std::algorithm::partition(static T, items: T[..], pred: func(v: T): Bool) -> UInt64`

Reorders the view so that elements satisfying `pred` precede the
others, preserving no particular order within each side, and returns the
index of the first element of the second side.

Example:

```
let v = try std::array::from(Int32, raw);
defer std::array::destroy(&v);
std::algorithm::sort(
 Int32, std::array::slice(Int32, &v, 0..(v.len - 1)), |a, b| a < b);
let at = std::algorithm::binary_search(
 Int32, std::array::slice(Int32, &v, 0..(v.len - 1)), 42, |a, b| a < b);
```

## Low-Level Memory Operations

The `std::mem` module provides functions for copying, moving, setting, and comparing blocks of memory. These operations are typically used when the type of the data is not known at compile time or when the data must be interpreted as a sequence of bytes. The functions take raw addresses and byte counts, operate one byte at a time, and never touch memory outside the requested range. The byte-at-a-time loop is deliberate: it is correct for every alignment and overlap pattern and gives a clean baseline that hotter paths can replace later without changing call sites.

The functions in this module complement the built-in allocation operators and the `std::heap` module, which manage memory but do not provide ways to manipulate its contents.

### Functions

#### `std::mem::copy`

Signature: `std::mem::copy(dest: &Void, src: &Void, count: UInt64) -> Void`

Copies exactly `count` bytes from `src` to `dest`. The regions must not overlap; use `std::mem::move` for overlapping regions.

Parameters: `dest` is the destination pointer; `src` is the source pointer; `count` is the number of bytes.

Example: the following function copies one buffer into another:

```
func copy_buffer(dst: &Void, src: &Void, n: UInt64): Void {
 std::mem::copy(dst, src, n);
}
```

#### `std::mem::move`

Signature: `std::mem::move(dest: &Void, src: &Void, count: UInt64) -> Void`

Copies exactly `count` bytes from `src` to `dest`, behaving as if the source were first copied to a temporary buffer. Overlapping regions are handled by choosing the transfer direction from the relative order of the two addresses.

Parameters: `dest` is the destination pointer; `src` is the source pointer; `count` is the number of bytes.

Example: the following function shifts a buffer one byte to the right:

```
func shift_right(buf: &Void, n: UInt64): Void {
 std::mem::move((buf as UInt64 + 1) as &Void, buf, n);
}
```

#### `std::mem::set`

Signature: `std::mem::set(ptr: &Void, value: Char, count: UInt64) -> Void`

Writes `value` into every byte of the `count`-byte region at `ptr`.

Parameters: `ptr` is the starting pointer; `value` is the byte value; `count` is the number of bytes.

Example: the following function fills a buffer with a marker byte:

```
func mark(buf: &Void, n: UInt64): Void {
 std::mem::set(buf, 0xAA, n);
}
```

#### `std::mem::zero`

Signature: `std::mem::zero(ptr: &Void, count: UInt64) -> Void`

Writes zero into every byte of the `count`-byte region at `ptr`. This is `std::mem::set` with a zero value, factored out because zeroing is by far the most common fill.

Parameters: `ptr` is the starting pointer; `count` is the number of bytes.

Example: the following function zeroes a buffer:

```
func zero_buffer(buf: &Void, n: UInt64): Void {
 std::mem::zero(buf, n);
}
```

#### `std::mem::compare`

Signature: `std::mem::compare(a: &Void, b: &Void, count: UInt64) -> Int32`

Compares the two `count`-byte regions lexicographically bytewise. Returns a negative value if `a` orders before `b`, zero if the regions are byte-identical, and a positive value otherwise.

Parameters: `a` and `b` are the pointers to the memory blocks; `count` is the number of bytes.

Example: the following function checks whether two buffers are identical:

```
func buffers_equal(a: &Void, b: &Void, n: UInt64): Bool {
 return std::mem::compare(a, b, n) == 0;
}
```

#### `std::mem::find_byte`

Signature: `std::mem::find_byte(ptr: &Void, value: Char, count: UInt64) -> UInt64`

Returns the offset of the first byte within the `count`-byte region at `ptr` that equals `value`, or `count` when no byte matches.

Parameters: `ptr` is the region; `value` is the byte; `count` is the region size.

Example: the following function finds the first zero byte in a block:

```
func first_zero(buf: &Void, n: UInt64): UInt64 {
 return std::mem::find_byte(buf, 0, n);
}
```

## Heap Allocation

The `std::heap` module exposes the allocator behind the language's `alloc`
and `free` operators, together with region and pool allocators for bulk
work. A program that simply needs memory uses the `alloc` and `free`
operators; `std::heap` is for programs that must control the allocation
strategy — an arena for per-request scratch memory, a pool for
fixed-size objects — or that must resize an allocation in place.

### Types

```
static HeapStats = Struct {
 allocated: UInt64, // bytes handed out
 blocks: UInt64,    // number of live allocations
};

static Arena = Struct {
 base: &Void,     // backing block
 offset: UInt64,    // bump pointer, in bytes
 cap: UInt64,     // capacity of the backing block
};

static Pool = Struct {
 block_size: UInt64, // size of one element block
 capacity: UInt64,   // number of blocks
 free_list: &Void,   // head of the free list
 storage: &Void,    // backing block holding all blocks
};
```

### Constants

`std::heap::ERR_OUT_OF_MEMORY` (12) is returned when an allocation
cannot be satisfied. `std::heap::ERR_INVALID_SIZE` (22) is returned when
a requested size is zero or cannot be represented.

### Functions

#### `std::heap::alloc`

Signature: `std::heap::alloc(size: UInt64) -> &Void!`

Allocates `size` bytes of uninitialised heap memory.

Parameters: `size` is the requested size in bytes; it must be greater
than zero.

Returns: a pointer to the new block on success.

Errors: `std::heap::ERR_OUT_OF_MEMORY`, `std::heap::ERR_INVALID_SIZE`.

Ownership: the caller owns the block and releases it with
`std::heap::free`. The `alloc` operator of the language calls this
allocator and reports failure as the zero pointer instead of an error.

#### `std::heap::free`

Signature: `std::heap::free(ptr: &Void) -> Void`

Releases a block obtained from `std::heap::alloc` or
`std::heap::realloc`. Freeing the zero pointer is a no-op; freeing any
other pointer not obtained from this allocator is undefined behaviour.

#### `std::heap::realloc`

Signature: `std::heap::realloc(ptr: &Void, size: UInt64) -> &Void!`

Resizes a live block, preserving its content up to the smaller of the
old and new sizes.

Returns: a pointer to the resized block, which may differ from `ptr`.

Errors: `std::heap::ERR_OUT_OF_MEMORY`; on error the original block
remains live and unchanged.

#### `std::heap::usable_size`

Signature: `std::heap::usable_size(ptr: &Void) -> UInt64`

Returns the actual size of a live block, which may exceed the size that
was requested.

#### `std::heap::stats`

Signature: `std::heap::stats() -> HeapStats`

Returns the current allocation counters of the heap.

#### `std::heap::arena_create`, `std::heap::arena_alloc`, `std::heap::arena_reset`, `std::heap::arena_destroy`

Signatures:
`std::heap::arena_create(cap: UInt64) -> Arena!`
`std::heap::arena_alloc(a: &Arena, size: UInt64) -> &Void!`
`std::heap::arena_reset(a: &Arena) -> Void`
`std::heap::arena_destroy(a: &Arena) -> Void`

An arena is a bump allocator over one backing block. `arena_create`
allocates the backing block; `arena_alloc` bumps the offset and returns
the new region; `arena_reset` marks the whole arena free at once without
releasing the backing block; `arena_destroy` releases the block.
Individual arena allocations are not freed.

Errors: `std::heap::ERR_OUT_OF_MEMORY` when the backing block cannot be
allocated or the arena is full.

Ownership: the arena owns its backing block; `arena_destroy` must be
scheduled with `defer` immediately after a successful
`arena_create`.

#### `std::heap::pool_create`, `std::heap::pool_alloc`, `std::heap::pool_free`, `std::heap::pool_destroy`

Signatures:
`std::heap::pool_create(block_size: UInt64, capacity: UInt64) -> Pool!`
`std::heap::pool_alloc(p: &Pool) -> &Void!`
`std::heap::pool_free(p: &Pool, ptr: &Void) -> Void`
`std::heap::pool_destroy(p: &Pool) -> Void`

A pool is a free-list allocator of fixed-size blocks with constant-time
allocation and release. It suits linked structures whose nodes never
change size; the `std::list` module builds its nodes over a pool when
one is supplied.

Errors: `std::heap::ERR_OUT_OF_MEMORY` when the pool is exhausted.

Ownership: the pool owns its backing block; `pool_destroy` must be
scheduled with `defer` immediately after a successful `pool_create`.

Example:

```
func process_requests(): Void! {
 let arena = try std::heap::arena_create(1 << 20);
 defer std::heap::arena_destroy(&arena);

 for 0..99 |i| {
    let scratch = try std::heap::arena_alloc(&arena, 4096);
    handle(scratch);
    std::heap::arena_reset(&arena);
 }
}
```

## Bit Manipulation

The `std::bit` module provides the bit-level operations that are awkward
to express with shifts and masks: counting, scanning, rotating, byte
reversal, and single-bit access. Every function takes a compile-time
unsigned integer type and operates on values of that type. Positions are
zero-based: bit 0 is the least significant bit.

### Constants

`std::bit::ERR_OUT_OF_RANGE` (34) is returned by the single-bit
functions when the bit position is not less than the width of the type.

### Functions

#### `std::bit::popcount`

Signature: `std::bit::popcount(static T, v: T) -> UInt64`

Returns the number of set bits in `v`.

#### `std::bit::clz`, `std::bit::ctz`

Signatures: `std::bit::clz(static T, v: T) -> UInt64`,
`std::bit::ctz(static T, v: T) -> UInt64`

Return the number of leading (most significant) or trailing (least
significant) zero bits. When `v` is zero the width of `T` is returned.

#### `std::bit::rotl`, `std::bit::rotr`

Signatures: `std::bit::rotl(static T, v: T, n: UInt64) -> T`,
`std::bit::rotr(static T, v: T, n: UInt64) -> T`

Rotate the bits of `v` left or right. `n` may exceed the width of `T`;
it is reduced modulo the width, so a rotation by the width is the
identity.

#### `std::bit::byteswap`

Signature: `std::bit::byteswap(static T, v: T) -> T`

Reverses the order of the bytes of `v`; `T` must be `UInt16`, `UInt32`,
`UInt64`, `UInt128`, or `UInt256`.

#### `std::bit::test`, `std::bit::set`, `std::bit::clear`, `std::bit::flip`

Signatures: `std::bit::test(static T, v: T, i: UInt64) -> Bool!`,
`std::bit::set(static T, v: T, i: UInt64) -> T!`,
`std::bit::clear(static T, v: T, i: UInt64) -> T!`,
`std::bit::flip(static T, v: T, i: UInt64) -> T!`

Test, set, clear, or flip bit `i` of `v`.

Errors: `std::bit::ERR_OUT_OF_RANGE` when `i` is not less than the width
of `T`.

Example:

```
static READY: UInt64 = 0x01;
static ERROR: UInt64 = 0x02;

let flags: UInt64 = 0;
flags = try std::bit::set(UInt64, flags, 0);
if try std::bit::test(UInt64, flags, 0) {
 // ready
}
```

## Mathematics

The mathematics modules provide constants, scalar functions, and the value types of vector and matrix algebra: `std::math` holds the constants and the scalar functions, `std::trig` holds the trigonometric functions, `std::linalg` holds the vectors, the 3x3 and 4x4 matrices, and the quaternions, and `std::camera` holds the projection and view matrices built on top of them. The types are used primarily in graphics and simulation, and the scalar functions are used throughout numerical code.

### Constants

| Constant | Value |
| --- | --- |
| `std::math::PI` | 3.141592653589793 |
| `std::math::E` | 2.718281828459045 |
| `std::math::SQRT2` | 1.4142135623730951 |

### Error Codes

`std::math::MATH_ERR_DOMAIN` (22) reports a domain violation such as a negative square root argument. `std::math::MATH_ERR_RANGE` (34) reports a result that overflows the range of `Real64`.

### Types

The following types are defined by `std::linalg`. Each type is a struct that groups the fields that constitute the value.

```
static Vec2 = Struct {
 x: Real64,
 y: Real64,
};

static Vec3 = Struct {
 x: Real64,
 y: Real64,
 z: Real64,
};

static Vec4 = Struct {
 x: Real64,
 y: Real64,
 z: Real64,
 w: Real64,
};

static Mat3 = Struct {
 m0: Vec3,
 m1: Vec3,
 m2: Vec3,
};

static Mat4 = Struct {
 m0: Vec4,
 m1: Vec4,
 m2: Vec4,
 m3: Vec4,
};

static Quat = Struct {
 x: Real64,
 y: Real64,
 z: Real64,
 w: Real64,
};
```

A `Mat4` stores four `Vec4` rows (`m0` is the first row) in row-major order; vectors are columns in products, so `m * v` pairs each row of `m` with `v` through the dot product.

### Scalar Functions (`std::math`)

Each function returns a `Real64` unless otherwise noted. Functions that can fail return a `Real64!`.

- `std::math::bits_of(v: Real64) -> UInt64` reinterprets the 64 bits of a `Real64` as a `UInt64` without changing a single bit.
- `std::math::real_of(bits: UInt64) -> Real64` reinterprets a `UInt64` bit pattern as a `Real64` without changing a single bit.
- `std::math::abs(x: Real64) -> Real64` returns the absolute value of `x`, computed by clearing the sign bit of the IEEE 754 representation.
- `std::math::fabs(x: Real64) -> Real64` returns the absolute value of `x`, equivalent to `abs`.
- `std::math::min(a: Real64, b: Real64) -> Real64` returns the smaller of `a` and `b`.
- `std::math::max(a: Real64, b: Real64) -> Real64` returns the larger of `a` and `b`.
- `std::math::floor(x: Real64) -> Real64` returns the largest integer not greater than `x`.
- `std::math::ceil(x: Real64) -> Real64` returns the smallest integer not less than `x`.
- `std::math::round(x: Real64) -> Real64` returns the nearest integer to `x`, rounding halves away from zero.
- `std::math::sqrt(x: Real64) -> Real64!` returns the square root of `x`, computed by Newton-Raphson iteration, or `MATH_ERR_DOMAIN` if `x` is not positive.
- `std::math::exp(x: Real64) -> Real64!` returns `e` raised to the power `x`, or `MATH_ERR_RANGE` if the result overflows.
- `std::math::ln(x: Real64) -> Real64!` returns the natural logarithm of `x`, or `MATH_ERR_DOMAIN` if `x` is not positive.
- `std::math::log(x: Real64) -> Real64!` returns the natural logarithm of `x`; an alias of `ln`.
- `std::math::log10(x: Real64) -> Real64!` returns the base-10 logarithm of `x`, or `MATH_ERR_DOMAIN` if `x` is not positive.
- `std::math::pow(base: Real64, exp: Real64) -> Real64!` returns `base` raised to the power `exp`, or an error propagated from `ln` and `exp`.
- `std::math::hypot(x: Real64, y: Real64) -> Real64` returns `sqrt(x*x + y*y)` without undue overflow.
- `std::math::clamp(x: Real64, lo: Real64, hi: Real64) -> Real64` returns `x` bounded to the interval [`lo`, `hi`].
- `std::math::lerp(a: Real64, b: Real64, t: Real64) -> Real64` returns the linear interpolation `a + t * (b - a)`.
- `std::math::sign(x: Real64) -> Real64` returns -1.0, 0.0, or 1.0 according to the sign of `x`.

Example: the following function computes the distance between two points using the scalar functions:

```
func distance_2d(ax: Real64, ay: Real64, bx: Real64, by: Real64): Real64! {
 let dx = ax - bx;
 let dy = ay - by;
 return try std::math::sqrt(dx * dx + dy * dy);
}
```

### Trigonometric Functions (`std::trig`)

All angles are radians unless a function name says otherwise.

- `std::trig::sin(x: Real64) -> Real64` returns the sine of `x`.
- `std::trig::cos(x: Real64) -> Real64` returns the cosine of `x`.
- `std::trig::tan(x: Real64) -> Real64` returns the tangent of `x`, the quotient of `sin` and `cos`.
- `std::trig::atan(x: Real64) -> Real64` returns the arc tangent of `x`.
- `std::trig::atan2(y: Real64, x: Real64) -> Real64` returns the arc tangent of `y / x`, using the signs of the arguments to determine the quadrant.
- `std::trig::asin(x: Real64) -> Real64!` returns the arc sine of `x`, or `MATH_ERR_DOMAIN` if `x` is outside the range [-1, 1].
- `std::trig::acos(x: Real64) -> Real64!` returns the arc cosine of `x`, or `MATH_ERR_DOMAIN` if `x` is outside the range [-1, 1].
- `std::trig::sinh(x: Real64) -> Real64` returns the hyperbolic sine of `x`.
- `std::trig::cosh(x: Real64) -> Real64` returns the hyperbolic cosine of `x`.
- `std::trig::tanh(x: Real64) -> Real64` returns the hyperbolic tangent of `x`.
- `std::trig::deg_to_rad(deg: Real64) -> Real64` converts degrees to radians.
- `std::trig::rad_to_deg(rad: Real64) -> Real64` converts radians to degrees.

### Vector Constructors and Operations (`std::linalg`)

- `std::linalg::vec2(x: Real64, y: Real64) -> Vec2` constructs a `Vec2`.
- `std::linalg::vec3(x: Real64, y: Real64, z: Real64) -> Vec3` constructs a `Vec3`.
- `std::linalg::vec4(x: Real64, y: Real64, z: Real64, w: Real64) -> Vec4` constructs a `Vec4`.
- `std::linalg::dot2(a: Vec2, b: Vec2) -> Real64`, `std::linalg::dot3(a: Vec3, b: Vec3) -> Real64`, and `std::linalg::dot4(a: Vec4, b: Vec4) -> Real64` return the dot products.
- `std::linalg::cross3(a: Vec3, b: Vec3) -> Vec3` returns the cross product of two three-dimensional vectors.
- `std::linalg::length2(v: Vec2) -> Real64`, `std::linalg::length3(v: Vec3) -> Real64`, and `std::linalg::length4(v: Vec4) -> Real64` return the Euclidean lengths.
- `std::linalg::distance(a: Vec3, b: Vec3) -> Real64` returns the distance between two points.
- `std::linalg::normalize2(v: Vec2) -> Vec2`, `std::linalg::normalize3(v: Vec3) -> Vec3`, and `std::linalg::normalize4(v: Vec4) -> Vec4` return unit-length copies; the zero vector normalizes to itself.

### Matrix Constructors (`std::linalg`)

- `std::linalg::mat4(...16 scalars...) -> Mat4` constructs a `Mat4` from sixteen scalar components in row-major order.
- `std::linalg::identity_mat4() -> Mat4` returns the four-by-four identity matrix.
- `std::linalg::mat3(...9 scalars...) -> Mat3` constructs a `Mat3` from nine scalar components in row-major order.
- `std::linalg::identity_mat3() -> Mat3` returns the three-by-three identity matrix.

### Matrix Operations (`std::linalg`)

- `std::linalg::mul4(a: Mat4, b: Mat4) -> Mat4` returns the matrix product of `a` and `b`, applying `a` after `b` to a column vector.
- `std::linalg::mul_vec4(m: Mat4, v: Vec4) -> Vec4` multiplies the matrix `m` by the vector `v`.
- `std::linalg::mul_vec3(m: Mat4, v: Vec3) -> Vec3` multiplies the matrix `m` by the vector `v`, promoting `v` with a `w` component of one.
- `std::linalg::transpose4(m: Mat4) -> Mat4` returns the transpose of `m`.
- `std::linalg::inverse4(m: Mat4) -> Mat4!` returns the inverse of `m`, computed by Gauss-Jordan elimination with partial pivoting, or error 22 if `m` is singular.
- `std::linalg::mul3(a: Mat3, b: Mat3) -> Mat3`, `std::linalg::mul_vec3(m: Mat3, v: Vec3) -> Vec3`, `std::linalg::transpose3(m: Mat3) -> Mat3`, and `std::linalg::inverse3(m: Mat3) -> Mat3!` provide the same operations for `Mat3`.

### Transformation Functions (`std::linalg`)

Angles are degrees.

- `std::linalg::translate4(x: Real64, y: Real64, z: Real64) -> Mat4` returns a matrix that translates by the given offsets.
- `std::linalg::rotate4(angle: Real64, axis: Vec3) -> Mat4` returns a matrix that rotates by `angle` degrees about `axis`, normalized internally; a zero axis yields the identity.
- `std::linalg::scale4(x: Real64, y: Real64, z: Real64) -> Mat4` returns a matrix that scales by the given factors.

### Quaternion Functions (`std::linalg`)

Angles are degrees.

- `std::linalg::quat(x: Real64, y: Real64, z: Real64, w: Real64) -> Quat` constructs a quaternion.
- `std::linalg::quat_identity() -> Quat` returns the identity quaternion.
- `std::linalg::quat_from_axis_angle(angle: Real64, axis: Vec3) -> Quat` constructs a quaternion representing a rotation by `angle` degrees about `axis`.
- `std::linalg::quat_normalize(q: Quat) -> Quat` returns a unit quaternion; the zero quaternion normalizes to the identity.
- `std::linalg::quat_mul(a: Quat, b: Quat) -> Quat` returns the Hamilton product, applying `a` after `b`.
- `std::linalg::quat_to_mat4(q: Quat) -> Mat4` converts a quaternion to a rotation matrix, normalizing it first.

Example: the following function builds a model-view-projection matrix for a rotating object:

```
func make_mvp(angle: Real64, width: Real64, height: Real64): std::linalg::Mat4 {
 let model = std::linalg::rotate4(angle, std::linalg::vec3(0.0, 1.0, 0.0));
 let view = std::linalg::look_at(
    std::linalg::vec3(0.0, 0.0, 5.0),
    std::linalg::vec3(0.0, 0.0, 0.0),
    std::linalg::vec3(0.0, 1.0, 0.0)
 );
 let proj = std::linalg::perspective(
    45.0,
    width / height,
    0.1,
    100.0
 );
 return std::linalg::mul4(proj, std::linalg::mul4(view, model));
}
```

## Camera and Projection

The `std::camera` module builds the viewing half of a model-view
projection stack: perspective and orthographic projection matrices and
the look-at view matrix. The types are `std::linalg::Mat4` and
`std::linalg::Vec3`; this module depends on `std::linalg` and adds the
camera domain to it. All angles are degrees.

### Functions

#### `std::camera::perspective`

Signature: `std::camera::perspective(fov: Real64, aspect: Real64, near: Real64, far: Real64) -> Mat4`

Returns a perspective projection matrix. The vertical field of view
`fov` is in degrees; `aspect` is the viewport's width divided by its
height; `near` and `far` are the distances to the clipping planes.

#### `std::camera::orthographic`

Signature: `std::camera::orthographic(left: Real64, right: Real64, bottom: Real64, top: Real64, near: Real64, far: Real64) -> Mat4`

Returns an orthographic projection matrix covering the given volume.

#### `std::camera::look_at`

Signature: `std::camera::look_at(eye: Vec3, center: Vec3, up: Vec3) -> Mat4`

Returns a view matrix that looks from `eye` toward `center` with the
given up vector.

Example:

```
func make_mvp(angle: Real64, width: Real64, height: Real64): std::linalg::Mat4 {
 let model = std::linalg::rotate4(
    angle, std::linalg::vec3(0.0, 1.0, 0.0));
 let view = std::camera::look_at(
    std::linalg::vec3(0.0, 0.0, 5.0),
    std::linalg::vec3(0.0, 0.0, 0.0),
    std::linalg::vec3(0.0, 1.0, 0.0));
 let proj = std::camera::perspective(45.0, width / height, 0.1, 100.0);
 return std::linalg::mul4(proj, std::linalg::mul4(view, model));
}
```

## Pseudorandom Number Generation

The `std::random` module provides a pseudorandom number generator built on a 64-bit state word. The generator is a module-level value: the `seed` and `auto_seed` functions initialise it, and the drawing functions read and advance the shared state. Programs that need independent sequences seed and draw in a controlled order, or use separate generator state of their own on top of the drawing primitives.

### Functions

#### `std::random::seed`

Signature: `std::random::seed(s: UInt64) -> Void`

Initialises the generator with the given seed. Two runs seeded with the same value produce the same sequence of draws.

Parameters: `s` is the initial seed.

Example: the following function creates a deterministic sequence:

```
func deterministic(): Void {
 std::random::seed(12345);
}
```

#### `std::random::auto_seed`

Signature: `std::random::auto_seed() -> Void!`

Initialises the generator from system entropy, so the sequence differs on each invocation. May block if the system has not gathered entropy yet.

Parameters: none.

Returns: a `Void` value on success.

Errors: the system-random module's error (19, `ENODEV`) is returned when the kernel has no randomness source to offer.

Example: the following function seeds a game:

```
func seed_game(): Void! {
 return try std::random::auto_seed();
}
```

#### `std::random::next_u64`

Signature: `std::random::next_u64() -> UInt64`

Advances the generator and returns the next 64-bit unsigned integer.

Parameters: none.

Returns: the next value.

Example: the following function draws a 64-bit value:

```
func draw_u64(): UInt64 {
 return std::random::next_u64();
}
```

#### `std::random::below`

Signature: `std::random::below(n: UInt64) -> UInt64`

Returns a random integer in the range `[0, n)`. The argument must be greater than zero.

Parameters: `n` is the exclusive upper bound.

Returns: a value in `[0, n)`.

Example: the following function rolls a six-sided die:

```
func roll_die(): UInt64 {
 return std::random::below(6) + 1;
}
```

#### `std::random::range`

Signature: `std::random::range(lo: UInt64, hi: UInt64) -> UInt64!`

Returns a uniformly distributed value in the half-open interval [`lo`, `hi`).

Errors: `std::random::ERR_INVALID_RANGE` (22) when `hi <= lo`.

#### `std::random::shuffle`

Signature: `std::random::shuffle(static T, items: T[..]) -> Void`

Shuffles the view in place with the Fisher-Yates algorithm. Every permutation is equally likely.

#### `std::random::choice`

Signature: `std::random::choice(static T, items: T[..]) -> T!`

Returns a uniformly distributed element of the view.

Errors: `std::random::ERR_OUT_OF_RANGE` (34) when the view is empty.

## Regular Expressions

The `std::regex` module provides functions for compiling regular expressions and for applying them to strings. A regular expression is compiled into a `Regex` value, which may then be used in matching, searching, and replacement operations.

### Types

```
static Regex = Struct {
 // Opaque implementation details.
};

static Match = Struct {
 start: Int32,
 end: Int32,
};
```

The `Regex` type is opaque. It is created by `std::regex::compile` and is passed by pointer to the functions that operate on it; it owns two heap resources, the compiled instruction program and the character-class bitmap chain, which `std::regex::destroy` releases. The `Match` type describes one match span as byte offsets into the searched string.

### Functions

#### `std::regex::compile`

Signature: `std::regex::compile(pattern: Char[]) -> Regex!`

Compiles the regular expression `pattern` into a `Regex` value.

Parameters: `pattern` is the regular expression to compile.

Returns: the compiled regular expression.

Errors: `std::regex::ERR_INVALID_PATTERN` is returned if the pattern is not a valid regular expression.

Example: the following function compiles a pattern that matches integers:

```
func integer_pattern(): std::regex::Regex! {
 return try std::regex::compile("[0-9]+");
}
```

#### `std::regex::destroy`

Signature: `std::regex::destroy(re: &Regex) -> Void`

Releases the instruction program and the character-class bitmap chain owned by `re`, then resets the value to the state produced by a fresh `compile` failure path. The value must not be used afterwards except by reassigning the result of `compile`.

Parameters: `re` is the compiled regular expression.

Example: the following function compiles, uses, and releases a pattern:

```
func scan_once(text: Char[]): Void! {
 let re = try std::regex::compile("[a-z]+");
 defer std::regex::destroy(&re);
 try std::inout::println("Match at: {try std::regex::find(&re, text)}");
}
```

#### `std::regex::match`

Signature: `std::regex::match(re: &Regex, text: Char[]) -> Bool`

Returns `true` if the entire string `text` matches the regular expression. The match is anchored: the pattern must describe the whole string, not merely a substring.

Parameters: `re` is the compiled regular expression; `text` is the string to test.

Returns: `true` if the string matches.

Example: the following function checks whether a string is entirely numeric:

```
func is_numeric(re: &std::regex::Regex, text: Char[]): Bool {
 return std::regex::match(re, text);
}
```

#### `std::regex::find`

Signature: `std::regex::find(re: &Regex, text: Char[]) -> Int32!`

Returns the index of the first substring of `text` that matches the regular expression, or `-1` if no match is found.

Parameters: `re` is the compiled regular expression; `text` is the string to search.

Returns: the index of the first match, or `-1`.

Errors: `std::regex::ERR_MATCH` is returned if the match operation encounters an error.

Example: the following function locates the first number in a string:

```
func first_number(re: &std::regex::Regex, text: Char[]): Int32! {
 return try std::regex::find(re, text);
}
```

#### `std::regex::find_all`

Signature: `std::regex::find_all(re: &Regex, text: Char[]) -> &Match!`

Returns every non-overlapping match of the regular expression in `text` as a heap array of `Match` records, terminated by a record with `start` and `end` both equal to -1.

Parameters: `re` is the compiled regular expression; `text` is the string to search.

Returns: the terminating `{-1, -1}`-ended array of match spans.

Errors: `std::regex::ERR_MATCH` is returned if the match operation encounters an error; the partial array is released before the error is reported.

Example: the following function counts the numbers in a string:

```
func count_numbers(re: &std::regex::Regex, text: Char[]): UInt64! {
 let ms = try std::regex::find_all(re, text);
 defer free ms;
 let n: UInt64 = 0;
 while @( (ms as UInt64 + n * 8) as &Int32 ) != -1 {
    n += 1;
 }
 return n;
}
```

Ownership: the caller owns the returned array and must release it with `free`.

#### `std::regex::replace`

Signature: `std::regex::replace(re: &Regex, text: Char[], replacement: Char[]) -> Char[]!`

Returns a new string in which every match of the regular expression has been replaced by `replacement`. In `replacement`, `$0` expands to the whole match and `$1` through `$4` to captured groups; any other expansion, including groups the match did not set, produces nothing.

Parameters: `re` is the compiled regular expression; `text` is the source string; `replacement` is the replacement pattern.

Returns: a newly allocated string containing the result.

Errors: `std::regex::ERR_MATCH` is returned if the match operation encounters an error.

Example: the following function replaces every integer with the placeholder `N`:

```
func anonymise(re: &std::regex::Regex, text: Char[]): Char[]! {
 return try std::regex::replace(re, text, "N");
}
```

Ownership: the caller owns the returned string and must release it with `free`.

#### `std::regex::split`

Signature: `std::regex::split(re: &Regex, text: Char[]) -> &&Char!`

Splits `text` at every match of the regular expression and returns a heap array of the substrings between the matches, terminated by a null pointer.

Parameters: `re` is the compiled regular expression; `text` is the string to split.

Returns: the null-pointer-terminated array of substrings.

Errors: `std::regex::ERR_MATCH` is returned if the match operation encounters an error; everything allocated so far is released first.

Example: the following function splits a string on any run of whitespace:

```
func split_words(re: &std::regex::Regex, text: Char[]): &&Char! {
 return try std::regex::split(re, text);
}
```

Ownership: the caller owns the returned array and every element and must release them with `free`.

## Wall-Clock Time

The `std::time` module provides the clocks of the system: the wall-clock and the monotonic clock for reading time, the suspension of the current thread for a specified duration, and the resolution of the monotonic clock for measuring intervals. The wall-clock counts real time and may move backward when the system time is adjusted; the monotonic clock always advances and is the appropriate source of timing information for measuring the duration of an operation.

### Types

```
static TimeSpec = Struct {
 sec: Int64,
 nsec: UInt64,
};
```

The `TimeSpec` type holds one clock reading: whole seconds and the nanosecond remainder.

### Constants

`std::time::ERR_NO_CLOCK` (19) is returned by `std::time::clock_resolution` when the system reports no monotonic clock; `std::time::ERR_OVERFLOW` (75) is returned when a timestamp cannot be represented as an unsigned nanosecond count.

### Functions

#### `std::time::realtime`

Signature: `std::time::realtime() -> TimeSpec!`

Reads the wall-clock clock, which counts real time and may move backward if the system time is adjusted. The return value holds whole seconds since the Unix epoch and the nanosecond remainder.

Parameters: none.

Returns: the current wall-clock reading.

Errors: the kernel's error is returned when the clock cannot be read.

Example: the following function records the current time:

```
func now_ts(): std::time::TimeSpec! {
 return try std::time::realtime();
}
```

#### `std::time::monotonic`

Signature: `std::time::monotonic() -> TimeSpec!`

Reads the monotonic clock, which never moves backward. Its zero point is unspecified; only differences between readings are meaningful.

Parameters: none.

Returns: the current monotonic reading.

Example: the following function records a monotonic timestamp:

```
func tick(): std::time::TimeSpec! {
 return try std::time::monotonic();
}
```

#### `std::time::to_ns`

Signature: `std::time::to_ns(ts: TimeSpec) -> UInt64!`

Folds a timespec into a single nanosecond count: `sec * 1_000_000_000 + nsec`. The conversion checks that `sec` is non-negative and that the multiplication and addition fit in `UInt64`; otherwise it returns `std::time::ERR_OVERFLOW`. Monotonicity guarantees ordering of clock readings, not immunity from integer overflow.

Parameters: `ts` is the reading to fold.

Returns: the nanosecond count.

Example: the following function converts a reading:

```
func as_ns(ts: std::time::TimeSpec): UInt64! {
 return try std::time::to_ns(ts);
}
```

#### `std::time::monotonic_ns`

Signature: `std::time::monotonic_ns() -> UInt64!`

Reads the monotonic clock and folds it into a single nanosecond count. The conversion is checked and returns `ERR_OVERFLOW` if the count exceeds `UInt64`. Monotonic readings are ordered, but applications must still account for the finite range of the integer representation.

Parameters: none.

Returns: the monotonic clock in nanoseconds.

Example: the following function measures an interval:

```
func measure(): UInt64! {
 let start = try std::time::monotonic_ns();
 // Perform the operation here.
 return try std::time::monotonic_ns() - start;
}
```

#### `std::time::sleep_ns`

Signature: `std::time::sleep_ns(ns: UInt64) -> Void!`

Suspends the current thread for the given number of nanoseconds. Other threads in the same program continue to run.

Parameters: `ns` is the duration of the suspension.

Returns: a `Void` value on success.

Errors: the kernel's error is returned when the sleep cannot be performed.

Example: the following function pauses for half a second:

```
func short_pause(): Void! {
 return try std::time::sleep_ns(500000000);
}
```

#### `std::time::clock_resolution`

Signature: `std::time::clock_resolution() -> UInt64!`

Returns the resolution of the monotonic clock in nanoseconds, estimated by sampling successive readings and keeping the smallest nonzero gap between them. The true resolution may be finer than any gap observed in a short run, so the result is an upper bound on what the clock can measure here.

Errors: `std::time::ERR_NO_CLOCK` when no monotonic clock is available.

## Calendar Dates and Times

The `std::datetime` module converts between seconds since the Unix epoch and civil calendar fields — year, month, day, hour, minute, and second. It uses the proleptic Gregorian calendar and UTC. Epoch values follow the POSIX time scale; applications that need local civil time apply an explicit time-zone rule before formatting or interpreting a `DateTime`.

### Types

```
static DateTime = Struct {
 year: Int32,   // proleptic Gregorian year, e.g. 2026
 month: Int32,   // 1 through 12
 day: Int32,    // 1 through 31
 hour: Int32,   // 0 through 23
 minute: Int32, // 0 through 59
 second: Int32, // 0 through 59
};
```

### Constants

`std::datetime::ERR_INVALID_DATE` (22) is returned when a field value
is outside its valid range, when a date does not exist in the given
month (such as 30 February), or when ISO text does not parse;
`std::datetime::ERR_OUT_OF_MEMORY` (12) is returned when a string
allocation fails.

### Functions

#### `std::datetime::is_leap`, `std::datetime::days_in_month`

Signatures: `std::datetime::is_leap(year: Int32) -> Bool`,
`std::datetime::days_in_month(year: Int32, month: Int32) -> Int32!`

`is_leap` reports whether `year` is a leap year. `days_in_month` returns
the number of days in the given month of the given year, taking leap
years into account.

Errors: `std::datetime::ERR_INVALID_DATE` from `days_in_month` when
`month` is not in 1..12.

#### `std::datetime::weekday`

Signature: `std::datetime::weekday(dt: DateTime) -> Int32`

Returns the day of the week of a valid date-time: 0 is Sunday and 6 is
Saturday. The fields of `dt` below the day are ignored.

#### `std::datetime::to_epoch`, `std::datetime::from_epoch`

Signatures: `std::datetime::to_epoch(dt: DateTime) -> Int64!`,
`std::datetime::from_epoch(sec: Int64) -> DateTime`

Convert between the civil form and whole seconds since the Unix epoch
(1970-01-01T00:00:00Z). The conversions are exact in both directions for
every representable value.

Errors: `std::datetime::ERR_INVALID_DATE` from `to_epoch` when a field
is out of range or the date does not exist.

#### `std::datetime::date_to_days`, `std::datetime::date_from_days`

Signatures:
`std::datetime::date_to_days(year: Int32, month: Int32, day: Int32) -> Int64!`,
`std::datetime::date_from_days(days: Int64) -> (Int32, Int32, Int32)`

The low-level building blocks behind the epoch conversions: the number
of days since 1970-01-01 for a calendar date, and the calendar date
(year, month, day) for a day count. Most programs use `to_epoch` and
`from_epoch` instead.

Errors: `std::datetime::ERR_INVALID_DATE` from `date_to_days` on an
invalid date.

#### `std::datetime::now`

Signature: `std::datetime::now() -> DateTime!`

Reads the wall clock and returns the current date and time in UTC.

Errors: the kernel's error when the clock cannot be read.

#### `std::datetime::add_seconds`, `std::datetime::add_days`

Signatures:
`std::datetime::add_seconds(dt: DateTime, n: Int64) -> DateTime`,
`std::datetime::add_days(dt: DateTime, n: Int32) -> DateTime`

Return the date-time `n` seconds or days after (or before, for negative
`n`) the given value, normalizing across month and year boundaries.
Arithmetic follows the POSIX time scale and normalizes across calendar boundaries.

#### `std::datetime::format_iso`, `std::datetime::parse_iso`

Signatures: `std::datetime::format_iso(dt: DateTime) -> Str!`,
`std::datetime::parse_iso(text: Char[]) -> DateTime!`

`format_iso` renders a date-time as `YYYY-MM-DDTHH:MM:SS` in UTC.
`parse_iso` reads the same format, requiring exactly two digits per
field and the `T` separator.

Errors: `std::datetime::ERR_OUT_OF_MEMORY` from `format_iso`;
`std::datetime::ERR_INVALID_DATE` from `parse_iso` on malformed text or
a nonexistent date.

Ownership: the caller owns the string returned by `format_iso` and
releases it with `std::str::destroy`.

Example:

```
let now = try std::datetime::now();
let later = std::datetime::add_seconds(now, 3600);
let text = try std::datetime::format_iso(later);
defer std::str::destroy(&text);
try std::inout::println("next hour: {std::str::as_chars(&text)}");
```

## File Operations

The `std::file` module provides functions for opening, reading, writing, and otherwise manipulating files. A file is represented by a value of type `File`, which bundles the kernel file descriptor with the operations the kernel provides on it; opening and closing go through `std::sys::open` and `std::sys::close`, positional access through `std::sys::lseek`, and bulk transfers through the looping `read_all` and `write_all` helpers, which turn the kernel's partial-transfer contract into all-or-error.

The module also provides `read_file`, the common convenience of slurping a whole file into memory: it sizes the file, allocates the exact buffer on the heap, reads the content in one loop, and appends a zero byte so textual content can be handled with the `std::string` module without a separate copy.

Errors are the raw kernel `errno` values: 2 when the file does not exist, 13 when the program lacks permission, 21 when the path names a directory, and 5 for an I/O failure.

### Types

```
static File = Struct {
 fd: UInt64,
};

static Blob = Struct {
 data: &Char,
 len: UInt64,
};
```

The `File` type identifies one open file by its descriptor. The `Blob` type is a heap-allocated byte buffer together with its valid length; the allocation always has room for `len + 1` bytes and the byte at offset `len` is always zero, so a `Blob` holding text is directly usable as a `Char[]`.

### Functions

#### `std::file::File.open`

Signature: `std::file::File.open(path: Char[], flags: UInt32) -> File!`

Opens the existing file at `path` with the given combination of the `std::sys::O_*` flags and returns it as a `File`. Common flag combinations: `std::sys::O_RDONLY` for reading, `std::sys::O_RDWR` for read-write access, `std::sys::O_CREAT | std::sys::O_TRUNC | std::sys::O_WRONLY` for replacing content.

Parameters: `path` is the path of the file to open; `flags` is the open-mode bitmask.

Returns: a handle to the file.

Errors: 2 is returned if the file does not exist; 13 is returned if the program lacks permission; 21 is returned if the path names a directory.

Example: the following function opens a file for reading:

```
func open_for_read(path: Char[]): std::file::File! {
 return try std::file::File.open(path, std::sys::O_RDONLY);
}
```

Ownership: the caller owns the file handle and must release it with `std::file::File.close`.

#### `std::file::File.create`

Signature: `std::file::File.create(path: Char[], mode: UInt32) -> File!`

Creates a new file at `path`, truncating any existing file, with the given permission bits (the usual value is 420, octal 0644).

Parameters: `path` is the path of the file to create; `mode` is the permission bitmask.

Returns: a handle to the file, opened for writing.

Example: the following function creates a file for writing:

```
func create_output(path: Char[]): std::file::File! {
 return try std::file::File.create(path, 420);
}
```

#### `std::file::File.read` and `std::file::File.write`

Signatures: `std::file::File.read(this: File, buf: &Void, cap: UInt64) -> UInt64!` and `std::file::File.write(this: File, buf: &Void, count: UInt64) -> UInt64!`

Perform one kernel transfer. `read` stores up to `cap` bytes into `buf` and returns the byte count, which is smaller at end of file; zero means the end has been reached. `write` passes `count` bytes to the kernel and returns the number accepted, which may be smaller than requested.

Example: the following function writes a string to a file:

```
func write_string(f: std::file::File, s: Char[]): UInt64! {
 return try std::file::File.write(f, s as &Void, std::string::len(s));
}
```

#### `std::file::File.read_all` and `std::file::File.write_all`

Signatures: `std::file::File.read_all(this: File, buf: &Void, cap: UInt64) -> UInt64!` and `std::file::File.write_all(this: File, buf: &Void, count: UInt64) -> UInt64!`

`read_all` reads until `buf` is exactly full or end of file is reached, and returns the total number of bytes stored; end of file is reported as a short total, never as an error. `write_all` writes every byte, looping past short writes, and returns the total written.

Example: the following function copies a file in blocks:

```
func copy_file(src: Char[], dst: Char[]): Void! {
 let f = try std::file::File.open(src, std::sys::O_RDONLY);
 defer std::file::File.close(f);
 let g = try std::file::File.create(dst, 420);
 defer std::file::File.close(g);
 let buf: Char[4096];
 while true {
    let n = try std::file::File.read(f, &buf[0] as &Void, 4096);
    if n == 0 {
      break;
    }
    try std::file::File.write_all(g, &buf[0] as &Void, n);
 }
}
```

#### `std::file::File.size`

Signature: `std::file::File.size(this: File) -> UInt64!`

Returns the size of the file in bytes. The current file offset is preserved: the position is saved, moved to the end for measurement, and restored afterwards.

Example: the following function reports the size of a file:

```
func report_size(f: std::file::File): Void! {
 let n = try std::file::File.size(f);
 try std::inout::println("File size: {n} bytes");
}
```

#### `std::file::File.close`

Signature: `std::file::File.close(this: File) -> Void!`

Closes the underlying file descriptor. After this call, the `File` value must not be used.

#### `std::file::read_file`

Signature: `std::file::read_file(path: Char[]) -> Blob!`

Opens `path`, reads its entire content into a freshly allocated heap buffer, closes the file, and returns the buffer with its length.

Parameters: `path` is the path of the file to read.

Returns: the `Blob`; its `data` field is NUL-terminated at offset `len`, so text files are directly usable as `Char[]`.

Errors: 2 is returned if the file does not exist; 13 is returned if the program lacks permission.

Example: the following function reads a configuration file:

```
func read_config(path: Char[]): std::file::Blob! {
 return try std::file::read_file(path);
}
```

Ownership: the caller owns the returned buffer and must release it with `free`.

#### `std::file::lock_exclusive`, `std::file::lock_shared`, `std::file::unlock`

Signatures:
- `std::file::lock_exclusive(f: File) -> Void!`,
- `std::file::lock_shared(f: File) -> Void!`,
- `std::file::unlock(f: File) -> Void!`

Place advisory locks on an open file. An exclusive lock excludes every other locker; a shared lock excludes exclusive lockers only. A lock is released by `std::file::unlock` or by closing the file, and it never survives the process: an advisory lock coordinates cooperative programs and does not prevent uncooperative access.

Errors: the kernel's error.

## High-Level Filesystem Operations

The `std::file` module speaks in open handles; the `std::fs` module
speaks in paths. It provides the one-shot operations a program reaches
for constantly — testing existence, copying, moving, removing,
atomically replacing content, measuring a tree, walking a tree — pairing
acquisition and release internally so the caller needs no cleanup.

### Constants

`std::fs` reports the kernel's error codes directly: 2 when a path does
not exist, 13 when permission is lacking, 17 when a directory already
exists, 18 when a move crosses devices, 20 when a path component is not
a directory, 21 when a file operation meets a directory, and 39 when a
directory to remove is not empty.

### Functions

#### `std::fs::exists`

Signature: `std::fs::exists(path: Char[]) -> Bool`

Returns `true` when the path can be stat'ed. Any error — a missing
path, a permission failure — returns `false`; use `std::stat` when the
distinction matters.

#### `std::fs::copy_file`

Signature: `std::fs::copy_file(src: Char[], dst: Char[]) -> Void!`

Copies the content of `src` to `dst`, replacing `dst` when it exists
and creating it with mode 0644 when it does not. The copy is performed
block by block; an error mid-copy leaves a partial `dst`.

#### `std::fs::move_file`

Signature: `std::fs::move_file(src: Char[], dst: Char[]) -> Void!`

Renames `src` to `dst`. When the rename fails because the two paths
live on different devices (error 18), the function falls back to
copying the content and removing the source; in that case an error
mid-copy leaves both paths partially updated.

#### `std::fs::remove_file`

Signature: `std::fs::remove_file(path: Char[]) -> Void!`

Removes a file or an empty directory.

#### `std::fs::write_file`

Signature: `std::fs::write_file(path: Char[], data: Char[], len: UInt64) -> Void!`

Writes `len` bytes of `data` to `path` atomically: the content is
written to a temporary file in the same directory and renamed over the
target, so a concurrent reader sees the old content or the new content,
never a partial file.

#### `std::fs::read_text`

Signature: `std::fs::read_text(path: Char[]) -> Str!`

Reads the whole file and returns it as an owning string. Convenience
over `std::file::read_file` for text processing.

Ownership: the caller owns the result and releases it with
`std::str::destroy`.

#### `std::fs::tree_size`

Signature: `std::fs::tree_size(path: Char[]) -> UInt64!`

Returns the total size in bytes of every regular file in the directory
tree rooted at `path`. Symbolic links are counted as links, not
followed; hard links are counted once per occurrence.

#### `std::fs::walk`

Signature: `std::fs::walk(root: Char[], f: func(path: Char[], kind: EntryKind): Void!) -> Void!`

Calls `f` once for every entry in the directory tree rooted at `root`,
including `root` itself, before the entries of a directory and before
descending into it.

### Types

```
static EntryKind = Enum {
 Regular,
 Directory,
 Symlink,
 Other,   // devices, sockets, and anything else
};
```

Errors returned by `f` stop the walk and propagate.

Example:

```
try std::fs::walk("logs", |path, kind| {
 if kind == EntryKind.Regular {
    let size = try std::fs::tree_size(path);
    try std::inout::println("{path}: {size}");
 }
 return {};
});
```

## Path Manipulation

The `std::path` module provides purely lexical path operations: no function in this module checks whether a path exists — the question these functions answer is what the path says, not what the filesystem holds. Results are produced either as pointers into the input (`basename`, `extension`) or into caller-supplied buffers (`dirname_into`, `join_into`, `normalize_into`), following the bounded-copy discipline of the string module: error 36 (`ENAMETOOLONG`) when the buffer is too small, and a terminating zero is always written within the capacity on success. All functions assume the Unix `/` separator.

### Functions

#### `std::path::basename`

Signature: `std::path::basename(path: Char[]) -> Char[]`

Returns a pointer to the final component of `path`, after stripping trailing separators; for a path of only separators the result is `"/"`. The pointer aliases the input and stays valid exactly as long as the input does.

Example: the following function extracts the file name from a path:

```
func name_of(path: Char[]): Char[] {
 return std::path::basename(path);
}
```

#### `std::path::extension`

Signature: `std::path::extension(path: Char[]) -> Char[]`

Returns a pointer to the extension of `path` — the bytes after the last dot of the basename, excluding a leading dot, so `.profile` has no extension. With no dot the result points at the terminator: an empty, still valid string. The pointer aliases the input.

Example: the following function checks the extension of a path:

```
func ext_of(path: Char[]): Char[] {
 return std::path::extension(path);
}
```

#### `std::path::dirname_into`

Signature: `std::path::dirname_into(path: Char[], out: Char[], cap: UInt64) -> UInt64!`

Writes the directory portion of `path` into `out` NUL-terminated and returns its length. Trailing separators are stripped before the search, the result never ends in a separator except the lone root `/`, a path with no separator yields `.`, and a path of only separators yields `/`.

Errors: error 36 is returned when `cap` is too small.

Example: the following function extracts the directory from a path:

```
func dir_of(path: Char[], out: Char[]): UInt64! {
 return try std::path::dirname_into(path, out, 4096);
}
```

#### `std::path::join_into`

Signature: `std::path::join_into(a: Char[], b: Char[], out: Char[], cap: UInt64) -> UInt64!`

Writes `a/b` into `out` NUL-terminated and returns the length. The separator is inserted only when `a` is nonempty and does not already end in one; `b` is used verbatim, so a `b` starting with a separator produces a doubled one.

Errors: error 36 is returned when `cap` is too small.

Example: the following function constructs a path to a file in a directory:

```
func file_in(dir: Char[], name: Char[], out: Char[]): UInt64! {
 return try std::path::join_into(dir, name, out, 4096);
}
```

#### `std::path::normalize_into`

Signature: `std::path::normalize_into(path: Char[], out: Char[], cap: UInt64) -> UInt64!`

Lexically normalizes `path` into `out`: separator runs collapse to one, `.` segments vanish, and `..` segments pop the previous real segment. An absolute path keeps its leading separator and can never be popped past the root; a relative path that resolves to nothing becomes `.`.

Errors: error 22 is returned when a `..` would climb above the root; error 36 is returned when `cap` is too small.

Example: the following function normalises a user-supplied path:

```
func canonical(path: Char[], out: Char[]): UInt64! {
 return try std::path::normalize_into(path, out, 4096);
}
```

## Directory Operations

The `std::dir` module provides directory listing built directly on the `getdents64(2)` system call, which fills a caller buffer with `dirent64` records. Each record holds an inode number, a record length, a one-byte entry type, and the NUL-terminated name; the module pins the field offsets as constants and exposes the record walk through `entry_first` and `entry_next`, so callers iterate a listing with plain address arithmetic and no per-entry allocation.

Directory creation and removal live in the `std::dirs` module; recursive walking and tree sizing live in `std::walker` and `std::du`.

### Types

```
static Dir = Struct {
 fd: UInt64,
};
```

The `Dir` type identifies one open directory by its descriptor. It is created by `std::dir::Dir.open` and released by `std::dir::Dir.close`.

### Constants

The entry-type values returned by `entry_kind`:

| Constant | Meaning |
| --- | --- |
| `DT_UNKNOWN` | Type not reported by the filesystem |
| `DT_FIFO` | Named pipe |
| `DT_CHR` | Character device |
| `DT_DIR` | Directory |
| `DT_BLK` | Block device |
| `DT_REG` | Regular file |
| `DT_LNK` | Symbolic link |
| `DT_SOCK` | Socket |

### Functions

#### `std::dir::Dir.open`

Signature: `std::dir::Dir.open(path: Char[]) -> Dir!`

Opens the directory at `path` for listing. The `O_DIRECTORY` flag makes the kernel reject the open when the path is not a directory.

Errors: 2 is returned if the directory does not exist; 20 is returned if the path names a file; 13 is returned if the program lacks permission.

Example: the following function opens a directory:

```
func open_dir(path: Char[]): std::dir::Dir! {
 return try std::dir::Dir.open(path);
}
```

#### `std::dir::Dir.read`

Signature: `std::dir::Dir.read(this: Dir, buf: &Void, cap: UInt64) -> UInt64!`

Reads the next batch of entries into `buf` and returns the number of bytes written, which is zero at end of directory. The content of `buf` stays valid until the next read on the same `Dir`.

#### `std::dir::Dir.close`

Signature: `std::dir::Dir.close(this: Dir) -> Void!`

Closes the underlying file descriptor. The `Dir` value must not be used afterwards.

#### `std::dir::list`

Signature: `std::dir::list(path: Char[], buf: &Void, cap: UInt64) -> UInt64!`

One-shot listing: opens the directory at `path`, reads its entries into `buf`, closes the directory, and returns the number of valid entry bytes in `buf`. If reading fails, the directory is still closed and the read error — or, when closing also fails, the close error — is reported.

Errors: 2 is returned if the directory does not exist; 20 is returned if the path names a file.

Example: the following function prints the regular files of a directory:

```
func print_files(path: Char[]): Void! {
 let ents: Char[4096];
 let n = try std::dir::list(path, &ents[0] as &Void, 4096);
 let e = std::dir::entry_first(&ents[0] as &Void, n);
 while e != 0 {
    if std::dir::entry_kind(e) == std::dir::DT_REG {
      try std::inout::println(std::dir::entry_name(e));
    }
    e = std::dir::entry_next(e, &ents[0] as &Void, n);
 }
}
```

#### `std::dir::entry_first`, `std::dir::entry_next`, `std::dir::entry_name`, `std::dir::entry_kind`

Signatures: `std::dir::entry_first(buf: &Void, count: UInt64) -> &Void`, `std::dir::entry_next(entry: &Void, buf: &Void, count: UInt64) -> &Void`, `std::dir::entry_name(entry: &Void) -> Char[]`, `std::dir::entry_kind(entry: &Void) -> Char`

The record-walking primitives. `entry_first` returns a pointer to the first record of a buffer filled by `list` or `Dir.read`, or the zero pointer when the byte count is too small to hold a complete record; `entry_next` returns the following record or the zero pointer when no further complete record fits. `entry_name` returns a pointer to the NUL-terminated name, and `entry_kind` returns the type byte, one of the `DT_*` constants. The returned pointers alias the listing buffer and stay valid until it is reused.

## Directory Creation and Removal

The `std::dirs` module creates and removes directories. Existence tests
live in `std::fs`; listing lives in `std::dir`.

### Functions

#### `std::dirs::mkdir`

Signature: `std::dirs::mkdir(path: Char[]) -> Void!`

Creates a single directory with mode 0755. Errors: the kernel's error;
17 (`EEXIST`) when the path already exists.

#### `std::dirs::mkdirs`

Signature: `std::dirs::mkdirs(path: Char[]) -> Void!`

Creates a directory and every missing parent. Succeeds without change
when the directory already exists, which makes the function safe to
call unconditionally before writing into a nested path.

#### `std::dirs::rmdir`

Signature: `std::dirs::rmdir(path: Char[]) -> Void!`

Removes an empty directory. Errors: the kernel's error; 2 when the path
does not exist, 39 when the directory is not empty.

Example:

```
try std::dirs::mkdirs("var/log/myapp");
```

## Buffered Readers and Writers

The `std::io` module provides buffered reading and writing over file
descriptors. Buffering batches small transfers into large ones: a
program that emits many small writes should write through a `Writer`
rather than calling `std::inout::print` in a loop, and a program that
consumes input line by line should read through a `Reader`.

### Types

```
static Reader = Struct {
 fd: UInt64,
 buf: &Char,   // heap buffer
 len: UInt64, // bytes buffered
 pos: UInt64, // next byte to consume
};

static Writer = Struct {
 fd: UInt64,
 buf: &Char,   // heap buffer
 len: UInt64, // bytes pending
 cap: UInt64,
};
```

### Constants

`std::io` reports the kernel's error codes directly;
`std::io::ERR_LINE_TOO_LONG` (36) is returned by `std::io::read_line`
when the line does not fit the caller's buffer.

### Functions

#### `std::io::Reader::open`, `std::io::Reader::close`

Signatures: `std::io::Reader::open(path: Char[]) -> Reader!`,
`std::io::Reader::close(r: &Reader) -> Void!`

Open the file for reading and take ownership of the descriptor, or
release it. `close` must be scheduled with `defer` immediately after a
successful `open`.

#### `std::io::Reader::read`

Signature: `std::io::Reader::read(r: &Reader, dst: Char[], count: UInt64) -> UInt64!`

Reads up to `count` bytes from the current position into `dst`, drawing
on the buffer first, and returns the number stored. A return of zero
reports end of input.

#### `std::io::Reader::read_line`

Signature: `std::io::Reader::read_line(r: &Reader, dst: Char[], cap: UInt64) -> Bool!`

Reads one line: stores the characters of the next line of input in
`dst`, without the terminating newline, terminated by a zero byte, and
returns `true`. Returns `false` when the input is exhausted.

Errors: `std::io::ERR_LINE_TOO_LONG` when the line does not fit in
`cap` bytes including the terminator.

#### `std::io::Writer::create`, `std::io::Writer::close`

Signatures: `std::io::Writer::create(path: Char[]) -> Writer!`,
`std::io::Writer::close(w: &Writer) -> Void!`

Create the file (truncating an existing one, mode 0644) and take
ownership of the descriptor, or flush pending content and release it.
`close` flushes; `close` must be scheduled with `defer` immediately
after a successful `create`, and any error it reports — including a
failed final flush — must be handled.

#### `std::io::Writer::write`, `std::io::Writer::write_string`, `std::io::Writer::write_byte`, `std::io::Writer::flush`

Signatures:
`std::io::Writer::write(w: &Writer, data: &Void, count: UInt64) -> Void!`,
`std::io::Writer::write_string(w: &Writer, s: Char[]) -> Void!`,
`std::io::Writer::write_byte(w: &Writer, b: Char) -> Void!`,
`std::io::Writer::flush(w: &Writer) -> Void!`

Append bytes to the buffer, flushing to the descriptor whenever the
buffer fills; `flush` drains the buffer explicitly. Content reaches the
file in `write` order.

Errors: the kernel's error from a flush performed inside any of the
write functions or by `flush`.

Example:

```
func copy_lines(src: Char[], dst: Char[]): Void! {
 let r = try std::io::Reader::open(src);
 defer std::io::Reader::close(&r);
 let w = try std::io::Writer::create(dst);
 defer std::io::Writer::close(&w);

 let line: Char[1024];
 while try std::io::Reader::read_line(&r, &line[0], 1024) {
    try std::io::Writer::write_string(&w, &line[0]);
    try std::io::Writer::write_byte(&w, '\n');
 }
}
```

## Descriptor Polling

The `std::poll` module waits until one of a set of file descriptors —
sockets, pipes, input devices — becomes ready for reading or writing,
without dedicating a thread to each. It is the building block beneath
timeout-capable network code and the way a single-threaded program
multiplexes several event sources.

### Types

```
static Poll = Struct {
 fds: &Void,   // heap array of pollfd records
 count: UInt64,
 cap: UInt64,
};

static PollEvent = Struct {
 fd: UInt64,
 readable: Bool,
 writable: Bool,
 hangup: Bool, // peer closed, or an error condition pending
};
```

### Constants

`std::poll::ERR_OUT_OF_MEMORY` (12) is returned when the descriptor
array cannot grow; `std::poll::ERR_NOT_FOUND` (2) is returned when a
descriptor to remove was never added.

### Functions

#### `std::poll::make`, `std::poll::destroy`

Signatures: `std::poll::make() -> Poll`,
`std::poll::destroy(p: &Poll) -> Void`

Allocate or release the descriptor array.

#### `std::poll::add`, `std::poll::remove`

Signatures:
`std::poll::add(p: &Poll, fd: UInt64, want_read: Bool, want_write: Bool) -> Void!`,
`std::poll::remove(p: &Poll, fd: UInt64) -> Void!`

Register interest in a descriptor, replacing the previous registration
when the descriptor is already present, or drop it.

#### `std::poll::wait`

Signature: `std::poll::wait(p: &Poll, out: PollEvent[..], timeout_ns: UInt64) -> UInt64!`

Waits up to `timeout_ns` for at least one registered descriptor to
become ready, fills `out` with up to `out.len` events, and returns the
number stored. A return of zero reports a timeout.

Ownership: the caller of `make` owns the set; `destroy` (usually
scheduled with `defer`) releases the descriptor array.

## File Metadata

The `std::stat` module reads the metadata record of a path: what kind
of file it is, how large it is, and when it was last modified. Use
`std::fs::exists` for a plain existence test; use `std::stat` when the
kind or the size matters.

### Types

```
static Stat = Struct {
 mode: UInt32,     // type and permission bits
 size: UInt64,     // bytes; meaningless for devices
 mtime_sec: Int64,   // last modification, seconds since the epoch
 mtime_nsec: UInt64, // nanosecond remainder
};
```

### Functions

#### `std::stat::stat`, `std::stat::lstat`

Signatures: `std::stat::stat(path: Char[]) -> Stat!`,
`std::stat::lstat(path: Char[]) -> Stat!`

Read the metadata of `path`. `stat` follows symbolic links; `lstat`
describes the link itself.

Errors: the kernel's error; 2 when the path does not exist, 13 when
permission is lacking, 40 (`ELOOP`) on a symlink loop.

#### `std::stat::is_dir`, `std::stat::is_reg`, `std::stat::is_lnk`, `std::stat::is_fifo`, `std::stat::is_sock`

Signature pattern: `std::stat::is_dir(st: Stat) -> Bool`

Predicates over the type bits of a record: directory, regular file,
symbolic link, named pipe, socket.

Example:

```
let st = try std::stat::stat(path);
if std::stat::is_reg(st) and st.size > 0 {
 process(path);
}
```

## System Interface

The `std::sys` module is the raw system call interface: every kernel interaction of the library goes through it, either through its wrapper functions or through the same staging-cell assembly pattern it demonstrates. The raw syscall wrappers translate a kernel return value of `-errno` into the corresponding positive `errno` value before placing it in the language error result. All public `std::` error payloads are therefore non-negative integer codes. A module may define a semantic error constant that shares a numeric value with a POSIX `errno`; use the originating function's documented constants to interpret that value.

### Functions

#### `std::sys::getpid`

Signature: `std::sys::getpid() -> UInt64!`

Returns the process identifier of the current process.

Example: the following function prints the process identifier:

```
func report_pid(): Void! {
 let pid = try std::sys::getpid();
 try std::inout::println("Process ID: {pid}");
}
```

#### `std::sys::gettid`

Signature: `std::sys::gettid() -> UInt64!`

Returns the kernel thread identifier of the calling thread.

#### `std::sys::exit` and `std::sys::exit_group`

Signatures: `std::sys::exit(code: Int32) -> Void` and `std::sys::exit_group(code: Int32) -> Void`

Terminate the calling thread, or the whole process and all of its threads, immediately. Neither returns.

#### `std::sys::sleep-level primitives`

The module also wraps `read`, `write`, `open`, `close`, `lseek`, `mmap`, `munmap`, `brk`, `getdents64`, `sched_yield`, `wait4`, `nanosleep`, and `clock_gettime`, and exports the flag constants those wrappers take: the `O_*` open flags, the `SEEK_*` whence values, the `PROT_*` and `MAP_*` mapping values, and the `CLOCK_REALTIME` / `CLOCK_MONOTONIC` clock identifiers.

## Error Codes

Every module of the library reports failures as integer codes, and the
kernel errors — 2, 12, 13, 22, and the rest — recur in every program.
The `std::err` module maps a code to a stable symbolic name and a
human-readable description, for log lines and diagnostic output.

### Functions

#### `std::err::name`

Signature: `std::err::name(code: UInt64) -> Char[]`

Returns the symbolic name of the error: 2 yields `"ENOENT"`, 12 yields
`"ENOMEM"`, 34 yields `"ERANGE"`. A code that is not in the table yields
`"UNKNOWN"`. The returned pointer refers to static storage: it is never
freed and stays valid for the lifetime of the program.

#### `std::err::text`

Signature: `std::err::text(code: UInt64) -> Char[]`

Returns the description of the error: 2 yields
`"No such file or directory"`, 13 yields `"Permission denied"`. A code
that is not in the table yields `"Unknown error"`. The returned pointer
refers to static storage.

Example:

```
let blob = std::file::read_file(path) catch |e| {
 try std::inout::println(
    "cannot read {path}: {std::err::name(e)} ({std::err::text(e)})"
 );
 return error e;
};
```

The table covers the kernel errno values 1 through 133. `std::err` is a
formatter for operating-system error numbers, not a universal registry
of every module's semantic errors. Module-specific constants retain
their own meaning even when their numeric value matches a kernel errno;
interpret an error using the function and module that produced it.

## Environment and Arguments

The `std::args` and `std::env` modules provide access to the program's command line and environment. Both are snapshot modules: `init` captures the process argument vector or environment at startup into module state, and the accessor functions read that state. The snapshot is never copied — the returned strings point into the process image and stay valid for the program's lifetime.

### Functions

#### `std::args::init`

Signature: `std::args::init() -> UInt64!`

Captures the argument vector. Called once at program start; later calls replace the snapshot.

Returns: the argument count.

#### `std::args::count`

Signature: `std::args::count() -> UInt64`

Returns the number of arguments captured by `init`. The first argument is the path by which the program was invoked.

#### `std::args::get`

Signature: `std::args::get(i: UInt64) -> Char[]`

Returns the `i`-th argument. The index must be smaller than `count`.

Example: the following function prints every argument:

```
func print_args(): Void! {
 let n = try std::args::init();
 let i: UInt64 = 0;
 while i < n {
    try std::inout::println(std::args::get(i));
    i += 1;
 }
}
```

#### `std::env::init`

Signature: `std::env::init() -> UInt64!`

Captures the environment block. Called once at program start; later calls replace the snapshot.

Returns: the number of variables.

#### `std::env::count`

Signature: `std::env::count() -> UInt64`

Returns the number of environment variables captured by `init`.

#### `std::env::get`

Signature: `std::env::get(i: UInt64) -> Char[]`

Returns the `i`-th entry in `NAME=VALUE` form. The index must be smaller than `count`.

#### `std::env::find`

Signature: `std::env::find(name: Char[]) -> Char[]`

Returns the value of the environment variable `name`, or an empty string when the variable is not set. The returned pointer aliases the environment snapshot.

Example: the following function reads the `PATH` variable:

```
func read_path(): Char[] {
 try std::env::init();
 return std::env::find("PATH");
}
```

## Child Processes

The `std::process` module provides child process management built on the `fork`/`execve` pair of system calls. `fork()` clones the calling process and returns twice — the child's process id in the parent, zero in the child — and `execve()` replaces the calling process image with a new program, so the classic spawn sequence is: fork, run `execve` in the child, and wait in the parent.

Argument passing uses the kernel convention: the argument vector and environment are arrays of pointers terminated by a zero entry, which `spawn1` builds on the heap for the single-argument case; the low-level `spawn` accepts pre-built vectors for full control. The built environment is empty — without a runtime there is no original environ pointer to inherit — and callers that need a specific environment build the vector themselves. Buffering caveat: `fork` copies the whole address space, including pending buffered-writer content; flush writers before forking when the child must not re-send what the parent already wrote.

### Types

```
static ChildResult = Struct {
 pid: UInt64,
 exited: Bool,
 code: UInt64,
};
```

The termination report of a reaped child. `exited` selects the meaning of `code`: the exit status for a normal exit, or the signal number for a signal death.

### Functions

#### `std::process::fork`

Signature: `std::process::fork() -> UInt64!`

Clones the calling process. Returns the child's process id in the parent and zero in the child; both copies keep running from the instruction after the call, and the return value is how they tell which copy they are.

#### `std::process::execve`

Signature: `std::process::execve(path: Char[], argv: &Void, envp: &Void) -> UInt64!`

Replaces the calling process image with the program at `path`, passing `argv` and `envp` as pointer vectors. On success this function never returns; it returns an error only when the image could not be started, in which case the caller is expected to exit the child with the errno as its status.

#### `std::process::spawn`

Signature: `std::process::spawn(path: Char[], argv: &Void, envp: &Void) -> UInt64!`

Forks and runs `execve` in the child with the given raw argument and environment vectors. Returns the child's process id in the parent. In the child, an `execve` failure terminates the child with the errno as its exit status, so the parent's wait observes the same code.

#### `std::process::spawn1`

Signature: `std::process::spawn1(path: Char[], arg0: Char[]) -> UInt64!`

Spawns `path` with one argument and an empty environment, the common case for helper tools. The temporary pointer vectors are freed in the parent before returning; the child keeps its own copy, which `execve` consumes.

Errors: raw kernel `errno` values; 11 (`EAGAIN`) when the kernel refuses the fork.

Example: the following function runs `ls -l`:

```
func list_files(): UInt64! {
 return try std::process::spawn1("ls", "-l");
}
```

#### `std::process::wait`

Signature: `std::process::wait(child: UInt64) -> ChildResult!`

Waits for the child `child` to terminate and returns its termination report. Interruption by a signal resumes the wait automatically.

#### `std::process::wait_any`

Signature: `std::process::wait_any() -> ChildResult!`

Waits for whichever child terminates next and returns its report, with `pid` identifying the reaped child. When no children remain, reports error 10 (`ECHILD`).

#### `std::process::wait_timeout`

Signature: `std::process::wait_timeout(child: UInt64, timeout_ns: UInt64) -> ChildResult!`

Waits up to `timeout_ns` of monotonic time for the child to terminate, polling with the non-blocking `WNOHANG` option. When the deadline passes while the child is still running, reports error 110 (`ETIMEDOUT`); the child keeps running and can be waited on again.

#### `std::process::run`

Signature: `std::process::run(path: Char[], arg0: Char[]) -> UInt64!`

Runs `path` with one argument to completion and returns its exit status. When the child dies on a signal, the signal number is reported as the error code instead.

Example: the following function waits for a child and prints its status:

```
func wait_for(child: UInt64): Void! {
 let r = try std::process::wait(child);
 try std::inout::println("Exit code: {r.code}");
}
```

## Pipes

The `std::pipe` module provides descriptor pairs with a kernel buffer between them — bytes written to the write end come out of the read end in order — and the captured-run helper built on them. Pipes are the plumbing that connects a parent to a child's standard streams: fork, point the child's descriptor 1 at the pipe's write end with `dup2`, and read the child's output from the read end.

### Types

```
static Pipe = Struct {
 r: UInt64,
 w: UInt64,
};

static ChildOutput = Struct {
 status: UInt64,
 data: &Char,
 len: UInt64,
};
```

`Pipe` is a connected descriptor pair: read from `r`, write to `w`. `ChildOutput` is the result of `run_capture`: the child's exit status (128 plus the signal number for signal deaths, the shell convention) and its stdout in a heap buffer the caller releases.

### Functions

#### `std::pipe::make`

Signature: `std::pipe::make() -> Pipe!`

Creates a pipe and returns its descriptor pair.

#### `std::pipe::dup2`

Signature: `std::pipe::dup2(old: UInt64, new: UInt64) -> Void!`

Redirects the descriptor `old` onto the descriptor number `new`; after a successful call, `new` refers to the same open file as `old`. Used by `run_capture` and available for custom plumbing.

#### `std::pipe::run_capture`

Signature: `std::pipe::run_capture(path: Char[], arg0: Char[]) -> ChildOutput!`

Runs the program at `path` with one argument and captures its standard output. The child redirects stdout to the pipe's write end and closes both original pipe ends before `execve`, so the executed program starts with exactly the redirected stdout; the parent closes its own copy of the write end first — which is what produces end of file once the child exits — then reads the pipe to exhaustion and reaps the child.

Returns: the exit status and the captured bytes.

Errors: raw kernel `errno` values; 27 (`EFBIG`) when the captured output exceeds the 64 MiB cap.

Example: the following function runs a helper and prints its output:

```
func run_helper(): Void! {
 let out = try std::pipe::run_capture("echo", "hello");
 defer free out.data;
 try std::sys::write(1, out.data as &Void, out.len);
}
```

## Low-Level Networking

The `std::net` module provides TCP sockets: a `TcpStream` is one connected endpoint with read and write, and a `TcpListener` accepts incoming connections. Addresses are IPv4 in the packed `UInt32` form produced by `std::net::addr_ipv4`; name resolution and datagram sockets live in the same module, as do the socket timeouts described below. Errors are raw kernel `errno` values: 111 (`ECONNREFUSED`) when the remote refuses a connection, 110 (`ETIMEDOUT`) on timeout, and 113 (`EHOSTUNREACH`) when no route exists.

### Types

```
static TcpListener = Struct {
 fd: UInt64,
};

static UdpSocket = Struct {
 fd: UInt64,
};
```

### Functions

#### `std::net::addr_ipv4`

Signature: `std::net::addr_ipv4(a: UInt8, b: UInt8, c: UInt8, d: UInt8) -> UInt32`

Packs four dotted components into the kernel's IPv4 address form.

Example: the following function builds the loopback address:

```
func loopback(): UInt32 {
 return std::net::addr_ipv4(127, 0, 0, 1);
}
```

#### `std::net::TcpStream.connect`

Signature: `std::net::TcpStream.connect(ip: UInt32, port: UInt16) -> TcpStream!`

Connects a new socket to the remote address and port.

Errors: 111 is returned if the remote refuses the connection; 110 is returned if the attempt times out.

Example: the following function connects to a local server:

```
func connect_local(): std::net::TcpStream! {
 return try std::net::TcpStream.connect(std::net::addr_ipv4(127, 0, 0, 1), 8080);
}
```

#### `std::net::TcpStream.read` and `std::net::TcpStream.write`

Signatures: `std::net::TcpStream.read(this: TcpStream, buf: &Void, cap: UInt64) -> UInt64!` and `std::net::TcpStream.write(this: TcpStream, data: &Void, len: UInt64) -> Void!`

`read` receives up to `cap` bytes and returns the count, zero meaning the peer closed the connection. `write` sends every byte, looping past short writes.

#### `std::net::TcpStream.close`

Signature: `std::net::TcpStream.close(this: TcpStream) -> Void!`

Closes the socket.

#### `std::net::TcpListener.bind` and `std::net::TcpListener.listen`

Signatures: `std::net::TcpListener.bind(ip: UInt32, port: UInt16) -> TcpListener!` and `std::net::TcpListener.listen(this: TcpListener, backlog: UInt32) -> Void!`

`bind` creates a socket bound to the local address and port — 98 (`EADDRINUSE`) is reported when the port is taken — and `listen` marks it ready to accept, queuing up to `backlog` pending connections.

#### `std::net::TcpListener.accept`

Signature: `std::net::TcpListener.accept(this: TcpListener) -> TcpStream!`

Accepts the next incoming connection and returns its socket.

Example: the following function serves one connection:

```
func serve_once(port: UInt16): Void! {
 let l = try std::net::TcpListener.bind(std::net::addr_ipv4(0, 0, 0, 0), port);
 try std::net::TcpListener.listen(l, 16);
 let s = try std::net::TcpListener.accept(l);
 defer std::net::TcpStream.close(s);
 try std::net::TcpStream.write(s, "hello" as &Void, 5);
}
```

#### `std::net::UdpSocket::bind`, `std::net::UdpSocket::send_to`, `std::net::UdpSocket::recv_from`, `std::net::UdpSocket::close`

Signatures:
`std::net::UdpSocket::bind(ip: UInt32, port: UInt16) -> UdpSocket!`,
`std::net::UdpSocket::send_to(s: UdpSocket, data: &Void, len: UInt64, ip: UInt32, port: UInt16) -> Void!`,
`std::net::UdpSocket::recv_from(s: UdpSocket, buf: &Void, cap: UInt64) -> (UInt64, UInt32, UInt16)!`,
`std::net::UdpSocket::close(s: UdpSocket) -> Void!`

`bind` creates a datagram socket bound to the local address; 98
(`EADDRINUSE`) is reported when the port is taken. `send_to` sends one
datagram, up to 65507 bytes, to the remote address. `recv_from` receives
one datagram into `buf` and returns the byte count together with the
sender address and port. A datagram larger than `cap` is truncated to
`cap` bytes.

#### `std::net::resolve`

Signature: `std::net::resolve(name: Char[]) -> UInt32!`

Resolves a host name to its first IPv4 address. Name resolution is synchronous and blocking; the numeric form `127.0.0.1` needs no lookup and can be built with `std::net::addr_ipv4` directly.

Errors: `std::net::ERR_RESOLUTION_FAILED` (22) when the name cannot be resolved.

#### `std::net::TcpStream::set_recv_timeout`, `std::net::TcpStream::set_send_timeout`

Signatures:
`std::net::TcpStream::set_recv_timeout(s: TcpStream, ns: UInt64) -> Void!`,
`std::net::TcpStream::set_send_timeout(s: TcpStream, ns: UInt64) -> Void!`

Set a limit on how long a subsequent `read` or `write` may block. When the limit expires, the operation reports 110 (`ETIMEDOUT`). A `ns` of zero clears the limit. The same two functions exist on `UdpSocket` (`std::net::UdpSocket::set_recv_timeout`, `std::net::UdpSocket::set_send_timeout`) with the same semantics for `recv_from` and `send_to`.

## HTTP Client

The `std::http` module provides a minimal HTTP/1.0 client. `get` performs one GET over a fresh TCP connection: it sends the request line and an empty header section, then reads the response until the server closes the connection, which HTTP/1.0 servers do after the body. Deliberate simplifications, consistent with the module's role as a basic building block: no header parsing beyond locating the end of the header section, no chunked transfer encoding, no redirects, no name resolution — the caller passes a numeric IPv4 address — and a 16 MiB cap on the total response size.

### Types

```
static Response = Struct {
 status: UInt64,
 body: Char[],
 body_len: UInt64,
 buf: Char[],
 buf_len: UInt64,
};
```

The `status` field is the numeric status code from the status line; the reason phrase is skipped. `buf` and `buf_len` cover the whole raw response and are owned by the caller; `body` and `body_len` alias the region past the header section inside the same allocation and are not separately released.

### Functions

#### `std::http::get`

Signature: `std::http::get(ip: UInt32, port: UInt16, path: Char[]) -> Response!`

Performs one GET of `path` on the host at the given address. The path is used verbatim in the request line and should start with `/`; an empty path sends `/`.

Returns: the response.

Errors: 71 (`EPROTO`) is returned when the response does not parse as HTTP; 27 (`EFBIG`) when the response exceeds the size cap; raw socket errors otherwise.

Example: the following function fetches a page and prints its status:

```
func fetch_status(ip: UInt32): Void! {
 let r = try std::http::get(ip, 80, "/index.html");
 defer std::http::free_response(r);
 try std::inout::println("Status: {r.status}");
}
```

#### `std::http::free_response`

Signature: `std::http::free_response(r: Response) -> Void`

Releases the heap buffer owned by `r`. The `Response` value must not be used afterwards.

## URLs

The `std::url` module parses URLs into their components and reconstructs them. Percent-encoding of individual components lives in `std::encoding`.

### Types

```
static Url = Struct {
 scheme: Char[],
 host: Char[],
 port: UInt16,
 path: Char[],
 query: Char[],
 fragment: Char[],
};
```

The `Url` type describes the components of a URL. The string fields of a parsed `Url` are slices into one owned heap allocation. The allocation is owned by the `Url` value and is released with `std::url::destroy`; individual fields are borrowed views and must not be freed separately.

### Functions

#### `std::url::parse`

Signature: `std::url::parse(url: Char[]) -> Url!`

Parses the given URL into its components. The scheme is required and must be followed by `://`; a port, when present, must be decimal digits no greater than 65535; the path, query, and fragment are optional and default to empty strings, and a port that is absent defaults to zero.

Parameters: `url` is the URL to parse.

Returns: the parsed URL.

Errors: `std::url::ERR_INVALID_URL` (22) is returned if the scheme or the port is malformed.

Example: the following function parses a URL and prints its host:

```
func show_host(url: Char[]): Void! {
 let u = try std::url::parse(url);
 defer std::url::destroy(&u);
 try std::inout::println("Host: {u.host}");
}
```

#### `std::url::destroy`

Signature: `std::url::destroy(url: &Url) -> Void`

Releases the allocation owned by a parsed URL and resets its fields to
empty values. Call this exactly once for every successful `parse` whose
ownership is not transferred.

#### `std::url::serialize`

Signature: `std::url::serialize(url: &Url) -> Char[]!`

Reconstructs a URL string from its components. A component that is empty is omitted, so parsing `http://host/p` and serialising the result yields the same string; a trailing `?` or `#` with no content is not preserved.

Parameters: `url` is the URL to serialise.

Returns: the reconstructed URL.

Errors: `std::url::ERR_ALLOC` (12) is returned when the allocation fails.

Example: the following function round-trips a URL:

```
func roundtrip(url: Char[]): Char[]! {
 let u = try std::url::parse(url);
 defer std::url::destroy(&u);
 return try std::url::serialize(&u);
}
```

Ownership: the caller owns the returned string and must release it with `free`.

## JSON

The `std::json` module parses JSON text into a `Value`, and the companion module `std::json_write` serialises a `Value` back to text. A `Value` is one of null, a boolean, a number, a string, an array, or an object; the kind is queried with the `is_*` predicates and the content extracted with the `as_*` accessors, each of which reports error 22 when the value is not of the expected kind. Arrays and objects are opaque to the caller: `arr_len`/`arr_get` and `obj_len`/`obj_get` walk them by index, and `free_value` releases the whole tree.

### Functions

#### `std::json::parse`

Signature: `std::json::parse(text: Char[]) -> Value!`

Parses the given JSON text into a `Value`. The parser is recursive descent over the UTF-8 bytes and builds the tree on the heap.

Parameters: `text` is the JSON text.

Returns: the parsed value.

Errors: error 22 is returned if the text is not valid JSON.

Example: the following function parses a JSON object:

```
func parse_object(text: Char[]): std::json::Value! {
 return try std::json::parse(text);
}
```

#### `std::json::is_null`, `std::json::is_bool`, `std::json::is_num`, `std::json::is_str`, `std::json::is_arr`, `std::json::is_obj`

Signatures: `std::json::is_*(v: &Value) -> Bool`

The kind predicates.

#### `std::json::as_bool`, `std::json::as_num`, `std::json::as_str`

Signatures: `std::json::as_bool(v: &Value) -> Bool!`, `std::json::as_num(v: &Value) -> Real64!`, `std::json::as_str(v: &Value) -> Char[]!`

Extract a value of the expected kind. Error 22 is reported when the kind does not match.

#### `std::json::arr_len`, `std::json::arr_get`

Signatures: `std::json::arr_len(v: &Value) -> UInt64`, `std::json::arr_get(v: &Value, i: UInt64) -> Value!`

The length of an array value and its `i`-th element.

#### `std::json::obj_len`, `std::json::obj_get`

Signatures: `std::json::obj_len(v: &Value) -> UInt64`, `std::json::obj_get(v: &Value, i: UInt64) -> (Char[], Value)!`

The number of pairs of an object value and its `i`-th `(key, value)` pair.

Example: the following function extracts a string field from an object:

```
func get_name(v: &std::json::Value): Char[]! {
 let n = std::json::obj_len(v);
 let i: UInt64 = 0;
 while i < n {
    let (key, val) = try std::json::obj_get(v, i);
    if std::string::compare(key, "name") == 0 {
      return try std::json::as_str(&val);
    }
    i += 1;
 }
 return error 22;
}
```

#### `std::json::free_value`

Signature: `std::json::free_value(v: Value) -> Void`

Releases the value and every nested allocation. Every value produced by `parse` must reach exactly one `free_value`.

#### `std::json::str_len`

Signature: `std::json::str_len(v: &Value) -> UInt64`

Returns the length of a string value in bytes, for buffers that must hold its content.

#### `std::json_write::stringify`

Signature: `std::json_write::stringify(v: Value, out: &std::bytes::ByteBuffer) -> Void!`

Serialises a `Value` to compact JSON text, appending the bytes to the caller's byte buffer.

Example: the following function parses and reprints a document:

```
func reprint(text: Char[]): std::bytes::ByteBuffer! {
 let v = try std::json::parse(text);
 defer std::json::free_value(v);
 let out = std::bytes::make();
 errdefer std::bytes::destroy(&out);
 try std::json_write::stringify(v, &out);
 return out;
}
```

## Binary Serialisation

The `std::serialize` module converts values of struct and array types into a
compact byte representation and back. It is built on the compile-time `sizeof`
builtin and works for the scalar types supported by the compiler. Aggregate
serialisation covers structs whose fields are all serialisable and fixed arrays
of serialisable element type. The serialisable scalar types are the integer
types, the floating-point types, and `Bool`. Pointers, views, channels, and
the opaque handles of the library are not serialisable.

The encoding is defined exactly, so bytes produced on one architecture
are readable on another: multi-byte integers are stored little-endian,
floating-point values as their IEEE-754 bit patterns, `Bool` as one
byte, fixed arrays as their elements with no count, and structs as their fields in declaration order with no padding bytes.
Enums are encoded using their declared underlying integer type. A
`Char[]` string serialises as a `UInt64` byte count followed by the UTF-8
bytes, without the null terminator. `extern` structs are not part of the
portable serialization contract because their layout is target-ABI
dependent; use field-wise serialization for portable records.

### Constants

`std::serialize::ERR_UNSUPPORTED_TYPE` (22) is returned when a type
cannot be serialised; `std::serialize::ERR_BUFFER_TOO_SMALL` (34) is
returned by `encode` when the destination does not hold the encoded
form; `std::serialize::ERR_INVALID_DATA` (22) is returned by `decode`
when the bytes are not a valid encoding of the requested type.

### Functions

#### `std::serialize::size`

Signature: `std::serialize::size(static T, value: &T) -> UInt64!`

Returns the number of bytes the value occupies in the binary
representation.

Errors: `std::serialize::ERR_UNSUPPORTED_TYPE`.

#### `std::serialize::encode`

Signature: `std::serialize::encode(static T, value: &T, buffer: &Char) -> UInt64!`

Serialises `value` into `buffer` and returns the number of bytes
written, which equals `std::serialize::size(T, value)`.

Errors: `std::serialize::ERR_UNSUPPORTED_TYPE`,
`std::serialize::ERR_BUFFER_TOO_SMALL`.

#### `std::serialize::decode`

Signature: `std::serialize::decode(static T, buffer: &Char, count: UInt64) -> T!`

Deserialises a value of type `T` from the first `count` bytes of
`buffer`, which must hold exactly one encoded value of that type.

Errors: `std::serialize::ERR_UNSUPPORTED_TYPE`,
`std::serialize::ERR_INVALID_DATA`.

Example:

```
static Point = Struct {
 x: Real64,
 y: Real64,
};

let p: Point = Point {1.0, 2.0};
let buf: Char[sizeof(Point)];
let n = try std::serialize::encode(Point, &p, &buf[0]);
let q = try std::serialize::decode(Point, &buf[0], n);
```

## CSV

The `std::csv` module reads and writes comma-separated values in the
common RFC 4180 form: a comma between fields, a double quote quoting a
field that contains a comma, quote, or newline, and a doubled quote for
a literal quote inside a quoted field. Lines may end in LF or CRLF.
Streaming is expressed with callback functions, so parsing a large file needs no
intermediate storage.

### Constants

`std::csv::ERR_INVALID_DATA` (22) is returned on malformed quoting;
`std::csv::ERR_OUT_OF_MEMORY` (12) is returned when an allocation fails.
Errors returned by a caller-supplied callback function propagate unchanged.

### Functions

#### `std::csv::parse_row`

Signature: `std::csv::parse_row(row: Char[], f: func(field: Char[]): Void!) -> Void!`

Calls `f` once for every field of a single record. The record is the
text of one line (without the line ending); quoted fields may not span
records in this form.

#### `std::csv::each_row`

Signature: `std::csv::each_row(text: Char[], f: func(row: Char[]): Void!) -> Void!`

Calls `f` once for every record of a document. Quoted fields may span
lines; the record passed to `f` is the raw record text without the line
ending.

#### `std::csv::write_row`

Signature: `std::csv::write_row(out: &std::str::Str, fields: std::str::Str[..]) -> Void!`

Appends one record to `out`: fields joined by commas, quoted where the
content requires it, terminated by a newline.

#### `std::csv::count_rows`

Signature: `std::csv::count_rows(text: Char[]) -> UInt64!`

Returns the number of records in the document.

Example:

```
try std::csv::each_row(text, |row| {
 return try std::csv::parse_row(row, |field| {
    return try std::inout::println(" {field}");
 });
});
```

## INI Configuration

The `std::ini` module parses the INI configuration format: sections in
brackets, `key = value` lines, and `;` or `#` comments. The result is
read-only; a program that writes configuration writes text, so there is
no serialiser.

### Types

```
static Ini = Struct {
 // section and key storage; opaque to the caller
 store: &Void,
};
```

### Constants

`std::ini::ERR_INVALID_DATA` (22) is returned on malformed text;
`std::ini::ERR_OUT_OF_MEMORY` (12) is returned when an allocation fails.

### Functions

#### `std::ini::parse`, `std::ini::destroy`

Signatures: `std::ini::parse(text: Char[]) -> Ini!`,
`std::ini::destroy(ini: &Ini) -> Void`

Parse a document or release it. `destroy` must be scheduled with `defer`
immediately after a successful `parse`.

#### `std::ini::get`, `std::ini::has`

Signatures: `std::ini::get(ini: &Ini, section: Char[], key: Char[]) -> Char[]`,
`std::ini::has(ini: &Ini, section: Char[], key: Char[]) -> Bool`

Look up a value. Keys written before any section header live in the
empty section `""`. A missing key yields an empty string from `get` and
`false` from `has`; the returned pointer aliases the parse storage and
stays valid until `destroy`.

#### `std::ini::each`

Signature: `std::ini::each(ini: &Ini, f: func(section: Char[], key: Char[], value: Char[]): Void!) -> Void!`

Calls `f` once for every entry, in document order. Errors returned by
`f` stop the iteration and propagate.

Example:

```
let ini = try std::ini::parse(text);
defer std::ini::destroy(&ini);
let port = std::ini::get(&ini, "server", "port");
if std::ini::has(&ini, "server", "debug") {
 pass;
}
```

## String Templates

The `std::template` module renders text with `{{name}}` placeholders,
substituting values from a `std::map::Map` of strings. It suits messages,
mail bodies, and configuration snippets — any document whose shape is
fixed and whose values come from a map. A placeholder whose name is not
in the map renders as the empty string; text outside placeholders is
copied verbatim, so arbitrary punctuation and newlines pass through.

### Constants

`std::template::ERR_OUT_OF_MEMORY` (12) is returned when an allocation
fails; `std::template::ERR_INVALID_TEMPLATE` (22) is returned on an
unclosed `{{`.

### Functions

#### `std::template::render`

Signature: `std::template::render(text: Char[], vars: &std::map::Map) -> Str!`

Renders `text`, substituting every `{{name}}` with the value of `name`
in `vars`.

Ownership: the caller owns the result and releases it with
`std::str::destroy`.

Example:

```
let vars = std::map::make(std::str::Str);
defer std::map::destroy(&vars);
let who = try std::str::from("world");
try std::map::put(std::str::Str, &vars, "who", who);
let msg = try std::template::render("Hello, {{who}}!", &vars);
defer std::str::destroy(&msg);
try std::inout::println("{std::str::as_chars(&msg)}");
```

## Non-Cryptographic Hashing

The `std::hash` module provides fast hash functions that are suitable for hash tables, checksums, and integrity checks: CRC-32 (RFC 1952, the polynomial `0xEDB88320` reflected form) and Adler-32 (RFC 1950). These functions are not suitable for cryptographic purposes. The CRC table is computed lazily on first use and then reused.

### Functions

#### `std::hash::crc32`

Signature: `std::hash::crc32(data: &Void, len: UInt64) -> UInt32`

Computes the CRC-32 checksum of the `len` bytes at `data`.

Parameters: `data` is the region to hash; `len` is its size in bytes.

Returns: the checksum.

Example: the following function computes a checksum:

```
func checksum(data: &Void, len: UInt64): UInt32 {
 return std::hash::crc32(data, len);
}
```

#### `std::hash::adler32`

Signature: `std::hash::adler32(data: &Void, len: UInt64) -> UInt32`

Computes the Adler-32 checksum of the `len` bytes at `data`. The sums run modulo 65521 byte by byte.

Parameters: `data` is the region to hash; `len` is its size in bytes.

Returns: the checksum.

Example: the following function computes an Adler-32 checksum:

```
func adler(data: &Void, len: UInt64): UInt32 {
 return std::hash::adler32(data, len);
}
```

## Cryptographic Primitives

The cryptographic primitives live in four focused modules: `std::sha256` and `std::sha512` compute digests, `std::hmac` computes the HMAC-SHA-256 keyed hash, and `std::pbkdf2` derives keys from passwords. Cryptographically secure random bytes come from `std::sysrand`. Integrity against adversaries belongs to these modules; the checksums of `std::hash` do not resist an attacker.

All four digest modules are built only on the language's fixed-width integer arithmetic and allocate nothing except where noted, so only heap allocation can fail.

### Functions

#### `std::sha256::sha256`

Signature: `std::sha256::sha256(data: &Void, len: UInt64, out: Char[32]) -> Void`

Computes the SHA-256 digest (FIPS 180-4) of the `len` bytes at `data` and writes the 32-byte digest to `out`.

Example: the following function hashes a buffer:

```
func hash(data: &Void, len: UInt64): Void {
 let digest: Char[32];
 std::sha256::sha256(data, len, &digest[0]);
}
```

#### `std::sha512::sha512`

Signature: `std::sha512::sha512(data: &Void, len: UInt64, out: Char[64]) -> Void`

Computes the SHA-512 digest (FIPS 180-4) of the `len` bytes at `data` and writes the 64-byte digest to `out`.

#### `std::hmac::hmac_sha256`

Signature: `std::hmac::hmac_sha256(key: &Void, key_len: UInt64, msg: &Void, msg_len: UInt64, out: Char[32]) -> Void!`

Computes the HMAC-SHA-256 code of the `msg_len` bytes at `msg` under the `key_len` bytes at `key` and writes the 32-byte code to `out`. The key is normalized to one 64-byte block as the standard requires.

Errors: the allocator's error (12) is returned when a scratch buffer cannot be allocated; every buffer is released before the function returns.

#### `std::pbkdf2::pbkdf2_sha256`

Signature: `std::pbkdf2::pbkdf2_sha256(password: &Void, pass_len: UInt64, salt: &Void, salt_len: UInt64, iterations: UInt32, out: Char[], dk_len: UInt64) -> Void!`

Derives `dk_len` bytes into `out` from the password and salt over the given iteration count (RFC 2898). The iteration count must be positive and the requested length must fit the 32-bit block index (up to 137 gigabytes).

Errors: error 34 (`ERANGE`) is returned for invalid parameters; the allocator's error (12) on allocation failure.

Example: the following function derives a key:

```
func derive(pw: &Void, pw_len: UInt64, salt: &Void, salt_len: UInt64, out: Char[], n: UInt64): Void! {
 return try std::pbkdf2::pbkdf2_sha256(pw, pw_len, salt, salt_len, 100000, out, n);
}
```

#### `std::sysrand::fill` and `std::sysrand::u64`

Signatures: `std::sysrand::fill(buf: &Void, len: UInt64) -> Void!` and `std::sysrand::u64() -> UInt64!`

Fills `buf` with cryptographically secure random bytes drawn from the kernel random number generator, or draws one secure 64-bit value. May block until entropy is available.

Errors: error 19 (`ENODEV`) is returned when the kernel has no randomness source to offer.

Example: the following function generates a random key:

```
func make_key(key: &Void, n: UInt64): Void! {
 return try std::sysrand::fill(key, n);
}
```

## Database Connectivity

The `std::db` module defines the driver interface for connecting to database engines and executing statements. A database driver is supplied as a package that implements this interface; applications use the same connection, transaction, query, and typed-parameter API with each driver.

### Types

```
static Connection = &ConnHandle;
static Transaction = &TxHandle;
static Statement = &StmtHandle;
static ResultSet = &ResultSetHandle;
```

The types of this module are opaque handles that refer to resources managed by the underlying driver.

### Functions

#### `std::db::connect`

Signature: `std::db::connect(connection_string: Char[]) -> Connection!`

Establishes a connection to a database. The connection string describes the database and the credentials that are required to access it.

Parameters: `connection_string` is the connection string.

Returns: a handle to the connection.

Errors: `std::db::ERR_CONNECTION` is returned if the connection cannot be established.

Example: the following function connects to a database:

```
func open_db(): std::db::Connection! {
 return try std::db::connect("postgresql://localhost/mydb");
}
```

#### `std::db::disconnect`

Signature: `std::db::disconnect(conn: Connection) -> Void!`

Closes the connection.

Parameters: `conn` is the connection to close.

Returns: a `Void` value on success.

Errors: `std::db::ERR_IO` is returned if the connection cannot be closed.

Example: the following function closes a connection:

```
func close_db(conn: std::db::Connection): Void! {
 return try std::db::disconnect(conn);
}
```

#### `std::db::begin_transaction`

Signature: `std::db::begin_transaction(conn: Connection) -> Transaction!`

Begins a new transaction on the given connection.

Parameters: `conn` is the connection.

Returns: a handle to the transaction.

Errors: `std::db::ERR_TRANSACTION` is returned if the transaction cannot be started.

Example: the following function begins a transaction:

```
func start_tx(conn: std::db::Connection): std::db::Transaction! {
 return try std::db::begin_transaction(conn);
}
```

#### `std::db::commit`

Signature: `std::db::commit(tx: Transaction) -> Void!`

Commits the transaction, making its changes permanent.

Parameters: `tx` is the transaction.

Returns: a `Void` value on success.

Errors: `std::db::ERR_TRANSACTION` is returned if the commit fails.

Example: the following function commits a transaction:

```
func finish_tx(tx: std::db::Transaction): Void! {
 return try std::db::commit(tx);
}
```

#### `std::db::rollback`

Signature: `std::db::rollback(tx: Transaction) -> Void!`

Rolls back the transaction, discarding its changes.

Parameters: `tx` is the transaction.

Returns: a `Void` value on success.

Errors: `std::db::ERR_TRANSACTION` is returned if the rollback fails.

Example: the following function rolls back a transaction:

```
func abort_tx(tx: std::db::Transaction): Void! {
 return try std::db::rollback(tx);
}
```

#### `std::db::query`

Signature: `std::db::query(tx: Transaction, sql: Char[]) -> ResultSet!`

Executes a query that returns rows and returns the result set.

Parameters: `tx` is the transaction; `sql` is the query.

Returns: a handle to the result set.

Errors: `std::db::ERR_QUERY` is returned if the query is invalid or fails.

Example: the following function executes a query:

```
func select_all(tx: std::db::Transaction): std::db::ResultSet! {
 return try std::db::query(tx, "SELECT * FROM users");
}
```

#### `std::db::execute`

Signature: `std::db::execute(tx: Transaction, sql: Char[]) -> Int32!`

Executes a statement that modifies the database and returns the number of rows that were affected.

Parameters: `tx` is the transaction; `sql` is the statement.

Returns: the number of rows affected.

Errors: `std::db::ERR_QUERY` is returned if the statement is invalid or fails.

Example: the following function inserts a row:

```
func insert_user(tx: std::db::Transaction, name: Char[]): Int32! {
 let stmt = try std::db::prepare(tx, "INSERT INTO users(name) VALUES(?)");
 defer std::db::finalize(stmt);
 try std::db::bind(Char[], stmt, 1, name);
 return try std::db::execute_prepared(stmt);
}
```

#### `std::db::prepare`

Signature: `std::db::prepare(tx: Transaction, sql: Char[]) -> Statement!`

Prepares a statement for repeated execution.

Parameters: `tx` is the transaction; `sql` is the statement.

Returns: a handle to the prepared statement.

Errors: `std::db::ERR_QUERY` is returned if the statement is invalid.

Example: the following function prepares a statement:

```
func prepare_insert(tx: std::db::Transaction): std::db::Statement! {
 return try std::db::prepare(tx, "INSERT INTO users(name) VALUES(?)");
}
```

#### `std::db::bind`

Signature: `std::db::bind(static T, statement: Statement, index: UInt32, value: T) -> Void!`

Binds one typed parameter to a prepared statement. `index` is one-based.
Supported parameter types are the fixed-width integer types, `Real32`,
`Real64`, `Bool`, NUL-terminated `Char[]`, and byte buffers represented
by a pointer and an explicit byte count. The driver receives the type
and length metadata together with the value; it never infers a parameter
type from an unbounded byte pointer.

Errors: `std::db::ERR_BIND` is returned if the parameter type is not
supported by the driver or the value cannot be bound.

#### `std::db::execute_prepared`

Signature: `std::db::execute_prepared(statement: Statement) -> Int32!`

Executes a prepared statement after its parameters have been bound and
returns the number of affected rows.

#### `std::db::finalize`

Signature: `std::db::finalize(statement: Statement) -> Void!`

Releases the resources held by a prepared statement. Every successful
`prepare` must be paired with exactly one `finalize`.

SQL text passed to `query`, `execute`, or `prepare` is treated as SQL
source, not as a formatting template. Never interpolate untrusted input
into SQL. Use `prepare` and typed `bind` for values supplied by users.

## Application Logging

The `std::log` module provides functions for emitting log messages at various levels of severity. The module maintains a global level threshold; messages below the threshold are discarded.

### Constants

| Constant | Value |
| --- | --- |
| `LEVEL_DEBUG` | 0 |
| `LEVEL_INFO` | 1 |
| `LEVEL_WARNING` | 2 |
| `LEVEL_ERROR` | 3 |
| `LEVEL_FATAL` | 4 |

### Functions

#### `std::log::set_level`

Signature: `std::log::set_level(level: Int32) -> Void!`

Sets the minimum level of messages that the logger will emit.

Parameters: `level` is the new threshold.

Returns: a `Void` value on success.

Errors: `std::log::ERR_INVALID_LEVEL` is returned if the level is not recognised.

Example: the following function sets the log level:

```
func configure_logging(): Void! {
 return try std::log::set_level(std::log::LEVEL_INFO);
}
```

#### `std::log::set_output`

Signature: `std::log::set_output(sink(level: Int32, message: Char[]): Void!) -> Void!`

Sets a custom function to which log messages are written. The function is of type `func(level: Int32, message: Char[]): Void!`.

Parameters: `sink` is the custom output function.

Returns: a `Void` value on success.

Errors: `std::log::ERR_INVALID_SINK` is returned if the function is not valid.

Example: the following function redirects log output:

```
func custom_sink(level: Int32, message: Char[]): Void! {
 return try std::inout::println("[{level}] {message}");
}

func redirect_logs(): Void! {
 return try std::log::set_output(custom_sink);
}
```

#### `std::log::debug`

Signature: `std::log::debug(message: Char[]) -> Void!`

Emits a message at the `DEBUG` level.

Parameters: `message` is the message.

Returns: a `Void` value on success.

Errors: `std::log::ERR_IO` is returned if the message cannot be written.

Example: the following function logs a debug message:

```
func trace(): Void! {
 return try std::log::debug("Entering function");
}
```

#### `std::log::info`

Signature: `std::log::info(message: Char[]) -> Void!`

Emits a message at the `INFO` level.

Parameters: `message` is the message.

Returns: a `Void` value on success.

Errors: `std::log::ERR_IO` is returned if the message cannot be written.

Example: the following function logs an information message:

```
func start(): Void! {
 return try std::log::info("Application started");
}
```

#### `std::log::warning`

Signature: `std::log::warning(message: Char[]) -> Void!`

Emits a message at the `WARNING` level.

Parameters: `message` is the message.

Returns: a `Void` value on success.

Errors: `std::log::ERR_IO` is returned if the message cannot be written.

Example: the following function logs a warning:

```
func warn(): Void! {
 return try std::log::warning("Configuration value missing");
}
```

#### `std::log::error`

Signature: `std::log::error(message: Char[]) -> Void!`

Emits a message at the `ERROR` level.

Parameters: `message` is the message.

Returns: a `Void` value on success.

Errors: `std::log::ERR_IO` is returned if the message cannot be written.

Example: the following function logs an error:

```
func fail(): Void! {
 return try std::log::error("Operation failed");
}
```

#### `std::log::fatal`

Signature: `std::log::fatal(message: Char[]) -> Void!`

Logs the message at the `FATAL` level and returns normally. The caller is responsible for propagating the error with `return error` if the failure is to be reported to `main`.

Parameters: `message` is the message.

Returns: a `Void` value on success.

Errors: `std::log::ERR_IO` is returned if the message cannot be written.

Example: the following function logs a fatal message and propagates an error:

```
func die(): Void! {
 try std::log::fatal("Unrecoverable error");
 return error 1;
}
```

## Framebuffer Surfaces

The `std::window` module provides off-screen pixel surfaces presented to the Linux framebuffer by `std::graphics`. A surface is a rectangular buffer with a title and a visibility flag; surfaces draw into their own buffers at any time, and `present` paints the visible ones centered over the screen. There is no window-manager negotiation and no decoration rendering; input events are not part of this module — they come from `std::input`.

### Types

```
static Win = &WinHandle;
```

The `Win` type is opaque. It is created by `std::window::create` and released by `std::window::destroy`.

### Functions

#### `std::window::create`

Signature: `std::window::create(title: Char[], width: Int32, height: Int32) -> Win!`

Creates a window of `width` by `height` pixels with the given title; the buffer starts cleared to black and the window starts hidden.

Returns: the opaque handle.

Errors: `std::window::ERR_NO_DISPLAY` (19) is returned when the allocations fail.

Example: the following function creates a window:

```
func make_window(): std::window::Win! {
 return try std::window::create("My Application", 800, 600);
}
```

#### `std::window::destroy`

Signature: `std::window::destroy(window: Win) -> Void!`

Releases the title copy, the pixel buffer, and the handle.

Errors: `std::window::ERR_INVALID_WINDOW` (22) is returned for a zero or already destroyed handle.

#### `std::window::show` and `std::window::hide`

Signatures: `std::window::show(window: Win) -> Void!` and `std::window::hide(window: Win) -> Void!`

Make the window visible or invisible to the next `present`.

#### `std::window::put_pixel`

Signature: `std::window::put_pixel(window: Win, x: UInt64, y: UInt64, r: Char, g: Char, b: Char) -> Void!`

Draws one pixel into the window's own buffer; out-of-range coordinates are ignored, matching the graphics clipping rule.

#### `std::window::fill`

Signature: `std::window::fill(window: Win, r: Char, g: Char, b: Char) -> Void!`

Fills the window buffer with one color.

#### `std::window::present`

Signature: `std::window::present(window: Win, ctx: std::graphics::GraphicsContext) -> Void!`

Blits the window's buffer onto the framebuffer, centered. Hidden windows are skipped.

Example: the following function shows a window with a blue background:

```
func show_blue(ctx: std::graphics::GraphicsContext): Void! {
 let w = try std::window::create("Demo", 640, 480);
 defer std::window::destroy(w);
 try std::window::fill(w, 0, 0, 255);
 try std::window::show(w);
 try std::window::present(w, ctx);
}
```

## User Input

The `std::input` module reads the Linux event device interface (`/dev/input/eventN`): every keyboard, mouse, and gamepad reports its activity as a stream of fixed-size event records, so one module serves them all. The record is 24 bytes: a 16-byte timestamp this module skips, then a 16-bit event type, a 16-bit code identifying the key or axis, and a 32-bit signed value (pressed versus released, movement delta, wheel steps).

### Types

```
static Event = Struct {
 type: UInt16,
 code: UInt16,
 value: Int32,
};
```

### Constants

The most common kernel event codes, mirrored so callers rarely need the kernel headers: `EV_KEY` (1), `EV_REL` (2), `KEY_ESC` (1), `KEY_ENTER` (28), `KEY_SPACE` (57), `KEY_LEFT` (105), `KEY_RIGHT` (106), `KEY_UP` (103), `KEY_DOWN` (108). The full set lives in the kernel's input-event-code header.

### Functions

#### `std::input::open_device`

Signature: `std::input::open_device(path: Char[]) -> UInt64!`

Opens the event device at `path` for reading, non-blocking, and returns its descriptor.

Errors: `std::input::ERR_NO_DEVICE` (19) is returned when the device cannot be opened.

#### `std::input::close_device`

Signature: `std::input::close_device(fd: UInt64) -> Void!`

Closes a descriptor returned by `open_device`.

#### `std::input::poll_event`

Signature: `std::input::poll_event(fd: UInt64, ev: &Event) -> Bool!`

Reads one event without blocking. Returns `true` and fills `ev` when an event was pending; returns `false` when the queue is empty. Read errors other than an empty queue propagate.

Example: the following function drains the pending events:

```
func drain(fd: UInt64): Void! {
 let ev: std::input::Event;
 while try std::input::poll_event(fd, &ev) {
    if ev.type == std::input::EV_KEY and ev.value == 1 {
      try std::inout::println("Key pressed");
    }
 }
}
```

#### `std::input::wait_event`

Signature: `std::input::wait_event(fd: UInt64, ev: &Event, timeout_ns: UInt64) -> Bool!`

Waits up to `timeout_ns` for one event, filling `ev` and returning `true` when one arrives; returns `false` on timeout. The deadline is measured on the monotonic clock, checking with a one-millisecond cadence.

## Graphics

The `std::graphics` module renders directly into the Linux console framebuffer (`/dev/fb0`): the kernel exposes the video memory as a mmap-able character device, so drawing is writing pixels into mapped memory. The module opens the device, learns the screen geometry and pixel layout through the `fb_var_screeninfo` and `fb_fix_screeninfo` ioctls, maps the memory, and from then on every drawing call is plain pointer arithmetic. Only 32-bit packed pixels (one byte each of blue, green, red, and an ignored pad byte) are served.

### Types

```
static GraphicsContext = Struct {
 fb: &Void,
 width: UInt64,
 height: UInt64,
 pitch: UInt64,
 fd: UInt64,
 map_len: UInt64,
};
```

### Functions

#### `std::graphics::create_context`

Signature: `std::graphics::create_context() -> GraphicsContext!`

Opens `/dev/fb0`, reads the screen geometry, and maps the video memory.

Returns: the mapped context.

Errors: `std::graphics::ERR_NO_DEVICE` (19) is returned when no framebuffer exists, the ioctls fail, or the pixel depth is not 32 bits.

Example: the following function creates a context:

```
func make_context(): std::graphics::GraphicsContext! {
 return try std::graphics::create_context();
}
```

#### `std::graphics::destroy_context`

Signature: `std::graphics::destroy_context(ctx: &GraphicsContext) -> Void!`

Unmaps the video memory and closes the device.

#### `std::graphics::put_pixel`

Signature: `std::graphics::put_pixel(ctx: GraphicsContext, x: UInt64, y: UInt64, r: Char, g: Char, b: Char) -> Void!`

Sets the pixel at `(x, y)` to the given color. Drawing calls silently clip at the screen edges.

#### `std::graphics::fill_rect`

Signature: `std::graphics::fill_rect(ctx: GraphicsContext, x: UInt64, y: UInt64, w: UInt64, h: UInt64, r: Char, g: Char, b: Char) -> Void!`

Fills the rectangle from `(x, y)` with the given size and color, clipped to the screen.

#### `std::graphics::draw_line`

Signature: `std::graphics::draw_line(ctx: GraphicsContext, x0: UInt64, y0: UInt64, x1: UInt64, y1: UInt64, r: Char, g: Char, b: Char) -> Void!`

Draws a straight line from `(x0, y0)` to `(x1, y1)` by Bresenham's algorithm.

#### `std::graphics::clear`

Signature: `std::graphics::clear(ctx: GraphicsContext, r: Char, g: Char, b: Char) -> Void!`

Fills the whole screen with the given color.

Example: the following function draws a red diagonal:

```
func draw(ctx: std::graphics::GraphicsContext): Void! {
 try std::graphics::clear(ctx, 0, 0, 0);
 try std::graphics::draw_line(ctx, 0, 0, ctx.width - 1, ctx.height - 1, 255, 0, 0);
}
```

## Images

The `std::image` module creates, loads, and saves pixel images. The file format is BMP: uncompressed 24- and 32-bit on load, 24-bit on save — the uncompressed baseline a syscall-only library can parse exactly. Loaded images are normalized to the module's RGBA8 layout. Pixel data is owned by the `Image` value: every constructor allocates it, `destroy` releases it.

### Types

```
static Image = Struct {
 width: UInt64,
 height: UInt64,
 format: UInt32,
 data: Char[],
};
```

Formats: `RGBA8` (4 bytes per pixel, the canonical layout), `RGB8` (3 bytes), and `GRAYSCALE8` (1 byte).

### Functions

#### `std::image::create`

Signature: `std::image::create(width: UInt64, height: UInt64, format: UInt32) -> Image!`

Creates a blank image with every pixel zeroed.

Errors: the allocator's error (12) is returned when the allocation fails.

#### `std::image::create_from_data`

Signature: `std::image::create_from_data(width: UInt64, height: UInt64, format: UInt32, data: Char[]) -> Image!`

Creates an image from existing pixel data, copying `width * height * pixel-size` bytes.

#### `std::image::load`

Signature: `std::image::load(path: Char[]) -> Image!`

Loads a 24- or 32-bit uncompressed BMP from `path` and returns it as an RGBA8 image. The 14-byte file header gives the pixel-data offset; the 40-byte info header gives width, signed height (negative means top-down rows), the bit count, and the compression field, which must be zero. Rows are padded to four bytes and stored blue-green-red; alpha is ignored on 32-bit input and set to 255 for 24-bit.

Errors: `std::image::ERR_NOT_FOUND` (2) is returned when the file cannot be opened; `std::image::ERR_UNSUPPORTED_FORMAT` (22) for any other format violation.

Example: the following function loads an image:

```
func load_image(path: Char[]): std::image::Image! {
 return try std::image::load(path);
}
```

#### `std::image::save`

Signature: `std::image::save(img: &Image, path: Char[]) -> Void!`

Saves `img` as a 24-bit BMP at `path`; RGBA8 and RGB8 sources are converted, alpha dropped. Other formats report `ERR_UNSUPPORTED_FORMAT` (22).

Example: the following function saves an image:

```
func save_image(img: &std::image::Image, path: Char[]): Void! {
 return try std::image::save(img, path);
}
```

#### `std::image::destroy`

Signature: `std::image::destroy(img: &Image) -> Void`

Releases the pixel buffer and resets the value; it must not be used afterwards.

## Audio

The `std::audio` module uses the Linux OSS-compatible `/dev/dsp` playback interface. The device accepts a stream of interleaved samples; the module negotiates sample format, rate, and channel count before playback. Stream parameters are negotiated with three ioctls — sample format, sample rate, channel count — and each negotiation is verified, reporting `ERR_UNSUPPORTED_FORMAT` when the hardware refused the requested value.

### Types

```
static AudioDevice = Struct {
 fd: UInt64,
};

static AudioStream = Struct {
 fd: UInt64,
 rate: UInt32,
 channels: UInt32,
 format: UInt32,
};
```

### Constants

| Constant | Meaning |
| --- | --- |
| `FORMAT_S16` | Signed 16-bit PCM |
| `FORMAT_F32` | 32-bit floating point |

### Functions

#### `std::audio::open_device`

Signature: `std::audio::open_device() -> AudioDevice!`

Opens the default output device `/dev/dsp`.

Errors: `std::audio::ERR_NO_DEVICE` (19) is returned if no audio device is available.

#### `std::audio::close_device`

Signature: `std::audio::close_device(dev: AudioDevice) -> Void!`

Closes the device and every stream bound to it.

Errors: `std::audio::ERR_INVALID_DEVICE` (22) is returned for a zero handle.

#### `std::audio::create_stream`

Signature: `std::audio::create_stream(dev: AudioDevice, rate: UInt32, channels: UInt32, format: UInt32) -> AudioStream!`

Creates a playback stream with the given sample rate, channel count, and sample format. All three parameters are negotiated with the device and the returned values must equal the requests.

Errors: `std::audio::ERR_UNSUPPORTED_FORMAT` (22) is returned when any negotiation is refused.

Example: the following function creates a stereo stream:

```
func make_stream(dev: std::audio::AudioDevice): std::audio::AudioStream! {
 return try std::audio::create_stream(dev, 44100, 2, std::audio::FORMAT_S16);
}
```

#### `std::audio::write`

Signature: `std::audio::write(stream: AudioStream, data: &Void, bytes: UInt64) -> Void!`

Queues `bytes` of sample data for playback: interleaved samples of the stream's format, two bytes per sample for `FORMAT_S16` and four for `FORMAT_F32`, times the channel count. Short writes are retried until every byte is queued.

Example: the following function plays a buffer:

```
func play_buffer(stream: std::audio::AudioStream, buf: &Void, n: UInt64): Void! {
 return try std::audio::write(stream, buf, n);
}
```

#### `std::audio::close_stream`

Signature: `std::audio::close_stream(stream: AudioStream) -> Void!`

Ends the stream's use; the device descriptor stays open for further streams and is released by `close_device`. The OSS model has no separate per-stream resource.

## Low-Level Native Threads

The `std::thread` module provides direct access to Linux clone tasks. A task receives a fresh stack and shares the process address space and selected process resources with its parent; it has its own task identifier and is joined with `wait4`. The clone flags and wait model define a shared-address-space task rather than a POSIX `pthread` object. The module is deliberately low-level; programs that can use the language-level concurrency primitives should prefer them.

### Types

```
static Thread = Struct {
 tid: UInt64,
 stack: &Void,
};
```

The `Thread` value identifies one spawned clone task by its kernel task id and owns the 1 MiB stack allocated for it; `join` waits for the task to exit and releases the stack. The task must be joined exactly once.

### Functions

#### `std::thread::spawn`

Signature: `std::thread::spawn(entry: func(): UInt64) -> Thread!`

Spawns a thread that runs `entry` on its own stack. The entry function's return value becomes the thread's exit code. Because the child continues on a fresh stack, `spawn` inlines the whole clone sequence and enters the entry through a trampoline; a handshake cell guarantees that concurrent spawns cannot overwrite each other's entry function.

Errors: `std::thread::THREAD_ERR_CANNOT_CREATE` (11) is returned when the kernel refuses the clone; the allocator's error (12) when the stack cannot be allocated. The stack is released when the spawn is refused.

Example: the following function creates a native thread:

```
func launch_worker(): std::thread::Thread! {
 return try std::thread::spawn(worker);
}
```

#### `std::thread::join`

Signature: `std::thread::join(t: Thread) -> UInt64!`

Waits for `t` to exit, releases its stack, and returns the low byte of its exit status.

Errors: `std::thread::THREAD_ERR_INVALID_THREAD` (22) is returned for a zero handle; `std::thread::THREAD_ERR_CANNOT_JOIN` (3) when the kernel cannot wait on the id.

#### `std::thread::yield`

Signature: `std::thread::yield() -> Void`

Yields the CPU to the scheduler.

#### `std::thread::current`

Signature: `std::thread::current() -> UInt64!`

Returns the kernel thread id of the calling thread.

## Atomic Operations

The `std::atomic` module provides atomic loads, stores, read-modify-write
operations, and fences with explicit memory ordering. It exists for code
beneath the channel-based concurrency model of the language: implementing
`std::sync`, sleeping on a futex, and building lock-free structures that
interoperate with foreign code. Programs that use `routine`, `async`, and
`chan` do not need it — captures are by value and communication is
through channels, so shared mutable state does not arise in ordinary
concurrent code.

### Types

```
static Order = Enum {
 Relaxed,   // no ordering constraint
 Acquire,   // later memory operations stay after this operation
 Release,   // earlier memory operations stay before this operation
 AcqRel,   // both Acquire and Release
 SeqCst,   // sequentially consistent
};
```

The orderings follow acquire/release semantics. `Acquire` prevents
subsequent memory operations from moving before the acquire operation;
`Release` prevents preceding memory operations from moving after the
release operation; `AcqRel` combines both constraints. `Relaxed` is
right for independent counters, while `SeqCst` provides a single total
order across sequentially consistent atomic operations.

### Functions

All operations are atomic with respect to every other thread. The
element type `T` is a compile-time parameter over the integer types,
`Bool`, and pointers.

#### `std::atomic::load`, `std::atomic::store`

Signatures: `std::atomic::load(static T, p: &T, order: Order) -> T`,
`std::atomic::store(static T, p: &T, v: T, order: Order) -> Void`

Atomically read or write the value at `p` with the given ordering.

#### `std::atomic::add`, `std::atomic::sub`

Signatures: `std::atomic::add(static T, p: &T, v: T, order: Order) -> T`,
`std::atomic::sub(static T, p: &T, v: T, order: Order) -> T`

Atomically add or subtract `v` and return the value the location held
before the operation. `T` must be an integer type.

#### `std::atomic::cas`

Signature: `std::atomic::cas(static T, p: &T, expected: &T,
desired: T, success_order: Order, failure_order: Order) -> Bool`

Compare-and-swap: when `*p` equals `*expected`, writes `desired` and
returns `true` using `success_order`; otherwise stores the observed
value into `*expected` and returns `false` using `failure_order`. The
failure order may be `Relaxed`, `Acquire`, or `SeqCst`; it may not be
`Release` or `AcqRel`, and it may not impose a stronger ordering than
the success order. Use `AcqRel`/`Acquire` when a successful swap
publishes data and a failed comparison must observe published data.

#### `std::atomic::fence`

Signature: `std::atomic::fence(order: Order) -> Void`

A stand-alone memory fence. A fence is heavier than the ordering
attached to an operation and is rarely needed; prefer ordered
loads and stores.

Example:

```
static Counter = Struct {
 value: UInt64,
 mu: std::sync::Mutex,
};

func bump(c: &Counter): Void! {
 // Lock-free fast path, mutex fallback.
 let expected: UInt64 = std::atomic::load(
    UInt64, &c.mu.locked, Order.Relaxed);
 while expected != 0 {
    try std::sync::lock(&c.mu);
    c.value += 1;
    return try std::sync::unlock(&c.mu);
 }
 if std::atomic::cas(UInt64, &c.mu.locked, &expected, 1,
      Order.Acquire, Order.Relaxed) {
    c.value += 1;
    std::atomic::store(UInt64, &c.mu.locked, 0, Order.Release);
    return;
 }
 pass;
}
```

## Synchronisation Primitives

The `std::sync` module provides the mutex, built directly on the futex system call: `lock` atomically claims the state word and sleeps on the futex when the mutex is held, `unlock` wakes one waiter. The module pairs with `std::thread`; programs that use language-level concurrency communicate through channels instead.

### Types

```
static Mutex = Struct {
 locked: UInt64,
};
```

The `Mutex` is a plain value; the zero value produced by default initialization is a free mutex, so a `Mutex` field needs no constructor.

### Functions

#### `std::sync::make`

Signature: `std::sync::make() -> Mutex`

Returns a free mutex.

#### `std::sync::lock`

Signature: `std::sync::lock(m: &Mutex) -> Void!`

Acquires the mutex, blocking until it is free. A futex error propagates with its errno.

#### `std::sync::unlock`

Signature: `std::sync::unlock(m: &Mutex) -> Void!`

Releases the mutex and wakes one waiter when waiters exist.

Example: the following function guards a counter:

```
static Counter = Struct {
 mu: std::sync::Mutex,
 n: UInt64,
};

func bump(c: &Counter): Void! {
 try std::sync::lock(&c.mu);
 c.n += 1;
 try std::sync::unlock(&c.mu);
}
```

## Compression

The `std::inflate` module decompresses raw DEFLATE streams (RFC 1951) — the compression format at the heart of gzip, zlib, and zip members. The decoder follows the classic count/symbols canonical-Huffman design: each code is described by how many symbols use each code length, and decoding walks the lengths one bit at a time, narrowing the candidate range until one symbol remains. No per-length lookup tables are built, which keeps the code and its stack footprint small at the cost of a few extra bit reads per symbol. The block-decoding machinery lives in the internal module `std::inflate_block`.

### Functions

#### `std::inflate::inflate`

Signature: `std::inflate::inflate(src: Char[], src_len: UInt64, dst: Char[], dst_cap: UInt64) -> UInt64!`

Decompresses the DEFLATE stream at `src[0..src_len]` into `dst[0..dst_cap]` and returns the number of bytes written.

Errors: error 22 covers every malformed stream — bad block type, over-subscribed or invalid Huffman codes, and references past the start of the output; error 27 reports a destination too small for the unpacked content.

Example: the following function decompresses a buffer:

```
func decompress(src: Char[], src_len: UInt64, dst: Char[], cap: UInt64): UInt64! {
 return try std::inflate::inflate(src, src_len, dst, cap);
}
```

## Terminal Control

The `std::term` module controls the terminal line discipline through the `TCGETS`/`TCSETS` ioctls. Its central operation is raw mode: `enter_raw` saves the current `Termios` structure, clears the `ICANON` and `ECHO` flags, and sets `VMIN` to 1 and `VTIME` to 0, so input arrives character by character and is not echoed; `restore` puts the saved structure back. Callers are expected to schedule the restore with `defer` immediately after entering raw mode.

### Types

```
static Termios = Struct {
 iflag: UInt32,
 oflag: UInt32,
 cflag: UInt32,
 lflag: UInt32,
 line: Char,
 cc: Char[19],
 ispeed: UInt32,
 ospeed: UInt32,
};
```

The `Termios` structure mirrors the kernel's `struct termios`; the control-character array `cc` is indexed by the kernel's `VMIN` (6) and `VTIME` (5) constants, which the module exports.

### Functions

#### `std::term::get`

Signature: `std::term::get(fd: UInt32) -> Termios!`

Reads the terminal configuration of the descriptor, which is usually 0 for standard input.

Errors: the kernel's error is returned; 25 (`ENOTTY`) when the descriptor is not a terminal.

#### `std::term::set`

Signature: `std::term::set(fd: UInt32, t: Termios) -> Void!`

Applies a terminal configuration to the descriptor.

#### `std::term::enter_raw`

Signature: `std::term::enter_raw(fd: UInt32) -> Termios!`

Saves the current configuration, applies raw mode, and returns the saved configuration for the matching `restore`.

Example: the following function reads one key press in raw mode:

```
func read_key(): Void! {
 let saved = try std::term::enter_raw(0);
 defer std::term::restore(0, saved);
 let c: Char;
 try std::sys::read(0, &c as &Void, 1);
}
```

#### `std::term::restore`

Signature: `std::term::restore(fd: UInt32, saved: Termios) -> Void!`

Restores a configuration previously returned by `enter_raw`.

## Postscript

The standard library follows two conventions throughout. The first is the resource management convention: every function that acquires a resource returns a handle, and a corresponding function releases it. The `create` and `destroy` pairs, the `open` and `close` pairs, and the `load` and `destroy` pairs are all instances of this pattern. Programs are expected to schedule the release of a resource with a `defer` statement placed immediately after the acquisition, so that the resource is released regardless of how the surrounding function exits.

The second is the error handling convention: every function that can fail returns a type suffixed with `!`, and the error value is an integer. The library does not use exceptions, because exceptions introduce control-flow paths that are invisible in the source and that complicate the reasoning about resource lifetimes. An integer error code is visible in the type of the function, is easy to compare against the documented constants, and does not require a separate mechanism for unwinding the stack.

The `std::thread` module is deliberately low-level. It exposes the operating system's threads directly, and it expects the program to manage the synchronisation of shared state with the primitives in `std::sync`. Most programs should not use these modules directly; the language-level concurrency primitives, `routine`, `async`, and `chan`, provide a safer and more structured alternative in which state is not shared and communication is explicit. The low-level modules exist for the programs that need to interoperate with code that was not written in Paxsy, or that must use operating system facilities that the higher-level primitives do not expose.

