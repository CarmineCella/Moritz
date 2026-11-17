; test_atoms.mu — Maple (matching pursuit) and SoundTypes in the music library.
;
; Every check is a (check ...) from test.mu. A passing run prints only the final line.
; Run with: musil tests/test_atoms.mu

load "test.mu"
load "music.mu"

var sr 22050
; a test sound: three notes, each a few harmonics with a decaying envelope, then a noise burst
function note (f dur) {
    var t (/ (range 0 (* dur sr)) sr)
    var x (* 0.3 (+ (sin (* 2 pi f t)) (* 0.5 (sin (* 4 pi f t))) (* 0.25 (sin (* 6 pi f t)))))
    return (* x (exp (* -3 t)))
}
var x (vec (note 220 0.4) (note 330 0.4) (note 440 0.4) (* 0.2 (noise (* 0.3 sr))))
var y (vec (note 262 0.5) (note 392 0.5) (note 196 0.5))

; --- dictionaries ---
var g (maple-gabor sr 128 2048)
check (equal? (type g) "opaque:maple-dictionary") "maple-gabor: a dictionary"
check (equal? (get (maple-info g) 'kind) "gabor") "maple-info: kind"
check (== (get (maple-info g) 'size) 5) "maple-info: one scale per power of 2 from 128 to 2048"
check (== (get (maple-info (maple-gammatone sr 300 300)) 'lo) 512) "maple-gammatone: windows rounded up to powers of 2"
var d (maple-cut y sr 'frames 50 50 "y")
check (== (get (maple-info d) 'size) 30) "maple-cut: frames of 50 ms every 50 ms"
check (near? (sum (* (maple-atom d 0) (maple-atom d 0))) 1 1e-4) "maple-atom: unit energy"
check (> (get (maple-info (maple-cut x sr 'onsets 2 1000 "x")) 'size) 1) "maple-cut: onsets"
check (== (get (maple-info (maple-cut x sr 'whole 500 0 "x")) 'size) 1) "maple-cut: whole"
check (== (get (maple-info (maple-merge d d)) 'size) 60) "maple-merge"
check (contains? (error-of (function () (maple-cut x sr 'grains 50 50 "x"))) "mode") "maple-cut: unknown mode"
check (contains? (error-of (function () (maple-merge d g))) "sounds") "maple-merge: only dictionaries of sounds"
maple-save d "/tmp/musil_test.atoms"
check (== (get (maple-info (maple-load "/tmp/musil_test.atoms")) 'size) 30) "maple-save, maple-load"

; --- decomposition and synthesis: Gabor ---
var b (maple-decompose x sr g (list 'atoms 400 'snr 40))
check (equal? (type b) "opaque:maple-book") "maple-decompose: a book"
var rows (maple-book b)
check (== (length rows) 8) "maple-book: 8 rows"
check (<= (length (head rows)) 400) "maple-decompose: at most the atoms asked"
check (== (length (head (maple-book (maple-decompose x sr g (list 'atoms 20))))) 20) "maple-decompose: 'atoms caps the book"
check (> (maple-snr b) 10) "maple-snr: 400 atoms explain the notes well"
check (== (length (maple-residual b)) (length x)) "maple-residual: the sound's length"
check (== (maple-rate b) sr) "maple-rate"
var z (maple-synth b nil)
check (< (/ (sum (* (- (slice z 0 (length x)) x) (- (slice z 0 (length x)) x))) (sum (* x x))) 0.15) "maple-synth: the book rebuilds the sound"
check (> (length (maple-synth b (list 'stretch 2))) (* 1.9 (length x))) "maple-synth: stretch"
check (<= (max (abs (maple-synth b (list 'ratio 1.5)))) 0.99) "maple-synth: never above 0.99"
var few (maple-decompose x sr g (list 'atoms 5000 'snr 6))
check (and (>= (maple-snr few) 6) (< (length (head (maple-book few))) 5000)) "maple-decompose: stops at the SNR"

; --- decomposition with sounds, cross synthesis ---
var bs (maple-decompose x sr d (list 'components 3 'segments 'blocks 'block 100))
check (> (length (head (maple-book bs))) 0) "maple-decompose: with a dictionary of sounds"
check (all? (map (vec->list (getidx (maple-book bs) 3)) (function (k) (>= k 0))) identity) "maple-book: sound atoms carry their index"
var d2 (maple-cut x sr 'frames 50 50 "x")
check (> (length (maple-synth bs nil d2)) 0) "maple-synth: cross synthesis with another dictionary"
check (contains? (error-of (function () (maple-synth b nil d2))) "two dictionaries of sounds") "maple-synth: cross needs sounds"
check (> (length (maple-hybrid x sr y sr nil)) 0) "maple-hybrid"

; --- sound types ---
var m (soundtypes-analyse x sr (list 'types (list 6 3) 'levels 2 'size 1024 'hop 256))
check (equal? (type m) "opaque:soundtypes-model") "soundtypes-analyse: a model"
var counts (soundtypes-levels m)
check (== (length counts) 2) "soundtypes-levels: two levels"
check (>= (head counts) (last counts)) "soundtypes-levels: fewer units above"
var u1 (soundtypes-units m 1)
check (== (length u1) (head counts)) "soundtypes-units: one per unit"
check (and (== (head (head u1)) 0) (all? (map u1 (function (u) (< (head u) (getidx u 1)))) identity)) "soundtypes-units: start before end"
check (all? (map u1 (function (u) (< (last u) 6))) identity) "soundtypes-units: types below k"
check (== (length (soundtypes-labels m)) 2) "soundtypes-labels: a strip per level"
var tr (soundtypes-transitions m 1)
check (== (length tr) 6) "soundtypes-transitions: k rows"
check (contains? (error-of (function () (soundtypes-units m 3))) "level") "soundtypes-units: no level 3"
var gen (soundtypes-generate m 1 2 (list 'method 'oracle 'seed 3))
check (> (length (head gen)) (* 1.5 sr)) "soundtypes-generate: about the length asked"
check (equal? gen (soundtypes-generate m 1 2 (list 'method 'oracle 'seed 3))) "soundtypes-generate: the same seed, the same result"
check (> (length (last (soundtypes-generate m 1 2 (list 'method 'markov 'order 3)))) 0) "soundtypes-generate: markov"
check (> (length (last (soundtypes-generate m 2 2 (list 'method 'classic 'inner 0.5)))) 0) "soundtypes-generate: classic, a level 2 played by new units"
var my (soundtypes-analyse y sr (list 'size 1024 'hop 256))
check (> (length (soundtypes-match m my 1 nil)) 0) "soundtypes-match: unit by unit"
check (> (length (soundtypes-match m my 0 (list 'contour 0.5))) 0) "soundtypes-match: frame by frame"
var mb (soundtypes-analyse-book b (list 'types (list 4 2)))
check (> (head (soundtypes-levels mb)) 0) "soundtypes-analyse-book: the atoms of a book"
check (contains? (error-of (function () (soundtypes-match m mb 1 nil))) "analysed the same way") "soundtypes-match: a sound and a book do not match"

report "test_atoms"
