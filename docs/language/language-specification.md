## Preface

Paxsy is a statically-typed, compiled systems programming language designed for software that must be fast, predictable, and safe without relying on a garbage collector. It targets native machine code and gives the programmer direct control over memory layout, allocation, and CPU behaviour while providing modern facilities for error handling, compile-time evaluation, and concurrency. Paxsy is intended for operating system kernels, device drivers, embedded firmware, real-time controllers, game engines, network servers, and any domain where the cost of every allocation and every branch must be understood.

The design of Paxsy rests on five principles. The first is performance: the language compiles to native code with no hidden runtime, no tracing collector, and no interpreter. The second is determinism: the same source compiled with the same options produces the same machine code and the same memory layout, and the runtime behaviour of a program is determined entirely by its source and its inputs, never by a background scheduler or a moving collector. The third is memory safety without a garbage collector: Paxsy prevents use-after-free, double-free, and zero-pointer dereference through explicit ownership conventions, zero-pointer checks, and a type system that distinguishes values from errors. The fourth is first-class concurrency: the language provides lightweight processes scheduled cooperatively over a pool of native threads, together with typed channels for message passing, so that concurrent programs can be written without locks in the common case. The fifth is first-class compile-time evaluation: computations, types, and constants can be evaluated before the program runs, so that array sizes, lookup tables, and configuration values are computed once at compile time rather than every time the program starts.

This document is for programmers who want to learn Paxsy or use it to build software. It introduces the language from first principles, then defines the syntax, semantics, memory model, and edge cases that affect everyday program behaviour. No prior knowledge of Paxsy is assumed.

The document is organised so that concepts are introduced in dependency order. A construct is fully described before it appears in any example. Headings are descriptive, not numbered, so that the reader can navigate by topic. All prose, all code, and all comments in code are written in English.

This document is the authoritative description of the Paxsy language. Three companion documents cover the surrounding ecosystem. `standard-library-reference.md` is the standard library reference. [Toolchain User Manual](toolchain-user-manual.md) describes the compiler, build system, package manager, project configuration, and debugging workflow. `code-style-guide.md` defines the conventions used by the standard library and user projects. Where any of those documents touches on a question of syntax or semantics, it defers to this document.

## Program Structure

A Paxsy program is a collection of source files, each with the extension `.px`, that together form a single translation unit. The compiler reads all source files belonging to a project, analyses them as a whole, and produces either an executable, a library, or an object file. A source file is a sequence of declarations: types, constants, functions, modules, and top-level statements. Each declaration occupies a region of the file, and each statement within a function body is terminated by a semicolon.

The entry point of every executable Paxsy program is a function named `main`. There are two permitted signatures for `main`. The first is `func main(): Void`, which returns no value and cannot propagate an error. The second is `func main(): Void!`, which returns no value but may propagate an error to the operating system. A program whose `main` returns `Void` terminates normally when control reaches the end of the function body. A program whose `main` returns `Void!` terminates normally when control reaches the end, and terminates with a non-zero exit status if an error propagates out of `main`. The following program is the smallest complete Paxsy program:

```
func main(): Void {
 pass;
}
```

The `pass` statement emits a single machine `NOP` instruction and is described in full later; here it serves only to give the function body a statement.

Comments in Paxsy are of two kinds. A single-line comment begins with `//` and extends to the end of the line. A multi-line comment begins with `/*` and ends with `*/`, and may span any number of lines. Comments are ignored by the compiler and have no effect on the generated code or on the size of the binary. The following program demonstrates both:

```
/*
 * This program prints a greeting.
 * It is a complete Paxsy program.
 */
func main(): Void {
 // The greeting is printed by a standard library call.
 pass;
}
```

The semicolon is the statement terminator. Every statement in Paxsy ends with a semicolon, including declarations, assignments, function calls used as statements, and the `return` statement. A missing semicolon is a compile-time error. The one exception is the final expression in a value-producing `if`, `match`, or `catch` block: it may omit its semicolon immediately before the closing brace. Writing the semicolon there is also valid and does not discard the expression's value.

The compilation of a Paxsy program proceeds through several phases. Lexical analysis converts the sequence of characters in each source file into a sequence of tokens. Parsing arranges those tokens into an abstract syntax tree according to the grammar of the language. Semantic analysis checks the tree for type correctness, name resolution, and adherence to the language's rules; it is during this phase that errors such as type mismatches, undeclared identifiers, and missing `else` branches in `match` are reported. Intermediate representation generation lowers the abstract syntax tree into a form that is closer to machine code and easier to optimise. Optimisation transforms the intermediate representation to reduce instruction count, eliminate dead code, and improve locality. Code generation emits assembly language for the target architecture. Assembly converts the assembly language into relocatable object files. Linking combines object files and resolves symbols to produce the final executable or library.

An object file produced by the Paxsy compiler contains several sections. The `.text` section holds executable machine instructions. The `.data` section holds initialised global and static data whose values are known at compile time. The `.bss` section holds uninitialised global and static data; because the initial values are all zero, the section occupies no space in the object file and is allocated and zeroed by the operating system when the program is loaded. The `.rodata` section holds read-only data, such as string literals and `static` values whose address is taken. The symbol table lists the names defined in the object file and the names it references from other object files; the linker uses this table to resolve references. When a function in one file calls a function in another, the compiler emits a call to an external symbol, and the linker patches the call site with the final address of the callee.

## Data Types

Paxsy is statically typed. Every expression has a type that is known at compile time, and every variable has a type that is fixed at the point of its declaration. The compiler uses these types to allocate storage, to select instructions, and to verify that operations are applied to compatible operands. Static typing is what allows a Paxsy program to be compiled to efficient machine code without runtime type checks, and it is what allows many programming errors to be detected before the program is ever run.

The built-in types of Paxsy are listed in the following table. Each row gives the type name, a description, its size in bytes, and its range of representable values. Signed integer types use two's complement representation. A floating-point type stores a sign bit, a biased exponent field, and a significand field. A `Real32` occupies four bytes, with one sign bit, eight exponent bits, and twenty-three significand bits, and an implicit leading one in the significand. A `Real64` occupies eight bytes, with one sign bit, eleven exponent bits, and fifty-two significand bits, and an implicit leading one in the significand. Addition, subtraction, multiplication, and division of floating-point values round the exact result to the nearest representable value, with ties broken toward the even significand. Positive zero, negative zero, positive infinity, negative infinity, and not-a-number are representable; an operation whose exact result is undefined produces not-a-number.

| Type | Description | Size (bytes) | Range |
|------|-------------|--------------|-------|
| `Int8` | Signed 8-bit integer | 1 | −128 to 127 |
| `Int16` | Signed 16-bit integer | 2 | −32,768 to 32,767 |
| `Int32` | Signed 32-bit integer | 4 | −2,147,483,648 to 2,147,483,647 |
| `Int64` | Signed 64-bit integer | 8 | −9,223,372,036,854,775,808 to 9,223,372,036,854,775,807 |
| `Int128` | Signed 128-bit integer | 16 | −(2¹²⁷) to 2¹²⁷−1 |
| `Int256` | Signed 256-bit integer | 32 | −(2²⁵⁵) to 2²⁵⁵−1 |
| `UInt8` | Unsigned 8-bit integer | 1 | 0 to 255 |
| `UInt16` | Unsigned 16-bit integer | 2 | 0 to 65,535 |
| `UInt32` | Unsigned 32-bit integer | 4 | 0 to 4,294,967,295 |
| `UInt64` | Unsigned 64-bit integer | 8 | 0 to 18,446,744,073,709,551,615 |
| `UInt128` | Unsigned 128-bit integer | 16 | 0 to 2¹²⁸−1 |
| `UInt256` | Unsigned 256-bit integer | 32 | 0 to 2²⁵⁶−1 |
| `Real32` | Single-precision floating point | 4 | ±1.18 × 10⁻³⁸ to ±3.40 × 10³⁸ |
| `Real64` | Double-precision floating point | 8 | ±2.23 × 10⁻³⁰⁸ to ±1.80 × 10³⁰⁸ |
| `UTF8` | UTF-8 code unit | 1 | 0 to 255 |
| `UTF16` | UTF-16 code unit | 2 | 0 to 65,535 |
| `UTF32` | UTF-32 code unit | 4 | 0 to 0x10FFFF |
| `Bool` | Boolean value | 1 | `true` or `false` |
| `Void` | Absence of value | 0 | — |
| `Auto` | Inferred static type | varies | — |

The `Void` type represents the absence of a value. A function that returns `Void` produces no value for its caller. `Void` is an ordinary zero-sized type. There is no exception mechanism in Paxsy; errors are values, not exceptions. The `Auto` type is a placeholder that the compiler replaces with a concrete type according to rules described later in this section.

Numeric literals may be written in several bases. A decimal literal is a sequence of decimal digits, optionally containing underscores as visual separators. A binary literal begins with `0b` and contains only the digits `0` and `1`. An octal literal begins with `0o` and contains the digits `0` through `7`. A hexadecimal literal begins with `0x` and contains the digits `0` through `9` and the uppercase letters `A` through `F`. Underscores may be inserted between digits in any base and are ignored by the compiler. The following assignments illustrate the syntax:

```
let decimal: Int32 = 1_000_000;
let binary: Int32 = 0b1010_1100;
let octal: Int32 = 0o755;
let hex: Int32 = 0xDEAD_BEEF;
```

