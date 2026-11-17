; maple: matching pursuit, after the author's Maple. A sound is explained as a sum of atoms taken from a dictionary,
; the strongest first; the list of atoms (a book) can be rebuilt, transformed, or rebuilt from another sound's atoms.
;   1. Gabor atoms        windowed cosines at every scale, position and frequency: the drums explained by a few hundred,
;                         the atom map drawn (time, frequency, level), the sound rebuilt slower and lower
;   2. atoms of sounds    a dictionary cut from a voice (frames of 100 ms), then the drums explained by pieces of the
;                         voice: the rhythm of one, the material of the other (a hybrid)
;   3. cross synthesis    a book of the drums by a gong's frames: each atom of the voice replaced by the closest frame
;                         of the gong (their mel spectra)
; The results are written to /tmp. Usage: musil maple.mu
load "signals.mu"
load "music.mu"
load "plot.mu"

function mono (path) {
    var w (read-wav path)
    return (list (head w) (head (getidx w 1)))
}
var drums (mono "../../data/drums.wav")
var sr (head drums)
var x (take (last drums) (* 3 sr))
print "drums:" (fixed (/ (length x) sr) 2) "s at" sr "Hz"

; --- 1. Gabor atoms ----------------------------------------------------------------------------------------------------
var gabor (maple-gabor sr 256 4096)
print "dictionary:" (maple-info gabor)
var book (maple-decompose x sr gabor (list 'atoms 600 'snr 25))
var rows (maple-book book)
print (length (head rows)) "atoms explain the drums to" (fixed (maple-snr book) 1) "dB"
var fig (figure "The drums as Gabor atoms: time, frequency")
add-scatter fig (head rows) (getidx rows 1) "atoms"
set-option fig "xlabel" "time (s)"
set-option fig "ylabel" "frequency (Hz)"
save-png fig "/tmp/musil_maple_atoms.png" 900 500
print "wrote /tmp/musil_maple_atoms.png: the atom map"
write-wav "/tmp/musil_maple_gabor.wav" sr (list (maple-synth book nil))
write-wav "/tmp/musil_maple_slower.wav" sr (list (maple-synth book (list 'stretch 1.5 'ratio 0.8)))
write-wav "/tmp/musil_maple_residual.wav" sr (list (maple-residual book))
print "wrote /tmp/musil_maple_gabor.wav (rebuilt), musil_maple_slower.wav (1.5 times slower, a third lower), musil_maple_residual.wav"

; --- 2. atoms cut from a voice: a hybrid --------------------------------------------------------------------------------
var vox (mono "../../data/Vox.wav")
var voice (maple-cut (last vox) (head vox) 'frames 100 50 "Vox.wav")
print "the voice:" (get (maple-info voice) 'size) "atoms of 100 ms"
var hybrid (maple-decompose x sr voice (list 'components 4 'candidates 24))
print "the drums by the voice's atoms:" (length (head (maple-book hybrid))) "atoms," (fixed (maple-snr hybrid) 1) "dB"
write-wav "/tmp/musil_maple_hybrid.wav" (maple-rate hybrid) (list (maple-synth hybrid nil))
print "wrote /tmp/musil_maple_hybrid.wav: the drums' rhythm, the voice's sound"

; --- 3. cross synthesis: the same book, another dictionary --------------------------------------------------------------
var gong (mono "../../data/gong_c_sharp.wav")
var bell (maple-cut (last gong) (head gong) 'frames 100 50 "gong")
write-wav "/tmp/musil_maple_cross.wav" (maple-rate hybrid) (list (maple-synth hybrid (list 'tails 0) bell))
print "wrote /tmp/musil_maple_cross.wav: each voice atom replaced by the closest frame of a gong"
