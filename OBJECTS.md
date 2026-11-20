# Moritz objects

Every object, by category, with its arguments, messages, inlets and outlets. The same reference is in
Moritz itself: right-click a box and choose Reference, or press Reference in a help window. This file
is generated from `help/reference/` by `moritz --reference-md`.

- [Oscillators](#oscillators)
- [Filters](#filters)
- [Signal Math](#signal-math)
- [Signal Routing](#signal-routing)
- [Delay](#delay)
- [Samples](#samples)
- [Spectral](#spectral)
- [Spatial](#spatial)
- [Analysis](#analysis)
- [Atoms and types](#atoms-and-types)
- [Learning](#learning)
- [I/O](#io)
- [MIDI](#midi)
- [Timing](#timing)
- [Control Math](#control-math)
- [Routing](#routing)
- [Patching](#patching)
- [Interface](#interface)
- [Code](#code)
- [Library](#library)

Objects follow their Max/MSP namesakes. Signal objects end in `~`. An unconnected signal inlet
takes a float instead. For control objects, the left inlet is "hot" (it makes the object output)
and the other inlets are "cold" (they only store a value). Numbers follow Max's types: an argument
written with a point (`2.`, `0.5`) makes `+ - * / %`, the comparisons, `pack`, `unpack` and `scale`
work in decimals; an integer argument, or none, makes them truncate.

## Oscillators

### `cycle~`

`cycle~ [freq] [phase]`

Cosine oscillator, read from a 512-point table with linear interpolation, as in Max. The left inlet takes the frequency as a signal or a float; a float in the right inlet restarts the cycle at that phase.

| Argument | |
| --- | --- |
| `freq` | the frequency in Hz (0) |
| `phase` | the starting phase, 0-1 (0) |

| Inlet | |
| --- | --- |
| 1 | frequency in Hz (signal or float) |
| 2 | phase 0-1: restarts the cycle there |

| Outlet | |
| --- | --- |
| 1 | cosine wave, -1 to 1 |

See also: `phasor~` `saw~` `rect~` `tri~` `sig~`

### `phasor~`

`phasor~ [freq]`

A ramp from 0 to 1, repeating at the frequency in the left inlet (signal or float). It is not band-limited: use it as an LFO, a position signal or an index, not as an audible sawtooth (see saw~). A float in the right inlet restarts the ramp at that phase (0-1).

| Argument | |
| --- | --- |
| `freq` | the frequency in Hz (0) |

| Inlet | |
| --- | --- |
| 1 | frequency in Hz (signal or float) |
| 2 | phase 0-1: restarts the ramp there |

| Outlet | |
| --- | --- |
| 1 | ramp from 0 to 1 |

See also: `cycle~` `saw~` `line~` `index~`

### `saw~`

`saw~ [freq]`

Band-limited sawtooth (PolyBLEP), from -1 to 1. The frequency is a signal or a float.

| Argument | |
| --- | --- |
| `freq` | the frequency in Hz (0) |

| Inlet | |
| --- | --- |
| 1 | frequency in Hz (signal or float) |

| Outlet | |
| --- | --- |
| 1 | band-limited sawtooth |

See also: `rect~` `tri~` `phasor~` `cycle~`

### `rect~`

`rect~ [freq] [duty]`

Band-limited pulse (PolyBLEP), from -1 to 1. The right inlet sets the duty cycle, the fraction of the period spent at 1 (clamped to 0.01-0.99); both inlets take signals or floats.

| Argument | |
| --- | --- |
| `freq` | the frequency in Hz (0) |
| `duty` | the duty cycle, 0-1 (0.5) |

| Inlet | |
| --- | --- |
| 1 | frequency in Hz (signal or float) |
| 2 | duty cycle 0-1 (pulse width) |

| Outlet | |
| --- | --- |
| 1 | band-limited pulse |

See also: `saw~` `tri~` `cycle~`

### `tri~`

`tri~ [freq] [slope]`

Triangle wave from -1 to 1. The slope is where in the period the wave turns from rising to falling (clamped to 0.01-0.99): 0.5 is symmetric, values near 0 or 1 make a sawtooth. Both inlets take signals or floats.

| Argument | |
| --- | --- |
| `freq` | the frequency in Hz (0) |
| `slope` | the turning point, 0-1 (0.5) |

| Inlet | |
| --- | --- |
| 1 | frequency in Hz (signal or float) |
| 2 | slope 0-1 (0.5 = symmetric) |

| Outlet | |
| --- | --- |
| 1 | triangle wave |

See also: `saw~` `rect~` `cycle~`

### `noise~`

`noise~`

White noise between -1 and 1. Each noise~ has its own seed, so two of them are uncorrelated.

| Inlet | |
| --- | --- |
| 1 | no messages |

| Outlet | |
| --- | --- |
| 1 | white noise, -1 to 1 |

See also: `cycle~` `reson~` `svf~` `nebula~`

### `click~`

`click~ [shape...]`

Plays an impulse each time it gets a bang: by default a single sample of 1, or the shape given as arguments or as a list of numbers (optionally after `set`). An empty list returns to the single sample.

| Argument | |
| --- | --- |
| `shape` | the samples of the impulse (1) |

| Message | |
| --- | --- |
| `bang` | plays the impulse |
| `set values...` | sets the impulse shape (a list of numbers alone does the same) |

| Inlet | |
| --- | --- |
| 1 | bang: one impulse; a list sets the impulse shape |

| Outlet | |
| --- | --- |
| 1 | impulse |

See also: `line~` `comb~` `reson~` `sig~`

### `sig~`

`sig~ [value]`

Turns a number into a constant signal. A signal in its inlet passes through unchanged.

| Argument | |
| --- | --- |
| `value` | the starting value (0) |

| Inlet | |
| --- | --- |
| 1 | number to turn into a signal |

| Outlet | |
| --- | --- |
| 1 | constant signal |

See also: `line~` `snapshot~` `+~`

### `line~`

`line~ [value]`

Signal ramps. A list of target-time pairs makes segments: `1 10 0 300` goes to 1 in 10 ms, then to 0 in 300 ms (a list as long as you like). A float alone jumps to that value, or ramps to it over the time last sent to the right inlet, which is used once and then reset to 0. A new message drops the segments still pending and starts from the current value.

| Argument | |
| --- | --- |
| `value` | the starting value (0) |

| Message | |
| --- | --- |
| `value [ms]` | jumps or ramps to the value |
| `target ms target ms ...` | a sequence of ramps |
| `stop` | freezes the ramp where it is |

| Inlet | |
| --- | --- |
| 1 | ramps: "1 10 0 300" goes to 1 in 10 ms, then to 0 in 300 ms; a float jumps |
| 2 | ramp time (ms) for the next single float |

| Outlet | |
| --- | --- |
| 1 | ramp signal |

See also: `line` `function` `sig~` `gain~`

### `nebula~`

`nebula~ [proto1] [proto2]`

Granular synthesis (sound_math's nebula; the engine of Musil's nebula): clouds of windowed grains read from two prototypes, each a waveform (`sine`, `triangle`, `square`, `noise`) or the name of a buffer~, mixed by `morph`. A grain starts every 1/density seconds, lasts `length` ms and is placed at random in the stereo field; the outlets are left and right.

For a waveform the frequency is in Hz; for a buffer~ it is a speed ratio (1 = as recorded), unless `pitch1` or `pitch2` gives the sound's pitch, so that a waveform and a sound can be morphed at the same pitch. The second number after density, length, freq and jitter is randomness, as a ratio of the value: `density 40 .5` gives 20 to 60 grains a second. The inlets after the left one take density, length, frequency and morph as floats.

| Argument | |
| --- | --- |
| `proto1` | the first prototype: sine, triangle, square, noise or a buffer~ name (sine) |
| `proto2` | the second prototype (as proto1) |

| Message | |
| --- | --- |
| `1/0` | starts or stops the cloud (also start, bang, stop) |
| `density d [r]` | grains per second (10), and its randomness 0-0.99 (0) |
| `length ms [r]` | grain duration in ms (100), and its randomness 0-1 (0) |
| `freq f [r]` | grain frequency, Hz or ratio (220), and its randomness 0-1 (0) |
| `morph m` | the mix of the prototypes, 0 = proto1, 1 = proto2 (0) |
| `amp a` | the amplitude of each grain (0.25) |
| `gliss g` | each grain glides to its frequency times 1 +- g, at random, 0-1 (0) |
| `jitter j [r]` | stereo position, 0 = right, 1 = left (0.5), and its randomness 0-1 (1) |
| `speed s` | how fast the read position moves through a buffer~ (1) |
| `grainlets n` | each grain lasts n periods of its frequency, replacing length; 0 = off (0) |
| `window type` | the grain window: hann, hamming or blackman (hann) |
| `poly n` | the most grains at once, 1-1024 (256) |
| `proto1 name` | changes the first prototype (proto2 the second) |
| `pitch1 hz` | the pitch of a buffer~ prototype, so its frequency is in Hz; 0 returns to ratios (pitch2 for the second) |
| `seed n` | restarts the random generator with this seed |

| Inlet | |
| --- | --- |
| 1 | 1/0, start, stop; density d [r], length ms [r], freq f [r], morph m, amp a, gliss g, jitter j [r], speed s, grainlets n, window w, poly n, proto1 x, proto2 x, pitch1 hz, pitch2 hz, seed n |
| 2 | density (grains per second) |
| 3 | grain length (ms) |
| 4 | frequency (Hz; for a buffer~, a speed ratio, or Hz once its pitch is given) |
| 5 | morph: 0 prototype 1 .. 1 prototype 2 |

| Outlet | |
| --- | --- |
| 1 | left |
| 2 | right |

See also: `buffer~` `groove~` `sparkle~` `noise~` `cycle~`

## Filters

### `reson~`

`reson~ [gain] [cf] [Q]`

Two-pole resonant bandpass with the structure of Max's reson~, scaled so that its gain at the centre frequency is exactly `gain`. The parameter inlets take signals or floats but are read once per 64-sample block; a list `gain cf Q` in the left inlet sets all three.

| Argument | |
| --- | --- |
| `gain` | the peak gain (1) |
| `cf` | the centre frequency in Hz (1000) |
| `Q` | the centre frequency over the bandwidth: higher is narrower (10) |

| Message | |
| --- | --- |
| `gain cf Q` | sets the three parameters |
| `clear` | empties the filter's memory |

| Inlet | |
| --- | --- |
| 1 | signal to filter |
| 2 | gain |
| 3 | centre frequency (Hz) |
| 4 | Q: higher is narrower |

| Outlet | |
| --- | --- |
| 1 | band-passed signal |

See also: `lores~` `svf~` `biquad~` `filterdesign` `noise~`

### `lores~`

`lores~ [cf] [res]`

Resonant low-pass (an RBJ cookbook biquad whose Q rises from 0.7 to about 25 with the resonance; Musil's lores). The cutoff and resonance inlets take signals or floats, read once per block.

| Argument | |
| --- | --- |
| `cf` | the cutoff frequency in Hz (1000) |
| `res` | the resonance, 0-1 (0) |

| Message | |
| --- | --- |
| `clear` | empties the filter's memory |

| Inlet | |
| --- | --- |
| 1 | signal to filter |
| 2 | cutoff frequency (Hz) |
| 3 | resonance 0-1 |

| Outlet | |
| --- | --- |
| 1 | low-passed signal |

See also: `svf~` `reson~` `biquad~` `filterdesign`

### `svf~`

`svf~ [freq] [q]`

State-variable filter (topology-preserving; Musil's svf) with four outlets: low-pass, high-pass, band-pass and notch. The frequency and Q inlets take signals or floats, read once per block.

| Argument | |
| --- | --- |
| `freq` | the cutoff or centre frequency in Hz (1000) |
| `q` | the resonance, 0-1 (0.5) |

| Inlet | |
| --- | --- |
| 1 | signal to filter |
| 2 | frequency (Hz) |
| 3 | Q 0-1 |

| Outlet | |
| --- | --- |
| 1 | low-pass |
| 2 | high-pass |
| 3 | band-pass |
| 4 | notch |

See also: `lores~` `reson~` `biquad~` `filterdesign`

### `biquad~`

`biquad~ [a0] [a1] [a2] [b1] [b2]`

Two-pole two-zero filter in Max's convention: `y = a0 x + a1 x[n-1] + a2 x[n-2] - b1 y[n-1] - b2 y[n-2]`. The coefficient inlets take floats, not signals; a list of five in the left inlet sets them all, as filterdesign outputs it. The defaults pass the signal unchanged.

| Argument | |
| --- | --- |
| `a0` | the gain of the input (1) |
| `a1` | the gain of the input one sample before (0) |
| `a2` | the gain of the input two samples before (0) |
| `b1` | the gain of the output one sample before, subtracted (0) |
| `b2` | the gain of the output two samples before, subtracted (0) |

| Message | |
| --- | --- |
| `a0 a1 a2 b1 b2` | sets the five coefficients |

| Inlet | |
| --- | --- |
| 1 | signal to filter |
| 2 | a0 |
| 3 | a1 |
| 4 | a2 |
| 5 | b1 |
| 6 | b2 |

| Outlet | |
| --- | --- |
| 1 | filtered signal |

See also: `filterdesign` `filterplot` `lores~` `svf~`

### `slide~`

`slide~ [up] [down]`

Smooths a signal: `y += (x - y) / slide`, where the slide is `up` samples while the input is above the output and `down` while it is below; 1 passes the signal through. Useful for envelope followers and portamento. The slide inlets take signals or floats.

| Argument | |
| --- | --- |
| `up` | the slide while rising, in samples (1) |
| `down` | the slide while falling, in samples (1) |

| Message | |
| --- | --- |
| `reset` | jumps to the input |

| Inlet | |
| --- | --- |
| 1 | signal to smooth; reset jumps to it |
| 2 | slide up (samples, 1 = none) |
| 3 | slide down (samples, 1 = none) |

| Outlet | |
| --- | --- |
| 1 | smoothed signal |

See also: `line~` `lores~` `abs~` `sah~`

### `conv~`

`conv~ buffer [channels]`

Convolves each inlet with the impulse response in a buffer~, with no latency: the first 256 taps directly, the rest by partitioned FFT (the engine is Musil's, musil/src/signals/convolution.h). Channel c uses the response's channel c, or its last one; an inlet with nothing connected takes the channel before, so one source into `conv~ hall 2` gives a stereo reverb. The response is normalised to unit energy unless `normalize 0`. It is reloaded when the buffer~ changes.

| Argument | |
| --- | --- |
| `buffer` | the buffer~ holding the impulse response (required) |
| `channels` | the number of inlets and outlets, 1-8 (1) |

| Message | |
| --- | --- |
| `set name` | uses another buffer~ |
| `gain x` | scales the response (1) |
| `normalize 0/1` | normalises the response to unit energy, or not (1) |

| Inlet | |
| --- | --- |
| 1 | signal to convolve with the buffer~ (channel); set name, gain x, normalize 0/1 |

| Outlet | |
| --- | --- |
| 1 | convolved signal (channel) |

See also: `buffer~` `condec` `reverb~`

### `filterdesign`

`filterdesign [type] [freq] [q] [gain]`

Computes the coefficients of a biquad~ for a filter type, from Robert Bristow-Johnson's Audio EQ Cookbook (as Musil's biquad): lowpass, highpass, bandpass, notch (or bandstop), peak, lowshelf, highshelf, allpass. A type, a frequency (left inlet), a Q (middle) or a gain in dB (right; used by peak and the shelves) outputs the list `a0 a1 a2 b1 b2`, for biquad~ and filterplot. The coefficients follow the current sample rate (44100 while audio is off).

| Argument | |
| --- | --- |
| `type` | the filter type (lowpass) |
| `freq` | the frequency in Hz (1000) |
| `q` | the Q (0.707) |
| `gain` | the gain in dB, for peak and the shelves (0) |

| Message | |
| --- | --- |
| `type` | sets the type and outputs |
| `freq [q] [gain]` | sets the frequency (and Q and gain) and outputs |
| `bang` | outputs the coefficients again |

| Inlet | |
| --- | --- |
| 1 | frequency (Hz), or a type: lowpass highpass bandpass notch peak lowshelf highshelf allpass; bang |
| 2 | Q |
| 3 | gain in dB (peak and shelves) |

| Outlet | |
| --- | --- |
| 1 | coefficients a0 a1 a2 b1 b2, for biquad~ |

See also: `biquad~` `filterplot` `lores~` `svf~`

### `filterplot`

`filterplot [width] [height]`

Draws the frequency response, in dB from +24 to -48 on a logarithmic frequency axis, of the biquad~ coefficients it receives. A list of 10, 15 ... numbers is a cascade of biquads in series. Dragging in it (locked) outputs `frequency gain` (Hz, dB) at the mouse, to drive a filterdesign.

| Argument | |
| --- | --- |
| `width` | the width in pixels, 120-2000 (300) |
| `height` | the height in pixels, 60-1000 (140) |

| Message | |
| --- | --- |
| `a0 a1 a2 b1 b2 ...` | the coefficients to draw |

| Inlet | |
| --- | --- |
| 1 | biquad~ coefficients a0 a1 a2 b1 b2 (or several sets: a cascade) |

| Outlet | |
| --- | --- |
| 1 | frequency and gain at the mouse, while dragging |

See also: `filterdesign` `biquad~` `svf~`

## Signal Math

### `+~` `-~` `*~` `/~`

`+~ [value]`

Add, subtract, multiply or divide two signals, sample by sample. A float in either inlet is used as a constant while no signal is connected there; the argument sets the right one. Division by 0 gives 0.

| Argument | |
| --- | --- |
| `value` | the right operand (0) |

| Inlet | |
| --- | --- |
| 1 | signal |
| 2 | signal or float to add |

| Outlet | |
| --- | --- |
| 1 | sum |

See also: `expr~` `gain~` `sig~` `clip~`

### `gain~`

`gain~ [ramp] [amplitude]`

A fader that scales the signal in its left inlet. It runs in dB from silence to +6 dB (amplitude 0 to 2) and shows its level in dB below; every change ramps linearly over `ramp` ms, so moving it never clicks. A float in the left inlet sets the amplitude (1 = unity), the right inlet the ramp time. The right outlet reports the amplitude when the fader is moved. The ramp and the amplitude are saved with the patch.

| Argument | |
| --- | --- |
| `ramp` | the ramp time in ms (20) |
| `amplitude` | the starting amplitude, 0-2 (1) |

| Message | |
| --- | --- |
| `amplitude` | sets the amplitude, without output |
| `bang` | outputs the amplitude |

| Inlet | |
| --- | --- |
| 1 | signal to scale; a float sets the amplitude (1 = unity) |
| 2 | ramp time (ms) |

| Outlet | |
| --- | --- |
| 1 | scaled signal (changes ramp, no clicks) |
| 2 | amplitude, when the fader moves |

See also: `*~` `line~` `meter~` `dac~`

### `clip~`

`clip~ [min] [max]`

Limits a signal to the range min..max. The limits are signals or floats.

| Argument | |
| --- | --- |
| `min` | the lower limit (-1) |
| `max` | the upper limit (1) |

| Inlet | |
| --- | --- |
| 1 | signal |
| 2 | minimum |
| 3 | maximum |

| Outlet | |
| --- | --- |
| 1 | signal limited to min..max |

See also: `tanh~` `abs~` `expr~`

### `abs~`

`abs~`

The absolute value of a signal (full-wave rectification).

| Inlet | |
| --- | --- |
| 1 | signal |

| Outlet | |
| --- | --- |
| 1 | absolute value |

See also: `clip~` `slide~` `tanh~`

### `tanh~`

`tanh~`

Soft saturation: the hyperbolic tangent of a signal, which stays within -1 and 1. Scale the input up to drive it harder.

| Inlet | |
| --- | --- |
| 1 | signal |

| Outlet | |
| --- | --- |
| 1 | soft-saturated signal |

See also: `clip~` `abs~` `expr~`

### `expr~`

`expr~ expression`

An expr evaluated for every sample. `$v1` to `$v9` read the inlets, which take signals or floats (a float is held while nothing is connected); the highest one used sets the number of inlets. `$f` and `$i` read them too, `$i` truncated. Operators: `+ - * / %`, comparisons, `&& || !`, `pi`; functions: sin cos tan asin acos atan sinh cosh tanh exp log log10 sqrt abs floor ceil int round, pow atan2 min max fmod random(lo, hi). A result that is not a finite number gives 0; errors are printed in the console. random() gives the same numbers at each run (its seed comes from the box's id); `seed n` seeds it again, at the next block.

| Argument | |
| --- | --- |
| `expression` | the expression, e.g. `$v1 * (1 - $v3) + $v2 * $v3` |

| Inlet | |
| --- | --- |
| 1 | value of $v1 (signal or float) |

| Outlet | |
| --- | --- |
| 1 | result of the expression, per sample |

See also: `expr` `+~` `*~` `clip~`

### `sah~`

`sah~ [threshold]`

Sample and hold: each time the right signal rises above the threshold, the current value of the left signal is held at the outlet until the next rise. A float in the right inlet sets the threshold; in the left inlet, a constant input.

| Argument | |
| --- | --- |
| `threshold` | the level the trigger must cross upwards (0) |

| Inlet | |
| --- | --- |
| 1 | signal to sample |
| 2 | trigger: holds the left signal when this rises above the threshold |

| Outlet | |
| --- | --- |
| 1 | held value |

See also: `phasor~` `noise~` `snapshot~` `slide~`

## Signal Routing

### `selector~`

`selector~ n [initial]`

Passes one of `n` signal inlets to the outlet; a number in the left inlet picks it, 1 to `n`, or 0 for none. Unlike Max's, it crossfades over 5 ms, so switching never clicks.

| Argument | |
| --- | --- |
| `n` | the number of signal inlets, 1-64 (1) |
| `initial` | the inlet passed at first, 0 = none (0) |

| Message | |
| --- | --- |
| `n` | picks the inlet (left inlet) |

| Inlet | |
| --- | --- |
| 1 | which input to pass: 1..n, 0 = none (5 ms crossfade) |
| 2 | signal to choose |

| Outlet | |
| --- | --- |
| 1 | the chosen signal |

See also: `gate~` `gate` `*~` `send~`

### `gate~`

`gate~ n [initial]`

Sends the signal in its right inlet to one of `n` outlets, picked by a number in the left inlet (1 to `n`, 0 = closed); the others output silence. It crossfades over 5 ms, so switching never clicks.

| Argument | |
| --- | --- |
| `n` | the number of outlets, 1-64 (1) |
| `initial` | the outlet open at first, 0 = closed (0) |

| Message | |
| --- | --- |
| `n` | picks the outlet (left inlet) |

| Inlet | |
| --- | --- |
| 1 | which outlet gets the signal: 1..n, 0 = closed (5 ms crossfade) |
| 2 | signal to route |

| Outlet | |
| --- | --- |
| 1 | the signal, when this outlet is chosen |

See also: `selector~` `gate` `send~`

### `send~`

`send~ name`

Makes its signal available to every receive~ of the same name, in any subpatcher of the patch. The chain runs send~ before its receive~s, so there is no delay, except around a feedback loop through them (one 64-sample block).

| Argument | |
| --- | --- |
| `name` | the name (required) |

| Message | |
| --- | --- |
| `set name` | changes the name |

| Inlet | |
| --- | --- |
| 1 | signal to send to every receive~ of this name; set name changes it |

See also: `receive~` `send` `tapin~` `dac~`

### `receive~`

`receive~ name`

Outputs the sum of the signals of every send~ with its name.

| Argument | |
| --- | --- |
| `name` | the name (required) |

| Message | |
| --- | --- |
| `set name` | listens to another name |

| Inlet | |
| --- | --- |
| 1 | set name: listen to another send~ name |

| Outlet | |
| --- | --- |
| 1 | sum of the send~s of this name |

See also: `send~` `receive` `adc~`

## Delay

### `tapin~`

`tapin~ [max]`

Writes its input into a delay line, read by the tapout~ connected to its outlet. The memory holds `max` ms at the rate it runs at (sized again when the rate changes); a longer delay is clipped to it. Since the link does not order the two, tapout~ -> ... -> tapin~ feedback is allowed.

| Argument | |
| --- | --- |
| `max` | the longest delay in ms, 1-60000 (1000) |

| Message | |
| --- | --- |
| `clear` | empties the delay line |

| Inlet | |
| --- | --- |
| 1 | signal to delay |

| Outlet | |
| --- | --- |
| 1 | delay line: connect to tapout~ |

See also: `tapout~` `comb~` `allpass~` `send~`

### `tapout~`

`tapout~ [ms...]`

Reads the delay line of the tapin~ connected to its left inlet, one tap per argument, each with its own inlet (signal or float) and outlet. Delays are in ms, interpolated, and limited by the tapin~'s length; inside a feedback loop the shortest delay is one block (64 samples).

| Argument | |
| --- | --- |
| `ms` | the delay of each tap in ms (one tap, 0) |

| Inlet | |
| --- | --- |
| 1 | delay line from tapin~; a float sets the first tap (ms) |

| Outlet | |
| --- | --- |
| 1 | output of this tap |

See also: `tapin~` `comb~` `allpass~`

### `comb~`

`comb~ [max] [delay] [a] [b] [c]`

Comb filter: `y = a x + b x[n-D] + c y[n-D]`, where D is the delay. The delay and the three gains are signals or floats; the memory holds `max` ms at the rate it runs at, as with tapin~.

| Argument | |
| --- | --- |
| `max` | the longest delay in ms, 1-10000 (50) |
| `delay` | the delay in ms (half of max) |
| `a` | the gain of the input (1) |
| `b` | the gain of the delayed input (0) |
| `c` | the feedback gain (0.5) |

| Message | |
| --- | --- |
| `clear` | empties the delay lines |

| Inlet | |
| --- | --- |
| 1 | signal |
| 2 | delay (ms) |
| 3 | a: gain of the input |
| 4 | b: gain of the delayed input |
| 5 | c: feedback gain |

| Outlet | |
| --- | --- |
| 1 | filtered signal |

See also: `allpass~` `tapin~` `reverb~` `reson~`

### `allpass~`

`allpass~ [max] [delay] [gain]`

Allpass filter: `y = -g x + x[n-D] + g y[n-D]`; it changes the phase but not the spectrum of a steady sound, and is a building block of reverbs. The delay and gain are signals or floats.

| Argument | |
| --- | --- |
| `max` | the longest delay in ms, 1-10000 (50) |
| `delay` | the delay in ms (half of max) |
| `gain` | the gain g (0.7) |

| Message | |
| --- | --- |
| `clear` | empties the delay lines |

| Inlet | |
| --- | --- |
| 1 | signal |
| 2 | delay (ms) |
| 3 | gain |

| Outlet | |
| --- | --- |
| 1 | filtered signal |

See also: `comb~` `tapin~` `reverb~` `fvcomb~`

## Samples

### `buffer~`

`buffer~ name [file] [ms] [channels]`

Named sample memory. Every object that names it (play~, groove~, record~, index~, poke~, waveform~, conv~, sparkle~, pursuit, soundtypes...) reads or writes the same samples; names are shared by all open patches, as in Max. A file given as argument is read when the patch opens; otherwise the buffer is empty, or `ms` long at the audio's sample rate. Right-click it to edit, load, save or clear it.

Double-click it (locked), or any object that names it, to open the sound editor: an overview, a waveform per channel and the spectrogram (Wave, Spectrum or Both), zoomed with Cmd+ and Cmd- or Cmd and the wheel, scrolled with the wheel or the overview. Drag to select (Shift extends the selection, Cmd+A selects all), click to set the playhead; Space plays the selection, or from the playhead, through the audio output beside the patches (L loops it). The Edit menu (or the right button) cuts, copies, pastes (over the selection, or at the playhead), deletes, trims to the selection, silences, fades in or out, changes the gain, normalizes the peak to 0 dB and reverses; each edit is one step of Undo (Cmd+Z). The buffer~ changes at once for every object that uses it; it is patch memory, so nothing is marked unsaved, and Save writes a copy as a WAV file. Copied samples paste into any sound editor (a sound with other channels is mixed down or spread). File > Open Sound... edits a sound file the same way, as a document of its own that Save writes back (24-bit WAV).

| Argument | |
| --- | --- |
| `name` | the name the other objects use (required) |
| `file` | a sound file read when the patch opens: WAV, AIFF, FLAC or MP3, relative to the patch (or among the bundled sounds) |
| `ms` | the length of an empty buffer, in milliseconds (0) |
| `channels` | the channels of an empty buffer, 1 to 8 (1) |

| Message | |
| --- | --- |
| `read file` | reads a sound file; the buffer takes its length, channels and sample rate |
| `write file` | writes the buffer as a WAV file |
| `size ms` | changes the length, keeping what fits and filling with silence |
| `clear` | sets every sample to 0 |

| Inlet | |
| --- | --- |
| 1 | read file, write file.wav, size ms, clear |

See also: `play~` `groove~` `record~` `waveform~` `index~` `poke~`

### `play~`

`play~ name [channels]`

Plays a buffer~ once, at its own sample rate, interpolating between samples. `start` or bang plays it from the beginning, or from `from` to `to` (ms). With a signal connected to its inlet it plays nothing by itself: it reads the buffer at that position in ms, for example from line~. Double-click it (locked) to edit the buffer~ in the sound editor.

| Argument | |
| --- | --- |
| `name` | the buffer~ to play (required) |
| `channels` | the number of outlets, 1 to 8 (1); a mono buffer plays on all of them |

| Message | |
| --- | --- |
| `start [from [to]]` | plays from `from` (ms, 0) to `to` (ms; the end when missing or 0) |
| `bang` | plays the whole buffer |
| `stop` | stops |
| `set name` | plays another buffer~ |

| Inlet | |
| --- | --- |
| 1 | start [from [to]] in ms, stop, bang; or a signal: read position in ms |

| Outlet | |
| --- | --- |
| 1 | audio, channel by channel |

See also: `buffer~` `groove~` `index~` `line~` `waveform~` `sfplay~`

### `groove~`

`groove~ name [channels]`

Variable-speed playback of a buffer~: the signal in the left inlet is the rate (1 as recorded, 0.5 an octave down, negative backwards). Without looping it stops at either end; with `loop 1` it cycles between the loop start and end (inlets 2 and 3, ms). The last outlet is a sync ramp, 0 to 1 through the loop. Double-click it (locked) to edit the buffer~ in the sound editor.

| Argument | |
| --- | --- |
| `name` | the buffer~ to play (required) |
| `channels` | the number of audio outlets, 1 to 8 (1) |

| Message | |
| --- | --- |
| `number` | jumps to that position (ms); in inlets 2 and 3, the loop start and end (ms; an end of 0, or not after the start, is the end of the buffer) |
| `loop 1/0` | turns looping on or off (off) |
| `startloop` | jumps to the loop start |
| `set name` | plays another buffer~ |

| Inlet | |
| --- | --- |
| 1 | rate (signal, 1 = normal, negative = backwards); a float jumps (ms); loop 1/0 |
| 2 | loop start (ms) |
| 3 | loop end (ms, 0 = end of the buffer) |

| Outlet | |
| --- | --- |
| 1 | audio, channel by channel |
| 2 | sync: position in the loop, 0 to 1 |

See also: `play~` `buffer~` `sig~` `record~` `sparkle~`

### `record~`

`record~ name [channels]`

Records its signal inlets into a buffer~, from its beginning; the buffer keeps its length, so give it one (buffer~ name file, or buffer~ name ms). Recording stops at the end unless `loop 1` makes it wrap around. A buffer with more channels than inlets takes the inlets again in turn. Double-click it (locked) to edit the buffer~ in the sound editor.

| Argument | |
| --- | --- |
| `name` | the buffer~ to record into (required) |
| `channels` | the number of signal inlets, 1 to 8 (1) |

| Message | |
| --- | --- |
| `1 / 0` | 1 starts recording from the beginning, 0 stops (left inlet) |
| `loop 1/0` | wraps to the beginning at the end instead of stopping (off) |
| `set name` | records into another buffer~ |

| Inlet | |
| --- | --- |
| 1 | signal to record; 1 starts, 0 stops, loop 1/0 |

| Outlet | |
| --- | --- |
| 1 | position in the buffer, 0 to 1 |

See also: `buffer~` `play~` `groove~` `waveform~` `sfrecord~` `poke~`

### `sfplay~`

`sfplay~ [channels]`

Plays a sound file without a buffer~: `open` reads the whole file into the object's own memory, then 1 plays it from the start (from the end when the speed is negative). Right-click it and choose Open file... to pick the file.

| Argument | |
| --- | --- |
| `channels` | the number of outlets, 1 to 8 (1); a mono file plays on all of them |

| Message | |
| --- | --- |
| `open file` | reads a WAV, AIFF, FLAC or MP3 file, relative to the patch |
| `1 / 0` | 1 plays from the start, 0 stops |
| `bang` | plays from the start |
| `loop 1/0` | loops the file (off) |
| `speed x` | the playback speed (1); negative plays backwards |

| Inlet | |
| --- | --- |
| 1 | open file, 1 plays, 0 stops, loop 1/0, speed x |

| Outlet | |
| --- | --- |
| 1 | audio, channel by channel |

See also: `play~` `buffer~` `sfrecord~` `groove~`

### `sfrecord~`

`sfrecord~ [channels]`

Records its signal inlets to a 24-bit WAV file at the audio's sample rate. `open` names the file, 1 starts and 0 stops; the file is complete once it stops. Audio must be on: it records once audio starts. The outlet reports the recorded time in ms, every 250 ms and when it stops. A writer thread saves the file, so recording never holds up the audio. Right-click it and choose Choose file... to pick the file.

| Argument | |
| --- | --- |
| `channels` | the number of signal inlets (and channels of the file), 1 to 8 (1) |

| Message | |
| --- | --- |
| `open file.wav` | the file to write, relative to the patch (it is overwritten) |
| `1 / 0` | 1 starts recording, 0 stops and closes the file |
| `start` | starts recording |
| `stop` | stops and closes the file |

| Inlet | |
| --- | --- |
| 1 | open file.wav, 1 starts, 0 stops; signal to record (channel 1) |

| Outlet | |
| --- | --- |
| 1 | recorded time (ms) |

See also: `sfplay~` `record~` `buffer~` `dac~`

### `waveform~`

`waveform~ name [width] [height]`

Shows a buffer~, one lane per channel, with its name and length; it follows the buffer while record~ writes into it. In locked mode, drag across it to select a region: the outlets give its start and end in ms.

| Argument | |
| --- | --- |
| `name` | the buffer~ to show (required) |
| `width` | the width in pixels, 80 to 2000 (320) |
| `height` | the height in pixels, 40 to 1000 (110) |

| Message | |
| --- | --- |
| `number` | draws a cursor at that time (ms) |
| `select start end` | sets the selection (ms) and outputs it |
| `set name` | shows another buffer~ |

| Inlet | |
| --- | --- |
| 1 | number: cursor (ms); select start end; set buffer~ name |

| Outlet | |
| --- | --- |
| 1 | selection start (ms) |
| 2 | selection end (ms) |

See also: `buffer~` `play~` `record~` `plot`

### `count~`

`count~ [min] [max] [loop]`

A sample counter: its output goes up by one every sample. bang starts it from `min`, a number from that value; `stop` stops it and the output holds where it stopped (`min` before the first start). At `max` it wraps to `min`, or stops there when `loop` is 0. Inside pfft~ a start falls on a frame boundary, so the count stays aligned to the bins.

| Argument | |
| --- | --- |
| `min` | the start value (0) |
| `max` | the value where it wraps or stops; 0, or not above min, counts on forever (0) |
| `loop` | 1 wraps at max, 0 stops there (1) |

| Message | |
| --- | --- |
| `bang` | counts from min |
| `number` | counts from that value |
| `stop` | stops counting |

| Inlet | |
| --- | --- |
| 1 | bang: count from min; a number: count from there; stop |

| Outlet | |
| --- | --- |
| 1 | count, one step per sample |

See also: `index~` `poke~` `phasor~` `line~` `sah~`

### `index~`

`index~ name [channel]`

Reads one channel of a buffer~ at the sample index in its inlet (a signal, or a number), without interpolation; indices outside the buffer read 0. Double-click it (locked) to edit the buffer~ in the sound editor.

| Argument | |
| --- | --- |
| `name` | the buffer~ to read (required) |
| `channel` | the channel to read, from 1 (1) |

| Message | |
| --- | --- |
| `set name` | reads another buffer~ |

| Inlet | |
| --- | --- |
| 1 | sample index (signal) |

| Outlet | |
| --- | --- |
| 1 | buffer~ value at that index |

See also: `poke~` `count~` `buffer~` `play~`

### `poke~`

`poke~ name [channel]`

Writes the signal in its left inlet into one channel of a buffer~, at the sample index in its right inlet (signals or numbers); indices outside the buffer are ignored. Double-click it (locked) to edit the buffer~ in the sound editor.

| Argument | |
| --- | --- |
| `name` | the buffer~ to write (required) |
| `channel` | the channel to write, from 1, at most the buffer's channels (1) |

| Message | |
| --- | --- |
| `set name` | writes into another buffer~ |

| Inlet | |
| --- | --- |
| 1 | value to write (signal) |
| 2 | sample index to write at (signal) |

See also: `index~` `count~` `record~` `buffer~`

### `condec`

`condec`

Convolution and deconvolution of whole buffer~s, computed at once when asked (the author's condec; the deconvolution is Musil's deconv). The result replaces the contents of the out buffer~, at the sample rate of the first; channels pair up, and the buffer with fewer channels repeats its last. Deconvolution divides the spectrum of y by that of h where h is above a threshold, so a recording can be "un-reverberated" by its impulse response. The outlet bangs when the result is in.

| Message | |
| --- | --- |
| `conv x h out [scale] [mix]` | writes x convolved with h into out (length x + h - 1), times scale (1), plus x times mix (0) |
| `deconv y h out [threshold] [scale]` | writes y deconvolved by h into out (the length of y): bins where h is below threshold (0.001) times its peak are set to 0; times scale (1) |

| Inlet | |
| --- | --- |
| 1 | conv x h out [scale mix] or deconv y h out [threshold scale]: buffer~ names |

| Outlet | |
| --- | --- |
| 1 | bang when the result is in the out buffer~ |

See also: `conv~` `buffer~` `reverb~` `render`

### `render`

`render [patch] [channels]`

Renders another patch offline, faster than real time, into a buffer~, as a background job. Each render starts the patch afresh at the audio's sample rate (loaded, the stored sends applied, loadbang) and records its dac~. The rendered patch has its own send/receive and buffer~ names; its printed output appears when the render ends. Renders asked for while one runs wait their turn, with the sends and input of the moment they were asked for.

| Argument | |
| --- | --- |
| `patch` | the patch file to render, relative to this patch |
| `channels` | the dac~ channels recorded into the buffer~, 1 to 16 (2) |

| Message | |
| --- | --- |
| `open patch.moritz` | chooses the patch to render |
| `send name value...` | sends the value to the patch's receive name before every render (kept until clear) |
| `input buffer` | feeds the buffer~ to the patch's adc~ |
| `render buffer seconds` | renders that many seconds into the buffer~, which takes their length and channels |
| `clear` | forgets the sends and the input |
| `stop` | cancels the render and the ones waiting |

See also: `buffer~` `poly~` `play~` `condec`

## Spectral

### `pfft~`

`pfft~ name [size] [overlap] [args...]`

Runs the abstraction name.moritz once per FFT frame. Each signal inlet is analysed with a Hann window every size/overlap samples; inside, fftin~ gives the spectrum and every signal vector is one whole frame (one bin per sample, bins 0 to size/2 - 1); what reaches fftout~ is resynthesised by overlap-add. The signal ports come from the fftin~ and fftout~ objects inside, then inlet and outlet add control ports. Latency is one frame (size samples). Double-click it (locked) to open the inner patch.

| Argument | |
| --- | --- |
| `name` | the abstraction to load, name.moritz (required) |
| `size` | the FFT size in samples, rounded up to a power of two, 128 to 16384 (512) |
| `overlap` | frames per window, 1 to size (4) |
| `args` | passed to the abstraction as #1, #2... |

| Message | |
| --- | --- |
| `anything` | to a control inlet, goes to the matching inlet box inside |

See also: `fftin~` `fftout~` `cartopol~` `poltocar~` `framedelta~` `specenv~`

### `fftin~`

`fftin~ [n]`

Inside a pfft~ patch: the spectrum of the pfft~'s signal inlet n, as real part, imaginary part and bin number (0 to size/2 - 1), one frame per vector.

| Argument | |
| --- | --- |
| `n` | the pfft~ signal inlet, from 1 (1) |

| Outlet | |
| --- | --- |
| 1 | real part |
| 2 | imaginary part |
| 3 | bin number, 0 to size/2 - 1 |

See also: `pfft~` `fftout~` `cartopol~`

### `fftout~`

`fftout~ [n]`

Inside a pfft~ patch: the real and imaginary parts written here are resynthesised to the pfft~'s signal outlet n. Several fftout~ with the same n add up.

| Argument | |
| --- | --- |
| `n` | the pfft~ signal outlet, from 1 (1) |

| Inlet | |
| --- | --- |
| 1 | real part |
| 2 | imaginary part |

See also: `pfft~` `fftin~` `poltocar~`

### `cartopol~`

`cartopol~`

Converts real and imaginary parts to amplitude and phase (-pi to pi), sample by sample.

| Inlet | |
| --- | --- |
| 1 | real part |
| 2 | imaginary part |

| Outlet | |
| --- | --- |
| 1 | amplitude |
| 2 | phase, -pi to pi |

See also: `poltocar~` `fftin~` `pfft~` `framedelta~`

### `poltocar~`

`poltocar~`

Converts amplitude and phase to real and imaginary parts, sample by sample.

| Inlet | |
| --- | --- |
| 1 | amplitude |
| 2 | phase |

| Outlet | |
| --- | --- |
| 1 | real part |
| 2 | imaginary part |

See also: `cartopol~` `fftout~` `pfft~` `frameaccum~`

### `framedelta~`

`framedelta~`

The difference of each bin from the same bin of the previous frame: from a phase, its frequency. Inside pfft~ it keeps one value per bin; elsewhere it is the difference from the previous sample.

| Inlet | |
| --- | --- |
| 1 | signal (in pfft~: one frame) |

| Outlet | |
| --- | --- |
| 1 | difference from the same bin of the previous frame |

See also: `frameaccum~` `cartopol~` `phasewrap~` `pvlock~` `pfft~`

### `frameaccum~`

`frameaccum~ [wrap]`

The running sum of each bin over the frames: from a frequency, a phase. Where the third inlet is not 0, that bin restarts from the value in the second inlet instead (for example the analysed phase, after a jump in the sound). Inside pfft~ it keeps one sum per bin; elsewhere a single one.

| Argument | |
| --- | --- |
| `wrap` | 1 keeps the sum in -pi..pi, for phase accumulation (0) |

| Inlet | |
| --- | --- |
| 1 | value to add each frame |
| 2 | value to restart from |
| 3 | reset: where not 0, the bin restarts |

| Outlet | |
| --- | --- |
| 1 | running sum per bin |

See also: `framedelta~` `phasewrap~` `pvlock~` `poltocar~` `pfft~`

### `pvlock~`

`pvlock~`

The resynthesis phase of a phase vocoder, with identity phase locking (Laroche and Dolson). Each frame it finds the amplitude peaks: a peak advances its previous output phase by its phase difference, and the bins around it keep their analysed offset from it, so the bins of one partial stay in step and stretched sounds stay clean. A reset anywhere in a frame outputs the analysed phase for the whole frame. It needs whole frames: use it inside pfft~.

| Inlet | |
| --- | --- |
| 1 | amplitude (peaks are found here) |
| 2 | analysed phase |
| 3 | phase difference (frequency) |
| 4 | reset: not 0 anywhere in a frame restarts the whole frame from the analysed phase |

| Outlet | |
| --- | --- |
| 1 | resynthesis phase, locked to the peaks |

See also: `frameaccum~` `framedelta~` `cartopol~` `sparkle~` `pfft~`

### `lrshift~`

`lrshift~ [n]`

Shifts each signal vector by n samples, filling with 0. Inside pfft~ every bin moves n bins up (negative: down), so bins can be combined with their neighbours or the spectrum shifted. Outside pfft~ it shifts each 64-sample vector. n is set by the argument only.

| Argument | |
| --- | --- |
| `n` | the shift in samples (bins); positive to the right, up (0) |

| Inlet | |
| --- | --- |
| 1 | signal (in pfft~: one frame) |

| Outlet | |
| --- | --- |
| 1 | shifted by n samples (bins) |

See also: `pfft~` `framedelta~` `cartopol~`

### `phasewrap~`

`phasewrap~`

Wraps a phase into -pi..pi.

| Inlet | |
| --- | --- |
| 1 | phase |

| Outlet | |
| --- | --- |
| 1 | phase wrapped to -pi..pi |

See also: `framedelta~` `frameaccum~` `cartopol~` `pvlock~`

### `sparkle~`

`sparkle~ [buffer] [channels] [window] [overlap]`

The phase vocoder of the author's sparkle, with phase locking and formant (spectral envelope) preservation: the engine of Musil's pvoc. With a buffer~ name it plays the buffer: the left inlet is the speed. Without one it transforms the signal in its left inlet. A frame is computed while the next hop plays (on a thread of its own), so the sound comes one hop late, and one window and one hop late when transforming the input. Pitch and formant ratios (inlets 2 and 3, 0.05 to 8) are read once per frame. Cross-synthesis mixes in a second sound: a buffer~ (read at the same position, or on its own when the first sound is live), or else the signal in inlet 5; inlet 4 is the amount (1).

| Argument | |
| --- | --- |
| `buffer` | the buffer~ to play; without it the object transforms its input |
| `channels` | with a buffer~, the number of channels, 1 to 8 (1) |
| `window` | the window in samples, 64 to 16384 (2048); the FFT is twice the next power of two |
| `overlap` | windows per hop, 2 to 32 (8) |

| Message | |
| --- | --- |
| `number` | the speed (1 as recorded, 0.25 four times slower, 0 frozen, negative backwards) |
| `bang` | plays the buffer~ from the start |
| `start [ms]` | plays from that position (0) |
| `stop` | stops |
| `loop [1/0]` | loops the buffer~ (1 when no argument; off at first) |
| `envelope C` | keeps the spectral envelope with C cepstral coefficients, so transposed voices keep their formants (30-80 for voices; 0 off, the default) |
| `threshold x` | drops the bins weaker than x times a full-scale sine, to remove noise (0.001 is -60 dB; 0 = off) |
| `robot [1/0]` | robot voice: all phases set to zero (1 when no argument) |
| `whisper [1/0]` | whisper: random phases (1 when no argument) |
| `cross mode [buffer]` | cross-synthesis with the buffer~ or inlet 5: 1 multiplicative, 2 spectral flattener (the second sound's envelope on the first, with the envelope order, 40 if none), 3 morph, 0 off |
| `set name` | with a buffer~, plays another one |

| Inlet | |
| --- | --- |
| 1 | speed (1 normal, 0 frozen, < 0 backwards), or the input without a buffer~; start [ms], stop, loop 1/0, envelope C, threshold x, robot 1/0, whisper 1/0, cross mode [buffer] (1 multiplicative, 2 spectral flattener, 3 morph, 0 off) |
| 2 | pitch ratio (2 = an octave up) |
| 3 | formant ratio (with envelope: moves the formants) |
| 4 | cross-synthesis amount, 0 to 1 |
| 5 | the second sound for cross-synthesis (without a cross buffer~) |

| Outlet | |
| --- | --- |
| 1 | output (channel) |

See also: `groove~` `pvlock~` `specenv~` `buffer~` `pitchshift~` `pfft~`

### `nmf`

`nmf [components] [iterations]`

Separates a buffer~ into parts that repeat (notes, sources) by non-negative matrix factorisation of its magnitude spectrogram, V ~ W H, with Musil's nmf (the same code): W holds a spectrum per part, H when and how loud it sounds. Each part is resynthesised with the original phases through a soft mask, so the parts add up to the sound (mixed to mono). It works as a background job; messages sent while it works wait their turn, in order.

| Argument | |
| --- | --- |
| `components` | the number of parts, 1 to 32 (3) |
| `iterations` | 1 to 10000 (100) |

| Message | |
| --- | --- |
| `run source dest` | unsupervised: writes one part per channel of the buffer~ dest |
| `learn example` | supervised: learns `components` parts from a recording of one source alone; send it once per source |
| `separate mix dest` | explains the mix with the learned parts only and writes one source per channel of dest, in the order they were learned |
| `forget` | clears what was learned |
| `components k` | the number of parts, 1 to 32 |
| `iterations n` | 1 to 10000 |
| `size n` | the FFT size, rounded to a power of two, 256 to 16384 (2048; hop size/4); a new size forgets what was learned |
| `seed n` | the seed of the random start (1) |
| `stop` | cancels the job and the messages waiting |

| Inlet | |
| --- | --- |
| 1 | run source dest (unsupervised); learn example, separate mix dest, forget (supervised); components k, iterations n, size n, seed n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done (dest has one part, or one source, per channel) |

See also: `hpss` `buffer~` `render` `atoms`

### `hpss`

`hpss [kernel] [size]`

Harmonic/percussive separation of a buffer~ (Musil's), as a background job. The magnitude spectrogram is median-filtered along time (sustained, harmonic sounds) and along frequency (attacks, percussive sounds); soft masks from the two weight the original spectra. The two results add up to the source; each channel is separated on its own.

| Argument | |
| --- | --- |
| `kernel` | the median filter length in frames and bins, odd, 3 to 101 (17); longer separates more |
| `size` | the FFT size, rounded up to a power of two, 256 to 16384 (2048; hop size/4) |

| Message | |
| --- | --- |
| `run source harmonic percussive` | writes the two parts into the buffer~s harmonic and percussive |
| `kernel n` | the filter length, 3 to 101 (made odd) |
| `size n` | the FFT size, 256 to 16384 |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run source harmonic percussive, kernel n, size n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: the two buffer~s are ready |

See also: `nmf` `buffer~` `onset~` `render`

### `specenv~`

`specenv~ [order] [iterations]`

Inside pfft~, the spectral envelope of the amplitudes in its left inlet, frame by frame (Musil's true envelope: a cepstral smoothing of the log spectrum, iterated so it rides on the harmonic peaks instead of averaging peaks and valleys). The second outlet is the fine structure: the amplitudes divided by the envelope, the harmonics or noise with the formants taken out.

| Argument | |
| --- | --- |
| `order` | cepstral coefficients kept, the resolution; about sr / (2 f0) follows a voice's formants without rippling at its harmonics; a number in the right inlet changes it (40) |
| `iterations` | 0 to 100; 0 is the plain cepstral smoothing (12) |

| Message | |
| --- | --- |
| `iterations n` | sets the iterations, 0 to 100 |

| Inlet | |
| --- | --- |
| 1 | amplitudes (inside pfft~: one frame); iterations n |
| 2 | order (cepstral coefficients: the resolution) |

| Outlet | |
| --- | --- |
| 1 | spectral envelope |
| 2 | fine structure: the amplitudes divided by the envelope |

See also: `pfft~` `cartopol~` `sparkle~` `impose~`

## Spatial

### `ambienc~`

`ambienc~ [order] [azimuth] [elevation]`

Encodes a mono signal at a direction as an ambisonic sound field of (order+1)^2 channels (AmbiX: ACN order, SN3D; at order 1, W Y Z X). Azimuth and elevation are numbers or signals; moving sources glide, the gains are interpolated over each block. Fields add channel by channel, so several sources can be summed before decoding. The gains are Musil's ambi-gains.

| Argument | |
| --- | --- |
| `order` | the ambisonic order, 1 to 5 (1) |
| `azimuth` | degrees, 0 in front, 90 to the left (0) |
| `elevation` | degrees, positive up (0) |

| Inlet | |
| --- | --- |
| 1 | the signal to place |
| 2 | azimuth (degrees, 0 front, 90 left) |
| 3 | elevation (degrees, up) |

| Outlet | |
| --- | --- |
| 1 | sound field channel (ACN: W Y Z X ...) |
| 2 | sound field channel (ACN: W Y Z X ...) |
| 3 | sound field channel (ACN: W Y Z X ...) |
| 4 | sound field channel (ACN: W Y Z X ...) |

See also: `ambirot~` `ambidec~` `binaural~` `hrtf~` `binpan~`

### `ambirot~`

`ambirot~ [order] [yaw]`

Turns a sound field about the vertical axis by the yaw in its last inlet, exactly: within each order the pair of channels m, -m rotates by m times the yaw. To follow a listener's head, rotate by minus the head's yaw.

| Argument | |
| --- | --- |
| `order` | the ambisonic order, 1 to 5 (1) |
| `yaw` | the rotation in degrees, positive to the left (0) |

| Message | |
| --- | --- |
| `number` | in the last inlet, the yaw (degrees) |

| Inlet | |
| --- | --- |
| 1 | sound field channel (ACN) |
| 2 | sound field channel (ACN), or the yaw in degrees (last inlet) |
| 3 | sound field channel (ACN), or the yaw in degrees (last inlet) |
| 4 | sound field channel (ACN), or the yaw in degrees (last inlet) |
| 5 | sound field channel (ACN), or the yaw in degrees (last inlet) |

| Outlet | |
| --- | --- |
| 1 | rotated sound field channel |
| 2 | rotated sound field channel |
| 3 | rotated sound field channel |
| 4 | rotated sound field channel |

See also: `ambienc~` `ambidec~` `binaural~`

### `ambidec~`

`ambidec~ order azimuth ...`

Decodes a sound field to loudspeakers on a horizontal ring, one outlet per azimuth, in the order given (ambidec~ 1 45 135 -135 -45 is a square: front left, back left, back right, front right). It uses the horizontal channels only: for an evenly spaced ring of at least 2N+1 speakers this is the exact (mode-matching) decoder, with 2D max-rE weights: Musil's ambi-decode on a ring.

| Argument | |
| --- | --- |
| `order` | the ambisonic order, 1 to 5 (1) |
| `azimuth` | the speakers' azimuths in degrees, one per outlet (45 135 -135 -45 when none is given) |

| Inlet | |
| --- | --- |
| 1 | sound field channel (ACN) |
| 2 | sound field channel (ACN) |
| 3 | sound field channel (ACN) |
| 4 | sound field channel (ACN) |

| Outlet | |
| --- | --- |
| 1 | speaker, in the order of the azimuths |
| 2 | speaker, in the order of the azimuths |
| 3 | speaker, in the order of the azimuths |
| 4 | speaker, in the order of the azimuths |

See also: `ambienc~` `ambirot~` `binaural~` `dac~`

### `binaural~`

`binaural~ [order]`

Plays a sound field on headphones. The field is decoded to 17 virtual speakers over the sphere (8 at ear height, 4 above, 4 below, 1 overhead), each heard through the measured head-related impulse responses of its direction (MIT KEMAR), with SN3D normalisation and 3D max-rE weights. All of it folds into one filter pair per channel, so the cost does not depend on the number of sources. A source in front plays at the level of the front head response.

| Argument | |
| --- | --- |
| `order` | the ambisonic order, 1 to 5 (1) |

| Inlet | |
| --- | --- |
| 1 | sound field channel (ACN: W Y Z X ...) |
| 2 | sound field channel (ACN: W Y Z X ...) |
| 3 | sound field channel (ACN: W Y Z X ...) |
| 4 | sound field channel (ACN: W Y Z X ...) |

| Outlet | |
| --- | --- |
| 1 | left ear |
| 2 | right ear |

See also: `ambienc~` `ambirot~` `hrtf~` `binpan~`

### `hrtf~`

`hrtf~ [azimuth] [elevation]`

Places a mono sound at a direction on headphones through the measured head-related impulse responses themselves (all 710 directions of the MIT KEMAR set): the four measured directions around the one asked for, interpolated. Without ambisonics in between, the cues of the outer ear stay sharp, so height is clearer than with binaural~, and the level follows the real head (quieter behind and above). Azimuth and elevation are numbers or signals; a moving direction crossfades within each block.

| Argument | |
| --- | --- |
| `azimuth` | degrees, 0 in front, 90 to the left, 180 behind (0) |
| `elevation` | degrees, -40 to 90 (0) |

| Inlet | |
| --- | --- |
| 1 | the signal to place |
| 2 | azimuth (degrees, 0 front, 90 left, 180 behind) |
| 3 | elevation (degrees, -40 to 90) |

| Outlet | |
| --- | --- |
| 1 | left ear |
| 2 | right ear |

See also: `binaural~` `ambienc~` `binpan~`

## Analysis

### `snapshot~`

`snapshot~ [ms]`

Reports the current value of its signal (the last sample of the latest audio block) as a number: on bang, and every `ms` milliseconds while audio runs. An interval of 0 reports only on bang; the shortest interval is 5 ms.

| Argument | |
| --- | --- |
| `ms` | the report interval, in milliseconds (0: only on bang) |

| Message | |
| --- | --- |
| `bang` | reports the value now |
| `number` | sets the report interval (ms); also the right inlet |
| `interval ms` | sets the report interval (ms) |

| Inlet | |
| --- | --- |
| 1 | signal; bang reports its value now |
| 2 | report interval (ms, 0 = only on bang) |

| Outlet | |
| --- | --- |
| 1 | signal value |

See also: `peakamp~` `meter~` `scope~` `descriptors~` `sig~`

### `peakamp~`

`peakamp~ [ms]`

Reports the largest absolute value its signal reached since the last report, then starts measuring again: on bang, and every `ms` milliseconds while audio runs (5 ms at least). Use atodb to read it in dB.

| Argument | |
| --- | --- |
| `ms` | the report interval, in milliseconds (0: only on bang) |

| Message | |
| --- | --- |
| `bang` | reports the peak now |
| `number` | sets the report interval (ms); also the right inlet |
| `interval ms` | sets the report interval (ms) |

| Inlet | |
| --- | --- |
| 1 | signal; bang reports the peak now |
| 2 | report interval (ms) |

| Outlet | |
| --- | --- |
| 1 | peak absolute value since the last report |

See also: `snapshot~` `meter~` `atodb` `descriptors~`

### `meter~`

`meter~`

A level meter: the peak of its signal drawn as a vertical bar from -60 dB to 0 dB, green, yellow above -12 dB, red above -3 dB. The bar falls back gradually, so short peaks stay visible for a moment.

| Inlet | |
| --- | --- |
| 1 | signal to show |

See also: `peakamp~` `scope~` `spectroscope~` `gain~`

### `scope~`

`scope~ [samples]`

An oscilloscope: the latest `samples` of its signal, from -1 to 1, triggered on a rising zero crossing so that a periodic wave stands still.

| Argument | |
| --- | --- |
| `samples` | how many samples the display spans, 32 to 1024 (512) |

| Inlet | |
| --- | --- |
| 1 | signal to show |

See also: `meter~` `spectroscope~` `waveform~` `snapshot~` `plot`

### `spectroscope~`

`spectroscope~ [width height]`

The live spectrum of its signal: a 2048-point FFT with a Hann window, frequency on a log scale from 20 Hz to half the sample rate, level from 0 dB (a full-scale sine) down to -90 dB, with grid lines at 100 Hz, 1 kHz and 10 kHz and at -30 and -60 dB. Peaks fall back slowly, so short sounds stay visible for a moment.

| Argument | |
| --- | --- |
| `width` | the width of the display, in pixels, 120 to 2000 (300) |
| `height` | the height of the display, in pixels, 60 to 1000 (130) |

| Inlet | |
| --- | --- |
| 1 | signal to analyse |

See also: `scope~` `meter~` `spectrogram` `descriptors~` `melbands~`

### `spectrogram`

`spectrogram [size] [hop]`

Writes the magnitude spectrogram of a buffer~ into another buffer~, as a background job: one channel a bin (size / 2 + 1 of them, or up to `maxfreq`), one sample a frame, values in dB (0 dB: a full-scale sine; down to -100). The result's sample rate is the number of frames a second, so plot shows its time axis. A Hann window of `size` samples, a frame every `hop`; a stereo sound is mixed to mono.

| Argument | |
| --- | --- |
| `size` | the window, in samples, a power of 2 from 64 to 65536 (2048) |
| `hop` | the step between frames, in samples, 16 or more (512) |

| Message | |
| --- | --- |
| `run sound matrix` | analyses the buffer~ sound and writes the spectrogram into the buffer~ matrix |
| `maxfreq Hz` | keeps only the bins up to this frequency (0: all of them) |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound matrix; maxfreq Hz, size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: matrix holds the spectrogram (a channel a bin, dB) |

See also: `melgram` `chromagram` `plot` `similarity` `spectroscope~` `buffer~`

### `melgram`

`melgram [bands] [size] [hop]`

Writes the mel spectrogram of a buffer~ into another buffer~, as a background job: the energy (dB) in `bands` triangular filters spaced evenly on the mel scale, the ear's resolution. One channel a band (lowest first), one sample a frame, at the number of frames a second.

| Argument | |
| --- | --- |
| `bands` | the number of mel bands, 2 to 256 (40) |
| `size` | the window, in samples, a power of 2 from 64 to 65536 (2048) |
| `hop` | the step between frames, in samples, 16 or more (512) |

| Message | |
| --- | --- |
| `run sound matrix` | analyses the buffer~ sound and writes the mel spectrogram into the buffer~ matrix |
| `bands n` | sets the number of bands |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound matrix; bands n, size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: matrix holds the mel spectrogram (a channel a band, dB) |

See also: `mfccgram` `spectrogram` `melbands~` `similarity` `plot`

### `mfccgram`

`mfccgram [coeffs] [bands] [size] [hop]`

Writes the mel-frequency cepstral coefficients of a buffer~ into another buffer~, as a background job: the cosine transform of each mel frame, a smooth description of the spectral envelope (the timbre) in a few numbers; coefficient 0 is the overall level. One channel a coefficient, one sample a frame. There are never more coefficients than bands.

| Argument | |
| --- | --- |
| `coeffs` | the number of coefficients, 1 to 128 (13) |
| `bands` | the number of mel bands they are computed from, 2 to 256 (40) |
| `size` | the window, in samples, a power of 2 from 64 to 65536 (2048) |
| `hop` | the step between frames, in samples, 16 or more (512) |

| Message | |
| --- | --- |
| `run sound matrix` | analyses the buffer~ sound and writes the MFCCs into the buffer~ matrix |
| `coeffs n` | sets the number of coefficients |
| `bands n` | sets the number of mel bands |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound matrix; coeffs n, bands n, size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: matrix holds the MFCCs (a channel a coefficient) |

See also: `melgram` `mfcc~` `similarity` `soundtypes` `plot`

### `chromagram`

`chromagram [size] [hop]`

Writes the chromagram of a buffer~ into another buffer~, as a background job: 12 channels, C C# D ... B, how much of each pitch class sounds, every frame scaled to a maximum of 1 (silence stays 0). The pitch classes are read from the spectral peaks (as HPCP), each given to its nearest note from C2 to B6; a peak that is probably the 3rd, 5th, 6th or 7th partial of a lower one counts a tenth. A large window resolves the low notes.

| Argument | |
| --- | --- |
| `size` | the window, in samples, a power of 2 from 256 to 65536 (4096) |
| `hop` | the step between frames, in samples, 16 or more (1024) |

| Message | |
| --- | --- |
| `run sound matrix` | analyses the buffer~ sound and writes the chromagram into the buffer~ matrix |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound matrix; size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: matrix holds the chromagram (12 channels, C first) |

See also: `chroma~` `keyfind` `similarity` `plot` `spectrogram`

### `onsets`

`onsets [threshold] [size] [hop]`

Finds where the notes and hits of a buffer~ begin, as a background job: the peaks of the spectral flux (how much the spectrum grows from frame to frame) above `threshold` times its moving median, at least `gap` ms apart, each placed at the sample where the energy jumps. The times (ms) come out as a list.

The onsets cut the sound into segments, the first from the beginning and the last to the end: after a run, the number of segments comes out, and `segment k` outputs the start and end of one of them, ready for play~.

| Argument | |
| --- | --- |
| `threshold` | how far above the median a peak must be, as a factor (2) |
| `size` | the window, in samples, a power of 2 from 64 to 65536 (1024) |
| `hop` | the step between frames, in samples, 16 or more (256) |

| Message | |
| --- | --- |
| `run sound [curve]` | finds the onsets of the buffer~ sound; the flux goes to the buffer~ curve if one is named |
| `segment k` | outputs the start and end (ms) of segment k, counting from 0 (k wraps around) |
| `threshold x` | sets the threshold factor (2) |
| `width n` | the frames of the moving median, made odd (21) |
| `gap ms` | the shortest time between two onsets (50) |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound [curve]; segment k; threshold x, width n, gap ms, size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |
| 3 | onset times (ms), a list |
| 4 | segment k: start end (ms) |
| 5 | number of segments |

See also: `onset~` `beats` `novelty` `play~` `atoms` `soundtypes`

### `beats`

`beats [size] [hop]`

Estimates the tempo of a buffer~ and tracks its beats, as a background job (Ellis 2007): the onset strength (spectral flux), its autocorrelation read from 30 to 300 bpm and weighted by a prior on the tempo listeners tap, then dynamic programming for beats that are both strong and regular. The tempo (bpm) and the beat times (ms, a list) come out.

| Argument | |
| --- | --- |
| `size` | the window, in samples, a power of 2 from 64 to 65536 (1024) |
| `hop` | the step between frames, in samples, 16 or more (256) |

| Message | |
| --- | --- |
| `run sound [curve]` | finds the tempo and the beats of the buffer~ sound; the onset strength goes to the buffer~ curve if one is named |
| `tempogram sound matrix` | writes the tempogram: 4 frames a second, one channel a bpm from 30 (channel 0) to 300, how periodic the onsets are at that tempo (1: perfectly) |
| `clicks buffer` | after a run, writes a click track into a buffer~: as long as the sound, a short 1 kHz click on every beat |
| `prior bpm octaves` | the centre of the tempo prior, 30 to 300 bpm (100), and its width in octaves (0.6) |
| `tightness x` | how strongly the beats keep to the tempo (100) |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound [curve]; tempogram sound matrix; clicks buffer; prior bpm octaves, tightness x, size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |
| 3 | tempo (bpm) |
| 4 | beat times (ms), a list |

See also: `onsets` `onset~` `keyfind` `metro` `plot`

### `keyfind`

`keyfind [size] [hop]`

Estimates the key of a buffer~, as a background job: the chromagram averaged over the sound (a pitch-class distribution) correlated with the Krumhansl-Kessler profile of every major and minor key (Krumhansl 1990). The key ("A minor"), its correlation (-1 to 1), the distribution (12 values, C first, a maximum of 1) and the correlations with all 24 keys (C major, C minor, C# major ... B minor) come out.

| Argument | |
| --- | --- |
| `size` | the window, in samples, a power of 2 from 256 to 65536 (4096) |
| `hop` | the step between frames, in samples, 16 or more (1024) |

| Message | |
| --- | --- |
| `run sound [from to]` | the key of the buffer~ sound, or of the part between from and to (ms) |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound [from to] (ms); size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |
| 3 | the key: "A minor" |
| 4 | its correlation with the profile (-1..1) |
| 5 | pitch-class distribution: 12 values, C first |
| 6 | correlation with the 24 keys: C major, C minor, C# major... |

See also: `chromagram` `chroma~` `beats`

### `similarity`

`similarity [frames]`

Writes the self-similarity matrix of a feature matrix (a chromagram, an MFCC matrix...) into a buffer~, as a background job: the cosine similarity of every frame with every other, 1 where two moments sound alike (Foote 1999). The structure of a piece becomes a picture: sections are squares on the diagonal, repeats are stripes beside it. Frames are averaged in groups so that the matrix has at most `frames` of them.

| Argument | |
| --- | --- |
| `frames` | the most frames the matrix has, 16 to 4000 (1000) |

| Message | |
| --- | --- |
| `run features matrix` | writes the self-similarity of the buffer~ features into the buffer~ matrix |
| `frames n` | sets the most frames (1000) |
| `standardize 0/1` | 1 takes each feature's mean off and scales it to unit variance first, so that MFCC coefficient 0 does not dominate (0) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run features matrix; frames n (at most), standardize 0/1, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: matrix holds the self-similarity matrix |

See also: `novelty` `chromagram` `mfccgram` `plot`

### `novelty`

`novelty [width] [threshold]`

Finds the section boundaries of a piece from its self-similarity matrix, as a background job (Foote 2000): a checkerboard kernel of `width` frames slides along the diagonal, high where what comes before is unlike what comes after. The curve goes to a buffer~; the boundaries (ms) are its peaks above `threshold` times its moving median, at least `gap` ms apart, and come out as a list. They cut the piece into sections: after a run the number of sections comes out, and `section k` outputs the start and end of one of them.

| Argument | |
| --- | --- |
| `width` | the width of the kernel, in frames of the matrix, 2 to 512 (32) |
| `threshold` | how far above the median a peak must be, as a factor (1.5) |

| Message | |
| --- | --- |
| `run similarity curve` | reads the square matrix in the buffer~ similarity and writes the novelty curve into the buffer~ curve |
| `section k` | outputs the start and end (ms) of section k, counting from 0 (k wraps around) |
| `width n` | sets the kernel width (frames) |
| `threshold x` | sets the threshold factor |
| `gap ms` | the shortest time between two boundaries (2000) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run similarity curve; section k; width n (frames), threshold x, gap ms, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |
| 3 | section boundaries (ms), a list |
| 4 | section k: start end (ms) |
| 5 | number of sections |

See also: `similarity` `onsets` `plot` `soundtypes`

### `curves`

`curves [size] [hop]`

Curves of a buffer~ over time, written to another buffer~ as a background job, one value every `hop` samples. Frame by frame the curves are jumpy: `smooth` passes them through a moving median.

| Argument | |
| --- | --- |
| `size` | the window, in samples, a power of 2 from 64 to 65536 (2048) |
| `hop` | the step between frames, in samples, 16 or more (512) |

| Message | |
| --- | --- |
| `loudness sound curve` | the level in dB, roughly as the ear weighs it (below 150 Hz and above 8 kHz taken out; 0 dB a full-scale sine) |
| `register sound curves` | 3 channels: the spectral centroid in MIDI pitch, the spread around it in octaves, the lowest strong partial in MIDI pitch |
| `polyphony sound curve` | how many voices seem to sound, from the strong spectral peaks, about three a voice (a rough estimate, 0 in silence) |
| `smooth n` | the frames of the moving median, made odd (1: no smoothing) |
| `size n` | sets the window (samples) |
| `hop n` | sets the step (samples) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | loudness sound curve, register sound curves, polyphony sound curve; smooth n, size n, hop n, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: the curve buffer~ is ready |

See also: `descriptors~` `spectrogram` `plot` `soundtypes`

### `descriptors~`

`descriptors~ [size] [hop] [names...]`

Low-level descriptors of its signal, measured over the last `size` samples every `hop` samples while audio runs, one outlet each. The names choose the outlets and their order; without names there are the eleven numbers, in this order: rms (root mean square level), centroid (Hz, the amplitude-weighted mean frequency), spread (Hz, the standard deviation around the centroid), skewness, kurtosis (3 for a Gaussian shape), flux (how much the spectrum grew since the last frame), hfc (high frequency content: the energy weighted by bin number), zcr (zero crossings per second), flatness (0 tonal to 1 noisy), periodicity (0 to 1) and f0 (Hz). Three names give lists instead: mfcc (13 MFCCs over 40 mel bands, as mfcc~), chroma (the 12 pitch classes, as chroma~) and melbands (40 mel bands in dB, as melbands~), at this box's size. All of them come from one FFT a frame.

f0 is found by YIN (de Cheveigné and Kawahara): the first period at which the frame's normalised difference with itself dips under the threshold (0.1), refined between samples and checked against the windows around it. A weak or missing fundamental does not send it an octave up. It is 0 when no period dips under the threshold (noise, silence), and is searched between 50 and 4000 Hz (range), and no lower than 2 sr / size (43 Hz with size 2048 at 44.1 kHz): a larger size reaches lower notes. periodicity is 1 minus the normalised difference at that period: 1 for a periodic sound, near 0 for noise. The formulas are Musil's (descriptors, yin, spectral-moments...), so a script gets the same numbers.

| Argument | |
| --- | --- |
| `size` | the frame, in samples, a power of 2 from 256 to 32768 (2048) |
| `hop` | the step between analyses, in samples, 16 or more (1024) |
| `names` | any of rms centroid spread skewness kurtosis flux hfc zcr flatness periodicity f0 mfcc chroma melbands (the eleven numbers) |

| Message | |
| --- | --- |
| `bang` | analyses the latest frame now |
| `hop n` | sets the step between analyses (samples) |
| `threshold t` | sets YIN's threshold, 0.001 to 1 (0.1); higher calls more frames voiced |
| `range lo hi` | sets the f0 searched, in Hz (50 4000) |

| Inlet | |
| --- | --- |
| 1 | signal to analyse; bang: analyse now; hop n; threshold t; range lo hi |

| Outlet | |
| --- | --- |
| 1 | rms: root mean square level |
| 2 | centroid (Hz): amplitude-weighted mean frequency |
| 3 | spread (Hz): spectral standard deviation around the centroid |
| 4 | skewness: asymmetry of the spectrum |
| 5 | kurtosis: peakedness of the spectrum |
| 6 | flux: how much the spectrum grew since the last frame |
| 7 | hfc: high frequency content |
| 8 | zcr: zero crossings per second |
| 9 | flatness: 0 tonal .. 1 noisy |
| 10 | periodicity: YIN's 1 - aperiodicity, 0..1 |
| 11 | f0 (Hz) by YIN; 0 when unvoiced |

See also: `melbands~` `mfcc~` `chroma~` `onset~` `knn` `plot`

### `melbands~`

`melbands~ [bands] [size] [hop]`

The live mel spectrum of its signal: a list of `bands` values in dB, measured over the last `size` samples every `hop` samples while audio runs. The live version of melgram; into a plot in image mode, the lists scroll by as a picture.

| Argument | |
| --- | --- |
| `bands` | the number of mel bands, 2 to 256 (40) |
| `size` | the window, in samples, a power of 2 from 64 to 32768 (2048) |
| `hop` | the step between analyses, in samples, 16 or more (512) |

| Message | |
| --- | --- |
| `bang` | analyses the latest window now |
| `hop n` | sets the step (samples) |

| Inlet | |
| --- | --- |
| 1 | signal to analyse; bang analyses now; hop n |

| Outlet | |
| --- | --- |
| 1 | mel bands (dB), a list every hop |

See also: `mfcc~` `chroma~` `melgram` `descriptors~` `plot`

### `mfcc~`

`mfcc~ [coeffs] [bands] [size] [hop]`

The live MFCCs of its signal: a list of `coeffs` cepstral coefficients of its mel spectrum, every `hop` samples while audio runs; coefficient 0 is the level. The live version of mfccgram, and a compact description of timbre for knn or kmeans.

| Argument | |
| --- | --- |
| `coeffs` | the number of coefficients, 1 to the number of bands (13) |
| `bands` | the number of mel bands, 2 to 256 (40) |
| `size` | the window, in samples, a power of 2 from 64 to 32768 (2048) |
| `hop` | the step between analyses, in samples, 16 or more (512) |

| Message | |
| --- | --- |
| `bang` | analyses the latest window now |
| `hop n` | sets the step (samples) |

| Inlet | |
| --- | --- |
| 1 | signal to analyse; bang analyses now; hop n |

| Outlet | |
| --- | --- |
| 1 | MFCCs, a list every hop |

See also: `melbands~` `mfccgram` `descriptors~` `knn` `kmeans`

### `chroma~`

`chroma~ [size] [hop]`

The live pitch classes of its signal: a list of 12 values, C first, scaled to a maximum of 1 (all 0 in silence), every `hop` samples while audio runs. The live version of chromagram.

| Argument | |
| --- | --- |
| `size` | the window, in samples, a power of 2 from 256 to 32768 (4096) |
| `hop` | the step between analyses, in samples, 16 or more (1024) |

| Message | |
| --- | --- |
| `bang` | analyses the latest window now |
| `hop n` | sets the step (samples) |

| Inlet | |
| --- | --- |
| 1 | signal to analyse; bang analyses now; hop n |

| Outlet | |
| --- | --- |
| 1 | 12 pitch classes (C first, max 1), a list every hop |

See also: `chromagram` `keyfind` `melbands~` `plot`

### `onset~`

`onset~ [threshold] [size] [hop]`

A bang when a note or a hit begins in its signal: the spectral flux peaks above `threshold` times its median over the last 21 frames, at least `gap` ms after the previous onset, while the level is above `gate` dB. The right outlet gives the flux every frame. With the default size and hop the bang comes about 6 ms after the attack enters the window.

| Argument | |
| --- | --- |
| `threshold` | how far above the median a peak must be, as a factor (2) |
| `size` | the window, in samples, a power of 2 from 64 to 32768 (1024) |
| `hop` | the step between analyses, in samples, 16 or more (256) |

| Message | |
| --- | --- |
| `bang` | analyses the latest window now |
| `threshold x` | sets the threshold factor (2) |
| `gap ms` | the shortest time between two onsets (50) |
| `gate dB` | the level under which nothing counts (-50) |
| `hop n` | sets the step (samples) |

| Inlet | |
| --- | --- |
| 1 | signal to analyse; threshold x, gap ms, gate dB, hop n |

| Outlet | |
| --- | --- |
| 1 | bang: an onset |
| 2 | spectral flux, every hop |

See also: `onsets` `descriptors~` `beats` `peakamp~`

## Atoms and types

### `atoms`

`atoms name`

A dictionary for pursuit, shared by name: pursuit and soundtypes find it by its name. It holds parametric atoms (gabor, gammatone: windowed cosines at every scale, position and frequency) or atoms of sound cut from recordings, each of unit energy. It starts empty; building one from files runs as a background job, and the number of atoms comes out when it is ready (for gabor and gammatone, the number of scales).

The folder messages read every WAV, AIFF, FLAC and MP3 file of a folder (not its subfolders), mixed to mono at 44.1 kHz, and replace the dictionary; `add` appends to a dictionary of sounds. Right-click the box for Info and Save the dictionary...

| Argument | |
| --- | --- |
| `name` | the name pursuit, soundtypes and the others use (required) |

| Message | |
| --- | --- |
| `gabor [min max] [fmax]` | Gaussian-windowed cosines, window lengths from min to max samples (powers of 2, from 32 to 65536; 256 and 8192), frequencies from 40 Hz up to fmax (16000), at the audio's sample rate |
| `gammatone [min max] [fmax]` | the same with a gammatone envelope (sharp attack, slow decay): good for attacks |
| `frames folder [ms] [hop]` | every sound file of a folder cut into Hann-windowed frames of ms milliseconds (100), one every hop ms (the same as ms) |
| `onsets folder [threshold] [max]` | the files cut at their onsets (threshold 2), each piece max ms long at most (1000); pieces under 20 ms are skipped |
| `files folder [max]` | every file whole, max ms at most (2000) |
| `add buffer [frames ms hop \| onsets threshold max \| whole]` | adds atoms cut from a buffer~ (whole by default, 2000 ms at most), at the dictionary's sample rate |
| `get k buffer` | writes atom k (from 0) of a dictionary of sounds into a buffer~, to listen to it |
| `clear` | empties the dictionary |
| `write file` | saves the dictionary (a .atoms file) |
| `read file` | loads a dictionary saved with write |
| `info` | prints its kind and size in the Console |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | gabor [min max] [fmax], gammatone ..., frames folder [ms hop], onsets folder [threshold max], files folder [max], add buffer [mode], get k buffer, clear, write file, read file, info, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: the dictionary is ready |
| 3 | number of atoms |

See also: `pursuit` `soundtypes` `buffer~` `onsets`

### `pursuit`

`pursuit`

Matching pursuit, as a background job: a buffer~ explained as a sum of atoms of a dictionary (an atoms object), the strongest first, then rebuilt, transformed or hybridised. With gabor and gammatone atoms it is an STFT per scale with local updates (every position on a grid of a quarter of the scale, the frequency refined by a parabola, the phase by an exact projection); it stops after `atoms` atoms or when the residual is `snr` dB under the sound. With atoms of sound the target is cut into segments (at its onsets, or in blocks), and each is explained by `components` atoms, each slid to its best place by FFT cross-correlation among the `candidates` closest in mel spectrum.

The decomposition (the book) is a buffer~ of 8 channels, one frame an atom, at the sound's sample rate: time (s, the atom's centre), frequency (Hz), level (dB), dictionary atom (-1 for parametric atoms), length (samples), amplitude, phase, start (samples). plot in scatter mode draws it as an atom map. After a run the number of atoms and the signal-to-residual ratio (dB) come out. A result that would clip is scaled down, with a message in the Console.

| Message | |
| --- | --- |
| `run sound dictionary book [residual]` | decomposes the buffer~ sound (mixed to mono) with the atoms dictionary into the buffer~ book; what is left goes to the buffer~ residual if named |
| `synth book dictionary out` | rebuilds a sound from a book into the buffer~ out, with ratio, stretch, tails and keep |
| `cross book from to out` | rebuilds a book made with dictionary from using the closest atoms (in mel spectrum) of dictionary to: both must be dictionaries of sounds |
| `atoms n` | parametric atoms: the most atoms to find (2000) |
| `snr dB` | parametric atoms: stops when the residual is this far under the sound (30) |
| `components n` | sound atoms: atoms a segment, 1 to 64 (4) |
| `candidates n` | sound atoms: the atoms tried by cross-correlation in each segment (16) |
| `segments onsets \| blocks` | sound atoms: cut the target at its onsets (default) or into blocks |
| `block ms` | the length of the blocks, and the longest segment (100) |
| `threshold x` | the onset threshold for segments onsets (2) |
| `ratio r` | synthesis: transposition, 0.05 to 20 (1) |
| `stretch s` | synthesis: time stretch, 0.05 to 50 (1) |
| `tails 1/0` | synthesis: 1, sound atoms ring whole; 0, they stop where they matched (1) |
| `keep f` | synthesis: the share of the atoms used, strongest first, 0 to 1 (1) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run sound dictionary book [residual], synth book dictionary out, cross book from to out; atoms n, snr dB, components n, candidates n, segments onsets\|blocks, block ms, threshold x, ratio r, stretch s, tails 0/1, keep f, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |
| 3 | atoms found |
| 4 | signal-to-residual (dB) |

See also: `atoms` `soundtypes` `typematch` `plot` `buffer~` `nmf`

### `soundtypes`

`soundtypes name`

A sound analysed into types, after SoundTypes (ICMC 2013), as a background job, and shared by name with typegen and typematch. Level 1: the frames of the sound (13 MFCCs, centroid and level; with `pitch 1` also the 12 pitch classes) or the atoms of a pursuit book are clustered into `types` types (k-means++) and smoothed; runs of one type are the units. With `segment onsets` the units are instead the notes or strokes between onsets, described and clustered as wholes. Levels 2 and 3 group the units of the level below into gestures, cut where the next type is surprising, then cluster them.

After an analysis the number of units of each level comes out as a list. Right-click the box for Info and Write the units as text...

| Argument | |
| --- | --- |
| `name` | the name typegen and typematch use (required) |

| Message | |
| --- | --- |
| `analyse sound` | analyses a buffer~ (mixed to mono) |
| `analyse book dictionary` | analyses the atoms of a pursuit book instead (a book is always cut into frames); its units are rebuilt from their atoms |
| `segment frames` | level 1 from runs of frames of one type (default) |
| `segment onsets [threshold] [gap]` | level 1 from the spans between onsets (threshold 1.5; onsets at least gap ms apart, 80) |
| `pitch 1/0` | adds the pitch classes (chroma) to the frame features, so that types follow harmony too (0) |
| `types k1 [k2 k3]` | the number of types at each level, 2 to 64 (12 6 4) |
| `levels n` | the number of levels, 1 to 3 (2) |
| `threshold z` | how surprising a type must be to end a gesture: its surprise above the mean plus z standard deviations (1) |
| `smooth n` | level-1 labels take the majority over n frames, made odd (5) |
| `size n` | the analysis window, a power of 2 from 256 to 16384 samples (2048) |
| `hop n` | the step between frames, 32 to 8192 samples (512) |
| `seed n` | the seed of the clustering (1) |
| `labels buffer` | writes the type of every frame into a buffer~, one channel a level: plot it with colors types |
| `transitions level buffer` | writes the probabilities of passing from type to type at a level into a buffer~ (a matrix) |
| `play level type` | outputs the start and end (ms) of a unit of that type, chosen at random |
| `write file` | writes the units as text, one a line: level start end type (seconds) |
| `info` | prints the units and types of each level in the Console |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | analyse sound, analyse book dictionary; segment frames \| onsets [threshold gap], pitch 1/0, types k1 k2 k3, levels n, threshold z, smooth n, size n, hop n, seed n; labels buffer, transitions level buffer, play level type, write file, info, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |
| 3 | units per level, a list |
| 4 | play: start end (ms) |

See also: `typegen` `typematch` `pursuit` `atoms` `plot` `kmeans`

### `typegen`

`typegen [model]`

New sequences of the types of a soundtypes model, at one of its levels: the classic n-grams of SoundTypes, a variable-order Markov model, or a factor oracle that follows the original and jumps where the recent past repeats (the default: continuity 0.8, context 1). `generate` realises the units with their own sound (or their atoms), joined by short crossfades, as a background job; `next` walks an endless sequence live, one unit at a time, to drive play~ or groove~ on the original.

| Argument | |
| --- | --- |
| `model` | the name of a soundtypes object |

| Message | |
| --- | --- |
| `generate seconds out [labels]` | writes a new sound of about this length into the buffer~ out, and its types into the buffer~ labels if named (for plot) |
| `next` | the next unit of an endless sequence: its start and end (ms) in the original, then its type |
| `reset` | starts the live sequence again |
| `classic n` | SoundTypes' n-grams: a context of n-1 types (2); a dead end starts again elsewhere |
| `markov [order] [count]` | variable order: the longest context, up to order units (3), seen at least count times (2) |
| `oracle [continuity] [context]` | factor oracle: follows the original with probability continuity, 0 to 1 (0.8), otherwise jumps to where the last context units (1) repeat |
| `level n` | generates units of level n, 1 to 3 (1) |
| `inner c [context]` | levels mixed: units of level 2 or 3 are played with the level below regenerated, an oracle walk of continuity c (0 to 1) and context (1) from each one's first sub-unit; 1 (default) plays them as they are |
| `model name` | uses another soundtypes object |
| `seed n` | the seed of the random choices (1) |
| `xfade ms` | the crossfade between units, 0.5 to 500 ms (10) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | generate seconds out [labels], next, reset; classic n, markov [order count], oracle [continuity context], level n, inner c [context], model name, seed n, xfade ms, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |
| 3 | next: the unit's type |
| 4 | next: start end (ms) in the original |

See also: `soundtypes` `typematch` `play~` `groove~` `plot`

### `typematch`

`typematch`

SoundTypes' rematch, as a background job: every frame, unit or gesture of sound A replaced by the closest one of sound B (duration included) in its closest type. A and B are soundtypes models analysed the same way (both sounds, or both books). At level 0, B's frames are taken as Hann grains of its analysis size and overlap-added at A's frames (the original rematch); at level 1 unit by unit; at level 2 gesture by gesture. A result that would clip is scaled down.

| Message | |
| --- | --- |
| `run A B out [level] [labels]` | rebuilds A with the material of B into the buffer~ out; the types of B over A's time go to the buffer~ labels if named |
| `level n` | 0, frame by frame; 1, unit by unit; 2 or 3, by the units of that level (gestures) (1) |
| `space own \| joint` | features standardised in each sound's own space, so that B's range covers A's (default), or in one space for both (absolute distances) |
| `fit 1/0` | 1, at A's times and durations (B's units cut or continued); 0, one after the other at B's own durations (1); not used at level 0 |
| `contour c` | A's loudness imposed on the result by c, 0 (B's own) to 1 (A's), so that A's rhythm and phrasing come through (0; the message alone sets 1); above level 0, only with fit 1 |
| `xfade ms` | the crossfade between units, 0.5 to 500 ms (10) |
| `stop` | cancels the job |

| Inlet | |
| --- | --- |
| 1 | run A B out [level] [labels]; level 0-3, fit 1/0, contour c, space own/joint, xfade ms, stop |

| Outlet | |
| --- | --- |
| 1 | progress, 0 to 1 |
| 2 | bang: done |

See also: `soundtypes` `typegen` `pursuit` `atoms`

## Learning

### `knn`

`knn [k]`

k nearest neighbours: learns from examples (add x1 x2 ... -> y1 y2 ...), then answers a list with the outputs of the k examples nearest to it. Regression: the mean of their outputs, or with `weighted 1` a mean weighted by inverse distance, which glides smoothly between the examples (as Wekinator). Classification, with `classify 1` or when the outputs are words: the most frequent output among them. Inputs are compared after standardisation, so values in different units weigh alike. The index of the nearest example also comes out.

| Argument | |
| --- | --- |
| `k` | the number of neighbours, 1 to 1000 (3) |

| Message | |
| --- | --- |
| `list` | predicts the outputs for this input |
| `add x1 x2 ... -> y1 y2 ...` | adds an example; the outputs may be numbers or words |
| `k n` | sets the number of neighbours |
| `weighted 0/1` | 1 weights the neighbours by inverse distance (0) |
| `classify 0/1` | 1 outputs the most frequent output instead of the mean (0) |
| `normalize 0/1` | 0 compares raw values instead of standardised ones (1) |
| `clear` | forgets the examples |
| `write file` | saves the examples as text, one a line (as add, without the word add) |
| `read file` | replaces the examples with those of a file |
| `print` | prints the number of examples in the Console |

| Inlet | |
| --- | --- |
| 1 | add x... -> y..., a list to predict; k n, weighted 0/1, classify 0/1, normalize 0/1, clear, write/read |

| Outlet | |
| --- | --- |
| 1 | prediction: the outputs (or the class) |
| 2 | index of the nearest example |

See also: `kmeans` `linreg` `zscore` `pca` `mfcc~` `descriptors~`

### `kmeans`

`kmeans`

Groups the points added into k clusters: `train k` starts with k-means++ and runs Lloyd's iterations, ten times from different starts, and keeps the tightest clustering; it prints the cluster sizes. A list then gives the index of its cluster (0 to k-1) and that cluster's centroid. Points are standardised first; adding or clearing points undoes the training.

| Message | |
| --- | --- |
| `list` | the cluster of this point and its centroid |
| `add x1 x2 ...` | adds a point |
| `train [k] [iterations]` | clusters the points into k (2), with at most this many iterations (100) |
| `centroid i` | outputs the centroid of cluster i |
| `seed n` | the seed of the random starts (1) |
| `normalize 0/1` | 0 clusters raw values instead of standardised ones (1) |
| `clear` | forgets the points |
| `write file` | saves the points as text, one a line |
| `read file` | replaces the points with those of a file |
| `print` | prints the number of points and of clusters in the Console |

| Inlet | |
| --- | --- |
| 1 | add x..., train k [iterations], a list to classify; centroid i, seed n, normalize 0/1, clear, write/read |

| Outlet | |
| --- | --- |
| 1 | cluster of the list, 0..k-1 |
| 2 | that cluster's centroid |

See also: `knn` `pca` `zscore` `soundtypes`

### `pca`

`pca`

Principal component analysis: `train` finds the directions of largest variance of the points added (standardised first) and outputs the share of the variance each carries. A list then gives its coordinates along them: a way to see many descriptors on two axes. At least two points are needed.

| Message | |
| --- | --- |
| `list` | the coordinates of this point on the components |
| `add x1 x2 ...` | adds a point |
| `train [dims]` | finds the first dims components (2) |
| `explained` | outputs the share of the variance of each component |
| `normalize 0/1` | 0 uses raw values instead of standardised ones (1) |
| `clear` | forgets the points |
| `write file` | saves the points as text, one a line |
| `read file` | replaces the points with those of a file |
| `print` | prints the number of points in the Console |

| Inlet | |
| --- | --- |
| 1 | add x..., train [dims], a list to project; explained, normalize 0/1, clear, write/read |

| Outlet | |
| --- | --- |
| 1 | coordinates on the components |
| 2 | share of the variance of each component |

See also: `zscore` `kmeans` `knn` `plot` `descriptors~`

### `zscore`

`zscore`

Standardises lists: `train` measures the mean and standard deviation of each value over the points added; a list then comes out as (x - mean) / sd, so values of different units (Hz, dB, 0 to 1) become comparable.

| Message | |
| --- | --- |
| `list` | standardises this list |
| `add x1 x2 ...` | adds a point |
| `train` | measures the means and standard deviations |
| `clear` | forgets the points |
| `write file` | saves the points as text, one a line |
| `read file` | replaces the points with those of a file |
| `print` | prints the number of points in the Console |

| Inlet | |
| --- | --- |
| 1 | add x..., train, a list to standardise; clear, write/read |

| Outlet | |
| --- | --- |
| 1 | (x - mean) / sd |

See also: `pca` `kmeans` `knn` `scale`

### `linreg`

`linreg`

Linear regression: `train` fits y = b0 + b1 x1 + b2 x2 ... to the examples added by least squares, one set of weights for each output; a list then gives the predicted outputs.

| Message | |
| --- | --- |
| `list` | predicts the outputs for this input |
| `add x1 x2 ... -> y1 y2 ...` | adds an example |
| `train` | fits the weights |
| `weights` | outputs the weights, intercept first, output after output |
| `clear` | forgets the examples |
| `write file` | saves the examples as text, one a line |
| `read file` | replaces the examples with those of a file |
| `print` | prints the number of examples in the Console |

| Inlet | |
| --- | --- |
| 1 | add x... -> y..., train, a list to predict; weights, clear, write/read |

| Outlet | |
| --- | --- |
| 1 | predicted outputs |
| 2 | weights: intercept first, output after output |

See also: `knn` `zscore` `pca` `expr`

## I/O

### `adc~`

`adc~ [ch...]`

Audio input: one outlet per channel given, numbered from 1. The input device is opened with all its channels (Patch > Audio Setup can limit them), so adc~ 1 2 3 4 5 6 7 8 reads an eight-channel interface; a channel the device does not have outputs silence. The input opens only when a patch contains adc~.

| Argument | |
| --- | --- |
| `ch` | the input channels (1 2) |

| Message | |
| --- | --- |
| `start` | turns audio on (also startwindow) |
| `stop` | turns audio off |

| Inlet | |
| --- | --- |
| 1 | start / stop |

| Outlet | |
| --- | --- |
| 1 | audio input channel 1 |
| 2 | audio input channel 2 |

See also: `dac~` `dspstate~` `meter~` `record~`

### `dac~`

`dac~ [ch...]`

Audio output: one inlet per channel given, numbered from 1. Several dac~s add up on the same channel. The output device is opened with all its channels (Patch > Audio Setup can limit them), so dac~ 1 2 3 4 5 6 7 8 plays on eight speakers. Channels beyond the device's are not heard, but the toolbar's meter shows them and the quick recorder writes them, and --render and the render object write them: an eight-channel piece can be made on a stereo card. A console note says when this happens.

| Argument | |
| --- | --- |
| `ch` | the output channels (1 2) |

| Message | |
| --- | --- |
| `start` | turns audio on (also startwindow) |
| `stop` | turns audio off |

| Inlet | |
| --- | --- |
| 1 | signal to output channel 1; start / stop |
| 2 | signal to output channel 2; start / stop |

See also: `adc~` `gain~` `dspstate~` `meter~`

### `dspstate~`

`dspstate~ [ms]`

Reports the audio state on bang, whenever audio starts, stops or is reopened, and every `ms` while audio runs. Its outlets, left to right, are as in Max: 1/0 for audio on or off, the sample rate, the signal vector size (64, or the frame's bin count inside pfft~) and the I/O buffer size; a fifth gives the CPU load in %. While audio is off it reports the last sample rate and buffer size.

| Argument | |
| --- | --- |
| `ms` | the reporting interval in ms (at least 20), 0 = only on bang and on changes (0) |

| Message | |
| --- | --- |
| `bang` | reports now |
| `ms` | sets the interval (also interval ms) |

| Inlet | |
| --- | --- |
| 1 | bang: report now; a number: also report every ms while audio runs (0 = off) |

| Outlet | |
| --- | --- |
| 1 | 1 while audio runs, 0 when off |
| 2 | sample rate (Hz) |
| 3 | signal vector size (samples) |
| 4 | I/O buffer size (samples) |
| 5 | CPU load (% of the buffer's time) |

See also: `adc~` `dac~` `loadbang`

## MIDI

### `notein`

`notein [channel]`

Notes from a MIDI keyboard or controller (the device is chosen in Patch > MIDI Setup). Outputs pitch and velocity, a velocity of 0 for a note-off, and, without a channel argument, the channel, from right to left: the pitch comes last.

| Argument | |
| --- | --- |
| `channel` | 1 to 16 to listen to that channel only; 0 or none for all channels, with an outlet for the channel (0) |

| Outlet | |
| --- | --- |
| 1 | pitch |
| 2 | velocity (0 = note-off) |
| 3 | channel (1-16) |

See also: `noteout` `stripnote` `makenote` `ctlin` `mtof`

### `ctlin`

`ctlin [controller] [channel]`

Control changes from MIDI input. Outputs the value, 0 to 127, then the controller number (only without a controller argument) and the channel (only without a channel argument), from right to left: the value comes last.

| Argument | |
| --- | --- |
| `controller` | 0 to 127 to listen to that controller only; none, or a number outside, for all (all) |
| `channel` | 1 to 16 to listen to that channel only; 0 or none for all (0) |

| Outlet | |
| --- | --- |
| 1 | controller value (0-127) |
| 2 | controller number, or channel |
| 3 | channel (1-16) |

See also: `ctlout` `bendin` `pgmin` `notein` `scale`

### `bendin`

`bendin [channel]`

Pitch bend from MIDI input, as 0 to 127 with 64 at the centre (the coarse part of the bend, as in Max). Without a channel argument a second outlet gives the channel.

| Argument | |
| --- | --- |
| `channel` | 1 to 16 to listen to that channel only; 0 or none for all (0) |

| Outlet | |
| --- | --- |
| 1 | pitch bend, 0-127 (64 = centre) |
| 2 | channel (1-16) |

See also: `ctlin` `pgmin` `notein` `scale`

### `pgmin`

`pgmin [channel]`

Program changes from MIDI input, as 1 to 128. Without a channel argument a second outlet gives the channel.

| Argument | |
| --- | --- |
| `channel` | 1 to 16 to listen to that channel only; 0 or none for all (0) |

| Outlet | |
| --- | --- |
| 1 | program number, 1-128 |
| 2 | channel (1-16) |

See also: `ctlin` `bendin` `notein`

### `noteout`

`noteout [channel]`

Sends MIDI notes to the output chosen in Patch > MIDI Setup (by default the virtual port Moritz, which a synth or DAW can listen to). A pitch in the left inlet sends a note with the velocity last set in the middle inlet; velocity 0 is a note-off. The velocity is 0 until it is set, so set it first, or send pitch velocity. Values are clipped to 0-127.

| Argument | |
| --- | --- |
| `channel` | the MIDI channel, 1 to 16 (1) |

| Message | |
| --- | --- |
| `number` | sends a note with that pitch and the stored velocity |
| `list pitch velocity` | sets the velocity and sends the note |

| Inlet | |
| --- | --- |
| 1 | pitch: sends the note (pitch velocity works too) |
| 2 | velocity (0 = note-off) |
| 3 | channel (1-16) |

See also: `makenote` `notein` `ctlout` `kslider`

### `ctlout`

`ctlout [controller] [channel]`

Sends MIDI control changes to the output chosen in Patch > MIDI Setup. A number in the left inlet sends that value, clipped to 0-127, to the controller and channel set by the arguments or the other inlets.

| Argument | |
| --- | --- |
| `controller` | the controller number, 0 to 127 (7, volume) |
| `channel` | the MIDI channel, 1 to 16 (1) |

| Message | |
| --- | --- |
| `number` | sends it as the controller's value |

| Inlet | |
| --- | --- |
| 1 | value: sends it (0-127) |
| 2 | controller number |
| 3 | channel (1-16) |

See also: `ctlin` `noteout` `scale`

### `makenote`

`makenote [velocity] [duration]`

Makes a note-on now and its note-off later: a pitch in the left inlet goes out at once with the velocity, and again with velocity 0 after the duration. A velocity of 0 sends only that, with nothing scheduled. Velocity and duration can also be changed in the other inlets.

| Argument | |
| --- | --- |
| `velocity` | the velocity of the note-ons, 0 to 127 (100) |
| `duration` | the time to the note-off, in milliseconds (250) |

| Message | |
| --- | --- |
| `number` | outputs the pitch with the velocity, and schedules its note-off |
| `list pitch velocity` | sets the velocity, then the same |
| `stop` | sends the pending note-offs now |
| `clear` | forgets the pending note-offs, without sending them |

| Inlet | |
| --- | --- |
| 1 | pitch: note-on now, note-off later; stop, clear |
| 2 | velocity |
| 3 | duration (ms) |

| Outlet | |
| --- | --- |
| 1 | pitch |
| 2 | velocity (0 for the note-off) |

See also: `noteout` `stripnote` `poly~` `metro` `delay`

### `stripnote`

`stripnote`

Passes note-ons and drops note-offs: a pitch in the left inlet goes out, with the velocity last set in the right inlet, only when that velocity is above 0. Outputs go right to left: velocity, then pitch.

| Message | |
| --- | --- |
| `number` | passes the pitch, with the stored velocity, when it is above 0 |
| `list pitch velocity` | sets the velocity, then the same |

| Inlet | |
| --- | --- |
| 1 | pitch: passes when the velocity is above 0 |
| 2 | velocity |

| Outlet | |
| --- | --- |
| 1 | pitch |
| 2 | velocity |

See also: `notein` `makenote` `select` `kslider`

## Timing

### `metro`

`metro [interval]`

Outputs a bang at a regular interval while it runs; the first bang comes at once. Ticks are scheduled at absolute times, so they don't drift. A new interval, in the right inlet, is used from the next tick on: the bang already scheduled comes first.

| Argument | |
| --- | --- |
| `interval` | the time between bangs in milliseconds, at least 1 (500) |

| Message | |
| --- | --- |
| `bang` | starts it (or restarts it, with a bang now) |
| `start` | the same as bang |
| `number` | not 0 starts it, 0 stops it |
| `stop` | stops it |

| Inlet | |
| --- | --- |
| 1 | 1 or bang starts, 0 or stop stops |
| 2 | interval (ms) |

| Outlet | |
| --- | --- |
| 1 | bang every interval |

See also: `delay` `counter` `line` `random` `makenote`

### `delay` `del`

`delay [time]`

Outputs a bang a given time after it gets one. A bang while it waits starts the wait again, so only one bang comes out. The right inlet stores the time for the next bang.

| Argument | |
| --- | --- |
| `time` | the delay in milliseconds (0) |

| Message | |
| --- | --- |
| `bang` | starts the wait (or starts it again) |
| `number` | sets the time and starts the wait |
| `stop` | cancels the bang still to come |

| Inlet | |
| --- | --- |
| 1 | bang starts (or restarts), stop cancels; a number sets the time and starts |
| 2 | delay time (ms) |

| Outlet | |
| --- | --- |
| 1 | bang after the delay |

See also: `metro` `makenote` `trigger` `line`

### `line`

`line [start] [grain]`

Control-rate ramps: target time goes from the current value to target in time milliseconds, outputting a value every grain milliseconds (the first one at once). A number alone jumps there, or ramps over the time set in the middle inlet, which then goes back to 0, as in Max. The right inlet sets the grain. To ramp a signal (an amplitude, a frequency) without steps, use line~.

| Argument | |
| --- | --- |
| `start` | the value it starts from (0) |
| `grain` | milliseconds between outputs during a ramp, at least 1 (20) |

| Message | |
| --- | --- |
| `target time grain` | ramps to target in time ms, and sets the grain |
| `target time` | ramps to target in time ms |
| `number` | jumps there, or ramps over the time set in the middle inlet |
| `set x` | stops any ramp and sets the value to x, without output |
| `stop` | stops the ramp where it is |

| Inlet | |
| --- | --- |
| 1 | target [time [grain]]: ramp to target |
| 2 | ramp time (ms) |
| 3 | grain: ms between outputs |

| Outlet | |
| --- | --- |
| 1 | ramp values |

See also: `line~` `metro` `scale` `delay`

### `loadbang`

`loadbang`

Sends a bang when the patch opens: use it to set starting values. A bang in its inlet, or a double-click on it in locked mode, fires it again.

| Message | |
| --- | --- |
| `bang` | fires it again |

| Inlet | |
| --- | --- |
| 1 | bang: fire again |

| Outlet | |
| --- | --- |
| 1 | bang when the patch opens |

See also: `metro` `message` `send` `delay`

## Control Math

### `+` `-` `*` `/` `%`

`+ [operand]`

Arithmetic on numbers: add, subtract, multiply, divide, remainder. The left inlet is hot: a number sets the left operand and outputs the result; the right inlet only stores the right operand. As in Max, the argument sets the type: an integer argument, or none, truncates both operands (7 / 2 is 3, 7.5 / 2 is 3); an argument with a point (2. or 0.5) works in decimals (7 / 2. is 3.5).

Dividing by 0, or a remainder by 0, gives 0. % keeps the sign of the left operand: -7 % 3 is -1.

| Argument | |
| --- | --- |
| `operand` | the right operand, and the type: integer, or decimal when written with a point (0) |

| Message | |
| --- | --- |
| `number` | sets the left operand and outputs the result |
| `list a b` | sets both operands and outputs the result |
| `bang` | outputs the result again |

| Inlet | |
| --- | --- |
| 1 | left operand: outputs the result |
| 2 | right operand (stored) |

| Outlet | |
| --- | --- |
| 1 | result |

See also: `expr` `scale` `==` `trigger`

### `==` `!=` `>` `<` `>=` `<=`

`== [operand]`

Comparisons: equal, not equal, greater, less, greater or equal, less or equal. The output is 1 when the comparison holds, 0 when it does not. The left inlet is hot, the right one stores the number to compare with. Typed like the arithmetic: an integer argument, or none, truncates the input (0.5 > 0 is 0); write > 0. to compare decimals.

| Argument | |
| --- | --- |
| `operand` | the number to compare with, and the type: integer, or decimal when written with a point (0) |

| Message | |
| --- | --- |
| `number` | compares it and outputs 1 or 0 |
| `list a b` | sets both numbers and outputs the result |
| `bang` | outputs the result again |

| Inlet | |
| --- | --- |
| 1 | left operand: outputs the result |
| 2 | right operand (stored) |

| Outlet | |
| --- | --- |
| 1 | result |

See also: `+` `if` `select` `expr` `gate`

### `mtof`

`mtof`

Converts a MIDI note number to a frequency in Hz, with A 440 Hz at note 69 and twelve equal steps per octave. Fractional notes are kept: 60.5 is a quarter tone above middle C.

| Message | |
| --- | --- |
| `number` | converts it and outputs the frequency |
| `bang` | outputs the last result again |

| Inlet | |
| --- | --- |
| 1 | MIDI note |

| Outlet | |
| --- | --- |
| 1 | frequency in Hz |

See also: `ftom` `dbtoa` `notein` `expr`

### `ftom`

`ftom`

Converts a frequency in Hz to a MIDI note number (440 Hz is 69). The result is not rounded: 450 Hz gives 69.39. A frequency of 0 or less gives 0.

| Message | |
| --- | --- |
| `number` | converts it and outputs the note |
| `bang` | outputs the last result again |

| Inlet | |
| --- | --- |
| 1 | frequency in Hz |

| Outlet | |
| --- | --- |
| 1 | MIDI note |

See also: `mtof` `atodb` `expr`

### `dbtoa`

`dbtoa`

Converts decibels to a linear amplitude: 0 dB is 1, -6 dB about 0.5, -20 dB 0.1.

| Message | |
| --- | --- |
| `number` | converts it and outputs the amplitude |
| `bang` | outputs the last result again |

| Inlet | |
| --- | --- |
| 1 | decibels |

| Outlet | |
| --- | --- |
| 1 | amplitude |

See also: `atodb` `mtof` `scale`

### `atodb`

`atodb`

Converts a linear amplitude to decibels: 1 is 0 dB, 0.5 about -6 dB. An amplitude of 0 or less gives -999.

| Message | |
| --- | --- |
| `number` | converts it and outputs the decibels |
| `bang` | outputs the last result again |

| Inlet | |
| --- | --- |
| 1 | amplitude |

| Outlet | |
| --- | --- |
| 1 | decibels |

See also: `dbtoa` `ftom` `scale`

### `scale`

`scale [in-low] [in-high] [out-low] [out-high] [exp]`

Maps a number from one range to another. Values outside the input range are not clipped; reversed ranges invert the map. With an exponent of 1 the map is a straight line; above 1 it starts slowly and speeds up, below 1 the reverse (the curve is mirrored for values below the input low). As in Max, exactly four integer arguments make the output integers (truncated); any other case outputs decimals. The inlets after the first change the five parameters.

| Argument | |
| --- | --- |
| `in-low` | the low end of the input range (0) |
| `in-high` | the high end of the input range (127) |
| `out-low` | the output for in-low (0) |
| `out-high` | the output for in-high (1) |
| `exp` | the curve exponent (1, a straight line) |

| Message | |
| --- | --- |
| `number` | maps it and outputs the result |
| `bang` | outputs the last result again |

| Inlet | |
| --- | --- |
| 1 | value to map |
| 2 | input low |
| 3 | input high |
| 4 | output low |
| 5 | output high |
| 6 | curve exponent |

| Outlet | |
| --- | --- |
| 1 | mapped value |

See also: `expr` `+` `mtof` `dbtoa` `line`

### `expr`

`expr expression`

Evaluates a C-like expression of its inputs: $f1..$f9 are decimals, $i1..$i9 the same inputs truncated to integers ($v1..$v9, for expr~, read like $f). There is one inlet per variable, up to the highest number used; the left one is hot, the others only store. A syntax error is printed to the console when the box is made, and the box then outputs nothing. A result that is not a finite number gives 0.

As in Max, values are typed: $i, integer constants (12) and the results of comparisons are integers; $f and constants with a point or an exponent (12., 1e3) are decimals. A division between two integers is an integer one: 7 / 2 is 3, while 7. / 2 or $f1 / 2 is 3.5.

Operators: + - * / % (dividing by 0 gives 0), the comparisons == != < > <= >= (1 or 0), && || ! (logic), unary minus, and ( ) for grouping. Functions of one argument: sin cos tan asin acos atan sinh cosh tanh exp log log10 sqrt abs fabs floor ceil int round. Of two: pow(x, y), atan2(y, x), min(a, b), max(a, b), fmod(a, b), random(lo, hi) (a decimal from lo to hi: the same numbers at each run, the seed coming from the box's id; `seed n` seeds it again). The constant pi.

| Argument | |
| --- | --- |
| `expression` | the expression, written on the box (required) |

| Message | |
| --- | --- |
| `number` | sets $f1 / $i1 and outputs the result |
| `list a b ...` | sets $f1, $f2 ... in order and outputs the result |
| `bang` | outputs the result again |

| Inlet | |
| --- | --- |
| 1 | $f1 / $i1: sets it and outputs; bang outputs |

| Outlet | |
| --- | --- |
| 1 | result |

See also: `expr~` `if` `+` `scale` `trigger`

### `if`

`if condition then message [else message]`

Chooses a message by a condition, as in Max. The condition is an expr expression over $i1..$i9 and $f1..$f9; when it is not 0 the message after then goes out, otherwise the one after else (if any). The messages are written like message boxes and use the same variables, plus $s1 for a word arriving at the left inlet. There is one inlet per variable, up to the highest number used (in the condition or the messages); the left one is hot.

out2 at the start of a message sends it from a second outlet (which exists only when out2 is used); send name at the start sends it to every receive name instead of an outlet. Errors, such as a missing then, are printed to the console.

| Argument | |
| --- | --- |
| `condition` | an expression, as in expr (required) |
| `then message` | the message sent when the condition holds (required) |
| `else message` | the message sent otherwise (none) |

| Message | |
| --- | --- |
| `number` | sets $i1 / $f1 and evaluates |
| `list a b ...` | sets the variables in order and evaluates |
| `word` | sets $s1 and evaluates |
| `bang` | evaluates again |
| `set a b ...` | stores the variables in order, without evaluating |

| Inlet | |
| --- | --- |
| 1 | $i1 / $f1 (or a word for $s1): sets it and evaluates; bang evaluates; set ...: store only |

| Outlet | |
| --- | --- |
| 1 | the message after then or else |

See also: `expr` `==` `select` `route` `gate` `send`

### `random`

`random [range]`

Outputs a random integer from 0 to range-1 on each bang. The generator starts from a different seed every time; seed makes the sequence repeatable. A range below 1 is taken as 1.

| Argument | |
| --- | --- |
| `range` | how many different integers it outputs (2) |

| Message | |
| --- | --- |
| `bang` | outputs a new random integer |
| `seed x` | restarts the generator from seed x, so the same sequence follows |

| Inlet | |
| --- | --- |
| 1 | bang: output a random integer; seed x: repeatable |
| 2 | range (the next integers are 0 .. range-1) |

| Outlet | |
| --- | --- |
| 1 | random integer 0 .. range-1 |

See also: `counter` `expr` `scale` `metro`

### `counter`

`counter [max] | counter min max`

Counts. Each bang outputs the count, then steps it up by one; after max (included) it goes back to min: counter 3 counts 0 1 2 3 0 1 ... With no arguments it keeps counting up. Arguments given in the wrong order are swapped. The right inlet sets the next value, like set.

| Argument | |
| --- | --- |
| `min` | the first value, and the value after max (0) |
| `max` | the last value before it wraps (no limit without arguments) |

| Message | |
| --- | --- |
| `bang` | outputs the count, then steps it |
| `number` | the same as bang; the number itself is ignored |
| `set n` | n is the next value output |
| `reset` | goes back to min, without output |

| Inlet | |
| --- | --- |
| 1 | bang: output the count, then step |
| 2 | next value |

| Outlet | |
| --- | --- |
| 1 | count |

See also: `metro` `random` `select` `+` `int`

## Routing

### `trigger` `t`

`trigger [types...]`

Sends its input out of every outlet, from right to left, converting it for each outlet: use it to order events, and to fill cold inlets before hot ones. One outlet per argument: b a bang, f the first number (0 for a word), i that number truncated, l or a the whole message, s the first word. Any other argument (a number or a word) is sent as it is, a constant. With no arguments it is trigger b b.

| Argument | |
| --- | --- |
| `types` | one per outlet: b, f, i, l, a, s or a constant (b b) |

| Message | |
| --- | --- |
| `anything` | sent out of the outlets, right to left, converted |

| Inlet | |
| --- | --- |
| 1 | anything: sent out of the outlets from right to left |

| Outlet | |
| --- | --- |
| 1 | the input as it is (outlets fire right to left) |
| 2 | the input as it is (outlets fire right to left) |

See also: `pack` `unpack` `int` `delay` `route`

### `pack`

`pack [items...]`

Builds a list from separate values: one inlet per item. The left inlet is hot: a number sets the first item and outputs the list; the others only store. Each argument gives a slot its first value and its type, as in Max: an integer (0) or i is an integer and truncates what it gets, a decimal (0.) or f keeps it, s or any other word holds words (a word is also its first value). A word sent to a number slot stores 0. With no arguments it is pack 0 0.

| Argument | |
| --- | --- |
| `items` | one per slot: its first value and type (0 0) |

| Message | |
| --- | --- |
| `number` | sets the first item and outputs the list |
| `list a b ...` | sets the items from the first and outputs the list |
| `bang` | outputs the list again |
| `set a b ...` | sets the items from the first, without output |

| Inlet | |
| --- | --- |
| 1 | first item: outputs the list |
| 2 | item (stored) |

| Outlet | |
| --- | --- |
| 1 | the list |

See also: `unpack` `prepend` `trigger` `route`

### `unpack`

`unpack [items...]`

Splits a list: one outlet per argument, sending the items out from right to left. The arguments type the outlets as in pack: an integer (0) or i truncates, a decimal (0.) or f passes decimals, s or another word passes words; a word arriving at a number outlet gives 0. Extra items are dropped. With no arguments it is unpack 0 0.

| Argument | |
| --- | --- |
| `items` | one per outlet: its type (0 0) |

| Message | |
| --- | --- |
| `list a b ...` | sends each item out of its outlet, right to left |

| Inlet | |
| --- | --- |
| 1 | list to split |

| Outlet | |
| --- | --- |
| 1 | list item |
| 2 | list item |

See also: `pack` `route` `trigger`

### `prepend`

`prepend [words...]`

Puts its words before every message it gets and outputs the result. prepend set turns a list into set ..., which shows it in a message box without sending it. A bang alone outputs the words alone. The right inlet replaces the words with the message it gets.

| Argument | |
| --- | --- |
| `words` | the words put in front (none) |

| Message | |
| --- | --- |
| `anything` | output with the words before it |
| `bang` | outputs the words alone |

| Inlet | |
| --- | --- |
| 1 | a message: output with the arguments before it |
| 2 | new arguments |

| Outlet | |
| --- | --- |
| 1 | the message, prefixed |

See also: `append` `route` `message` `pack`

### `append`

`append [words...]`

Puts its words after every message it gets and outputs the result. A bang alone outputs the words alone. The right inlet replaces the words with the message it gets.

| Argument | |
| --- | --- |
| `words` | the words put after (none) |

| Message | |
| --- | --- |
| `anything` | output with the words after it |
| `bang` | outputs the words alone |

| Inlet | |
| --- | --- |
| 1 | a message: output with the arguments after it |
| 2 | new arguments |

| Outlet | |
| --- | --- |
| 1 | the message, extended |

See also: `prepend` `route` `message` `pack`

### `route`

`route [keys...]`

Dispatches messages by their first word: a message whose first word (or number) equals a key goes out of that key's outlet without it, as a bang when nothing is left. Anything else goes out of the last outlet, unchanged. Keys can be words or numbers: route 1 2 sends 2 64 out of its second outlet as 64. With no arguments it is route 0.

| Argument | |
| --- | --- |
| `keys` | one per outlet, words or numbers (0) |

| Message | |
| --- | --- |
| `anything` | dispatched by its first word |

| Inlet | |
| --- | --- |
| 1 | message: its first word picks the outlet |

| Outlet | |
| --- | --- |
| 1 | messages starting with "?", without that word |
| 2 | messages that matched nothing |

See also: `select` `gate` `unpack` `prepend` `if`

### `select` `sel`

`select [values...]`

Bangs the outlet whose argument equals the input; anything else goes out of the last outlet unchanged. Only the first word of a message is compared, and a number never equals a word. With a single argument there is a right inlet, which changes the value to match. With no arguments it is select 0.

| Argument | |
| --- | --- |
| `values` | one per outlet, words or numbers (0) |

| Message | |
| --- | --- |
| `anything` | a bang from the matching outlet, or passed out of the last one |

| Inlet | |
| --- | --- |
| 1 | value to match |
| 2 | new value to match (with one key) |

| Outlet | |
| --- | --- |
| 1 | bang when the input is ? |
| 2 | input that matched nothing |

See also: `route` `==` `gate` `counter`

### `gate`

`gate [outlets] [open]`

Passes messages to one of its outlets, or to none. The left inlet chooses the outlet, from 1, and 0 closes the gate (numbers outside are clipped); the right inlet takes the data. Mind the order: the choice is on the left, the data on the right. gate 1 with a toggle is an on/off switch.

| Argument | |
| --- | --- |
| `outlets` | how many outlets, 1 to 64 (1) |
| `open` | the outlet open at first, 0 for closed (0) |

| Message | |
| --- | --- |
| `number` | in the left inlet, chooses the open outlet (0 closes) |
| `anything` | in the right inlet, sent out of the open outlet |

| Inlet | |
| --- | --- |
| 1 | which outlet is open (0 closes all) |
| 2 | data to let through |

| Outlet | |
| --- | --- |
| 1 | output while open |

See also: `route` `select` `selector~` `if`

### `int` `i`

`int [value]`

Stores an integer and outputs it again on bang. Numbers are truncated toward 0: 3.7 becomes 3, and -3.7 becomes -3. The right inlet stores without output.

| Argument | |
| --- | --- |
| `value` | the first value (0) |

| Message | |
| --- | --- |
| `number` | stores it and outputs it |
| `bang` | outputs the stored value |
| `set x` | stores x without output |

| Inlet | |
| --- | --- |
| 1 | number: store and output; bang: output |
| 2 | number: store only |

| Outlet | |
| --- | --- |
| 1 | the stored integer |

See also: `float` `trigger` `pack` `counter`

### `float` `f`

`float [value]`

Stores a decimal number and outputs it again on bang. The right inlet stores without output.

| Argument | |
| --- | --- |
| `value` | the first value (0) |

| Message | |
| --- | --- |
| `number` | stores it and outputs it |
| `bang` | outputs the stored value |
| `set x` | stores x without output |

| Inlet | |
| --- | --- |
| 1 | number: store and output; bang: output |
| 2 | number: store only |

| Outlet | |
| --- | --- |
| 1 | the stored float |

See also: `int` `trigger` `pack` `flonum`

### `send` `s`

`send name`

Sends every message it gets to all the receive objects with the same name, without cables. Names are shared by all open patches and their subpatchers, as in Max, so choose names that won't clash. A message box sends too: ; name message. The name dsp is reserved: 1 or start turns audio on, 0 or stop off. Without a name it sends nothing.

| Argument | |
| --- | --- |
| `name` | the name the receive objects listen to (required) |

| Message | |
| --- | --- |
| `anything` | sent to every receive name |

| Inlet | |
| --- | --- |
| 1 | message to every receive with this name |

See also: `receive` `send~` `message` `print`

### `receive` `r`

`receive name`

Outputs every message sent to its name, by send objects or by message boxes (; name message), from any open patch.

| Argument | |
| --- | --- |
| `name` | the name it listens to (required) |

| Outlet | |
| --- | --- |
| 1 | messages sent to this name |

See also: `send` `receive~` `message` `loadbang`

### `print`

`print [name]`

Writes every message it gets to the console (Patch > Console), as name: message. Give each print its own name to tell the lines apart.

| Argument | |
| --- | --- |
| `name` | the label at the start of each line (print) |

| Message | |
| --- | --- |
| `anything` | printed to the console |

| Inlet | |
| --- | --- |
| 1 | anything: shown in the console (Patch > Console) |

See also: `send` `message` `trigger`

## Patching

### `p`

`p [name]`

A subpatcher: a patch inside a box, saved inside the parent's file. Its ports are the inlet, inlet~, outlet and outlet~ boxes inside; double-click it in locked mode (Cmd+double-click in edit mode) to open it. The name is only a label: two p boxes with the same name are separate patches. A patch saved as name.moritz and typed as name is an abstraction, found next to the patch, then in the libraries, then among the bundled examples; its arguments replace #1 to #9 inside it, #0 becomes a number unique to each copy (for local send, receive and buffer~ names), and saving from its window writes its file.

| Argument | |
| --- | --- |
| `name` | a label (none) |

| Message | |
| --- | --- |
| `anything` | goes out of the inlet box inside that matches the inlet; a number in a signal inlet becomes a constant signal |

See also: `inlet` `outlet` `inlet~` `outlet~` `poly~` `pfft~`

### `inlet` `in`

`inlet [number]`

An inlet of the p, abstraction or poly~ voice it is in: each one adds a control inlet to the box outside, and what arrives there leaves its outlet. Ports are numbered left to right by x position; with a number, as Max's poly~ voices write them (in 1), they are ordered by that number, before the unnumbered ones.

| Argument | |
| --- | --- |
| `number` | the port's position, from 1 (by x position) |

| Outlet | |
| --- | --- |
| 1 | what arrives at this inlet of the subpatcher |

See also: `inlet~` `outlet` `p` `poly~`

### `inlet~` `in~`

`inlet~ [number]`

A signal inlet of the p, abstraction or poly~ voice it is in: it outputs the signal arriving at that inlet of the box outside. A number sent to the inlet outside becomes a constant signal; other messages are ignored. Ports are numbered left to right by x position, or by their number (in~ 1), numbered ones first.

| Argument | |
| --- | --- |
| `number` | the port's position, from 1 (by x position) |

| Outlet | |
| --- | --- |
| 1 | signal arriving at this inlet of the subpatcher |

See also: `inlet` `outlet~` `p` `poly~` `pfft~`

### `outlet` `out`

`outlet [number]`

An outlet of the p, abstraction or poly~ voice it is in: what arrives leaves through that outlet of the box outside. Ports are numbered left to right by x position, or by their number (out 2), numbered ones first.

| Argument | |
| --- | --- |
| `number` | the port's position, from 1 (by x position) |

| Inlet | |
| --- | --- |
| 1 | leaves the subpatcher through this outlet |

See also: `outlet~` `inlet` `p` `poly~`

### `outlet~` `out~`

`outlet~ [number]`

A signal outlet of the p, abstraction or poly~ voice it is in: its signal leaves through that outlet of the box outside; in a poly~ the voices' signals are summed. Ports are numbered left to right by x position, or by their number (out~ 1), numbered ones first.

| Argument | |
| --- | --- |
| `number` | the port's position, from 1 (by x position) |

| Inlet | |
| --- | --- |
| 1 | signal leaving the subpatcher through this outlet |

See also: `outlet` `inlet~` `p` `poly~`

### `poly~`

`poly~ name [voices] [args...]`

Copies of the abstraction name.moritz (found as any abstraction: next to the patch, in the libraries, among the bundled examples), with voice allocation. Its ports are the in, in~, out and out~ boxes of the voice; the voices' signal outlets are summed. The args replace #1 to #9 in each voice, and #0 is unique to each voice. A voice is busy from its note until the signal into its thispoly~ returns to zero; double-click the box (locked) to open the first voice.

| Argument | |
| --- | --- |
| `name` | the abstraction, name.moritz (required) |
| `voices` | the number of copies, 1 to 128 (1) |
| `args` | the abstraction's arguments, #1 to #9 inside each voice |

| Message | |
| --- | --- |
| `note ...` | sends the rest to a free voice (the one idle longest), or steals the oldest note when all are busy |
| `midinote pitch velocity` | a velocity above 0 sends "pitch velocity" to the voice already playing that pitch, or to a free one; a velocity of 0 goes to the voice playing that pitch, so note-offs reach their note-on's voice |
| `target n` | later messages go to voice n only (0 = all voices) |
| `mute n [1/0]` | stops (1, the default) or restarts voice n (0 = all); a muted voice gets no new notes |
| `anything` | goes to the target voice, or to every voice; a number in a signal inlet becomes a constant signal |

See also: `thispoly~` `p` `inlet~` `outlet~` `kslider` `makenote`

### `thispoly~`

`thispoly~`

Inside a poly~ voice: outputs the voice number, from 1, when the patch opens and on bang (0 outside a poly~). Connect the voice's amplitude envelope to its inlet: the voice stays busy while that signal is not zero, and becomes free when it returns to zero. Without it, a voice taken by a note stays busy, and later notes steal the oldest.

| Message | |
| --- | --- |
| `bang` | outputs the voice number |
| `number` | sets the voice busy (not 0) or free (0) |
| `mute [1/0]` | mutes (1, the default) or unmutes the voice |

| Inlet | |
| --- | --- |
| 1 | voice envelope: busy while it is not 0; a number sets busy; mute 1/0 |

| Outlet | |
| --- | --- |
| 1 | voice number (from 1) |

See also: `poly~` `line~` `inlet~`

## Interface

### `message`

`message [contents]`

Sends its contents when clicked (locked) or when anything arrives. In the contents, $1..$9 are replaced by the values that arrive ($n with no value becomes 0), a comma separates messages sent one after the other, and after a semicolon the first word names a receive and the rest is sent there: "; freq 440; amp 0.2". On screen a new line starts after each semicolon; when editing, Return finishes and Shift+Return adds a line break (a space).

| Argument | |
| --- | --- |
| `contents` | the words, numbers, $1..$9, commas and semicolons to send (empty) |

| Message | |
| --- | --- |
| `bang` | sends the contents ($1..$9 become 0) |
| `anything` | sends the contents, with $1..$9 taken from it |
| `set ...` | replaces the contents without sending (set alone empties the box) |

| Inlet | |
| --- | --- |
| 1 | bang sends it; $1..$9 take the incoming values; set ... changes it |

| Outlet | |
| --- | --- |
| 1 | the message |

See also: `button` `receive` `send` `prepend` `trigger`

### `flonum`

`flonum [value]`

A number box that keeps decimals. In locked mode, drag up or down to change it (left of the point by 1, right of it by 0.01), or click, type a value and press Enter. The value is saved with the patch; it is not sent when the patch opens.

| Argument | |
| --- | --- |
| `value` | the starting value (0) |

| Message | |
| --- | --- |
| `number` | shows and sends it |
| `bang` | sends the value again |
| `set x` | shows x without sending it |

| Inlet | |
| --- | --- |
| 1 | number: show and output; set x: show only |

| Outlet | |
| --- | --- |
| 1 | the number |

See also: `number` `slider` `float` `message`

### `number`

`number [value]`

A number box that keeps whole numbers: what arrives is truncated toward zero (3.7 becomes 3, -2.9 becomes -2). In locked mode, drag up or down to change it by 1, or click, type a value and press Enter. The value is saved with the patch; it is not sent when the patch opens.

| Argument | |
| --- | --- |
| `value` | the starting value (0) |

| Message | |
| --- | --- |
| `number` | shows and sends it, truncated |
| `bang` | sends the value again |
| `set x` | shows x without sending it |

| Inlet | |
| --- | --- |
| 1 | number: show and output; set x: show only |

| Outlet | |
| --- | --- |
| 1 | the number |

See also: `flonum` `slider` `int` `message`

### `button`

`button`

Flashes and sends bang when clicked (locked) or when any message arrives.

| Message | |
| --- | --- |
| `anything` | flashes and sends bang |

| Inlet | |
| --- | --- |
| 1 | anything: flash and bang |

| Outlet | |
| --- | --- |
| 1 | bang |

See also: `toggle` `message` `metro` `trigger`

### `toggle`

`toggle`

An on/off switch: a click (locked) or a bang flips it and sends 1 or 0. Its state is not saved with the patch.

| Message | |
| --- | --- |
| `bang` | flips it and sends 1 or 0 |
| `number` | sets it (anything but 0 is on) and sends 1 or 0 |
| `set x` | sets it without sending |

| Inlet | |
| --- | --- |
| 1 | number: 1/0 and output; bang: flip |

| Outlet | |
| --- | --- |
| 1 | 1 or 0 |

See also: `button` `gate` `metro` `slider`

### `link`

`link [text] target`

Underlined text; a click (locked) or a bang opens the target with the system: a document such as a PDF in its viewer, a folder, or a web page (an address with :// or mailto:). A relative path is looked for next to the patch, then among the bundled examples and documents, so docs/... works anywhere. If the target can't be opened, the console says so.

| Argument | |
| --- | --- |
| `text` | what the box shows (the target) |
| `target` | the file, folder or address to open |

| Message | |
| --- | --- |
| `bang` | opens the target |
| `set target` | changes the target, without opening it |

| Inlet | |
| --- | --- |
| 1 | bang: open the target; set target: change it |

See also: `comment` `message`

### `slider`

`slider [min max]`

A horizontal slider; click or drag it in locked mode. It outputs whole numbers when both limits are written as integers (slider 0 127), and decimals when one is written with a point (slider 0. 1.). Values are clipped to the range; dragging sends only changes. Its value is not saved with the patch.

| Argument | |
| --- | --- |
| `min` | the value at the left end (0) |
| `max` | the value at the right end (127) |

| Message | |
| --- | --- |
| `number` | moves it and sends the value (always, even unchanged) |
| `bang` | sends the value again |
| `set x` | moves it without sending |

| Inlet | |
| --- | --- |
| 1 | number: set and output; set x: set only |

| Outlet | |
| --- | --- |
| 1 | value |

See also: `flonum` `number` `scale` `line`

### `comment`

`comment text`

A text note in the patch, with no inlets or outlets. In edit mode press c and type, or double-click a comment to change it; the text is kept as typed, commas, semicolons and $1 included, and a new comment left empty is removed.

| Argument | |
| --- | --- |
| `text` | the note |

See also: `message` `link`

### `kslider`

`kslider [octaves] [lowest] [mode]`

A piano keyboard from the lowest note up, octaves times 12 keys plus one. Clicking a key (locked) sends its velocity, then its pitch; the velocity depends on where the key is clicked, louder lower down. Mode 0 plays while the mouse is down, sends velocity 0 on release, and glides from key to key when dragged; mode 1 toggles keys on and off, for chords.

| Argument | |
| --- | --- |
| `octaves` | number of octaves, 1 to 8 (4) |
| `lowest` | the MIDI note of the lowest key (36), kept so the keyboard ends at or below 127 |
| `mode` | 0 play, 1 toggle (0) |

| Message | |
| --- | --- |
| `pitch [velocity]` | lights the key and passes the note through, with the velocity from the right inlet (100) when none is given; velocity 0 releases it |
| `set pitch [velocity]` | lights the key (velocity 0 unlights it) without output |
| `flush` | releases every held key, sending its note-off |
| `clear` | unlights every key without output |

| Inlet | |
| --- | --- |
| 1 | pitch (or pitch velocity): lights the key and passes through; set p v; flush; clear |
| 2 | velocity for pitches sent to the left inlet |

| Outlet | |
| --- | --- |
| 1 | pitch |
| 2 | velocity (0 = note-off) |

See also: `makenote` `noteout` `poly~` `mtof` `stripnote`

### `function`

`function [domain] [lo] [hi] [x y ...]`

A breakpoint editor. In locked mode, click to add a point, drag to move it, Shift-click to delete it; the first and last points stay at 0 and at the domain. bang outputs the curve for line~ (or line) from the right outlet: the first value, then a list of value-time pairs. The points are saved with the patch, after the range.

| Argument | |
| --- | --- |
| `domain` | the length of the curve, in milliseconds (1000; at least 1) |
| `lo` | the bottom of the vertical range (0) |
| `hi` | the top of the vertical range (1) |
| `x y ...` | the points, time (ms) and value; without them the curve is flat at lo |

| Message | |
| --- | --- |
| `bang` | outputs the curve: the first value, then value-time pairs |
| `number` | outputs the value of the curve at that time (ms), from the left outlet |
| `domain ms` | changes the length, stretching the points in time |
| `range lo hi` | changes the vertical range; points outside it are clipped |
| `clear` | resets the curve: two points, flat at lo |
| `set x y ...` | replaces the points |

| Inlet | |
| --- | --- |
| 1 | bang: the curve for line~; a number (ms): the value there; domain, range, clear, set |

| Outlet | |
| --- | --- |
| 1 | value at the time received |
| 2 | the curve for line~: first value, then value-time pairs |

See also: `line~` `line` `plot`

### `plot`

`plot [buffer] [width height] [mode]`

Curves, bars, scatter plots and matrices (heat maps). It shows a buffer~, followed as it changes (its channels are rows, its sample rate gives a time axis in seconds; a rate of 1 gives a plain matrix), or the data it receives: a list is a curve (x y pairs in scatter mode; in image mode a new column of a scrolling image), a number scrolls a curve. A click (locked) outputs x (ms on a time axis, otherwise the index) and y (the row of an image, the value of a curve); double-click, or right-click > Open window, opens a large window where the wheel zooms (Shift: vertically), dragging moves, r resets, e exports a PNG and the values under the mouse are shown. Right-click > Export PNG... saves an image.

| Argument | |
| --- | --- |
| `buffer` | a buffer~ to show (none) |
| `width height` | the size of the box in pixels (300 140) |
| `mode` | auto, line, bars, scatter or image (auto: curves for up to 4 rows, an image above) |

| Message | |
| --- | --- |
| `buffer name [channel]` | shows that buffer~, or only one channel (from 0) |
| `number` | adds a point to a scrolling curve (history points) |
| `list` | one curve (or bars), x y pairs in scatter mode, a new column in image mode; also set ... or list ... |
| `mode m` | auto, line, bars, scatter (rows 0 and 1 are x and y, row 2 the colour) or image (row 0 at the bottom) |
| `range lo hi` | fixes the values shown (y of a curve, colours of an image); range auto (or any other word) returns to automatic |
| `xrange from to` | fixes the part shown on the x axis (seconds on a time axis); xrange auto returns to all |
| `rows first last` | what the first and last rows of an image stand for on the y axis (rows 30 300 for bpm); rows alone turns it off |
| `normalize 1/0` | colours each row of an image over its own range (1) |
| `ylog 1/0` | a logarithmic y axis for curves and scatter plots (1) |
| `marks t ...` | vertical lines, ms on a time axis; marks alone removes them; also the right inlet |
| `cursor ms` | a vertical cursor; a negative value, or none, hides it |
| `title text` | a title over the plot and the window |
| `labels a b c ...` | names of the rows of an image or of the bars; labels chroma (C C# ... B), labels keys (C Cm Db C#m ... Bm, as keyfind orders them) |
| `colors viridis\|gray\|types` | the colours of an image; types gives each value its own colour, for categories such as sound types |
| `history n` | the points of a scrolling curve, the columns of a scrolling image (256) |
| `clear` | removes the data, the buffer~, the marks and the cursor |
| `open` | opens the window |
| `export file.png [w h]` | writes the plot as a PNG, relative to the patch (900 500) |

| Inlet | |
| --- | --- |
| 1 | buffer name [channel], a list, a number; mode m, range lo hi, xrange from to, rows first last, normalize 0/1, ylog 1/0, marks, cursor ms, title, labels, colors, history n, clear, open, export file.png |
| 2 | marks: times (ms on a time axis) drawn as vertical lines |

| Outlet | |
| --- | --- |
| 1 | x clicked (ms on a time axis) |
| 2 | y clicked: the row of an image, the value of a curve |

See also: `buffer~` `waveform~` `soundtypes` `melbands~` `filterplot` `spectroscope~`

## Code

### `code`

`code [inlets] [outlets] [file.mu] [@trusted]`

A script in Musil inside the patch, as js is in Max. It runs off the audio thread when it gets a bang, in an interpreter of its own that keeps its variables from one run to the next (with the std and signals libraries loaded; load "music.mu" or "scientific.mu" for the others), and the last outlet bangs when it has run. Its inlets set the variables in1, in2... (a number, a vector for a list of numbers, a string, or a list; nil until a value comes): a message to the left inlet sets in1 and runs the script, the other inlets only store. In the script, (out k values...) sends out of outlet k; (buffer name) reads a buffer~ (a vector per channel), (buffer-rate name) its rate, (set-buffer! name x [rate]) writes one, and (send name values...) reaches the receive boxes of that name.

The script is kept in the patch, or in the .mu file the box names (read at every run, next to the patch). Double-click the box (Cmd-double-click when editing) to open it in the code editor: Cmd-S puts it back into the box, Cmd-Enter runs a block in the box's interpreter. What it prints goes to the Console. A code box reads files anywhere and writes them only in its patch's folder (a relative path is there) and in ~/Documents/Moritz; it does not run programs, use the network, wait or quit (exec, udp-send, serve, sleep, exit ... stop with "not allowed here"). With @trusted, its first run asks whether the patches in that folder may do everything; the answer is remembered. The Console and moritz --run have all of Musil.

| Argument | |
| --- | --- |
| `inlets` | 1 to 16 (1) |
| `outlets` | 0 to 16, besides the last one, which bangs when the script has run (1) |
| `file.mu` | a script in a file instead of the patch (none) |
| `@trusted` | asks, once per folder, to let the script do everything (write anywhere, run programs, the network) |

| Message | |
| --- | --- |
| `bang` | runs the script |
| `anything` | to the left inlet, becomes in1 and runs the script; to inlet k, becomes ink |
| `set name values` | sets a variable without running |
| `stop` | stops the script running |
| `reset` | a fresh interpreter: the variables of earlier runs are gone |
| `open` | opens the script in the code editor |
| `read file.mu` | runs that file from now on |

| Inlet | |
| --- | --- |
| 1 | bang runs the script; anything else becomes in1 and runs it; set name values, stop, reset, open, read file.mu |

| Outlet | |
| --- | --- |
| 1 | what the script sends with (out k ...), k from 1; the last outlet: bang when the script has run |
| 2 | what the script sends with (out k ...), k from 1; the last outlet: bang when the script has run |

See also: `buffer~` `send` `receive` `print`

## Library

### `binpan~`

`binpan~ [azimuth] [elevation]`

A mono sound placed around the listener, for headphones. Inlets: the signal, the azimuth in degrees (0 in front, 90 left, -90 right, 180 behind), the elevation in degrees (up). Outlets: left and right. An order-2 ambisonic encoder (ambienc~ 2) into binaural~ 2: each binpan~ has its own binaural~, so for many sources sum their ambisonic fields and decode them with a single binaural~.

| Argument | |
| --- | --- |
| `azimuth` | the starting azimuth, in degrees (0) |
| `elevation` | the starting elevation, in degrees (0) |

See also: `ambienc~` `binaural~` `hrtf~` `ambirot~`

### `fvcomb~`

`fvcomb~ ms name`

One Freeverb comb filter, used by reverb~: a feedback comb with a one-pole low-pass in the loop (damping), as in Jezar's Freeverb. One signal inlet, one signal outlet. Its decay time T60 (seconds) arrives on the receive name `name-t60`; the feedback g = 10^(-3 d / T60) for its delay d, ramped over 50 ms, so every comb dies away in the same time.

| Argument | |
| --- | --- |
| `ms` | the delay, in milliseconds (100 at most) |
| `name` | the prefix of the receive that sets the decay: name-t60 |

See also: `reverb~` `comb~` `tapin~` `tapout~` `biquad~`

### `impose-fx`

`impose-fx`

The spectral patch of impose~, run by its pfft~ once a frame: the carrier's amplitudes divided by their envelope (its fine structure) times the modulator's envelope, with the carrier's phases. fftin~ 1 is the modulator, fftin~ 2 the carrier, and the control inlet the envelope order of both specenv~. Internal: use impose~.

See also: `impose~` `pfft~` `specenv~` `cartopol~`

### `impose~`

`impose~ [order]`

The spectral envelope of one sound imposed on another (Musil's impose-envelope): the carrier speaking with the modulator's formants, an orchestra or a synth that speaks with a voice. Inlets: the modulator (whose colour is taken), the carrier (what sounds), the envelope order. One signal outlet. Built from specenv~ in a pfft~ of 1024 points, overlap 4 (impose-fx): the carrier's fine structure times the modulator's envelope.

| Argument | |
| --- | --- |
| `order` | the envelope order, its resolution: about sr / (2 f0) of the modulator (0 or none: 60) |

See also: `specenv~` `pfft~` `sparkle~` `pitchshift~` `impose-fx`

### `pitchshift~`

`pitchshift~ [semitones]`

A live pitch shifter. Inlets: the signal, the transposition in semitones (decimals allowed), formant preservation 1/0 (on at the start: a voice keeps its timbre; 0: the formants move with the pitch). One signal outlet, a window (2048 samples) late. It is sparkle~ without a buffer~, its pitch ratio 2^(semitones / 12), with envelope 60 for the formants.

| Argument | |
| --- | --- |
| `semitones` | the starting transposition, in semitones (0) |

See also: `sparkle~` `impose~` `specenv~` `pfft~`

### `reverb~`

`reverb~ [mix] [T60]`

A stereo Freeverb. Inlets: the signal, the wet/dry mix (0 dry to 1 wet, a 20 ms ramp) and the decay time T60 in seconds (0.1 to 100). Outlets: left and right. Per channel, eight damped combs (fvcomb~) in parallel, then four allpass~ (gain 0.5) in series; the right channel's delays are 23 samples longer (at 44.1 kHz), so the two outputs are decorrelated. Each comb's feedback is g = 10^(-3 d / T60) for its delay d in seconds, so the tail falls by 60 dB in T60 seconds, the highs a little faster (damping).

| Argument | |
| --- | --- |
| `mix` | the wet/dry mix, 0 to 1 (0 or none: 0.3) |
| `T60` | the decay time, in seconds, 0.1 to 100 (0 or none: 2) |

See also: `fvcomb~` `allpass~` `comb~` `tapin~` `conv~`