A floating-point literal contains a decimal point, or an exponent, or both. The exponent is introduced by `e` for decimal floating-point and by `p` for hexadecimal floating-point. In a hexadecimal floating-point literal, the digits before and after the point are hexadecimal, and the exponent after `p` is a decimal integer denoting a power of two. For example, `0x1.2p3` denotes the value 1.125 multiplied by 2³, which is 9.0.

A numeric literal may carry a suffix that constrains its type. The suffix `U` indicates that the literal belongs to the unsigned integer family. The suffix `f` indicates that the literal belongs to the floating-point family. With no suffix, an integer literal is given the smallest signed integer type that can represent its value, and a floating-point literal is given type `Real64`. With the `U` suffix, the compiler selects the smallest unsigned integer type that can represent the value. With the `f` suffix, the compiler selects the smallest floating-point type that can represent the value, which is `Real32` if the value fits in `Real32` and `Real64` otherwise. The following declarations demonstrate the inference:

```
let small = 5;      // Int8
let medium = 300;     // Int16
let large = 100000;    // Int32
let huge = 10_000_000_000; // Int64
let unsigned = 255U;   // UInt8
let single = 3.14f;    // Real32
let double = 3.14;    // Real64
let wide = 1e40f;     // Real64, because 1e40 does not fit in Real32
```

The inference of integer literals is platform-independent. The value `5` becomes `Int8` on every platform, regardless of the width of the CPU registers or the operating system. This rule ensures that programs behave identically wherever they are compiled.

Paxsy provides four **type families**: `Int`, `UInt`, `Real`, and `Char`. A type family is not a single concrete type but a set of related types from which the compiler selects one according to the value assigned to the variable. The selection is made at the first assignment, and once made it is permanent: the variable's concrete type never changes. The `Int` family selects the smallest signed integer type that can hold the assigned value. The `UInt` family selects the smallest unsigned integer type that can hold the assigned value. The `Real` family selects `Real64` by default, or `Real32` if the value is suffixed with `f` and fits in `Real32`. The `Char` family selects `UTF8` on every platform and represents one UTF-8 code unit. A Unicode scalar value that encodes to multiple UTF-8 bytes is represented by a `UTF32` value or by its UTF-8 byte sequence; a single `Char` always occupies exactly one byte. The following declarations demonstrate the families:

```
let a: Int = 5;      // Int8
let b: Int = 300;     // Int16
let c: Int = 100000;    // Int32
let d: UInt = 255;     // UInt8
let e: Real = 3.14159;   // Real64
let f: Real = 3.14159f; // Real32
let g: Char = 'A';     // UTF8
let h: UTF32 = 'é' as UTF32;   // one Unicode scalar value
let i: UTF32 = '𐍈' as UTF32; // one Unicode scalar value
let first_byte: Char = 'A';   // one UTF-8 code unit
```

The family mechanism exists because it gives the programmer a way to write code that is portable across platforms while still allowing the compiler to choose compact representations. A variable declared as `Int` with the value 5 occupies one byte, not eight, and yet the program that contains it will compile and behave identically on every architecture.

The `Auto` type is a general type-inference mechanism that is broader than the family types. A variable declared with `Auto` may become any concrete type: an integer, a floating-point value, a pointer, an array, a struct, or anything else. The concrete type is determined at the first assignment and is permanent thereafter. Unlike `Int`, which is restricted to signed integers, `Auto` can become a value of any type. The following declarations illustrate:

```
let x = 5;         // x becomes Int8
let y = "hello";      // y becomes Char[]
let z = {1, 2, 3};     // z becomes an array of three Int8 values
```

The rules of literal type inference that apply to the family types also apply to `Auto`. Once the concrete type is fixed, subsequent assignments must be compatible with that type; an attempt to assign a value of a different type is a compile-time error unless an implicit conversion exists.

Paxsy does not have a `null` keyword. A pointer is a value of type `&T`, where `T` is the type of the data it points to. A pointer may hold the address `0`, which is called the **zero pointer** and indicates that the pointer does not refer to any valid object. The zero pointer is compared with the integer literal `0`: `if ptr == 0 { ... }`. Dereferencing a pointer whose value is `0` results in undefined behaviour. A pointer that does not refer to any valid object but whose value is not zero also results in undefined behaviour when dereferenced, but the language does not and cannot detect this case; avoiding it is the programmer's responsibility. The compiler may detect some of these cases at compile time and report them as errors, but the language does not guarantee any particular behaviour at runtime for the remaining cases, and the programmer is responsible for preventing them.

The address-of operator `&` produces a pointer to a variable. The dereference operator `@` accesses the value that a pointer refers to. When `@` appears on the left side of an assignment, it modifies the value that the pointer refers to. The following statements demonstrate the operators:

```
let a: Int32 = 5;
let p: &Int32 = &a; // p holds the address of a
@p = 10;        // a is now 10
let b: Int32 = @p;   // b is 10
```

The `@` operator binds tightly to its operand, so `@p` is the value pointed to by `p`, and `@p + 1` is that value plus one.

Paxsy provides **error-carrying types**, written with the suffix `!`. A type `T!` may hold either a value of type `T` or an error value. A function whose return type is suffixed with `!` may return an error instead of a value. Error-carrying types are the foundation of Paxsy's error handling model and are described in full in the section on functions.

A **string literal** is a sequence of characters enclosed in double quotes. The type of a string literal is `Char[]`, which is a null-terminated array of `Char` values. The null terminator is a `Char` value equal to zero that follows the last character of the string. String literals support **interpolation**: inside the string, an expression enclosed in `{` and `}` is evaluated, converted to a string representation, and inserted at that position. The following statements illustrate:

```
let count: Int32 = 5;
let name: Char[] = "Alice";
let greeting: Char[] = "Hello, {name}, you have {count} messages.";
```

The expression inside the braces may be any expression, and it is evaluated in the surrounding scope. The result of the interpolation is a new `Char[]` value. When a string literal is assigned to a fixed-size array of `Char`, the array must be large enough to hold every character of the string plus the null terminator. If the array is larger than necessary, the remaining elements are filled with zero. If the array is smaller, the compiler reports an error. The following statements illustrate:

```
let a: Char[8] = "Tom"; // {'T', 'o', 'm', 0, 0, 0, 0, 0}
let b: Char[4] = "Tom"; // {'T', 'o', 'm', 0}
let c: Char[3] = "Tom"; // compile-time error: no room for the null terminator
```

The `Char` type is a family, and its concrete type is selected according to the same rule as the other families: a variable declared `Char` becomes `UTF8`, on every platform, and the selection is permanent for the variable. A character value may be converted to another encoding with the `as` operator: `'e' as UTF16` re-encodes the code point into the target form. When the target is narrower than the code point and the value is known at compile time, the compiler reports an error; at runtime the code point is truncated to the target width, consistently with the wrapping rule for integer arithmetic. The fixed default keeps programs that use `Char` for text deterministic across platforms, while `as` gives the programmer explicit control wherever another encoding is required.

## Variables and Declarations

A **variable** is a named region of memory that holds a value. A variable is introduced by a `let` declaration, which names the variable, optionally gives its type, and optionally provides an initial value. The syntax has three forms. The first form declares a variable without a type and without an initial value:

```
let x;
```

The variable `x` is given the type `Auto`, which means that its concrete type will be determined by its first assignment. Until that assignment, the variable cannot be used. The second form declares a variable with a type and without an initial value:

```
let x: Int32;
```

The variable `x` has type `Int32` and is uninitialised. The compiler places uninitialised variables in the `.bss` section, where they are zeroed by the operating system when the program starts. Access to a variable before it is assigned is a compile-time error. The third form declares a variable with a type and an initial value:

```
let x: Int32 = 5;
```

The variable `x` is initialised to 5 and can be used immediately. The type annotation may be omitted when the initial value's type is obvious:

```
let x = 5;     // Int8
let y = 3.14;    // Real64
let z = "hello"; // Char[]
```

Variables in Paxsy are mutable by default. A declaration may qualify its type with `const` to make the binding immutable:

```
let fixed_var: const Int = 0;
let mutable_var: Int = 1;

// fixed_var = 2; // compile-time error: const binding cannot be reassigned
mutable_var = 2;
```

A `const` binding must be initialised in its declaration and cannot be assigned again. For a struct or array value, `const` also prevents mutation through that binding. For a pointer, `const` qualifies the pointer value itself; the object addressed by the pointer retains its own mutability unless its pointee type is separately qualified. The qualifier is shallow and does not recursively make referenced objects immutable. Type-family resolution occurs before the qualifier is applied, so `const Int` resolves to the same concrete integer type that `Int` would select for the initializer.

A variable declared without an initial value is uninitialised and cannot be read until it is assigned. The compiler reports an error if a variable is read before it has been assigned a value.

Multiple variables may be declared in a single `let` statement using parentheses:

```
let (a: Int32 = 5, b: Int32 = 10);
let (c, d) = {1, 2.0};
```

In the first form, each variable has its own type annotation and initial value. In the second form, a tuple is destructured into the named variables, and each variable receives the corresponding element of the tuple.

## Composite Types

Paxsy provides four composite types: arrays, tuples, structs, and unions, together with enumerations, which are a distinct kind of type definition that interacts closely with the composite types. Each composite type groups multiple values into a single value, and each has a distinct memory layout and set of operations.

### Arrays

