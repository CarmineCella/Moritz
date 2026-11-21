; test_signals.mu — self-checking test of the signals library (signals.h + signals.mu).
;
; Every check is a (check ...) from test.mu. A passing run prints only the final line.
; Run with: musil tests/test_signals.mu

load "test.mu"
load "signals.mu"

function near? (a b eps) (< (max (abs (- a b))) eps)
var sr 8000

; --- generators ---
var s440 (sine sr 440 0.1)
check (== (length s440) 800) "sine: length"
check (near? (rms s440) 0.7071 1e-3) "sine: rms"
check (== (length (noise 100)) 100) "noise: length"
check (all? (noise 100) (function (x) (between? x -1 1))) "noise: range"
check (equal? (impulse 4) (vec 1 0 0 0)) "impulse"
var tbl (gen 16 (vec 1))
check (== (length tbl) 17) "gen: n+1 samples"
check (== (head tbl) (last tbl)) "gen: guard point"
check (near? (max tbl) 1 1e-12) "gen: normalised"
check (near? (getidx tbl 4) 1 1e-12) "gen: sine table peak at a quarter"
check (equal? (mix (list (list 0 (vec 1 1)) (list 3 (vec 2 2)))) (vec 1 1 0 2 2)) "mix"
check (equal? (add-at (zeros 3) 1 (vec 5 5 5)) (vec 0 5 5 5)) "add-at: grows"
check (equal? (add-at (ones 4) 1 (vec 1 1)) (vec 1 2 2 1)) "add-at: adds"
check (equal? (fade-in (ones 4) 2) (vec 0 0.5 1 1)) "fade-in"
check (equal? (fade-out (ones 4) 2) (vec 1 1 0.5 0)) "fade-out"
check (equal? (normalize-peak (vec 1 -4 2)) (vec 0.25 -1 0.5)) "normalize-peak"
check (equal? (normalize-peak (zeros 3)) (zeros 3)) "normalize-peak: silence"
check (near? (rms (normalize-rms s440 0.1)) 0.1 1e-12) "normalize-rms"
check (== (amp->db 1) 0) "db"
check (near? (db->amp -6.0206) 0.5 1e-4) "undb"
check (near? (amp->db (db->amp -20)) -20 1e-12) "db round trip"

; --- oscillator (C++) ---
var o (osc 8 (+ (zeros 8) 1) (vec 0 1 0 -1 0))
check (equal? o (vec 0 0.5 1 0.5 0 -0.5 -1 -0.5)) "osc: linear interpolation through the table"
check (near? (acf-f0 (osc sr (+ (zeros 1600) 330) (gen 512 (vec 1))) sr) 333.3 1) "osc: pitch"
var two (oscbank sr (list (ones 100) (* 0.5 (ones 100))) (list (+ (zeros 100) 100) (+ (zeros 100) 200)) tbl)
check (== (length two) 100) "oscbank: length"
check (contains? (error-of (function () (oscbank sr (list (ones 2)) (list) tbl))) "same length") "oscbank: arity"
check (contains? (error-of (function () (osc 8 (vec 1) (vec 1)))) "at least 2") "osc: table too short"

; --- windows and spectra ---
check (equal? (fixed (hann 4) 6) (vec 0 0.5 1 0.5)) "hann: periodic"
check (near? (hamming 8) (vec 0.08 0.2147 0.54 0.8653 1 0.8653 0.54 0.2147) 1e-3) "hamming"
check (near? (head (blackman 8)) 0 1e-12) "blackman: starts at zero"
var x (sin (* tau 3 (/ (range 16) 16)))
var S (fft x)
check (== (length (head S)) 16) "fft: (list re im), power of two"
check (== (length (head (fft (ones 100)))) 128) "fft: zero-pads to the next power of two"
check (near? (getidx (magnitudes S) 3) 8 1e-12) "fft: energy at bin 3"
check (near? (ifft S) x 1e-12) "ifft: round trip"
seed 1
var r (noise 1024)
check (near? (ifft (fft r)) r 1e-12) "fft/ifft: round trip on noise"
check (contains? (error-of (function () (ifft (list (ones 3) (ones 3))))) "power of two") "ifft: size"
check (contains? (error-of (function () (ifft (list (ones 4))))) "(list re im)") "ifft: shape"
var P (car->pol (list (vec 1 0) (vec 0 1)))
check (equal? (head P) (vec 1 1)) "car->pol: magnitude"
check (near? (last P) (vec 0 (/ pi 2)) 1e-12) "car->pol: phase"
check (near? (head (pol->car P)) (vec 1 0) 1e-12) "pol->car: re"
check (near? (last (pol->car P)) (vec 0 1) 1e-12) "pol->car: im"
check (equal? (spectrum-freqs 8 8000) (vec 0 1000 2000 3000)) "spectrum-freqs"
var ms (magnitude-spectrum (* (sine sr 1000 0.032) (hann 256)))
check (== (length ms) 128) "magnitude-spectrum: half the bins"
check (== (getidx (spectrum-freqs 256 sr) (argmax ms)) 1000) "magnitude-spectrum: peak at the sine's frequency"
check (near? (max (magnitude-spectrum (sine sr 1000 0.032))) 1 1e-9) "magnitude-spectrum: full-scale sine reads 1"
check (equal? (fixed (conv (vec 1 2 3) (vec 1 1)) 10) (vec 1 3 5 3)) "conv"
check (equal? (fixed (conv (vec 1 2) (vec 3)) 10) (vec 3 6)) "conv: scalar kernel"
check (== (length (conv (ones 100) (ones 50))) 149) "conv: length"
; conv is C++ now (overlap-add for a long x): the same as the FFT convolution written in Musil it replaced
function conv-mu (x h) {
    var n (+ (length x) (length h) -1)
    return (take (ifft (complex-mul (fft (vec x (zeros (- n (length x))))) (fft (vec h (zeros (- n (length h))))))) n)
}
var cx (osc sr (+ (zeros 5000) 330) (gen 512 (vec 1 0.5 0.3)))
var ch (* (noise 37) (exp (* -0.1 (range 37))))
check (near? (conv cx ch) (conv-mu cx ch) 1e-9) "conv: a long x, overlap-add"
check (near? (conv ch cx) (conv-mu ch cx) 1e-9) "conv: a long h"
check (equal? (conv-fast cx ch) (conv cx ch)) "conv-fast is conv"
check (equal? (conv (vec 2) (vec 3)) (vec 6)) "conv: one sample each"
check (== (length (conv (vec) (vec 1 2))) 0) "conv: empty"
var cm (complex-mul (list (vec 0) (vec 1)) (list (vec 0) (vec 1)))
check (equal? (list (head cm) (last cm)) (list (vec -1) (vec 0))) "complex-mul: i * i = -1"

