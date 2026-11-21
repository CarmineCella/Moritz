# Changes

## 0.13

### The score

- **Transformations of a selection are C++** (`src/music/score_transform.h`): pure functions on the
  score, which Moritz's roll and score object call too, so a transformation gives the same score
  everywhere. `score-transpose!`, `score-invert!` (around a pitch, a number or a name; twice gives
  the score back), `score-retrograde!` (within the span of the events), `score-augment!` and
  `score-diminish!` (from the first onset), `score-quantize-grid!` (onsets and ends toward a grid in
  seconds, by a strength), `score-humanize!`, `score-thin!` and `score-density!` (seeded through the
  portable random module: the same seed, the same result on every system). Each takes an optional
  list of ids or events (none: every event). A Musil note transposed or inverted has its sound
  shifted by as much.
- `balanceGains` (`src/music/score.h`, beside `panGains`): a source with a channel for each output
  played at an azimuth; on two channels a constant-power balance, so that Moritz's score object pans a
  stereo sound by its clip's and its track's azimuth as it pans a mono one.
- `score-shift!` and `score-scale!` are those functions now (`score-shift!` takes ids too); the
  results are the same.
- **The roll's layout is C++** (`src/music/score_layout.h`): the orchestral order of the rows, the
  clef of a row from its pitches, the steps of the staff. `instrument-rank`, `staff-clef` and
  `score-orchestral-order` are builtins; `score-roll` and the plot's roll draw with them, as Moritz's
  roll does.

### Orchestration

- **`'sustain` holds notes with the `'path` connection.** The path is chosen once every segment is
  searched, so a player cannot be fixed during the search as `'closest` and `'best` do; instead each
  solution of a segment offers its continuation into the next (that segment's best with the players
  whose pitch the target keeps holding their very sound), ranked among the solutions by its own cost.
  A held note moves by no semitone, so the path takes the continuation where holding costs little,
  and the connection merges the held notes into longer ones. Musil's dynamic settings
  (`dynamic_orchidea.mu`, Moritz's orchidea for a segmented target) hold notes again.
- **The report's `'held` says what the connection holds**: the notes it merges into longer ones (the
  same player, instrument and pitch across a boundary, within `'hold`), for every connection; it said
  0 with the path.
- **No two solutions of a segment sound the same**: two players of an instrument with their sounds
  swapped were two solutions (choosing the second sounded like the first); they are one now, so the
  solutions kept are that many different sounds.
- **A segment no sound passes the filters for is a rest** (its partials' pitches, the styles, the
  dynamics): one silent solution, the reason in its report (`'rest`, which `mimetic-report` prints),
  and the other segments are orchestrated; the whole run failed with "empty search space" before
  (MicroSOL with `'partials 0.3`).

## 0.12

### The score

- **A score is one C++ structure** (`src/music/score.h`), independent of the interpreter and of
  Moritz, which plays the same structure: notes (a MIDI pitch, fractional for microtones, and cents;
  dynamics, technique, a row), sounds (a region of a file or of a named buffer), patches, messages
  and other scores placed inside it (by file or embedded; a placement stretches, transposes, scales
  the gain and turns the direction). Rows, a tempo and a meter, ids that are never reused, a time
  index for range queries. A score cannot contain itself, directly or through others: it is refused
  when it would.
- **Musil uses it as a value**: `(type s)` is `"opaque:score"`, an event of it `"opaque:event"`, and
  both answer as records (`get`, `opt`, `has?`, `put!`, `keys`, the new `pairs`), so music.mu's
  functions keep their names and give the same results. What only Musil can hold (a note's
  database, a synth's function, a buffer's vector) stays with the event; what is plain data is kept
  in the structure. `(get s 'events)` makes a new list each time: `event` and `score-add!` add.
- **.score files are Musil data**: `score-write` writes the score as a list of lists, `score-read`
  reads it with Musil's reader and runs nothing (nesting, sizes, numbers and forms are checked: a
  file from elsewhere is an error, never a crash). Times are written in seconds, exactly:
  read, written and read again, a score is the same. `score->data`, `data->score`, `score->text`,
  `text->score`; `score->coll` and `coll->score` (one event a row, for the flat parts).
- New: `score-make`, `score?`, `score-add!`, `score-take-id!`, `score-find`, `score-remove-id!`,
  `score-query`, `score-copy`, `score-placed`, `score-pan-gains`, `score-bbt`, `score-bbt->sec`,
  `score-bind!` (sounds from a database for the notes of a score read from a file). `score-duration`
  and `score-flatten` are C++ now.
- **Placements compose**: a score event's `stretch`, `transpose` and azimuth are applied when the
  score renders and plays (the azimuth of a score placed with `event-at` used to be ignored: its
  events are turned by it now). A sound event may name a region (`from`, `to`). Messages and
  patches (for Moritz) make no sound in Musil.
- `put` (a copy of a record), `keys` and `values` work on anything that answers as a record.
- `same?` is true of two handles of the same C++ object (a score).

### Libraries

- **Orchidea's analysis and search are C++ functions of their own** (`orchidea::analyse` and
  `orchidea::search_segments` in `music.h`), which `orchidea-analyse` and `orchidea-search` wrap: a
  program that embeds Musil's engines (Moritz's `orchidea` object) calls them directly and finds the
  same orchestration for the same seed. So is the reading of a feature file (`db_parse`, under
  `db-read`). The analysis refuses a database whose block is not a power of two (or whose hop or
  number of coefficients is absurd) and a partials window above 2^20, instead of failing later;
  onsets given as a list that are negative are left out.
