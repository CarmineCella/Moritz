# Notes

Technical notes worth keeping: how Moritz is tested, where it departs from the tools it ports, what
was corrected in Musil along the way, what Max objects imported patches miss most, and the sources.
The plan is in [ROADMAP.md](ROADMAP.md); how to work on the code is in [CLAUDE.md](CLAUDE.md).

## Testing

`ctest --test-dir build` runs every test (CLAUDE.md says when and how). What it runs:

- `moritz --render patch seconds [out.wav] [bang=id[@sec]] [send=id:msg[@sec]] [midi=t:bytes]
  [png=file] [extra=patch]` renders a patch offline (and draws it into a PNG).
  `tools/test-patches.sh` renders every example, help patch, library abstraction, tutorial and
  cross-test this way, and fails on any warning; a cross-test (`tests/cross/*.moritz`) compares an
  object with Musil's function for the same thing and must print PASS.
- `tools/test-musil.sh musil|moritz`: Musil's tests, golden references and examples, on Musil alone
  and on Musil in Moritz.
- The engine tests (`tests/engine/`), the interface tests driven as a user would
  (`tools/test-gui.sh`, `act=` actions in `src/guitest.cpp`), the `embed` test (interpreters on
  threads), and the AddressSanitizer and ThreadSanitizer builds (`-DMORITZ_SANITIZE=address|thread`).
- GitHub Actions builds and tests on macOS, Linux and Windows, each on its own system, with both
  sanitizers and Musil built alone, for a `v*` tag (which publishes the release) or when started by
  hand.
- The multitrack engine (`tests/engine/multitrack_tests.cpp`): recording is tested with a simulated
  device (the output fed back to the input a latency later, the take checked to the sample once the
  latency is taken off); streaming with a slow disk (`DiskStream::testDelayMs`) and a locate (misses
  first, then the samples as decoded whole); tracks on several cores against one thread, bit for bit;
  `multitrack_cpu_sixteen_tracks` prints what sixteen tracks cost on one thread and on the pool; the
  threaded paths (the audio thread on its own thread while the interface records, moves faders, adds
  inserts and undoes) under ThreadSanitizer. The interface cases record with a fake device too
  (`act=mtrecord`: audio off, the session run by hand with a sine as its input).

## Max objects most missed in imported patches

`prepend` and `append` exist since 0.8; the others, most often missing first: `zl`, `pak`,
`uzi`, `number~`, `umenu`, `multislider`,
`midiin`/`midiout`/`midiformat`/`midiinfo`, `coll`, `dict`, `change`, `!-`,
`delay~`, `edge~`, `>~`, `scale~`, `delta~`, `fft~`, `vexpr`, `incdec`,
`preset`/`pattrstorage`, `attrui`; the `mc.*` family.

## Departures from Maple and SoundTypes

Changes made while porting, which the original tools could take back:

- **Maple, FFT matching**: atoms were tried at one position per frame (the
  frame start); now every position (parametric atoms: a grid of a quarter of
  the scale, as MPTK; sound atoms: every sample, by cross-correlation).
- **Maple, parametric atoms**: frequencies were a fixed grid (octave
  divisions); now any frequency (the FFT peak refined by a parabola), with
  the optimal phase from an exact projection on cosine and sine (Maple used
  phase slices).
- **Maple, sound atoms**: a mel-spectrum preselection of candidates makes
  large dictionaries usable; an atom starts in its segment but is matched
  and subtracted whole, over what follows (no unexplained tails spilling
  into the next segment). With the TinySOL clarinet in 100 ms frames, Vox is
  rebuilt with an envelope correlation of 0.82 (the original Maple: 0.83).
- **Maple, generation**: the probabilistic generation (per-rank chains,
  weights discarded) is replaced by SoundTypes' generation on atom books.
- **SoundTypes, levels**: the "levels" were n-gram orders of one sequence;
  now real levels: gestures cut where the next type is surprising (high
  -log P), clustered by their content, recursively.
- **SoundTypes, generation**: besides n-grams, variable-order Markov (with
  back-off) and the factor oracle (Allauzen, Crochemore and Raffinot;
  navigated as OMax and the Variable Markov Oracle).
- **SoundTypes, synthesis**: units are realised with their own sound,
  joined by crossfades (or rebuilt from atoms), instead of a frame-by-frame
  phase-vocoder resynthesis from one representative frame per type.
- **SoundTypes, segmentation**: level 1 is either runs of frame types or,
  as in the original, the notes between onsets clustered as wholes; the
  features can include the pitch classes, so that types follow harmony.
- **SoundTypes, mixing levels**: a sequence generated at level 2 or 3 can
  be played with the level below regenerated (an oracle walk inside each
  unit): the form of one level with the detail of another.
