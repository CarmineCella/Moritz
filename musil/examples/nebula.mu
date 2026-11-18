; nebula: granular clouds (the engine of Moritz's nebula~)
; Usage: musil nebula.mu [sound.wav]   (defaults to the bundled data/ file)
;
; (nebula sr seconds opts) spawns grains for `seconds` and lets the last ones finish; => (list left right).
; A grain is a windowed snippet of two prototypes (waveforms or sounds) mixed by "morph"; it starts
; every 1/density seconds, lasts "length" ms, sounds at "freq" (Hz for a waveform, a speed ratio for
; a sound, unless its pitch is given), glides by up to "gliss" and is panned around "jitter". The
; "-random" options spread a parameter by a ratio of it; any parameter given as (list start end)
; ramps over the cloud. The same seed gives the same cloud, here and in nebula~.
load "system.mu"
load "signals.mu"

var w (read-wav (if (> (length args) 0) (getidx args 0) "../../data/Vox.wav"))
var sr (head w)
var x (head (getidx w 1))

function out (name y) {
    var path (concat "/tmp/musil_nebula_" name ".wav")
    write-wav path sr (normalize-peak-stereo y)
    print (pad-right name 14 " ") (fixed (/ (length (head y)) sr) 2) "s ->" path
}
var t0 (clock)
; sine grains, dense and short, spread in pitch and space
out "shimmer" (nebula sr 4 (list (list "density" 80) (list "density-random" 0.5) (list "length" 40) (list "length-random" 0.5)
                                (list "freq" 880) (list "freq-random" 0.3) (list "amp" 0.2) (list "seed" 1)))
; a sine cloud that turns into a square one, thinning out and falling
out "morph" (nebula sr 6 (list (list "proto1" "sine") (list "proto2" "square") (list "morph" (list 0 1))
                              (list "density" (list 60 8)) (list "length" (list 30 200)) (list "freq" (list 660 110))
                              (list "gliss" 0.2) (list "seed" 2)))
; grains of the voice: speed 1 keeps its pitch, the read position crawls at a quarter of the speed
out "voice" (nebula sr 8 (list (list "proto1" x) (list "freq" 1) (list "speed" 0.25) (list "density" 30)
                              (list "length" 120) (list "length-random" 0.3) (list "jitter-random" 1) (list "seed" 3)))
; the voice and a triangle at the same pitch: the voice's pitch given, so freq is in Hz for both
var f0 (acf-f0 (slice x (floor (/ (length x) 3)) 2048) sr)
out "hybrid" (nebula sr 6 (list (list "proto1" x) (list "pitch1" f0) (list "proto2" "triangle") (list "freq" 220)
                               (list "morph" (list 0 1)) (list "density" 40) (list "length" 80) (list "speed" 0.5) (list "seed" 4)))
; grainlets: each grain lasts 8 periods of its frequency, so low grains are long and high ones short
out "grainlets" (nebula sr 5 (list (list "grainlets" 8) (list "freq" 300) (list "freq-random" 0.8) (list "density" 50) (list "seed" 5)))
print "all in" (fixed (- (clock) t0) 2) "s; the voice's f0" (fixed f0 1) "Hz"
