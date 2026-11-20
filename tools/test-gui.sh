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
XDG_CONFIG_HOME="$T/home/.config"      # where FLTK keeps preferences on Linux (GitHub's machines set it)
APPDATA="$T/home/AppData/Roaming"      # ... and on Windows
LOCALAPPDATA="$T/home/AppData/Local"
export HOME USERPROFILE XDG_CONFIG_HOME APPDATA LOCALAPPDATA
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

# 4. a save the disk refuses (a file size limit) leaves the file as it was
cp "$HERE/examples/03-generative-music/techno.moritz" "$T/big.moritz"
cp "$T/big.moritz" "$T/big-original.moritz"
( trap '' XFSZ; ulimit -f 8; "$MORITZ" "$T/big.moritz" --window-png none act=save > "$out" 2>&1 )
status=0
if cmp -s "$T/big.moritz" "$T/big-original.moritz" && grep -q "alert: Can't save" "$out" &&
   [ -z "$(ls -A "$T" | grep '\.tmp$')" ]; then ok; else fail "a failed save keeps the file"; fi

# 5. undo puts back what changed and keeps the rest: the buffer~'s sound, the edited script's editor
run "$G/undo.moritz" --window-png none open=c1 act=wait:0.5 act=buffer:snd "act=codeedit:;edited" act=new:button \
    act=undo act=buffer:snd act=state
frames=$(grep "state: buffer snd frames" "$out" | head -1)
if [ $status -eq 0 ] && [ "$(grep -c "$frames" "$out")" = 2 ] && ! grep -q "frames 0$" "$out" &&
   grep -q "state: code bound code c1 edited yes" "$out"; then ok; else fail "undo keeps buffer~ and the code editor"; fi

# 6. a script put back into its box is an edit: undo takes it back, and the editor follows the box
run "$G/undo.moritz" --window-png none open=c1 "act=codeedit:;edited" act=codesave act=undo act=state
expect "undo of a script" "state: code text (out 1 42)"
if ! grep -q "state: code bound code c1 edited no" "$out"; then fail "the editor is bound to the box again"; fi

# 7. closing asks about an abstraction edited in its window, first, by its name
mkdir -p "$T/abs"
printf '(patch\n  (obj i inlet (pos 20 20))\n  (obj o outlet (pos 20 80))\n  (connect i 0 o 0))\n' > "$T/abs/twice.moritz"
printf '(patch\n  (obj a1 twice (pos 20 20)))\n' > "$T/abs/main.moritz"
run "$T/abs/main.moritz" --window-png none act=opensub:a1 act=new:button answer=0 act=confirmclose act=state
expect "closing asks about the abstraction" "ask: Save the changes to the abstraction \"twice.moritz\"?"
expect "Cancel keeps the document" "state: documents 1"

# 8. a link banged by the patch opens nothing by itself
run --render "$G/link.moritz" 0.1
expect "a link banged at load" "not opened"

# 9. a Max patch can't make the import write outside its folder
mkdir -p "$T/max/sub"
printf '{"patcher":{"boxes":[{"box":{"id":"obj-1","maxclass":"newobj","text":"inlet","patching_rect":[10,10,30,20]}}],"lines":[]}}\n' > "$T/max/x.maxpat"
printf '{"patcher":{"boxes":[{"box":{"id":"obj-1","maxclass":"newobj","text":"poly~ ../x 2","patching_rect":[10,10,80,20]}}],"lines":[]}}\n' > "$T/max/sub/main.maxpat"
run --import "$T/max/sub/main.maxpat" "$T/max/sub/main.moritz"
if [ $status -eq 0 ] && [ ! -e "$T/max/x.moritz" ] && grep -q "outside the folder" "$out"; then ok; else fail "Max import stays in its folder"; fi

# 10. opening the script of a code box whose file is missing does not make the file
mkdir -p "$T/code"
cp "$G/missing.moritz" "$T/code/"
run "$T/code/missing.moritz" --window-png none open=c1 act=state
if [ $status -eq 0 ] && [ ! -e "$T/code/missing-script.mu" ]; then ok; else fail "a missing script is not made by opening it"; fi

# 11. Recent files hold absolute paths, whatever the command line said
( cd "$G" && "$MORITZ" numbers.moritz --window-png none > "$out" 2>&1 )
if grep -rq "numbers.moritz" "$HOME" 2>/dev/null && ! grep -rq "^0:numbers.moritz" "$HOME" 2>/dev/null; then ok
else fail "recent files: absolute paths"; fi

