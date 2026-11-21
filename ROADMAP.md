# Roadmap

What Moritz is meant to become, the decisions behind it, the versions that lead to 1.0, and what is
asked for and not done yet. This is the one place for the plan: it is kept current as work lands.
How to work on the code is in [CLAUDE.md](CLAUDE.md); technical notes (testing, departures from
the ported tools, corrections to Musil, sources) are in [NOTES.md](NOTES.md); the design sketches of
the editors are in [mockups/](mockups/README.md); what changed in each release is in the GitHub
releases and, for Musil, in [musil/CHANGES.md](musil/CHANGES.md).

## Vision

Moritz is a Max-like environment for sound and music computing by Carmine-Emanuele Cella. It brings
together his research (sparkle, condec/pconv, Maple, SoundTypes, Orchidea, the scores and
computer-assisted composition of Musil, WEFT) in a single, free, open-source system (BSD 2-Clause)
for macOS, Linux and Windows.

It is first a **teaching tool** (CNSM Paris): orchestration and AI for music are taught with it side
by side. Every object is short, readable C++ written from first principles, so students can open the
box and see how a phase vocoder, k-means or attention works. The tools are introduced little by
little through tutorials, not split into packages. A book bringing the pieces together, with Moritz
as the front end, follows.

Musil is its language, and also a program of its own: a small scripting language for sound and music
computing, shipped with Moritz and alone.

## Decisions

### Moritz and Musil

- **One implementation of each algorithm, in Musil** (`musil/src/signals`, `music`, `scientific`):
  its C++ engines are headers that need neither the interpreter nor Moritz, and Moritz's objects call
  them directly. Nothing in `musil/` includes Moritz.
