# Changes

## 0.11

### Language

- **The libraries' names are their own.** A library (a file found on the load path) runs in an
  environment that mirrors the program's: its functions see its names, so a program's
  `(var sum 0)` or its own `function length` no longer breaks `mean` or `median`. A `set` on a
  library variable still changes it for the library. (Before, a program could redefine a library
  name for the library's functions too; now it does so for itself only.)
- **load** searches next to the loading file, then `MUSIL_PATH` and `~/.musil`, then the current
  directory (the current directory came second), and runs a module again when the file changed
  since it was loaded.
- **expr**: `&&` and `||` short-circuit, as `and` and `or` do.
- **Closures that hold their own scope are freed**: a cycle collector frees the object pattern
  (`put! self 'get (function () self)`) and inner closures kept in an outer scope.
- **Faster control flow**: `return`, `break` and `continue` are no longer C++ exceptions (a
  `continue` costs under a microsecond instead of about 13), and a `(function ...)` form prepares
  its body once.
- **live**: a `set-param` with a time lands on its very sample, `voice-set` glides in 5 ms whatever
  the block size, and the noise node draws the same numbers on every system.

### Libraries

- **Random numbers** are the same on every system for a seed: `rand`, `noise`, SoundTypes and
  Orchidea draw from the raw generator (`src/rng.h`); `knn` breaks ties the same everywhere.
- **Orchidea** keeps its best evaluated population and an elite of every generation (the best
  solution found could be lost).
- **`resample`** anti-aliases properly when downsampling (a Kaiser lowpass at 0.93 of the new
  Nyquist, as wide as the factor needs: aliases under -65 dB); upsampling is unchanged.
- **`autocorr`** by FFT (0.5 s to 3 ms for a second of audio).
- **`conv`** is C++ (overlap-add over real FFTs, a streaming node as before); `conv-fast` is the
  same function. `binaural-decode` is six times faster.
- **`deconv`** (and Moritz's `condec`) regularises the inversion (Tikhonov:
  `Y conj(H) / (|H|^2 + threshold^2 max|H|^2)`) instead of cutting the spectrum where `|H|` is
  under the threshold; `threshold` 0 is the plain inverse.
- **`stft`** pads its last frame, so `istft` gives back the whole input (it was shorter);
  **`moving-source`** crossfades its blocks with `hann-fade` (no clicks where the direction changes).
- **`pvoc`** (and `sparkle~`): the sound is padded with a window at both ends, so an attack at the
  very start and the ending come out whole; a pitch shift moves each partial's region whole and
  keeps the level (it was +1.7 dB an octave up, -4.2 dB an octave down).
- **`nebula`** (and `nebula~`): about three times cheaper with many grains; triangle and square
  are band-limited per octave, so high grains do not alias.
- The recursive loops (`iir`, `comb`, `allpass`, `lag`, `adsr`, `svf`, `conv~`'s engine) flush
  denormals to zero.
- **Ambisonic decoding**: a regular ring of at least 2N+1 speakers keeps the exact decoder; any
  other ring (5.0, too few speakers) and any layout with elevations (`binaural-decode`'s 17
  directions) get the energy-preserving decoder (EPAD): every direction about as loud.
- **`conv~`'s engine** spreads its work over the callbacks of a block (the worst callback with a
  10 s response: 145 to 44 microseconds).
- **Maple**: the sound pursuit is 2.7 times faster (cached spectra), the books the same.
- **One implementation of the analyses** (`src/signals/mir.h`): `chromagram`, `mel-spectrogram`,
  `mfcc-gram`, `self-similarity`, `foote-novelty`, `tempogram`, `beat-track`, `key-scores` and
  `tempo-estimate-with` give Moritz's numbers; keys are named as musicians spell them (E♭ major, F♯ minor).
- Headers for Moritz: the FFT has a real-input transform (`RealFFT`), and the spectral envelope
  (`envelope.h`, `spectral-envelope` a builtin on it, the order advice unified: about sr / (2 f0)),
  HPSS (`hpss.h`) and the RBJ biquads (`biquad.h`, `biquad` a builtin on it) have one header each.

## 0.10c

Musil 0.10c is 0.10b made safer: the same programs give the same results, and what used to crash
the interpreter (or let a script do more than its host allows) is now an error.

### Language and runtime

- **The evaluation port asks for a token.** The first line of a message is the port's token, written
  when the port opens to `~/.musil/port-<port>.token` (or to a folder of the user's in the
  temporary directory), readable by the user alone; `musil --send` reads it (or takes `--token`).
  Code that arrives while a program runs waits until the program is idle or in `sleep`, and no
  longer resets the evaluation it interrupts (sending code during a loop could crash).
- **No more crashes on deep or huge input**: text nested too deep for the reader (`parse`), lists
  nested too deep for `equal?`, a vector too large to make (`(range 1e12)`, `(range 0 1 1e-300)`)
  and any C++ exception inside a builtin are Musil errors that `try` catches. The stack limit is
  the one of the thread the interpreter really runs on (an interpreter on a thread of 512 KB used
  to overflow).
- **Literals stay literals**: a variable bound to a number literal gets its own copy, and a quoted
  list is a fresh list each time, so `(var x 0)` then `setidx`, or `(var L '())` then `push`, inside
  a function no longer change what the next call starts from. The mutators refuse a literal reached
  otherwise (`cannot change a constant of the program text`). Lists and vectors bound by `var` to
  another variable are still shared, as documented.
- **Errors after a call** report the line of the code that called, not the last line run in the
  function called.
- **Stop** (`Interp::request_stop`, for hosts; Ctrl-C in `musil`): an interrupt that `try` cannot
  catch; it ends `sleep` and `udp-receive` at once.
- **Capabilities** (`Interp::caps`, for hosts): what a program may do (read and write files, under
  which folders; run programs; the network; wait; exit) is checked inside every builtin that
  touches the system. `read` refuses what is not a regular file (`/dev/zero`), and files over 1 GB.
- **Several interpreters at once**: the evaluation port, the loops, the idle hooks and the OSC input
  belong to their interpreter; the controls, the playhead and the audio device are shared under
  locks.
- **The live engine's audio thread** neither allocates nor frees: buffers are made when a synth is
  compiled, what the audio thread lets go of is freed by another thread, the binaural node's head
  responses are computed when the synth is compiled. The sound is the same.

### Libraries

- **Linear algebra**: `det`, `rank`, `inv`, `solve` and `eig-sym` use tolerances relative to the
  matrix's scale: a tiny (or huge) matrix is no longer called singular or diagonal for its size.
- **`mp`**: plain matching pursuit may take an atom again (listed once, its weights summed) and
  orthogonal pursuit takes each atom once (it was the other way round); with `'nonneg` the
  orthogonal refit is a true non-negative least squares (Lawson-Hanson), not clamped weights.
- **`midi-read`** checks every read (a truncated or corrupt file gives the notes before the
  damage instead of reading past the data) and keeps overlapping notes of the same pitch.
- Interpreters on several threads (as in Moritz) share the HRTF set, its responses at other rates,
  the sample-folder index and `resample`'s table safely.
- `svf`'s band output is documented as it is: its peak gain is `1/(2 - 1.98 q)` (up to +34 dB at
  q 1), as `svf~`'s.

