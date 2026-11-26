## Guiding Principles

The rules in this guide are derived from a small number of principles. When a specific rule does not cover a situation, the principles below should be consulted and applied in the spirit in which they are written. Every other section of this document is an elaboration of these principles in a particular domain.

### Readability over Cleverness

Code is read far more often than it is written. A function is typically written once and read dozens or hundreds of times over its lifetime, by its original author and by others who must maintain it. The cost of writing code is therefore dominated by the cost of reading it, and any saving in keystrokes at the moment of writing that increases the difficulty of reading later is a false economy.

The implication of this principle is that clarity should be preferred to brevity whenever the two conflict. A variable whose name is `remaining_capacity` is better than one named `rc`, even though the shorter name takes fewer characters to type, because the reader of the code does not have to reconstruct the meaning of the abbreviation. A function that is broken into several short, well-named steps is better than a single long function, even though the long function has fewer lines, because the reader can understand each step in isolation. A construct that requires a comment to explain what it does should be rewritten so that the comment is unnecessary.

This principle does not mean that code should be verbose for its own sake. It means that the reader's time is more valuable than the writer's, and that the writer should therefore invest effort in making the code easy to read.

### Explicit Is Better Than Implicit

A reader should be able to understand the behaviour of a piece of code without consulting external documentation or inferring hidden state. Whenever a choice exists between making something explicit and leaving it implicit, the explicit choice should be preferred.

In practice, this principle manifests in several ways. Public APIs should annotate the types of their parameters and their return values, so that a caller can understand the interface without reading the implementation. Struct fields should annotate their types, so that the layout of the struct is clear from its definition. Module-level variables should annotate their types, so that the reader does not have to trace the flow of assignments to determine what type the variable has. When a type is obvious from the initializer and the variable is a local binding, the `Auto` inference mechanism may be used, because the reader can see the type by looking at the initializer on the same line; but even here, explicit annotation is never wrong and is sometimes preferable for the sake of uniformity.

The principle of explicitness also applies to error handling. A function that can fail should say so in its return type, using the `!` suffix. A caller that propagates an error should use `try`; a caller that handles an error should use `catch`. Silently ignoring an error is never acceptable, because it hides a behaviour from the reader and from the compiler.

### Fail Fast and Visibly

An error that occurs during the execution of a program should be detected as close as possible to its source and reported as clearly as possible to the caller. Errors that are detected early are easier to diagnose than errors that are detected late, and errors that are reported clearly are easier to fix than errors that are reported obscurely.

Every resource acquisition must be paired with a cleanup path. When a function opens a file, allocates memory, or acquires a lock, it must ensure that the resource is released on every exit path, whether the function returns normally or propagates an error. The `defer` statement is the idiomatic mechanism for this pairing, and its use is strongly encouraged.

Inputs should be validated at the point of entry to a function, and errors should be returned immediately. A function that checks its preconditions at the beginning is easier to read and easier to trust than one whose preconditions are scattered throughout its body. When a precondition is violated, the function should return an error rather than continue with a value that is known to be invalid.

### Minimalism

Paxsy is a systems programming language, and systems programs are written close to the machine. Abstractions have a cost, both in the code that implements them and in the cognitive load that they place on the reader, and that cost should be justified by a corresponding benefit.

This principle does not mean that abstractions are forbidden. It means that an abstraction should be introduced when it solves a real problem, not when it merely reorganises code. A struct that groups related fields is a good abstraction; a wrapper type that adds no behaviour to the value it wraps is not. A function that factors out common logic is a good abstraction; a function that merely renames an existing operation is not.

When in doubt, prefer the simpler construct. If a language feature can express an idea directly, use that feature rather than building a layer on top of it.

### English Identifiers

All identifiers, comments, and documentation in Paxsy code must be written in English. This rule ensures that code can be read and maintained by programmers around the world, and it is consistent with the convention established by the standard library and the language specification.

The rule applies to variable names, function names, type names, module names, and any other identifier that the programmer chooses. It also applies to comments and documentation, including documentation comments and inline comments. The rule does not apply to string literals that are part of the program's data, which may contain text in any language that the program supports.

## File Organisation

The organisation of a source file follows a fixed pattern. The file begins with a header comment that identifies the file and its purpose. Imports follow the header, grouped by origin. Declarations follow the imports, grouped by kind. Each of these elements is described in the subsections that follow.

### The File Header

Every file with the extension `.px` must begin with a multi-line comment that identifies the file and its purpose. The header serves several purposes: it records the copyright and license under which the file is distributed, it describes the file's contents in a single sentence, and it names the module that the file declares if the file declares a named module.

The header comment must contain, in order, a copyright and license line, a one-sentence description of the file's purpose, and, if the file declares a named module, a line identifying the module. The following header is correct:

```
/*
 * Copyright (C) 2026 Example Corp
 * Distributed under the Apache License, Version 2.0.
 *
 * Module: user_management
 * Provides user authentication and profile management functions.
 */
```

The following header is not acceptable, because it omits the copyright and license information, uses an informal description, and does not identify the module:

```
// user_management.px
// does auth stuff
```

The copyright and license line should name the copyright holder and the license under which the file is distributed. The description should be a single sentence that explains what the file contains. The module line should name the module if the file declares one, so that the reader can find the module's declarations without scanning the file.

### Import Ordering

Imports, written with the `use` directive, must appear at the top of the file, immediately after the header comment. They must be grouped by origin, with a blank line between each group. The first group contains imports from the standard library. The second group contains imports from third-party libraries. The third group contains imports from local modules within the same project.

The following block of imports demonstrates the correct ordering:

```
use std::inout;
use std::file;
use std::time;

use crypto::aes;
use web::http;

use local::database;
use local::config;
```

Within each group, imports should be sorted alphabetically by the path of the module. The following block of imports is not acceptable, because the groups are interleaved and the individual imports are not sorted:

```
use local::config;
use std::file;
use crypto::aes;
use std::inout;
```

The `use` directive has two forms: `use module;` imports all public symbols of a module, and `use module::symbol;` imports a single symbol. A file that uses only one or two symbols from a module should import those symbols individually; a file that uses many symbols from a module may import the module as a whole. Wildcard imports beyond the first form do not exist in Paxsy, and group imports such as `use module::{a, b};` are not part of the language.

### Declaration Order

Within a file, declarations should be grouped by kind and ordered as follows. First come `static` type definitions, including structs, unions, enums, and type aliases. Next come `static` constants and type aliases. Next come `extern` prototypes, which declare functions or variables that are defined elsewhere. Next come ordinary function implementations. Finally come `routine` definitions, if any.

Logical groups within these categories should be separated by a single blank line. The order ensures that a reader who is scanning the file from top to bottom encounters the definitions of types before the functions that use them, and the definitions of helper functions before the functions that call them.

