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
# under -DMORITZ_SANITIZE=thread: the threads macOS starts inside the event loop are not reported (tsan-gui.supp)
TSAN_OPTIONS="suppressions=$HERE/tools/tsan-gui.supp${TSAN_OPTIONS:+ $TSAN_OPTIONS}"
export TSAN_OPTIONS
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

# 2. Patch > New with a help window in front: the box goes into the help patch (it crashed when help patches were
#    read-only)
run --window-png none act=help:metro act=state act=canvas act=new:print act=canvas
expect "Patch > New in a help window" "state: help in front yes"
expect "the box is in the help patch" "state: canvas boxes 26"

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

# 12. with a help window in front, Edit Mode, New and Undo act on the help patch, not on the document behind it
run "$G/numbers.moritz" --window-png none act=help:metro act=wait:0.3 act=menu:patch.edit act=new:button act=canvas \
    act=menu:edit.undo act=canvas act=state
expect "Edit Mode with a help window in front" "state: canvas metro help — edited — Moritz locked no"
if [ "$(grep "state: canvas boxes" "$out" | tr '\n' '|')" = "state: canvas boxes 26|state: canvas boxes 25|" ]; then ok
else fail "New and Undo with a help window in front"; fi
expect "the document behind is left alone" "state: front $G/numbers.moritz edited no"
run "$G/numbers.moritz" --window-png none act=menu:patch.edit act=state
expect "Edit Mode on a document" "state: front"

# 13. a code window comes to the front after a document window: the menus follow it (Revert, active for the edited
# document, is not for the code window; Save and Undo are, on its text) (the waits let the windows come to the
# front first: slower under a sanitizer or on X11)
run "$G/undo.moritz" --window-png none open=c1 act=new:button act=raise:doc act=wait:0.5 act=menuitem:file.revert \
    act=raise:code act=wait:0.5 act=menuitem:file.revert act=menuitem:file.save act=menuitem:edit.undo
if [ "$(grep "state: menu item" "$out" | tr '\n' '|')" = "state: menu item file.revert active|state: menu item file.revert inactive|state: menu item file.save active|state: menu item edit.undo active|" ]
then ok; else fail "the menus follow a code window that comes to the front"; fi

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
seq=$(grep "state: cable " "$out" | grep -v " s 0 d 0" | sed 's/state: cable //' | tr '\n' '|')
if [ $status -eq 0 ] && [ "$seq" = "a 0 c 0|a 0 b 0|c 0 b 0|a 0 b 0|" ] && grep -q "state: undo no redo yes" "$out"; then ok
else fail "a cable moved by its end (got $seq)"; fi

# 17. a box dragged with Shift held onto a cable lights the cable, and dropped there goes into it: a -> b becomes
#     a -> c -> b, one undo step with the move; not into a signal cable it can't take, and not without Shift
run "$G/cables.moritz" --window-png none act=menu:patch.edit "act=drag:306,255/200,200/90,175:shift:hold" act=lit \
    act=release:90,175:shift act=cables act=undo act=pos:c "act=drag:306,255/490,175:shift:hold" act=lit \
    act=release:490,175:shift act=undo "act=drag:306,255/90,175" act=cables
seq=$(grep "state: cable \|state: lit\|state: pos" "$out" | sed 's/state: //' | tr '\n' '|')
if [ $status -eq 0 ] && [ "$seq" = "lit a 0 b 0|cable a 0 c 0|cable c 0 b 0|cable s 0 d 0|pos c 300 250|lit none|cable a 0 b 0|cable s 0 d 0|" ]
then ok; else fail "a box dropped into a cable with Shift (got $seq)"; fi

# 18. a help patch is a document to play with, edit and copy from: unlocked, its boxes are copied into a new patch;
#     played with, it closes without a question; edited, Save asks where (not the shipped file, which stays as it
#     was), and closing asks first
# (the new window is brought to the front first: on X11 without a window manager, and slower under a sanitizer,
# it may come up after the paste)
run --window-png none act=help:metro act=menu:patch.edit act=menu:edit.selectall act=menu:edit.copy act=menu:file.new \
    act=wait:0.5 act=raise:last act=wait:0.5 act=menu:edit.paste act=canvas
expect "boxes copied from a help patch" "state: canvas Untitled — edited — Moritz locked no"
expect "all of them" "state: canvas boxes 25"
run --window-png none act=help:function act=click:fn act=canvas act=closefront act=state   # a point added to its function
if [ $status -eq 0 ] && grep -q "state: canvas function help — Moritz" "$out" && grep -q "state: documents 0" "$out" &&
   ! grep -q "^ask:" "$out"; then ok
