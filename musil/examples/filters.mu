; filters: biquads on a real sound, judged by their effect on the spectral centroid
; Usage: musil filters.mu [sound.wav]   (defaults to the bundled data/ files)
load "system.mu"
load "signals.mu"

var w (read-wav (if (> (length args) 0) (getidx args 0) "../../data/cage.wav"))
var sr (head w)
var x (head (getidx w 1))
var n (next-pow2 (/ sr 2))
var freqs (spectrum-freqs n sr)
function centroid-of (sig) (fixed (spectral-centroid (magnitude-spectrum (take sig n)) freqs) 0)

print "original         centroid:" (centroid-of x) "Hz"
var lp (lowpass x sr 300 0.707)
print "lowpass 300      centroid:" (centroid-of lp) "Hz"
write-wav "/tmp/musil_lp.wav" sr lp

var hp (highpass x sr 1500 0.707)
print "highpass 1500    centroid:" (centroid-of hp) "Hz"
write-wav "/tmp/musil_hp.wav" sr hp

var bp (lowpass (highpass x sr 800 0.707) sr 1200 0.707)
print "bandpass 800-1200 centroid:" (centroid-of bp) "Hz (highpass then lowpass)"
write-wav "/tmp/musil_bp.wav" sr bp

var boosted (peak-eq x sr 2500 2 12)
print "peak +12 dB @2500 centroid:" (centroid-of boosted) "Hz"

; the state-variable filter: four responses of one filter (q 0..1 is the resonance), and a cutoff that
; moves (freq as a vector as long as the sound); lores, the resonant lowpass of Moritz's lores~
print "svf low 500      centroid:" (centroid-of (svf x sr 500 0.5 "low")) "Hz"
print "svf band 1000    centroid:" (centroid-of (svf x sr 1000 0.8 "band")) "Hz"
var sweep (svf x sr (+ 200 (* 3000 (/ (range (length x)) (length x)))) 0.85 "band")
write-wav "/tmp/musil_svf_sweep.wav" sr (normalize-peak sweep)
print "svf band sweep 200 -> 3200 Hz -> /tmp/musil_svf_sweep.wav"
print "lores 800 res .7 centroid:" (centroid-of (lores x sr 800 0.7)) "Hz"

; the coefficients themselves, and a filter's impulse response
var coeffs (biquad "lowpass" sr 300 0.707 0)
print "lowpass b:" (fixed (head coeffs) 5) " a:" (fixed (last coeffs) 5)
print "impulse response, first 8:" (fixed (take (apply-filter (impulse 64) coeffs) 8) 4)
