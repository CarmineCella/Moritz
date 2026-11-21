; reverse-echo.mu - a process for the sound editor (Process > A Musil script...): echoes that come
; before the sound instead of after it, a swell into each attack. The selection is reversed, given
; taps echoes, delay milliseconds apart, each fb times the one before, and reversed again; the
; result is longer than the selection by the echoes, at the start, and as loud at its peak as the
; selection was. Try it on a voice (Vox.wav) or a drum hit. See tremolo.mu for how it is called.
var delay-ms 150   ; between the echoes
var fb 0.6         ; each echo this much of the one before
var taps 5         ; how many echoes

function peak (chans) {                     ; the largest magnitude of all the channels
    var p 0
    var k 0
    while (< k (length chans)) {
        set p (max p (max (abs (getidx chans k))))
        set k (+ k 1)
    }
    return p
}

function process (x sr) {
    var d (floor (* sr (/ delay-ms 1000)))
    var was (peak x)
    var y (map x (function (c) {
        var r (vec (reverse c) (zeros (* taps d)))
        var acc r
        var g fb
        var k 1
        while (<= k taps) {
            set acc (+ acc (* g (delay r (* k d))))
            set g (* g fb)
            set k (+ k 1)
        }
        return (reverse acc)
    }))
    var top (peak y)
    return (if (> top 0) (map y (function (c) (* c (/ was top)))) y)
}
