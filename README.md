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
    a message box, **f** a `flonum` and **i** a `number`. **Patch > New** has
    every object.
  - Drag from an outlet (bottom edge) to an inlet (top edge) to connect.
  - Click or rubber-band to select. Drag to move. Backspace deletes boxes and
    cables. Double-click a box to edit its text.
- **Locked mode**
  - Click a message box to send it.
  - Drag a number box up or down. On a `flonum`, dragging left of the decimal
    point changes it by 1 and right of it by 0.01. You can also click the box,
    type a value and press Enter.
- **Audio**: Patch > Audio On (**Cmd+/**), and Patch > Audio Setup... to pick
  devices, sample rate and buffer size. Audio input opens only while the patch
  contains `adc~`; macOS asks for microphone access the first time.
- **Undo/redo**: Cmd+Z and Shift+Cmd+Z.

## Objects

| Object | Inlets | Description |
| --- | --- | --- |
| `cycle~ [freq]` | freq (signal/float), phase | Interpolated cosine oscillator, 512-point table |
| `reson~ [gain cf Q]` | signal, gain, cf, Q | Two-pole resonant bandpass with peak gain = `gain`. A list `gain cf Q` in the left inlet sets all three. Defaults are 1, 1000, 10 |
| `noise~` | — | White noise |
| `click~ [shape...]` | bang / list | Outputs an impulse on bang. A list sets the impulse shape |
| `+~ -~ *~ /~ [arg]` | signal, signal/float | Signal arithmetic. A float sets the operand while the inlet is unconnected |
| `+ - * / [arg]` | hot, cold | Control arithmetic. The left inlet outputs, the right inlet stores |
| `flonum`, `number` | float / bang / `set` | Float and integer number boxes |
| `message` | bang / list / `set` | Sends its contents. `$1`..`$9` are replaced by the incoming values, `,` separates messages, and `; name value` sends to `receive name` |
| `send name` / `s name` | any | Sends to every `receive name` |
| `receive name` / `r name` | — | Outputs what is sent to `name` |
| `loadbang` | | Bangs when the patch is opened |
| `adc~ [ch...]` | | Audio input, channels 1 and 2 by default |
| `dac~ [ch...]` | signals | Audio output. The messages `start` and `stop` turn audio on and off |

A message box can also send `; dsp start` or `; dsp stop`.

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

`(obj id class args...)` creates a box, and `(pos x y)` is optional.
`(connect from outlet to inlet)` connects two boxes, counting ports from 0.
`;` starts a comment outside strings.

`examples/` has four demos (they are also inside the app bundle, and
File > Open starts there):

- `sine.mlsp`: the patch from the spec, with frequency and volume controls.
- `noise-presets.mlsp`: noise through `reson~`, with presets sent as remote messages.
- `clicks.mlsp`: `click~` striking two resonators.
- `thru.mlsp`: audio input passed to the output.

## Source layout

- `dsp.h`: DSP kernels (oscillator, filter, noise, click, arithmetic), with no
  dependencies.
- `minlsp.h`: declarations for atoms, boxes, the patch, the audio engine and
  the GUI.
- `minlsp.cpp`: the object classes, patch editing and file I/O, signal-graph
  ordering, the miniaudio engine, the FLTK canvas and window, and `main`.

The audio callback only ever `try_lock`s the patch. If the GUI is changing the
graph at that moment, the callback outputs one block of silence instead of
waiting. Values sent to signal inlets while audio runs go through atomics.
Signal feedback loops are refused when you connect, as in MSP.

To run a patch offline for testing, use
`minlsp --render patch.mlsp seconds [out.wav] [bang=<id> ...]`. It prints the
peak and RMS level of each channel and can write a WAV file.
