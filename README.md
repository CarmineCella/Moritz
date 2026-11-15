# Moritz

Moritz is a Max-like environment for sound and music computing: patch boxes
together on a canvas and hear the result in real time. It's written in C++
and uses FLTK for the GUI, miniaudio for audio and RtMidi for MIDI. It runs
on macOS, Linux and Windows.

### The name

Max, the patching environment Moritz follows, was named after Max Mathews,
the pioneer of computer music. In German, Max never goes far without
Moritz: *Max und Moritz* (1865), Wilhelm Busch's story of two boys and their
seven pranks, is one of the best-known picture books in the language. Moritz
is Max's companion: it works the way Max does, and gets up to some mischief
of its own. (Busch's two boys are hiding somewhere in the program, too.)

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

### Windows

On Windows, install [MSYS2](https://www.msys2.org), open the **MSYS2 MINGW64**
shell and install the tools once:

```sh
pacman -S make curl zip mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
```

Then, in the Moritz folder, `make` builds `build/windows/Moritz/moritz.exe`
with its folders, and `make run` opens it. The same program can also be
cross-compiled from a Mac (`brew install mingw-w64`) or from Linux
(`sudo apt install mingw-w64`, with the POSIX thread model selected by
`update-alternatives`):

```sh
make windows       # build/windows/Moritz/moritz.exe, with its folders
make windows-zip   # build/Moritz-<version>-windows-x86_64.zip
```

`moritz.exe` is linked statically and needs no DLLs; it runs on Windows 10 and
later. Audio goes through WASAPI and MIDI through the Windows multimedia API.
On Windows there is no virtual MIDI port, so MIDI output goes to a device chosen
in Patch > MIDI Setup....

### Continuous integration

Every push to GitHub builds Moritz on macOS, Linux and Windows
(`.github/workflows/build.yml`), each on its own system with the same
`make` targets described above, and renders every example, help patch,
tutorial and library abstraction with `tools/test-patches.sh`. The builds are kept as downloads of
each run. Pushing a tag `v<version>` also publishes them as a GitHub release.
`tools/test-patches.sh` runs locally too: `tools/test-patches.sh ./moritz`.

### Releases and code signing

`make dist` builds `build/Moritz-<version>-macos-universal.zip`
(`make linux-dist` the Linux `.tar.gz`, `make windows-zip` the Windows zip), which holds
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
  - **Cmd+X**, **Cmd+C** and **Cmd+V** cut, copy and paste boxes with the
    cables between them, through the system clipboard: between windows,
    between patches, from a help patch into yours. Pasted boxes land at the
    mouse; names already used are renamed. **Cmd+D** duplicates the selection.
    The clipboard holds the boxes as `.moritz` text, so a patch fragment can
    also be pasted from a text editor or a web page.
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
- **Scrolling**: when a patch is larger than its window, scrollbars appear at
  the right and bottom; the mouse wheel or the trackpad scrolls too.
- **Several patches**: File > New and File > Open... open each patch in its
  own window. All open patches play at the same time, through the same audio
  output, and share the `send`/`receive` and `buffer~` names, as in Max.
- **Undo/redo**: Cmd+Z and Shift+Cmd+Z, per patch. **Cmd+W** closes the front
  window: a subpatcher, a help patch or the console, or else the patch itself
  (asking to save). On macOS Moritz stays open without windows; elsewhere
  closing the last patch quits.
- **Code view**: the **Code** button in the toolbar, or Patch > Code View
  (**Cmd+T**), shows the patch as code (its text, in the file format) beside the drawing
  (in a subpatcher or help window, that patch). The two are views of the same
  patch: edit the drawing and the text follows; edit the text and, when you
  pause typing, the drawing follows. Boxes whose line you didn't change keep
  running untouched, so the sound goes on; a text that doesn't read as a patch
  is not applied, and the status line says why. Text edits are undone with
  Cmd+Z like any other edit.
- **Tutorials**: Help > Tutorials opens the tutorial patches, in order.
- **Import from Max**: File > Import from Max... (**Shift+Cmd+I**) converts a
  Max patch (`.maxpat` or `.maxhelp`): boxes, arguments, cables, positions,
  subpatchers, sliders and keyboards. Abstractions next to it (`poly~`
  voices, `pfft~` patches, objects named after a `.maxpat`) are converted
  into `.moritz` files beside the original. Objects Moritz doesn't have stay
  as unknown boxes with their text, and the console lists them, together with
  the attributes (`@name value`) left out. The result opens as a new patch, to
  save where you like. `moritz --import patch.maxpat [out.moritz]` does the
  same from the command line.
- **Starting without audio**: `moritz --no-audio` (or
  `open Moritz.app --args --no-audio`) opens with audio off, for a machine
  whose audio device misbehaves.

## Objects

Objects follow their Max/MSP namesakes. Signal objects end in `~`. An
unconnected signal inlet takes a float instead. For control objects, the left
inlet is "hot" (it makes the object output) and the other inlets are "cold"
(they only store a value). The short names `t`, `sel`, `del`, `i`, `f`, `s`
and `r` work too. Numbers follow Max's types: an argument written with a
point (`2.`, `0.5`) makes `+ - * / %`, the comparisons, `pack`, `unpack` and
`scale` work in decimals; an integer argument, or none, makes them truncate.

**Oscillators and generators**

| Object | Description |
| --- | --- |
| `nebula~ [proto1] [proto2]` | Granular synthesis, the author's *nebula*: clouds of windowed grains of two prototypes (`sine`, `triangle`, `square`, `noise` or a `buffer~`), mixed by `morph`. Inlets: on/off, density (grains/s), length (ms), frequency (Hz; for a `buffer~` a speed ratio), morph. Messages: `density d [r]`, `length ms [r]`, `freq f [r]` (the second number is randomness, as a ratio), `gliss g`, `jitter j [r]` (stereo position), `speed s` (through a `buffer~`), `grainlets n` (grains of n periods), `amp a`, `window hann/hamming/blackman`, `poly n`, `proto1 x`, `proto2 x`, `seed n`. Two outlets, left and right |
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
| `filterdesign [type] [freq] [q] [gain]` | The coefficients of a `biquad~` for a filter shape (Robert Bristow-Johnson's cookbook, as Musil's `biquad`): `lowpass`, `highpass`, `bandpass`, `notch`, `peak`, `lowshelf`, `highshelf`, `allpass`. A type, a frequency (left), a Q (middle) or a gain in dB (right) outputs `a0 a1 a2 b1 b2` |
| `filterplot [width height]` | Draws the frequency response (dB on a log frequency axis) of the coefficients it receives; 10, 15 ... numbers are biquads in series. Dragging in it outputs the frequency and gain at the mouse, to drive a `filterdesign` |
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
| `specenv~ [order] [iterations]` | The spectral envelope of each frame (Musil's true envelope: cepstral smoothing iterated so it rides on the harmonic peaks): outlets the envelope (the formants) and the fine structure (the amplitudes divided by it). `order` (the resolution; right inlet) is about sr / (2 f0); `iterations` 0 is the plain cepstral smoothing |
| `pvlock~` | Phase vocoder resynthesis phase with identity phase locking: inlets amplitude, analysed phase, phase difference, reset. Each spectral peak advances by its own frequency and the bins around it keep their analysed offsets from it, so stretched sounds stay clean |
| `sparkle~ [buffer] [channels] [window] [overlap]` | The phase vocoder of the author's *sparkle*, with phase locking around spectral peaks and formant (spectral envelope) preservation. With a `buffer~` it plays it: the left inlet is the speed (1 as recorded, 0 frozen, negative backwards), `start [ms]` or bang plays, `stop`, `loop 1`. Without one it transforms its input, one window late. Inlets 2 and 3: pitch and formant ratios. `envelope C` keeps the formants (C cepstral coefficients, 30-80 for voices); `threshold x` drops weak bins; `robot 1`, `whisper 1`. The last outlet is the read position, 0 to 1. Window 2048, overlap 8 by default Cross-synthesis with a second sound, as in *sparkle*: `cross mode [buffer]`, mode 1 multiplicative (the geometric mean of the two spectra), 2 spectral flattener (the second sound's envelope on the first), 3 morphing; the amount (0 to 1) is inlet 4, and without a `buffer~` the second sound is the signal in inlet 5 |
| `lrshift~ n` | Shifts a frame by `n` bins, to combine bins with their neighbours |
| `phasewrap~` | Wraps a phase into -pi..pi |

**Spatial** (ambisonics as in Musil; AmbiX conventions: azimuth in degrees, 0 in front, positive to the left; elevation positive up; a sound field of order N has (N+1)^2 channels, ACN order, SN3D)

| Object | Description |
| --- | --- |
| `ambienc~ [order] [azimuth] [elevation]` | Encodes a signal at a direction into a sound field (order 1 by default: W Y Z X). Azimuth and elevation are numbers or signals; moving sources glide. Fields add channel by channel |
| `ambirot~ [order]` | Turns a sound field about the vertical axis by the yaw in its last inlet (exactly, per order) |
| `ambidec~ order azimuth ...` | Decodes a sound field to loudspeakers on a horizontal ring, one outlet per azimuth (`ambidec~ 1 45 135 -135 -45` is a square). The exact decoder for an evenly spaced ring of at least 2N+1 speakers, from the horizontal channels, with 2D max-rE weights |
| `binaural~ [order]` | A sound field on headphones: decoded to 17 virtual speakers spread over the sphere (8 at ear height, 4 above, 4 below, 1 overhead), each heard through measured head-related impulse responses (MIT KEMAR, Gardner and Martin 1994). The decoder uses the SN3D normalisation and 3D max-rE weights; everything folds into one filter pair per channel, so the cost does not depend on the number of sources |
| `hrtf~ [azimuth] [elevation]` | A mono sound at a direction on headphones, through the measured head responses themselves: the whole MIT KEMAR set (710 directions, elevation -40 to 90), the four around the direction mixed; a moving direction crossfades within each block. Height is clearer than with `binaural~`, whose blend of responses blurs the outer ear's notches; and its level follows the real head (quieter behind and above), where `binaural~` matches it only in front (up to 8 dB louder overhead) |

A note on the decoders: Musil's `ambi-decode` and `binaural-decode` sample the
field at each speaker without the decoder normalisation (a factor 2 per order
on a ring, 2n+1 on the sphere), which blurs the image: on a ring of eight
speakers its energy vector is 0.57 at order 1 and 0.66 at order 2, against
the theoretical 0.71 and 0.87 that `ambidec~` reaches; its ten virtual
speakers for headphones (none below) also bend directions by up to 16
degrees, 5-7 degrees with `binaural~`'s 17. The spherical harmonics
themselves are exact (checked against the closed forms up to order 3, and
the normalisation up to order 5).

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
sending `; dsp start` or `; dsp stop`. `dspstate~ [ms]` reports the audio
state on bang, whenever audio starts, stops or is reopened, and every `ms`
while audio runs. Its outlets, left to right, are as in Max: 1/0 for audio
on/off, the sample rate, the signal vector size (64, or the frame's bin count
inside `pfft~`) and the I/O buffer size; a fifth outlet gives the CPU load
in %. While audio is off it still reports
the last sample rate and buffer size.

**Offline processing** (background jobs: the patch keeps running while they work; outlets: progress 0..1, then a bang when the result is in its `buffer~`; `stop` cancels)

| Object | Description |
| --- | --- |
| `moritz~ [patch] [channels]` | Renders another patch offline, faster than real time, into a `buffer~`: `open patch.moritz`, `send name value` (sets its `receive name` before every render), `input buffer` (feeds its `adc~`), `render buffer seconds`. Each render starts the patch afresh; it has its own `send`/`receive` and `buffer~` names. Renders asked for while one runs wait their turn |
| `nmf [components] [iterations]` | Non-negative matrix factorisation of the magnitude spectrogram (KL updates, as Musil's). Unsupervised: `run source dest` writes one part per channel of `dest`, resynthesised with the original phases, so the parts add up to the source (mixed to mono). Supervised (Musil's `nmf-learn-parts` and `nmf-separate-with`): `learn example` learns `components` parts of a source from a recording of it alone (once per source), then `separate mix dest` explains a mix with those parts only and writes one source per channel; `forget` clears them. `components`, `iterations`, `size` (FFT), `seed`. Messages sent while it works wait their turn |
| `hpss [kernel] [size]` | Harmonic/percussive separation (Musil's): median filters along time and frequency make soft masks; `run source harmonic percussive` writes two `buffer~`s that add up to the source |

**Learning** (examples are messages: `add x1 x2 ...` or `add x1 x2 ... -> y1 y2 ...`; `clear`, `write file`, `read file`, `print`; a list asks the model)

| Object | Description |
| --- | --- |
| `knn [k]` | k nearest neighbours. Regression (the mean of the k nearest outputs; `weighted 1` weights them by inverse distance, gliding smoothly between examples as Wekinator does) or classification (words as outputs, or `classify 1`). Inputs are standardised (`normalize 0` to turn off). Outlets: the prediction, the nearest example's index |
| `kmeans` | `train k [iterations]` clusters the points (k-means++, best of 10 starts); a list then gives its cluster (0..k-1) and that cluster's centroid. `centroid i`, `seed n`, `normalize 0/1` |
| `pca` | `train [dims]` finds the principal components and outputs the share of variance of each; a list then gives its coordinates along them |
| `zscore` | `train` measures each value's mean and standard deviation; a list then comes out standardised |
| `linreg` | `train` fits y = b0 + b1 x1 + ... by least squares, one set of weights per output; a list then gives the predictions; `weights` outputs them |

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
| `+ - * / % [arg]` | Arithmetic. As in Max, the argument sets the type: with an integer argument (or none) the inputs are truncated and `7 / 2` is 3; with a decimal one (`/ 2.`) it is 3.5. Dividing by 0 gives 0 |
| `== != > < >= <= [arg]` | Comparisons that output 1 or 0, typed like the arithmetic (`> 0` truncates its input, `> 0.` does not) |
| `mtof`, `ftom`, `dbtoa`, `atodb` | MIDI note ↔ Hz and dB ↔ amplitude conversions |
| `scale inlo inhi outlo outhi [exp]` | Maps a range linearly, or along a curve when `exp` is given. Four integer arguments give integer output, as in Max |
| `expr ...` | Expressions over `$f1`..`$f9` and `$i1`..`$i9`, typed as in Max (`7 / 2` is 3, `7. / 2` is 3.5), with `+ - * / % == != < > <= >= && \|\| !`, functions such as `sin`, `pow`, `sqrt`, `min`, `max`, `int` and `random(lo, hi)`, and the constant `pi` |
| `if cond then msg [else msg]` | As in Max: the condition is written as in `expr` (`$i1 > 5`); the messages like message boxes, with the same `$i`/`$f` variables and `$s1`. `out2` sends from a second outlet, `send name` to a `receive`. One inlet per variable; `set` stores without evaluating |
| `random [n]` | Bang outputs a random integer from 0 to n-1. `seed x` makes the sequence repeatable |
| `counter [max]` or `counter min max` | Each bang (or number) outputs the count, then steps it; `max` is included before it wraps back. `reset`, `set n` |

**Routing and messages**

| Object | Description |
| --- | --- |
| `trigger b f i l a s ...` | Outputs right to left, converting the input to the type of each outlet |
| `pack`, `unpack [items]` | Build a list or split one. Each argument gives a slot its type, as in Max: `0` or `i` integer (truncates), `0.` or `f` decimal, `s` symbol; no arguments means two integers |
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
| `inlet`, `inlet~`, `outlet`, `outlet~` | A subpatcher's ports, numbered left to right by position (`in`, `in~`, `out`, `out~` are the same objects). With a number, as Max's `poly~` voices write them (`in 1`, `out~ 2`), they are ordered by that number |
| `poly~ name voices [args]` | `voices` copies of the abstraction `name.moritz`, with their signal outlets summed. `note ...` sends the rest of the message to a free voice, stealing the oldest note when all are busy; `midinote pitch velocity` does the same for a velocity above 0, and sends a velocity of 0 (the note-off) to the voice playing that pitch; `target n` sends later messages to voice `n` only (`0` = all); `mute n 1/0` stops or restarts voice `n`; anything else goes to the target voice or to all |
| `thispoly~` | Inside a `poly~` voice: outputs the voice number at load or on bang. The voice stays busy while the signal in its inlet (the envelope) is not zero; a number sets busy directly |
| any `name` | An abstraction: the patch `name.moritz`, found next to the patch, then in the libraries, then in the bundled examples. Its arguments replace `#1`..`#9` inside it, and `#0` becomes a number unique to each copy, for local `send`/`receive`/`buffer~` names. Saving from its window writes the abstraction's file |

**Interface**

| Object | Description |
| --- | --- |
| `message` | Sends its contents when clicked. `$1`..`$9` are replaced by the incoming values, `,` separates messages, and `; name value` sends to `receive name`. As in Max, the box starts a new line after each `;`, so `; cf 1200; q 8` shows as three lines. When editing, Return finishes and Shift+Return adds a line break; line breaks count as spaces |
| `flonum`, `number` | Number boxes |
| `button`, `toggle` | Bang, and on/off |
| `slider [min max]` | Horizontal slider. As in Max, it outputs integers when both limits are written as integers (`slider 0 127`), and decimals when one has a point (`slider 0. 1.`) |
| `comment` | A text note |
| `function [domain] [lo] [hi]` | Breakpoint editor: in locked mode click to add a point, drag it, Shift-click to delete it. bang outputs the curve for `line~` (the first value, then value-time pairs over `domain` ms); a number (ms) outputs the value there. `domain`, `range`, `clear`, `set x y ...`; the points are saved with the patch |
| `kslider [octaves] [lowest] [mode]` | Piano keyboard (4 octaves from note 36). Outlets: pitch and velocity; the velocity depends on where you click (louder lower down). Mode 0 plays while the mouse is down and sends velocity 0 on release (drag to glide); mode 1 toggles keys, for chords. A pitch in the left inlet lights its key and passes through with the velocity from the right inlet; `set p v` only lights it, `flush` releases every held key, `clear` unlights them all without output |

**Library** (abstractions in `library/`, usable in any patch)

| Object | Description |
| --- | --- |
| `reverb~ [mix] [T60]` | A stereo Freeverb. Inlets: the signal, wet/dry mix (0-1, default 0.3) and decay time T60 in seconds (0.1-100, default 2). Outlets: left and right. Per channel, eight damped combs (`fvcomb~`) in parallel, then four `allpass~` in series; the right channel's delays are 23 samples longer, so the outputs are decorrelated. Each comb's feedback is `g = 10^(-3 d / T60)` for its delay `d` in seconds, so low frequencies fall by 60 dB in T60 seconds, and highs a little faster (damping) |
| `fvcomb~ ms name` | One Freeverb comb (feedback with a damping low-pass), used by `reverb~`; its T60 arrives on `name-t60` |
| `impose~ [order]` | The carrier (right inlet) with the spectral envelope of the modulator (left): an orchestra or a synth that speaks with a voice's formants (Musil's `impose-envelope`). Third inlet: the envelope order (60). Built from `specenv~` in `pfft~` (`impose-fx`) |
| `binpan~ [azimuth] [elevation]` | A mono sound placed around the listener on headphones: `ambienc~ 2` into `binaural~ 2`. Inlets: signal, azimuth, elevation; outlets: left, right |
| `pitchshift~ [semitones]` | A live pitch shifter keeping the formants: `sparkle~` without a buffer~. Inlets: signal, semitones, formant preservation 1/0 |

As in Musil, where the C++ core gives the primitives and the language
builds on them, the library holds what is made of other objects: open any
of them (double-click) to see how it works.

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

`examples/` has thirty-two demos, plus the abstractions they use. They are also inside the app bundle, and
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
- `filters.moritz`: `svf~` swept by a `phasor~` LFO in the signal domain;
  then filter design: `filterdesign` → `biquad~` on noise or Beethoven, its
  response drawn by `filterplot` (drag on it to move the filter).
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
  frequencies; `selector~` picks the source, `dspstate~` shows the sample
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
- `sparkle-cross.moritz`: `sparkle~`'s three cross-syntheses: a voice
  through drums (multiplicative), Beethoven's 7th with the voice's formants
  (spectral flattener), and a morph from the voice to a gong.
- `pitch-shift.moritz`: transposing a voice with `sparkle~`, with and without
  formant preservation, moving the formants alone, robot and whisper,
  denoising; the same on the live input.
- `spectral-envelope.moritz`: `specenv~` in `pfft~` (`specenv-fx.moritz`):
  the voice's envelope on noise, a saw chord or Beethoven (a whisper, a
  vocoder, an orchestra that speaks), and the other way round.
- `nebula.moritz`: `nebula~` presets: sine clouds, glass rain, grainlets, the
  voice as a cloud, a morph from a sine to a gong.
- `ambisonics-order2.moritz`: order 1 against order 2: the same moving
  voice decoded to a ring of eight speakers (two rows of meters, order 2
  visibly narrower) and on headphones, switching between the orders.
- `ambisonics.moritz`: two sources (one circling) in a first-order sound
  field, turned with `ambirot~` and decoded to four speakers.
- `binaural.moritz`: the same scene on headphones with `binaural~`, the drums
  also moving up and down.
- `hrtf.moritz`: `hrtf~`: a voice or noise bursts flying around and above
  the listener, or placed by hand; switch to an order-2 ambisonic rendering
  to hear how much clearer height is with the direct responses.
- `nmf-supervised.moritz`: source separation by `nmf`: a mix of voice and
  drums recorded, then separated blindly (`run`) and with the sources learned
  first (`learn`, `separate`).
- `condec.moritz`: `condec` offline: a voice convolved with the
  Concertgebouw and recovered by deconvolution, then made to ring through a
  gong, whose sound deconvolution recovers from the result.
- `convolution.moritz`: `conv~` with three impulse responses, the
  Concertgebouw (`hall.wav`, stereo), a bell and a gong, on a voice,
  Beethoven's Seventh or drums, with a wet/dry mix.
- `follower.moritz`: `descriptors~` follows a voice (`Vox.wav`); its f0, centroid and
  level drive a filtered sawtooth that imitates it.
- `bounce.moritz`: `moritz~` renders `bounce-source.moritz` (generative FM
  bells) offline four times, with different seeds, pitches and speeds, into
  four `buffer~`s to play and compare.
- `separation.moritz`: `hpss` splits `hpss_demo.wav` into its sustained and
  its percussive parts, and `nmf` a mix of four stems (`stems_demo.wav`:
  drums, bass, chords, melody) into four parts, as background jobs.
- `mapping.moritz`: `knn` learns four FM sounds placed on an x/y plane and
  glides between them as the sliders move; record your own, save them.
- `clusters.moritz`: `kmeans` learns the sounds of a drum loop by itself
  (from the descriptors of its frames, in two passes), then classifies every
  hit and replays the loop with synthesized drums: the brightness of each
  cluster picks kick, snare or hat.
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
`gong_c_sharp.wav`, `hpss_demo.wav`, `stems_demo.wav` (a mix of four stems),
and `hall.wav`, a stereo impulse response of the
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
- `objects_offline.cpp`: `moritz~`, `nmf` and `hpss`, which run as
  background jobs (`jobs.h`, `jobs.cpp`: a worker thread per job, results
  handed back to the GUI thread). `objects_learn.cpp`: `knn`, `kmeans`,
  `pca`, `zscore` and `linreg`.
- `objects_sound.cpp`: `sparkle~`, `conv~`, `condec`, `descriptors~`,
  `function` and `nebula~`, adapted from the author's *sound_math* tools
  (sparkle, pconv, condec, features, nebula); `vocoder.h` is sparkle's phase
  vocoder (with its cross-syntheses) and `partconv.h` the zero-latency
  convolver.
- `objects_spectral.cpp` (`specenv~`), `objects_filters.cpp`
  (`filterdesign`, `filterplot`) and `objects_spatial.cpp` (ambisonics and
  `binaural~`, `hrtf~`; `hrtf_kemar.cpp` holds the head-related impulse responses), all
  following Musil's signals library.
- `audio.cpp`: the miniaudio engine. `midi.h`, `midi.cpp`: MIDI through
  RtMidi; its callbacks queue the messages for the GUI thread.
- `gui.h`, `theme.h`, `canvas.cpp`, `mainwindow.cpp`: the FLTK editor: the
  `App` (audio, MIDI, console, help windows, the open patches), a window per
  patch with its toolbar, and subpatcher windows. `textview.cpp` is the text
  view of a patch.
- `maximport.cpp`, `json.h`: Import from Max.
- `main.cpp`: `main` and the command-line modes (`--render`, `--import`).

`library/` holds the abstractions usable in any patch (`reverb~`), `help/`
the help patches, `name.help.moritz`, with their helper abstractions
(`help-*.moritz`), and `tutorials/` the tutorial patches. They are copied
into the app bundle.

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
owns one per voice. An invisible host
patch owns everything shared (the DSP lock, the scheduler, audio I/O, and
the `send`/`receive` and `buffer~` names, which are global as in Max); every
open patch and help patch runs as one of its "extras", and
an inner patch's signal chain runs inside its owner box. `pfft~` buffers
its input, and every hop it runs the inner chain once over the whole frame
(the inner patch's vector size is fftsize/2), then overlap-adds the
inverse FFT.

To run a patch offline for testing, use
`moritz --render patch.moritz seconds [out.wav] [bang=<id>] [send=<id>:<message>] [png=<file>] [edit] [extra=<patch>] ...`
(`bang=<id>@<seconds>` and `send=<id>:<message>@<seconds>` act at that time in the render)
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

`hrtf~` and `binaural~` use the MIT KEMAR head-related impulse responses (compact set, all 710 directions, in `src/hrtf_kemar.cpp`) by
Bill Gardner and Keith Martin, MIT Media Laboratory (1994), "HRTF Measurements
of a KEMAR Dummy-Head Microphone", as distributed with Musil.

The drawing of Max and Moritz in `resources/maxundmoritz.png` is Wilhelm
Busch's own, from *Max und Moritz* (1865); it is in the public domain (scan
from Wikimedia Commons, "Max und Moritz (Busch) 001.png").
