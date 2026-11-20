#!/bin/sh
# test-musil.sh — Musil's tests, as Musil's own build runs them (musil/CMakeLists.txt):
#
#   tools/test-musil.sh musil  path/to/musil     Musil on its own, the reference: every test_*.mu, the golden
#                                                references (musil/examples/reference*.mu against
#                                                musil/tests/golden), the limits, every example
#   tools/test-musil.sh moritz path/to/moritz    Musil in Moritz (moritz --run): the same tests and references,
#                                                without what Moritz leaves out (the live, plot and controls
#                                                libraries; its synths are silent)
#   add --update (musil only) to rewrite the golden files after an intended change
#
# Lines with timings, random numbers or file sizes are left out of the comparison, and paths are reduced
# to file names. Used by CI on macOS, Linux and Windows.
MODE=$1
BIN=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
UPDATE=$3
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case "$MODE" in musil|moritz) ;; *) echo "usage: tools/test-musil.sh musil|moritz path/to/program [--update]"; exit 2 ;; esac
case "$(uname -s)" in *_NT*) WINDOWS=1 ;; *) WINDOWS= ;; esac
export MUSIL_NOSHOW=1 MUSIL_NULL_AUDIO=1
[ "$MODE" = musil ] && export MUSIL_PATH="$ROOT/musil/src"
TMP=$(mktemp -d 2>/dev/null || mktemp -d -t musil)
# a home of their own: the user's datasets (~/.musil/datasets.mu, ~/Documents/Moritz/datasets) and preferences are
# not seen, so the examples run on the bundled MicroSOL
HOME="$TMP/home"
USERPROFILE="$TMP/home"
XDG_CONFIG_HOME="$TMP/home/.config"
APPDATA="$TMP/home/AppData/Roaming"
LOCALAPPDATA="$TMP/home/AppData/Local"
export HOME USERPROFILE XDG_CONFIG_HOME APPDATA LOCALAPPDATA
mkdir -p "$HOME"
fails=0
count=0

run() {     # run a .mu file with the program under test
    if [ "$MODE" = musil ]; then "$BIN" "$@"; else "$BIN" --run "$@"; fi
}
fail() { echo "FAIL $1"; fails=$((fails + 1)); }
filter() {
    grep -vE "took|rand|bytes" | sed -E 's#[^ ]*/([^ /]+\.mu)#\1#g; s#(^| )(std|system|scientific)\.mu:[0-9]+#\1\2.mu#g' | tr -d '\r' |
        awk 'NF { for (; blank > 0; blank--) print ""; print; seen = 1; next } seen { blank++ }'   # no blank lines at either end
}
# what Moritz's Musil prints differently: its synths are silent, so a score with a synth never needs scaling
hosted() {
    if [ "$MODE" = moritz ]; then grep -v "render: the mix peaked"; else cat; fi
}

# --- the tests of each library
cd "$ROOT/musil/tests" || exit 1
TESTS="test_core test_std test_system test_scientific test_signals test_live test_music test_datasets test_atoms test_plot test_memory test_stress test_smoke test_load test_robust test_semantics"
[ "$MODE" = moritz ] && TESTS=$(echo "$TESTS" | sed 's/test_live//; s/test_plot//')
# MUSIL_SKIP_TESTS="test_a test_b": left out (CI's thread-sanitizer job: test_robust nests deeper than TSan follows)
kept=
for t in $TESTS; do case " $MUSIL_SKIP_TESTS " in *" $t "*) ;; *) kept="$kept $t" ;; esac; done
TESTS=$kept
# test_live checks the audio thread in real time (voices start and stop within tens of ms): on a busy
# machine it may miss, so it gets three tries, and says when it needed more than one
for t in $TESTS; do
    count=$((count + 1))
    tries=1
    [ "$t" = test_live ] && tries=3
    k=1
    while ! run "$t.mu" > "$TMP/out.txt" 2>&1; do
        if [ $k -ge $tries ]; then fail "$t"; tail -5 "$TMP/out.txt"; break; fi
        echo "retry $t (try $k: $(grep -m1 'assertion failed' "$TMP/out.txt" | sed 's/.*assertion failed: //'))"
        k=$((k + 1))
    done
done

# --- the golden references
REFS="reference reference_std reference_scientific reference_signals reference_live reference_music reference_plot"
[ "$MODE" = moritz ] && REFS="reference reference_std reference_scientific reference_signals reference_music"
[ -n "$WINDOWS" ] && REFS=$(echo "$REFS" | sed 's/reference_live//')     # OSC input (live.h) is not available on Windows
for r in $REFS; do
    count=$((count + 1))
    run "../examples/$r.mu" > "$TMP/out.txt" 2>&1
    status=$?
    filter < "$TMP/out.txt" | hosted > "$TMP/actual.txt"
    if [ "$UPDATE" = "--update" ] && [ "$MODE" = musil ]; then cp "$TMP/actual.txt" "golden/$r.out"; continue; fi
    hosted < "golden/$r.out" > "$TMP/golden.txt"
    if [ $status -ne 0 ] || ! diff -q "$TMP/golden.txt" "$TMP/actual.txt" > /dev/null; then
        fail "$r (exit $status)"; diff "$TMP/golden.txt" "$TMP/actual.txt" | head -10
    fi
done
count=$((count + 1))     # machine-dependent output: only run
run ../examples/reference_system.mu > "$TMP/out.txt" 2>&1 || { fail reference_system; tail -5 "$TMP/out.txt"; }

if [ "$MODE" = musil ]; then
    # --- the limits: performance, and a runaway recursion that must stop with an error
    for t in perf_loop perf_fib perf_matrix; do
        count=$((count + 1))
        run "$t.mu" > "$TMP/out.txt" 2>&1 || { fail "$t"; tail -5 "$TMP/out.txt"; }
    done
    count=$((count + 1))
    if run inf_recursion.mu > /dev/null 2>&1; then fail "inf_recursion (should stop with an error)"; fi

    # --- every example runs without error (the long ones are run by hand, not tested)
    SKIP="live_loops live_session live_house live_techno live_trance live_emptyset live_spatial flyby flyby_controls
          atmospheres atmospheres2 haas dynamic_orchidea make_audio_demos"
    [ -n "$WINDOWS" ] && SKIP="$SKIP live_controls"
    cd "$ROOT/musil/examples" || exit 1
    for ex in *.mu; do
        name=${ex%.mu}
        case "$name" in reference*) continue ;; esac
        case " $(echo $SKIP) " in *" $name "*) continue ;; esac
        count=$((count + 1))
        if ! run "$ex" > "$TMP/out.txt" 2>&1; then fail "example $name"; tail -5 "$TMP/out.txt"; fi
    done
fi

rm -rf "$TMP"
echo "musil ($MODE): $count runs, $fails failed"
[ $fails -eq 0 ]
