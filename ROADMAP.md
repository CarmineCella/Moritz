# Roadmap

What is asked for and not done yet, kept up to date as work lands. The design behind it is in
[mockups/README.md](mockups/README.md) (decisions and plan).

## 0.12 (in progress, on main)

- [x] Sound editor core: overview, waveform, spectrogram, zoom, selection, preview, edits with undo, save.
- [ ] Sound editor, second part: Process menu (offline objects, a Moritz patch, a Musil script),
      markers and regions (by hand, from onsets, beats, novelty, soundtypes units), regions table,
      To coll, the pencil (move single samples by hand), Cmd+L for loop, tool modes.
- [x] Orchidea in Moritz: `orchestra` and `orchidea`, mimetic mode.
- [x] Score foundation: the score in Musil's C++, .score files as Musil data, `transport`, `score`.
- [ ] One datasets folder for Moritz and Musil: FullSOL, TinySOL, MicroSOL by name; a Datasets window;
      TinySOL download; dbgen ("Analyse a folder…") without code.
- [ ] The outlets of `orchestra` and `orchidea` described in the reference and tooltips.
- [x] Musil's live functions in Moritz say why they are missing.
- [ ] Musil's plots as a host service in Moritz (`show`, `save-png`), one renderer with `plot`.
- [ ] Musil's examples run in Moritz's tests too (those that can).

## Orchestration

- [ ] **Sample selection by query**: one small language to choose a subset of a database's samples,
      written once in Musil's C++ and used by `orchestra` and by Musil (`db-select`). Fields from the
      database: instrument, family, technique (playing style), dynamics, pitch, string, mute and the
      other tags in the file names, and the analysed features. Patterns (`pizz*`, `/sul_?pont/`),
      ranges (`C4..G5`, `p..f` in the order of dynamics, `pitch 60..72`), lists (`Vn|Va`), and, or,
      not; per instrument or for the whole orchestra. Shows how many samples remain before a run.
- [ ] Orchidea's morphological and granular modes, and the adaptive segmentation, in C++ (today
      about 800 lines of music.mu), producing a score.
- [ ] `orchidea` outputs a score (the shared C++ score) instead of a plot.
- [ ] Examples as patches: Grisey (Partiels: an orchestral spectrum of a trombone note), Haas and
      Ligeti's Atmosphères (Musil's haas.mu and atmospheres.mu), played through the score.
- [ ] More examples for every orchestration mode and idea (mimetic, morphological, granular, queries,
      orchestrating a sound file over time, a score as target), and a tutorial on orchestration in
      `tutorials/`.

## 0.13 and 0.14

- [ ] Timeline (mockup 4), designed for a multitrack mode.
- [ ] Roll and the active `score` object (mockups 5 and 2); Generate and Transform panel; `coll`.

## Later

- [ ] Multitrack mode, as in Reaper: recording into tracks, disk streaming, mixer, non-destructive
      clip edits, tracks on several cores; VST/AU last, if ever.
- [ ] Musil's live functions played through Moritz's engine.
- [ ] Patches made from Musil: a `patch` library (make boxes and cables, open the patch, change the
      patch in front through the same path as the code view, with undo), as in mockup 13.
- [ ] A local language model that edits patches from text prompts (see below).
- [ ] Notation, MusicXML import, spectral painting, the patch dock.

## Ideas to think through

- **A local language model for patches.** A patch is text (the code view), and changes to it already
  go through one path that keeps what did not change and can be undone (`Patch::applyText`). A prompt
  ("add a reverb after the oscillator") goes to a local model (llama.cpp embedded, or a server such as
  Ollama on the same machine, with the user's consent for the network), together with the patch's text
  and the reference of the objects it may use; the answer is parsed as untrusted input, shown as a
  change to accept, applied as one undo step. The same for Musil code in the code editor.