## Formatting

Formatting rules are the least interesting part of a style guide, but they are also the rules that most affect the visual consistency of a codebase. A file that follows the formatting rules in this section looks like every other file in the project, and a reader can focus on the logic rather than on the layout. The rules in this section are enforced mechanically by the `paxsy fmt` command described in the [Toolchain User Manual](toolchain-user-manual.md), which rewrites a file to match them; programmers are expected to run it before committing, and continuous integration is expected to run it with `--check`.

### Indentation

Indentation is performed with four spaces per level. Tabs are never used. The choice of four spaces rather than two or eight is a compromise between the readability of deeply nested code and the amount of horizontal space consumed by each level. Two spaces produce code that is difficult to scan at a glance because the levels are not visually distinct; eight spaces cause deeply nested code to run off the right side of the screen. Four spaces are a widely used middle ground.

The prohibition of tabs is deliberate. Different editors display tabs with different widths, so a file that uses tabs looks different to different readers. Spaces are unambiguous: four spaces are four spaces in every editor. Mixing tabs and spaces in the same file is especially harmful, because the file's appearance depends on the tab width assumed by the reader's editor.

### Line Length

Lines must not exceed eighty characters. The limit is a convention that allows two files to be viewed side by side on a standard display, and it encourages the programmer to break complex expressions into simpler ones. A line that is difficult to keep within eighty characters is often a sign that the expression on the line should be factored into a helper function or a named intermediate variable.

When a line must be broken, it should be broken at a natural boundary. For a function call, the arguments should be broken one per line, with the opening parenthesis remaining on the line that contains the function name and each argument indented by four spaces relative to the function call:

```
let result = calculate_complex_value(
 first_parameter,
 second_parameter,
 third_parameter,
);
```

For a long arithmetic expression, the line should be broken after an operator, so that the reader's eye can follow the continuation:

```
let total = first_component
 + second_component
 + third_component;
```

The following line is not acceptable, because it exceeds eighty characters and does not break at a natural boundary:

```
let result = calculate_complex_value(first_parameter, second_parameter, third_parameter);
```

### Brace Style

The opening brace of a block is placed on the same line as the statement that introduces it, preceded by a single space. The closing brace is placed on its own line, aligned with the first character of the statement. This style is known as the K&R style or the Egyptian style.

The following examples demonstrate the correct placement of braces for the constructs that introduce blocks:

```
if condition {
 statements;
} else {
 statements;
}

func example(param: Int32): Void {
 statements;
}

match value {
 case 1 { statements; }
 case 2 { statements; }
 else { statements; }
}
```

The following examples are not acceptable, because the opening brace is placed on a line by itself:

```
if condition
{
 statements;
}

func example(param: Int32): Void
{
 statements;
}
```

The rule applies without exception. A struct definition, a union definition, an enum definition, a function body, a loop body, a conditional body, and a `match` arm all use the same brace style.

### Blank Lines

Blank lines are used to separate logical groups of code. A single blank line is placed between function definitions, between logical groups of declarations within a module, and between the major sections of a long function body. No more than one consecutive blank line is used anywhere in a file; two or more blank lines are never necessary and are visually distracting.

The following example demonstrates the correct use of blank lines between declarations:

```
static MAX_SIZE = 1024;

static Status = Enum {
 Ok = 0,
 Error = 1,
};

func first_function(): Void {
 // ...
}

func second_function(): Void {
 // ...
}
```

A blank line is placed between the constant and the type definition, between the type definition and the first function, and between the two functions. No blank lines are placed inside the struct definition or inside any function body unless they separate logical groups within that body.

### Spacing

Spacing around operators and keywords follows the rules in the following table. The rules ensure that the code is visually consistent and that operators are easy to distinguish from their operands.

| Context | Rule | Example |
| --- | --- | --- |
| Keywords | One space after `if`, `while`, `for`, `match`, `return`, `func`, `let`, `static`, `routine`, `async`, `await`, `select`, `close`, `defer`, `pass` | `if condition {` |
| Binary operators | One space on each side of `=`, `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `and`, `or`, `as`, `$`, `&`, `^`, `<<`, `>>`, `>>>`, `<-` | `a + b`, `x as Int32` |
| Unary operators | No space after `@`, `&` (address-of), `~`, `not`, unary `-` | `@ptr`, `&value`, `not flag` |
| Error suffix | No space between the type and `!` | `Int32!`, `Void!` |
| Semicolons | No space before `;` | `let x = 5;` |
| Commas | One space after each comma in a list | `{1, 2, 3}` |
| Type annotations | No space before `:`; one space after | `let x: Int32` |
| Pointer types | No space between `&` and the type | `let p: &Int32` |
| Array types | The size follows the element type, in brackets | `let x: Int32[4]` |

The following code demonstrates the correct application of these rules:

```
let sum: Int32 = a + b;
let ptr: &Int32 = &value;
@ptr = 10;
let result: Int32! = divide(a, b);
```

The following code is not acceptable, because it violates several spacing rules at once:

```
let sum:Int32=a+b;
let ptr : & Int32 = & value;
@ ptr = 10;
```

## Naming

Names are the primary means by which a reader understands the purpose of a piece of code. A well-chosen name communicates what a value represents or what a function does, and it does so without requiring the reader to consult the implementation. A poorly chosen name obscures the code's meaning and forces the reader to reconstruct it from context. The rules in this section are therefore among the most important in the guide.

### Variables and Functions

Variables, function names, and parameter names use `snake_case`: all lowercase letters, with underscores between words. This convention is used throughout the standard library and is the one that most programmers find easiest to read for multi-word identifiers.

```
let user_name = "Alice";
let max_connections = 100;

func calculate_total(items: Item[]): Int32 {
 // ...
}

func process_user_request(request: Request): Response {
 // ...
}
```

Single-letter names are permitted for loop counters and for variables whose scope is very small, such as an accumulator in a three-line loop. Names such as `i`, `j`, and `k` are conventional for loop indices; a single-letter name such as `x` is acceptable for a mathematical parameter in a function whose purpose is obvious from its name and body. Outside these cases, single-letter names should be avoided, because they do not communicate the meaning of the value.

Callback functions should be named after the operation they perform, not after the algorithm that invokes them. Prefer a descriptive name such as `compare_by_price` for a comparison function.

### Types

Types, including structs, unions, enums, and type aliases, use `PascalCase`: each word begins with an uppercase letter, and words are not separated by underscores. This convention distinguishes types from variables and functions at a glance.

```
static User = Struct {
 name: Char[],
 age: Int32,
};

static ResponseCode = Enum {
 Ok,
 Error,
 Timeout,
};