else fail "a help patch played with closes without a question"; fi
run --window-png none act=help:metro act=menu:patch.edit act=new:button act=save act=canvas
expect "Save on a help patch asks where" "save as: metro-help.moritz in (the system's choice) -> cancelled"
expect "cancelled, it is still the help patch" "state: canvas help "
run --window-png none act=help:metro act=menu:patch.edit act=new:button answer=2 act=closefront act=state
expect "closing an edited help patch asks" "ask: Keep your changes to the help patch \"metro.help.moritz\" as a patch of yours?"
expect "Don't Save closes it" "state: documents 0"
run --window-png none act=help:metro act=menu:patch.edit act=new:button "saveas=$T/mine.moritz" act=save act=canvas
helpfile=$(grep "state: canvas help " "$out" | sed 's/state: canvas help //')
if [ $status -eq 0 ] && grep -q "state: canvas mine.moritz — Moritz" "$out" && grep -q "(obj met metro 250" "$T/mine.moritz" &&
   [ -z "$helpfile" ]; then ok; else fail "a help patch saved as a patch of the user's"; fi
if cmp -s "$HERE/help/metro.help.moritz" "$(dirname "$MORITZ")/../Resources/help/metro.help.moritz" ||
   cmp -s "$HERE/help/metro.help.moritz" "$(dirname "$MORITZ")/help/metro.help.moritz"; then ok
else fail "the shipped help patch is as it was"; fi

# 19. the sound editor on a buffer~: a double-click (locked) opens it; a selection dragged and cut through the
#     menu changes the buffer~ in place (one undo step, put back by Undo); the patch is not marked edited (a
#     buffer~ is patch memory); Save writes a WAV copy where asked; closing it asks nothing
run "$G/sound.moritz" --window-png none act=dblclick:snd act=sound act=sounddrag:0.1,0.3 act=menu:edit.cut \
    act=buffer:voice act=sound act=menu:edit.undo act=buffer:voice act=state "saveas=$T/voice-copy.wav" act=soundsave \
    act=closefront act=state
frames=$(grep "state: buffer voice frames" "$out" | sed 's/.*frames //' | tr '\n' ' ')
cut=${frames%% *}
if [ $status -eq 0 ] && grep -q "state: sound buffer~ voice — Moritz" "$out" && [ "$cut" -lt 27731 ] 2>/dev/null &&
   [ "$cut" -gt 18000 ] && [ "${frames#* }" = "27731 " ] && grep -q "state: front $G/sound.moritz edited no" "$out" &&
   [ -s "$T/voice-copy.wav" ] && ! grep -q "^ask:" "$out" && grep -q "state: documents 1" "$out"; then ok
else fail "the sound editor cuts a buffer~ in place, undoes, saves a copy (frames: $frames)"; fi

expect "the status line says which objects use the buffer~" "used by play~ groove~ (2 boxes in 1 patch)"
expect "and the sound's rate, channels and length" "state: sound info  44.1 kHz · 1 ch · 0:00.629"

# 19b. Shift-drag extends the selection from its far end; a click without a drag sets the playhead, nothing selected
run "$G/sound.moritz" --window-png none act=dblclick:snd act=sounddrag:0.1,0.2 act=sound act=sounddrag:0.4,0.4:shift \
    act=sound act=sounddrag:0.3,0.3 act=sound
sel=$(grep "state: sound selection" "$out" | sed 's/state: sound selection //' | tr '\n' '|')
a1=$(echo "$sel" | cut -d'|' -f1 | cut -d' ' -f1)
a2=$(echo "$sel" | cut -d'|' -f2 | cut -d' ' -f1)
e2=$(echo "$sel" | cut -d'|' -f2 | cut -d' ' -f2)
third=$(echo "$sel" | cut -d'|' -f3)
if [ $status -eq 0 ] && [ "$a1" = "$a2" ] && [ "$e2" -gt 17000 ] 2>/dev/null && [ "$e2" -lt 18000 ] &&
   echo "$third" | grep -q "^\([0-9]*\) \1 playhead \1$"; then ok
else fail "Shift-drag extends the selection, a click sets the playhead (got $sel)"; fi

# 20. keys in the sound editor: Cmd+A selects all, Backspace deletes, Cmd+Z undoes, Shift+Cmd+Z redoes
run "$G/sound.moritz" --window-png none act=dblclick:snd act=soundkey:a:cmd act=soundkey:delete act=buffer:voice \
    act=soundkey:z:cmd act=buffer:voice act=soundkey:z:cmd:shift act=buffer:voice
frames=$(grep "state: buffer voice frames" "$out" | sed 's/.*frames //' | tr '\n' ' ')
if [ $status -eq 0 ] && [ "$frames" = "0 27731 0 " ]; then ok; else fail "keys in the sound editor (frames: $frames)"; fi

# 21. a sound file: edits make it unsaved; closing asks (Cancel keeps it open, Save writes the file in place);
#     undoing back to the file leaves nothing to save
cp "$HERE/data/anechoic1.wav" "$T/s.wav"
run --window-png none "act=soundopen:$T/s.wav" act=sounddrag:0.1,0.3 act=soundop:cut act=soundop:selectall \
    act=soundgain:-6 act=sound answer=0 act=closefront act=sound answer=1 act=closefront act=state
