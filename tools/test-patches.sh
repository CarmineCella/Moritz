#!/bin/sh
# test-patches.sh — renders every example, help and library patch offline and
# fails if any of them prints a warning or an error, or crashes.
#
#   tools/test-patches.sh [path/to/moritz] [folder with examples, help, library]
#
# Used by CI on macOS, Linux and Windows; run it locally after changes.
MORITZ=${1:-./moritz}
ROOT=${2:-.}
OUT=$(mktemp -d 2>/dev/null || mktemp -d -t moritz)
fails=0
count=0
for f in "$ROOT"/examples/*.moritz "$ROOT"/examples/*/*.moritz "$ROOT"/help/*.moritz "$ROOT"/library/*.moritz "$ROOT"/tutorials/*.moritz; do
    [ -f "$f" ] || continue
    count=$((count + 1))
    log=$("$MORITZ" --render "$f" 1 "$OUT/out.wav" 2>&1)
    status=$?
    bad=$(printf '%s\n' "$log" | grep -iE "warn|error|unknown|cannot|can't|fail")
    if [ $status -ne 0 ] || [ -n "$bad" ]; then
        echo "FAIL $f (exit $status)"
        printf '%s\n' "$bad" | head -5
        fails=$((fails + 1))
    fi
done
rm -rf "$OUT"
echo "$count patches rendered, $fails failed"
# every object has an entry in help/reference
missing=$("$MORITZ" --reference-missing 2>/dev/null | tr -d '\r')
if [ -n "$missing" ]; then
    echo "FAIL objects without a reference entry (help/reference/*.txt):"
    printf '%s\n' "$missing" | head -20
    fails=$((fails + 1))
fi
# what a code box can call: exactly the list in tests/code-names.txt (a new name, or a refused one that
# came back, shows here; after an intended change: moritz --code-names > tests/code-names.txt)
HERE=$(cd "$(dirname "$0")/.." && pwd)
if ! "$MORITZ" --code-names 2>/dev/null | tr -d '\r' | diff -q "$HERE/tests/code-names.txt" - > /dev/null; then
    echo "FAIL the names a code box can call differ from tests/code-names.txt:"
    "$MORITZ" --code-names 2>/dev/null | tr -d '\r' | diff "$HERE/tests/code-names.txt" - | head -10
    fails=$((fails + 1))
fi
[ $count -gt 0 ] && [ $fails -eq 0 ]