static NetworkConnection = Union {
 ipv4: UInt32,
 ipv6: UInt64,
};
```

### Compile-Time Constants

Compile-time constants, declared with `static`, use `SCREAMING_SNAKE_CASE`: all uppercase letters, with underscores between words. This convention distinguishes constants from ordinary variables and signals that the value is known at compile time and cannot be changed at runtime.

```
static MAX_BUFFER_SIZE = 1024;
static DEFAULT_PORT = 8080;
static PI = 3.14159;
```

The convention applies to constants whose values are numbers or simple strings. A `static` declaration that introduces a type alias uses `PascalCase`, because it names a type rather than a value.

### Enum Variants

Enum variants use `PascalCase`. The variant name should describe a state or a category rather than an implementation detail.

```
static Color = Enum {
 Red,
 Green,
 Blue,
};

static Status = Enum {
 Ok = 0,
 NotFound = 1,
 Error = 2,
};
```

### Private Symbols

A symbol that is intended to be private to the file or module in which it is declared should be prefixed with a single underscore. The underscore signals to the reader that the symbol is not part of the public interface and should not be referenced from other parts of the program.

```
module network {
 func public_api(): Void {
    // ...
 }

 func _parse_header(): Bool {
    // ...
 }
}
```

The convention is not enforced by the compiler; it is a matter of discipline. Following it consistently makes the visibility of each symbol obvious at the call site and helps prevent accidental dependencies on internal helpers.

### Acronyms

Acronyms within type names are treated as ordinary words: the first letter is uppercase and the remaining letters are lowercase. This rule produces names that are easier to read than those in which every letter of the acronym is uppercase.

```
// Correct
static HttpRequest = Struct { ... };
static JsonValue = Struct { ... };
static Crc32Table = Struct { ... };

// Incorrect
static HTTPRequest = Struct { ... };
static JSONValue = Struct { ... };
static CRC32Table = Struct { ... };
```

The same rule applies to acronyms within function names: `parse_json` is correct, `parseJSON` is not. The rule applies uniformly, even for acronyms that are conventionally written in all capitals in other contexts.

### Modules

Module names, and the file names that contain them, use `snake_case`. A module that manages user accounts should be named `user_management`, not `UserManagement` or `user-management`.

```
module user_management {
 // ...
}
```

The file that contains the module should have the same name as the module, with the extension `.px`. A module named `user_management` should be contained in a file named `user_management.px`.

## Declarations

The way a value is declared determines how a reader will interpret it. The rules in this section ensure that declarations are explicit and consistent, so that the reader can determine the type and the intended use of a value from its declaration alone.

### `let` Declarations

Variables are mutable by default. Use the `const` type qualifier when a binding must remain unchanged after initialization. A const binding is initialized at its declaration and is never assigned again:

```
let fixed_var: const Int = 0;
let counter: Int = 0;
counter += 1;
```

Prefer `const` for configuration values, validated inputs, and local values whose identity must remain stable. Remember that `const` is shallow for pointer values: it prevents reassignment of the pointer, not mutation of the object it addresses.


A `let` declaration introduces a variable. The declaration may include an explicit type, an initial value, or both. The rules for annotation depend on the scope of the variable.

A variable that is part of a public API, a struct field, or a module-level binding should always have an explicit type annotation. The reader of a public API does not have access to the implementation and cannot infer the type from the initializer, so the annotation is essential. The reader of a struct definition needs to know the layout of the struct, so field types must be explicit. The reader of a module-level binding cannot see the initializer without scrolling, so an explicit type makes the binding self-describing.

A local variable within a function body may omit the type annotation when the type is obvious from the initializer and when the variable's scope is small enough that the initializer is visible on the same screen. In this case, the `Auto` mechanism infers the type from the initializer.

```
let counter: Int32 = 0;     // Explicit, in a public context
let name = "Alice";       // Inferred as Char[]
let (x: Int32, y: Int32) = {10, 20}; // Multiple declarations
```

The following declarations are not acceptable, because they omit the type in a public context or fail to group related declarations:

```
let counter = 0;       // Missing type in public scope
let x = 10; let y = 20;   // Should be grouped or on separate lines
```

### `func` Declarations

A function declaration must include the type of every parameter and the type of the return value. The return type is written after the parameter list and a colon. If the function can fail, the return type is suffixed with `!`.

A function whose body is a single expression may be written in the shorthand form, which omits the braces and the `return` keyword. This form is appropriate for trivial getters, pure mathematical functions, and other short operations.

```
func square(x: Int32): Int32 = x * x;

func read_config(path: Char[]): Config! {
 let data = try std::file::read_file(path);
 defer free data;
 return parse_json(data);
}
```

The following declarations are not acceptable, because they omit the return type or fail to signal that the function can return an error:

```
func square(x: Int32) {    // Missing return type
 return x * x;
}

func read_config(path: Char[]): Config { // Missing ! on the return type
 let data = try std::file::read_file(path); // try in a non-! function
 return parse_json(data);
}
```

A function that is declared as a method of a struct uses the same rules. The struct name precedes the function name, separated by a dot, and the first parameter is the struct itself. The name of the first parameter is chosen by the author; there is no reserved keyword for it.

```
func Point.distance(this: Point, other: Point): Real64 {
 let dx = this.x - other.x;
 let dy = this.y - other.y;
 return sqrt(dx * dx + dy * dy);
}
```

The first parameter of a method is an ordinary parameter whose name is chosen by the author — by convention `this` — and whose type is the struct on which the method is declared. There is no reserved keyword for this parameter.

### `static` Declarations

Use `static` for compile-time type aliases, compile-time type parameters,
and named static values. Static constants must be initialised at the point of
declaration; deferred assignment is not permitted. `static` is the sole
compile-time keyword; `comptime` is an ordinary identifier.

```paxsy
static MAX_SIZE = 1024;
static BASE = 6;
static SCALED = BASE * 7;
static UserId = Int32;

func add_as(static T, left: T, right: T): T {
 return left + right;
}
```

The distinction is that a type alias introduces a name for a type, while a
static constant introduces a name for a value. A type alias such as
`static UserId = Int32;` is compile-time type metadata; a scalar value such as
`static MAX_SIZE = 1024;` is eligible for constant-expression expansion.

Write examples that use the compile-time forms documented in this guide and the language specification.

### Struct, Union, and Enum Declarations

Use ordinary `Struct` for language-native records, `extern Struct` for C ABI records, and `packed Struct` for byte-exact layouts. Choose the modifier because of a concrete layout requirement; do not use `packed` merely to reduce memory usage. `extern` and `packed` cannot be combined. Specify an enum's underlying integer type when its storage width or ABI representation is part of the interface:

```
static EventKind = Enum UInt16 {
 Open = 1,
 Close = 2,
};