expect "an edited sound asks before closing" "ask: Save changes to the sound \"s.wav\"?"
expect "Cancel keeps it" "state: sound s.wav — edited — Moritz"
cutframes=$(grep "state: sound frames" "$out" | head -1 | sed 's/state: sound frames \([0-9]*\).*/\1/')
run --window-png none "act=soundopen:$T/s.wav" act=sound
if [ $status -eq 0 ] && [ "$cutframes" -lt 27731 ] 2>/dev/null && grep -q "state: sound frames $cutframes channels 1" "$out"
then ok; else fail "Save wrote the file in place ($cutframes frames)"; fi
run --window-png none "act=soundopen:$T/s.wav" act=sounddrag:0.1,0.2 act=soundop:reverse act=soundop:undo \
    act=closefront act=state
if [ $status -eq 0 ] && ! grep -q "^ask:" "$out"; then ok; else fail "undone back to the file: closing asks nothing"; fi

# 22. the clipboard of samples goes from one editor to another (a file into a buffer~), and the spectrogram is
#     computed in the background
run "$G/sound.moritz" --window-png none "act=soundopen:$T/s.wav" act=soundspec act=sounddrag:0,0.1 act=soundop:copy \
    act=sound act=raise:doc act=dblclick:snd act=sounddrag:0.2,0.2 act=soundop:paste act=buffer:voice
copied=$(grep "state: sound selection 0 " "$out" | head -1 | sed 's/state: sound selection 0 \([0-9]*\).*/\1/')
if [ $status -eq 0 ] && [ "$copied" -gt 0 ] 2>/dev/null && grep -q "state: buffer voice frames $((27731 + copied))$" "$out" &&
   grep -q "state: spectrogram frames [1-9]" "$out"; then ok
else fail "paste between sound editors ($copied frames), the spectrogram"; fi

# 23. the patch holding the buffer~ closed while its editor is open: the editor keeps the samples and says so;
#     editing them goes on (it changes no buffer~ any more)
run "$G/sound.moritz" --window-png none act=dblclick:snd act=raise:doc act=close act=wait:0.3 act=sound \
    act=sounddrag:0.1,0.2 act=soundop:reverse act=soundop:undo act=sound
expect "the buffer~ gone" "state: sound buffer~ voice (not in any open patch now) — Moritz"
expect "edits go on" "state: sound frames 27731 channels 1"

# the Datasets window (Extras > Datasets...): MicroSOL listed as what comes with Moritz; a feature file registered by
# name (written in the scratch home's ~/.musil/datasets.mu, read again by the next run); a folder of sounds analysed
# into a feature file next to it, on the job's thread, and registered by name
run --window-png none act=menu:extras.datasets act=datasets
expect "the Datasets window lists MicroSOL" "state: dataset microsol bundled present"
expect "the default datasets folder" "state: datasets folder $HOME/Documents/Moritz/datasets"
run --window-png none "act=datasetsregister:fullsol:$HERE/data/microsol/microsol.spectrum.db"
run --window-png none act=datasets
expect "a file registered by name" "state: dataset fullsol registered present"
mkdir -p "$T/sounds" && cp -R "$HERE/data/microsol/microsol" "$T/sounds/Mine"
run --window-png none "act=datasetsanalyse:minesol:$T/sounds/Mine" act=datasets
expect "a folder analysed" "/sounds/minesol.spectrum.db"
expect "the analysis registered" "state: dataset minesol registered present"
run --window-png none act=datasetsforget:fullsol "act=datasetsfolder:$T/sounds" act=datasets
if [ $status -eq 0 ] && ! grep -q "dataset fullsol" "$out" && grep -q "state: datasets folder .*/sounds" "$out"; then ok
else fail "a registration forgotten, the datasets folder chosen"; fi

# 24. the Process menu: its items (To Score inactive without a score object), a dialog's questions (in batch mode,
#     printed), a filter over the selection as one undo step, pvoc's stretch to a new sound (Result: a new sound)
run "$G/sound.moritz" --window-png none "act=soundopen:$T/s.wav" act=soundmenus act=soundmark:toscore \
    act=processdialog:typematch act=processdialog:pvoc act=sounddrag:0.1,0.3 act=sound \
    act=process:filter:type=lowpass,freq=500 act=processwait act=sound act=soundop:undo act=sound \
    act=processresult:new act=soundmenus act=process:pvoc:stretch=2 act=processwait act=sound
