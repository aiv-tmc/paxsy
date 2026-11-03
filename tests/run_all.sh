#!/bin/sh
# Runs every test module in order. Each module is its own unittest
# program, so a failure aborts the run before the next module starts.
set -e
for t in test_tool test_quality test_codegen test_pkg test_opt; do
    python3 "$t.py"
done
