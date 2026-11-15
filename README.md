# Moritz

Moritz is a Max-like sound computing environment: patch boxes together on a
canvas and hear the result in real time. It's written in C++ and uses FLTK
for the GUI, miniaudio for audio and RtMidi for MIDI. It runs on macOS and
Linux.

## Build

```sh
make          # first run downloads FLTK 1.4.4, miniaudio and RtMidi into deps/
make run      # opens Moritz.app
```

`make` produces `Moritz.app`, a universal (arm64 + x86_64) macOS bundle,
statically linked with FLTK and ad-hoc signed. It needs the Xcode command line
tools, cmake and curl. `make clean` removes the build products, and
`make distclean` also removes `deps/`.

### Linux

On Linux, `make` (or `make linux`) builds FLTK for X11 and puts the program in
`build/linux/Moritz/`, with the examples, library and help next to it; run
`build/linux/Moritz/moritz`, or `make run`. It needs a C++17 compiler, cmake,
curl and the ALSA and X11 development packages; on Debian or Ubuntu:

```sh
sudo apt install build-essential cmake curl libasound2-dev libx11-dev libxext-dev \
  libxft-dev libxinerama-dev libxcursor-dev libxrender-dev libxfixes-dev libfontconfig1-dev
```

Audio goes through miniaudio (PulseAudio, PipeWire or ALSA, whichever is
there) and MIDI through ALSA.

### Releases and code signing

`make dist` builds `build/Moritz-<version>-macos-universal.zip`, which holds
the app, the examples, the README and the license. By default the app is
signed ad-hoc. That is enough on the Mac that built it, but macOS blocks a
downloaded ad-hoc app. To open a downloaded copy, either run

```sh
xattr -dr com.apple.quarantine Moritz.app
```

or try to open it once, then click **Open Anyway** in System Settings >
Privacy & Security.

A release that opens anywhere without these steps needs an Apple Developer
ID. Store notary credentials once with `xcrun notarytool store-credentials`,
then run `make notarize SIGN_ID="Developer ID Application: Name (TEAMID)"`.
This signs the app with the hardened runtime (microphone entitlement in
`resources/Moritz.entitlements`), notarizes and staples it, and rebuilds the
zip.

## Using it

The editor follows Max's conventions:

- **Edit mode / locked mode**: press **Cmd+E** to switch. New patches start
  in edit mode; opened patches start locked.
- **Edit mode**
  - Double-click empty space and type an object (`cycle~ 440`), then press
    Enter. With the mouse over the canvas, **n** creates an object box, **m**
    a message box, **f** a `flonum`, **i** a `number`, **b** a `button`,
    **t** a `toggle` and **c** a `comment`. **Patch > New** lists every
    object by category.
  - While you type an object name, a list of matching objects appears. Use
    Up/Down to choose, Tab to complete and keep typing arguments, Enter to
    complete and create the box, or click an entry.
  - Drag from an outlet (bottom edge) to an inlet (top edge) to connect.
    Rest the mouse on an inlet or outlet to see what it expects or sends.
  - Click or rubber-band to select. Drag to move. **Option-drag** copies the
    selection, including the cables between the copied boxes. Backspace
    deletes boxes and cables. Double-click a box to edit its text.
- **Locked mode**
  - Click a message box or a `button` to send it, click a `toggle` to switch
    it, and drag a `slider`.
  - Drag a number box up or down. On a `flonum`, dragging left of the decimal
    point changes it by 1 and right of it by 0.01. You can also click the box,
    type a value and press Enter.