expect "the Process menu" "state: sound process menu: pursuit: rebuild with atoms... | typematch: replace by units of... | hpss: keep... | nmf: keep components... | pvoc: stretch or transpose... | sparkle~: cross with... | filter (filterdesign)... | condec: convolve with... | A patch (inlet~ to outlet~)... | A Musil script (process x sr)... | Result: replace the selection (on) | Result: a new sound | Stop (Esc)"
expect "To Score, with the score object" "To Coll... | To Score... | Show the Regions Panel"
expect "the To score button too" "state: sound to score active"
expect "a dialog asks for the other sound among the buffer~s" "ask: typematch: the selection rebuilt with the units of another sound: with=[buffer~ voice|"
expect "and for its settings" "ask: pvoc: stretch or transpose the selection: stretch=1 transpose=0 formants=keep window=2048 result=replace"
expect "a filter replaces the selection" "state: process filter: 0.200 s processed"
expect "Result: a new sound" "state: process pvoc: a new sound of 0:00.40"
expect "the menu says so" "Result: replace the selection | Result: a new sound (on)"
undo=$(grep "state: sound undo" "$out" | tr '\n' '|')
if [ $status -eq 0 ] && [ "$undo" = "state: sound undo no edited no|state: sound undo yes edited yes|state: sound undo no edited no|state: sound undo no edited yes|" ]
then ok; else fail "a process is one undo step; a new sound is unsaved ($undo)"; fi

# 25. A patch... and A Musil script... over the selection (examples/14-sound-editor), the recent list they share, a
#     script's error on the status line with its line, a patch without inlet~
E="$HERE/examples/14-sound-editor"
printf 'function process (x sr) {\n  var y 1\n  (no-such-function x)\n}\n' > "$T/bad.mu"
printf '(patch (obj g *~ 0.5 (pos 20 20)))\n' > "$T/noin.moritz"
run --window-png none "act=soundopen:$T/s.wav" act=sounddrag:0.1,0.3 "act=processscript:$E/tremolo.mu" act=processwait \
    "act=processpatch:$E/ring-modulator.moritz" act=processwait act=sound "act=processscript:$T/bad.mu" act=processwait \
    "act=processpatch:$T/noin.moritz" act=sound act=soundmenus act=soundop:undo act=soundop:undo act=sound
expect "a script processes the selection" "state: process tremolo.mu: 0.200 s processed"
expect "a patch too" "state: process ring-modulator.moritz: 0.200 s processed"
expect "a script's error, with its line" "bad.mu:3"
expect "a patch needs inlet~ and outlet~" "state: sound status noin.moritz: the patch needs an inlet~"
expect "the recent list" "Recent: bad.mu | Recent: ring-modulator.moritz | Recent: tremolo.mu"
expect "both undone" "state: sound undo no edited no"

# 26. markers and regions: M at the playhead, the Marker tool's click, a marker dragged in its lane, undo; a region
#     from the selection, renamed, chosen by its row; To coll; saved beside the file and read back with it
cp "$HERE/data/anechoic1.wav" "$T/m.wav"
run --window-png none "act=soundopen:$T/m.wav" act=sounddrag:0.1,0.1 act=soundkey:m act=soundtool:marker \
    act=sounddrag:0.3,0.3 act=soundmark:move:1:0.2 act=soundmarks act=soundop:undo act=soundmarks act=soundtool:select \
    act=sounddrag:0.2,0.4 act=soundmark:addregion act=soundmark:rename:1:voice act=sounddrag:0,0 act=soundmark:row:1 \
    act=soundmark:rowdouble:1 act=soundmarks "saveas=$T/m.coll" act=soundmark:coll "saveas=$T/m.score" act=soundmark:score \
    act=soundsave act=closefront \
    "act=soundopen:$T/m.wav" act=soundmarks
# (the mouse lands on a pixel: a frame within a pixel's width of the time aimed at)
if grep -q "state: sound markers 2 88[0-9][0-9]:marker 1 132[0-9][0-9]:marker 2" "$out"; then ok
else fail "M and the Marker tool place markers; one dragged"; fi
if [ "$(grep -c "state: sound markers 2 44[0-9][0-9]:marker 1 132[0-9][0-9]:marker 2" "$out")" = 3 ]; then ok
else fail "undo puts it back; the markers are read back with the file"; fi
if grep -q "state: sound region 1 voice 88[0-9][0-9] 176[0-9][0-9] type - energy" "$out"; then ok
else fail "a region from the selection, renamed"; fi
expect "its row selects it" "state: sound regions 1 selected 1"
expect "a double-click on its row asks its name" "ask: Name of region 1 (voice)"
if grep -q '(1 0.2[0-9]* 0.4[0-9]* "-" ' "$T/m.coll" && grep -q '"voice")' "$T/m.coll" && grep -q "(columns index start end type energy pitch name)" "$T/m.coll"
then ok; else fail "To coll writes the regions as rows"; fi
if grep -q '(marker 0.1[0-9]* "marker 1" hand)' "$T/m.wav.markers" && grep -q '(region 0.2[0-9]* 0.4[0-9]* "voice"' "$T/m.wav.markers"
then ok; else fail "markers saved beside the file"; fi
# To score: a sound event for the region, which a score object reads and plays
printf '(patch\n  (obj sc score regs m.score 2 (pos 20 20))\n  (obj lb loadbang (pos 20 80))\n  (obj m message "info, play" (pos 20 120))\n  (obj p print (pos 20 160))\n  (obj o dac~ 1 2 (pos 200 160))\n  (connect lb 0 m 0)\n  (connect m 0 sc 0)\n  (connect sc 4 p 0)\n  (connect sc 0 o 0)\n  (connect sc 1 o 1))\n' > "$T/play.moritz"
run --render "$T/play.moritz" 0.5
if [ $status -eq 0 ] && grep -q '(label "voice")' "$T/m.score" && grep -q "print: info 1 1 0.4" "$out" &&
   ! grep -q "ch1 peak 0.0000" "$out"