- **Shared language and engines, separate host services.** What shows, plays or finds data is a
  service of the host behind the same functions: a script calls `show`, `play`, `figure` or the live
  functions the same way everywhere; Musil alone answers with its own windows and audio, Moritz with
  its windows and its engine, so a script runs in both, and Moritz has one audio engine and one way
  of drawing (the `plot` object and Musil's `show` share one renderer, in Musil). Musil alone keeps
  its live audio and its read-only roll; until Musil's live functions play through Moritz's engine,
  Moritz says why they are missing.
- **Musil is the offline language in Moritz**: it generates, transforms and renders; Moritz's audio
  thread runs only Moritz's C++. Musil is still the language of patches as data: patches and scores
  are C++ structures shown to it as values (mockups 10 to 14).
- **Datasets** live in one folder both look in: TinySOL downloaded from GitHub's release assets,
  MicroSOL bundled as a fallback, FullSOL wherever the user says it is; `db-gen` builds a database
  from any folder of sounds.

### Time and scores

- **The recursive score is the time model.** A score holds notes (played by an instrument: SOL
  samples or a patch), sounds (regions of a file or a buffer~), patches (that run while their clip
  plays, with automated parameters), messages and other scores, at any depth; a score cannot contain
  itself. The timeline and the roll are views of a score; the sound editor is the view of sounds, the
  leaves; patches and scores contain each other (mockup 1).
- **The score is one C++ structure, defined in Musil's C++** (`musil/src/music/score.h`). Musil uses
  it as a value; Moritz adds editing with undo, sample-accurate playback and the views. Orchestrations
  and generators produce it.
- **A .score file is Musil data**: the score as a list of lists, read without running code (safe for
  files from elsewhere), written back after every edit; times in seconds. A score converts to and from
  a coll for its flat parts.
- **`score` is an active object** (mockup 2): edited inside its box, driven by messages, played on the
  transport, its events sent from its outlets; double-click opens it as timeline or roll.
- **Each editor is written once, as a panel** (mockup 8), first in its own window, later in an
  arranger window or a patch's dock.

### Objects and teaching

- The author's tools become Moritz objects and Musil functions (Maple with FFT matching; SoundTypes
  for types, matching and generation; Orchidea; WEFT's networks), with **Max's arithmetic** and
  **import from Max**; export to Max later.
- **Teaching objects stay** beside the high-level ones (`nebula~` and the granulator, `sparkle~` and
  `pvoc`, the spectral building blocks).
- **A version is complete** when it has objects with help patches, examples, a tutorial chapter,
  reference entries, tests, and a release whose CI passes on all three systems.

## Versions

Done: 0.5 foundations (Windows, CI, code view, Max import); 0.6-0.7 signal processing (sparkle~
cross-synthesis, NMF, filter design, nebula~, ambisonics and binaural); 0.8 analysis and MIR; 0.9
atoms and types (Maple, SoundTypes); 0.10 Musil in Moritz (the code box, one data folder); 0.10c
safety; 0.11 the engine and editor rebuilt (timed events, one plan published to the audio thread,
the editor's command table and undo rule), YIN, `descriptors~` on the shared analyses.

| Version | Content |
| --- | --- |
| 0.12 | Sound editor; Orchidea in Moritz; the score foundation (score, transport); datasets; Musil in Moritz |
| 0.13 | Timeline (mockup 4), designed for a multitrack mode; patch clips and track instruments and effects |
| 0.14 | Roll and the active score box (mockups 5, 2); Generate and Transform; coll |
| then | Orchestration: queries, morphological and granular modes, the pieces as patches |
| then | Neural networks (WEFT): `mlp`, classifier, `autoencoder`/`vae` with a playable latent space, images as data (MNIST). WEFT is finished first in its own project, then ported |
| then | Multitrack mode, as in Reaper (layout B of mockup 8, with a mixer panel) |
| then | Research: VAE on matching pursuit; a 2-D corpus map; a transformer on atom sequences; `concat~` |
| then | Integration: export to Max, a documentation site, complete tutorials |
| 1.0-rc | Stabilisation: fixes, performance, testing on real Windows machines; feature freeze |
| 1.0 | Pilot: classes at CNSM, evaluation, a system paper (ICMC, SMC or NIME) |

## Open

### 0.12 (in progress, on main)

- [x] Sound editor core: overview, waveform, spectrogram, zoom, selection, preview, edits with undo, save.
- [x] Sound editor, second part: Process menu (offline objects, a Moritz patch, a Musil script),
      markers and regions (by hand, from onsets, beats, novelty, soundtypes units), regions table,
      To coll, To score, the pencil (move single samples by hand), Cmd+L for loop, tool modes.
- [ ] Sound editor: spectral painting (draw on the spectrogram to erase or keep a part, an STFT mask);
      a buffer~'s markers saved in its patch (now kept while the patch is open, and beside its WAV).
- [x] Orchidea in Moritz: `orchestra` and `orchidea`, mimetic mode.
- [x] Score foundation: the score in Musil's C++, .score files as Musil data, `transport`, `score`.
- [x] One datasets folder for Moritz and Musil: FullSOL, TinySOL, MicroSOL by name; a Datasets window;
      TinySOL download; dbgen ("Analyse a folder…") without code.
- [x] The outlets of `orchestra` and `orchidea` described in the reference and tooltips.
- [x] Musil's live functions in Moritz say why they are missing.
- [ ] Musil's plots as a host service in Moritz (`show`, `save-png`), one renderer with `plot`.
- [ ] Musil's examples run in Moritz's tests too (those that can).
- [ ] Sound editor leftovers: a buffer~'s markers saved inside its patch, region edges dragged with the
      mouse, To score straight into a score object.
- [ ] Musil's golden tests now and then fail when ctest runs them in parallel with the examples (they
      seem to share output files): give each its own files.

### 0.13 and 0.14

- [ ] Timeline (mockup 4): tracks with a mixer strip in the model, sound, score, patch and message
      clips, automation lanes, inspector, render; the score document with undo shared by every view.
- [ ] Patch clips that run while they play; track instruments and effects.
- [ ] Roll and the active `score` object (mockups 5 and 2): editing in the box and in its window,
      piano roll, list, the score as text in sync; Transform; Generate (Musil, offline); `coll`.
- [ ] A tempo map (today a score has one tempo); stretched sound clips played stretched.

### Orchestration

- [ ] **Sample selection by query**: one small language to choose a subset of a database's samples,
      written once in Musil's C++ and used by `orchestra` and by Musil (`db-select`). Fields from the
      database: instrument, family, technique (playing style), dynamics, pitch, string, mute and the
      other tags in the file names, and the analysed features. Patterns (`pizz*`, `/sul_?pont/`),
      ranges (`C4..G5`, `p..f` in the order of dynamics, `pitch 60..72`), lists (`Vn|Va`), and, or,
      not; per instrument or for the whole orchestra. Shows how many samples remain before a run.
- [ ] Orchidea's morphological and granular modes, and the adaptive segmentation, in C++ (today
      about 800 lines of music.mu), producing a score.
