; octave-down.mu - a process for the sound editor (Process > A Musil script...): the selection an
; octave lower, its formants kept, with Musil's phase vocoder (pvoc, the engine of sparkle~), mixed
; with what it was. See tremolo.mu for how the editor calls (process x sr).
load "signals.mu"

var wet 0.7       ; how much of the lower octave

function process (x sr)
    (map x (function (c)
        (+ (* (- 1 wet) c) (* wet (take (pvoc c (list (list "pitch" 0.5) (list "envelope" 40))) (length c))))))
