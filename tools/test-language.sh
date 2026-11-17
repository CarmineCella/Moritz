#!/bin/sh
# test-language.sh — runs the tests of the Moritz language (tests/language) with moritz --run:
# every test_*.moritz must pass, and every reference_*.moritz must print what tests/language/golden
# holds (lines with timings, random numbers or file sizes are left out, paths reduced to file names).
#
#   tools/test-language.sh [path/to/moritz]
#   tools/test-language.sh [path/to/moritz] --update    rewrites the golden files after an intended change
#
# Used by CI on macOS, Linux and Windows.
MORITZ=$(cd "$(dirname "${1:-./moritz}")" && pwd)/$(basename "${1:-./moritz}")
UPDATE=$2
cd "$(dirname "$0")/../tests/language" || exit 1
fails=0
filter() {
    grep -vE "took|rand|bytes" | sed -E 's#[^ ]*/([^ /]+\.moritz)#\1#g; s#(^| )(std|system|scientific)\.moritz:[0-9]+#\1\2.moritz#g' | tr -d '\r' |
        awk 'NF { for (; blank > 0; blank--) print ""; print; seen = 1; next } seen { blank++ }'   # no blank lines at either end
}
for t in test_*.moritz; do
    out=$("$MORITZ" --run "$t" 2>&1)
    if [ $? -ne 0 ]; then echo "FAIL $t"; printf '%s\n' "$out" | tail -5; fails=$((fails + 1)); fi
done
for r in reference*.moritz; do
    name=${r%.moritz}
    "$MORITZ" --run "$r" > /tmp/moritz_golden_out.txt 2>&1
    status=$?
    filter < /tmp/moritz_golden_out.txt > /tmp/moritz_golden_actual.txt
    if [ "$UPDATE" = "--update" ]; then cp /tmp/moritz_golden_actual.txt "golden/$name.out"; continue; fi
    if [ $status -ne 0 ] || ! diff -q "golden/$name.out" /tmp/moritz_golden_actual.txt > /dev/null; then
        echo "FAIL $r (exit $status)"; diff "golden/$name.out" /tmp/moritz_golden_actual.txt | head -10
        fails=$((fails + 1))
    fi
done
# every patch reads the same through the language as through Moritz's loader
if ! "$MORITZ" --language-check ../../examples/*/*.moritz ../../help/*.moritz ../../library/*.moritz ../../tutorials/*.moritz; then
    echo "FAIL patches read differently by the language"; fails=$((fails + 1))
fi
echo "language: $(ls test_*.moritz reference*.moritz | wc -l | tr -d ' ') files, $fails failed"
[ $fails -eq 0 ]
