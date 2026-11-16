# Moritz — development plan

What Moritz is meant to become, the design decisions behind it, and the
steps that lead to version 1.0.

## Vision

Moritz is a Max-like environment for sound and music computing by
Carmine-Emanuele Cella. It brings together his research (sparkle,
condec/pconv, Maple, SoundTypes, Orchidea, the scores and computer-assisted
composition of Musil, WEFT) in a single, free, open-source system
(BSD 2-Clause) for macOS, Linux and Windows.

It is first a **teaching tool** (CNSM Paris): orchestration and AI for music
are taught with it side by side, so both need basic tools early. Every object
is short, readable C++ with no dependencies, written from first principles,
so students can open the box and see how a phase vocoder, k-means or
attention works. The tools are introduced little by little through
tutorials, not split into packages. A book bringing all the pieces together,
with Moritz as the front end, follows.

## Design decisions

- **All in one system.** The algorithms of the author's tools become native
  Moritz objects: their code lives in `src/`, adapted, never linked as an
  external library.
- **Maple** does sound decomposition and hybridisation (sparse atomic
  matching pursuit with dictionaries), with **FFT matching**: the inner
  products of a frame with an atom at every offset computed by FFT, for
  Gabor/gammatone and sound dictionaries alike.
- **SoundTypes** (ICMC 2013) does the symbolic level: types (clusters of
  frames), type matching for hybridisation, and n-gram Markov generation. It
  replaces Maple's former probabilistic generation, which lost the weights,
  used per-rank chains and a shared prefix.
- **Generation**: a VAE on spectral frames rendered by Maple (matching
  pursuit as the renderer); then a VAE on atom activations; a transformer on
  atom sequences later.
- **The recursive score is the universal time model**, as in Musil: events
  are sounds, database notes, patch instruments, patch calls, orchestrations
  or other scores; a roll display, recursive rendering, playback, MusicXML
  export, and a file format close to Musil's. Orchestrations produce scores.
  Multitrack, transport and a clip grid are later views of the same model.
- **Orchestration**: Orchidea's mimetic orchestration first, then granular
  and morphological orchestration, and playability checks.
- **Neural networks (WEFT)**: the WEFT code (header-only, standard library)
  and its 23 notes become objects and tutorial patches (MLP, classifier,
  autoencoder/VAE, convolutional networks, attention). Images are data only:
  a matrix display, PNG loading, MNIST; no image processing environment.
- **Plots**: one display for curves, scatter plots, bars and matrices (heat
  maps), from Musil's figures. Analysis results (spectrograms, mel, MFCC,
  chroma, self-similarity, NMF factors) and images (MNIST, network weights)
  share it.
- **Teaching objects stay**: `nebula~` coexists with the granulator example,
  `sparkle~` with `pvoc`, the high-level objects with the spectral building
  blocks.
- **Datasets** are release assets downloaded on demand, added with the
  feature that needs them: Maple dictionaries (sounds only), TinySOL with its
  feature database (Orchidea), MNIST (WEFT). `db-gen` builds a database from
  any folder of sounds, in Musil's format.
- **Max compatibility**: Moritz follows Max's arithmetic (integer unless a
  decimal argument; typed `pack`/`unpack`; integer division in `expr`).
  Patches import from Max (`.maxpat`, `.maxhelp`), with a report of what does
  not convert; export to Max comes later.
- **Tutorials** accompany every step: patching and control, DSP, samples,
  spectral processing and sparkle, convolution and space, descriptors,
  classic machine learning, Maple, SoundTypes, neural networks, corpus and
  concatenative synthesis, orchestration, scores and computer-assisted
  composition, timeline.
- **A step is complete** when it has objects with help patches, examples, a
  tutorial chapter, tests in CI on all three systems, README entries and a
  release.

## Testing

`moritz --render patch seconds [bang=id[@sec]] [send=id:msg[@sec]]
[midi=t:bytes] [png=file] [extra=patch]` renders a patch offline and draws
it into a PNG. `tools/test-patches.sh` renders every example, help patch,
library abstraction and tutorial this way, and fails on any warning; GitHub
Actions runs it on macOS, Linux and Windows, each built on its own system,
and publishes the builds as a release for every `v*` tag.

## Steps

| Step | Block | Content |
| --- | --- | --- |
| 1 | Foundations | Windows port, CI on macOS/Linux/Windows rendering every patch, releases built by CI. Several patches open at once. Clipboard as `.moritz` text. Code view. Tutorials. Import from Max. v0.5 |
| 2 | Signal processing | Cross-synthesis in `sparkle~`. Supervised `nmf`. Filter design, spectral envelope. `nebula~`. Ambisonics and binaural. v0.6 |
| 3 | Analysis and MIR | Onsets and segmentation. Mel, MFCC, chroma (live and offline). Key, tempo, beat tracking. Self-similarity, novelty. Loudness, register, polyphony curves. Plots and matrix display. v0.8 |
| 4 | Maple and SoundTypes | Maple with FFT matching, dictionaries (frames, onsets, files, Gabor/gammatone), decomposition, hybridisation. SoundTypes: types, matching, Markov generation. Dataset: Maple dictionaries. |
| 5 | Recursive score | Score model, roll, rendering, playback, MusicXML, save/load. Database layer and `db-gen`. Dataset: TinySOL. |
| 6 | Orchestration | Orchidea mimetic orchestration (static, dynamic) into scores; granular and morphological orchestration; playability checks. |
| 7 | Neural networks | `mlp`, classifier, `autoencoder`/`vae` with a playable latent space, images as data. Dataset: MNIST. |
| 8 | Research | VAE on matching pursuit; 2-D corpus map. |
| 9 | Integration | Export to Max, generated documentation site, complete tutorials. |
| 10 | Stabilisation | Fixes, performance, testing on real Windows machines. Feature freeze: v1.0-rc. |
| 11 | Pilot | Classes at CNSM, evaluation, v1.0, system paper (ICMC, SMC or NIME). |

