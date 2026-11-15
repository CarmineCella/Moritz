# Moritz — development plan

A working document for the next conversations: what Moritz is meant to become,
the decisions already taken, and the timeline. Written on 15 November 2026,
at version 0.4.

## Vision

Moritz is Carmine-Emanuele Cella's own sound computing environment: a
Max-like patcher that brings together his research (sparkle, condec/pconv,
Maple, SoundTypes, Orchidea, the scores and computer-assisted composition of
Musil, WEFT) in a single, free, open-source system (BSD 2-Clause) for macOS,
Linux and Windows.

It is meant first as a **teaching tool** (CNSM Paris, with colleagues): the
author will teach **orchestration** and **AI for music** at the same time, so
both need basic tools early. Its strongest teaching argument: every object is
short, readable C++ with no dependencies, written from first principles, so
students can open the box and see how a phase vocoder, k-means or attention
works. The tools are "shown little by little" through tutorials, not split
into packages. Later, a book bringing all the pieces together, with Moritz as
the front end.

## State at 0.4

- Patcher: Max conventions, undo, completion, port help, multiline message
  boxes, subpatchers, abstractions, `poly~` (`note`, `midinote`), `pfft~`,
  a library folder (`library/`, plus `~/Documents/Moritz/library`), help
  patches for every object (right-click a box, or Option-click in locked
  mode), about 130 objects, 32 example patches with real recordings (from
  Musil's examples).
- MIDI (RtMidi), audio (miniaudio), GUI (FLTK); all downloaded by the
  Makefile. macOS universal app; Linux build (`make linux`).
- From the author's tools: `sparkle~` (phase vocoder, formant preservation,
  phase locking; buffer or live), `conv~` (zero-latency multichannel
  convolution), `condec` (convolution/deconvolution of buffers),
  `descriptors~`, `function` (breakpoint editor).
- Background jobs (worker threads, progress, stop): `moritz~` (offline
  rendering of another patch), `nmf`, `hpss`.
- Learning: `knn`, `kmeans`, `pca`, `zscore`, `linreg`.
- Testing: `moritz --render patch seconds [bang=id] [send=id:msg]
  [midi=t:bytes] [png=file] [extra=patch]` renders offline and draws the
  patch into a PNG; all patches are checked this way after every change.
- Release v0.4 on GitHub (CarmineCella/Moritz) with macOS and Linux
  downloads.

## Decisions taken

- **All in one system.** The author's algorithms become native Moritz
  objects: their code is copied into `src/` and adapted, never linked as
  external libraries.
- **Maple** does sound decomposition and hybridisation (sparse atomic
  matching pursuit with dictionaries). **SoundTypes** (ICMC 2013) does the
  symbolic level: types (clusters of frames), type matching for
  hybridisation, and n-gram Markov generation; it replaces Maple's old
  probabilistic generation (which gave "noisy bubbles": weights discarded,
  per-rank chains, shared prefix).
- **Maple needs FFT matching**: the inner products of a frame with an atom at
  every offset by FFT, for Gabor/gammatone and for sound dictionaries alike.
- **Generation research**: a VAE on spectral frames rendered by Maple (MP as
  the renderer); then a VAE on atom activations; the transformer on atom
  sequences later.
- **The recursive score is the universal time model**, ported from Musil:
  events are sounds, database notes, patch instruments, patch calls,
  orchestrations or other scores; roll display (Musil's `plot.h`), recursive
  render, playback, MusicXML (Musil's `score-notate`), save/load close to
  Musil's format. Orchestrations produce scores. Multitrack, transport and a
  clip grid are later views of the same model.
- **Orchestration**: Orchidea's mimetic orchestration first (Musil's
  `orchidea.h` plus the `music.mu` glue rewritten in C++), then granular and
  morphological orchestration, playability checks.
- **WEFT**: the code (header-only, standard library) plus the 23 notes become
  objects and tutorial patches (MLP, classifier, autoencoder/VAE,
  convolutional networks, attention). Images as data only: a `matrix`
  display, PNG loading, MNIST. No image processing environment.
- **`nebula~`** (sound_math) coexists with the granulator example, as
  `sparkle~` coexists with `pvoc`; the spectral objects stay (teaching).
- **Datasets** as release assets with a download script/menu, added when the
  feature that needs them lands: Maple dictionaries (sounds only), TinySOL
  with its feature database (Orchidea), MNIST (WEFT). `db-gen` ported so
  databases can be made from any folder, in Musil's format.
- **Max compatibility**: Moritz follows Max's arithmetic (integer unless a
  float argument; typed `pack`/`unpack`; integer division in `expr`).
  **Import from Max first** (adoption, and it measures what is missing), then
  export. About 15 shared objects differ in behaviour (see the list at the
  end).
