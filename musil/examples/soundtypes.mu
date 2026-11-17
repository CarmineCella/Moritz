; soundtypes: sound types, after the author's SoundTypes (ICMC 2013). A sound becomes a sequence of symbols, its types;
; new sequences become new sounds, and one sound can be rebuilt with the types of another.
;   1. analysis        the frames of a drum loop (MFCCs, centroid, loudness) clustered into types; runs of a type are
;                      the units of level 1; where the next type is surprising a gesture ends: the units of level 2
;   2. generation      new sequences of units: a factor oracle (continue the original, or jump where the recent past
;                      repeats), a variable-order Markov model, the classic n-grams
;   3. rematch         the voice rebuilt with the drums' material, unit by unit, keeping the voice's loudness
; The results are written to /tmp. Usage: musil soundtypes.mu
load "signals.mu"
load "music.mu"

function mono (path) {
    var w (read-wav path)
    return (list (head w) (head (getidx w 1)))
}
var drums (mono "../../data/drums.wav")
var sr (head drums)
var x (last drums)

; --- 1. analysis -------------------------------------------------------------------------------------------------------
var m (soundtypes-analyse x sr (list 'types (list 10 5) 'levels 2))
var counts (soundtypes-levels m)
print "the drums:" (head counts) "units at level 1," (last counts) "gestures at level 2"
print "the first units (start end type):"
each (take (soundtypes-units m 1) 6) (function (u) (print "  " (fixed (head u) 3) (fixed (getidx u 1) 3) (last u)))
var tr (soundtypes-transitions m 1)
print "type 0 is followed by:" (fixed (head tr) 2)

; --- 2. generation -----------------------------------------------------------------------------------------------------
each (list 'oracle 'markov 'classic) (function (method) {
    var g (soundtypes-generate m 1 8 (list 'method method 'order 3 'continuity 0.7 'seed 2))
    var path (concat "/tmp/musil_types_" (str method) ".wav")
    write-wav path sr (list (head g))
    print "wrote" path ":" (length (last g)) "units," (fixed (/ (length (head g)) sr) 1) "s"
})
var g2 (soundtypes-generate m 2 8 (list 'inner 0.6 'seed 5))
write-wav "/tmp/musil_types_gestures.wav" sr (list (head g2))
print "wrote /tmp/musil_types_gestures.wav: new gestures, each played by new units of level 1"

; --- 3. rematch --------------------------------------------------------------------------------------------------------
var vox (mono "../../data/Vox.wav")
var mv (soundtypes-analyse (last vox) (head vox) (list 'types (list 10 5)))
write-wav "/tmp/musil_types_match.wav" sr (list (soundtypes-match mv m 1 (list 'contour 0.8)))
write-wav "/tmp/musil_types_frames.wav" sr (list (soundtypes-match mv m 0 (list 'contour 1)))
print "wrote /tmp/musil_types_match.wav (unit by unit) and musil_types_frames.wav (frame by frame): the voice made of drums"