static CVector = extern Struct {
 x: Real32,
 y: Real32,
};

static WireHeader = packed Struct {
 kind: UInt8,
 length: UInt32,
};
```


Struct, union, and enum types are defined inside `static` declarations using curly braces to delimit the body. Every field of a struct or union must have an explicit type. Enum variants may have explicit values; the default underlying type is `Int32`, and the default values are consecutive integers starting from zero.

```
static Point = Struct {
 x: Real64,
 y: Real64,
};

let p: Point = Point {.x = 1.0, .y = 2.0};
```

The following definition is not acceptable, because the fields do not have types:

```
static Point = Struct {
 x,
 y,
};
```

Named initialization is preferred when the order of the fields is not obvious or when some fields are skipped. Positional initialization is acceptable when the struct has few fields and their order is clear from the definition.

## Memory Management and Resource Safety

Paxsy provides manual memory management. The programmer decides when memory is allocated and when it is released, and the language does not perform automatic garbage collection. With this control comes the responsibility to pair every allocation with a release and to ensure that pointers are valid before they are dereferenced. The rules in this section describe how to do so reliably.

### Stack and Heap

Stack allocation is preferred for values whose size is known at compile time and whose lifetime is limited to the enclosing function. A local variable, an array whose size is fixed, and a struct whose fields are fixed all belong on the stack.

Heap allocation is used when the size of the allocation is not known until runtime or when the value must outlive the function in which it is created. The `alloc` operator performs heap allocation; the `free` operator releases it.

The following function uses a stack-allocated buffer, because the size of the buffer is fixed and its lifetime is limited to the function:

```
func process(): Void {
 let buffer: Int8[1024]; // Stack allocation is appropriate
 // ...
}
```

The following function uses a heap allocation unnecessarily, because the size of the buffer is known at compile time and the buffer does not outlive the function:

```
func process(): Void {
 let buffer: &Int8 = alloc Int8[1024]; // Unnecessary heap allocation
 defer free buffer;
 // ...
}
```

Heap allocation should be reserved for cases in which the size is dynamic or the lifetime is longer than the enclosing function.

### Pointer Discipline

A pointer of type `&T` may hold the address `0`, which indicates that it does not refer to a valid object. Before dereferencing a pointer with `@`, the programmer should check whether the pointer is `0`. Dereferencing a pointer that is `0` is undefined behaviour.

The ownership of a pointer should be documented in a comment. A pointer whose ownership is transferred to the caller should be marked as such; a pointer that is borrowed and must not be released by the caller should be marked as such. The comment makes the calling convention explicit and prevents both leaks and double frees.

```
func process_buffer(buf: &Int32, len: Int32): Void {
 if buf == 0 {
    return;
 }
 for 0..(len - 1) |i| {
    @buf += 1; // Safe: buf is not zero
 }
}
```

The following function does not check whether the pointer is `0` before dereferencing it. The behaviour of the function when called with a zero pointer is undefined.

```
func process_buffer(buf: &Int32, len: Int32): Void {
 for 0..(len - 1) |i| {
    @buf += 1; // DANGER: no check for a zero pointer
 }
}
```

### `defer` Discipline

The `defer` statement schedules a statement or a block to be executed when the enclosing function exits. The deferred statement is executed whether the function returns normally or propagates an error. This makes `defer` the idiomatic mechanism for pairing a resource acquisition with its release.

A `defer` statement should appear immediately after the resource is acquired. Placing it elsewhere risks the resource leaking if the function exits between the acquisition and the `defer`. The deferred statements within a function are executed in last-in-first-out order, so a resource that is acquired last is released first.

```
func copy_data(src_path: Char[], dst_path: Char[]): Void! {
 let src = try std::file::File.open(src_path);
 defer std::file::File.close(src);

 let dst = try std::file::File.create(dst_path);
 defer std::file::File.close(dst);

 let buf: &Int8 = alloc Int8[1024];
 if buf == 0 {
    return error 1;
 }
 defer free buf;

 // ...
}
```

The following function does not pair its resources with deferred releases. If the function exits early, the resources leak.

```
func copy_data(src_path: Char[], dst_path: Char[]): Void! {
 let src = try std::file::File.open(src_path);
 let dst = try std::file::File.create(dst_path);
 // Missing defers; both files leak on every exit path.

 let buf: &Int8 = alloc Int8[1024];
 defer free buf;
}
```

The `errdefer` statement schedules a statement to run only when the function exits with an error; on a successful return it does not run. It is the idiomatic companion to `defer` when a resource must be released on failure but handed to the caller on success:

```
func load_table(path: Char[]): Table! {
 let data = try std::file::read_file(path);
 errdefer free data;
 let table = try parse_table(data);
 return table; // success: ownership of table passes to the caller
}
```

### `alloc` and `free` Pairing

Every call to `alloc` must be paired with exactly one call to `free`. The pairing should be enforced by a `defer` statement placed immediately after the allocation succeeds. Calling `free` on a pointer that was not obtained from `alloc` is undefined behaviour; calling `free` on a stack-allocated variable is a compile-time error.

```
let ptr: &Int32 = alloc Int32[1];
if ptr == 0 {
 return error 1;
}
defer free ptr;
@ptr = 42;
```

The following code is not acceptable, because it attempts to free memory that was not obtained from `alloc`:

```
let arr: Int32[10];
free arr; // ERROR: arr is stack-allocated
```

## Error Handling

Paxsy handles errors through return values. A function that can fail returns a type suffixed with `!`, and the caller either propagates the error with `try` or handles it with `catch`. The rules in this section describe how to use these mechanisms consistently.

### When to Return an Error

A function should return an error when it encounters a condition that the caller can reasonably be expected to handle. Examples include a failed file open, a failed allocation, an invalid argument, and a network timeout. The error should be reported as soon as it is detected, and the function should return immediately rather than continuing with a value that is known to be invalid.

```
func divide(a: Int32, b: Int32): Int32! {
 if b == 0 {
    return error 1;
 }
 return a / b;
}
```

The following function is not acceptable, because it silently returns a value when an error condition is detected:

```
func divide(a: Int32, b: Int32): Int32 {
 if b == 0 {
    return 0; // Silent failure; the caller cannot distinguish
 }
 return a / b;
}
```

### Unrecoverable Failures

Paxsy has no separate mechanism for aborting a program. Every failure that must be reported is expressed as an error value, either returned with `return error` or propagated with `try`. A `main` function returning `Void!` propagates a returned error to the operating system as a non-zero exit code. A condition that the program cannot detect at runtime results in undefined behaviour; the compiler may detect some such cases at compile time and report them as errors, and the programmer is responsible for preventing the rest.

### `try` and `catch`

The `try` operator propagates an error from a called function to the caller. It may be used only inside a function whose return type is suffixed with `!`. The `catch` operator handles an error locally; the catch block must produce a value of the success type of the expression.

A function that propagates errors should use `try` rather than `catch`, so that the caller is given the opportunity to handle the error in its own context. A function that has a meaningful way to recover from an error should use `catch` and provide a fallback value.

The two operators are alternatives: an expression either propagates its error with `try` or handles it locally with `catch`, and combining them in a single expression is not a language construct; a program that needs both propagation and local handling must first handle the error locally with `catch`, and then, if the handler itself produces an error, propagate that error with `try`.

```
func load_file(path: Char[]): Char[]! {
 let data = try std::file::read_file(path);
 defer free data;
 return data;
}
```

The following function is not acceptable, because it uses `try` inside a function that does not return `!`:

```
func load_file(path: Char[]): Char[] {
 let data = try std::file::read_file(path); // Error: function does not return !
 return data;
}
```

### Error Codes

Error codes should be named `static` constants rather than bare integer literals. A named constant communicates the meaning of the error and makes it easy to change the code in one place.

```
static ERR_DIV_BY_ZERO = 1;

