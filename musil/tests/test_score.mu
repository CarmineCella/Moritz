; test_score: the score as a C++ structure (music/score.h) seen from Musil: events and their keys, ids, placements
; and cycles, .score files read and written (round trips, untrusted input), coll rows, bars and beats
load "test.mu"
load "music.mu"
var tmp-root (getenv "TMPDIR")
if (or (equal? (type tmp-root) "nil") (equal? tmp-root "")) { set tmp-root "/tmp" }
var tmp (concat tmp-root "/musil_test_score_" (str (floor (* 1e6 (rand)))))
mkdir tmp
function close? (a b eps) (< (max (abs (- a b))) eps)

; --- a score is a value, its events answer as records ---
var s (score "t" 8000)
check (score? s) "score?: a score"
check (not (score? (list 1 2))) "score?: a list is not"
check (equal? (type s) "opaque:score") "type: opaque:score"
check (and (equal? (get s 'name) "t") (== (get s 'sr) 8000) (== (get s 'bpm) 60) (equal? (get s 'meter) (list 4 4))) "score: name, sr, bpm, meter"
check (equal? (get s 'layout) "stereo") "score: its layout, kept with it"
var e1 (event s 1 0.5 "a.wav")
var e2 (event s 0 0.25 "b.wav")
check (equal? (type e1) "opaque:event") "event: opaque:event"
check (and (== (get e1 'id) 1) (== (get e2 'id) 2)) "ids: one by one"
check (equal? (get e1 'kind) 'file) "event: kind"
check (has? e1 'gain) "has?: a key it has"
check (not (has? e1 'nothing)) "has?: a key it has not"
check (== (opt e1 'nothing 7) 7) "opt: the default"
put! e1 'colour "red"
check (equal? (get e1 'colour) "red") "put!: a key of the user's"
put! e1 'at 2
check (== (score-duration s) 2.5) "put!: the onset changes the score"
check (contains? (keys e1) 'colour) "keys: the user's too"
check (equal? (get (put e1 'gain 0.5) 'gain) 0.5) "put: a copy..."
check (== (get e1 'gain) 1) "put: ... the event untouched"
check (== (get (head (score-events s)) (quote id)) 1) "score-events: in the order added"
check (equal? (head (score-events s)) e1) "equal?: the same event"

; --- ids are never reused ---
score-remove! s e1
check (== (length (score-events s)) 1) "score-remove!"
var e3 (event s 3 1 "c.wav")
check (== (get e3 'id) 3) "ids: never reused after a removal"
check (equal? (get (score-find s 3) 'source) "c.wav") "score-find"
check (equal? (type (score-find s 1)) "nil") "score-find: gone"
check (== (get e1 'at) 2) "a removed event still reads"
check (== (score-take-id! s) 4) "score-take-id!"
check (== (get (event s 0 1 "d.wav") 'id) 5) "ids go on after a taken one"

; --- time queries ---
var q (score "q" 1000)
each (range 100) (function (k) (event q k 0.5 "x.wav"))
event q 10 30 "long.wav"
check (== (length (score-query q 20 22)) 3) "score-query: two short events and a long one"
check (== (length (score-query q 50.6 50.9)) 0) "score-query: a gap"
check (== (length (score-query q 99 1000)) 1) "score-query: the last"

; --- scores in scores: placements and cycles ---
var inner (score "inner" 8000)
event inner 0 1 "i.wav"
var outer (score "outer" 8000)
var se (event outer 2 0 inner)
check (close? (get se 'dur) 1.6 1e-9) "a score placed: its duration with room for releases"
check (same? (get se 'source) inner) "same?: the score placed is the score"
check (contains? (error-of (function () (event inner 0 0 outer))) "cannot contain itself") "a cycle through another score is refused"
check (contains? (error-of (function () (event outer 0 0 outer))) "cannot contain itself") "a score in itself is refused"
var third (score "third" 8000)
event third 0 0 outer
check (contains? (error-of (function () (event inner 0 0 third))) "cannot contain itself") "a cycle two levels down is refused"
put! se 'stretch 2
put! se 'transpose 7
put! se 'az 30
put! se 'dur 3
var f (score-flatten outer 0)
check (== (length f) 1) "score-flatten: the inner event"
check (and (== (get (head f) 'at) 2) (== (get (head f) 'dur) 2) (== (get (head f) 'az) 30)) "score-flatten: stretched and turned by the placement"
var notes-in (score "notes" 8000)
event notes-in 0.5 1 (record (list 'kind 'note 'instr "Fl" 'midi 60 'pitch "C4" 'dyn "mf" 'tech "ord" 'cents 0))
var notes-out (score "notes out" 8000)
var ne (event notes-out 1 0 notes-in)
put! ne 'transpose 5
check (== (get (head (score-flatten notes-out 0)) 'midi) 65) "score-flatten: notes transposed"
check (== (get (head (score-events notes-in)) 'midi) 60) "score-flatten: the score inside untouched"
check (contains? (error-of (function () (score-flatten outer 17))) "deep") "score-flatten: too deep"

; --- .score files: written, read, the same ---
var w (score "gesture" 44100)
put! w 'bpm 108
put! w 'rows (list (list "Fl" "treble") (list "Vc" "bass"))
event w 0.25 0.5 (record (list 'kind 'note 'instr "Fl" 'midi 79 'pitch "G5" 'dyn "mf" 'tech "ord" 'cents 0))
event w 1.1 0.3333333333333333 (record (list 'kind 'note 'instr "Vc" 'midi 48.5 'pitch "C3" 'dyn "p" 'tech "pizz" 'cents 12))
var snd (event-at w 2 1.5 "../../data/drums.wav" -30 10)
put! snd 'from 0.25
event w 3 0 (record (list 'kind 'message 'source "density" 'values (list 0.6 'up "x")))
event w 4 2 (record (list 'kind 'patch 'source "grains.moritz" 'values (list 1 2)))
var tex (score "texture" 44100)
event tex 0 1 (record (list 'kind 'note 'instr "Cl" 'midi 62 'pitch "D4" 'dyn "pp" 'tech "ord" 'cents 0))
var pl (event w 5 2 tex)
put! pl 'transpose 7
put! pl 'stretch 1.5
put! pl 'gain 0.5
var path (concat tmp "/gesture.score")
score-write w path
var r (score-read path)
check (equal? (score->text r) (score->text w)) "score-write, score-read: the same score"
check (equal? (read path) (score->text r)) "read, written again: the same text"
check (== (length (score-events r)) 6) "score-read: every event"
check (and (== (get r 'bpm) 108) (equal? (get r 'name) "gesture")) "score-read: tempo and name"
check (equal? (map (score-events r) (function (e) (get e 'kind))) (list 'note 'note 'file 'message 'patch 'score)) "score-read: the kinds"
check (== (get (getidx (score-events r) 1) 'midi) 48.5) "score-read: a microtonal pitch"
check (== (get (getidx (score-events r) 1) 'cents) 12) "score-read: cents"
check (equal? (get (getidx (score-events r) 3) 'values) (list 0.6 'up "x")) "score-read: a message's values"
check (== (get (getidx (score-events r) 2) 'from) 0.25) "score-read: a sound's region"
check (and (== (get (last (score-events r)) 'stretch) 1.5) (== (get (last (score-events r)) 'transpose) 7)) "score-read: a placement"
check (equal? (get (get (last (score-events r)) 'source) 'name) "texture") "score-read: the score placed, embedded"
check (equal? (get r 'rows) (list (list "Fl" "treble") (list "Vc" "bass"))) "score-read: rows"
check (equal? (score->data r) (score->data w)) "score->data: the same data"
check (equal? (score->text (data->score (score->data w))) (score->text w)) "data->score: back from the data"
check (equal? (head (score->data w)) 'score) "score->data: a (score ...) form"
; numbers written short and still read back: the arithmetic's noise is not written, a value that needs 17 digits has them
var noisy (score "noisy" 44100)
event noisy (* 0.3 0.75) (+ 0.1 0.2) (record (list 'kind 'note 'instr "Fl" 'midi 60 'pitch "C4" 'dyn "mf" 'tech "ord" 'cents 0))
event noisy (/ 1 3) (/ 9922 44100) (record (list 'kind 'note 'instr "Fl" 'midi 62 'pitch "D4" 'dyn "mf" 'tech "ord" 'cents 0))
var ntext (score->text noisy)
check (and (contains? ntext "(note 0.225 0.3 ") (not (contains? ntext "0.22499999999999998")) (not (contains? ntext "0.30000000000000004"))) "score->text: 0.3 x 0.75 is written 0.225, 0.1 + 0.2 is 0.3"
var nback (text->score ntext)
check (and (close? (get (head (score-events nback)) 'at) (* 0.3 0.75) 1e-15) (== (get (head (score-events nback)) 'at) 0.225)) "the noise left out reads back as the number meant"
check (and (== (get (last (score-events nback)) 'at) (/ 1 3)) (== (get (last (score-events nback)) 'dur) (/ 9922 44100))) "a value that needs 17 digits reads back exactly"
check (equal? (score->text nback) ntext) "written again: the same text"
; a score placed by file, three times: one score
var tex-path (concat tmp "/texture.score")
score-write tex tex-path
write (concat tmp "/piece.score") "(score \"piece\" (score 0 \"texture.score\") (score 2 \"texture.score\" (transpose 3)) (score 4 \"texture.score\" (db -6)))"
var piece (score-read (concat tmp "/piece.score"))
var placed (score-events piece)
check (== (length placed) 3) "a score placed by file three times"
check (same? (get (head placed) 'source) (get (last placed) 'source)) "... is one score, three instances"
check (close? (get (last placed) 'gain) (pow 10 (/ -6 20)) 1e-12) "(db -6): a gain in decibels"
check (contains? (score->text piece) "\"texture.score\"") "written back by its file's name"
; a sound named bare in a .score is found next to the score (wherever the score is), else where Musil finds files and in
; the shared data folder (data/), as the score object finds it: score-read and render play it (read-wav could not open
; "stems_drums.wav" before)
mkdir (concat tmp "/sounds")
write-wav (concat tmp "/sounds/beside.wav") 8000 (list (* 0.5 (ones 800)))
write (concat tmp "/sounds/bare.score") "(score \"bare\" (sr 8000) (sound 0 0.1 \"beside.wav\") (sound 0.2 0.1 \"drums.wav\"))"
var bare (score-read (concat tmp "/sounds/bare.score"))
check (and (ends-with? (score-sound-file (head (score-events bare))) "/sounds/beside.wav") (exists? (score-sound-file (head (score-events bare))))) "score-sound-file: a bare name, next to the .score"
check (ends-with? (score-sound-file (last (score-events bare))) "data/drums.wav") "score-sound-file: a bare name in the shared data folder"
var bare-y (score-render bare "mono")
check (> (max (abs (take (head bare-y) 800))) 0.4) "score-render: the sound next to the .score plays"
check (> (max (abs (slice (head bare-y) 1600 800))) 0.01) "score-render: the sound in data/ plays"
write (concat tmp "/outer.score") "(score \"outer\" (sr 8000) (score 0 \"sounds/bare.score\"))"
var flat (score-flatten (score-read (concat tmp "/outer.score")) 0)
check (ends-with? (get (head flat) 'source) "/sounds/beside.wav") "score-flatten: a sound of a score placed inside is found from that score's folder"
check (equal? (score-sound-file "no-such-sound.wav") "no-such-sound.wav") "score-sound-file: a file that is nowhere, as written"
; a cycle through files
write (concat tmp "/a.score") "(score \"a\" (score 0 \"b.score\"))"
write (concat tmp "/b.score") "(score \"b\" (score 0 \"a.score\"))"
check (contains? (error-of (function () (score-read (concat tmp "/a.score")))) "cannot contain itself") "a cycle through files is refused"

; --- untrusted input: errors, never a crash ---
function bad (text want msg) {
    var err (error-of (function () (text->score text)))
    check (contains? err want) msg
}
bad "(score \"x\" (note 0 1 Fl 60" "missing )" "an unclosed form"
bad (concat "(score \"x\" " (join (map (vec->list (range 5000)) (function (k) "(")) "") ")") "nested too deep" "nesting 5000 deep"
bad "(score \"x\" (note 0 1e400 Fl 60))" "finite" "a huge number"
bad "(score \"x\" (note -1 1 Fl 60))" "out of range" "a negative onset"
bad "(score \"x\" (note 1.1.30 0.0.90 Fl G5 mf ord))" "seconds" "bars.beats.ticks: refused with the reason"
bad "(score \"x\" (note 0 1 Fl Q9))" "not a pitch" "a pitch that is not one"
bad "(score \"x\" (frobnicate 1 2))" "unknown form" "an unknown form"
bad "(score \"x\" (note 0 1 Fl 60 mf ord (colour red)))" "unknown option" "an unknown option"
bad "(score \"x\" (sound 0 1))" "missing" "a sound without its file"
bad "(score \"x\" (meter 0 4))" "out of range" "a meter of no beats"
bad "(score \"x\" (sr nan))" "finite" "not a number"
bad "(notscore)" "not a score" "not a score"
bad "(score \"a\") (score \"b\")" "one (score" "two scores in one file"
bad "" "one (score" "nothing"
var deep-text (concat (join (map (vec->list (range 20)) (function (k) "(score \"d\" (score 0 ")) "") "(score \"end\")" (join (map (vec->list (range 20)) (function (k) "))")) ""))
bad deep-text "deep" "scores embedded 20 deep"
bad "(score \"x\" (score 0 (score \"y\") (stretch 0)))" "out of range" "a stretch of 0"
check (== (length (score-events (text->score "(score \"x\" (note 0 1 Fl 60 (id 3)) (note 1 1 Fl 62 (id 3)))"))) 2) "a repeated id: renumbered, not lost"
check (equal? (get (head (score-events (text->score "(score \"x\" (note 0 1 Fl C#4 mf))"))) 'midi) 61) "a pitch by name"

; --- coll rows: one event a row ---
var rows (score->coll w)
check (equal? (head rows) (list 'note 0.25 0.5 'Fl 79 'mf 'ord 1)) "score->coll: a note's row"
check (== (length rows) 5) "score->coll: the score placed is not a row"
var back (coll->score "back" 44100 rows)
check (equal? (score->coll back) rows) "coll->score: the same rows back"
check (equal? (get (head (score-events (coll->score "n" 44100 (list (list 'note 0 1 'Vn 'G5 'f 'ord 1))))) 'midi) 79) "coll->score: a pitch by name"
check (contains? (error-of (function () (coll->score "n" 44100 (list (list 'nope 0))))) "unknown row") "coll->score: an unknown row"

; --- bars and beats; where a sound goes ---
check (equal? (score-bbt w 0) (list 1 1 0)) "score-bbt: the start"
check (equal? (score-bbt w (/ 60 108)) (list 1 2 0)) "score-bbt: the second beat"
check (equal? (score-bbt w (* 4 (/ 60 108))) (list 2 1 0)) "score-bbt: the second bar"
check (close? (score-bbt->sec w 2 1 240) (* 4.5 (/ 60 108)) 1e-12) "score-bbt->sec"
check (close? (score-pan-gains 45 2) (vec (map (pan-azimuth (vec 1) 45) head)) 1e-15) "score-pan-gains: pan-azimuth's law"
check (close? (score-pan-gains 30 6) (vec (map (pan-n (vec 1) (speaker-ring 6) 30) head)) 1e-12) "score-pan-gains: pan-n on a ring"

; --- music.mu on the score: what it did, it does ---
var m (score "m" 8000)
var b (event m 0 0.25 (sin (* 0.1 (range 4000))))
check (equal? (get b 'kind) 'buffer) "a vector: a buffer, kept for Musil"
check (equal? (type (get b 'source)) "vec") "... its vector is there"
check (not (contains? (score->text m) "source")) "... and not written to the file"
var mm (score-merge m m)
check (and (== (length (score-events mm)) 2) (!= (get (head (score-events mm)) 'id) (get (last (score-events mm)) 'id))) "score-merge: copies, distinct ids"
event m 0.1 0 (record (list 'kind 'message 'source "tonic" 'values (list 62)))
check (== (length (head (score-render m "mono"))) (+ 2000 (floor (* 0.6 8000)))) "score-render: a message makes no sound"

; --- transformations of a selection (music/score_transform.h: the same as Moritz's roll and score object) ---
function four () {
    var x (coll->score "x" 44100 (list (list 'note 0 0.5 'Fl 72 'mf 'ord 1) (list 'note 0.5 0.25 'Fl 74 'p 'ord 1)
                                       (list 'note 1 1 'Vc 48 'f 'pizz 1) (list 'note 2.25 0.75 'Vc 50 'mf 'ord 1)))
    put! (score-find x 4) 'cents 25
    return x
}
function pitches-of (x) (map (score-events x) (function (e) (get e 'midi)))
function ats-of (x) (map (score-events x) (function (e) (get e 'at)))
function durs-of (x) (map (score-events x) (function (e) (get e 'dur)))
var t0 (score->text (four))
var x (four)
score-transpose! x 3
check (equal? (pitches-of x) (list 75 77 51 53)) "score-transpose!: every note"
score-transpose! x -3
check (equal? (score->text x) t0) "score-transpose!: and back, the same score"
score-transpose! x 12 (list 1 (score-find x 3))
check (equal? (pitches-of x) (list 84 74 60 50)) "score-transpose!: the notes of a selection (ids and events)"
set x (four)
score-invert! x 60
check (equal? (pitches-of x) (list 48 46 72 70)) "score-invert!: around C4"
check (== (get (score-find x 4) 'cents) -25) "score-invert!: the cents mirrored too"
score-invert! x 'C4
check (equal? (score->text x) t0) "score-invert!: twice, the same score"
set x (four)
score-retrograde! x
check (equal? (ats-of x) (list 2.5 2.25 1 0)) "score-retrograde!: backwards within the span"
score-retrograde! x
check (equal? (score->text x) t0) "score-retrograde!: twice, the same score"
set x (four)
score-retrograde! x (list 1 2)
check (equal? (ats-of x) (list 0.25 0 1 2.25)) "score-retrograde!: a selection within its own span"
set x (four)
score-augment! x 2
check (and (equal? (ats-of x) (list 0 1 2 4.5)) (equal? (durs-of x) (list 1 0.5 2 1.5))) "score-augment!: from the first onset"
score-diminish! x 2
check (equal? (score->text x) t0) "score-diminish!: back"
set x (four)
score-augment! x 2 (list 3 4)
check (and (equal? (ats-of x) (list 0 0.5 1 3.5)) (equal? (durs-of x) (list 0.5 0.25 2 1.5))) "score-augment!: a selection, from its first onset"
check (contains? (error-of (function () (score-augment! x 0))) "> 0") "score-augment!: a factor of 0 is refused"
var q (coll->score "q" 44100 (list (list 'note 0.3 0.4 'Fl 72 'mf 'ord 1) (list 'note 1.1 0.65 'Fl 72 'mf 'ord 1) (list 'msg 0.7 'tonic 62)))
score-quantize-grid! q 0.5 1
check (and (equal? (ats-of q) (list 0.5 1 0.5)) (equal? (durs-of q) (list 0.5 1 0))) "score-quantize-grid!: onsets and ends on the grid, a message's onset"
set q (coll->score "q" 44100 (list (list 'note 0.3 0.4 'Fl 72 'mf 'ord 1)))
score-quantize-grid! q 0.5 0.5
check (and (close? (head (ats-of q)) 0.4 1e-12) (close? (head (durs-of q)) 0.45 1e-12)) "score-quantize-grid!: half way"
var h1 (four)
var h2 (four)
score-humanize! h1 0.01 1 7
score-humanize! h2 0.01 1 7
check (equal? (score->text h1) (score->text h2)) "score-humanize!: the same seed, the same score"
check (not (equal? (score->text h1) t0)) "score-humanize!: something moved"
check (< (max (abs (- (vec (ats-of h1)) (vec (ats-of (four)))))) 0.0100001) "score-humanize!: by no more than asked"
set x (four)
check (== (length (score-thin! x 1 3)) 0) "score-thin!: keep 1 keeps all"
check (equal? (score-thin! x 0 3) (vec 1 2 3 4)) "score-thin!: keep 0, all taken out"
check (== (length (score-events x)) 0) "score-thin!: the score is empty"
var th1 (four)
var th2 (four)
check (equal? (score-thin! th1 0.5 11) (score-thin! th2 0.5 11)) "score-thin!: the same seed, the same events"
set x (four)
check (equal? (score-density! x (list 1 0) 5) (vec 4)) "score-density!: the second half of the span thinned out"
set x (four)
score-shift! x 1 (list 2)
check (equal? (ats-of x) (list 0 1.5 1 2.25)) "score-shift!: a selection"
score-shift! x -10
check (equal? (ats-of x) (list 0 0 0 0)) "score-shift!: never before 0"
set x (four)
score-scale! x 0.5
check (equal? (ats-of x) (list 0 0.25 0.5 1.125)) "score-scale!: onsets and durations"
var sh (score "sh" 44100)
score-add! sh (record (list 'kind 'note 'at 0 'dur 1 'midi 60 'instr "Fl" 'shift 0.5))
score-transpose! sh 2
check (== (get (head (score-events sh)) 'shift) 2.5) "score-transpose!: a Musil note's sound shifted as much"

; --- the layout of the roll (music/score_layout.h) ---
check (== (instrument-rank 'Vn) 29) "instrument-rank: Vn"
check (== (instrument-rank "Zz") 1002) "instrument-rank: an unknown one after, by length"
check (equal? (head orchestral-order) "Picc") "orchestral-order"
check (equal? (staff-clef (list 40 50 70)) "bass") "staff-clef: a median below C4"
check (equal? (staff-clef (list 59 61)) "treble") "staff-clef: a median at C4"
check (equal? (staff-clef (list)) "none") "staff-clef: no pitches"

each (ls (concat tmp "/sounds")) (function (f) (remove (concat tmp "/sounds/" f)))
each (ls tmp) (function (f) (remove (concat tmp "/" f)))
remove tmp
report "test_score"
