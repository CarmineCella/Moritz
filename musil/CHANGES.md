# Changes

## 0.10c

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