func divide(a: Int32, b: Int32): Int32! {
 if b == 0 {
    return error ERR_DIV_BY_ZERO;
 }
 return a / b;
}
```

The following function is not acceptable, because the error code is a magic number:

```
func divide(a: Int32, b: Int32): Int32! {
 if b == 0 {
    return error 1; // Magic number
 }
 return a / b;
}
```

### Catch Blocks

A catch block must either produce a meaningful value of the expected type or log the error and produce a fallback. An empty catch block that silently discards the error is forbidden, because it hides the failure from the reader and from any diagnostic tooling. When the function whose error is being caught returns `Void!`, the catch block must produce a `Void` value; the idiomatic way to fill an empty catch block for a `Void` result is the `pass` statement, which emits a machine `NOP`.

```
let value = divide(10, 0) catch |e| {
 std::inout::println("Divide failed: {e}") catch |e2| {
    pass;
 };
 return 0;
};
```

The `pass` statement is required here only because every branch in Paxsy must contain at least one statement; it emits a single machine NOP that has no effect on the program's behaviour, and it is the idiomatic way to express "do nothing" when a statement is syntactically required.

The following catch block is not acceptable, because it neither logs nor explains the error:

```
let value = divide(10, 0) catch |e| {
 return 0; // Silent: the error is discarded
};
```

A catch block should also avoid using `pass` as a placeholder for "to be implemented". When a catch block has real work to do, that work should be written; when it does not, a comment should explain why the error can be safely ignored.

## Control Flow

The control-flow constructs of Paxsy have a small number of style rules, all of which are designed to make the flow of execution obvious at a glance.

### `if` and `else`

The condition of an `if` statement is never enclosed in parentheses. The body of the `if`, the `else if`, and the `else` are always enclosed in braces, even when the body contains a single statement. The `if` construct may be used as an expression, and this form is encouraged over ternary-style logic.

```
if num > 10 {
 num = 10;
} else if num < 0 {
 num = 0;
}

let max = if a > b { a } else { b };
```

The following code is not acceptable, because it encloses the condition in parentheses and omits the braces around a single-statement body:

```
if (num > 10) {     // Parentheses are not used
 num = 10;
}

if num > 10
 num = 10;      // Braces are mandatory
```

### `match`

The `match` construct is used for matching a value against a fixed set of alternatives. A `match` may include an `else` branch to handle values that do not match any case. When the match is used as an expression, the `else` branch is required, because the construct must produce a value on every path. The `or` keyword combines several values into a single case. A case may not hold a range; an interval of values is expressed with a boolean guard, as in `case x >= 0 and x <= 10 { ... }`.

```
match status {
 case Status.Ok {
    std::inout::println("Success");
 }
 case Status.Error {
    std::inout::println("Error");
 }
 else {
    std::inout::println("Unknown");
 }
}
```

The first `match` below is not acceptable because it has no `else` branch; the second is not acceptable because it uses a range as a case pattern instead of a boolean guard:

```
match value {
 case 0 {
    std::inout::println("zero");
 }
 case 1 {
    std::inout::println("one");
 }
 // Missing else
}

match value {
 case 0..10 { // a range is not a case pattern
    // ...
 }
}

// The interval is expressed with a boolean guard:
match value {
 case value >= 0 and value <= 10 {
    // ...
 }
 else {
    // ...
 }
}
```

### Loops

The `for` loop iterates over a range. The bounds of a range are inclusive on both ends, so `0..9` means the values 0 through 9. A range may carry a `step` — `for 0..10 step 2 |i|` iterates the values 0, 2, 4, 6, 8, 10 — and may end with `..=` to guarantee that the end value is emitted even when the step does not land on it; the full semantics of ranges are given in the language specification. An open-ended range, such as `0..`, requires an explicit `break` inside the loop, because the loop would otherwise never terminate.

The `while` loop is used for iteration whose termination depends on a condition rather than on a range. The `do-while` loop is used when the body must execute at least once.

Labelled `break` and `continue` statements are preferred to boolean flags for controlling nested loops. A label makes the target of the `break` or `continue` explicit and removes the need for a state variable that exists only to control the loop.

```
for 0..9 |i| {
 std::inout::println("{i}");
}

let i: Int32 = 0;
while i < 10 {
 std::inout::println("{i}");
 i += 1;
}

outer: for 0..9 |i| {
 for 0..9 |j| {
    if i * j > 20 {
      break outer;
    }
 }
}
```

The following loops are not acceptable, because they enclose the range in parentheses or use an open-ended range without a break:

```
for (0..9) |i| {     // Parentheses are not used
 std::inout::println("{i}");
}

for 0.. |i| {      // Open-ended without a break
 std::inout::println("{i}");
}
```

### `pass`

The `pass` statement emits a single machine `NOP` instruction. It is used for precise delay loops, for code alignment, for debug breakpoints, and for filling a branch that must be empty. It is an ordinary statement, not an empty statement, and it always emits code.

Every branch in Paxsy — the body of an `if`, an `else`, a `while`, a `for`, a `match` case, a `select` arm, and a catch block — contains a statement block. Use `pass` only when a deliberate no-op is part of the program; do not use it to conceal unfinished logic. An empty block is written according to the grammar defined in the language specification.

The following loop uses `pass` to create a deliberate delay:

```
volatile for 0..1000 |i| {
 pass; // Emits a real NOP for timing
}
```

The following use of `pass` is not acceptable, because it is a placeholder for a body that has not been written:

```
if condition {
 pass; // TODO: implement this
}
```

When a branch has real work to do, write that work directly. When an intentional no-op is required, use `pass` and explain why it is necessary. A branch that is empty because its implementation is unfinished must be completed before the code is considered ready.

## Modules and Imports

Modules are the mechanism by which Paxsy organises code into reusable components and controls visibility. The rules in this section describe how modules should be structured and how their contents should be imported.

### Module Blocks

A named module is used for any reusable component: a library, a subsystem within a larger program, or a collection of related types and functions. The name of the module should describe its responsibility, and the module should contain declarations that are cohesive and related to one another.

An unnamed module is used only at the entry point of a program or for global glue code that does not belong to any specific module. A library file should never use an unnamed module, because the declarations inside it would be visible to the entire project and would pollute the global namespace.

```
module user_management {
 static User = Struct {
    name: Char[],
    age: Int32,
 };