An **array** is an ordered sequence of values of the same type, stored contiguously in memory. The type of an array is written by appending `[]` to the element type, or by writing a fixed size inside the brackets. An array variable is declared by writing the variable name, a colon, and the array type:

```
let a: Int32[4];
```

This declaration allocates space for four `Int32` values. The elements are uninitialised, and the compiler places the array in the `.bss` section if it is a top-level or static variable, or on the stack if it is a local variable. An array may be initialised with an array literal, which is written with curly braces:

```
let a: Int32[4] = {1, 2, 3, 4};
```

If the number of values in the literal is less than the size of the array, the remaining elements are zero-initialised:

```
let a: Int32[4] = {1, 2}; // {1, 2, 0, 0}
```

If the number of values exceeds the size of the array, the compiler reports an error. If the size is omitted from the type, the array's size is inferred from the number of values in the literal:

```
let a: Int32[] = {1, 2, 3, 4}; // four elements
```

Elements of an array are accessed by index, using square brackets. The index is an integer expression, and the first element has index zero:

```
let a: Int32[4] = {10, 20, 30, 40};
let first: Int32 = a[0]; // 10
let third: Int32 = a[2]; // 30
```

Accessing an index outside the bounds of the array results in undefined behaviour. The compiler may detect some cases at compile time and report them as errors, but the language does not guarantee any particular behaviour at runtime, and the programmer is responsible for preventing out-of-bounds access.

Indexing an array with an enumeration (a range expression, described in the section on enumerations) yields a *view* into the original array: a pointer to the first selected element together with a count. No elements are copied. The view is an lvalue, so reading and writing through it access the original storage, and it carries its length in a `len` field. The type of a view is written with trailing `[..]`: the value of `a[2..5]` below has type `Int32[..]`, and an array coerces to a view wherever a view is expected.

```
let a: Int32[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
let s1 = a[2..5]; // view of elements 2 through 5, no copy
s1[0] = 99;     // a[2] is now 99
let s2 = a[..<5]; // view of elements 0 through 4
let s3 = a[5>..]; // view of elements 6 through 9
```

Indexing an array with an ordinary array of integers yields a *gather*: a new array holding the selected elements, in the order given by the index array. Duplicates and reordering are permitted. A gather copies, because an arbitrary index list cannot be represented as a contiguous view.

```
let idx: Int32[3] = {4, 0, 2};
let g = a[idx];   // new array {a[4], a[0], a[2]}
```

Indexing with an enumeration is free; the gather is the only form of indexing that allocates.

### Tuples

A **tuple** is an ordered sequence of a fixed number of values that may have different types. A tuple is declared by writing the comma-separated list of types in parentheses:

```
let t: (Int32, Real64) = {5, 3.14};
```

A tuple literal is written with curly braces, and its type is inferred from the values. Elements of a tuple are accessed by index, using square brackets with a constant index:

```
let first: Int32 = t[0];   // 5
let second: Real64 = t[1]; // 3.14
```

A tuple may be destructured into separate variables using a `let` declaration with a tuple pattern:

```
let (a, b) = t; // a is 5, b is 3.14
```

The types of the destructured variables are inferred from the types of the tuple's elements. Tuples are compared element-wise; two tuples are equal if all corresponding elements are equal. A function may return a tuple to return multiple values:

```
func divide(a: Int32, b: Int32): (Int32, Int32) {
 return {a / b, a % b};
}

let (quotient, remainder) = divide(10, 3);
```

### Structs

A **struct** is a composite type consisting of a fixed set of named fields, each with its own type. Structs are the primary means of grouping related data in Paxsy. A struct is defined with the `Struct` keyword inside a `static` declaration:

```
static Point = Struct {
 x: Real64,
 y: Real64,
};
```

The name of the struct is an identifier. The fields are listed inside curly braces, each with a name and a type, separated by commas. The order of the fields in the definition determines their order in memory. A struct definition does not allocate memory; it introduces a new type that can be used to declare variables.

A variable of struct type is declared like any other variable:

```
let p: Point;
```

Such a variable is uninitialised and is placed in the `.bss` section. A struct variable may be initialised with a struct literal, which is written by naming the struct and providing values in curly braces. There are two forms of struct literal: positional and named. In the positional form, the values appear in the order of the fields:

```
let p: Point = Point {1.0, 2.0};
```

In the named form, each value is preceded by the field name and an equals sign, and the fields may appear in any order:

```
let p: Point = Point {.x = 1.0, .y = 2.0};
```

Fields are accessed with the dot operator:

```
let x: Real64 = p.x;
p.y = 3.0;
```

An ordinary struct uses the language's native layout: fields remain in declaration order, each field begins at the next offset satisfying its natural alignment, and the final size is rounded up to the maximum field alignment. This layout is deterministic for a given target ABI. Use `extern Struct` when a type must match the target platform's C ABI, and `packed Struct` when a byte-exact layout is required. These modifiers are part of the type declaration:

```
static Example = Struct {
 a: Int8,
 b: Int32,
 c: Int16,
};
```

The field `a` has size 1 and alignment 1, so it is placed at offset 0. The field `b` has size 4 and alignment 4, so it must be placed at an offset that is a multiple of 4; the smallest such offset that is at least 1 is 4. The field `c` has size 2 and alignment 2, so it must be placed at an offset that is a multiple of 2; the smallest such offset that is at least 8 is 8. The total size before rounding is 10, but the struct's alignment is the maximum of its fields' alignments, which is 4, so the total size is rounded up to 12. The layout is therefore: byte 0 holds `a`, bytes 1 through 3 are padding, bytes 4 through 7 hold `b`, bytes 8 and 9 hold `c`, and bytes 10 and 11 are trailing padding. The compiler never reorders fields, at any optimisation level. Field offsets and total size are stable for a given target ABI, but may differ between ABIs. Ordinary structs are appropriate for language-native data. Use `extern Struct` for C ABI compatibility and `packed Struct` for byte-exact formats or hardware register maps.

#### Extern structs

An `extern Struct` uses the target platform's C ABI layout and calling convention rules for its fields. Field order is preserved; field alignment, inter-field padding, tail padding, and aggregate alignment follow the C ABI selected by the compilation target. This is the form intended for foreign-function interfaces, operating-system structures, and hardware or binary interfaces whose layout is defined by a platform ABI:

```
static Vector = extern Struct {
 x: Real32,
 y: Real32,
 z: Real32,
 w: Real32,
};
```

Every field of an extern struct must have a representation supported by the target ABI. Its layout follows the target ABI. An extern struct is layout-compatible with the corresponding C structure when the field types and target ABI agree.

#### Packed structs

A `packed Struct` places fields consecutively in declaration order without inter-field or trailing padding. Its alignment is one byte. The size is the sum of the storage sizes of its fields. Field access may therefore be unaligned; the compiler generates accesses that preserve the specified byte layout:

```
static PacketHeader = packed Struct {
 kind: UInt8,
 length: UInt32,
};
```

In this example, `kind` is at offset 0, `length` is at offset 1, and the total size is 5 bytes. `packed` controls byte layout; it does not introduce C-style bit fields. Fields must have a statically known storage size. `extern` and `packed` are mutually exclusive modifiers.

A struct value is copied when it is assigned to another variable or passed to a function. The copy is a shallow copy; pointer fields point to the same objects in both copies. Struct values may be stored in the `.data` section if they are initialised at compile time, in the `.bss` section if they are uninitialised, or on the stack if they are local variables.

Methods on structs are declared outside the struct definition. A method is a function whose first parameter has the struct type, and whose name is qualified by the struct name. The first parameter is an ordinary parameter that the method's author names; by convention it is named `this`, and its type is the struct on which the method is declared. There is no implicit parameter and no `self` keyword.

```
func Point.distance(this: Point, other: Point): Real64 {
 let dx = this.x - other.x;
 let dy = this.y - other.y;
 return sqrt(dx * dx + dy * dy);
}
```

The name `Point.distance` is the function's identity; it is not an overload of a function named `distance`. A method may be called with dot syntax on an instance of the struct:

```
let p: Point = Point {3.0, 4.0};
let q: Point = Point {0.0, 0.0};
let d = p.distance(q);
```

The same method may also be called with the full name:

```
let d = Point.distance(p, q);
```

Both forms are equivalent. A method may be defined for multiple structs at once:

```
func {Point, Vector3}.magnitude(): Real64 { ... }
```

This defines a function named `magnitude` for both `Point` and `Vector3`. The full names `Point.magnitude` and `Vector3.magnitude` refer to the same function. A method declared for multiple structs at once cannot reference instance fields, because the fields differ between the two types. Such methods are useful only when the body does not depend on the instance's fields, and the parameter list may be empty in this case.

### Unions

A **union** is a composite type in which all fields share the same memory. A union is defined with the `Union` keyword inside a `static` declaration:

```
static Value = Union {
 as_int: Int32,
 as_real: Real32,
};
```

The fields of a union all begin at offset zero, so the size of a union is the size of its largest field, rounded up to the alignment of the largest field. The layout of the union above is four bytes, because both fields have size 4 and alignment 4. A union value is created by initialising the first field:

```
let v: Value = Value {42};
```

Only the first field may be initialised with a literal. After the union is created, any field may be accessed:

```
v.as_real = 3.14;
let x: Int32 = v.as_int; // reinterprets the bits of 3.14 as Int32
```

Reading a field other than the one most recently written reinterprets the underlying bytes. This is the intended use of unions: to provide multiple views of the same storage.