; --- cepstral envelope ---
var tone (osc sr (+ (zeros 1024) 200) (gen 512 (ones 12)))
var shaped (magnitudes (fft (* (bandpass tone sr 900 3) (hann 1024))))
var picks (function (m) (vec (map (list 26 51 77 102 128 154) (function (k) (getidx m k)))))
var env (spectral-envelope shaped 20)
check (== (length env) 1024) "spectral-envelope: full length"
check (< (spectral-irregularity (take env 512)) (* 0.3 (spectral-irregularity (take shaped 512)))) "spectral-envelope: smoother than the spectrum"
check (< (abs (- (argmax (take env 512)) 115)) 40) "spectral-envelope: peaks near the formant"
check (> (min (picks (/ env shaped))) 0.1) "spectral-envelope: stays within an order of magnitude of the harmonic peaks"
check (< (/ (max (picks (/ env shaped))) (min (picks (/ env shaped)))) 3) "spectral-envelope: peak-to-envelope ratio is consistent across harmonics"
; spectral-envelope is C++ now (signals/envelope.h, Moritz's specenv~ too): the same as the Musil it replaced
function spectral-envelope-mu (mags order) {
    var n (length mags)
    var lifter (vec (ones (+ order 1)) (zeros (- n (* 2 order) 1)) (ones order))
    function smooth (log-spec) (head (fft (* (ifft (list log-spec (zeros n))) lifter)))
    var target (log (max mags (* 1e-4 (max mags))))
    var e (smooth target)
    times 12 (function (k) (set e (smooth (max target e))))
    return (exp e)
}
check (near? (/ env (spectral-envelope-mu shaped 20)) (ones 1024) 1e-9) "spectral-envelope: the true envelope, as written in Musil"
check (contains? (error-of (function () (spectral-envelope (ones 100) 10))) "power of two") "spectral-envelope: a power of two"
var flat (flatten-spectrum shaped 20)
check (< (stdev (log (picks flat))) (* 0.5 (stdev (log (picks shaped))))) "flatten-spectrum: harmonics level out"
check (near? (impose-envelope shaped shaped 20) shaped 1e-9) "impose-envelope: a spectrum given its own envelope is unchanged"
check (== (length (cepstrum (ones 8))) 8) "cepstrum: length"
check (near? (cepstrum (ones 8)) (zeros 8) 1e-9) "cepstrum: flat spectrum has zero cepstrum"

; --- stft ---
var sig (sine sr 200 0.1)
var frames (stft sig 256 64)
check (== (length frames) 10) "stft: frame count, the last reaching the end (padded)"
check (== (length (head (head frames))) 256) "stft: frame size"
var y (istft frames 256 64)
check (== (length y) (+ (* 64 9) 256)) "istft: length"
check (>= (length y) (length sig)) "istft: at least as long as the input"
check (near? (slice y 256 300) (slice sig 256 300) 1e-12) "istft: exact reconstruction where frames fully overlap"
check (near? (slice y 16 (- (length sig) 16)) (drop sig 16) 1e-9) "istft: the whole input back, its end included"
check (== (length (stft (ones 100) 256 64)) 1) "stft: a signal shorter than a frame gives one frame"
check (equal? (istft (list) 256 64) (vec)) "istft: no frames"
check (== (length (head (stft-magnitudes frames))) 128) "stft-magnitudes"

; --- features ---
var a (vec 0 1 0 0)
var f (vec 0 100 200 300)
check (== (spectral-centroid a f) 100) "spectral-centroid"
check (== (spectral-centroid (zeros 4) f) 0) "spectral-centroid: silence"
check (== (spectral-spread a f) 0) "spectral-spread: single line"
check (near? (spectral-spread (vec 1 1) (vec 0 100)) 50 1e-12) "spectral-spread"
check (== (spectral-skewness a f) 0) "spectral-skewness: degenerate"
check (near? (spectral-kurtosis (vec 1 1) (vec 0 100)) 1 1e-12) "spectral-kurtosis: two equal lines"
check (== (spectral-flux (vec 2 0 3) (vec 1 1 1)) 3) "spectral-flux: rectified"
check (== (spectral-irregularity (vec 1 3 2)) 3) "spectral-irregularity"
check (== (spectral-decrease (vec 1 1 1)) 0) "spectral-decrease: flat"
check (< (spectral-decrease (vec 4 2 1)) 0) "spectral-decrease: falling"
check (near? (spectral-flatness (ones 8)) 1 1e-9) "spectral-flatness: white"
check (< (spectral-flatness (vec 1 0 0 0)) 0.01) "spectral-flatness: tonal"
check (== (spectral-rolloff (vec 1 1 1 1) (vec 0 1 2 3) 0.5) 1) "spectral-rolloff"
check (near? (hfc (vec 0 0 1)) (/ 2 3) 1e-12) "hfc: sum of a^2 i over sum of i"
check (near? (zcr s440) (/ 87 800) 1e-12) "zcr: 440 Hz at 8 kHz, 87 sign changes in 800 samples (the first sample, 0, counts as positive)"
check (== (zcr (ones 10)) 0) "zcr: no crossings"
check (== (zcr (vec 1 0 -1 0 1)) (/ 2 5)) "zcr: 0 counts as positive"
check (== (spectral-flatness (zeros 8)) 0) "spectral-flatness: silence is 0"
check (equal? (spectral-moments (vec 1 1) (vec 0 100)) (vec 50 50 0 1)) "spectral-moments: centroid spread skewness kurtosis"
check (== (spectral-flux (vec 2 0 3) 1) 3) "spectral-flux: a number as the previous frame"
check (contains? (error-of (function () (spectral-moments (vec 1 2) (vec 1)))) "same length") "spectral-moments: lengths"
check (near? (acf-f0 sig sr) 200 1e-9) "acf-f0: 200 Hz"
check (== (acf-f0 (noise 800) sr) 0) "acf-f0: noise is unvoiced"
check (== (acf-f0 (zeros 100) sr) 0) "acf-f0: silence"

; --- yin ---
function cents (f want) (abs (* 1200 (log2 (/ (max f 1e-9) want))))
each (list 100 220 440 1000 2000) (function (f) {
    var p (yin (sine sr f 0.128) sr)
    check (< (cents (head p) f) 0.5) (concat "yin: a sine at " (str f) " Hz, within half a cent")
    check (> (last p) 0.99) (concat "yin: a sine at " (str f) " Hz is periodic")
})
; harmonics with phases of their own, amps from the fundamental up
function harmonic-tone (f0 amps n) {
    var t (/ (range n) sr)
    var x (zeros n)
    for (var k 0) (< k (length amps)) (var k (+ k 1)) {
        var x (+ x (* (getidx amps k) (sin (+ (* tau (+ k 1) f0 t) k))))
    }
    return (* 0.3 x)
}
var weak (harmonic-tone 62.5 (vec 0.2 1 0.2 0.8 0.2 0.5) 512)
check (near? (acf-f0 weak sr) 125 1) "acf-f0: a weak fundamental and weak odd partials make it jump an octave up"
check (< (cents (head (yin weak sr)) 62.5) 1) "yin: the same tone keeps its octave"
var weak2 (harmonic-tone 80 (vec 0.25 1 0.25 0.8 0.25 0.5) 320)
check (near? (acf-f0 weak2 sr) 160 1) "acf-f0: four periods of a weak fundamental, an octave up"
check (< (cents (head (yin weak2 sr)) 80) 1) "yin: four periods are enough"
check (< (cents (head (yin (harmonic-tone 200 (vec 0 1 1 1 1) 1024) sr)) 200) 0.5) "yin: a missing fundamental"
check (< (cents (head (yin (harmonic-tone 100 (vec 0 0 1 1 1 1) 1024) sr)) 100) 0.5) "yin: the fundamental and its octave missing"
var saw (- (* 2 (- (* (range 1024) (/ 110 sr)) (floor (* (range 1024) (/ 110 sr))))) 1)
check (< (cents (head (yin saw sr)) 110) 2) "yin: a naive sawtooth at 110 Hz"
seed 7
var hiss (noise 2048)
check (equal? (head (yin hiss sr)) 0) "yin: noise is unvoiced"
check (< (last (yin hiss sr)) 0.5) "yin: noise is not periodic"
check (equal? (yin (zeros 1024) sr) (list 0 0)) "yin: silence"
check (equal? (head (yin (sine sr 50 0.25) sr 0.1 100)) 0) "yin: nothing under fmin"
var noisy (+ (sine sr 300 0.128) (* 0.3 (noise 1024)))
check (< (cents (head (yin noisy sr)) 300) 5) "yin: a sine in noise at threshold 0.1"
check (== (head (yin noisy sr 0.01)) 0) "yin: unvoiced at a stricter threshold"
check (contains? (error-of (function () (yin (vec 1 2 3) sr))) "too short") "yin: a frame too short"
check (contains? (error-of (function () (yin (zeros 1100000) sr))) "longer") "yin: a frame, not a whole sound"
check (contains? (error-of (function () (yin s440 sr 0))) "threshold") "yin: threshold in (0, 1]"
check (contains? (error-of (function () (yin s440 sr 0.1 500 400))) "fmin") "yin: fmin under fmax"
var track (yin-track (vec (sine sr 200 0.3) (zeros 2400)) sr 512 256)
check (== (length (head track)) 19) "yin-track: a frame every hop, the first at 0"
check (< (cents (getidx (head track) 4) 200) 0.5) "yin-track: the tone"
check (== (getidx (head track) 18) 0) "yin-track: the silence after it"
check (== (getidx (last track) 18) 0) "yin-track: no periodicity in silence"

; --- descriptors (as Moritz's descriptors~) ---
var dm (descriptors (sine sr 500 0.5) sr 1024 512)
check (== (length dm) 11) "descriptors: eleven rows"
check (== (length (head dm)) 8) "descriptors: a frame every hop"
check (near? (getidx (head dm) 3) 0.7071 1e-3) "descriptors: rms"
check (near? (getidx (getidx dm 1) 3) 500 2) "descriptors: the centroid of a sine is its frequency"
check (near? (getidx (getidx dm 7) 3) 1000 10) "descriptors: zcr, crossings a second"
check (< (cents (getidx (getidx dm 10) 3) 500) 0.5) "descriptors: f0"
check (> (getidx (getidx dm 9) 3) 0.99) "descriptors: periodicity"
check (< (getidx (getidx dm 8) 3) 0.01) "descriptors: a sine is not flat"
check (> (getidx (getidx dm 5) 0) (getidx (getidx dm 5) 3)) "descriptors: the flux of the first frame (from silence) is larger"
var frame (slice (sine sr 500 0.5) 1024 1024)
check (near? (getidx (getidx dm 10) 3) (head (yin frame sr)) 1e-9) "descriptors: f0 is yin of the frame"
check (== (length (autocorr (ones 10))) 5) "autocorr: n/2 lags"
check (contains? (error-of (function () (autocorr (vec 1 2)))) "too short") "autocorr: short"

; --- filters ---
var lp (biquad "lowpass" sr 1000 0.707 0)
check (== (length (head lp)) 3) "biquad: b"
check (== (length (last lp)) 3) "biquad: a"
check (near? (sum (head lp)) (sum (last lp)) 1e-12) "biquad lowpass: unity gain at DC"
var ap (biquad "allpass" sr 1000 0.707 0)
function alt-sum (v) (+ (- (getidx v 0) (getidx v 1)) (getidx v 2))
check (and (near? (sum (head ap)) (sum (last ap)) 1e-12) (near? (abs (alt-sum (head ap))) (abs (alt-sum (last ap))) 1e-12)) "biquad allpass: unity gain at DC and Nyquist"
var hp (biquad "highpass" sr 1000 0.707 0)
check (near? (sum (head hp)) 0 1e-12) "biquad highpass: zero gain at DC"
check (contains? (error-of (function () (biquad "weird" sr 1 1 0))) "unknown type") "biquad: unknown type"
; biquad is C++ now (signals/biquad.h, shared with Moritz and mir.h): the cookbook formulas as Musil wrote them
function biquad-mu (type sr f0 q gain-db) {
    if (or (> (length f0) 1) (> (length q) 1) (> (length gain-db) 1)) { error "biquad: a moving cutoff, q or gain is a streaming feature (a synth); offline they are single numbers" }
    var w0 (/ (* tau f0) sr)
    var c (cos w0)
    var s (sin w0)
    var alpha (/ s (* 2 q))
    var A (pow 10 (/ gain-db 40))
    var sA (sqrt A)
    if (equal? type "lowpass")  { return (list (vec (/ (- 1 c) 2) (- 1 c) (/ (- 1 c) 2)) (vec (+ 1 alpha) (* -2 c) (- 1 alpha))) }
    if (equal? type "highpass") { return (list (vec (/ (+ 1 c) 2) (- 0 (+ 1 c)) (/ (+ 1 c) 2)) (vec (+ 1 alpha) (* -2 c) (- 1 alpha))) }
    if (equal? type "bandpass") { return (list (vec alpha 0 (- 0 alpha)) (vec (+ 1 alpha) (* -2 c) (- 1 alpha))) }
    if (equal? type "notch")    { return (list (vec 1 (* -2 c) 1) (vec (+ 1 alpha) (* -2 c) (- 1 alpha))) }
    if (equal? type "peak")     { return (list (vec (+ 1 (* alpha A)) (* -2 c) (- 1 (* alpha A))) (vec (+ 1 (/ alpha A)) (* -2 c) (- 1 (/ alpha A)))) }
    if (equal? type "lowshelf") {
        return (list (vec (* A (+ (- (+ A 1) (* (- A 1) c)) (* 2 sA alpha))) (* 2 A (- (- A 1) (* (+ A 1) c))) (* A (- (- (+ A 1) (* (- A 1) c)) (* 2 sA alpha))))
                     (vec (+ (+ A 1) (* (- A 1) c) (* 2 sA alpha)) (* -2 (+ (- A 1) (* (+ A 1) c))) (- (+ (+ A 1) (* (- A 1) c)) (* 2 sA alpha))))
    }
    if (equal? type "highshelf") {
        return (list (vec (* A (+ (+ (+ A 1) (* (- A 1) c)) (* 2 sA alpha))) (* -2 A (+ (- A 1) (* (+ A 1) c))) (* A (- (+ (+ A 1) (* (- A 1) c)) (* 2 sA alpha))))
                     (vec (+ (- (+ A 1) (* (- A 1) c)) (* 2 sA alpha)) (* 2 (- (- A 1) (* (+ A 1) c))) (- (- (+ A 1) (* (- A 1) c)) (* 2 sA alpha))))
    }
    error "biquad: unknown type " type
}
each (list "lowpass" "highpass" "bandpass" "notch" "peak" "lowshelf" "highshelf") (function (t) {
    var c (biquad t 44100 1234 0.9 -4.5)
    var m (biquad-mu t 44100 1234 0.9 -4.5)
    check (and (near? (head c) (head m) 1e-12) (near? (last c) (last m) 1e-12)) (concat "biquad " t ": the cookbook's coefficients")
})
check (contains? (error-of (function () (biquad "lowpass" sr (vec 100 200) 1 0))) "streaming") "biquad: a moving cutoff is for a synth"
check (equal? (iir (vec 1 0 0 0) (vec 1) (vec 1 -0.5)) (vec 1 0.5 0.25 0.125)) "iir: one-pole decay"
check (equal? (iir (vec 1 0 0) (vec 2 2) (vec 2)) (vec 1 1 0)) "iir: a0 scaling"
check (contains? (error-of (function () (iir (vec 1) (vec 1) (vec 0)))) "nonzero") "iir: a0 zero"
var n1 (noise 4000)
var freqs (spectrum-freqs 4096 sr)
function centroid-of (v) (spectral-centroid (magnitude-spectrum v) freqs)
check (< (centroid-of (lowpass n1 sr 300 0.707)) (* 0.5 (centroid-of n1))) "lowpass lowers the centroid"
check (> (centroid-of (highpass n1 sr 2000 0.707)) (* 1.3 (centroid-of n1))) "highpass raises the centroid"
var notched (notch (sine sr 1000 0.5) sr 1000 5)
check (< (rms (drop notched 2000)) (* 0.1 (rms (sine sr 1000 0.5)))) "notch removes its frequency"
check (near? (sum (head (biquad "peak" sr 1000 1 0))) (sum (last (biquad "peak" sr 1000 1 0))) 1e-12) "peak with 0 dB is flat at DC"
check (equal? (take (comb (impulse 8) 2 0.5) 6) (vec 1 0 0.5 0 0.25 0)) "comb"
check (equal? (fixed (take (allpass (impulse 6) 2 0.5) 4) 6) (vec -0.5 0 0.75 0)) "allpass"
var dc (dc-block (+ (zeros 400) 1))
check (< (abs (last dc)) 0.15) "dc-block: constant decays"
check (== (head dc) 1) "dc-block: passes the step edge"
var ring (reson (impulse 10) sr 440 0.05)
check (== (length ring) 400) "reson: lasts tau seconds"
check (near? (acf-f0 (take ring 400) sr) 440 20) "reson: rings at its frequency"
var rev (schroeder-reverb (impulse 100) sr 0.5)
check (== (length rev) (+ 100 4000)) "schroeder-reverb: input plus rt60 seconds"
var early (amp->db (rms (slice rev 100 800)))
var late (amp->db (rms (slice rev 3300 800)))
check (< late (- early 30)) "schroeder-reverb: the tail decays"
check (> late -120) "schroeder-reverb: but is still there at the end"
check (equal? (delay (vec 1 2 3 4) 1) (vec 0 1 2 3)) "delay: integer"
check (equal? (delay (vec 1 2 3 4) 1.5) (vec 0 0.5 1.5 2.5)) "delay: fractional (interpolates from silence too)"
check (contains? (error-of (function () (delay (vec 1) -1))) ">= 0") "delay: negative"

; --- phase vocoder ---
check (equal? (local-maxima (vec 0 1 0 2 3 2 0)) (vec 1 4)) "local-maxima"
check (equal? (local-maxima (vec 1 2 3)) (vec)) "local-maxima: none interior"
check (equal? (gather (vec 10 20 30) (vec 2 0 -1)) (vec 30 10 30)) "gather"
check (contains? (error-of (function () (gather (vec 1) (vec 5)))) "out of range") "gather: range"
check (near? (princarg (vec 0 3.5 -3.5 7)) (vec 0 (- 3.5 tau) (- tau 3.5) (- 7 tau)) 1e-12) "princarg"
var tone (osc sr (+ (zeros 8000) 220) (gen 1024 (vec 1 0.5 0.3)))
var st (pvoc-stretch tone 2)
check (near? (length st) (* 2 (length tone)) 300) "pvoc-stretch: about twice as long"
check (near? (acf-f0 (slice st 6000 2048) sr) (acf-f0 (slice tone 2000 2048) sr) 2) "pvoc-stretch: pitch kept"
check (near? (slice (pvoc-stretch tone 1) 2048 4000) (slice tone 2048 4000) 1e-9) "pvoc-stretch: factor 1 is the identity"
check (near? (rms (slice st 4000 8000)) (rms tone) 0.1) "pvoc-stretch: level kept"
var st4 (pvoc-stretch tone 4)
var env4 (envelope-follow (slice st4 4000 24000) 2000)
check (< (/ (- (max env4) (min env4)) (mean env4)) 0.05) "pvoc-stretch x4: no amplitude modulation (fixed synthesis hop)"
check (near? (length (pvoc-pitch tone 1.5)) (length tone) 300) "pvoc-pitch: same length"
check (near? (acf-f0 (slice (pvoc-pitch tone 1.5) 2000 2048) sr) 333.3 3) "pvoc-pitch: a fifth up"
check (near? (acf-f0 (slice (pvoc-pitch tone 0.5) 2000 2048) sr) 111 2) "pvoc-pitch: an octave down"
check (near? (length (pvoc tone (list (list "stretch" (list 1 3))))) (* 1.5 (length tone)) 400) "pvoc: a ramped stretch 1->3 (the hop ramps linearly, as in sparkle: 1.5x overall)"
check (equal? (pvoc (vec) (list)) (vec)) "pvoc: empty input"
; the level of a pitch shift: each partial's lobe moved whole (resampling it bin by bin made +1.7 dB at 2, -4.2 at 0.5)
var lt (sine sr 440 2)
function db-ratio (a b) (* 20 (log10 (/ (rms (slice a 4000 8000)) (rms (slice b 4000 8000)))))
check (< (abs (db-ratio (pvoc-pitch lt 2) lt)) 0.5) "pvoc-pitch: an octave up keeps the level"
check (< (abs (db-ratio (pvoc-pitch lt 0.5) lt)) 0.5) "pvoc-pitch: an octave down keeps the level"
check (< (abs (db-ratio (pvoc-pitch lt 1.37) lt)) 0.5) "pvoc-pitch: any ratio keeps the level"
; the signal is padded with a window on both sides: an attack at the very start comes out whole, the end too
var at0 (pvoc lt (list))
check (> (/ (rms (take at0 400)) (rms (take lt 400))) 0.9) "pvoc: an attack at the start is kept (padded)"
check (> (/ (rms (slice at0 (- (length lt) 400) 400)) (rms (drop lt (- (length lt) 400)))) 0.9) "pvoc: the end is kept (padded)"
check (contains? (error-of (function () (pvoc tone (list (list "nope" 1))))) "unknown option") "pvoc: unknown option"
check (contains? (error-of (function () (pvoc tone (list (list "cross" (list 2 0.5 tone)))))) "needs an envelope") "pvoc: cross mode 2 needs an envelope"
var voiced (bandpass (osc sr (+ (zeros 8000) 200) (gen 512 (ones 12))) sr 900 3)
function formant-bin (v) (argmax (take (spectral-envelope (magnitudes (fft (* (slice v 2000 1024) (hann 1024)))) 20) 512))
function bin-mag (v k) (getidx (magnitude-spectrum (* (slice v 2000 2048) (hann 2048))) k)
var shifted (pvoc-pitch-formant voiced 1.5 100)
check (> (bin-mag shifted 77) (* 10 (+ 1e-9 (bin-mag shifted 51)))) "pvoc-pitch-formant: harmonics moved to 300 Hz (bin 77), none left at 200 (bin 51)"
function centroid-2k (v) (spectral-centroid (magnitude-spectrum (* (slice v 2000 2048) (hann 2048))) (spectrum-freqs 2048 sr))
check (< (abs (- (centroid-2k shifted) (centroid-2k voiced))) (* 0.3 (abs (- (centroid-2k (pvoc-pitch voiced 1.5)) (centroid-2k voiced))))) "pvoc-pitch-formant: the spectral balance stays where it was (the formants), not with the pitch"
check (> (abs (- (formant-bin (pvoc-pitch voiced 1.5)) (formant-bin voiced))) 15) "pvoc-pitch: formant moves without preservation"
check (< (formant-bin (pvoc-formants voiced 0.5 100)) (* 0.7 (formant-bin voiced))) "pvoc-formants: formants move down, pitch kept"
check (near? (acf-f0 (slice (pvoc-formants voiced 0.5 100) 2000 2048) sr) 200 3) "pvoc-formants: pitch kept"
check (equal? (gate-spectrum (vec 1 0.05 0.5) 0.1) (vec 1 0 0.5)) "gate-spectrum"
check (near? (length (denoise tone 0.05)) (length tone) 300) "denoise: length"
check (< (rms (denoise (* 0.01 (noise 8000)) 0.5)) 0.002) "denoise: a quiet noise below the threshold is removed"
check (near? (length (robotize tone)) (length tone) 300) "robotize: length"
check (near? (length (whisperize tone)) (length tone) 300) "whisperize: length"
var m0 (spectral-morph tone voiced 0 1024 256)
var m1 (spectral-morph tone voiced 1 1024 256)
check (near? (slice m0 2048 2000) (slice tone 2048 2000) 1e-9) "spectral-morph: t = 0 is the first sound"
check (near? (slice m1 2048 2000) (slice voiced 2048 2000) 1e-9) "spectral-morph: t = 1 is the second"
check (== (length (spectral-morph tone voiced 0.5 1024 256)) (length m0)) "spectral-morph: halfway"
var xs (pvoc-cross voiced tone 2 1 100)
check (near? (length xs) (length voiced) 300) "pvoc-cross: length"
check (> (rms xs) 0) "pvoc-cross mode 2: something comes out"
check (> (rms (pvoc-cross voiced tone 1 0.5 100)) 0) "pvoc-cross mode 1"
check (near? (slice (pvoc-cross voiced tone 3 0 100) 4096 2000) (slice (pvoc voiced (list)) 4096 2000) 1e-9) "pvoc-cross mode 3 at 0 is the first sound"

; --- nebula ---
var table (gen 1024 (vec 1 0.5 0.3))
var cloud (nebula sr 0.5 (list (list "seed" 3) (list "density" 40) (list "length" 30) (list "freq" 440)))
check (== (length cloud) 2) "nebula: stereo"
check (== (length (head cloud)) (length (last cloud))) "nebula: two channels as long"
check (>= (length (head cloud)) 4000) "nebula: at least the seconds asked (and the last grains' tails)"
check (> (rms (head cloud)) 0.01) "nebula: grains"
check (near? (head cloud) (head (nebula sr 0.5 (list (list "seed" 3) (list "density" 40) (list "length" 30) (list "freq" 440)))) 1e-12) "nebula: a seed gives the same cloud"
check (near? (acf-f0 (slice (head (nebula sr 0.5 (list (list "seed" 1) (list "density" 200) (list "length" 50) (list "freq" 400) (list "jitter" 1) (list "jitter-random" 0)))) 1000 2048) sr) 400 5) "nebula: grains at the frequency asked"
check (< (rms (last (nebula sr 0.3 (list (list "seed" 1) (list "jitter" 1) (list "jitter-random" 0))))) 1e-9) "nebula: jitter 1 is all left"
var snd (osc sr (+ (zeros 8000) 200) table)
check (> (rms (head (nebula sr 0.3 (list (list "proto1" snd) (list "seed" 2) (list "freq" 1))))) 0.01) "nebula: a sound as prototype"
check (== (length (head (nebula sr 0.3 (list (list "density" (list 5 50)) (list "seed" 2))))) (length (head (nebula sr 0.3 (list (list "density" (list 5 50)) (list "seed" 2)))))) "nebula: a ramped density"
check (contains? (error-of (function () (nebula sr 1 (list (list "proto1" "saw"))))) "waveform") "nebula: unknown waveform"
check (contains? (error-of (function () (nebula sr 1 (list (list "nope" 1))))) "unknown option") "nebula: unknown option"
; a square grain at 5 kHz (44.1 kHz): its harmonics above Nyquist are not in the table it reads, so nothing folds
; back (the 5th harmonic, 25 kHz, would alias to 19.1 kHz)
var sq (head (nebula 44100 0.5 (list (list "proto1" "square") (list "freq" 5000) (list "density" 2) (list "length" 400) (list "seed" 1))))
var sqm (magnitude-spectrum (* (slice sq 4000 8192) (hann 8192)))
function band-peak (m f) (max (slice m (- (round (/ (* f 8192) 44100)) 3) 7))
check (< (amp->db (/ (band-peak sqm 19100) (band-peak sqm 5000))) -60) "nebula: a high square grain does not alias"
check (> (amp->db (/ (band-peak sqm 15000) (band-peak sqm 5000))) -15) "nebula: its harmonics below Nyquist are kept"

; --- deconv, svf, lores ---
var dry (* (noise 2000) (hann 2000))
var ir (vec 1 0 0 0.5 0 0.25)
check (near? (deconv (take (conv dry ir) 2000) ir 0) dry 1e-6) "deconv: undoes conv (threshold 0: the plain inverse)"
check (near? (deconv (take (conv dry ir) 2000) ir) dry 1e-4) "deconv: undoes conv (the default regularisation)"
check (near? (deconv (vec 2 4 6 0) (vec 2) 1) (vec 0.5 1 1.5 0) 1e-9) "deconv: Tikhonov, half the inverse at threshold times the peak"
check (== (length (deconv (vec 1 2 3) (vec 1))) 3) "deconv: as long as y"
check (near? (deconv (vec 2 4 6 0) (vec 2) 0) (vec 1 2 3 0) 1e-12) "deconv: by a gain"
check (near? (deconv (vec 2 4 6 0) (vec 2)) (vec 1 2 3 0) 1e-5) "deconv: by a gain, regularised"
var wn (noise 4096)
check (< (centroid-of (svf wn sr 300 0.5 "low")) (* 0.5 (centroid-of wn))) "svf low lowers the centroid"
check (> (centroid-of (svf wn sr 2000 0.5 "high")) (centroid-of wn)) "svf high raises it"
check (near? (+ (svf wn sr 700 0.3 "low") (svf wn sr 700 0.3 "high")) (svf wn sr 700 0.3 "notch") 1e-9) "svf: notch = low + high"
check (near? (svf wn sr 700 0.3 "band") (svf wn sr (+ (zeros 4096) 700) (+ (zeros 4096) 0.3) "band") 1e-12) "svf: freq and q as vectors"
check (contains? (error-of (function () (svf wn sr 700 0.3 "comb"))) "type") "svf: unknown type"
; the band output is not normalised (svf~'s behaviour): its peak gain is 1/(2 - 1.98 q)
var s700 (sine sr 700 1)
function band-gain (q) (/ (rms (drop (svf s700 sr 700 q "band") 4000)) (rms (drop s700 4000)))
check (near? (band-gain 0) 0.5 0.01) "svf: band gain 0.5 at q 0"
check (near? (band-gain 0.5) (/ 1 1.01) 0.01) "svf: band gain about 1 at q 0.5"
check (near? (band-gain 1) 50 1) "svf: band gain 50 (+34 dB) at q 1"
; the recursive loops flush denormals to zero (x86 and AArch64): a decaying tail reaches 0 instead of crawling
; through the denormal numbers (0.5^1050 is one)
check (== (getidx (iir (impulse 1100) (vec 1) (vec 1 -0.5)) 1050) 0) "iir: no denormals in the tail"
check (> (getidx (iir (impulse 1100) (vec 1) (vec 1 -0.5)) 1000) 0) "iir: the tail itself is kept"
check (near? (lores wn sr 1000 0) (lowpass wn sr 1000 0.707) 1e-12) "lores: res 0 is a plain lowpass"
check (> (rms (lores (osc sr (+ (zeros 8000) 1000) table) sr 1000 1)) (* 5 (rms (lores (osc sr (+ (zeros 8000) 1000) table) sr 1000 0)))) "lores: res 1 resonates at the cutoff"

; --- hpss and onsets ---
var tone2 (* 0.5 (sin (* tau 220 (/ (range 16000) sr))))
var clicks2 (mix (map (list 0 4000 8000 12000) (function (p) (list p (* 0.8 (noise 200))))))
var mixed2 (+ tone2 (vec clicks2 (zeros (- 16000 (length clicks2)))))
var hp (hpss mixed2 512 128 17)
check (== (length (head hp)) 16000) "hpss: outputs as long as the input"
check (> (/ (dot (head hp) tone2) (* (norm (head hp)) (norm tone2))) 0.98) "hpss: the harmonic part is the tone"
check (> (rms (slice (last hp) 4000 200)) (* 20 (+ 1e-9 (rms (slice (last hp) 2000 1000))))) "hpss: the percussive part is the clicks"
check (< (max (abs (- (+ (head hp) (last hp)) mixed2))) 0.02) "hpss: the parts add up to the input (soft masks sum to one)"
check (contains? (error-of (function () (hpss mixed2 500 128 17))) "power of 2") "hpss: n must be a power of 2"
check (contains? (error-of (function () (hpss mixed2 512 128 2))) ">= 3") "hpss: kernel"
var on (onsets mixed2 sr 512 128 0.3)
check (== (length on) 4) "onsets: 0 first, then the three later clicks"
check (near? on (vec 0 0.5 1.0 1.5) 0.01) "onsets: at the samples where the hits start (refined inside the frame)"
var close-clicks (+ tone2 (vec (zeros 4000) (* 0.8 (noise 200)) (zeros 100) (* 0.8 (noise 200)) (zeros 11500)))
check (== (length (onsets close-clicks sr 512 128 0.3)) 2) "onsets: a second peak within n samples of the previous onset is not a new onset"
check (and (>= (length (onsets mixed2 sr 512 128 0.98)) 2) (< (length (onsets mixed2 sr 512 128 0.98)) 4)) "onsets: the threshold is relative to the flux maximum (0.98 keeps the strongest peak(s) only, after 0)"
check (== (length (onset-strength mixed2 512 128)) (length (stft mixed2 512 128))) "onset-strength: one value per frame"
check (equal? (onsets (zeros 4000) sr 512 128 0.3) (vec 0)) "onsets: silence has only the start"
var quiet-loud (+ tone2 (vec (zeros 4000) (* 0.1 (noise 200)) (zeros 3800) (* 0.9 (noise 200)) (zeros 7800)))
check (== (length (onsets quiet-loud sr 512 128 0.3)) 2) "onsets: a global threshold misses the quiet hit"
check (== (length (onsets-adaptive quiet-loud sr 512 128 3 9)) 3) "onsets-adaptive: the local median finds it"
check (equal? (onsets-adaptive (zeros 4000) sr 512 128 3 9) (vec 0)) "onsets-adaptive: silence"
check (== (length (segments mixed2 sr on)) 4) "segments: one per onset, from 0 to the end"
check (== (sum (vec (map (segments mixed2 sr on) length))) 16000) "segments: tile the whole sound"

; --- nmf separation ---
seed 3
var env1 (bpf 0 (list (list 4000 1) (list 4000 0) (list 4000 1) (list 4000 0)))
var env2 (bpf 1 (list (list 8000 0) (list 8000 1)))
var pa (* env1 (+ (sin (* tau 220 (/ (range 16000) sr))) (* 0.5 (sin (* tau 440 (/ (range 16000) sr))))))
var pb (* env2 (+ (sin (* tau 330 (/ (range 16000) sr))) (* 0.5 (sin (* tau 990 (/ (range 16000) sr))))))
var sep (nmf-separate (+ pa pb) 512 128 2 60)
var srcs (head sep)
check (== (length srcs) 2) "nmf-separate: k sources"
check (== (length (head srcs)) 16000) "nmf-separate: as long as the input"
check (< (max (abs (- (+ (head srcs) (last srcs)) (+ pa pb)))) 1e-6) "nmf-separate: the sources add up to the input"
function corr (p q) (/ (dot p q) (* (norm p) (norm q)))
var c1 (max (corr (head srcs) pa) (corr (last srcs) pa))
var c2 (max (corr (head srcs) pb) (corr (last srcs) pb))
check (and (> c1 0.8) (> c2 0.8)) "nmf-separate: each source matches one of the two mixed sounds"
check (equal? (mat-shape (getidx sep 1)) (list 257 2)) "nmf-separate: W is bins x k"
check (== (nrows (getidx sep 2)) 2) "nmf-separate: H is k x frames"
var learnt (nmf-learn-parts (list pa pb) 512 128 2 40)
check (equal? (mat-shape (head learnt)) (list 257 4)) "nmf-learn-parts: two parts per example, side by side"
check (equal? (last learnt) (list (list 0 1) (list 2 3))) "nmf-learn-parts: the groups"
var sup (nmf-separate-with (+ pa pb) 512 128 (head learnt) (last learnt) 40)
check (== (length (head sup)) 2) "nmf-separate-with: one source per group"
check (and (> (corr (head (head sup)) pa) 0.85) (> (corr (last (head sup)) pb) 0.85)) "nmf-separate-with: supervised, each source is its instrument"
check (< (max (abs (- (+ (head (head sup)) (last (head sup))) (+ pa pb)))) 1e-6) "nmf-separate-with: the sources add up to the mix"
check (== (nrows (last sup)) 4) "nmf-separate-with: the activations, one row per part"

; --- morphology descriptors ---
var m1 (vec (* 0.5 (sine sr 220 1)) (* (noise sr) (exp (* -20 (/ (range sr) sr)))) (zeros sr) (* 0.3 (sine sr 880 1)))
var spm (frame-spectra m1 2048 512 1024)
check (== (length (head spm)) 1024) "frame-spectra: ncoeff bins per frame"
check (> (spectral-similarity (head spm) (getidx spm 1)) 0.9) "spectral-similarity: a held tone's frames are alike"
check (< (spectral-similarity (getidx spm 5) (getidx spm 20)) 0.6) "spectral-similarity: a tone and noise are not"
var erm (event-rate m1 sr 1 512)
check (== (length erm) 3) "event-rate: times, rates, and the events kept"
check (and (> (length (last erm)) 0) (< (length (last erm)) 8)) "event-rate: a few events, not the vibrato's flux peaks"
var fp (flux-peaks m1 sr 1024 256 2 21)
check (and (>= (length fp) 1) (<= (length fp) 8)) "flux-peaks: the noise burst and the tones' starts, not the vibrato"
check (all? (map (vec->list (diff fp)) (function (d) (>= d 0.05))) identity) "flux-peaks: at least 50 ms apart"
var pd (peak-density (vec 0.5 0.6 0.7 2.5) 4 1 0.5)
check (== (length pd) 8) "peak-density: one value per step"
check (and (== (getidx pd 1) 3) (== (getidx pd 5) 1) (== (getidx pd 7) 0)) "peak-density: the count within the window, per second"
var pe (polyphony-estimate spm)
check (and (== (length pe) (length spm)) (>= (min pe) 0)) "polyphony-estimate: one value per frame"
var rcm (register-curve spm sr 2048)
check (== (length rcm) 3) "register-curve: centroids, spreads, lows"
check (near? (getidx (last rcm) 5) 57 1) "register-curve: the lowest partial of A3 is A3"
check (< (getidx (head rcm) 5) (getidx (head rcm) (- (length (head rcm)) 5))) "register-curve: the centroid rises with the octave"
var quiet (vec (* 0.5 (sine sr 440 0.5)) (* 1e-5 (noise sr)))                    ; a tone, then noise far below -60 dB
var qsp (frame-spectra quiet 1024 256 512)
var qrc (register-curve qsp sr 1024)
var qc (head qrc)
check (and (== (getidx qc -1) (getidx qc -20)) (near? (getidx qc -1) 69 1.5)) "register-curve: below -60 dB the frames keep the values of the tone before"
check (== (last (polyphony-estimate qsp)) 0) "polyphony-estimate: 0 below -60 dB"
check (== (length (median-filter (head qrc) 5)) (length (head qrc))) "register-curve: smoothed with median-filter"
var lc (loudness-curve m1 sr 512)
check (< (getidx lc 40) (getidx lc 5)) "loudness-curve: silence is quieter"
var ctm (coherence-time spm 512 sr 0.75)
check (> (getidx ctm 2) 0.5) "coherence-time: a held tone persists"
check (< (getidx ctm 40) 0.2) "coherence-time: silence has none"
var pk (spectral-peaks (getidx spm 5) sr 2048 3)
check (near? (min (head pk)) 220 2) "spectral-peaks: the fundamental, refined by a parabola"

; --- convolution reverb ---
var imp (vec 1 (zeros 100))
var cv (converb imp (vec 0.5 0 0.25) 1 1)
check (== (length cv) 1) "converb: a mono input and a mono response give one channel"
check (near? (take (head cv) 4) (vec 1.5 0 0.25 0) 1e-9) "converb: dry plus wet"
check (== (length (converb imp (list (vec 1) (vec 0.5)) 1 1)) 2) "converb: one response per channel spreads a mono input"
check (== (length (head (converb imp (vec 1 0 0) 0 1))) 103) "converb: as long as the input plus the response (n + m - 1)"
var hall (concerthall (sine sr 440 0.1) sr 0.7 0.3)
check (== (length hall) 2) "concerthall: stereo"
check (near? (conv-fast (vec 1 0 0.5) (vec 1 1)) (vec 1 1 0.5 0.5) 1e-12) "conv-fast: a small case"
var cx (noise 5000)
var ch (noise 300)
check (< (max (abs (- (conv cx ch) (conv-fast cx ch)))) 1e-9) "conv-fast: equals conv (overlap-add over blocks)"
check (> (length (head hall)) (* 2 sr)) "concerthall: the tail of the Concertgebouw follows"
check (> (rms (take (head hall) 800)) 0.2) "concerthall: the dry part is there"
check (near? (sqrt (sum (* (head (hall-ir 44100)) (head (hall-ir 44100))))) 1 0.02) "hall-ir: the response has unit energy at its own rate (the wet part keeps the level)"
check (contains? (find-file "hall_concertgebouw.wav") "hall_concertgebouw") "the hall's response ships with the libraries"

; --- spatial ---
check (equal? (fixed (ambi-gains 1 0 0) 6) (vec 1 0 0 1)) "ambi-gains: front is (W 0 0 X)"
check (equal? (fixed (ambi-gains 1 90 0) 6) (vec 1 1 0 0)) "ambi-gains: left is Y"
check (equal? (fixed (ambi-gains 1 0 90) 6) (vec 1 0 1 0)) "ambi-gains: up is Z"
check (== (length (ambi-gains 3 20 10)) 16) "ambi-gains: (order+1)^2 channels"
check (near? (ambi-gains 1 30 20) (vec 1 (* (cos (/ (* 20 pi) 180)) (sin (/ (* 30 pi) 180))) (sin (/ (* 20 pi) 180)) (* (cos (/ (* 20 pi) 180)) (cos (/ (* 30 pi) 180)))) 1e-9) "ambi-gains: SN3D first order is (1 y z x)"
check (contains? (error-of (function () (ambi-gains 9 0 0))) "0..7") "ambi-gains: order bounds"
var sx (sine sr 440 0.05)
check (== (length (ambi-encode sx 2 30 0)) 9) "ambi-encode: second order, nine channels"
check (near? (getidx (ambi-rotate (ambi-encode sx 1 90 0) -90) 3) (getidx (ambi-encode sx 1 0 0) 3) 1e-9) "ambi-rotate: turning a left source by -90 puts it in front"
check (near? (getidx (ambi-rotate (ambi-encode sx 3 40 15) 25) 9) (getidx (ambi-encode sx 3 65 15) 9) 1e-9) "ambi-rotate: exact at third order"
check (contains? (error-of (function () (ambi-rotate (list sx sx sx) 10))) "(order+1)^2") "ambi-rotate: channel count"
check (equal? (speaker-ring 4) (vec 0 90 180 -90)) "speaker-ring"
var dec (vec (map (ambi-decode (ambi-encode sx 1 45 0) (speaker-ring 4)) rms))
check (and (near? (getidx dec 0) (getidx dec 1) 1e-6) (> (getidx dec 0) (* 2 (getidx dec 2)))) "ambi-decode: a source at 45 feeds the front and left speakers equally, the others less"
; the energy vector of a source at a speaker, (sum g^2 u) / (sum g^2): its theoretical values with max-rE on a ring
function energy-vector (o ring) {
    var gs (vec (map (ambi-decoder o ring) (function (row) (dot row (ambi-gains o 0 0)))))
    var e (* gs gs)
    var ex (sum (* e (cos (* ring (/ pi 180)))))
    var ey (sum (* e (sin (* ring (/ pi 180)))))
    return (/ (sqrt (+ (* ex ex) (* ey ey))) (sum e))
}
check (near? (energy-vector 1 (speaker-ring 4)) 0.7071 1e-3) "ambi-decoder: a ring at order 1 has the energy vector of max-rE (0.71)"
check (near? (energy-vector 2 (speaker-ring 6)) 0.8660 1e-3) "ambi-decoder: order 2 on six speakers, 0.87"
check (== (getidx (head (ambi-decoder 1 (speaker-ring 4))) 2) 0) "ambi-decoder: a ring does not hear the height channel"
check (!= (getidx (getidx (ambi-decoder 1 (list (list 0 0) (list 90 40) (list 0 -30))) 1) 2) 0) "ambi-decoder: speakers with elevations sample the sphere"
check (== (length binaural-speakers) 17) "binaural-decode: seventeen virtual speakers"
; irregular layouts and too few speakers: the energy-preserving decoder, a source equally loud from every direction
function src-energy (o spk az el) { var gs (vec (map (ambi-decoder o spk) (function (row) (dot row (ambi-gains o az el)))))
                                    return (sum (* gs gs)) }
function energy-spread-db (o spk el) {
    var es (vec (map (vec->list (* 15 (range 24))) (function (az) (src-energy o spk az el))))
    return (* 10 (log10 (/ (max es) (min es))))
}
var five (vec 0 30 -30 110 -110)
check (< (energy-spread-db 1 five 0) 1) "ambi-decoder: 5.0 at order 1, every direction about as loud (EPAD)"
check (< (abs (* 10 (log10 (/ (src-energy 1 five 0 0) (src-energy 1 five 180 0))))) 1) "ambi-decoder: 5.0, front and back balanced"
check (< (energy-spread-db 2 (speaker-ring 4) 0) 1) "ambi-decoder: order 2 on four speakers (too few), within 1 dB (1.5 with the regular formula)"
check (< (energy-spread-db 2 five 0) 0.1) "ambi-decoder: 5.0 at order 2 (13.5 dB with the regular formula)"
check (< (energy-spread-db 1 binaural-speakers 0) 1) "ambi-decoder: the 17 directions of binaural-decode, even around"
check (< (energy-spread-db 1 binaural-speakers 40) 1) "ambi-decoder: the 17 directions, even above"
check (< (energy-spread-db 3 binaural-speakers 0) 0.1) "ambi-decoder: the 17 directions at order 3, even (sampling the sphere was 0.4 dB off)"
check (near? (head (ambi-decoder 2 (speaker-ring 6))) (head (ambi-decoder 2 (vec 0 -60 -120 180 120 60))) 1e-12) "ambi-decoder: a regular ring in any order is the exact decoder"
var pn (vec (map (pan-n sx (speaker-ring 8) 100) rms))
check (and (> (getidx pn 2) (getidx pn 3)) (== (getidx pn 0) 0) (== (getidx pn 5) 0)) "pan-n: between the two nearest speakers only"
check (near? (vec (map (pan-azimuth sx 90) rms)) (vec (rms sx) 0) 1e-6) "pan-azimuth: 90 is the left"
check (near? (vec (map (pan-azimuth sx -90) rms)) (vec 0 (rms sx)) 1e-6) "pan-azimuth: -90 is the right"
check (near? (vec (map (pan-azimuth sx 0) rms)) (* (rms sx) (vec 0.7071 0.7071)) 1e-3) "pan-azimuth: front is equal power"
var h (hrir 90 0 sr)
check (== (hrtf-loaded) 710) "hrtf: the bundled KEMAR set loads on first use (710 directions)"
check (== (length h) 2) "hrir: two ears"
check (== (length (head (hrir 30 0 44100))) 128) "hrir: 128 taps at 44100"
check (== (length (head (hrir 30 0 48000))) 139) "hrir: resampled to another rate"
check (> (rms (head h)) (* 2 (rms (last h)))) "hrir: a left source is louder at the left ear"
check (> (argmax (abs (last h))) (argmax (abs (head h)))) "hrir: and arrives later at the right ear"
var hm (hrir-model 90 0 sr)
check (near? (* 1000 (/ (- (argmax (abs (last hm))) (argmax (abs (head hm)))) sr)) 0.66 0.15) "hrir-model: the interaural delay is about 0.65 ms"
var hf (magnitude-spectrum (head (hrir 0 0 44100)))
var hb (magnitude-spectrum (head (hrir 180 0 44100)))
var fr (take (spectrum-freqs 128 44100) (length hf))
check (> (spectral-centroid hf fr) (* 1.15 (spectral-centroid hb fr))) "hrtf: behind is duller than in front (the pinna): a cue the model cannot give"
check (not (equal? (head (hrir 0 0 44100)) (head (hrir 0 60 44100)))) "hrtf: elevation changes the response"
hrtf-unload
check (== (hrtf-loaded) 0) "hrtf-unload: back to the model"
check (> (length (head (hrir 90 0 44100))) 128) "hrir: the model when nothing is loaded"
check (== (hrtf-load "hrtf_kemar.csv") 710) "hrtf-load: explicit"
check (contains? (error-of (function () (hrtf-load "nope.csv"))) "cannot open") "hrtf-load: missing file"
var click (vec (noise 400) (zeros 2000))
var bl (vec (map (binaural click 90 0 sr) rms))
check (> (getidx bl 0) (* 2 (getidx bl 1))) "binaural: a left click is much louder in the left ear"
var bnz (* 0.3 (noise sr))
var bf (vec (map (binaural-decode (ambi-encode bnz 1 0 0) sr) rms))
var bd (vec (map (binaural bnz 0 0 sr) rms))
check (and (near? (getidx bf 0) (getidx bf 1) 1e-6) (near? (/ (getidx bf 0) (getidx bd 0)) 1 0.05)) "binaural-decode: a front source is equal in both ears, at the level of the front head response"
var br (vec (map (binaural-decode (ambi-encode click 1 -90 0) sr) rms))
check (> (getidx br 1) (* 1.5 (getidx br 0))) "binaural-decode: a right source is louder at the right ear"
check (== (length (moving-source sx 1 (vec 0 90 180) (vec 0 0 0) 800)) 4) "moving-source: B-format"
; the blocks are crossfaded: a still source is ambi-encode, a moving one glides without a step at the block edges
check (all? (zip (moving-source (ones 1024) 1 (vec 30 30 30 30) (vec 10 10 10 10) 256) (ambi-encode (ones 1024) 1 30 10)) (function (p) (near? (head p) (last p) 1e-12))) "moving-source: a still source is encoded as one"
check (< (max (abs (diff (getidx (moving-source (ones 512) 1 (vec 0 90) (vec 0 0) 256) 1)))) 0.02) "moving-source: no click where the direction changes"
check (equal? (channel (list sx (* 2 sx)) 1) (* 2 sx)) "channel"
check (equal? (channel sx 1) sx) "channel: a vector is its own channel"
check (== (max (head (normalize-peak-stereo (list (* 3 sx) sx)))) 1) "normalize-peak-stereo"

; --- resampling ---
var slow (sine sr 100 0.02)
check (== (length (resample slow 2)) 320) "resample: length"
check (near? (drop (take (resample slow 2) 300) 40) (drop (take (sine (* 2 sr) 100 0.02) 300) 40) 1e-3) "resample x2 matches the sine at the higher rate"
check (near? (drop (take (resample slow 1.5) 200) 40) (drop (take (sine (* 1.5 sr) 100 0.02) 200) 40) 1e-3) "resample x1.5: any ratio"
check (near? (slice (resample (ones 400) 0.7) 130 4) (ones 4) 1e-9) "resample: a constant stays a constant (away from the edges)"
check (near? (acf-f0 (resample (sine sr 1000 0.5) 0.5) 4000) 1000 1) "resample: downsampling keeps a tone below the new Nyquist"
check (== (length (resample-to slow sr 4000)) 80) "resample-to"
check (contains? (error-of (function () (resample slow 0))) "> 0") "resample: factor"

; --- channels ---
check (equal? (interleave (list (vec 1 2) (vec 3 4))) (vec 1 3 2 4)) "interleave"
check (equal? (deinterleave (vec 1 3 2 4) 2) (list (vec 1 2) (vec 3 4))) "deinterleave"
check (contains? (error-of (function () (interleave (list (vec 1) (vec 1 2))))) "same length") "interleave: lengths"
check (contains? (error-of (function () (deinterleave (vec 1 2 3) 2))) "multiple") "deinterleave: length"

; --- envelopes ---
check (equal? (bpf 0 (list (list 4 1))) (vec 0 0.25 0.5 0.75)) "bpf: one segment, end excluded"
check (equal? (bpf 0 (list (list 2 1) (list 2 0))) (vec 0 0.5 1 0.5)) "bpf: two segments"
check (equal? (bpf 5 (list)) (vec)) "bpf: no segments"
check (contains? (error-of (function () (bpf 0 (list (list 0 1))))) ">= 1") "bpf: zero-length segment"
check (equal? (envelope-follow (vec 1 1 -1 -1 0 0) 2) (vec 1 1 0)) "envelope-follow"
check (equal? (envelope-from-values (vec 0 1 0) 2) (vec 0 0.5 1 0.5)) "envelope-from-values"

; --- music information retrieval: chroma, key, tempo, beats, structure ---
var msr 22050
var amaj (+ (sine msr 220 4) (sine msr 277.18 4) (sine msr 329.63 4))          ; A major: A3 C#4 E4
var Cg (chromagram amaj msr 4096 1024)
check (== (length Cg) 12) "chromagram: twelve rows"
check (== (length (head Cg)) (+ 1 (floor (/ (length amaj) 1024)))) "chromagram: a frame every hop, the first at 0"
var prof (chroma-mean Cg)
check (all? (list 9 1 4) (function (k) (> (getidx prof k) 0.8))) "chromagram: A, C# and E the strong pitch classes of an A major chord"
check (all? (list 0 2 3 5 6 7 8 10 11) (function (k) (< (getidx prof k) 0.2))) "chromagram: the others weak"
check (near? (max prof) 1 1e-9) "chromagram: frames scaled to a maximum of 1"
check (> (getidx prof 1) (getidx prof 0)) "chromagram: C# above C"
var ptone (chroma-mean (chromagram (sine msr 440 1) msr 4096 1024))
check (and (near? (getidx ptone 9) 1 1e-9) (< (max-of (map (filter (vec->list (range 12)) (function (k) (!= k 9))) (function (k) (getidx ptone k)))) 0.05)) "chromagram: a pure tone on its class only (the peaks, not the window's lobe)"
var harm (mix (map (vec->list (range 1 8)) (function (k) (list 0 (* (/ 0.5 k) (sine msr (* 110 k) 1))))))
var hprof (chroma-mean (chromagram harm msr 4096 1024))
check (and (near? (getidx hprof 9) 1 1e-9) (< (getidx hprof 4) 0.1) (< (getidx hprof 1) 0.1)) "chromagram: the 3rd and 5th partials of a tone count little (no fifth, no third)"
check (== (max (chroma-mean (chromagram (zeros 8192) msr 4096 1024))) 0) "chromagram: silence stays 0"
if (not (equal? (take (key-estimate prof) 2) (list "A" 'major))) { print "key-estimate differs: profile" prof "scores" (take (key-scores prof) 4) }
check (equal? (take (key-estimate prof) 2) (list "A" 'major)) "key-estimate: A major"
check (== (length (key-scores prof)) 24) "key-scores: the 24 keys"
check (>= (last (head (key-scores prof))) (last (getidx (key-scores prof) 1))) "key-scores: best first"
var amin (+ (sine msr 220 4) (sine msr 261.63 4) (sine msr 329.63 4))          ; A minor: A3 C4 E4
check (equal? (take (key-estimate (chroma-mean (chromagram amin msr 4096 1024))) 2) (list "A" 'minor)) "key-estimate: A minor"
check (equal? (key-name (list "D" 'minor 0.5)) "D minor") "key-name"
check (equal? (rotate-vec (vec 1 2 3) 1) (vec 3 1 2)) "rotate-vec"
check (near? (correlation (vec 1 2 3) (vec 2 4 6)) 1 1e-9) "correlation: 1 for a scaled copy"
check (== (correlation (vec 1 1 1) (vec 1 2 3)) 0) "correlation: 0 for a constant"
check (== (length (chroma-mean Cg)) 12) "chroma-mean"
; a click train at 120 bpm: tempo, beats
var mfps (/ msr 256)
var train (clicks (* 0.5 (range 20)) msr 10)                                     ; a click every half second
check (== (length train) (* 10 msr)) "clicks: the length asked for"
check (> (max train) 0.5) "clicks: audible"
var tflux (onset-strength train 1024 256)
check (near? (tempo-estimate tflux mfps) 120 1) "tempo-estimate: 120 bpm from a click train"
check (near? (tempo-estimate-with tflux mfps 60 0.3) 60 1) "tempo-estimate-with: a prior on the slow side takes the half (a click train is periodic there too)"
var bt (beat-track tflux mfps 120 100)
check (> (length bt) 15) "beat-track: the beats found"
var bts (/ (+ (* bt 256) 512) msr)                                               ; a frame's time: the middle of its window
check (< (max (abs (- bts (* 0.5 (round (* 2 bts)))))) 0.03) "beat-track: on the clicks (within 30 ms)"
check (near? (mean (diff bts)) 0.5 0.01) "beat-track: the period kept"
var bb (beats train msr 1024 256)
check (near? (head bb) 120 1) "beats: the bpm"
check (near? (mean (diff (last bb))) 0.5 0.01) "beats: the period"
check (< (max (abs (- (last bb) (* 0.5 (round (* 2 (last bb))))))) 0.03) "beats: on the clicks"
var tg (tempogram tflux mfps 128)
check (== (length tg) 271) "tempogram: a row per bpm from 30 to 300"
check (== (length (head tg)) (length tflux)) "tempogram: a column per frame"
var mid (floor (/ (length tflux) 2))
var tcol (vec (map tg (function (r) (getidx r mid))))
check (near? (argmax tcol) 90 1) "tempogram: 120 bpm the maximum in the middle of the train"
check (and (> (getidx tcol 90) 0.3) (<= (max tcol) 1.000001)) "tempogram: divided by the energy (lag 0), at most 1"
check (< (getidx tcol 50) (* 0.7 (getidx tcol 90))) "tempogram: 80 bpm weaker"
var tg4 (tempogram tflux mfps 128 (/ mfps 4))
check (== (length (head tg4)) (ceil (/ (length tflux) (round (/ mfps 4))))) "tempogram: a column every step frames"
check (== (max (vec (map (tempogram (zeros 400) mfps 128) max))) 0) "tempogram: no pulse stays dark"
; structure: two different textures
var two (mix (list (list 0 (+ (sine msr 220 4) (sine msr 330 4))) (list (* 4 msr) (* 0.5 (noise (* 4 msr))))))
var mf (mfcc-gram two msr 2048 512 40 13)
check (== (length mf) 13) "mfcc-gram: ncoeff rows"
check (== (length (head mf)) (+ 1 (floor (/ (length two) 512)))) "mfcc-gram: a column per frame"
var ms (mel-spectrogram two msr 2048 512 32)
check (== (length ms) 32) "mel-spectrogram: nmel rows"
check (> (getidx (getidx ms 2) 10) (getidx (getidx ms 30) 10)) "mel-spectrogram: a 220 Hz chord is low, not high"
var mfs (mel-spectrogram (sine msr 1000 1) msr 2048 512 40)
check (near? (max (vec (map mfs (function (r) (getidx r 10))))) 0 3) "mel-spectrogram: a full-scale sine is about 0 dB"
check (== (max (vec (map (mel-spectrogram (zeros 4096) msr 2048 512 40) max))) -100) "mel-spectrogram: silence at -100 dB"
var Sm (self-similarity (drop mf 1))
check (== (length Sm) (length (head mf))) "self-similarity: frames x frames"
check (near? (getidx (head Sm) 0) 1 1e-9) "self-similarity: 1 on the diagonal"
check (near? (getidx (getidx Sm 5) 10) (getidx (getidx Sm 10) 5) 1e-12) "self-similarity: symmetric"
check (> (getidx (getidx Sm 10) 20) (getidx (getidx Sm 10) 300)) "self-similarity: a frame nearer its own texture than the other"
var nv (foote-novelty Sm 32)
check (== (length nv) (length Sm)) "foote-novelty: a value per frame"
var half (floor (/ (length Sm) 2))
check (near? (argmax nv) half 8) "foote-novelty: the peak at the change of texture"
check (and (== (head nv) 0) (== (last nv) 0)) "foote-novelty: 0 at the ends, where less than half the kernel fits"
check (<= (max nv) 2) "foote-novelty: a difference of two mean similarities (at most 2)"
var bds (novelty-peaks nv (/ msr 512) 2 31)
check (and (== (length bds) 1) (near? (head bds) 4 0.3)) "novelty-peaks: the one boundary, at 4 s"
var bs (beat-sync (list (vec 1 2 3 4 5 6) (vec 0 0 0 1 1 1)) (vec 0 0.5) 4)    ; fps 4: beats at frames 0 and 2
check (equal? bs (list (vec 1.5 4.5) (vec 0 0.75))) "beat-sync: the columns averaged between beats"
check (equal? (mat-log1p (list (vec 0 (- (exp 1) 1))) 1) (list (vec 0 1))) "mat-log1p"


; --- the analyses are mir.h's (as Moritz's objects): the same as the Musil they replaced ---
var major-keys-mu (list "C" "Db" "D" "Eb" "E" "F" "F#" "G" "Ab" "A" "Bb" "B")
var minor-keys-mu (list "C" "C#" "D" "Eb" "E" "F" "F#" "G" "G#" "A" "Bb" "B")
function key-scores-mu (chroma) {
    var out (list)
    each (vec->list (range 12)) (function (k) {
        push out (list (getidx major-keys-mu k) 'major (correlation chroma (rotate-vec major-profile k)))
        push out (list (getidx minor-keys-mu k) 'minor (correlation chroma (rotate-vec minor-profile k)))
    })
    return (reverse (sort-by out last))
}
function tempo-estimate-with-mu (flux fps centre width) {
    var ac (autocorr (- flux (mean flux)))
    var best 0
    var best-bpm centre
    for (var bpm 30) (<= bpm 300) (var bpm (+ bpm 0.5)) {
        var lag (/ (* fps 60) bpm)
        var l0 (floor lag)
        if (< (+ l0 1) (length ac)) {
            var v (+ (* (getidx ac l0) (- 1 (- lag l0))) (* (getidx ac (+ l0 1)) (- lag l0)))
            var w (exp (* -0.5 (pow (/ (log2 (/ bpm centre)) width) 2)))
            if (> (* v w) best) { set best (* v w)
                                  set best-bpm bpm }
        }
    }
    return best-bpm
}
; bad sizes are errors, not crashes or gigabytes
check (contains? (error-of (function () (tempo-estimate-with (rand 40) -10 100 0.6))) "fps") "tempo-estimate-with: a negative fps is an error"
check (contains? (error-of (function () (tempo-estimate-with (rand 40) 1e12 100 0.6))) "fps") "tempo-estimate-with: a huge fps is an error"
check (contains? (error-of (function () (chromagram (rand 100) 8000 4294967296 1))) "2^24") "chromagram: n above 2^24 is an error"
check (<= (length (resample (ones 64) 0.0001)) 1) "resample: a tiny factor (its kernel bounded)"
var pcd (vec 0.9 0.1 0.5 0.1 0.7 0.6 0.1 0.8 0.1 0.5 0.1 0.4)
check (all? (zip (key-scores pcd) (key-scores-mu pcd)) (function (p) (and (equal? (head (head p)) (head (last p))) (equal? (getidx (head p) 1) (getidx (last p) 1)) (near? (last (head p)) (last (last p)) 1e-12)))) "key-scores: as written in Musil (keys spelled as musicians spell them)"
check (equal? (head (head (key-scores (rotate-vec pcd 3)))) "Eb") "key-scores: a key a minor third above C is Eb"
check (equal? (key-name (key-estimate pcd)) "C major") "key-estimate: C major"
var pulse (vec (map (vec->list (range 600)) (function (k) (if (== (mod k 25) 0) 1 (* 0.1 (mod (* k 7) 3))))))
check (== (tempo-estimate-with pulse 50 100 0.6) (tempo-estimate-with-mu pulse 50 100 0.6)) "tempo-estimate-with: as written in Musil"
check (== (tempo-estimate pulse 50) 120) "tempo-estimate: a pulse every 25 frames at 50 fps is 120 bpm"

; --- harmonize ---
var hsr 44100
function cents (f g) (* 1200 (log2 (/ f g)))
function db-ratio (y x) (* 20 (log10 (/ (rms y) (rms x))))
var hx (* 0.5 (sine hsr 220 1.2))
var hv (harmonize-voices hx hsr (list 4 7 12 -12) (list (list "seed" 1)))
check (== (length hv) 4) "harmonize-voices: a vector per voice"
check (== (length (head hv)) (length hx)) "harmonize-voices: as long as the input (the latency taken out)"
each (list 0 1 2 3) (function (k) {
    var y (getidx hv k)
    var want (* 220 (pow 2 (/ (getidx (list 4 7 12 -12) k) 12)))
    check (< (abs (cents (head (yin (slice y 26000 4096) hsr 0.15 50 2000)) want)) 5) (concat "harmonize: a sine's voice " k " at its interval, within 5 cents")
    check (< (abs (db-ratio (slice y 22050 22050) (slice hx 22050 22050))) 1.5) (concat "harmonize: voice " k " at the input's level, within 1.5 dB")
})
; a voice: a pulse train at 150 Hz (a period of 294 samples) through resonances at 700 and 1200 Hz
var glottal (== (mod (range (* 2 hsr)) 294) 0)
var hvox (* 0.5 (normalize-peak (+ (bandpass glottal hsr 700 5) (* 0.4 (bandpass glottal hsr 1200 5)))))
; where the first formant is: the centroid of the power between 450 and 1050 Hz
function envelope-peak (y) {
    var m (magnitudes (fft (* (slice y 40000 8192) (hann 8192))))
    var lo (floor (/ (* 450 8192) hsr))
    var hi (floor (/ (* 1050 8192) hsr))
    var p (pow (slice m lo (- hi lo)) 2)
    return (/ (sum (* p (* (+ lo (range (- hi lo))) (/ hsr 8192)))) (sum p))
}
var hf1 (head (harmonize-voices hvox hsr (list 4) (list (list "formants" 1))))
var hf2 (head (harmonize-voices hvox hsr (list 4) (list (list "formants" 1.2))))
check (< (abs (cents (head (yin (slice hf1 40000 4096) hsr 0.15 50 2000)) (* 150 (pow 2 (/ 4 12))))) 5) "harmonize: a voice a major third up"
check (< (abs (- (envelope-peak hf1) 700)) 70) "harmonize: formant 1 keeps the voice's formant (700 Hz, within 10%)"
check (< (abs (- (envelope-peak hf2) 840)) 84) "harmonize: formant 1.2 moves it to 840 Hz (within 10%)"
check (< (abs (cents (head (yin (slice hf2 40000 4096) hsr 0.15 50 2000)) (* 150 (pow 2 (/ 4 12))))) 5) "harmonize: formant 1.2 leaves the pitch"
; a glide from 150 to 300 Hz in 1.5 s: the f0 followed and the voice a fifth up, within 20 cents
var gT 1.5
var gt (/ (range (floor (* hsr gT))) hsr)
var glide (* 0.5 (sin (* tau 150 (/ gT (log 2)) (- (pow 2 (/ gt gT)) 1))))
var gf0 (harmonize-f0 glide hsr)
var gv (head (harmonize-voices glide hsr (list 7) (list)))
each (list 0.3 0.6 0.9 1.2) (function (t) {
    var at (floor (* t hsr))
    var want (* 150 (pow 2 (/ t gT)))
    check (< (abs (cents (getidx gf0 at) want)) 20) (concat "harmonize-f0: the glide at " t " s, within 20 cents")
    check (< (abs (cents (head (yin (slice gv (- at 512) 1024) hsr 0.15 50 2000)) (* want 1.4983))) 20) (concat "harmonize: a fifth above the glide at " t " s, within 20 cents")
})
; noise: no f0, nothing broken
var hn (harmonize (noise 44100) hsr (list 4 7) (list (list "dry" 1)))
check (all? (head hn) (function (v) (and (< v 10) (> v -10)))) "harmonize: noise gives numbers, no NaN"
; scale: A3 sung in C major, two steps up is C4 (a minor third), four is E4; midi-chord and midi-root at absolute pitches
var hsc (harmonize-voices hx hsr (list 2 4) (list (list "mode" "scale") (list "key" "C") (list "scale" "major")))
check (< (abs (cents (head (yin (slice (head hsc) 26000 4096) hsr 0.15 50 2000)) 261.63)) 5) "harmonize: scale mode, a third in C major above A is C"
check (< (abs (cents (head (yin (slice (last hsc) 26000 4096) hsr 0.15 50 2000)) 329.63)) 5) "harmonize: scale mode, a fifth in C major above A is E"
var hch (harmonize-voices hx hsr (list 0 0) (list (list "mode" "midi-chord") (list "notes" (list (list 0 1.2 64 100) (list 0.2 1 55 64)))))
check (< (abs (cents (head (yin (slice (head hch) 26000 4096) hsr 0.15 50 2000)) 329.63)) 5) "harmonize: midi-chord, the first note's voice at E4"
check (< (abs (cents (head (yin (slice (last hch) 26000 4096) hsr 0.15 50 2000)) 196)) 5) "harmonize: midi-chord, the second note's voice at G3"
check (< (rms (slice (last hch) 0 4000)) 1e-6) "harmonize: midi-chord, a voice is silent before its note"
check (< (abs (- (db-ratio (slice (last hch) 26000 8000) (slice (head hch) 26000 8000)) (* 20 (log10 (/ 64 100))))) 1.5) "harmonize: midi-chord, velocity is level"
var hrt (harmonize-voices hx hsr (list 0 7) (list (list "mode" "midi-root") (list "notes" (list (list 0 1.2 48)))))
check (< (abs (cents (head (yin (slice (last hrt) 26000 4096) hsr 0.15 50 2000)) 196)) 5) "harmonize: midi-root, a fifth above C3"
; the same seed, the same voices (vibrato's irregularity and humanizing are random)
var hopt (list (list "seed" 9) (list "humanize" (list 10 30)) (list "vibrato" (list 20 5 0.5)))
check (near? (head (harmonize hx hsr (list 4 -5) hopt)) (head (harmonize hx hsr (list 4 -5) hopt)) 1e-12) "harmonize: a seed gives the same output"
check (> (max (abs (- (head (harmonize hx hsr (list 4 -5) hopt)) (head (harmonize hx hsr (list 4 -5) (list (list "seed" 10) (list "humanize" (list 10 30)) (list "vibrato" (list 20 5 0.5)))))))) 1e-3) "harmonize: another seed, other voices"
check (contains? (error-of (function () (harmonize hx hsr (list 1 2 3 4 5 6 7 8 9) (list)))) "8 voices") "harmonize: at most 8 voices"
check (contains? (error-of (function () (harmonize hx hsr (list 4) (list (list "scale" "nope"))))) "unknown scale") "harmonize: unknown scale"
check (equal? (scale-steps 'dorian) (list 0 2 3 5 7 9 10)) "scale-steps"

report "test_signals"