## 0.10b

Musil is the reference of Moritz's signal processing: where Moritz has an object for what a Musil
function does, the object runs Musil's code. The engines are in `src/signals/` (and
`src/scientific/nmf.h`), self-contained C++ that Moritz builds into its objects.

- **The phase vocoder is one engine** (`src/signals/vocoder.h`), `pvoc`'s and `sparkle~`'s.
  Each frame's frequencies are measured against the frame one analysis hop before it, so a
  real-time player may freeze, jump and play backwards; the rest is `pvoc` as it was (the envelope
  that rides the harmonic peaks, every bin locked to a peak). Whisper's random phases are drawn
  the same way on every system.
- **`nebula`**: granular synthesis (`src/signals/nebula.h`, the engine of `nebula~`): clouds of
  grains of two prototypes, waveforms or sounds, with density, length, frequency, glissando and
  pan, spread at random and able to ramp; a seed gives the same cloud as `nebula~`. Example
  `nebula.mu`.
- **`svf`**: the state-variable filter of `svf~` (`src/signals/filters.h`), low, high, band or
  notch, with a frequency and a resonance that may move; it streams in a synth. **`lores`**: the
  resonant lowpass of `lores~`. `filters.mu` uses both.
- **`deconv`**: deconvolution (`src/signals/convolution.h`, with the partitioned convolution of
  `conv~` and the whole-buffer convolution of `condec`).