- [ ] `orchidea` outputs a score instead of a plot.
- [ ] Playability checks.
- [ ] Examples as patches: Grisey (Partiels: an orchestral spectrum of a trombone note), Haas and
      Ligeti's Atmosphères (Musil's haas.mu and atmospheres.mu), played through the score.
- [ ] More examples for every orchestration mode and idea (mimetic, morphological, granular, queries,
      a sound file orchestrated over time, a score as target), and a tutorial on orchestration.

### Tutorials

The patches in `tutorials/` are numbered in the order they are taught; the code tutorial stays last.

- [ ] **Orchestration**: a tutorial patch before the code one (with the orchestration features).
- [ ] **Scores**: what a score is for and how it works (the score object, the transport, timeline and
      roll, scores from Musil), before the code one.

### Documents

Guides shipped as PDFs in `docs/` (Help > Documents), written as tutorials with patches to open
alongside, in the style of `docs/hybridization/` (LaTeX source next to the PDF), each finished with the
feature it describes:

- [ ] **Orchestration**: the topic taught through Moritz: targets, databases and queries, orchestras,
      the mimetic, morphological and granular modes, reading and editing the result as a score, the
      pieces (Grisey, Haas, Atmosphères) as worked examples.
- [ ] **The sound editor**: views, selection and edits, processing with objects, patches and Musil
      scripts, markers and regions, from sounds to scores.
- [ ] **Scores, timeline and roll**: the score as data and as an object, the transport, the timeline,
      the roll, generating and transforming, Musil and scores.
- [ ] **Multitrack**: recording, the mixer, arranging and rendering (with the multitrack mode).

### Later

- [ ] Multitrack mode, as in Reaper: recording into tracks (arming, several inputs, punch-in,
      latency compensation), long files streamed from disk, a mixer (faders, pan, mute and solo,
      sends and buses, meters), non-destructive clip edits (fades, crossfades, gain, stretch), tracks
      on several cores. Plug-ins (CLAP, VST3, AU) last, if ever: patches already are the effects.
- [ ] Musil's live functions played through Moritz's engine.
- [ ] Patches made from Musil: a `patch` library (make boxes and cables, open the patch, change the
      patch in front through the code view's path, with undo), as in mockup 13.
- [ ] A local language model that edits patches from text prompts (see below).
- [ ] MIDI and MusicXML export of scores, MusicXML import, notation; spectral painting in the sound
      editor; the patch dock (layout C).
- [ ] Larger Maple dictionaries as downloads; a live oracle (`oracle~`, OMax-style); a graph view of
      the types; a separate cepstrum object.

### Ideas to think through

- **A local language model for patches.** A patch is text (the code view), and changes to it already
  go through one path that keeps what did not change and can be undone (`Patch::applyText`). A prompt
  ("add a reverb after the oscillator") goes to a local model (llama.cpp embedded, or a server such as
  Ollama on the same machine, with the user's consent for the network), together with the patch's text
  and the reference of the objects it may use; the answer is parsed as untrusted input, shown as a
  change to accept, applied as one undo step. The same for Musil code in the code editor.