Later: multitrack, transport and clip-grid views; a plugin host (CLAP and
VST3, floating editors); the transformer on atom sequences; `concat~`.

### Step 1: foundations (v0.5)

- Max arithmetic: typed `+ - * / %`, comparisons, `pack`/`unpack`, `scale`,
  `expr`; a number written `2.` stays decimal and is saved as written.
- Windows: `make` in MSYS2, or `make windows` cross-compiled with MinGW-w64
  (static, no DLLs, UTF-8 through the manifest, icon).
- Several patches open at once: an invisible host patch runs them all;
  per-window menus on Windows and Linux, one menu bar on macOS.
- Cut, copy, paste and duplicate through the system clipboard as `.moritz`
  text, between windows and patches, without reloading the patch.
- Code view (Code button, Cmd+T): the patch as text beside the drawing, two
  views of the same patch kept in sync both ways; text edits apply when
  typing pauses and keep unchanged boxes running.
- `if` (as in Max); `dspstate~` with Max's outlets.
- Import from Max (File menu, `moritz --import`): all 733 Max 9 help patches
  convert and load; 81.6% of their boxes map to Moritz objects. Numbered
  `in 1`/`out~ 2` order a voice's ports, as in Max.
- `--no-audio` start option.

Max objects most often missing in imported patches: `prepend`, `zl`, `pak`,
`uzi`, `number~`, `umenu`, `multislider`,
`midiin`/`midiout`/`midiformat`/`midiinfo`, `coll`, `dict`, `change`, `!-`,
`delay~`, `edge~`, `>~`, `scale~`, `delta~`, `fft~`, `vexpr`, `incdec`,
`preset`/`pattrstorage`, `attrui`; the `mc.*` family.

### Step 2: signal processing (v0.6, v0.7)

- `sparkle~` cross-synthesis in sparkle's three modes (multiplicative,
  spectral flattener, morphing), with a buffer~ or a live second sound;
  denoising and spectral morphing through its `threshold` and cross mode 3.
- `nmf`: unsupervised `run`, and Musil's supervised mode (`learn` each
  source, `separate` a mix, `forget`); messages queue while a job runs. On a
  voice and drums mix: 0.98 correlation per source supervised, 0.70-0.76
  unsupervised.
- `specenv~` (true envelope through the cepstrum, in pfft~), `filterdesign`
  and `filterplot` (RBJ cookbook biquads), `nebula~` (sound_math's nebula).
- Ambisonics (`ambienc~`, `ambirot~`, `ambidec~`) and `binaural~` (17 virtual
  speakers through MIT KEMAR HRIRs). The spherical harmonics are exact
  against their closed forms; the decoders are normalised (2 per order on a
  ring, 2n+1 on the sphere), giving a ring energy vector of 0.71 / 0.87 at
  orders 1 / 2, the theoretical values (see Corrections for Musil).
- `hrtf~`: direct binaural panning with all 710 KEMAR directions,
  interpolated and crossfaded when moving; exact at measured directions. Its
  levels follow the real head, while `binaural~` is up to 8 dB louder
  overhead (low-order ambisonics on headphones).
- Library abstractions: `impose~`, `binpan~`, `pitchshift~`.
- Scrollbars and wheel scrolling when a patch is larger than its window;
  `slider` typed by its written limits; timed `--render` actions
  (`bang=id@4.5`).
- A browser at startup (tutorials, examples by topic from a `; topic:` line
  in each example, objects with their help, recent files, search) and
  tutorials 1-8 (first patch, numbers, sound files, filters, spectrum,
  grains, space, separation).

### Step 3: analysis and MIR (v0.8)

- Feature matrices in `buffer~`s (a channel a row, a sample a frame, the
  sample rate the frame rate): `spectrogram`, `melgram`, `mfccgram`,
  `chromagram`; live `melbands~`, `mfcc~`, `chroma~`.
- Onsets and segmentation: `onsets` (offline, `segment k` for `play~`) and
  `onset~` (live).
- Tempo and beats: `beats` (prior on the tempo, dynamic programming, click
  track, tempogram).
- Key: `keyfind` (Krumhansl-Kessler profiles, over a whole sound or a part).
- Form: `similarity` (self-similarity matrix) and `novelty` (boundaries,
  `section k`).
- Curves: `curves` (loudness, register, polyphony; median smoothing).
- `plot`: curves, bars, scatter plots and matrices from a `buffer~` or lists,
  marks, cursor, labels, a window with zoom and values, PNG export; the same
  display serves the images of the neural-network step.
- `prepend` and `append`.
- The chromagram reads spectral peaks and discounts non-octave partials
  (Musil's mislabels keys: see Corrections for Musil). Checked: the march
  in E-flat (trio in A-flat), the preludes in C minor and D major, the
  Allegretto in A minor; tempi of the drums (119) and the march (116).
- Sounds for MIR, public domain or CC0: Sousa's "The Stars and Stripes
  Forever" (U.S. Marine Band), two Bach preludes (Kimiko Ishizaka).
- Help patches for every object, five examples (structure, key, beats,
  segments, live) and tutorial 9.

## Open points

- A separate cepstrum object (the cepstrum is inside `specenv~`).
- Where datasets are published: releases of the Moritz repository, or
  Musil's existing TinySOL release.

## Corrections for Musil

Problems found in Musil's code while porting it, fixed in Moritz and still
to be fixed in Musil (`src/signals.h`, `src/signals.mu`):

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