- **Audio**: the **Audio** button in the toolbar (or Patch > Audio On,
  **Cmd+/**) starts and stops audio; it turns green while audio runs. The
  **Edit** button next to it switches edit mode. Patch > Audio Setup... picks
  devices, sample rate and buffer size. Audio input opens only while the patch
  contains `adc~`; macOS asks for microphone access the first time.
- **Subpatchers**: in locked mode, double-click a `p`, an abstraction or a
  `pfft~` to open it in its own window (Cmd+double-click does the same in
  edit mode). Undo, Save and the other menu commands work there too.
- **Console**: Patch > Console (**Shift+Cmd+M**) is Moritz's log. It shows
  what `print` objects print (`print name` writes `name: message`), errors
  (a missing sound file, a bad `expr`, an unknown object, a refused
  connection), and audio and file events (the device and sample rate when
  audio starts, patches opened and saved). Clear empties it.
- **MIDI**: Moritz listens to every MIDI input (and to devices plugged in
  later) and sends to a virtual port called "Moritz" that synths and DAWs can
  connect to. **Patch > MIDI Setup...** picks one input or an output device
  instead.
- **Help**: right-click (or Ctrl-click) any box and choose **Help**, or
  Option-click it in locked mode, to open its help patch: a small working
  example with notes. Help patches run alongside your patch (they play
  through the same audio) and stop when you close them. Objects without one
  print a one-line description to the console.
- **Library**: abstractions in `library/` work in any patch, like built-in
  objects, and are listed under **Patch > New > Library** and in completion.
  To add your own, put `name.moritz` (and optionally `name.help.moritz`) in
  `~/Documents/Moritz/library`; it is searched first.
- **Undo/redo**: Cmd+Z and Shift+Cmd+Z. **Cmd+W** closes the front window:
  a subpatcher or the console, or else the patch itself (asking to save).

## Objects

Objects follow their Max/MSP namesakes. Signal objects end in `~`. An
unconnected signal inlet takes a float instead. For control objects, the left
inlet is "hot" (it makes the object output) and the other inlets are "cold"
(they only store a value). The short names `t`, `sel`, `del`, `i`, `f`, `s`
and `r` work too.

**Oscillators and generators**

| Object | Description |
| --- | --- |
| `cycle~ [freq]` | Cosine oscillator (512-point table, interpolated). The right inlet sets the phase |
| `phasor~ [freq]` | Ramp from 0 to 1, for LFOs and as a position signal |
| `saw~`, `rect~ [freq duty]`, `tri~ [freq slope]` | Band-limited sawtooth and pulse (PolyBLEP), and a triangle |
| `noise~` | White noise |
| `click~ [shape...]` | Outputs an impulse on bang. A list sets the impulse shape |
| `sig~ [value]` | Turns a number into a signal |
| `line~` | Signal ramps. `1 10 0 300` goes to 1 in 10 ms, then to 0 in 300 ms. A float alone jumps, or ramps over the time given in the right inlet |

**Filters**

| Object | Description |
| --- | --- |
| `reson~ [gain cf Q]` | Two-pole resonant bandpass. Its peak gain is `gain`. `reson~`, `lores~` and `svf~` read signals in their parameter inlets once per 64-sample block |
| `lores~ [cf res]` | Resonant low-pass; `res` goes from 0 to 1 |
| `svf~ [freq q]` | State-variable filter with low, high, band and notch outlets. `q` goes from 0 to 1 |
| `biquad~ [a0 a1 a2 b1 b2]` | Raw biquad: `y = a0 x + a1 x1 + a2 x2 - b1 y1 - b2 y2`. The coefficient inlets take numbers (or a list of five in the left inlet), not signals |
| `conv~ buffer [channels]` | Convolution with the impulse response in a `buffer~`, with no latency (the first 256 taps directly, the rest by partitioned FFT). Channel c uses the response's channel c, or its last; an unconnected inlet takes the channel before, so a mono source into `conv~ hall 2` gives a stereo reverb. Normalised to unit energy (`normalize 0` to turn off), `gain x`, `set name` |
| `slide~ [up down]` | Smooths a signal: `y += (x - y) / slide`, with `up` samples while rising and `down` while falling (1 = no smoothing); `reset` jumps to the input |

**Signal math**

| Object | Description |
| --- | --- |
| `+~ -~ *~ /~ [arg]` | Signal arithmetic |
| `clip~ [min max]`, `abs~`, `tanh~` | Limiting, rectifying and soft saturation |
| `gain~ [ramp amplitude]` | A fader that scales the signal in its left inlet. Every change ramps linearly over `ramp` ms (20 by default), so it never clicks. The fader runs in dB from silence to +6 dB; a number sets the amplitude (1 = unity), the right inlet sets the ramp time, the right outlet reports the amplitude |
| `expr~ ...` | `expr` evaluated per sample. `$v1`..`$v9` are the inlets, which take signals or floats |
| `sah~ [threshold]` | Sample and hold: keeps the left signal each time the right one rises above the threshold |

**Signal routing**

| Object | Description |
| --- | --- |
| `selector~ n [initial]` | Passes one of `n` signal inlets; the left inlet picks 1..`n` (0 = none). Switching crossfades over 5 ms, so it never clicks |
| `gate~ n [initial]` | Sends the signal in its right inlet to one of `n` outlets, picked by the left inlet (0 = closed); also crossfades over 5 ms |
| `send~ name`, `receive~ name` | Signals by name, across subpatchers. `receive~` sums every `send~` of its name; `set name` changes it. The chain runs `send~` first, so there is no delay, except around a feedback loop through them (one 64-sample block) |

**Delay**

| Object | Description |
| --- | --- |
| `tapin~ [max ms]` | Writes a delay line. Connect its outlet to `tapout~` |
| `tapout~ [ms ...]` | One tap per argument; each inlet sets that tap's delay. A feedback loop from `tapout~` back to `tapin~` is allowed, with a minimum delay of 64 samples |
| `comb~ [max delay a b c]` | Comb filter: `y = a x + b x[n-D] + c y[n-D]`. Like `tapin~` and `allpass~`, its memory is sized for 96 kHz, so at lower rates delays a little beyond `max` still work |
| `allpass~ [max delay gain]` | Allpass filter, for reverbs |

**Samples** (WAV, FLAC and MP3; paths are relative to the patch file)

| Object | Description |
| --- | --- |
| `buffer~ name [file] [ms] [channels]` | Named sample memory. Messages: `read file`, `write file.wav` (16-bit), `size ms`, `clear`. An empty buffer runs at the audio sample rate |
| `play~ name [channels]` | Plays the buffer once on `start [from [to]]` or bang. With a signal in its inlet, it reads at that position in ms instead |
| `groove~ name [channels]` | Variable-speed playback; the rate is the signal in the left inlet. A float there jumps to that position in ms. `loop 1` turns looping on, and inlets 2 and 3 set the loop points. The last outlet is a sync ramp |
| `record~ name [channels]` | Records into a buffer: `1` starts, `0` stops, `loop 1` wraps around at the end. The outlet is the record position, 0 to 1 |
| `sfplay~ [channels]` | Plays a file directly: `open file`, `1` (or bang) / `0`, `loop 1`, `speed x` (negative plays backwards) |
| `sfrecord~ [channels]` | Records its inlets (1 channel by default) to a 24-bit WAV: `open file.wav`, then `1` starts and `0` stops. The outlet reports the recorded time in ms. Samples go through a buffer to a writer thread, so recording never holds up the audio |
| `waveform~ name [width height]` | Shows a `buffer~`, one lane per channel. In locked mode, drag to select a region: the outlets give its start and end in ms. A number draws a cursor (ms); `select start end`, `set name` |
| `condec` | Convolution and deconvolution of whole buffers (the author's *condec*): `conv x h out [scale mix]` and `deconv y h out [threshold scale]` write into the `buffer~` `out`, then bang. Deconvolution divides the spectra where the response is above `threshold` (0.001) times its peak, so a recording can be "un-reverberated" by its impulse response |
| `index~ name [channel]`, `poke~ name [channel]` | Read or write a buffer~ at a sample index given as a signal (no interpolation); `set name` |
| `count~ [min max loop]` | Sample counter: bang starts it from `min` (a number: from there), `stop` stops it. It wraps at `max` (0 = never), or stops there when `loop` is 0. While stopped it outputs `min` |

**Spectral** (inside a `pfft~` patch each signal vector is one whole frame: one bin per sample)

| Object | Description |
| --- | --- |
| `pfft~ name [size overlap]` | Runs the abstraction `name.moritz` once per FFT frame (default 512, overlap 4, Hann window). Its signal inlets and outlets come from the `fftin~` and `fftout~` objects inside; `inlet` and `outlet` add control ports after them. Latency is one frame |
| `fftin~ n` | Real part, imaginary part and bin number of `pfft~` signal inlet `n` (bins 0 to size/2 - 1) |
| `fftout~ n` | Real and imaginary parts for `pfft~` signal outlet `n` |
| `cartopol~`, `poltocar~` | Real/imaginary to amplitude/phase, and back |
| `framedelta~`, `frameaccum~ [wrap]` | Per bin: difference from the previous frame, and running sum (with `1`, kept in -pi..pi). Where `frameaccum~`'s third inlet is not zero, that bin restarts from the value in its second inlet |
| `pvlock~` | Phase vocoder resynthesis phase with identity phase locking: inlets amplitude, analysed phase, phase difference, reset. Each spectral peak advances by its own frequency and the bins around it keep their analysed offsets from it, so stretched sounds stay clean |
| `sparkle~ [buffer] [channels] [window] [overlap]` | The phase vocoder of the author's *sparkle*, with phase locking around spectral peaks and formant (spectral envelope) preservation. With a `buffer~` it plays it: the left inlet is the speed (1 as recorded, 0 frozen, negative backwards), `start [ms]` or bang plays, `stop`, `loop 1`. Without one it transforms its input, one window late. Inlets 2 and 3: pitch and formant ratios. `envelope C` keeps the formants (C cepstral coefficients, 30-80 for voices); `threshold x` drops weak bins; `robot 1`, `whisper 1`. The last outlet is the read position, 0 to 1. Window 2048, overlap 8 by default |
| `lrshift~ n` | Shifts a frame by `n` bins, to combine bins with their neighbours |
| `phasewrap~` | Wraps a phase into -pi..pi |

**Analysis**

| Object | Description |
| --- | --- |
| `snapshot~ [ms]` | Outputs the signal's current value on bang, or every `ms` |
| `peakamp~ [ms]` | Outputs the peak level since the last report |
| `meter~`, `scope~ [samples]` | Level meter and a triggered oscilloscope, drawn live |
| `descriptors~ [size] [hop] [names...]` | Low-level descriptors every `hop` samples (1024) over `size` (2048): `rms centroid spread skewness kurtosis flux hfc zcr flatness periodicity f0`, one outlet each, in the order named (all by default). Centroid and spread are in Hz, zcr in crossings per second; f0 comes from the autocorrelation (0 when unvoiced) and periodicity is the height of its peak. Formulas from *sound_math*'s `features.h` |
| `spectroscope~ [width height]` | Live spectrum: 2048-point FFT, log frequency from 20 Hz to half the sample rate, 0 dB (a full-scale sine) down to -90 dB |

**Audio I/O**: `adc~ [ch...]` and `dac~ [ch...]`. The default channels are 1
and 2. `start` and `stop` turn audio on and off, and so does a message box
sending `; dsp start` or `; dsp stop`. `dspstatus~ [ms]` reports the audio
state on bang, whenever audio starts, stops or is reopened, and every `ms`
while audio runs. Its outlets, left to right, are 1/0 for audio on/off, the CPU
load in %, the sample rate, the I/O buffer size and the signal vector size (64,
or the frame's bin count inside `pfft~`). While audio is off it still reports
the last sample rate and buffer size.

**MIDI** (devices in Patch > MIDI Setup; channels 1-16)

| Object | Description |
| --- | --- |
| `notein [channel]` | Outlets: pitch, velocity (0 = note-off), and the channel when there is no argument |
| `ctlin [controller] [channel]` | Controller values 0-127; outlets for the controller number and channel when they are not given |
| `bendin [channel]`, `pgmin [channel]` | Pitch bend as 0-127 (64 = centre), program changes as 1-128 |
| `noteout [channel]`, `ctlout [controller] [channel]` | Send notes (inlets pitch, velocity, channel) and controllers (value, controller, channel) |
| `makenote [velocity] [duration]` | A pitch goes out at once with the velocity, and again with velocity 0 after `duration` ms; `stop` sends pending note-offs, `clear` drops them |
| `stripnote` | Passes note-ons only |

**Timing**

| Object | Description |
| --- | --- |
| `metro ms` | Repeating bang: `1` or bang starts it, `0` or `stop` stops it. Ticks are scheduled at absolute times, so they don't drift |
| `delay ms` | Delayed bang. A new bang restarts it, and `stop` cancels it |
| `line [start grain]` | Control ramps: `target time` outputs a value every `grain` ms (20 by default). The middle inlet gives the time for the next single number; `stop`, `set x` |
| `loadbang` | Bangs when the patch opens (double-click it in locked mode to fire it again) |

**Control math**

| Object | Description |
| --- | --- |
| `+ - * / % [arg]` | Arithmetic, always in floats (`7 / 2` is 3.5, unlike Max); dividing by 0 gives 0 |
| `== != > < >= <= [arg]` | Comparisons that output 1 or 0 |
| `mtof`, `ftom`, `dbtoa`, `atodb` | MIDI note ↔ Hz and dB ↔ amplitude conversions |
| `scale inlo inhi outlo outhi [exp]` | Maps a range linearly, or along a curve when `exp` is given |
| `expr ...` | Expressions over `$f1`..`$f9` and `$i1`..`$i9`, with `+ - * / % == != < > <= >= && \|\| !`, functions such as `sin`, `pow`, `sqrt`, `min`, `max`, `int` and `random(lo, hi)`, and the constant `pi` |
| `random [n]` | Bang outputs a random integer from 0 to n-1. `seed x` makes the sequence repeatable |
| `counter [max]` or `counter min max` | Each bang (or number) outputs the count, then steps it; `max` is included before it wraps back. `reset`, `set n` |

**Routing and messages**

| Object | Description |
| --- | --- |
| `trigger b f i l a s ...` | Outputs right to left, converting the input to the type of each outlet |
| `pack`, `unpack [items]` | Build a list or split one |
| `route a b ...` | Sends a message out the outlet matching its first word, with that word removed. Unmatched messages go out the last outlet |
| `select a b ...` | Bangs the outlet matching the input; other values pass through the last outlet |
| `gate [n] [open]` | The left inlet picks the outlet (0 closes it); the right inlet is the data |
| `int`, `float` | Store a number; bang outputs it |
| `send name`, `receive name` | Wireless messages |
| `print [name]` | Prints to the console |

**Patching**

| Object | Description |
| --- | --- |
| `p name` | Subpatcher, saved inside the parent's file |
| `inlet`, `inlet~`, `outlet`, `outlet~` | A subpatcher's ports, numbered left to right by position (`in`, `in~`, `out`, `out~` are the same objects) |
| `poly~ name voices [args]` | `voices` copies of the abstraction `name.moritz`, with their signal outlets summed. `note ...` sends the rest of the message to a free voice, stealing the oldest note when all are busy; `midinote pitch velocity` does the same for a velocity above 0, and sends a velocity of 0 (the note-off) to the voice playing that pitch; `target n` sends later messages to voice `n` only (`0` = all); `mute n 1/0` stops or restarts voice `n`; anything else goes to the target voice or to all |
| `thispoly~` | Inside a `poly~` voice: outputs the voice number at load or on bang. The voice stays busy while the signal in its inlet (the envelope) is not zero; a number sets busy directly |
| any `name` | An abstraction: the patch `name.moritz`, found next to the patch, then in the libraries, then in the bundled examples. Its arguments replace `#1`..`#9` inside it, and `#0` becomes a number unique to each copy, for local `send`/`receive`/`buffer~` names. Saving from its window writes the abstraction's file |

**Interface**

| Object | Description |
| --- | --- |
| `message` | Sends its contents when clicked. `$1`..`$9` are replaced by the incoming values, `,` separates messages, and `; name value` sends to `receive name`. As in Max, the box starts a new line after each `;`, so `; cf 1200; q 8` shows as three lines. When editing, Return finishes and Shift+Return adds a line break; line breaks count as spaces |
| `flonum`, `number` | Number boxes |
| `button`, `toggle` | Bang, and on/off |
| `slider [min max]` | Horizontal slider. It outputs integers when both limits are integers |
| `comment` | A text note |
| `function [domain] [lo] [hi]` | Breakpoint editor: in locked mode click to add a point, drag it, Shift-click to delete it. bang outputs the curve for `line~` (the first value, then value-time pairs over `domain` ms); a number (ms) outputs the value there. `domain`, `range`, `clear`, `set x y ...`; the points are saved with the patch |
| `kslider [octaves] [lowest] [mode]` | Piano keyboard (4 octaves from note 36). Outlets: pitch and velocity; the velocity depends on where you click (louder lower down). Mode 0 plays while the mouse is down and sends velocity 0 on release (drag to glide); mode 1 toggles keys, for chords. A pitch in the left inlet lights its key and passes through with the velocity from the right inlet; `set p v` only lights it, `flush` releases every held key, `clear` unlights them all without output |

**Library** (abstractions in `library/`, usable in any patch)

| Object | Description |
| --- | --- |
| `reverb~ [mix] [T60]` | A stereo Freeverb. Inlets: the signal, wet/dry mix (0-1, default 0.3) and decay time T60 in seconds (0.1-100, default 2). Outlets: left and right. Per channel, eight damped combs (`fvcomb~`) in parallel, then four `allpass~` in series; the right channel's delays are 23 samples longer, so the outputs are decorrelated. Each comb's feedback is `g = 10^(-3 d / T60)` for its delay `d` in seconds, so low frequencies fall by 60 dB in T60 seconds, and highs a little faster (damping) |
| `fvcomb~ ms name` | One Freeverb comb (feedback with a damping low-pass), used by `reverb~`; its T60 arrives on `name-t60` |

## Patch files (`.moritz`)

```lisp
(patch
  (obj osc  cycle~ 440 (pos 40 130))
  (obj gain *~ 0.2     (pos 40 180))
  (obj out  dac~       (pos 40 240))
  (obj p1   message "; freq 880" (pos 200 40))
  (connect osc 0 gain 0)
  (connect gain 0 out 0))
```

`(obj id class args...)` creates a box, and `(pos x y)` is optional. A `p`
box holds its contents as a nested `(patch ...)` form.
`(connect from outlet to inlet)` connects two boxes, counting ports from 0.
`;` starts a comment outside strings.

`examples/` has twenty-eight demos, plus the abstractions they use. They are also inside the app bundle, and
File > Open starts there.

- `sine.moritz`: the patch from the spec, with frequency and volume controls.
- `noise-presets.moritz`: noise through `reson~`, with presets sent as remote messages.
- `clicks.moritz`: `click~` striking two resonators.
- `thru.moritz`: audio input passed to the output.
- `sequencer.moritz`: `metro` → `counter` → `expr` → `mtof`, driving a `saw~`
  through `lores~` with a `line~` envelope, then `reverb~`. Sliders set the
  step time and the cutoff, number boxes the reverb mix and T60.
- `synth.moritz`: notes played through `trigger`, a pulse and a triangle, and an
  attack/decay/release envelope.
- `echo.moritz`: noise bursts into a `tapin~`/`tapout~` feedback delay.
- `sampler.moritz`: drums in a `buffer~`, with `groove~` at any rate,
  including backwards, and one-shot `play~`.
- `recorder.moritz`: records from the microphone into a looping buffer.
- `filters.moritz`: `svf~` swept by a `phasor~` LFO in the signal domain.
- `control.moritz`: `random`, `select`, `route`, `pack`, `expr`, `counter`,
  `gate`, `delay`, `line` and `print`. Open the console to see the output.
- `reverb.moritz`: a Schroeder reverb (4 × `comb~` → 2 × `allpass~`) on
  drums played by `sfplay~`.
- `freeverb.moritz`: the `reverb~` abstraction on a gong, with number boxes
  for mix and decay time.
- `lfo.moritz`: a tremolo, with `snapshot~`/`peakamp~` showing signal values as
  numbers.
- `fm.moritz`: two-operator FM, with the operator inside a `p fm` subpatcher,
  envelopes on amplitude and index, and bell/brass/glass/gong presets.
- `fm-bells.moritz`: generative bells from two copies of the `fmvoice`
  abstraction (`fmvoice.moritz`), each with its own harmonicity, index and decay.
- `granulator.moritz`: asynchronous granular synthesis: a scheduler
  subpatcher fires grains at random intervals, positions and pans, and
  `poly~` plays each in one of 32 `grainvoice.moritz` voices (a Hann window
  and read position driven by one `line~` ramp). Sliders and presets for
  density, size, pitch, position and spread. It reads a voice (`sounds/Vox.wav`).
- `spectra.moritz`: `spectroscope~` and `scope~` showing a sine, a saw, a
  pulse, noise and a band-pass whose centre glides (`slide~`) to random
  frequencies; `selector~` picks the source, `dspstatus~` shows the sample
  rate and CPU load.
- `tape.moritz`: a four-second tape loop: `record~` fills a `buffer~` from the
  microphone or a voice (`Vox.wav`), `waveform~` shows it, and dragging across it
  picks the region `groove~` loops, with a moving cursor. `sfrecord~` saves
  what you hear to the Desktop.
- `busses.moritz`: two players in subpatchers send their sound with `send~`;
  the main patch receives it, and `gate~` routes each player to a reverb bus
  or an echo bus, also by name.
- `midi-keys.moritz`: `notein` from a MIDI keyboard, a `kslider`, or
  `makenote` improvising, into `poly~` with `midinote` and out through
  `noteout`; the mod wheel (`ctlin 1`) sets the `reverb~` mix.
- `stretch.moritz`: `sparkle~` reading a voice (`Vox.wav`) with two
  `function` curves over 15 s, one for the speed (down to frozen) and one for the pitch; with
  `envelope 60` the formants stay put while the pitch moves.
- `convolution.moritz`: `conv~` with three impulse responses, the
  Concertgebouw (`hall.wav`, stereo), a bell and a gong, on a voice,
  Beethoven's Seventh or drums, with a wet/dry mix.
- `follower.moritz`: `descriptors~` follows a voice (`Vox.wav`); its f0, centroid and
  level drive a filtered sawtooth that imitates it.
- `keyboard.moritz`: two `kslider`s play `poly~` with `midinote`: one plays
  while you hold the mouse, the other holds chords. Each `keyvoice.moritz`
  voice sustains while its key is down and releases when it goes up.
- `poly-synth.moritz`: `poly~` with eight voices of `polyvoice.moritz` (detuned
  saws, filter, pluck envelope feeding `thispoly~`), played by chord buttons
  or a random arpeggiator, through a stereo delay.
- `cross.moritz`: cross synthesis in `pfft~` (`xsynth.moritz`) between
  Beethoven's Seventh (or a sawtooth chord) and a voice (`Vox.wav`). Every
  bin mixes the two amplitudes and phases, `A = X a1 + x a2 + q sqrt(a1 a2)`
  and `phi = Y phi1 + y phi2`, with a denoise threshold on `A`; presets for
  the geometric mean of the two spectra, the classic exchange, an amplitude
  mix and a phase blend.
- `pvoc.moritz`: a phase vocoder built from spectral objects, to see how
  one works (`sparkle~` is the ready-made one). `pv-rec.moritz` analyses a
  guitar phrase (`Gambale_cut.wav`) into
  buffers (amplitude, phase, phase difference), and `pv-play.moritz`
  resynthesises it at any speed, frozen or backwards, without changing its
  pitch, using `pvlock~` to keep the partials coherent. The phrase plays
  once while it is analysed; then the stretched version starts.

The sounds in `examples/sounds/` come from the examples of Musil: `Vox.wav`
(a voice), `Beethoven_Symph7.wav` (an excerpt of the Seventh Symphony),
`drums.wav`, `Gambale_cut.wav` (guitar), `archeos-bell.wav`,
`gong_c_sharp.wav`, and `hall.wav`, a stereo impulse response of the
Concertgebouw in Amsterdam (5 s).

## Source layout

All sources are in `src/`:

- `dsp.h`: DSP kernels (oscillators, filters, ramps, delay lines, FFT, a
  lock-free queue), with no dependencies.
- `moritz.h`, `core.cpp`: atoms, the s-expression reader, boxes and the
  patch: editing, nesting, signal-graph ordering, scheduler, messaging and
  file format.
- `objects.h`, `classes.cpp`: helpers shared by the objects, and the class
  table that creates boxes by name and finds abstractions.
- `objects_signal.cpp`, `objects_control.cpp`, `objects_samples.cpp`,
  `objects_midi.cpp`, `objects_patcher.cpp`: the objects; the last one holds `p`, abstractions,
  `pfft~` and the spectral and sample-index objects. `expr.h` is the
  expression language of `expr` and `expr~`.
- `objects_sound.cpp`: `sparkle~`, `conv~`, `condec`, `descriptors~` and
  `function`, adapted from the author's *sound_math* tools (sparkle, pconv,
  condec, features); `vocoder.h` is sparkle's phase vocoder and
  `partconv.h` the zero-latency convolver.
- `audio.cpp`: the miniaudio engine. `midi.h`, `midi.cpp`: MIDI through
  RtMidi; its callbacks queue the messages for the GUI thread.
- `gui.h`, `theme.h`, `canvas.cpp`, `mainwindow.cpp`: the FLTK editor, main
  window, toolbar and subpatcher windows.
- `main.cpp`: `main` and the `--render` mode.

`library/` holds the abstractions usable in any patch (`reverb~`), and
`help/` the help patches, `name.help.moritz`, with their helper abstractions
(`help-*.moritz`). Both are copied into the app bundle.

`resources/` holds the app icon and the image in the About box, drawn by
`tools/make_icon.py` (numpy and Pillow; the results are committed, so the
build doesn't need Python).

The audio callback only ever `try_lock`s the patch. If the GUI is changing the
graph at that moment, the callback outputs one block of silence instead of
waiting. Values sent to signal inlets while audio runs go through atomics, and
`line~` ramps go through a lock-free queue. Timed objects such as `metro`,
`delay` and `line` run on a scheduler on the GUI thread (like Max with
Overdrive off). Signal feedback loops are refused when you connect, as in MSP,
except through `tapin~`/`tapout~`.

Subpatchers, abstractions and `pfft~` each own an inner patch, and `poly~`
owns one per voice. The top
patch owns everything shared (the DSP lock, the scheduler, audio I/O, and
the `send`/`receive` and `buffer~` names, which are global as in Max), and
an inner patch's signal chain runs inside its owner box. `pfft~` buffers
its input, and every hop it runs the inner chain once over the whole frame
(the inner patch's vector size is fftsize/2), then overlap-adds the
inverse FFT.

To run a patch offline for testing, use
`moritz --render patch.moritz seconds [out.wav] [bang=<id>] [send=<id>:<message>] [png=<file>] [edit] [extra=<patch>] ...`
(`<id>` can name a box inside a subpatcher, as `p1/osc`; `midi=<seconds>:<bytes>`,
for example `midi=0.5:144 60 100`, feeds a MIDI message in, and MIDI output is
printed). `png=` draws the
patch as it looks after the render (locked, or in edit mode with `edit`), so
drawing can be checked without a screen; `extra=` runs a second patch
alongside, the way help patches run.
The scheduler follows sample time, so `metro` and the other timed objects work
offline too. It prints the peak and RMS level of each channel, sends `print`
output to stdout, and can write a WAV file.

## License

Moritz is released under the BSD 2-Clause License; see `LICENSE`.
Created by Carmine-Emanuele Cella (2026).
