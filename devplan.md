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

| Step | Planned | Block | Content |
|---|---|---|---|
| 1 | 16-22 Nov | Foundations | Windows port, CI on macOS/Linux/Windows rendering every patch, releases built by CI. Several patches open at once. Clipboard as `.moritz` text. Code view. Tutorials. Import from Max. v0.5 |
| 2 | 23-29 Nov | Signal processing | Cross-synthesis in `sparkle~`. Supervised `nmf`. Filter design, spectral envelope. `nebula~`. Ambisonics and binaural. v0.6 |
| 3 | 30 Nov-6 Dec | Analysis and MIR | Onsets and segmentation. Mel, MFCC, chroma (live and offline). Key, tempo, beat tracking. Self-similarity, novelty. Loudness, register, polyphony curves. Plots and matrix display. |
| 4 | 7-13 Dec | Maple and SoundTypes | Maple with FFT matching, dictionaries (frames, onsets, files, Gabor/gammatone), decomposition, hybridisation. SoundTypes: types, matching, Markov generation. Dataset: Maple dictionaries. |
| 5 | 14-20 Dec | Recursive score | Score model, roll, rendering, playback, MusicXML, save/load. Database layer and `db-gen`. Dataset: TinySOL. |
| 6 | 21 Dec-3 Jan | Orchestration | Orchidea mimetic orchestration (static, dynamic) into scores; granular and morphological orchestration; playability checks. |
| 7 | 4-10 Jan | Neural networks | `mlp`, classifier, `autoencoder`/`vae` with a playable latent space, images as data. Dataset: MNIST. |
| 8 | 11-17 Jan | Research | VAE on matching pursuit; 2-D corpus map. |
| 9 | 18-24 Jan | Integration | Export to Max, generated documentation site, complete tutorials. |
| 10 | 25-31 Jan | Stabilisation | Fixes, performance, testing on real Windows machines. Feature freeze: v1.0-rc. |
| 11 | Feb | Pilot | Classes at CNSM, evaluation, v1.0, system paper (ICMC, SMC or NIME). |

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
  orders 1 / 2, the theoretical values. Musil's `ambi-decode` and
  `binaural-decode` lack this normalisation.
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

## Open points

- A separate cepstrum object (the cepstrum is inside `specenv~`).
- Where datasets are published: releases of the Moritz repository, or
  Musil's existing TinySOL release.

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
