# minlsp

A minimal clone of Max/MSP: patch boxes together on a canvas and hear the
result in real time. It's written in C++ and uses FLTK for the GUI and
miniaudio for audio.

## Build

```sh
make          # first run downloads and builds FLTK 1.4.4 and miniaudio into deps/
make run      # opens minlsp.app
```

`make` produces `minlsp.app`, a universal (arm64 + x86_64) macOS bundle,
statically linked with FLTK and ad-hoc signed. It needs the Xcode command line
tools, cmake and curl. `make clean` removes the build products, and
`make distclean` also removes `deps/`.

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
- **Console**: Patch > Console (**Shift+Cmd+M**) shows what `print` objects
  print, plus errors such as a missing sound file or a bad `expr`.
- **Undo/redo**: Cmd+Z and Shift+Cmd+Z.

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
| `reson~ [gain cf Q]` | Two-pole resonant bandpass. Its peak gain is `gain` |
| `lores~ [cf res]` | Resonant low-pass; `res` goes from 0 to 1 |
| `svf~ [freq q]` | State-variable filter with low, high, band and notch outlets. `q` goes from 0 to 1 |
| `biquad~ [a0 a1 a2 b1 b2]` | Raw biquad: `y = a0 x + a1 x1 + a2 x2 - b1 y1 - b2 y2` |

**Signal math**

| Object | Description |
| --- | --- |
| `+~ -~ *~ /~ [arg]` | Signal arithmetic |
| `clip~ [min max]`, `abs~`, `tanh~` | Limiting, rectifying and soft saturation |
| `expr~ ...` | `expr` evaluated per sample. `$v1`..`$v9` are the inlets, which take signals or floats |
| `sah~ [threshold]` | Sample and hold: keeps the left signal each time the right one rises above the threshold |

**Delay**

| Object | Description |
| --- | --- |
| `tapin~ [max ms]` | Writes a delay line. Connect its outlet to `tapout~` |
| `tapout~ [ms ...]` | One tap per argument; each inlet sets that tap's delay. A feedback loop from `tapout~` back to `tapin~` is allowed, with a minimum delay of 64 samples |
| `comb~ [max delay a b c]` | Comb filter: `y = a x + b x[n-D] + c y[n-D]` |
| `allpass~ [max delay gain]` | Allpass filter, for reverbs |

**Samples** (WAV, FLAC and MP3; paths are relative to the patch file)

| Object | Description |
| --- | --- |
| `buffer~ name [file] [ms] [channels]` | Named sample memory. Messages: `read file`, `write file.wav`, `size ms`, `clear` |
| `play~ name` | Plays the buffer once on `start [from [to]]` or bang. With a signal in its inlet, it reads at that position in ms instead |
| `groove~ name` | Variable-speed playback; the rate is the signal in the left inlet. A float there jumps to that position in ms. `loop 1` turns looping on, and inlets 2 and 3 set the loop points. The last outlet is a sync ramp |
| `record~ name` | Records into a buffer: `1` starts, `0` stops, `loop 1` wraps around at the end |
| `sfplay~ [channels]` | Plays a file directly: `open file`, `1`/`0`, `loop 1`, `speed x` |
| `index~ name`, `poke~ name` | Read or write a buffer~ at a sample index given as a signal |
| `count~ [min max loop]` | Sample counter: bang starts it from `min`, `stop` stops it. It wraps at `max`, or stops there when `loop` is 0 |

**Spectral** (inside a `pfft~` patch, a signal carries one frame: one bin per sample)

| Object | Description |
| --- | --- |
| `pfft~ name [size overlap]` | Runs the abstraction `name.mlsp` once per FFT frame (default 512, overlap 4, Hann window). Its signal inlets and outlets come from the `fftin~` and `fftout~` objects inside; `inlet` and `outlet` add control ports after them. Latency is one frame |
| `fftin~ n` | Real part, imaginary part and bin number of `pfft~` signal inlet `n` (bins 0 to size/2 - 1) |
| `fftout~ n` | Real and imaginary parts for `pfft~` signal outlet `n` |
| `cartopol~`, `poltocar~` | Real/imaginary to amplitude/phase, and back |
| `framedelta~`, `frameaccum~ [wrap]` | Per bin: difference from the previous frame, and running sum (with `1`, kept in -pi..pi) |
| `phasewrap~` | Wraps a phase into -pi..pi |