then ok; else fail "To score writes a score the score object plays"; fi

# 27. markers from onsets, regions from types (background jobs); the regions of an analysis replace the last one's
run --window-png none "act=soundopen:$HERE/data/drums.wav" act=soundmark:onsets act=processwait act=soundmark:regiononsets \
    act=processwait act=soundmark:types act=processwait act=soundmarks
onsets=$(grep "state: process Markers from Onsets: " "$out" | sed 's/.*Onsets: \([0-9]*\) found/\1/')
types=$(grep "state: process Regions from Types: " "$out" | sed 's/.*Types: \([0-9]*\)$/\1/')
if [ $status -eq 0 ] && [ "$onsets" -gt 20 ] 2>/dev/null && [ "$types" -gt 8 ] 2>/dev/null &&
   grep -q "state: sound markers $onsets 64:onset 1 " "$out" && grep -q "state: sound regions $types selected 0" "$out" &&
   grep -q "state: sound region 1 A 1 0 " "$out"
then ok; else fail "markers from onsets ($onsets), regions from types replacing those from onsets ($types)"; fi

# 28. the Draw tool: zoomed in, a stroke moves the samples it crosses (joined between the mouse's events), one undo
#     step; zoomed out it says to zoom in; Cmd+L loops (L alone does nothing); a trackpad's pinch zooms
run --window-png none "act=soundopen:$T/s.wav" act=soundtool:draw act=sounddrag:0.1,0.1 act=sound act=soundzoom:0.1,0.2 \
    "act=sounddraw:0.2,0.5/0.2004,-0.5" act=soundsample:8820 act=soundsample:8829 act=soundsample:8838 act=sound \
    act=soundop:undo act=sound act=soundpinch:500 act=sound act=soundkey:l act=sound act=soundkey:l:cmd act=sound
samples=$(grep "state: sound sample" "$out" | sed 's/state: sound sample [0-9]* //' | tr '\n' ' ')
case "$samples" in
    "0.50"*" "*"0.0"*" -0.50"*) ok ;;
    *) fail "the Draw tool's stroke ($samples)" ;;
esac
undo=$(grep "state: sound undo" "$out" | head -3 | tr '\n' '|')
if [ "$undo" = "state: sound undo no edited no|state: sound undo yes edited yes|state: sound undo no edited no|" ]; then ok
else fail "a stroke is one undo step ($undo)"; fi
expect "Draw asks to zoom in" "Draw: zoom in until each sample has a pixel"
expect "a pinch zooms in" "state: sound loop no view 0.066667"
expect "Cmd+L loops" "state: sound loop yes view"
if [ "$(grep -c "state: sound loop no" "$out")" = 5 ]; then ok; else fail "L alone does not loop"; fi

# 29. threads: a process stopped (Process > Stop), then the window closed while another runs, and a script that never
#     ends stopped by closing its window (its thread finds the editor gone)
printf 'function process (x sr) { var k 0\n  while true { set k (+ k 1) }\n}\n' > "$T/forever.mu"
run --window-png none "act=soundopen:$HERE/data/beachboys.wav" act=process:pvoc:stretch=4 act=processstop act=processwait \
    act=process:pvoc:stretch=4 act=closefront "act=soundopen:$T/s.wav" "act=processscript:$T/forever.mu" act=wait:0.3 \
    act=processstop act=processwait "act=processscript:$T/forever.mu" act=wait:0.2 act=closefront act=wait:0.3 act=state
expect "Stop stops a process" "state: process pvoc: stopped"
expect "and a script" "state: process forever.mu: stopped"
expect "closing while they run" "state: documents 0"

# 24. a score box's window in its Timeline view; a clip dragged is one undo step, taken back by Undo; the score is
#     kept in the patch, so the patch is edited (and the window asks nothing when it closes)
run "$G/timeline.moritz" --window-png none act=tlopen:s act=timeline act=tldrag:3,1 act=timeline act=tlop:undo \
    act=timeline act=state act=closefront act=timeline
moves=$(grep "state: timeline clip 3 " "$out" | sed 's/state: timeline clip 3 //' | cut -d' ' -f1-4 | tr '\n' '|')
if [ $status -eq 0 ] && grep -q "state: timeline score piece — timeline — Moritz" "$out" &&
   [ "$moves" = "sound voice 1 2|sound voice 2 2|sound voice 1 2|" ] && grep -q "state: timeline undo no redo yes" "$out" &&
   grep -q "state: front $G/timeline.moritz edited yes" "$out" && grep -q "state: timeline none" "$out" && ! grep -q "^ask:" "$out"
then ok; else fail "a score box's timeline: a clip dragged, undone (got $moves)"; fi