- **Tutorials** for every stage, written with each block: patching and
  control, DSP, samples, spectral/sparkle, convolution and space,
  descriptors, classic ML, Maple, SoundTypes, neural networks, corpus and
  concatenative synthesis, orchestration, scores and CAC, timeline.
- **Every block is done when it has**: objects with help patches, an example,
  its tutorial chapter, tests in CI on all three systems, README entries and
  a release.
- Housekeeping: `docs/` (sound_math, maple, condec and the Musil patcher
  note) is temporary and never committed; commits carry no Claude co-author
  line.

## Timeline

Weeks start on Monday. One block a week assumes 3-5 working sessions.

| Week | Dates | Block | Content |
|---|---|---|---|
| 1 | 16-22 Nov | Foundations | Windows port (MinGW), CI on GitHub Actions for macOS/Linux/Windows running every patch through `--render`, builds as downloads, releases built by CI. Several patches open at once. Cut/copy/paste/duplicate through the system clipboard (as `.moritz` text). "View source" in the box menu. Help > Tutorials. **Import from Max…** (`.maxpat` reader, object mapping, subpatchers, abstractions, a report of what did not convert). v0.5 |
| 2 | 23-29 Nov | Signal processing (Musil, sound_math) | The 3 cross-synthesis modes of `sparkle~`. `nmf` as in Musil (supervised: learn parts, separate with). Filter designs, spectral envelope and cepstrum, denoise, spectral morph. `nebula~`. Ambisonics and binaural. v0.6 |
| 3 | 30 Nov-6 Dec | Analysis and MIR (Musil) | Onsets and segmentation. Mel, MFCC, chroma (live and offline). Key, tempo, beat tracking. Self-similarity, novelty. Loudness, register, polyphony curves. `matrix` display. v0.7 |
| 4 | 7-13 Dec | Maple and SoundTypes | Maple with FFT matching, dictionaries (frames, onsets, files, Gabor/gammatone), decomposition, hybridisation. SoundTypes: types, matching, Markov generation. Dataset: Maple dictionaries. v0.8 |
| 5 | 14-20 Dec | Recursive score | Musil's score model, roll, render, playback, MusicXML, save/load. Database layer and `db-gen`. Dataset: TinySOL. v0.9 |
| 6 | 21 Dec-3 Jan | Orchestration (holidays) | Orchidea mimetic (static, dynamic) into scores; granular and morphological orchestration; playability checks. v0.10 |
| 7 | 4-10 Jan | WEFT and neural networks | `mlp`, classifier, `autoencoder`/`vae` with a playable latent space, images as data. Dataset: MNIST. v0.11 |
| 8 | 11-17 Jan | Research | VAE on matching pursuit; 2-D corpus map if there is time. |
| 9 | 18-24 Jan | Integration | Export to Max, generated documentation site, tutorials completed. |
| 10 | 25-31 Jan | Stabilisation | Fixes, performance, Windows on a real machine. Feature freeze: v1.0-rc. |
| Feb | | Pilot and paper | CNSM classes, evaluation, v1.0. |

### Status of week 1 (15 November)

Released as v0.5 on 15 November (built and tested by CI on macOS, Linux and
Windows; Windows builds natively in MSYS2 or cross-compiled):

- Max arithmetic (committed): typed `+ - * / %`, comparisons, `pack`/`unpack`,
  `scale`, `expr`; `2.` kept as written.
- Windows: `make` in MSYS2, or `make windows` cross-compiled with MinGW-w64
  (static, no DLLs, UTF-8 code page through the manifest, icon). Rendered
  by CI on Windows; not yet tried by hand on a Windows machine.
- CI: `.github/workflows/build.yml` (macOS, Linux, Windows built on Linux and
  tested on Windows, release on tags) and `tools/test-patches.sh`.