 func create_user(name: Char[], age: Int32): User! {
    // ...
 }
}
```

### `use` Directives

The `use` directive brings the symbols of a module into the current scope. The directive has two forms: `use module;` imports all public symbols of the module, and `use module::symbol;` imports a single symbol. The directive must appear at the top of the file, after the header comment and before any other declarations.

```
use std::inout::println;
use std::file;
```

The following directives are not acceptable, because Paxsy does not have wildcard imports beyond the first form, nor group imports:

```
use std::*;       // Overly broad
use std::graphics::*; // Pollutes the namespace
use std::inout::{println, print}; // Group imports do not exist
```

The first form of the `use` directive imports all public symbols of the named module. It should be used when the file needs many symbols from the module; for files that use only one or two symbols, the second form is preferred because it makes the dependency explicit.

### Visibility

A top-level declaration that is placed outside any module block is private to the file in which it appears. It is not visible to other files in the project. This default is deliberate: it encourages the programmer to make visibility explicit rather than accidental.

A symbol that is intended to be private but is declared inside a module block should be prefixed with a single underscore, following the convention described in the naming section. The underscore is a signal to readers that the symbol is an implementation detail.

```
module public_api {
 func open(): Handle! {
    // ...
 }

 func _validate(): Bool {
    // ...
 }
}
```

Only the symbols that are part of the module's public interface should be exported. Internal helpers should be private, so that their implementation can be changed without affecting the rest of the program.

## Concurrency

Paxsy provides two concurrency models: lightweight processes, written with `routine`, and native threads, written with `async` and `await`. Both communicate through typed channels. The rules in this section describe how to use these mechanisms idiomatically.

### `routine`

A `routine` is a lightweight process scheduled by the Paxsy runtime. A routine captures the variables of its enclosing scope by value, so the state of the routine is independent of the state of its caller. A routine does not return a value, and it cannot be awaited.

A routine should be documented with a comment that describes its purpose, the channels it uses, and the conditions under which it terminates. The comment makes it possible for a reader to understand the routine's behaviour without tracing through its body.

```
// Worker routine: receives jobs from `jobs`, processes each one, and
// sends the result on `results`. Terminates when `jobs` is closed.
routine {
 for 0.. |i| {
    let job = <- jobs;
    if job == 0 {
      break;
    }
    results <- process(job);
 }
}
```

### `async` and `await`

An `async` block or function creates a native thread. The result of the `async` expression is a channel of type `chan T`, where `T` is the return type of the block or function. The `await` operator waits for the channel to produce its value and returns that value.

The channel returned by an `async` should be stored in a named variable with an explicit type. Naming the channel makes the relationship between the thread and its result explicit, and the explicit type documents the type of the value that will eventually be produced.

```
let result_ch: chan Int32 = async compute(input);
let result: Int32 = await result_ch;
```

### `chan`

A channel is declared with the `chan` keyword. A channel with no buffer size blocks a sender until a receiver is ready, and blocks a receiver until a sender is ready. A channel with a buffer size accepts up to that many values before a sender blocks.

The type of a channel should always be explicit, both at its point of declaration and in any function signature that accepts or returns a channel. The buffer size should be chosen to match the expected pattern of production and consumption.

```
let jobs: chan Job = chan 16;
let results: chan JobOutcome = chan 16;
```

### `select`

The `select` statement waits for one of several channel operations to become ready. A `select` may include an `else` branch to handle the case in which no arm is ready immediately. Without `else`, the `select` blocks until one of the arms becomes ready.

Arms of a `select` statement begin with the keyword `on`, while arms of a `match` statement begin with the keyword `case`. The two constructs are distinct, and the keywords make the distinction visible at a glance.

```
select {
 on x = <- a {
    // handle x from channel a
 }
 on b <- 42 {
    // sent 42 on channel b
 }
 else {
    // no arm was ready
 }
}
```

The `select` statement should be used sparingly. In many cases, a single channel and a blocking receive are simpler and equally correct. The `select` is appropriate when the program must respond to events from multiple sources and cannot afford to block on any single one.

## Inline Assembly and volatile

Inline assembly and the `volatile` keyword give the programmer direct control over the code that is emitted. They are powerful and should be used with care.

### Inline Assembly

Inline assembly is declared with the `asm` statement, whose argument is a string containing assembly instructions. The string is passed to the assembler without validation by the compiler. Inline assembly should be used only when no Paxsy abstraction exists for the operation that must be performed.

```
asm "mov eax, 42";
```

When the assembly instruction has side effects that the compiler cannot observe — for example, when it accesses a memory-mapped register or controls interrupts — the `volatile` keyword must be applied. A `volatile asm` statement is emitted exactly where it appears, and the compiler is forbidden to reorder or remove it.

```
// Disable interrupts; the instruction must not be reordered.
volatile asm "cli";
```

Inline assembly should be accompanied by a comment that explains what the instruction does and why it is necessary. The comment helps a future reader understand the purpose of the instruction, and it documents the assumptions on which the instruction depends.

### `volatile`

The `volatile` keyword can be applied to a statement, a block, or an `asm` statement. It instructs the compiler to emit the annotated code without optimisation, reordering, or removal. It is used for memory-mapped I/O, for variables shared with interrupt handlers, and for precise delay loops.

```
volatile {
 let status = read_mmio(0x4000);
 if status & 0x02 != 0 {
    // handle interrupt flag
 }
}
```

The `volatile` keyword should not be applied to ordinary variables. A variable that is not shared with hardware or an interrupt handler does not need `volatile`, and applying it unnecessarily prevents the compiler from performing legitimate optimisations.

```
volatile let counter = 0; // Not necessary; counter is not shared
```

## Comments and Documentation

Comments are the mechanism by which the author of a piece of code communicates with its readers. The rules in this section describe the kinds of comments that Paxsy recognises and the purposes for which each is appropriate.

### Documentation Comments

A documentation comment begins with three slashes `///` and is placed immediately above the declaration that it documents. It is used for public types, functions, and modules. The comment should describe what the declaration does and why it exists, not how it is implemented. For functions, the comment should describe the parameters, the return value, the error conditions, and any safety invariants that the caller must observe.

