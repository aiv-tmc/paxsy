# Paxsy documentation

Paxsy is a statically typed, compiled systems language for software that must be fast, predictable, and explicit about memory, errors, and concurrency. This documentation set is the written contract for that language: it defines what Paxsy source code means, how the toolchain behaves, what the standard library provides, which style conventions keep programs maintainable, and how automated tools should use the reference material.

The purpose of these documents is practical. A programmer should be able to answer a language question without reading the compiler source. A maintainer should be able to distinguish user-visible behaviour from implementation detail. An editor, agent, or language model should be able to retrieve the smallest correct context before generating or reviewing Paxsy code.

> **Note:** This documentation was processed with AI assistance and subsequently reviewed by the author. If you notice an error, inaccuracy, or unclear wording, please report it via an issue or pull request.

## What the documentation defines

- **The language**: syntax, static semantics, memory model, error handling, compile-time evaluation, concurrency, inline assembly, and the rules that make Paxsy programs predictable.
- **The toolchain**: installation, project layout, configuration, build/test/bench commands, package management, formatting, diagnostics, linking, and recovery workflows.
- **The standard library**: the public `std` API, including ownership rules, error values, valid inputs, and the boundary between portable language facilities and operating-system interfaces.
- **The conventions**: naming, formatting, declaration structure, error style, comments, and examples that keep source consistent across projects.
- **The integration layer**: instructions for LLMs and automated agents so generated Paxsy code is grounded in the same sources as human-written code.

## How to use it

Start with the question, not with the file. For a language-semantics question, open the specification at the relevant section. For a command or project-configuration question, use the toolchain manual. For an API signature, ownership rule, or error code, use the standard library reference. For contribution style, use the code style guide.

For compiler or architecture work, treat compiler source and regression tests as executable evidence. Use this documentation to state the contract; use tests to prove that the implementation satisfies it.

For LLM-assisted work, use `llm/SKILL.md` and `llm/integration-guide.md`. Retrieve only the specification section, API declaration, and toolchain command needed for the task, then verify generated code with the documented commands.

## Reading rules

1. **Prefer the smallest governing rule.** Do not infer behaviour from examples when a normative paragraph defines it.
2. **Keep layers separate.** Language semantics, toolchain behaviour, library APIs, and style conventions answer different kinds of questions.
3. **Verify generated claims.** Type-checking, building, execution, and regression tests are different levels of evidence.
4. **Preserve terminology.** Use the same names used by the language: `static`, `try`, `routine`, channel `select`, `std::` modules, and the documented error model.
5. **Keep changes narrow.** Documentation edits should clarify the contract without silently changing it.

## Scope

This tree is the reference layer for Paxsy. It is written for language users, maintainers, contributors, and automated tools that need the same stable source of meaning. Compiler internals, issue tracking, and implementation experiments belong with the compiler repository and its tests; this documentation states the rules those systems must satisfy.
