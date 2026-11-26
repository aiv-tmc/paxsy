#!/bin/sh
# Runs every test module in order. Each module is its own unittest
# program, so a failure aborts the run before the next module starts.
set -e
for t in test_tool test_quality test_static test_type_safety test_modules test_parser_precedence test_language test_determinism test_cli test_perf test_artifacts test_codegen test_pkg test_opt test_link test_std; do
    python3 "$t.py"
done