- **`db-sounds-folder`, `mimetic-seating` and `continuation-plan` are builtins.** `db-load` and
  `orchestrate-mimetic` call the first two as before (they were functions of `music.mu`; the
  `mimetic-seats` table is gone); `merge-continuations-within`, and so every connection of an
  orchestration, is made of `continuation-plan` (a span that starts within 30 ms of the end of one
  of its key continues it, up to the hold).
- **A folder of sounds that cannot be read whole no longer stops the program**: `db-gen`, `db-locate`
  and `db-load` skip a subfolder they may not enter (a system folder in the temporary directory made
  the walk throw: an error in a script, a crash in a program calling the C++ directly), follow no
  link to a folder, and walk 64 levels and a million entries at most.
- **Datasets by name, in one place for Musil and Moritz** (`music/datasets.h`). `(dataset "fullsol")`
  is the feature file of a dataset: a path registered for the name (`dataset-register!`, once, for a
  set that lives elsewhere such as FullSOL), then the datasets folder chosen (`datasets-folder!`), then
  the default folder `~/Documents/Moritz/datasets`, then the MicroSOL that comes with Musil; in a
  folder, `X.spectrum.db` at its top or one folder down, named `x` without a trailing year
  (FullSOL2020 is `fullsol`). `(datasets)` lists what is known and where; `dataset?`,
  `dataset-forget!`, `datasets-folder` and `datasets-config` complete it. The configuration is one
  file in Musil's syntax, `~/.musil/datasets.mu`, read at each use and written atomically; Moritz's
  Datasets window reads and writes the same. A malformed configuration is an error naming its line.
- **`db-load` and `db-load-first` take names**: the examples say
  `(db-load-first (list "fullsol" "tinysol" "microsol"))` instead of relative paths, and run with
  whatever is there.
- **`(dataset-fetch "tinysol")` downloads TinySOL** from Musil's release assets into the datasets
  folder (curl), checks its size and SHA-256, unpacks it in a temporary folder there and moves it into
  place, the feature file last; Ctrl-C stops it and leaves nothing. It uses the network and runs
  programs, so a sandboxed interpreter (a code box in Moritz) is refused; `fetch_tinysol.sh` calls it.
  `(dataset-install name archive)` installs a zip already on disk the same way.
- **The analysis of `db-gen` is a C++ function of its own** (`db_generate`), which Moritz's Datasets
  window and `orchestra` call; it writes the feature file through a temporary file renamed at the end,
  so a stopped or failed analysis leaves nothing half written, and `db-gen` now checks the capability
  profile (reading the folder, writing the file) like every builtin that touches files.

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
  `continue` costs under a microsecond instead of about 13), and a `(function ...)` form of the
  program text prepares its body once (a form built as data, which a program may change, every time).
  A `break`, `continue` or `return` stops at the end of a loaded file: a `break` in a file loaded
  inside a loop no longer ends the loop that loaded it.
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
  directions) get the energy-preserving decoder (EPAD): much more even than before (5.0 at order 3:
  19 dB between the loudest and the quietest direction instead of 27), though a ring with fewer
  than 2N+1 speakers cannot make every direction equally loud.
- **`conv~`'s engine** spreads its work over the callbacks of a block (the worst callback with a
  10 s response: 145 to 44 microseconds).
- **Maple**: the sound pursuit is 2.7 times faster (cached spectra), the books the same.
- **One implementation of the analyses** (`src/signals/mir.h`): `chromagram`, `mel-spectrogram`,
  `mfcc-gram`, `self-similarity`, `foote-novelty`, `tempogram`, `beat-track`, `key-scores` and
  `tempo-estimate-with` give Moritz's numbers; keys are named as musicians spell them (E♭ major, F♯ minor).
  `tempo-estimate-with` reads only lags under half the curve's length, so a short excerpt can give
  another tempo than before.
- **YIN** (`src/signals/pitch.h`, de Cheveigné & Kawahara 2002): `(yin x sr [threshold] [fmin]
  [fmax])` gives the fundamental and the periodicity of a frame, `yin-track` of every frame of a
  sound. The difference function by FFT, its cumulative mean normalisation, the absolute threshold
  (0.1), parabolic interpolation and the best local estimate: a weak or missing fundamental no
  longer reads an octave up (`acf-f0`, kept, does), and the period is found between samples (a
  sine from 55 to 1500 Hz at 44.1 kHz within 0.05 cent). Moritz's `descriptors~` uses it for its f0
  and periodicity.
- **`descriptors`**: the eleven low-level descriptors of `descriptors~` (rms, spectral centroid,
  spread, skewness, kurtosis, flux, hfc, zero crossings, flatness, YIN's periodicity and f0), frame
  by frame. The formulas are C++ in `src/signals/mir.h`, shared with `descriptors~`:
  `spectral-centroid`, `spectral-spread`, `spectral-skewness` and `spectral-kurtosis` read the new
  `spectral-moments`; `spectral-flatness`, `spectral-flux`, `hfc` and `zcr` are builtins on it. Two
  values change: `spectral-flatness` of silence is 0 (it was 1), and `zcr` counts 0 as positive (a
  sine starting at 0 has one crossing fewer).
- Bad sizes are errors: `tempo-estimate-with` with an fps that is not in (0, 1e6], an STFT size
  above 2^24; `resample` bounds its kernel for factors under 1/128; a negative `seed` is the same
  on every system.
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
