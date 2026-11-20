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

each (ls tmp) (function (f) (remove (concat tmp "/" f)))
remove tmp
report "test_score"