### Enums

An **enum** is a type whose values are a fixed set of named constants. An enum is declared with `Enum` inside a `static` type declaration. The optional integer type immediately following `Enum` selects the enum's underlying representation. An empty body is valid and declares an enum with no named variants:

```
static Color = Enum {
 Red,
 Green,
 Blue,
};

static E = Enum Int32 {};

static State = Enum Int32 {
 Ready = 1,
 Running = 2,
 Stopped = 3,
};
```

When the underlying type is omitted, it defaults to `Int32`. An explicit underlying type must be a fixed-width integer type (`Int8`, `Int16`, `Int32`, `Int64`, `Int128`, `Int256`, or the corresponding unsigned type). Each variant's value must be representable by that type. Variants without an explicit value receive the next integer after the preceding variant; the first implicit value is zero. Explicit values may be mixed with implicit values. Duplicate numeric values are permitted and represent aliases for the same underlying value.

The underlying type determines the enum's size, alignment, signedness, and representable range. `sizeof(E)` and `alignof(E)` are identical to those of the selected underlying integer type. A variable of enum type is declared like any other variable:

```
let state: E = E.Running;
```

Enum values support equality and ordering comparisons through their underlying integer values. They are used with the `match` construct, which is described in the control-flow section. An enum value may be converted to its underlying integer type with `as`; converting an integer to an enum requires an explicit `as` cast and preserves the numeric value, whether or not a named variant has that value.

## Operators

Paxsy provides a rich set of operators for arithmetic, bitwise manipulation, comparison, logical combination, casting, and memory access. The following subsections describe each category in turn, and the section concludes with a table of precedence and associativity.

The arithmetic operators are `+` for addition, `-` for subtraction, `*` for multiplication, `/` for division, and `%` for modulus. They apply to the integer and floating-point types. For integer types, division truncates toward zero and modulus has the sign of the dividend. For floating-point types, division produces the result of dividing the two operands, rounded to the nearest representable value with ties broken toward the even significand, and the modulus operator is not defined for floating-point operands. The compound assignment operators `+=`, `-=`, `*=`, `/=`, and `%=` perform the corresponding operation and assign the result to the left operand.

For integer types, the operations `+`, `-`, `*`, unary `-`, and their compound forms wrap: the result is the low N bits of the exact mathematical result, interpreted as an N-bit two's-complement value. For example, `127 as Int8 + 1` yields `-128`, and `255 as UInt8 + 1` yields `0`. Wrapping applies on every platform and at every optimisation level; the language has no trapping arithmetic. Division and remainder keep the rules stated above and are unaffected by wrapping; division by zero remains undefined, as before.

Automatic type promotion occurs during arithmetic operations when the two operands have different arithmetic types. The narrower operand is promoted to the wider type before the operation is performed. The following example demonstrates:

```
let x: Int32 = 5;
let y: Real64 = 2.5;
let z: Real64 = x + y; // x is promoted to Real64
```

The result of the expression has the wider type, and the value of the narrower operand is preserved in the promotion.

The bitwise operators are `~` for bitwise NOT, `&` for bitwise AND, `$` for bitwise OR, `^` for bitwise XOR, `<<` for left shift, `>>` for arithmetic right shift, and `>>>` for logical right shift. The operator `<<<` is also provided as a synonym for `<<`. The compound assignment forms `&=`, `$=`, `^=`, `<<=`, `>>=`, and `>>>=` are also provided. Bitwise operators apply only to integer types. Paxsy spells bitwise OR as `$`; the single pipe `|` is used as a delimiter in constructs such as `for ... |item|` and `catch |error|`, while `||` is the symbolic logical-OR operator. An arithmetic right shift of a negative value fills the vacated high bits with copies of the sign bit; a logical right shift fills them with zero.

The comparison operators are `==` for equality, `!=` for inequality, `<` for less than, `>` for greater than, `<=` for less than or equal to, and `>=` for greater than or equal to. They produce a `Bool` value. The logical operators are `and`, `or`, and `not`; `&&` and `||` are their symbolic aliases. The `and` operator returns `true` if both operands are `true`, the `or` operator returns `true` if at least one operand is `true`, and the `not` operator returns the negation of its operand. Both `and` and `or` short-circuit: the right operand is evaluated only if the left operand does not determine the result.

The `as` operator performs a type conversion. It may convert between any two types, and the compiler generates the appropriate instructions. A conversion from a wider integer type to a narrower one truncates the high bits. A conversion from a signed to an unsigned type reinterprets the bits. A conversion from a floating-point type to an integer type truncates the fractional part. A conversion from an integer type to a floating-point type rounds to the nearest representable value. A conversion between a pointer type and an integer type reinterprets the address as an integer. The programmer is responsible for ensuring that the conversion is meaningful; the compiler performs the conversion as requested and does not insert runtime checks. The `as` operator always produces a value of the destination type and never modifies the source operand.

The `&` operator, applied as a prefix to an expression, produces the address of that expression. The operand must be an lvalue: a variable, a field of a struct, an element of an array, or a dereferenced pointer. The result has type `&T`, where `T` is the type of the operand.

The `@` operator, applied as a prefix to a pointer expression, dereferences the pointer. On the left side of an assignment, it modifies the value that the pointer refers to. On the right side, it reads that value.

The `<-` operator is the channel send and receive operator. Its use is described in full in the section on concurrency, and it is mentioned here only so that the reader is aware of its existence and its place among the other operators.

The following table lists the operators in order of decreasing precedence. Operators on the same row have the same precedence and are evaluated from left to right, except where noted.

| Precedence | Operators | Associativity |
|------------|-----------|---------------|
| Highest | `@` `&` `~` `not` `-` (unary) | Right to left |
| | `as` | Left to right |
| | `*` `/` `%` | Left to right |
| | `+` `-` | Left to right |
| | `<<` `>>` `>>>` `<<<` | Left to right |
| | `&` | Left to right |
| | `^` | Left to right |
| | `$` | Left to right |
| | `==` `!=` `<` `>` `<=` `>=` | Left to right |
| | `and` `&&` | Left to right |
| | `or` `||` | Left to right |
| Lowest | `<-` | Right to left |

The `&&` and `||` spellings are symbolic aliases for `and` and `or`, respectively. The `&` operator appears twice in the table. As a unary prefix operator, it is the address-of operator and has the highest precedence. As a binary infix operator, it is the bitwise AND operator and has lower precedence. The precedence depends on whether the operator is used in prefix or infix position.

The precedence of operators is a matter of convention and convenience. It determines the order in which operations are performed when an expression contains multiple operators, and it is the reason that `a + b * c` means `a + (b * c)` rather than `(a + b) * c`. When in doubt, the programmer should use parentheses to make the intended order explicit; the compiler does not warn about redundant parentheses, and readability is improved by their use.

## Control Flow

Paxsy provides several constructs for controlling the order in which statements are executed: the `if`-`else` construct, the compile-time existence checks, the `match` construct, the `while`, `do`-`while`, and `for` loops, the `break` and `continue` statements with optional labels, and the `pass` statement.

Every control-flow construct takes **one statement** as its body. A statement is either a block `{ ... }` or any single statement, so braces are optional for a one-statement body and `else if` chains compose naturally:

```px
if x > 3 std::inout::println("big");   // one statement, no braces
else if x > 1 std::inout::println("mid"); // chains without nesting blocks
else {                  // a block is one statement too
 std::inout::println("small");
 pass;
}
```

The `{ ... }` block is itself a full statement: it opens a scope, executes its items in order, and can appear anywhere a statement can.

The `if`-`else` construct evaluates a condition and executes its body statement if the condition is true, and optionally the `else` body if the condition is false:

```
if condition {
 statements;
} else if condition {
 statements;
} else {
 statements;
}
```

The condition is an expression of type `Bool`. It is not enclosed in parentheses. The body of each branch is enclosed in curly braces, even if it contains only one statement. The `else if` form is a shorthand for nesting an `if` inside an `else`; it may be repeated as many times as needed. The `if` construct may also be used as an expression, in which case the value of the construct is the final expression in the branch that was executed. The final expression may be written with or without a semicolon immediately before the branch's closing brace:

```
let max = if a > b { a } else { b };
```

If the `else` branch is omitted from an `if` expression, the expression has type `Bool` and its value is the result of the condition:

```
let is_positive: Bool = if x > 0;
```

### Compile-Time Existence Checks

Paxsy provides compile-time existence checks (`if let`, `if func`, and `if static`). The examples in this subsection are part of the grammar and backend contract.

#### The `if let` Form

The form `if let x { ... } else { ... }` checks whether a variable named `x` is visible in the current scope. If it is, the first block is compiled and executed. If it is not, the `else` block, if present, is compiled and executed. The following example demonstrates the taken branch:

```
let x: Int32 = 5;
if let x {
 // x is visible; this branch is compiled.
 pass;
} else {
 // this branch is discarded.
 pass;
}
```

The following example demonstrates the discarded branch:

```
if let missing {
 // missing is not visible; this branch is discarded.
 pass;
} else {
 // this branch is compiled.
 pass;
}
```

#### The `if func` Form

The form `if func x { ... } else { ... }` checks whether a function named `x` is visible in the current scope. The following example demonstrates the taken branch:

```
func compute(): Int32 { return 42; }

if func compute {
 // compute is visible; this branch is compiled.
 pass;
} else {
 // this branch is discarded.
 pass;
}
```