```
/// Calculates the average score for a list of student results.
///
/// Arguments:
/// scores - Pointer to an array of test scores (0-100).
/// count - Number of elements in the array.
///
/// Returns:
/// The average score as a Real64, or an error if the input is invalid.
///
/// Safety:
/// The caller must ensure that `scores` points to at least `count`
/// valid Int32 values.
func calculate_average(scores: &Int32, count: Int32): Real64! {
 // ...
}
```

A documentation comment that merely restates the function's name is not useful. The following comment adds no information beyond what the declaration already communicates:

```
// Calculates average
func calculate_average(scores: &Int32, count: Int32): Real64! {
 // ...
}
```

### Inline Comments

An inline comment begins with two slashes `//` and extends to the end of the line. It is used within the body of a function to explain the purpose of a particular statement or group of statements. An inline comment should explain why the code does what it does, not what the code does; the code itself should communicate the latter.

```
// Skip the eight-byte metadata header before reading the payload.
std::sys::lseek(file.fd, 8, std::sys::SEEK_SET);
```

The following comment is not useful, because it restates what the code already says:

```
i += 1; // Increment i
```

### Block Comments

A block comment begins with `/*` and ends with `*/` and may span multiple lines. It is used for the file header and for temporarily disabling a block of code during debugging. It is not used for ordinary documentation; documentation comments and inline comments are preferred for those purposes.

### String Interpolation

Paxsy supports interpolation within string literals using the syntax `{expression}`. This mechanism should be used in preference to manual concatenation, because it is more readable and because it avoids the risk of buffer overflows. The target of the interpolation must be a `Char[]` large enough to hold the resulting string plus the null terminator.

```
let msg = "Count: {count}, Name: {name}";
```

Prefer interpolation for values embedded in a single formatted message. When a string must be assembled incrementally or retained as an owning value, use the string-building operations in `std::str`:

```
let msg = "Count: {count}, Name: {name}";

let owned = try std::str::concat("Name: ", name);
defer std::str::destroy(&owned);
```

## Standard Library Patterns

The Paxsy standard library provides a set of modules for common tasks. Each module has a small number of idiomatic patterns of use, and following those patterns makes the code consistent with the rest of the ecosystem.

### Console I/O

The functions in `std::inout` return a value of type `Void!`, because writing to a console can fail. Every call to `std::inout::print` or `std::inout::println` must therefore be handled with `try` or `catch`. In a function that returns `Void!`, `try` is the natural choice. In a function that does not return `Void!`, `catch` is used to handle the failure locally.

```
func report(message: Char[]): Void! {
 try std::inout::println(message);
}
```

### File Handles

A file handle is opened with `std::file::File.open` or `std::file::File.create`, and it must be closed with `std::file::File.close`. The pairing should be enforced with a `defer` statement placed immediately after the open.

```
let file = try std::file::File.open(path);
defer std::file::File.close(file);
let data = try std::file::File.read(file, buffer, size);
```

### Paths

Paths should be combined with `std::path::join` rather than by concatenating strings. The `join` function inserts the correct separator for the platform, and it handles the case where one of the components already ends with a separator.

```
let full = std::path::join(base, name);
```

The following code is not acceptable, because it hard-codes the path separator:

```
let full = base + "/" + name;
```

### JSON

Data parsed from JSON should be validated with the `as_object` and `as_array` helpers before its fields are accessed. The helpers return an error if the value is not of the expected type, and handling that error prevents the program from accessing a field that does not exist.

```
let root = try std::json::parse(text);
let obj = try std::json::as_object(&root);
```

### Graphics

Every graphics resource is created with a `create_*` function and released with a corresponding `destroy_*` function. The release should be scheduled with `defer` immediately after the resource is created.

```
let buf = try std::graphics::create_buffer(ctx, size, std::graphics::Static);
defer std::graphics::destroy_buffer(ctx, buf);
```

## Anti-Patterns

The patterns in this section are forbidden. Each is accompanied by an incorrect example, a corrected version, and an explanation of why the incorrect version is harmful.

### Magic Numbers

A magic number is a numeric literal that appears in the code without explanation. Magic numbers obscure the meaning of the value and make it difficult to change the value in one place.

```
// Incorrect
let buffer: Char[1024];
if code == 404 {
 // ...
}

// Correct
static BUFFER_SIZE = 1024;
static HTTP_NOT_FOUND = 404;

let buffer: Char[BUFFER_SIZE];
if code == HTTP_NOT_FOUND {
 // ...
}
```

The corrected version uses named constants for both the buffer size and the HTTP status code. A reader can now understand the meaning of each value without consulting a specification.

### Deep Nesting

Deeply nested code is difficult to read, because the reader must hold the state of every enclosing block in mind while reading the innermost block. Nesting should be flattened with early returns whenever possible.

```
// Incorrect
func process(x: Int32): Void {
 if condition1 {
    if condition2 {
      if condition3 {
        // ...
      }
    }
 }
}

// Correct
func process(x: Int32): Void {
 if not condition1 { return; }
 if not condition2 { return; }
 if not condition3 { return; }
 // ...
}
```

The corrected version checks each condition in turn and returns early when a condition is not met. The body of the function is then at a single level of indentation, and the reader can follow the logic without tracking nested scopes.

### Code Duplication

When the same logic appears in two or more places, it should be factored into a helper function. Duplication makes the code longer, harder to maintain, and prone to divergence.

```
// Incorrect
func process_a(): Void {
 let file = std::file::File.open("a.txt");
 // process
 std::file::File.close(file);
}

func process_b(): Void {
 let file = std::file::File.open("b.txt");
 // process
 std::file::File.close(file);
}

// Correct
func process_file(name: Char[]): Void! {
 let file = try std::file::File.open(name);
 defer std::file::File.close(file);
 // process
}
```

The corrected version factors the common logic into a single function that takes the file name as a parameter. The two callers now share a single implementation, and any change to the logic is made in one place.

### Silent Error Swallowing

An error that is not handled and not propagated is invisible to the reader and to any diagnostic tooling. Every error must be either propagated with `try` or handled with `catch`, and the catch block must do something meaningful with the error.

```
// Incorrect
let result = try operation() catch {
 return 0;
};

// Correct
let result = try operation() catch |e| {
 std::inout::println("Operation failed: {e}") catch |e2| {
    pass;
 };
 return fallback_value;
};
```

The corrected version logs the error before returning the fallback value. A reader who examines the code can see that the error is acknowledged, and a diagnostic tool can record its occurrence.

### Unpaired Resources