# 25. a clip dragged to another track, trimmed at both ends (a sound's from and to follow), split at the playhead,
#     a point clicked on a lane, a track muted by its header's M
run "$G/timeline.moritz" --window-png none act=tlopen:s act=tldrag:1,0,1 act=tltrim:3,right,2.5 act=tltrim:2,left,2.5 \
    act=tlplayhead:2 act=tlclick:3 act=tlop:split act=tlpoint:voice,density,1.5,0.75 act=tlmute:drums act=timeline
expect "a clip dragged to the other track" "state: timeline clip 1 sound voice 0 1"
expect "trimmed at its end, split at the playhead" "state: timeline clip 3 sound voice 1 1 from 0 to 1"
expect "the right half goes on in the source" "state: timeline clip 5 sound voice 2 0.5 from 1 to 1.5"
expect "trimmed at its start" "state: timeline clip 2 sound drums 2.5 0.5 from 1 to 1.5"
expect "a point on the lane (its value as near as a pixel)" "state: timeline clip 6 msg voice 1.5 0 value 0.7"
if [ $status -eq 0 ] && grep -q "Mute" "$out"; then ok; else fail "a track muted by its header"; fi

# 26. a new score: a sound added, saved where asked (atomically: no temporary file left), opened again the same;
#     edited, closing asks (Cancel keeps it, Don't Save closes it)
run --window-png none act=tlnew "act=tladdsound:$HERE/data/drums.wav" "saveas=$T/new.score" act=tlsave act=timeline
if [ $status -eq 0 ] && grep -q "drums.wav" "$T/new.score" && grep -q "state: timeline new.score — timeline — Moritz" "$out" &&
   [ -z "$(ls -A "$T" | grep '\.tmp$')" ]; then ok; else fail "a new score saved as a file"; fi
run --window-png none "act=tlfile:$T/new.score" "saveas=$T/new.wav" act=tlop:render act=wait:3 act=timeline
if [ $status -eq 0 ] && [ -s "$T/new.wav" ] && grep -q "state: timeline status rendered" "$out"; then ok
else fail "a score rendered to a WAV file"; fi
run --window-png none "act=tlfile:$T/new.score" act=timeline act=tldrag:1,1 answer=0 act=closefront act=timeline answer=2 \
    act=closefront act=timeline
expect "a score file opened" "state: timeline clip 1 sound track 1 0"
expect "closing an edited score asks" "ask: Save changes to the score \"new.score\"?"
expect "Cancel keeps it" "state: timeline new.score — edited — timeline — Moritz"
expect "Don't Save closes it" "state: timeline none"

# 27. a sub-score clip double-clicked: its own timeline, the breadcrumb, the instance it is; an edit there is every
#     instance's and goes into its file with Save; a patch clip double-clicked opens the patch
mkdir -p "$T/sub"
cp "$G/piece.score" "$G/gesture.score" "$G/timeline.moritz" "$T/sub/"
run --window-png none "act=tlfile:$T/sub/piece.score" act=tldbl:3 act=timeline act=tldrag:2,0.25 act=tlsave act=timeline
expect "a sub-score's timeline" "state: timeline gesture.score — timeline — Moritz"
expect "the breadcrumb" "state: timeline crumbs piece.score|gesture.score|"
if [ $status -ne 0 ] || ! grep -q "(note 0.5 0.25 Fl 74 mf (id 2))" "$T/sub/gesture.score"; then fail "an instance edited and saved"
else ok; fi
run --window-png none "act=tlfile:$T/sub/piece.score" act=tldbl:4 act=state
expect "a patch clip opens its patch" "state: front $T/sub/timeline.moritz"

# 30. the roll and the timeline of one score box are views of one document: a note dragged in the roll is an undo
#     step of the score (the patch edited, but no undo step of the patch), the text view follows; the Timeline view
#     shows the edit there, its Undo takes it back and the roll shows it; a clip dragged in the timeline is undone by
#     the roll's Undo; the roll's selection is the timeline's; the List view is the same window's
run "$G/score.moritz" --window-png none act=roll:s "act=rolldrag:1:0.5,1" act=rollstate act=state act=rolltimeline act=timeline \
    act=tlop:undo act=rollstate act=tldrag:3,1 act=rollstate act=rollundo act=timeline act=rollselect:2,3 act=timeline \
    act=tlview:list act=rollstate
expect "a note dragged in the roll" "state: roll note 1 0.5 0.5 62"
expect "the text view follows" "state: roll text (note 0.5 0.5 Fl 62 mf (id 1))"
expect "a roll edit marks the patch edited" "state: front $G/score.moritz edited yes"
expect "but is not an undo step of the patch" "state: undo no redo no"
expect "the timeline shows the roll's edit" "state: timeline clip 1 note Fl 0.5 0.5"
expect "the timeline's Undo takes the roll's edit back" "state: roll note 1 0 0.5 60"
expect "a clip dragged in the timeline is in the roll" "state: roll note 3 2 0.5 67"
expect "the roll's Undo takes the timeline's edit back" "state: timeline clip 3 note Fl 1 0.5"
expect "one selection" "state: timeline selection 2 3 "
expect "the List view: the same window" "state: roll windows 1 view list"
if [ $status -eq 0 ] && [ "$(grep -c "state: timeline score piece — timeline — Moritz" "$out")" = 3 ]; then ok
else fail "the Timeline view shows the same score"; fi