**Analysis**

| Object | Description |
| --- | --- |
| `snapshot~ [ms]` | Outputs the signal's current value on bang, or every `ms` |
| `peakamp~ [ms]` | Outputs the peak level since the last report |
| `meter~`, `scope~ [samples]` | Level meter and a triggered oscilloscope, drawn live |

**Audio I/O**: `adc~ [ch...]` and `dac~ [ch...]`. The default channels are 1
and 2. `start` and `stop` turn audio on and off, and so does a message box
sending `; dsp start` or `; dsp stop`.

**Timing**

| Object | Description |
| --- | --- |
| `metro ms` | Repeating bang: `1` or bang starts it, `0` or `stop` stops it. Ticks are scheduled at absolute times, so they don't drift |
| `delay ms` | Delayed bang. A new bang restarts it, and `stop` cancels it |
| `line [start grain]` | Control ramps: `target time` outputs a value every `grain` ms (20 by default) |
| `loadbang` | Bangs when the patch opens (double-click it in locked mode to fire it again) |

**Control math**

| Object | Description |
| --- | --- |
| `+ - * / % [arg]` | Arithmetic |
| `== != > < >= <= [arg]` | Comparisons that output 1 or 0 |
| `mtof`, `ftom`, `dbtoa`, `atodb` | MIDI note ↔ Hz and dB ↔ amplitude conversions |
| `scale inlo inhi outlo outhi [exp]` | Maps a range linearly, or along a curve when `exp` is given |
| `expr ...` | Expressions over `$f1`..`$f9` and `$i1`..`$i9`, with `+ - * / % == != < > <= >= && \|\| !`, functions such as `sin`, `pow`, `sqrt`, `min`, `max`, `int` and `random(lo, hi)`, and the constant `pi` |
| `random [n]` | Bang outputs a random integer from 0 to n-1. `seed x` makes the sequence repeatable |
| `counter [max]` or `counter min max` | Each bang outputs the count, then steps it, wrapping back to the start |

**Routing and messages**

| Object | Description |
| --- | --- |
| `trigger b f i l a s ...` | Outputs right to left, converting the input to the type of each outlet |
| `pack`, `unpack [items]` | Build a list or split one |
| `route a b ...` | Sends a message out the outlet matching its first word, with that word removed. Unmatched messages go out the last outlet |
| `select a b ...` | Bangs the outlet matching the input; other values pass through the last outlet |
| `gate [n]` | The left inlet picks the outlet (0 closes it); the right inlet is the data |
| `int`, `float` | Store a number; bang outputs it |
| `send name`, `receive name` | Wireless messages |
| `print [name]` | Prints to the console |

**Patching**

| Object | Description |
| --- | --- |
| `p name` | Subpatcher, saved inside the parent's file |
| `inlet`, `inlet~`, `outlet`, `outlet~` | A subpatcher's ports, numbered left to right by position |
| any `name` | An abstraction: the patch `name.mlsp`, found next to the patch (or in the bundled examples). Its arguments replace `#1`..`#9` inside it, and `#0` becomes a number unique to each copy, for local `send`/`receive`/`buffer~` names. Saving from its window writes the abstraction's file |

**Interface**

| Object | Description |
| --- | --- |
| `message` | Sends its contents when clicked. `$1`..`$9` are replaced by the incoming values, `,` separates messages, and `; name value` sends to `receive name` |
| `flonum`, `number` | Number boxes |
| `button`, `toggle` | Bang, and on/off |
| `slider [min max]` | Horizontal slider. It outputs integers when both limits are integers |
| `comment` | A text note |

## Patch files (`.mlsp`)

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

`examples/` has eighteen demos, plus the abstractions they use. They are also inside the app bundle, and
File > Open starts there.

- `sine.mlsp`: the patch from the spec, with frequency and volume controls.
- `noise-presets.mlsp`: noise through `reson~`, with presets sent as remote messages.
- `clicks.mlsp`: `click~` striking two resonators.
- `thru.mlsp`: audio input passed to the output.
- `sequencer.mlsp`: `metro` → `counter` → `expr` → `mtof`, driving a `saw~`
  through `lores~` with a `line~` envelope. Sliders set the step time and the
  cutoff.
