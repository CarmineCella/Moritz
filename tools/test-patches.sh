#!/bin/sh
# test-patches.sh — renders every example, help and library patch offline and
# fails if any of them prints a warning or an error, or crashes.
#
#   tools/test-patches.sh [path/to/moritz] [folder with examples, help, library]
#
# Without a path, the program of the CMake build in build/ (ctest runs it so).
# Used by CI on macOS, Linux and Windows; run it locally after changes.
MORITZ=$1
if [ -z "$MORITZ" ]; then
    for m in build/Moritz.app/Contents/MacOS/moritz build/Moritz/moritz build/Moritz/moritz.exe; do
        [ -x "$m" ] && MORITZ=$m && break
    done
fi
ROOT=${2:-.}
OUT=$(mktemp -d 2>/dev/null || mktemp -d -t moritz)
# every run of the program has a time limit where `timeout` exists (Linux, MSYS2; brew's coreutils on
# macOS), so a hang fails with the patch's name instead of stopping the run
LIMIT=
if command -v timeout > /dev/null 2>&1; then LIMIT="timeout 300"; elif command -v gtimeout > /dev/null 2>&1; then LIMIT="gtimeout 300"; fi
fails=0
count=0
# tests/cross: an object against Musil's function for the same thing (a code box compares them)
for f in "$ROOT"/examples/*.moritz "$ROOT"/examples/*/*.moritz "$ROOT"/help/*.moritz "$ROOT"/library/*.moritz "$ROOT"/tutorials/*.moritz "$(cd "$(dirname "$0")/.." && pwd)"/tests/cross/*.moritz; do
    [ -f "$f" ] || continue
    count=$((count + 1))
    dur=1
    case "$f" in */tests/cross/*) dur=3 ;; esac     # a cross-test records, then compares: give it time
    log=$($LIMIT "$MORITZ" --render "$f" $dur "$OUT/out.wav" 2>&1)
    status=$?
    bad=$(printf '%s\n' "$log" | grep -iE "warn|error|unknown|cannot|can't|fail")
    if [ $status -ne 0 ] || [ -n "$bad" ]; then
        echo "FAIL $f (exit $status)"
        printf '%s\n' "$bad" | head -5
        fails=$((fails + 1))
    else
        # a cross-test passes only when every comparison in it ran and said PASS (a comparison that never
        # runs would otherwise pass in silence)
        want=$(grep -o 'print \\"PASS' "$f" | wc -l | tr -d ' ')
        got=$(printf '%s\n' "$log" | grep -c '^PASS')
        if [ "$want" -gt 0 ] && [ "$got" -ne "$want" ]; then
            echo "FAIL $f: $got of $want comparisons said PASS"
            fails=$((fails + 1))
        fi
    fi
done
rm -rf "$OUT"
echo "$count patches rendered, $fails failed"
# every object has an entry in help/reference
missing=$($LIMIT "$MORITZ" --reference-missing 2>/dev/null | tr -d '\r')
if [ -n "$missing" ]; then
    echo "FAIL objects without a reference entry (help/reference/*.txt):"
    printf '%s\n' "$missing" | head -20
    fails=$((fails + 1))
fi
# what a code box can call: exactly the list in tests/code-names.txt (a new name shows here; what a box
# may do is checked inside the builtins; after an intended change: moritz --code-names > tests/code-names.txt)
HERE=$(cd "$(dirname "$0")/.." && pwd)
if ! $LIMIT "$MORITZ" --code-names 2>/dev/null | tr -d '\r' | diff -q "$HERE/tests/code-names.txt" - > /dev/null; then
    echo "FAIL the names a code box can call differ from tests/code-names.txt:"
    "$MORITZ" --code-names 2>/dev/null | tr -d '\r' | diff "$HERE/tests/code-names.txt" - | head -10
    fails=$((fails + 1))
fi
[ $count -gt 0 ] && [ $fails -eq 0 ]