# 31. the text view: a valid text is applied (a note more), one that does not read is not (its error said, the score
#     as it was)
run "$G/score.moritz" --window-png none act=roll:s "act=rolltext:$G/score-text.txt" act=rollstate "act=rolltext:$G/score-bad.txt" act=rollstate
expect "a text applied" "state: roll note 4 2 1 72"
expect "a text that does not read" "state: roll text error"
expect "says why" "bars.beats.ticks"
if [ "$(grep -c "state: roll note 4 2 1 72" "$out")" = 2 ]; then ok; else fail "a bad text leaves the score as it was"; fi

# 32. the score box edited in place (locked): a click is not an edit; a note dragged is (the patch edited, an undo step
#     of the score); Edit > Undo with the box clicked last takes it back (the score's history, not the patch's), and
#     the roll's window, opened then, can redo it
run "$G/score.moritz" --window-png none act=draw act=click:s act=state "act=boxdrag:s:2:40,0" act=state act=menu:edit.undo \
    act=roll:s act=rollstate act=menu:edit.redo act=rollstate
if [ $status -eq 0 ] && grep -q "state: front $G/score.moritz edited no" "$out" && grep -q "state: front $G/score.moritz edited yes" "$out" &&
   ! grep -q "state: undo yes" "$out" && grep -q "state: roll note 2 0.5 0.5 64" "$out" && grep -q "state: roll undo no redo yes" "$out" &&
   grep -q "state: roll note 2 1 0.5 64" "$out"; then ok; else fail "the score box edited in place, undone by Edit > Undo"; fi

# 33. both views of one box: a double-click (locked) on the box opens its window in the Roll view (a box's default),
#     opening it in the Timeline view brings the same window forward; its Roll view again
run "$G/score.moritz" --window-png none act=dblclick:s act=rollstate act=tlopen:s act=timeline act=tlview:roll act=rollstate
expect "a double-click on the box opens its roll" "state: roll score piece — roll — Moritz"
expect "and its timeline" "state: timeline score piece — timeline — Moritz"
if [ $status -eq 0 ] && [ "$(grep -c "state: roll windows 1 view roll" "$out")" = 2 ]; then ok; else fail "one window for both"; fi

# 34. the cents, always visible and edited: the box's own roll shows each detuned note's cents (a tempered one
#     nothing); in the roll, Alt+Up a cent, Shift+Alt+Up ten, each an undo step; the List's cents cell typed into
run "$G/detuned.moritz" --window-png none act=draw act=boxcents:s:1 act=boxcents:s:2 act=boxcents:s:3 act=roll:s act=rollselect:3 \
    act=rollkey:up:alt act=rollkey:up:alt:shift act=rollstate act=rollundo act=rollundo act=rollstate "act=rollcell:3:cents:-12.5" \
    act=rollstate act=rollundo act=rollstate
expect "the box shows a note's cents" "state: box cents text +19"
expect "and a lower one's" "state: box cents text -41"
expect "nothing for a tempered note" "state: box cents none"
expect "Alt+Up, then Shift+Alt+Up: eleven cents" "state: roll cents 3 11"
expect "the List's cell" "state: roll cents 3 -12.5"
if [ $status -eq 0 ] && [ "$(grep -c "state: roll cents 3 " "$out")" = 2 ] && [ "$(grep -c "state: roll cents 1 19" "$out")" = 4 ]; then ok
else fail "the cents edits undone exactly"; fi

# 35. one window per score, its views switched in place: the selection and the playhead kept; opening the score again
#     brings the same window forward in the view asked; closed and double-clicked again, it opens in the view used last
run "$G/score.moritz" --window-png none act=roll:s act=rollselect:2,3 act=tlplayhead:0.75 act=scview:timeline act=timeline \
    act=scstate act=scview:piano act=rollstate act=scstate act=tlopen:s act=rollstate act=scview:list act=closefront \
    act=scstate act=dblclick:s act=rollstate
expect "the selection kept by the Timeline view" "state: timeline selection 2 3 "
expect "the Piano roll view, in the same window" "state: roll windows 1 view piano"
expect "the selection kept by the roll" "state: roll selection 2 3"
expect "opened again: the same window" "state: roll windows 1 view timeline"
expect "closed" "state: scstate none"
expect "a double-click: the view used last" "state: roll windows 1 view list"
if [ $status -eq 0 ] && [ "$(grep -c "playhead 0.75$" "$out")" = 2 ]; then ok; else fail "the playhead kept across the views"; fi