- `synth.mlsp`: notes played through `trigger`, a pulse and a triangle, and an
  attack/decay/release envelope.
- `echo.mlsp`: noise bursts into a `tapin~`/`tapout~` feedback delay.
- `sampler.mlsp`: `buffer~` with `groove~` at any rate, including backwards,
  and one-shot `play~`.
- `recorder.mlsp`: records from the microphone into a looping buffer.
- `filters.mlsp`: `svf~` swept by a `phasor~` LFO in the signal domain.
- `control.mlsp`: `random`, `select`, `route`, `pack`, `expr`, `counter`,
  `gate`, `delay`, `line` and `print`. Open the console to see the output.
- `reverb.mlsp`: a Schroeder reverb (4 × `comb~` → 2 × `allpass~`) on
  `sfplay~`.
- `lfo.mlsp`: a tremolo, with `snapshot~`/`peakamp~` showing signal values as
  numbers.
- `fm.mlsp`: two-operator FM, with the operator inside a `p fm` subpatcher,
  envelopes on amplitude and index, and bell/brass/glass/gong presets.
- `fm-bells.mlsp`: generative bells from two copies of the `fmvoice`
  abstraction (`fmvoice.mlsp`), each with its own harmonicity, index and decay.
- `granulator.mlsp`: four copies of the `grain` abstraction (`grain.mlsp`)
  granulating a sung phrase, with rate, pitch, position and spread.
- `cross.mlsp`: cross synthesis in `pfft~` (`xsynth.mlsp`): a sawtooth chord
  takes on the spectral envelope of a voice.
- `pvoc.mlsp`: a phase vocoder. `pv-rec.mlsp` analyses the phrase into two
  buffers, and `pv-play.mlsp` resynthesises it at any speed, frozen or
  backwards, without changing its pitch.

`examples/sounds/bell.wav` was synthesized with minlsp itself, from
`click~` and five `reson~` filters. `voice.wav` is a synthetic sung phrase
(glottal pulses through moving vowel formants).

## Source layout

All sources are in `src/`:

- `dsp.h`: DSP kernels (oscillators, filters, ramps, delay lines, FFT, a
  lock-free queue), with no dependencies.
- `minlsp.h`, `core.cpp`: atoms, the s-expression reader, boxes and the
  patch: editing, nesting, signal-graph ordering, scheduler, messaging and
  file format.
- `objects.h`, `classes.cpp`: helpers shared by the objects, and the class
  table that creates boxes by name and finds abstractions.
- `objects_signal.cpp`, `objects_control.cpp`, `objects_samples.cpp`,
  `objects_patcher.cpp`: the objects; the last one holds `p`, abstractions,
  `pfft~` and the spectral and sample-index objects. `expr.h` is the
  expression language of `expr` and `expr~`.
- `audio.cpp`: the miniaudio engine.
- `gui.h`, `theme.h`, `canvas.cpp`, `mainwindow.cpp`: the FLTK editor, main
  window, toolbar and subpatcher windows.
- `main.cpp`: `main` and the `--render` mode.

The audio callback only ever `try_lock`s the patch. If the GUI is changing the
graph at that moment, the callback outputs one block of silence instead of
waiting. Values sent to signal inlets while audio runs go through atomics, and
`line~` ramps go through a lock-free queue. Timed objects such as `metro`,
`delay` and `line` run on a scheduler on the GUI thread (like Max with
Overdrive off). Signal feedback loops are refused when you connect, as in MSP,
except through `tapin~`/`tapout~`.

Subpatchers, abstractions and `pfft~` each own an inner patch. The top
patch owns everything shared (the DSP lock, the scheduler, audio I/O, and
the `send`/`receive` and `buffer~` names, which are global as in Max), and
an inner patch's signal chain runs inside its owner box. `pfft~` buffers
its input, and every hop it runs the inner chain over the frame's bins in
64-sample chunks, then overlap-adds the inverse FFT.

To run a patch offline for testing, use
`minlsp --render patch.mlsp seconds [out.wav] [bang=<id>] [send=<id>:<message>] ...`
(`<id>` can name a box inside a subpatcher, as `p1/osc`).
The scheduler follows sample time, so `metro` and the other timed objects work
offline too. It prints the peak and RMS level of each channel, sends `print`
output to stdout, and can write a WAV file.