- **SoundTypes, rematch**: frame by frame as the original (grains of the
  source overlap-added at the target's frames, each sound's features
  standardised in its own space), or at any level (units, gestures,
  sections), with the closest instance of the closest type, at the target's
  timing or not,
  and with the target's loudness envelope imposed (contour), so that speech
  keeps its rhythm when made of music.

## Corrections for Musil

Problems found in Musil's code while porting it, fixed in Moritz and, since
Musil 0.9.1, in Musil too (`musil/src/signals.h`, `musil/src/signals.mu`):

- **Ambisonic decoders** (`ambi-decode`, `binaural-decode`): no normalisation
  of the orders (2 per order on a ring, 2n+1 on the sphere), so the energy
  vector of a ring is 0.57 / 0.66 at orders 1 / 2 instead of the theoretical
  0.71 / 0.87. The binaural decoder's ten virtual speakers (none below) bend
  directions by up to 16 degrees; Moritz uses 17 (5-7 degrees). Moritz:
  `ambidec~`, `binaural~` in `objects_spatial.cpp`.
- **`chromagram`**: every bin goes to the pitch class of its centre. Below
  about 400 Hz a 4096-point spectrum has fewer bins than semitones, so some
  pitch classes get the bass notes and others none (on the Sousa march: C, D,
  F, G, A, B-flat inflated, E-flat missing); a pure tone also spills onto
  neighbouring classes through the window's main lobe; and overtones count as
  notes, so the 3rd and 5th harmonics pull the harmony a fifth and a third
  up. Result: wrong keys (the C minor prelude read as C major, the E-flat
  march as F major). Moritz reads the spectral peaks (parabolic
  interpolation), gives each to its nearest note from C2 to B6, and counts a
  probable 3rd, 5th, 6th or 7th partial a tenth (`mir::chroma` in `mir.h`).
- **`foote-novelty`**: the kernel's parts outside the matrix count as zero,
  so the curve is inflated at both ends (spurious boundaries at the start and
  the end). Moritz averages the kernel over what lies inside and gives 0
  where less than half of it fits.
- **`tempogram`**: every column is scaled to its own maximum, so quiet
  passages and single loud hits (whose long decay looks periodic at every
  tempo) become bright vertical bands. Moritz rectifies the onset strength
  above its local mean (0.3 s, as Grosche and Mueller) and divides by the
  energy (lag 0). It is also computed at every frame, with all lags up to the
  window: minutes of music take minutes; Moritz computes 4 columns a second
  and only the lags of 30-300 bpm.
- **`mel-spectrogram`** (and so `mfcc-gram`): `10 log10(1 + e)` on unscaled
  FFT magnitudes depends on the FFT size and is not a level in dB; Moritz
  scales the spectrum (a full-scale sine is 0 dB) and floors at -100 dB.
- **`register-curve`, `polyphony-estimate`**: a frame counts as silent only
  when its maximum is exactly 0, so noise in quiet passages gives jumpy
  values; Moritz uses a -60 dB floor, keeps the previous values in silence,
  and offers median smoothing.

## Sources

- **Max 9 reference pages** (`*.maxref.xml` in the Max application) define
  the behaviour Moritz follows.
- **Shared objects that behave differently from Max**: `slider` (attributes
  vs `[min max]`), `gain~` (integer values, log curve), `poly~` in/out
  numbering, `pfft~` defaults (overlap 2 in Max, 4 here), `sfplay~` (`speed`
  message, no dialog), `kslider` (note-off on release here),
  `selector~`/`gate~` (5 ms crossfade here), `biquad~` (number coefficients
  here), `expr` (fewer functions; `random()` returns decimals). About 30 more
  have fewer inlets/outlets or options than in Max. Max has no `expr~`,
  `inlet~`/`outlet~`, `lrshift~`.
- **Musil** (github.com/CarmineCella/musil): `src/music/orchidea.h` (the
  search core), `src/music.mu` (scores, databases, computer-assisted
  composition, orchestration, MusicXML), `src/signals.h`/`signals.mu` (DSP,
  MIR, NMF, spatialisation), `src/plot.h` (figures and the roll),
  `src/scientific.h`/`.mu` (machine learning, NMF), `fetch_tinysol.sh`
  (datasets as release assets).
- **sound_math**: `BlockVocoder.h` (sparkle), `BlockConv.h`, `features.h`,
  `tools/soundtypes` (SoundTypes.h, ModelingSpace.h), `tools/nebula`,
  `tools/combsyn`.
- **Maple**: `src/algorithms.h` (dictionaries, pursuit, reconstruction,
  Markov generation), `params.txt`, dictionaries (Kitchen, People, Doors,
  Animals, Orchestra, Cl_ord, TinySOL_flat, SOL_flat).
- **WEFT** (github.com/CarmineCella/weft): `src/*.h` (Matrix, Dense, Conv2D,
  attention, Adam, STFT, MelTransform, AudioFeatures…), `notes/` 00-23, the
  book draft in `book/`.