Every resource that is acquired must be released on every exit path. When a resource is not paired with a release, the resource leaks and the program eventually exhausts the available supply.

```
// Incorrect
let file = try std::file::File.open(path);
let data = try std::file::File.read(file, buf, size);
// The file is never closed.

// Correct
let file = try std::file::File.open(path);
defer std::file::File.close(file);
let data = try std::file::File.read(file, buf, size);
```

The corrected version schedules the release of the file handle with `defer`. The release is now performed regardless of how the function exits.

### Implicit Narrowing Casts

A conversion from a wider type to a narrower type may truncate the value. Such a conversion must be written explicitly with the `as` operator, and the safety of the conversion must be documented.

```
// Incorrect
let big: Int64 = 100000;
let small: Int32 = big; // Truncation is not signalled

// Correct
let big: Int64 = 100000;
let small: Int32 = big as Int32; // Explicit; the value fits
```

The corrected version makes the conversion explicit. The reader can see that a cast is being performed and can verify that the value is in range.

### Global Mutable State

A mutable variable at module scope is accessible from every function in the file and can be modified by any of them. Such variables make the behaviour of a program difficult to reason about, because any function may change the state on which any other function depends.

```
// Incorrect
let global_counter: Int32 = 0;

func increment(): Void {
 global_counter += 1;
}

// Correct
static Counter = Struct {
 value: Int32,
};

func increment(c: Counter): Void {
 c.value += 1;
}
```

The corrected version encapsulates the counter in a struct and passes it explicitly to the functions that operate on it. The state is now local to the struct, and the functions that use it declare their dependency in their signatures.

### Overly Broad Imports

An import that brings in an entire module when only a few symbols are needed makes the file's dependencies unclear and increases the risk of name collisions. Specific imports should be preferred.

```
// Incorrect
use std::graphics;

// Correct
use std::graphics::create_buffer;
use std::graphics::destroy_buffer;
```

The corrected version imports the two symbols that the file needs, making the file's dependencies explicit. A reader can see at a glance which parts of the standard library the file relies on.

## A Complete Example

The following file demonstrates every major convention from this guide. It is a small packet-processing module that reads a raw byte buffer, validates the header, extracts the payload, and prints a summary. The file uses structs, enums, methods, `static` constants, `defer`, error handling, `match`, and the standard library.

```
/*
 * Copyright (C) 2026 Example Corp
 * Distributed under the Apache License, Version 2.0.
 *
 * Module: packet_processor
 * Validates and processes simple network packets.
 */

use std::inout::println;
use std::array;

static MAX_PACKET_SIZE = 1500;
static HEADER_SIZE = 4;

static PacketType = Enum {
 Data = 0,
 Ack = 1,
 Error = 2,
};

static Packet = Struct {
 kind: PacketType,
 sequence: UInt16,
 payload: &UInt8,
 payload_len: Int32,
};

static ParseError = Enum {
 ZeroPointer = 1,
 TooShort = 2,
 UnknownType = 3,
 AllocFailed = 4,
};

/// Parses a raw byte buffer into a Packet struct.
///
/// Arguments:
/// raw - Pointer to the raw bytes. Must not be zero.
/// len - Length of the buffer in bytes.
///
/// Returns:
/// A validated Packet on success. The caller assumes ownership of the
/// payload pointer and must release it with free.
///
/// Errors:
/// Returns a ParseError code on validation or allocation failure.
func parse_packet(raw: &UInt8, len: Int32): Packet! {
 if raw == 0 {
    return error ParseError.ZeroPointer;
 }
 if len < HEADER_SIZE {
    return error ParseError.TooShort;
 }

 let kind_val = raw[0] as Int32;
 let kind: PacketType;
 match kind_val {
    case 0 {
      kind = PacketType.Data;
    }
    case 1 {
      kind = PacketType.Ack;
    }
    case 2 {
      kind = PacketType.Error;
    }
    else {
      return error ParseError.UnknownType;
    }
 }

 let seq = raw[1] as UInt16 $ (raw[2] as UInt16 << 8);

 let payload_len = len - HEADER_SIZE;
 let payload: &UInt8 = alloc UInt8[payload_len];
 if payload == 0 {
    return error ParseError.AllocFailed;
 }
 errdefer free payload;
 // Ownership of payload is transferred to the caller on success.

 for 0..(payload_len - 1) |i| {
    payload[i] = raw[HEADER_SIZE + i];
 }

 return Packet {kind, seq, payload, payload_len};
}

/// Prints a human-readable summary of a packet.
///
/// Arguments:
/// pkt - The packet to summarize.
func print_summary(pkt: Packet): Void {
 let type_name = if pkt.kind == PacketType.Data {
    "DATA"
 } else if pkt.kind == PacketType.Ack {
    "ACK"
 } else {
    "ERROR"
 };

 println(
    "Packet: type={type_name}, seq={pkt.sequence}, len={pkt.payload_len}"
 ) catch |e| {
    // Console output is best-effort; nothing to do on failure.
    pass;
 };
}

func main(): Void! {
 let raw: UInt8[8] = {0, 0, 5, 0, 10, 20, 30, 40};

 let pkt = try parse_packet(&raw[0], 8);
 defer free pkt.payload;

 print_summary(pkt);
}
```

## Postscript

The rules in this guide fall into two categories. The first category contains rules that are enforced by the compiler or that have a clear technical rationale. The prohibition of tabs, for example, ensures that the file's appearance does not depend on the reader's editor; the prohibition of a missing `else` in `match` is enforced by the language itself. The second category contains rules that are conventions rather than requirements. The choice of `snake_case` for variables, for example, is a convention that is not enforced by the compiler; the language would accept `myVariable` just as readily. The rules in the second category are important because they make the codebase consistent, and consistency is what allows a reader to move from one file to another without re-learning the local style.

When a rule in this guide conflicts with a rule in another document, the other document takes precedence for the topic it covers. The language specification defines what is valid Paxsy; the standard library reference defines the interfaces of the standard library; the [Toolchain User Manual](toolchain-user-manual.md) defines the behaviour of the compiler and its associated tools. This guide defines how Paxsy code should be written, and it assumes the rules established by the other documents.

The highest-priority rules are these. Every file has a header. Imports are grouped and ordered. Indentation is four spaces. Lines are at most eighty characters. Names use `snake_case` for variables and functions, `PascalCase` for types, and `SCREAMING_SNAKE_CASE` for constants. Every resource acquisition is paired with a `defer` that releases it. Every error is either propagated with `try` or handled with `catch`. Every `match` has an `else`. No magic numbers, no deep nesting, no duplicated logic, no silent error swallowing, no unpaired resources, no global mutable state, no overly broad imports. A programmer who follows these rules will produce code that is clear, correct, and consistent with the rest of the Paxsy ecosystem.
