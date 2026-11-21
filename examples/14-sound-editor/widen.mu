; widen.mu - a process for the sound editor (Process > A Musil script...): a stereo sound made wider
; or narrower by its mid and side (the sum and the difference of its channels: side times width),
; a mono sound made stereo by the Haas effect (the right channel a few milliseconds later, heard as
; space, not as an echo). A mono sound comes back with two channels: choose "Result: a new sound"
; in the Process menu first (put back into one channel, the two would be mixed down again).
var width 1.8      ; stereo: 0 mono, 1 as it is, 2 twice as wide
var haas-ms 12     ; mono: the delay of the right channel

function process (x sr) {
    if (== (length x) 1) {
        var c (head x)
        return (list c (delay c (* sr (/ haas-ms 1000))))
    }
    var l (getidx x 0)
    var r (getidx x 1)
    var m (* 0.5 (+ l r))
    var s (* 0.5 width (- l r))
    var g (/ 1 (max 1 (* 0.5 (+ 1 width))))   ; wider is louder at the sides: kept from clipping
    return (list (* g (+ m s)) (* g (- m s)))
}
