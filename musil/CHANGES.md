# Changes

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
