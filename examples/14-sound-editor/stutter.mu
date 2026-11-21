; stutter.mu - a process for the sound editor (Process > A Musil script...): the first slice of the
; selection repeated, quieter each time, before the selection plays on, as a stuttering sampler does.
; The result is longer than the selection (times slices more). Select an attack (a drum hit, a
; syllable) and run it. See tremolo.mu for how the editor calls (process x sr).
var ms 90          ; the slice, in milliseconds
var times 4        ; how many repeats before the sound goes on
var fall 0.15      ; how much quieter each repeat is

function process (x sr) {
    var n (min (floor (* sr (/ ms 1000))) (length (head x)))
    var edge (min 64 (floor (/ n 4)))          ; short fades: no clicks between the repeats
    return (map x (function (c) {
        var s (fade-out (fade-in (take c n) edge) edge)
        var out (take c 0)
        var k 0
        while (< k times) {
            set out (vec out (* s (- 1 (* fall k))))
            set k (+ k 1)
        }
        return (vec out c)
    }))
}
