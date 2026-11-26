---
name: paxsy-docs
description: Use when answering questions about Paxsy, generating Paxsy code, reviewing Paxsy patches, or choosing the authoritative Paxsy documentation source.
---

# Paxsy documentation skill

## When to use

Use this skill for Paxsy language design, compiler behaviour, toolchain commands, standard-library APIs, style questions, or LLM/editor/agent integrations.

## Source order

When sources disagree, use this order:

1. Compiler implementation and regression tests for executable behaviour.
2. `../language/language-specification.md` for syntax and semantics.
3. `../language/toolchain-user-manual.md` for command-line and project behaviour.
4. `../language/standard-library-reference.md` for public APIs.
5. `integration-guide.md` for LLM workflow, prompt structure, and verified/unverified claim labels.
6. `../language/code-style-guide.md` for source and contribution conventions.

## Minimum context pack

For a small source-generation task, load the relevant section of `../language/language-specification.md`, the exact `../language/standard-library-reference.md` declaration needed, and the matching `../language/toolchain-user-manual.md` command if the task involves building or testing.

For architecture or bug-diagnosis tasks, start with compiler source and regression tests, then load the narrowest user-facing reference.

## Answer rules

- State the governing document first and quote the smallest relevant rule.
- Separate executable behaviour from specified source semantics.
- Mark generated examples as unverified unless the exact compiler command was run and succeeded.
- Preserve the documentation spelling: behaviour, optimisation, synchronisation, normalise, serialise, colour, artefact.
- Keep answers narrow: prefer the documented form over an inferred shortcut.

## Output style

When the user asks for a patch, include: affected file path, rationale, risk, and validation command. When the user asks for an explanation, state the governing document first and quote the smallest relevant rule.
