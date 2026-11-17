#!/bin/sh
# test-musil.sh — runs the tests of Musil (musil/tests) with moritz --run: every test_*.mu must pass,
# and every reference_*.mu must print what musil/tests/golden holds (lines with timings, random numbers
# or file sizes are left out, paths reduced to file names).
#
#   tools/test-musil.sh [path/to/moritz]
#   tools/test-musil.sh [path/to/moritz] --update    rewrites the golden files after an intended change
#
# Used by CI on macOS, Linux and Windows.
MORITZ=$(cd "$(dirname "${1:-./moritz}")" && pwd)/$(basename "${1:-./moritz}")
UPDATE=$2
cd "$(dirname "$0")/../musil/tests" || exit 1
export MUSIL_NOSHOW=1
fails=0
filter() {
    grep -vE "took|rand|bytes" | sed -E 's#[^ ]*/([^ /]+\.mu)#\1#g; s#(^| )(std|system|scientific)\.mu:[0-9]+#\1\2.mu#g' | tr -d '\r' |
        awk 'NF { for (; blank > 0; blank--) print ""; print; seen = 1; next } seen { blank++ }'   # no blank lines at either end
}
for t in test_*.mu; do
    out=$("$MORITZ" --run "$t" 2>&1)
    if [ $? -ne 0 ]; then echo "FAIL $t"; printf '%s\n' "$out" | tail -5; fails=$((fails + 1)); fi
done
for r in reference*.mu; do
    name=${r%.mu}
    "$MORITZ" --run "$r" > /tmp/musil_golden_out.txt 2>&1
    status=$?
    filter < /tmp/musil_golden_out.txt > /tmp/musil_golden_actual.txt
    if [ "$UPDATE" = "--update" ]; then cp /tmp/musil_golden_actual.txt "golden/$name.out"; continue; fi
    if [ $status -ne 0 ] || ! diff -q "golden/$name.out" /tmp/musil_golden_actual.txt > /dev/null; then
        echo "FAIL $r (exit $status)"; diff "golden/$name.out" /tmp/musil_golden_actual.txt | head -10
        fails=$((fails + 1))
    fi
done
echo "musil: $(ls test_*.mu reference*.mu | wc -l | tr -d ' ') files, $fails failed"
[ $fails -eq 0 ]