The following example demonstrates the discarded branch:

```
if func absent {
 // absent is not visible; this branch is discarded.
 pass;
} else {
 // this branch is compiled.
 pass;
}
```

#### The `if static` Form

The form `if static x { ... } else { ... }` checks whether a `static` name `x` is visible in the current scope. The following example demonstrates the taken branch:

```
static MAX = 100;

if static MAX {
 // MAX is visible; this branch is compiled.
 pass;
} else {
 // this branch is discarded.
 pass;
}
```

The following example demonstrates the discarded branch:

```
if static NOT_DEFINED {
 // NOT_DEFINED is not visible; this branch is discarded.
 pass;
} else {
 // this branch is compiled.
 pass;
}
```

The `else` branch is optional. A branch that is discarded due to an existence check does not need to type-check, provided the check is resolved at compile time. This feature allows a program to adapt to the presence or absence of declarations without resorting to conditional compilation directives.

### The match Construct

The `match` construct compares a value against a sequence of patterns and executes the block associated with the first matching pattern:

```
match value {
 case 1 { statements; }
 case 2 or 3 or 4 { statements; }
 case Color.Green { statements; }
 else { statements; }
}
```

The value is not enclosed in parentheses. Each `case` introduces a pattern followed by a block of statements in curly braces. The pattern may be a literal, a named constant, an enum variant, a list of these joined by the `or` keyword, or a boolean guard expression. The else branch is optional. When it is present, it handles any value that does not match any of the cases. When it is absent and no case matches, the match construct produces no effect. There is no fallthrough: after the block of a matching case is executed, control leaves the match construct. The break statement is not used and is not needed in a match. The match construct may be used as an expression, in which case the value of the construct is the value produced by the executed block; all blocks must produce a value of the same type, and the else branch is required when the construct is used as an expression, because the construct must produce a value on every path.

The `or` keyword inside a `case` joins a list of alternative constant patterns; each alternative is a distinct value, and the list form is not a logical expression. A `case` may instead hold a boolean guard: an arbitrary expression of type `Bool`, which may mention the matched value and any visible variable, and which matches when it evaluates to true. A case that contains any operator — a comparison, a logical `and`/`or`/`not` over subexpressions, arithmetic — is a guard, and the whole case is evaluated as one expression; the list form and the guard form may not be mixed within a single case. Cases are evaluated top to bottom, and the block of the first matching case runs.

### The while Loop

The `while` loop evaluates a condition before each iteration and executes its body as long as the condition is true:

```
while condition {
 statements;
}
```

The condition is not enclosed in parentheses. The body is enclosed in curly braces. If the condition is false on the first evaluation, the body is not executed at all.

### The do-while Loop

The `do`-`while` loop executes its body first and evaluates its condition afterwards, so the body is always executed at least once:

```
do {
 statements;
} while condition;
```

The semicolon after the `while` clause is mandatory. The condition is not enclosed in parentheses. The body is enclosed in curly braces.

### The for Loop

The `for` loop iterates over a range of values. The range is written with two boundaries separated by `..`, and the loop variable is introduced inside `|` characters:

```
for 0..9 |i| {
 statements;
}
```

Ranges in Paxsy are inclusive of both boundaries, so the loop above iterates from 0 through 9. The loop variable is a copy of the current value of the range, and it is valid only within the body of the loop. The type of the loop variable is inferred from the type of the range, and it may be given explicitly:

```
for 0..9 |i: Int32| {
 statements;
}
```

An open-ended range omits the upper boundary and requires an explicit `break` inside the loop:

```
for 0.. |i| {
 if i >= 10 { break; }
 statements;
}
```

A range may also be written with `<` and `>` modifiers to control inclusivity. The modifier `<` before a boundary excludes that boundary, and the modifier `>` includes it (the default). The forms `0..<10`, `..<5`, and `5>..` are all valid. A range may carry a step and may mark its end with `..=` to guarantee inclusion; the full semantics of ranges are given in the section on enumerations.

The `for` loop also iterates the elements of an array or a view directly: `for a |x| { ... }` visits each element of `a` in order, and `for a[2..5] |x| { ... }` visits the elements of the view without copying them.

### Enumerations

A range expression such as `0..10` is a value in its own right, called an
**enumeration**. An enumeration generates the sequence of values from its
start to its end and may be used anywhere a sequence is wanted: as the
source of a `for` loop, as an array index producing a view, or as the
source of a gather index array. Both boundaries are inclusive by
default, and the forms `0..<10`, `0>..10`, and `0>..<10` exclude the
marked boundary.

```
0..10      // 0, 1, 2, ..., 10
0..<10     // 0, 1, 2, ..., 9
'a'..'z'    // 'a', 'b', ..., 'z'
0.0..10.0    // 0.0, 1.0, ..., 10.0
```

An enumeration may carry a `step` and may mark its end with `..=` to
guarantee that the end value is emitted even when the step grid does not
land on it:

```
0..10 step 2 // 0, 2, 4, 6, 8, 10
0..9 step 2   // 0, 2, 4, 6, 8
0..=9 step 2 // 0, 2, 4, 6, 8, 9
0.0..1.0 step 0.1
```

The `step` must be a positive constant expression: an integer for
integer and character ranges, a `Real` constant for real ranges. The
k-th element of an enumeration is computed as `start + k*step`, never by
repeated addition, so floating-point enumerations do not accumulate
rounding error. With a plain `..` bound, the last emitted value is the
largest `start + k*step` that does not exceed the end; with `..=`, the
end itself is appended if the grid has not reached it.

Both boundaries of an enumeration must have the same type; `0..10.0` or
`'a'..0.0` is a compile-time error. Character enumerations follow the
code table of the concrete encoding of their bounds: `'a'..'z'` over
UTF-8 bounds enumerates characters in increasing code-point order.
Enumerations are ascending; an enumeration whose start exceeds its end
is empty. An enumeration with an omitted boundary is open-ended; such a
value may appear only as the source of a `for` loop, where the body must
terminate it with `break`, and may not be stored in a variable or used
as an index.

### Labels, break, and continue

The `break` statement exits the innermost enclosing loop immediately. The `continue` statement skips the rest of the current iteration and proceeds to the next. Both statements accept an optional label to refer to an outer loop:

```
outer: for 0..9 |i| {
 inner: for 0..9 |j| {
    if i * j > 20 { break outer; }
    if j % 2 == 0 { continue outer; }
    statements;
 }
}
```

A label is written before the loop keyword followed by a colon. Labels must be unique within the function.

### The pass Statement

The `pass` statement emits a single machine `NOP` instruction. It is an ordinary statement and is not an empty statement. It is used to create deliberate delays, to align code, to provide a breakpoint location during debugging, and to fill branches that would otherwise be empty. A `NOP` instruction occupies one byte of machine code in most architectures and consumes one cycle (or a small number of cycles) when executed. The `pass` statement differs from an empty statement, which the compiler discards without emitting any code; `pass` is always emitted. A common idiom is a busy-wait loop:

```
let i: Int32 = 0;
while i < 1000000 {
 pass;
 i += 1;
}
```

Use `pass` only when an intentional no-op or machine `NOP` is part of the program. It is not a substitute for an unfinished branch; complete the branch before treating the program as ready.

## Functions

A **function** is a named block of statements that may be called from other parts of the program. Functions are the primary means of structuring a Paxsy program. A function definition consists of the `func` keyword, the function's name, a parameter list in parentheses, the return type, and a body in curly braces:

```
func add(a: Int32, b: Int32): Int32 {
 return a + b;
}
```

Each ordinary parameter has a name and may include a type annotation after a colon. If the annotation is omitted, the current compiler assigns the parameter type `Int64`; this shorthand is retained for compatibility, but explicit annotations are recommended. A `static` parameter is a separate compile-time type parameter form described below. The return type follows the parameter list and a colon. A function that returns no value has return type `Void`. The body is a sequence of statements. A function with a non-`Void` return type must return a value on every path through the body; the compiler checks this requirement. A function may return early with the `return` statement, which accepts an expression of the function's return type:

```
func absolute(x: Int32): Int32 {
 if x < 0 {
    return -x;
 }
 return x;
}
```

A `Void` function may return early with the statement `return;`, which accepts no expression. Control reaching the end of a `Void` function's body is equivalent to a `return;` at that point.

A function whose body is a single expression may be written in the shorthand form, which omits the braces and the `return` keyword and uses an equals sign:

```
func square(x: Int32): Int32 = x * x;
```

The shorthand form is equivalent to a function body containing a single `return` statement.

A function may be declared before it is defined by writing its signature followed by a semicolon:

```
func add(a: Int32, b: Int32): Int32;
```

Such a declaration is called a **prototype**. A prototype allows a function to be called before its body appears in the source file. When the body appears in the program, its signature must match the prototype exactly, and the definition supersedes the prototype. When no body for the prototype appears in any source file of the program, the prototype declares an external symbol: the compiler records a reference to the symbol in the object file, and the linker resolves the reference against the runtime, handwritten assembly, foreign libraries, and the other objects that make up the final program. A definitionless prototype is therefore the only mechanism the language provides for referring to code that lives outside the program, and no dedicated keyword is required. Because the absence of a body is not detectable until link time, implementations are encouraged to emit a warning when a prototype has no definition.

A function may be declared as a method of a struct by qualifying its name with the struct name:

```
func Point.distance(this: Point, other: Point): Real64 {
 let dx = this.x - other.x;
 let dy = this.y - other.y;
 return sqrt(dx * dx + dy * dy);
}
```

The first parameter of the function is conventionally named `this` when the method is intended to be called on an instance, but the name is not special; the function's author chooses it. A method may be called with dot syntax on an instance of the struct, in which case the instance is passed as the first argument, or with the full name, in which case all arguments are passed explicitly:

```
let d = p.distance(q);
let d = Point.distance(p, q);
```

The full name of a method is its identity. There is no function overloading in Paxsy; two functions in the same scope may not share a name, even if their parameter lists differ. This rule means that the name `Point.distance` refers to exactly one function, and there is no ambiguity in a call.

Paxsy's error handling is built on the error-carrying types introduced in the section on data types. A function whose return type is suffixed with `!` may return an error instead of a value. Inside such a function, the statement `return error <expression>;` produces an error value and returns it to the caller. The expression is the error code; it is conventionally a named constant of an integer type:

```
static ERR_DIV_BY_ZERO = 1;

func divide(a: Int32, b: Int32): Int32! {
 if b == 0 {
    return error ERR_DIV_BY_ZERO;
 }
 return a / b;
}
```

The `try` operator, applied to an expression whose type is suffixed with `!`, propagates an error to the caller if the expression produced an error, and otherwise yields the successful value:

```
func compute(a: Int32, b: Int32): Int32! {
 let result = try divide(a, b);
 return result * 2;
}
```

The `try` operator may only be used inside a function whose return type is suffixed with `!`. Applying `try` to an expression inside a function that does not return `!` is a compile-time error.

The `catch` operator, applied to an expression whose type is suffixed with `!`, handles an error locally. The syntax is `<expression> catch |e| { statements }`, and the block must produce a value of the expression's success type:

```
let result = divide(10, 0) catch |e| {
 // handle the error
 return 0;
};
```

Inside the catch block, the variable named between the `|` characters holds the error code as an `Int64` value. The block may use it in expressions, formatting, and calls like any other local variable. The value produced by the block becomes the value of the entire `catch` expression.

### Compile-Time Parameters

A function parameter may be declared as a compile-time parameter by prefixing the parameter name with the `static` keyword. The parameter type is inferred from the argument at the call site. The syntax is:

```
func make_array(static T, size: Int32): &T! { ... }
```

At the call site, the argument passed for a compile-time parameter must be a compile-time constant. A type name is a valid compile-time constant. Inside the function body and signature, the parameter `T` may be used as a type. The following example demonstrates a function that allocates an array whose element type is a compile-time parameter:

```
func make_buffer(static T, count: Int32): &T! {
 let p: &T = alloc T[count];
 if p == 0 {
    return error 1;
 }
 return p;
}

let buf: &Int32 = try make_buffer(Int32, 16);
defer free buf;
```

The next example demonstrates a function that returns a value of a compile-time type:

```
func cast_to(static T, value: Int32): T {
 return value as T;
}

let a: Real64 = cast_to(Real64, 42);
let b: Int16 = cast_to(Int16, 42);
```

The final example demonstrates a function that uses a compile-time parameter to choose between two branches:

```
func select_value(static T, use_first: Bool, a: T, b: T): T {
 if use_first {
    return a;
 }
 return b;
}

let x: Int32 = select_value(Int32, true, 10, 20);
let y: Real64 = select_value(Real64, false, 1.0, 2.0);
```

## Memory Management

Paxsy provides two forms of memory: the stack and the heap. The stack is a region of memory that grows and shrinks as functions are called and return; it holds local variables, parameters, and saved return addresses. The heap is a region of memory that the program manages explicitly through the `alloc` and `free` operators.

Stack allocation happens automatically when a variable is declared inside a function. The compiler allocates space for the variable in the function's stack frame and releases it when the function returns. The stack frame also contains space for saved registers, the return address, and any arguments that do not fit in registers. The size of a stack frame is fixed at compile time and is computed from the sizes of the local variables and the alignment requirements of the function's calls. Local variables that are not initialised are placed in the `.bss` section, which the operating system zeroes when the program starts.

### Scope and Lifetime

A variable declared inside a block exists only within that block. When the block ends, the variable goes out of scope and can no longer be referenced. A variable declared inside a function exists until the function returns, and a parameter exists for the duration of the call. The following example shows a nested block in which the inner variable is not accessible after the block closes:

```
func example(): Void {
 let x: Int32 = 0;
 {
    let y: Int32 = 1; // y exists only in this block
    x = y;        // x can be modified from the outer scope
 }
 // y is not accessible here
 let z: Int32 = x;    // z gets the value of x, which is 1
}
```

A non-pointer assignment copies the value, so changing one variable does not change the other. The following example shows that `a` remains `5` after `b` is assigned `10`:

```
let a: Int32 = 5;
let b: Int32 = a; // b is a copy of a
b = 10;       // a is still 5
```

Two pointers to the same variable both observe a change made through either one. The following example shows both pointers reflecting the write:

```
let a: Int32 = 5;
let ptr1: &Int32 = &a;
let ptr2: &Int32 = &a;
@ptr1 = 10;     // a becomes 10
// ptr2 now points to a, which is 10
```

Heap allocation is performed with the `alloc` operator. Its general form is `alloc T[N]`, where `T` is the element type and `N` is the number of elements to allocate. When a single value is required, the element count may be omitted and the operator written `alloc T`; this form is exactly equivalent to `alloc T[1]`. The operator returns a pointer of type `&T` to the first element of the allocated block, or the zero pointer if the allocation fails. The following statements demonstrate:

```
let p: &Int32 = alloc Int32;
if p == 0 {
 return error 1;
}
@p = 42;

let page: &Int8 = alloc Int8[4096];
```

The allocated memory is not initialised. It contains whatever bit pattern was left by the previous occupant of that memory. The programmer is responsible for writing meaningful values into the allocation before reading from it.

The compiler automatically includes the standard heap implementation when either memory operator is used; programs do not need to write `use std::heap` just to use `alloc` or `free`. The operators are native Paxsy syntax and are type-checked by the compiler. A typed pointer such as `&Int32` can be passed directly to `free`; the allocator boundary treats it as an opaque address, so an explicit cast to `&Void` is unnecessary.

Heap memory is released with the `free` operator, which takes a pointer and returns the block it refers to:

```
free p;
```

Calling `free` on the zero pointer is a safe no-op. Calling `free` on a pointer that was not obtained from `alloc` results in undefined behaviour. The compiler may detect some cases at compile time and report them as errors, but the language does not guarantee any particular behaviour at runtime, and the programmer is responsible for preventing invalid frees.

The `defer` statement schedules a statement or block of statements to be executed when the enclosing function exits, whether by reaching its end, by returning early, or by propagating an error. Deferred statements are executed in last-in-first-out order: the statement scheduled last is executed first. The `defer` statement is particularly useful for releasing resources, such as heap allocations and open files, on every exit path:

```
func process(): Void! {
 let p: &Int32 = alloc Int32[1];
 if p == 0 {
    return error 1;
 }
 defer free p;
 // use p
}
```

The `free p` statement is executed when the function exits, regardless of which path it takes. If the function returns an error, the deferred statement still runs.

A deferred statement may itself return an error. If the enclosing function has an error-bearing return type, the first error produced during cleanup becomes the function's result when the function was otherwise returning successfully. If the function is already propagating an error, that original error is preserved while all remaining deferred statements still execute. When several cleanup statements fail during an otherwise successful exit, the first cleanup error in execution order is returned. A deferred fallible operation in a function with a non-error return type must handle its error locally with `catch`.

The `errdefer` statement schedules a statement to run only when the
enclosing function exits with an error, whether by `return error ...`
or by a `try` propagating out. On a normal exit, `errdefer` statements do
not run; on an error exit, the scheduled `defer` and `errdefer`
statements run together in last-in-first-out order. `errdefer` is the
idiomatic way to release a resource that a function has acquired when the
function fails, particularly when a successful return hands ownership of
the resource to the caller:

```
func duplicate(buf: &UInt8, len: Int32): &UInt8! {
 let out: &UInt8 = alloc UInt8[len];
 if out == 0 {
    return error 1;
 }
 errdefer free out;
 for 0..(len - 1) |i| {
    out[i] = buf[i];
 }
 return out; // success: errdefer does not run; the caller owns out
}
```

A `defer` statement may be prefixed with `volatile`, which prevents the compiler from removing or reordering the deferred statement:

```
volatile defer free p;
```

The `volatile` keyword, applied to a statement or a block of statements, instructs the compiler to emit the code exactly as written, without optimising it away, reordering it, or combining it with other operations. This is necessary when the code has side effects that the compiler cannot observe, such as writing to a memory-mapped hardware register or reading a value that may be changed by an interrupt. The following statements demonstrate:

```
volatile let flag: Bool = true;
volatile {
 let status = read_register(0x4000);
 if status & 0x01 != 0 {
    // handle interrupt
 }
}
```

The `volatile` keyword should be used sparingly. Applying it to ordinary variables that are not shared with hardware or interrupts prevents the compiler from performing legitimate optimisations and can significantly degrade performance.

## `static` declarations and compile-time facilities

Paxsy uses `static` as its compile-time declaration modifier. `comptime` is
not reserved and is lexed as an ordinary identifier. Constant evaluation covers
the compile-time forms documented here.

