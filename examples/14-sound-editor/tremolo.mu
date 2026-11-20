; tremolo.mu - a process for the sound editor: Process > A Musil script... runs it over the selection
; (all of the sound when nothing is selected). The editor calls (process x sr): x is a list of channel
; vectors, one per channel; sr is the rate. It returns the processed sound at the same rate: a vector
; (every channel the same) or a list of channel vectors. The result replaces the selection (one undo
; step) or opens as a new sound, as the Process menu's Result says; an error is shown on the status
; line and in the Console. The script runs with a code box's permissions (no programs, no network).
;
; Here: the loudness swung by a sine, rate times a second, depth deep.
var rate 6        ; Hz
var depth 0.8     ; 0 (nothing) to 1 (down to silence)

function process (x sr) {
    var t (/ (range (length (head x))) sr)
    var swing (- 1 (* depth 0.5 (+ 1 (sin (* tau rate t)))))
    return (map x (function (c) (* c swing)))
}
