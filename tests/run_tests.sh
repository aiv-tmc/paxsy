#!/bin/sh
# Frontend test runner.
#
# Positive tests (tests/positive/*.px) must pass 'paxsy check' cleanly.
# Negative tests (tests/negative/*.px) must fail with exactly the expected
# diagnostics, declared via header comments:
#   // expect-line: N     an error must be reported at line N of main.px
#   // expect-text: txt   the error output must contain this substring
#   // expect-count: N    exactly N errors must be reported (default: 1)
#
# Each test is compiled as a fresh scratch project under tests/.tmp/.
# Exit status is 0 when every test passes.

set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PAXSY="$ROOT/paxsy"
TMP="$ROOT/tests/.tmp"

rm -rf "$TMP"
mkdir -p "$TMP"

pass=0
fail=0

report_fail() {
    fail=$((fail + 1))
    echo "FAIL $1: $2"
}

run_suite() {
    for src in "$1"/*.px; do
        [ -e "$src" ] || continue
        name=$(basename "$src" .px)
        dir="$TMP/$2_$name"
        proj=$(basename "$dir")
        mkdir -p "$dir/src"
        cp "$src" "$dir/src/main.px"
        {
            echo '[project]'
            echo "name = \"$proj\""
            echo 'version = "0.1.0"'
        } > "$dir/$proj.toml"
        "$PAXSY" check "$dir" >"$dir/out.log" 2>&1
        rc=$?
        if [ "$2" = pos ]; then
            if [ $rc -eq 0 ]; then
                pass=$((pass + 1))
            else
                report_fail "positive/$name" "expected clean check (log: tests/.tmp/pos_$name/out.log)"
            fi
        else
            exp_line=$(sed -n 's|^// expect-line: ||p' "$src" | head -1)
            exp_text=$(sed -n 's|^// expect-text: ||p' "$src" | head -1)
            exp_count=$(sed -n 's|^// expect-count: ||p' "$src" | head -1)
            exp_count=${exp_count:-1}
            out=$(cat "$dir/out.log")
            if [ $rc -eq 0 ]; then
                report_fail "negative/$name" "check succeeded, expected failure"
                continue
            fi
            nerr=$(printf '%s\n' "$out" | grep -c ':error(')
            if [ "$nerr" != "$exp_count" ]; then
                report_fail "negative/$name" "expected $exp_count error(s), got $nerr"
                continue
            fi
            if [ -n "$exp_line" ] && ! printf '%s\n' "$out" | grep -q "main.px $exp_line:"; then
                report_fail "negative/$name" "no error at line $exp_line"
                continue
            fi
            if [ -n "$exp_text" ] && ! printf '%s\n' "$out" | grep -qF "$exp_text"; then
                report_fail "negative/$name" "output lacks '$exp_text'"
                continue
            fi
            pass=$((pass + 1))
        fi
    done
}

run_suite "$ROOT/tests/positive" pos
run_suite "$ROOT/tests/negative" neg

echo "tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