### Static constants

A module-level static constant is declared with one of these forms:

```paxsy
static MAX_SIZE = 1024;
static BASE = 6;
static SCALED = BASE * 7;
```

Static constants are immutable. Their initializer may be a side-effect-free constant expression. The HIR evaluator expands supported scalar constant expressions at use sites after project-wide type inference. It covers literal values, supported unary/binary arithmetic and comparisons, casts represented by HIR, and references to other static constants. Cyclic constant references produce a compile-time diagnostic; expressions outside the supported constant subset are not recursively evaluated. Unreferenced scalar constants with supported initializers do not leave runtime storage; taking a constant's address preserves its storage.

Static declarations cover type aliases, scalar constants, compile-time type parameters, and named callable values. Aggregate CTFE, compile-time loops, and runtime-dependent expressions are outside the compile-time subset; a declaration accepted by the parser must still satisfy the constant-expression subset.

### Static type aliases

A static declaration whose right-hand side is a type name introduces a type
alias:

```paxsy
static UserId = Int32;

func load_user(id: UserId): Void {
 // ...
}
```

The alias is compile-time type metadata and does not introduce a distinct
runtime type. It can be used in declarations and function signatures.

### Compile-time type parameters

A function parameter prefixed with `static` is a compile-time type parameter:

```paxsy
func add_as(static T, left: T, right: T): T {
 return left + right;
}

func main(): Void {
 let total: Int32 = add_as(Int32, 20, 22);
}
```

The type argument is supplied at the call site and the generic function is
specialised before HIR generation. This mechanism is distinct from value-level
const generics and from a general CTFE function evaluator.

### Other `static` forms

`static let` declares a binding with the same initializer and optional type
annotation syntax as `let`. A named function value may be bound with
`static Name = func(...) { ... };`; the resulting name is callable like an
ordinary function. These forms keep type-level declarations, constant
bindings, and callable declarations visually consistent.

### Compile-Time Builtins

The `sizeof` and `alignof` builtins are compile-time constants and may be
used anywhere an integer constant is accepted. They support scalar, pointer,
view, and fixed-array types:

```
sizeof(Int32)      // 4: size in bytes of a type
alignof(Real64)     // 8: alignment requirement of a type
```

`sizeof` accepts a type name and returns its size in bytes; `alignof` accepts a type name and returns its alignment requirement. Both results have type `UInt64`.

## Modules and Imports

A **module** is a named collection of declarations that can be imported by other parts of the program. A module is introduced by the `module` keyword followed by a name and a body in curly braces:

```
module geometry {
 static Point = Struct {
    x: Real64,
    y: Real64,
 };

 func distance(a: Point, b: Point): Real64 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
 }
}
```

Named modules may be nested. Each nested name extends the qualified path, so a declaration inside `module inner` nested in `module outer` is addressed as `outer::inner::name`:

```px
module outer {
 module inner {
    func add(a: Int, b: Int): Int { return a + b; }
 }
}

let value = outer::inner::add(2, 3);
```

A nested module can be imported as a namespace with `use outer::inner;`, which brings its direct members into the current scope. A single declaration can be imported with `use outer::inner::add;`. Nested modules may be split across source files in the same way as their parent module.

Declarations inside a named module are accessible from outside the module by qualifying their names with the module name and the `::` operator:

```
let p: geometry::Point = geometry::Point {1.0, 2.0};
let q: geometry::Point = geometry::Point {4.0, 6.0};
let d = geometry::distance(p, q);
```

A module may also be unnamed, in which case its declarations are visible throughout the project without qualification:

```
module {
 func helper(): Void {
    // ...
 }
}
```

The `helper` function declared in the unnamed module above may be called from any file in the project by its bare name.

Declarations placed outside any module are private to the file in which they appear. They are not visible to other files, even if those files are part of the same project.

The `use` directive brings the symbols of a module into the current scope. There are two forms. The first form imports the entire module:

```
use geometry;
```

After this directive, the symbols of the `geometry` module may be referenced by their bare names:

```
let d = distance(p, q);
```

The second form imports a single symbol:

```
use geometry::distance;
```

After this directive, the `distance` function may be referenced by its bare name, but other symbols of the `geometry` module must still be qualified. There is no wildcard import beyond the first form, and there is no group import syntax. The `use` directive may only refer to a module, never to a file path.

A function that is defined outside the program — by the runtime, by a foreign library, or by handwritten assembly — is declared with an ordinary prototype that has no definition in any source file of the program, as described in the section on functions. No separate keyword is needed: the compiler emits a reference to the symbol, and the linker resolves it. Foreign data is exposed in the same way, through functions supplied by the foreign code; the language provides no syntax for declaring external variables.

### Module Resolution and Imports

Modules are declared in Paxsy source files. During project semantic analysis, declarations from the collected project sources are registered as local modules, while the standard library and installed packages are registered as external module namespaces. A `use` directive connects a local module or an external package namespace to the current source scope. A missing module is a semantic error.

A whole-module import (`use geometry;`) imports the members of a local module into the current scope. A qualified import (`use std::inout;`) connects the named external package module. For the bundled standard library, available module names are discovered from source `module` declarations rather than an enumerated `[modules]` table. Imported names are then resolved by semantic analysis and HIR lowering.

The project configuration does not provide a general `[modules]` alias table. Package installation and dependency recording are toolchain operations described in the [Toolchain User Manual](toolchain-user-manual.md); they should not be confused with source-level module declarations.

### Accessing Module Members Inside the Same File

All modules declared in a single file are visible to one another within that file. A declaration in one module may refer to a symbol in another module by qualifying the name with the module name and the `::` operator, without any additional import or `use` directive. This rule makes it possible to split a file into several modules without introducing import statements between them, and it applies regardless of the order in which the modules are declared.

## Concurrency

Paxsy provides two models of concurrency: lightweight processes, written with the `routine` keyword, and native threads, written with the `async` and `await` keywords. Both models communicate through typed channels, written as `chan T`, and the `<-` operator sends and receives values on a channel. The two models share a common set of primitives for channel creation, closing, and multiplexing, and they differ in the scheduling and resource model that underlies them.

Lightweight processes are scheduled by the Paxsy runtime over a pool of native threads. Many routines may run concurrently on a small number of operating system threads, and the runtime switches between routines at well-defined yield points, such as when a routine performs a channel operation or explicitly yields. Because routines are scheduled cooperatively, a routine that never yields can starve other routines on the same thread. Routine capture variables by value only, never by reference; this rule prevents data races and makes each routine's state independent of the state of its caller.

Native threads are scheduled by the operating system and run in parallel on multiple CPU cores. An `async` block creates a new native thread, runs the block on that thread, and produces a channel through which the thread's result can be received. The `await` keyword waits for the thread to complete and retrieves its result. Native threads capture variables by value as well, which ensures that the state of the new thread is independent of the caller's stack.

The choice between routines and native threads depends on the workload. A routine is appropriate for I/O-bound tasks, where the cost of creating a native thread would be disproportionate to the work performed. A native thread is appropriate for CPU-bound tasks that benefit from parallel execution across multiple cores.

A **channel** is a typed queue through which values are passed between concurrent entities. A channel is declared with the `chan` keyword:

```
let ch: chan Int32 = chan;
```

The declaration `chan` with no argument creates an unbuffered channel, which means that a send operation blocks until a receive operation is ready to accept the value, and vice versa. A channel may be buffered by providing a capacity:

```
let ch: chan Int32 = chan 10;
```

A buffered channel accepts up to ten values before a send operation blocks. The `<-` operator is used to send and receive on a channel:

```
ch <- 42;     // send the value 42
let x = <- ch;   // receive a value
```

When `<-` appears between a channel and a value, it is a send operation. When it appears before a channel, it is a receive operation. Sends block until the value can be accepted; receives block until a value is available.

A channel may be closed with the `close` statement:

```
close ch;
```

After a channel is closed, sending to it results in undefined behaviour, and receiving from it yields the zero value of the channel's element type. A receive from a closed channel that still has buffered values yields those values before producing zero values. The compiler may detect some cases of sending to a closed channel at compile time and report them as errors, but the language does not guarantee any particular behaviour at runtime, and the programmer is responsible for preventing sends to closed channels.

The `select` statement waits for one of several channel operations to become ready and executes the corresponding arm:

```
select {
 on let x = <- ch1 {
    // handle x from ch1
 }
 on ch2 <- 42 {
    // sent 42 to ch2
 }
}
```

Each arm begins with the `on` keyword and a channel operation. The first arm receives from `ch1` and binds the received value to the variable `x`. The second arm sends the value `42` to `ch2`. The else branch is optional. When it is present, it is executed if no arm is ready immediately. When it is absent, the select statement blocks until one of the arms becomes ready. If several arms are ready, the select statement chooses one fairly. The scope of a variable bound in an arm is limited to the body of that arm. The compiler distinguishes `case` and `on` by the enclosing construct: `case` appears inside a `match`, and `on` appears inside a `select`.

A `routine` is a lightweight process:

```
routine {
 // ...
}
```

The block is the body of the routine. It may send and receive on channels, perform computations, and call functions. It captures its surrounding variables by value. A routine does not return a value; there is no `await` for a routine.

An `async` block creates a native thread and produces a channel through which its result is received:

```
let result_ch: chan Int32 = async {
 return 42;
};
```

