# Moritz objects

Every object, by category. Each one has a help patch: right-click a box (or
Option-click it in locked mode) and choose Help; the browser (Help > Browser)
lists them all with a search box.

- [Oscillators and generators](#oscillators-and-generators)
- [Filters](#filters)
- [Signal math](#signal-math)
- [Signal routing](#signal-routing)
- [Delay](#delay)
- [Samples](#samples)
- [Spectral](#spectral)
- [Spatial](#spatial)
- [Analysis](#analysis)
- [Music information retrieval](#music-information-retrieval)
- [Audio I/O](#audio-io)
- [Offline processing](#offline-processing)
- [Learning](#learning)
- [MIDI](#midi)
- [Timing](#timing)
- [Control math](#control-math)
- [Routing and messages](#routing-and-messages)
- [Patching](#patching)
- [Interface](#interface)
- [Library](#library)

Objects follow their Max/MSP namesakes. Signal objects end in `~`. An
unconnected signal inlet takes a float instead. For control objects, the left
inlet is "hot" (it makes the object output) and the other inlets are "cold"
(they only store a value). The short names `t`, `sel`, `del`, `i`, `f`, `s`
and `r` work too. Numbers follow Max's types: an argument written with a
point (`2.`, `0.5`) makes `+ - * / %`, the comparisons, `pack`, `unpack` and
`scale` work in decimals; an integer argument, or none, makes them truncate.

## Oscillators and generators

| Object | Description |
| --- | --- |
| `nebula~ [proto1] [proto2]` | Granular synthesis, the author's *nebula*: clouds of windowed grains of two prototypes (`sine`, `triangle`, `square`, `noise` or a `buffer~`), mixed by `morph`. Inlets: on/off, density (grains/s), length (ms), frequency (Hz; for a `buffer~` a speed ratio, 1 = as recorded, as the original's relative pitch), morph. Messages: `density d [r]`, `length ms [r]`, `freq f [r]` (the second number is randomness, as a ratio), `gliss g`, `jitter j [r]` (stereo position), `speed s` (through a `buffer~`), `grainlets n` (grains of n periods), `amp a`, `window hann/hamming/blackman`, `poly n`, `proto1 x`, `proto2 x`, `pitch1 hz`, `pitch2 hz` (a sound prototype's pitch: its frequency is then in Hz too, so a waveform and a sound morph at the same pitch), `seed n`. Two outlets, left and right |
| `cycle~ [freq]` | Cosine oscillator (512-point table, interpolated). The right inlet sets the phase |
| `phasor~ [freq]` | Ramp from 0 to 1, for LFOs and as a position signal |
| `saw~`, `rect~ [freq duty]`, `tri~ [freq slope]` | Band-limited sawtooth and pulse (PolyBLEP), and a triangle |
| `noise~` | White noise |
| `click~ [shape...]` | Outputs an impulse on bang. A list sets the impulse shape |
| `sig~ [value]` | Turns a number into a signal |
| `line~` | Signal ramps. `1 10 0 300` goes to 1 in 10 ms, then to 0 in 300 ms. A float alone jumps, or ramps over the time given in the right inlet |

## Filters

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

## Signal math

| Object | Description |
| --- | --- |
| `+~ -~ *~ /~ [arg]` | Signal arithmetic |
| `clip~ [min max]`, `abs~`, `tanh~` | Limiting, rectifying and soft saturation |
| `gain~ [ramp amplitude]` | A fader that scales the signal in its left inlet. Every change ramps linearly over `ramp` ms (20 by default), so it never clicks. The fader runs in dB from silence to +6 dB; a number sets the amplitude (1 = unity), the right inlet sets the ramp time, the right outlet reports the amplitude |
| `expr~ ...` | `expr` evaluated per sample. `$v1`..`$v9` are the inlets, which take signals or floats |
| `sah~ [threshold]` | Sample and hold: keeps the left signal each time the right one rises above the threshold |

## Signal routing

| Object | Description |
| --- | --- |
| `selector~ n [initial]` | Passes one of `n` signal inlets; the left inlet picks 1..`n` (0 = none). Switching crossfades over 5 ms, so it never clicks |
| `gate~ n [initial]` | Sends the signal in its right inlet to one of `n` outlets, picked by the left inlet (0 = closed); also crossfades over 5 ms |
| `send~ name`, `receive~ name` | Signals by name, across subpatchers. `receive~` sums every `send~` of its name; `set name` changes it. The chain runs `send~` first, so there is no delay, except around a feedback loop through them (one 64-sample block) |

## Delay

| Object | Description |
| --- | --- |
| `tapin~ [max ms]` | Writes a delay line. Connect its outlet to `tapout~` |
| `tapout~ [ms ...]` | One tap per argument; each inlet sets that tap's delay. A feedback loop from `tapout~` back to `tapin~` is allowed, with a minimum delay of 64 samples |
| `comb~ [max delay a b c]` | Comb filter: `y = a x + b x[n-D] + c y[n-D]`. Like `tapin~` and `allpass~`, its memory is sized for 96 kHz, so at lower rates delays a little beyond `max` still work |
| `allpass~ [max delay gain]` | Allpass filter, for reverbs |

## Samples

Sound files: WAV, FLAC and MP3 (written as WAV). Paths are relative to the patch file.

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

## Spectral

Inside a `pfft~` patch each signal vector is one whole frame: one bin per sample.

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

## Spatial

Ambisonics as in Musil, with the AmbiX conventions: azimuth in degrees, 0 in front, positive to the left; elevation positive up; a sound field of order N has (N+1)^2 channels, in ACN order, SN3D.

| Object | Description |
| --- | --- |
| `ambienc~ [order] [azimuth] [elevation]` | Encodes a signal at a direction into a sound field (order 1 by default: W Y Z X). Azimuth and elevation are numbers or signals; moving sources glide. Fields add channel by channel |
| `ambirot~ [order]` | Turns a sound field about the vertical axis by the yaw in its last inlet (exactly, per order) |
| `ambidec~ order azimuth ...` | Decodes a sound field to loudspeakers on a horizontal ring, one outlet per azimuth (`ambidec~ 1 45 135 -135 -45` is a square). The exact decoder for an evenly spaced ring of at least 2N+1 speakers, from the horizontal channels, with 2D max-rE weights |
| `binaural~ [order]` | A sound field on headphones: decoded to 17 virtual speakers spread over the sphere (8 at ear height, 4 above, 4 below, 1 overhead), each heard through measured head-related impulse responses (MIT KEMAR, Gardner and Martin 1994). The decoder uses the SN3D normalisation and 3D max-rE weights; everything folds into one filter pair per channel, so the cost does not depend on the number of sources |
| `hrtf~ [azimuth] [elevation]` | A mono sound at a direction on headphones, through the measured head responses themselves: the whole MIT KEMAR set (710 directions, elevation -40 to 90), the four around the direction mixed; a moving direction crossfades within each block. Height is clearer than with `binaural~`, whose blend of responses blurs the outer ear's notches; and its level follows the real head (quieter behind and above), where `binaural~` matches it only in front (up to 8 dB louder overhead) |

## Analysis

| Object | Description |
| --- | --- |
| `snapshot~ [ms]` | Outputs the signal's current value on bang, or every `ms` |
| `peakamp~ [ms]` | Outputs the peak level since the last report |
| `meter~`, `scope~ [samples]` | Level meter and a triggered oscilloscope, drawn live |
| `descriptors~ [size] [hop] [names...]` | Low-level descriptors every `hop` samples (1024) over `size` (2048): `rms centroid spread skewness kurtosis flux hfc zcr flatness periodicity f0`, one outlet each, in the order named (all by default). Centroid and spread are in Hz, zcr in crossings per second; f0 comes from the autocorrelation (0 when unvoiced) and periodicity is the height of its peak. Formulas from *sound_math*'s `features.h` |
| `spectroscope~ [width height]` | Live spectrum: 2048-point FFT, log frequency from 20 Hz to half the sample rate, 0 dB (a full-scale sine) down to -90 dB |
| `melbands~ [bands] [size] [hop]`, `mfcc~ [coeffs] [bands] [size] [hop]`, `chroma~ [size] [hop]` | Live versions of `melgram`, `mfccgram` and `chromagram` (below): a list every `hop` samples, while audio runs (bang: now). Into a `plot` in image mode, the lists scroll by as a picture |
| `onset~ [threshold] [size] [hop]` | A bang at every onset of its signal: the spectral flux of each frame (1024 samples, every 256) above `threshold` (2) times its median over the last 21 frames, at least `gap` ms apart (50), while the level is above `gate` dB (-50). Right outlet: the flux |

## Music information retrieval

After Musil; background jobs on a `buffer~`, like `nmf`: outlets progress and done, then the results; `size` and `hop` set the analysis window and step, `stop` cancels. A feature matrix is stored in a `buffer~`: one channel a row (a bin, a band, a coefficient, a pitch class), one sample a frame, and the buffer's sample rate is the number of frames a second, so `plot` shows its time axis. Times are in ms.

| Object | Description |
| --- | --- |
| `spectrogram [size] [hop]` | `run sound matrix`: the magnitude spectrogram in dB (0 dB: a full-scale sine; -100 at most), a channel a bin, up to `maxfreq` Hz |
| `melgram [bands] [size] [hop]`, `mfccgram [coeffs] [bands] [size] [hop]` | `run sound matrix`: the energy in `bands` (40) triangular bands on the mel scale (dB), or the first `coeffs` (13) cepstral coefficients of those bands (their cosine transform: the timbre in a few numbers; 0 is the level) |
| `chromagram [size] [hop]` | `run sound matrix`: 12 channels, C first: how much of each pitch class sounds, each frame scaled to a maximum of 1. Read from the spectral peaks (each tone counts once, as in HPCP), each given to its nearest note from C2 to B6; a peak that is probably the 3rd, 5th, 6th or 7th partial of a lower one counts a tenth, otherwise brass and strings pull the harmony a fifth up |
| `onsets [threshold] [size] [hop]` | `run sound [curve]`: the onset times (ms, a list), the peaks of the spectral flux above `threshold` times its moving median, each placed where the energy jumps (Musil's `onsets-adaptive`). The onsets cut the sound into segments: `segment k` outputs the start and end of the k-th, ready for `play~`; the next outlet gives their number |
| `beats [size] [hop]` | `run sound [curve]`: the tempo (bpm) and the beat times (ms): the autocorrelation of the onset strength weighted by a prior on the tempo people tap (`prior 100 0.6`: centre and width in octaves), then the beats by dynamic programming (`tightness 100`; Ellis 2007). `clicks buffer` writes a click track to play with the sound; `tempogram sound matrix` writes how periodic the onsets are at every tempo, 4 times a second (one channel a bpm, 30 to 300) |
| `keyfind [size] [hop]` | `run sound [from to]`: the key ("Eb major"), its correlation, the pitch-class distribution (12 values) and the correlation with all 24 keys, from Krumhansl and Kessler's profiles (Krumhansl 1990) |
| `similarity [frames]` | `run features matrix`: the self-similarity matrix of a feature matrix (cosine similarity of every frame with every other; Foote 1999), frames averaged to at most `frames` (1000). `standardize 1` scales each feature first (for MFCCs) |
| `novelty [width] [threshold]` | `run similarity curve`: Foote's novelty (2000), a checkerboard kernel of `width` frames along the diagonal, and the section boundaries (ms): its peaks above `threshold` times its moving median, at least `gap` ms apart. `section k` outputs the start and end of the k-th section |
| `curves [size] [hop]` | `loudness sound curve` (dB, as the ear weighs it roughly), `register sound curves` (3 channels: centroid and lowest strong partial in MIDI pitch, spread in octaves), `polyphony sound curve` (how many voices seem to sound: a rough estimate); `smooth n` applies a moving median of n frames |

Checked on recordings: `keyfind` names the keys of the march (E-flat major, its trio A-flat), the two preludes (C minor, D major) and the Beethoven Allegretto (A minor); `beats` finds the drums at 119 bpm and the march at 116; `novelty` finds the march's strains and trios.

## Audio I/O

`adc~ [ch...]` and `dac~ [ch...]`. The default channels are 1
and 2. `start` and `stop` turn audio on and off, and so does a message box
sending `; dsp start` or `; dsp stop`. `dspstate~ [ms]` reports the audio
state on bang, whenever audio starts, stops or is reopened, and every `ms`
while audio runs. Its outlets, left to right, are as in Max: 1/0 for audio
on/off, the sample rate, the signal vector size (64, or the frame's bin count
inside `pfft~`) and the I/O buffer size; a fifth outlet gives the CPU load
in %. While audio is off it still reports
the last sample rate and buffer size.

## Offline processing

Background jobs: the patch keeps running while they work. Outlets: progress 0..1, then a bang when the result is in its `buffer~`; `stop` cancels.

| Object | Description |
| --- | --- |
| `moritz~ [patch] [channels]` | Renders another patch offline, faster than real time, into a `buffer~`: `open patch.moritz`, `send name value` (sets its `receive name` before every render), `input buffer` (feeds its `adc~`), `render buffer seconds`. Each render starts the patch afresh; it has its own `send`/`receive` and `buffer~` names. Renders asked for while one runs wait their turn |
| `nmf [components] [iterations]` | Non-negative matrix factorisation of the magnitude spectrogram (KL updates, as Musil's). Unsupervised: `run source dest` writes one part per channel of `dest`, resynthesised with the original phases, so the parts add up to the source (mixed to mono). Supervised (Musil's `nmf-learn-parts` and `nmf-separate-with`): `learn example` learns `components` parts of a source from a recording of it alone (once per source), then `separate mix dest` explains a mix with those parts only and writes one source per channel; `forget` clears them. `components`, `iterations`, `size` (FFT), `seed`. Messages sent while it works wait their turn |
| `hpss [kernel] [size]` | Harmonic/percussive separation (Musil's): median filters along time and frequency make soft masks; `run source harmonic percussive` writes two `buffer~`s that add up to the source |

## Learning

Examples are messages: `add x1 x2 ...` or `add x1 x2 ... -> y1 y2 ...`; `clear`, `write file`, `read file`, `print`; a list asks the model.

| Object | Description |
| --- | --- |
| `knn [k]` | k nearest neighbours. Regression (the mean of the k nearest outputs; `weighted 1` weights them by inverse distance, gliding smoothly between examples as Wekinator does) or classification (words as outputs, or `classify 1`). Inputs are standardised (`normalize 0` to turn off). Outlets: the prediction, the nearest example's index |
| `kmeans` | `train k [iterations]` clusters the points (k-means++, best of 10 starts); a list then gives its cluster (0..k-1) and that cluster's centroid. `centroid i`, `seed n`, `normalize 0/1` |
| `pca` | `train [dims]` finds the principal components and outputs the share of variance of each; a list then gives its coordinates along them |
| `zscore` | `train` measures each value's mean and standard deviation; a list then comes out standardised |
| `linreg` | `train` fits y = b0 + b1 x1 + ... by least squares, one set of weights per output; a list then gives the predictions; `weights` outputs them |

## MIDI

Devices in Patch > MIDI Setup; channels 1-16.

| Object | Description |
| --- | --- |
| `notein [channel]` | Outlets: pitch, velocity (0 = note-off), and the channel when there is no argument |
| `ctlin [controller] [channel]` | Controller values 0-127; outlets for the controller number and channel when they are not given |
| `bendin [channel]`, `pgmin [channel]` | Pitch bend as 0-127 (64 = centre), program changes as 1-128 |
| `noteout [channel]`, `ctlout [controller] [channel]` | Send notes (inlets pitch, velocity, channel) and controllers (value, controller, channel) |
| `makenote [velocity] [duration]` | A pitch goes out at once with the velocity, and again with velocity 0 after `duration` ms; `stop` sends pending note-offs, `clear` drops them |
| `stripnote` | Passes note-ons only |

## Timing

| Object | Description |
| --- | --- |
| `metro ms` | Repeating bang: `1` or bang starts it, `0` or `stop` stops it. Ticks are scheduled at absolute times, so they don't drift |
| `delay ms` | Delayed bang. A new bang restarts it, and `stop` cancels it |
| `line [start grain]` | Control ramps: `target time` outputs a value every `grain` ms (20 by default). The middle inlet gives the time for the next single number; `stop`, `set x` |
| `loadbang` | Bangs when the patch opens (double-click it in locked mode to fire it again) |

## Control math

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

## Routing and messages

| Object | Description |
| --- | --- |
| `trigger b f i l a s ...` | Outputs right to left, converting the input to the type of each outlet |
| `pack`, `unpack [items]` | Build a list or split one. Each argument gives a slot its type, as in Max: `0` or `i` integer (truncates), `0.` or `f` decimal, `s` symbol; no arguments means two integers |
| `prepend args...`, `append args...` | Put words before or after every message (`prepend set` shows a list in a message box); the right inlet replaces them |
| `route a b ...` | Sends a message out the outlet matching its first word, with that word removed. Unmatched messages go out the last outlet |
| `select a b ...` | Bangs the outlet matching the input; other values pass through the last outlet |
| `gate [n] [open]` | The left inlet picks the outlet (0 closes it); the right inlet is the data |
| `int`, `float` | Store a number; bang outputs it |
| `send name`, `receive name` | Wireless messages |
| `print [name]` | Prints to the console |

## Patching

| Object | Description |
| --- | --- |
| `p name` | Subpatcher, saved inside the parent's file |
| `inlet`, `inlet~`, `outlet`, `outlet~` | A subpatcher's ports, numbered left to right by position (`in`, `in~`, `out`, `out~` are the same objects). With a number, as Max's `poly~` voices write them (`in 1`, `out~ 2`), they are ordered by that number |
| `poly~ name voices [args]` | `voices` copies of the abstraction `name.moritz`, with their signal outlets summed. `note ...` sends the rest of the message to a free voice, stealing the oldest note when all are busy; `midinote pitch velocity` does the same for a velocity above 0, and sends a velocity of 0 (the note-off) to the voice playing that pitch; `target n` sends later messages to voice `n` only (`0` = all); `mute n 1/0` stops or restarts voice `n`; anything else goes to the target voice or to all |
| `thispoly~` | Inside a `poly~` voice: outputs the voice number at load or on bang. The voice stays busy while the signal in its inlet (the envelope) is not zero; a number sets busy directly |
| any `name` | An abstraction: the patch `name.moritz`, found next to the patch, then in the libraries, then in the bundled examples. Its arguments replace `#1`..`#9` inside it, and `#0` becomes a number unique to each copy, for local `send`/`receive`/`buffer~` names. Saving from its window writes the abstraction's file |

## Interface

| Object | Description |
| --- | --- |
| `message` | Sends its contents when clicked. `$1`..`$9` are replaced by the incoming values, `,` separates messages, and `; name value` sends to `receive name`. As in Max, the box starts a new line after each `;`, so `; cf 1200; q 8` shows as three lines. When editing, Return finishes and Shift+Return adds a line break; line breaks count as spaces |
| `flonum`, `number` | Number boxes |
| `button`, `toggle` | Bang, and on/off |
| `slider [min max]` | Horizontal slider. As in Max, it outputs integers when both limits are written as integers (`slider 0 127`), and decimals when one has a point (`slider 0. 1.`) |
| `comment` | A text note |
| `plot [buffer] [width height] [mode]` | Curves, bars, scatter plots and matrices (after Musil's figures). It shows a `buffer~` (followed as it changes: its channels are rows, its sample rate the time axis) or the lists and numbers it receives: a list is a curve (or x y pairs in scatter mode, or a new column of a scrolling image in image mode), a number scrolls a curve. Modes `auto` (curves up to 4 rows, an image above), `line`, `bars`, `scatter`, `image` (row 0 at the bottom, viridis or gray). `range lo hi`, `xrange from to`, `rows first last` (the y axis of an image), `normalize 1` (each row on its own scale), `marks` (also the right inlet: vertical lines, ms on a time axis), `cursor ms`, `title`, `labels` (`labels chroma`, `labels keys`), `history n`, `clear`, `export file.png [w h]`. A click outputs x (ms on a time axis) and y. Double-click opens a window: the wheel zooms (Shift: vertically), dragging moves, values under the mouse, r resets, e exports a PNG |
| `function [domain] [lo] [hi]` | Breakpoint editor: in locked mode click to add a point, drag it, Shift-click to delete it. bang outputs the curve for `line~` (the first value, then value-time pairs over `domain` ms); a number (ms) outputs the value there. `domain`, `range`, `clear`, `set x y ...`; the points are saved with the patch |
| `kslider [octaves] [lowest] [mode]` | Piano keyboard (4 octaves from note 36). Outlets: pitch and velocity; the velocity depends on where you click (louder lower down). Mode 0 plays while the mouse is down and sends velocity 0 on release (drag to glide); mode 1 toggles keys, for chords. A pitch in the left inlet lights its key and passes through with the velocity from the right inlet; `set p v` only lights it, `flush` releases every held key, `clear` unlights them all without output |

## Library

Abstractions in `library/`, usable in any patch like built-in objects.

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