# 12. with a help window in front, Undo and Edit Mode act on nothing (not on a document behind it)
run "$G/numbers.moritz" --window-png none act=help:metro act=menu:edit.undo act=menu:patch.edit act=menu:edit.paste
expect "Undo with a help window in front" "state: menu edit.undo disabled"
expect "Edit Mode with a help window in front" "state: menu patch.edit disabled"
run "$G/numbers.moritz" --window-png none act=menu:patch.edit act=state
expect "Edit Mode on a document" "state: front"

# 13. a code window comes to the front after a help window: Save (Cmd+S) is active again, Undo acts on its text
# (the waits let the windows come to the front first: slower under a sanitizer or on X11)
run "$G/undo.moritz" --window-png none open=c1 act=help:metro act=wait:0.5 act=menuitem:file.save act=menuitem:edit.undo \
    act=raise:code act=wait:0.5 act=menuitem:file.save act=menuitem:edit.undo
expect "Save with a help window in front" "state: menu item file.save inactive"
expect "Undo with a help window in front" "state: menu item edit.undo inactive"
if [ "$(grep -c "state: menu item file.save active" "$out")" = 1 ] &&
   [ "$(grep -c "state: menu item edit.undo active" "$out")" = 1 ]; then ok; else fail "Save in a code window after a help window"; fi

# 14. the code view, typed into, replaces a subpatcher whose window is open and animated: the window's timers
#     must not walk the freed patch before FLTK deletes the window (the patch is long, so applying it takes
#     longer than the timer's period)
flashpatch() {   # flashpatch <a box more inside the subpatcher>
    printf '(patch\n  (obj sub p inner (pos 20 20)\n    (patch\n      (obj osc cycle~ 440 (pos 20 20))\n'
    printf '      (obj m meter~ (pos 20 60))\n%s      (connect osc 0 m 0)))\n' "$1"
    k=0
    while [ $k -lt 1500 ]; do printf '  (obj a%d + %d (pos %d %d))\n' $k $k $((20 + k % 40 * 60)) $((100 + k / 40 * 30)); k=$((k + 1)); done
    printf ')\n'
}
flashpatch "" > "$T/flash.moritz"
flashpatch '      (obj c comment "changed" (pos 120 20))
' > "$T/flash-edited.txt"
run "$T/flash.moritz" --window-png none act=opensub:sub act=wait:0.3 "act=type:$T/flash-edited.txt" act=wait:1.5 act=state
expect "a subpatcher's window closed by the code view" "state: boxes 1501"

# 15. boxes dragged fast, out past the patch's top-left corner and back, end where the mouse took them (they
#     drifted: each event moved them by its own step, and a box stopped at the edge lost the steps beyond it),
#     and keep their places relative to each other; at 100% and at 150%
run "$G/drag.moritz" --window-png none act=menu:patch.edit act=menu:edit.selectall \
    "act=drag:206,160/2,2/-300,-200/100,80/500,400/-50,300/306,260" act=pos:b1 act=pos:b2 act=zoom:1.5 \
    "act=drag:306,260/0,0/-400,-10/52,30/406,360" act=pos:b1 act=pos:b2
expect "a fast drag at 100%" "state: pos b1 300 250"
expect "the other box with it" "state: pos b2 300 400"
expect "a fast drag at 150%" "state: pos b1 400 350"
expect "the other box with it at 150%" "state: pos b2 400 500"

# 16. a cable's end dragged to another box moves the cable there (its inlet end, then its outlet end), one undo
#     step each; dropped on nothing, it stays as it was and nothing is recorded
run "$G/cables.moritz" --window-png none act=menu:patch.edit "act=drag:108,238/200,240/306,255" act=cables \
    act=undo act=cables "act=drag:108,134/250,200/306,260" act=cables act=undo "act=drag:108,238/500,500" act=cables act=state
seq=$(grep "state: cable " "$out" | sed 's/state: cable //' | tr '\n' '|')
if [ $status -eq 0 ] && [ "$seq" = "a 0 c 0|a 0 b 0|c 0 b 0|a 0 b 0|" ] && grep -q "state: undo no redo yes" "$out"; then ok
else fail "a cable moved by its end (got $seq)"; fi

rm -rf "$T"
echo "$count interface cases, $fails failed"
[ $fails -eq 0 ]