The type of the channel produced by `async` is determined by the return type of the block's body. If the body contains a `return` statement of a value of type `T`, the channel is `chan T`. If the body contains `return error e;`, the channel is `chan T!` where `T` is the type of the non-error return. If the block returns no value, the type is `chan Void`. An `async` may also be applied to a named function:

```
let result_ch: chan Int32 = async compute(10, 20);
```

The `await` keyword waits for the channel produced by an `async` to yield its result and returns that result:

```
let result: Int32 = await result_ch;
```

Inside a routine, `await` cooperatively yields the current thread to other routines. Inside a native thread, `await` blocks the thread until the result is available. An `async` block or function whose body returns a value of type `T` produces a value of type `chan T`. If the body's return type is an error-carrying type `T!`, the produced channel has type `chan T!`; awaiting such a channel yields a value of type `T!`, which the caller handles with `try` or `catch` in the usual way. An `async` block or function whose body returns a non-`!` type produces a channel of type `chan T`; awaiting it yields a value of type `T` directly. There is no mechanism for an asynchronous task to abort the program. `await` on a channel returned directly by `chan` (not by `async`) is not permitted; the `await` operator is reserved for channels produced by `async`.

The memory model of Paxsy's concurrency is a shared address space with no data races by construction. Two routines or threads cannot access the same memory simultaneously because they do not share references: all captures are by value, and all communication is through channels. This rule eliminates data races without requiring locks, and it makes the behaviour of concurrent programs predictable. The cost is that large data structures must be copied when they cross a channel or a capture boundary; the programmer must weigh this cost against the safety benefits.

The following example demonstrates a producer and a consumer communicating through a channel:

```
func main(): Void {
 let jobs: chan Int32 = chan;
 let results: chan Int32 = chan;

 routine {
    for 1..100 |i| {
      jobs <- i;
    }
    close jobs;
 }

 routine {
    for 0.. |i| {
      let job = <- jobs;
      if job == 0 { break; }
      results <- job * job;
    }
    close results;
 }

 for 0.. |i| {
    let r = <- results;
    if r == 0 { break; }
    pass;
 }
}
```

The first routine produces the numbers 1 through 100 and sends each one on the `jobs` channel, then closes the channel. The second routine receives jobs, squares each one, and sends the result on the `results` channel. The main function receives results and discards them. The `for 0..` loops are open-ended and terminate when the `break` statement is reached.

A more advanced example uses `select` to multiplex several channels:

```
func main(): Void {
 let a: chan Int32 = chan;
 let b: chan Int32 = chan;
 let done: chan Void = chan;

 routine {
    for 1..10 |i| { a <- i; }
    close a;
 }

 routine {
    for 1..10 |i| { b <- i; }
    close b;
 }

 routine {
    for 1..20 |i| {
      select {
        on let x = <- a {
          pass;
        }
        on let y = <- b {
          pass;
        }
      }
    }
    done <- {};
 }

 await done;
}
```

The main function creates two channels and two routines that each send a sequence of integers. A third routine selects from both channels, receiving values as they become available, and signals completion by sending on the `done` channel. The main function awaits the `done` channel.

The concurrency primitives of Paxsy are designed to make correct concurrent programs the path of least resistance. By forbidding shared mutable state and requiring explicit communication through channels, the language eliminates whole classes of bugs at the cost of occasional copying. The result is a concurrency model that is safe, predictable, and compatible with the language's overall emphasis on determinism.

## Inline Assembly and volatile

Paxsy provides the `asm` statement for embedding raw assembly language in a program. The syntax is `asm "..."`, where the string literal contains the assembly instructions:

```
asm "mov eax, 42";
asm "add eax, 10";
```

The string is passed verbatim to the assembler and is not validated by the compiler. The programmer is responsible for ensuring that the instructions are syntactically valid and semantically correct for the target architecture. An `asm` statement may appear anywhere a normal statement is allowed.

The `asm` statement is emitted at the point where it appears in the source code. The compiler does not reorder it with respect to other `asm` statements, but it may reorder it with respect to ordinary code if the `volatile` keyword is not present. When correctness depends on the order of the assembly instructions relative to other operations, the `volatile` keyword must be applied:

```
volatile asm "cli";
```

A `volatile asm` statement is emitted exactly where it appears in the source, and the compiler is forbidden to reorder it, remove it, or combine it with other instructions. This is necessary for instructions that have side effects the compiler cannot observe, such as disabling interrupts, writing to memory-mapped hardware, or reading from a device register.

Inline assembly is appropriate when no Paxsy abstraction exists for the operation that must be performed, and when the operation cannot be expressed in Paxsy at all. Examples include privileged instructions, architecture-specific optimisations, and access to hardware that is not modelled by the language. Inline assembly should be used sparingly; it is not portable, it is not checked by the compiler, and it is difficult to debug. When inline assembly is used, it should be accompanied by a comment that explains what the instruction does and why it is necessary.

## Complete Example Program

The following program demonstrates many of the language's features in a single, cohesive whole. It is a small job-processing system: a producer routine generates jobs and sends them on a channel; a pool of worker routines receives jobs, processes them, and sends results on a second channel; a coordinator routine collects results and reports statistics. The program uses structs, enums, methods, `static` functions, `defer`, error handling, `routine`, `async`, `chan`, `select`, and `match`.

```
/*
 * A small job-processing system.
 * Demonstrates concurrency, error handling, and compile-time evaluation.
 */

static MAX_JOBS = 100;

static Job = Struct {
 id: Int32,
 payload: Int32,
};

static Result = Enum {
 Ok,
 Failed,
};

static JobOutcome = Struct {
 job_id: Int32,
 kind: Result,
 value: Int32,
};

func Job.describe(this: Job): Char[] {
 return "job {this.id} with payload {this.payload}";
}

func is_even(x: Int): Bool {
 return x % 2 == 0;
}

func process(job: Job): JobOutcome! {
 if job.payload < 0 {
    return error 1;
 }
 let value = job.payload * job.payload;
 return JobOutcome {.job_id = job.id, .kind = Result.Ok, .value = value};
}

func main(): Void! {
 let jobs: chan Job = chan 16;
 let outcomes: chan JobOutcome = chan 16;
 let done: chan Void = chan;

 routine {
    for 1..MAX_JOBS |i| {
      let job = Job {.id = i, .payload = i};
      jobs <- job;
    }
    close jobs;
 }

 // The loop variable `w` is used only to control the number of iterations.
 for 1..4 |w| {
    routine {
      for 0.. |i| {
        let job = <- jobs;
        if job.id == 0 { break; }
        let outcome = process(job) catch |e| {
          return JobOutcome {.job_id = job.id, .kind = Result.Failed, .value = e};
        };
        outcomes <- outcome;
      }
    }
 }

 let collected: Int32 = 0;
 let failed: Int32 = 0;
 let running: Bool = true;
 while running {
    select {
      on let o = <- outcomes {
        match o.kind {
          case Result.Ok {
            collected += 1;
          }
          case Result.Failed {
            failed += 1;
          }
        }
        if collected + failed >= MAX_JOBS {
          running = false;
        }
      }
    }
 }

 done <- {};
 await done;
}
```

The program is structured around two channels. The `jobs` channel carries `Job` values from a producer routine to a set of worker routines. The `outcomes` channel carries `JobOutcome` values from the workers back to the coordinator, which is the main function itself. A third channel, `done`, is used only to demonstrate the `await` operation on an `async` channel; in this program it is created and immediately awaited.

The `Job` struct is defined with two fields, `id` and `payload`, both of type `Int32`. A method `Job.describe` is declared outside the struct; it takes a `Job` as its first parameter (named `this`) and returns a `Char[]` describing the job. The `is_even` helper is an ordinary runtime function. The `process` function takes a `Job` and returns a `JobOutcome!`, signalling an error if the payload is negative. The error path is exercised by the `catch` block in the worker routines.

The producer routine sends `MAX_JOBS` jobs on the `jobs` channel and then closes it. Each worker routine receives jobs in a loop, processes them, and sends outcomes on the `outcomes` channel. The `catch` block in the worker handles the error case by constructing a `JobOutcome` with `kind` set to `Result.Failed` and `value` set to the error code. The coordinator loop uses `select` to receive outcomes as they arrive, updates two counters, and terminates when all jobs have been accounted for. The `match` statement on `o.kind` dispatches on the outcome type, with a mandatory `else` branch for completeness.

The program illustrates the interaction of Paxsy's concurrency model with its error handling model: errors do not propagate across channels automatically but are represented as ordinary values of the channel's element type. The `catch` block converts a `Result!` into a `JobOutcome` that can be sent on the channel, and the coordinator handles the outcome as a normal value. This design keeps error handling explicit and predictable in concurrent code, at the cost of requiring the programmer to decide how errors should be represented at each boundary.

## Language Design Summary

Paxsy combines static typing, explicit memory management, compile-time type construction, and channel-based concurrency. Variables are mutable by default; `const` expresses an immutable binding when a value must not be reassigned. Struct layout is explicit through ordinary, `extern`, and `packed` forms. Enums have a defined integer representation, selected explicitly when ABI or storage requirements demand it.

The language's error model uses error-bearing result types rather than exceptions. Memory allocation and release are explicit, and resource lifetimes are expressed with `defer` and `errdefer`. The concurrency model separates cooperative routines from native threads and uses typed channels for message passing. These rules define the programming model independently of any particular standard-library module or target platform.