- **`nmf` and `nmf-with-parts` are C++** (`src/scientific/nmf.h`, the factorisation of Moritz's
  `nmf` box), about five times faster than the Musil they were; `nmf-separate` and the supervised
  separation are faster with them. The starting point is drawn the same way on every system.
- **One build with Moritz**: Moritz's CMake build includes Musil's (`musil/CMakeLists.txt`), and
  both fetch the same FLTK, 1.4.4 (`cmake/fltk.cmake`); Musil still builds on its own from
  `musil/`.
- **The ambisonic harmonics and decoders** (`src/signals/ambisonics.h`) and the reader of
  head-response sets (`src/signals/hrtf.h`) are shared with `ambienc~`, `ambidec~`, `binaural~`
  and `hrtf~`; `src/signals/fft.h` is the FFT of all the engines and of `mir.h`.

## 0.10

Musil now lives in Moritz's repository (`musil/`), as the scripting language of Moritz, and is
released with it. It stays independent: nothing in `musil/` uses Moritz (`tools/check-musil.sh`).

- **Maple and SoundTypes in the music library.** Matching pursuit: dictionaries of Gabor or
  gammatone atoms or of pieces of sounds (`maple-gabor`, `maple-gammatone`, `maple-cut`,
  `maple-folder`, `maple-merge`, `maple-save`, `maple-load`), decomposition into a book
  (`maple-decompose`, `maple-book`, `maple-residual`, `maple-snr`), resynthesis, transformation
  and cross synthesis (`maple-synth`, `maple-hybrid`). Sound types: analysis into types and
  levels (`soundtypes-analyse`, `soundtypes-analyse-book`, `soundtypes-levels`,
  `soundtypes-units`, `soundtypes-labels`, `soundtypes-transitions`), generation by factor
  oracle, variable-order Markov model or n-grams (`soundtypes-generate`), rematch
  (`soundtypes-match`). Their C++ is `src/music/maple.h` and `src/music/soundtypes.h`, built
  on `src/signals/mir.h` (spectrograms, mel bands, MFCCs, chroma, onsets, tempo, beats, keys,
  novelty, loudness). Moritz's analysis, atoms, pursuit and sound-types boxes run the same code.
  Examples `maple.mu` and `soundtypes.mu`; tests `test_atoms.mu`.
- **`kmeans` starts with k-means++ and keeps the best of ten starts** (`kmeans_pp` in
  `src/scientific/algorithms.h`, shared with Moritz's kmeans box): it used to start from the
  first k points, and could merge two groups. The result is the same on every system, and the
  clusters are numbered in the order the points first meet them.
- **The CMake build** has no IDE target (the IDE stays in Musil's own repository), and installs the
  headers of `src/music` and `src/signals` too.
- **The command line finds its libraries next to it**, in `src/`, after `MUSIL_PATH` and
  before `~/.musil`: a `musil` shipped with its libraries runs as it is.
- **`src/help.txt` is committed**, so a build without Python has the help.
- **The data of the examples and tests** is the folder `data/` at the root of Moritz's tree,
  shared with Moritz's patches. `classic_mir.mu` analyses `beachboys.wav`.
- **`test_music.mu` runs in a host without the live or plot library** (as Moritz hosts
  Musil): the checks of playback and of the roll's drawing run when those libraries are there.
- **The IDE** stays in Musil's own repository (archived at 0.9.1); Moritz's code editor and
  Console do its work.