# 36. a box's @view: the window opens in that view the first time (the timeline example: its Timeline); switching views
#     does not change the box (the patch is not edited, its @view saved as it was)
run "$HERE/examples/14-timeline/timeline.moritz" --window-png none act=dblclick:sc act=scstate
expect "the timeline example opens its Timeline" "state: score view timeline"
printf '(patch\n  (obj s score v @view list (pos 20 20)))\n' > "$T/view.moritz"
run "$T/view.moritz" --window-png none act=dblclick:s act=scstate act=scview:roll act=state act=save
expect "@view list" "state: score view list"
if [ $status -eq 0 ] && grep -q "state: front $T/view.moritz edited no" "$out" && grep -q "@view list" "$T/view.moritz"; then ok
else fail "@view kept, switching views is no edit"; fi

# 37. Play always plays: from the playhead (the transport stopped), again Stop; with the patch's transport running past
#     the score's end (the score not playing then), Play starts the score from its playhead; Space does what the button
#     does whatever is selected (a selection made after the playhead was placed: from its start); a play started here
#     stops at the score's end, the playhead back where it started
run "$G/score.moritz" --window-png none act=roll:s act=tlplayhead:0.5 act=scplay act=scstate act=scplay act=scstate \
    act=scpatchplay:30 act=scstate act=scplay act=scstate act=rollselect:3 act=scspace act=scspace act=scstate \
    act=tlplayhead:1.4 act=scplay act=wait:0.4 act=sctick act=scstate
expect "Play from the playhead" "state: score view roll playing yes transport yes button on position 0.5 playhead 0.5"
expect "again: Stop" "state: score view roll playing no transport no button off"
expect "the patch's clock past the score's end" "state: score view roll playing no transport yes button off"
expect "Space from the selection's start" "state: score view roll playing yes transport yes button on position 1 playhead 1"
expect "the end of the score" "state: score view roll playing no transport no button off position 1.4 playhead 1.4"
if [ $status -eq 0 ] && [ "$(grep -c "state: score view roll playing yes transport yes button on position 0.5" "$out")" = 2 ]; then ok
else fail "Play with the transport already running"; fi

# 38. the dynamics lane's row chooser: a row chosen filters the lane, and the lane, drawn alone (as when only it is
#     redrawn), never paints over the chooser (it vanished when clicked)
run "$G/score.moritz" --window-png none act=roll:s act=rolldynrow:1 act=rolldynrow:0
expect "a row chosen" "state: roll dynrow Fl lane Fl covered no shown yes"
expect "all of them again" "state: roll dynrow all lane all covered no shown yes"

# 39. what a track holds, opened from the timeline: a lane's name opens the patch that receives it with those receives
#     selected (the header double-clicked too); a track's name the sound its clip plays, a sound clip double-clicked
#     the sound editor with the clip's region selected; a track with only a lane, its receives (the timeline example)
run "$G/timeline.moritz" --window-png none act=tlopen:s act=tlname:voice,density act=selected act=tlname:voice act=sound \
    act=tldbl:2 act=sound
expect "a lane's name: its receive selected" "state: selected timeline.moritz: rd"
expect "a track's name: the sound it plays" "state: sound anechoic1.wav — Moritz"
expect "a clip's region selected" "state: sound selection 22050 66150"
run "$G/timeline.moritz" --window-png none act=tlopen:s act=tlheaddbl:voice,density act=selected
expect "a lane's header double-clicked" "state: selected timeline.moritz: rd"
run "$HERE/examples/14-timeline/timeline.moritz" --window-png none act=tlopen:sc act=tlname:filter act=selected
expect "a track of lanes: the receives it drives" "state: selected timeline.moritz: rc"

# 40. track heights: a track's lower edge dragged, another folded to one line (a double-click on its edge), kept in the
#     score's rows, so saved and read again; Alt folds every one, Undo puts them back; a lane sized on its own
mkdir -p "$T/h"
cp "$G/piece.score" "$G/gesture.score" "$T/h/"
run --window-png none "act=tlfile:$T/h/piece.score" act=tledge:drums,30 act=tlfold:gestures act=timeline act=tlsave
expect "a track sized" "state: timeline row drums 94"
expect "a track folded" "state: timeline row gestures 20"
if [ $status -eq 0 ] && grep -q "(row drums (data height 94))" "$T/h/piece.score"; then ok; else fail "the heights saved with the score"; fi
run --window-png none "act=tlfile:$T/h/piece.score" act=timeline act=tlfold:drums,alt act=timeline act=tlop:undo act=timeline
expect "read again" "state: timeline row drums 94"
expect "Alt: every track at once" "state: timeline row drums 20"
if [ $status -eq 0 ] && [ "$(grep -c "state: timeline row drums 94" "$out")" = 2 ]; then ok; else fail "Undo puts the heights back"; fi
run "$G/timeline.moritz" --window-png none act=tlopen:s act=tledge:voice,density,20 act=timeline act=state
expect "a lane sized on its own" "state: timeline row voice/density 58"
expect "its track as it was" "state: timeline row voice 64"

rm -rf "$T"
echo "$count interface cases, $fails failed"
[ $fails -eq 0 ]
