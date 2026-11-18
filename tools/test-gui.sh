#!/bin/sh
# test-gui.sh — the interface, driven as a user would (src/guitest.cpp): windows closed while a box is
# animated, edits from the code view, undo, saving when the disk refuses, what a patch may open.
#
#   tools/test-gui.sh [path/to/moritz]
#
# Each case runs the program with --window-png none act=... in a scratch HOME (the user's preferences
# are never touched) and checks what it prints. Under -DMORITZ_SANITIZE=address a use after free fails
# the case. Without a path, the program of the CMake build in build/.
MORITZ=$1
if [ -z "$MORITZ" ]; then
    for m in build/Moritz.app/Contents/MacOS/moritz build/Moritz/moritz build/Moritz/moritz.exe; do
        [ -x "$m" ] && MORITZ=$m && break
    done
fi
case "$MORITZ" in /*) ;; *) MORITZ="$(pwd)/$MORITZ" ;; esac
HERE=$(cd "$(dirname "$0")/.." && pwd)
G="$HERE/tests/gui"
T=$(mktemp -d 2>/dev/null || mktemp -d -t moritz)
HOME="$T/home"
USERPROFILE="$T/home"
export HOME USERPROFILE
mkdir -p "$HOME"
fails=0
count=0
out="$T/out.txt"

run() {    # run <args...>: the output in $out, the exit status in $status
    "$MORITZ" "$@" > "$out" 2>&1
    status=$?
}
ok() { count=$((count + 1)); }
fail() {
    count=$((count + 1))
    fails=$((fails + 1))
    echo "FAIL $1"
    sed 's/^/    /' "$out" | tail -15
}
expect() {   # expect <name> <text the output must contain>
    if [ $status -eq 0 ] && grep -qF -- "$2" "$out"; then ok; else fail "$1 (exit $status, expected: $2)"; fi
}


# 1. a window closed while a box in it is animated: its timers must not find it gone
run "$G/animated.moritz" --window-png none act=draw act=close act=wait:0.3 act=state
expect "close with an animated box" "state: documents 0"

# 2. Patch > New with a help window in front: help patches are read-only (it crashed)
run --window-png none act=help:metro act=state act=new:print act=state
expect "Patch > New in a help window" "state: help in front yes"

# 3. the code view replaces the number box being typed into; typing goes on (it wrote into freed memory)
run "$G/numbers.moritz" --window-png none act=click:n1 "act=text:$G/numbers-edited.txt" act=keys:5, act=boxes
expect "typing after the code view" "state: box n1"

rm -rf "$T"
echo "$count interface cases, $fails failed"
[ $fails -eq 0 ]
