; chords.mu - the script of chords.moritz, in a file of its own (the box says
; "code chords.mu"): the harmonic field above a note, chords taken from it, and a
; melody that wanders through them, played by sines into a buffer~. Edit it here
; or by double-clicking the box; the box reads it again at every run.
load "music.mu"

; in1: the root (a pitch name, or a MIDI number); C2 at first
var root (if (equal? in1 nil) "C2" in1)
var field (harmonic-series root 16)
var chord (take (drop field 4) 5)
print "the field above" (pitch->name root) ":" (map chord midi->pitch)

; a melody: a random walk on the chord's notes
var notes (list)
var at 2
for (var k 0) (< k 16) (var k (+ k 1)) {
    set at (max 0 (min 4 (+ at (- (floor (* 3 (rand))) 1))))
    push notes (getidx chord at)
}
print "a melody through it:" (map notes midi->pitch)

; the chord held under it, the melody a sine an octave up, an eighth of a second a note
var sr 44100
var step 0.25
var y (zeros (floor (* sr (+ 0.5 (* step (length notes))))))
each chord (function (p) (add-at! y 0 (* 0.08 (sine sr (midi->hz p) (/ (length y) sr)))))
for (var k 0) (< k (length notes)) (var k (+ k 1)) {
    var tone (* 0.3 (sine sr (* 2 (midi->hz (getidx notes k))) step) (hann (floor (* step sr))))
    add-at! y (floor (* k step sr)) tone
}
set-buffer! "chout" (* 0.9 (normalize-peak y)) sr
out 1 (map notes midi->pitch)