- Several patches open at once (an invisible host patch runs them all as
  extras); per-window menus on Windows/Linux, one menu bar on macOS.
- Cut/copy/paste/duplicate through the system clipboard as `.moritz` text,
  between windows and patches, without reloading the patch.
- Code view (Code button, Cmd+T): the patch as text beside the drawing,
  two views of the same patch kept in sync both ways; text edits apply when
  typing pauses and keep unchanged boxes running (`Patch::applyText`).
- `if` (as in Max); `dspstatus~` renamed `dspstate~`, with Max's outlets.
- Help > Tutorials with the first two tutorials (`tutorials/`).
- Import from Max (File menu, `moritz --import`): all 733 Max 9 help patches
  convert and load; 81.6% of their boxes map to Moritz objects. Numbered
  `in 1`/`out~ 2` now order a voice's ports, as in Max.
- `--no-audio` start option.

Max objects most often missing in imported patches (candidates for later
weeks): `prepend`, `zl`, `pak`, `uzi`, `number~`, `umenu`, `multislider`,
`midiin`/`midiout`/`midiformat`/`midiinfo`, `coll`, `dict`, `change`, `!-`,
`delay~`, `edge~`, `>~`, `scale~`, `delta~`, `fft~`, `vexpr`, `incdec`,
`preset`/`pattrstorage`, `attrui`; the `mc.*` family.

After the paper: multitrack/transport/clip-grid views, VST player (CLAP+VST3,
floating editor; needs the global state refactored per instance), the
transformer on atom sequences, `concat~`.

**Paper**: ICMC 2027's call is not out yet; ICMC 2026 closed on 22 December
2025 (extended to 4 January 2026) and ICMC 2025 in mid-January. If 2027 is
similar, a system paper in week 7; otherwise March, with the February pilot
as evaluation (also consider SMC, NIME). Decide in December.

## Open questions

- When do the two courses start? (If in December, swap weeks 6 and 7.)
- Datasets: a release of the Moritz repository, or Musil's existing TinySOL
  release?
- The paper venue and deadline.

## Reference notes

- **Max 9 reference pages** (to check behaviour):
  `/Applications/Max_9.app/Contents/Resources/C74/docs/refpages/**/*.maxref.xml`.
  Max 8 and 9 are installed on the development Mac.
- **Shared objects that behave differently from Max** (after the arithmetic
  change): `slider` (attributes vs `[min max]`), `gain~` (integer values,
  log curve), `poly~` in/out numbering (Max numbers them), `pfft~` defaults
  (overlap 2 in Max, 4 here), `sfplay~` (`speed` message, no dialog),
  `kslider` (note-off on release here), `selector~`/`gate~` (5 ms crossfade
  here), `biquad~` (number coefficients here), `expr` (fewer functions;
  `random()` returns decimals). About 30 more have fewer inlets/outlets or
  options than in Max. Max has no `expr~`, `inlet~`/`outlet~`,
  `lrshift~`.
- **Musil** (github.com/CarmineCella/musil): `src/music/orchidea.h` (the
  search core), `src/music.mu` (scores, databases, CAC fragments,
  orchestration methods, MusicXML), `src/signals.h`/`signals.mu` (DSP,
  MIR, NMF, spatialisation), `src/plot.h` (FLTK figures and the roll),
  `src/scientific.h`/`.mu` (ML, NMF), `fetch_tinysol.sh` (datasets as release
  assets).
- **sound_math** (in `docs/`): `BlockVocoder.h` (sparkle), `BlockConv.h`,
  `features.h`, `tools/soundtypes` (SoundTypes.h, ModelingSpace.h),
  `tools/nebula`, `tools/combsyn`.
- **Maple** (in `docs/maple`): `src/algorithms.h` (dictionaries, pursuit,
  reconstruction, Markov generation), `params.txt`, dictionaries in `dict/`
  (Kitchen, People, Doors, Animals, Orchestra, Cl_ord, TinySOL_flat, SOL_flat).
- **WEFT** (github.com/CarmineCella/weft): `src/*.h` (Matrix, Dense, Conv2D,
  attention, Adam, STFT, MelTransform, AudioFeatures…), `notes/` 00-23, the
  unfinished book in `book/`.
