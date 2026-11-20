# Moritz

<p align="center">
  <img src="resources/moritz.png" width="200" alt="The Moritz logo: a Lissajous figure on an oscilloscope over a harmonic spectrum">
</p>

Moritz is a Max-like environment for sound and music computing. You build
patches by placing boxes on a canvas and connecting them with cables, and you
hear the result as you play with them. It works the way Max does, so what
you know from Max carries over, and it runs on macOS, Linux and Windows.

Moritz was made for teaching. Every object is a short piece of readable C++
with no dependencies beyond three libraries (FLTK for the interface,
miniaudio for audio, RtMidi for MIDI), so students can open the box and see
how a phase vocoder, a granulator or k-means actually works. It brings
together, in one free and open system, the research tools of its author:
the *sparkle* phase vocoder, *condec* convolution, the *nebula* granulator,
and the signal processing, analysis and learning of *Musil*.

## What you can do with it

- **Synthesis**: oscillators, noise, FM, granular synthesis (`nebula~`, and a
  granulator built from `poly~`), polyphonic instruments.
- **Sound files**: buffers, players, looping, recording, a sound editor
  (waveform and spectrogram, selection, cut and paste, fades, undo), and
  offline rendering of whole patches into sound.
- **Filters and effects**: resonant and state-variable filters, biquads
  designed and drawn on screen, delays, a reverb, zero-latency convolution
  with any impulse response.
- **The spectrum**: a phase vocoder that stretches time and shifts pitch
  while keeping the formants, cross-synthesis between two sounds, spectral
  envelopes, and the building blocks to make your own (`pfft~`).
- **Space**: ambisonics up to order 5, decoding to speakers, and 3D sound on
  headphones from measured head responses.
- **Listening machines**: descriptors, onsets, tempo and beats, key,
  chromagrams and MFCCs, the form of a piece, live or on recordings, drawn
  by `plot`.
- **Atoms and types**: matching pursuit explains a sound as a sum of atoms
  (windowed cosines, or pieces of other recordings: a voice made of kitchen
  sounds); sound types turn a recording into sequences of symbols at three
  levels, from short sounds to sections, and make new music from them with
  n-grams, Markov models or a factor oracle.
- **Separation and learning**: non-negative matrix factorisation and
  harmonic/percussive separation, nearest neighbours, k-means, PCA and
  regression, to map gestures to sounds or to let the computer sort sounds by
  itself.
- **MIDI**: keyboards, controllers, notes in and out, polyphonic voices.
- **Max patches**: File > Import from Max opens `.maxpat` files, converting
  what Moritz knows and reporting the rest.
- **Programming with Musil**: what is easier to write than to draw, you
  write in Musil, the scripting language of Moritz, at the Console, in the
  code editor, or in a `code` box inside a patch (see
  [Programming inside Moritz](#programming-inside-moritz-with-musil)).

The complete list of objects is in [OBJECTS.md](OBJECTS.md), and as a PDF in
Help > Documents.

## Programming inside Moritz with Musil

Some things are easier to write than to draw: loops, lists of lists,
matrices, a search through a thousand sounds. For those, Moritz has a
scripting language, **Musil**: a small Lisp whose numbers are vectors, with
libraries for offline signal processing and analysis (STFT, phase vocoder,
filters, onsets, spectral descriptors, convolution, ambisonics), statistics
and learning (matrices, PCA, k-means, kNN, NMF), and music (notes, chords,
rhythms, scores, MIDI files, MusicXML, orchestration with sample databases).

- **Where you write it**: in a `code` box inside a patch, in the code
  editor (File > New Code, or open any `.mu` file), or at the Console's
  prompt, one line at a time. In the editor Cmd-Enter runs the paragraph at
  the cursor, Shift-Cmd-Enter the whole script, Tab completes, and the help
  of the name under the cursor is beside the text. What a script prints goes
  to the Console.
- **The `code` box**: `code [inlets] [outlets] [file.mu]` runs its script off
  the audio thread when it gets a bang, in an interpreter of its own. Its
  inlets set the variables `in1`, `in2`...; `out 1 values` sends out of its
  first outlet; its last outlet bangs when the script has run. The script is
  kept in the patch, or in a `.mu` file next to it; double-click the box to
  edit it. A box reads files anywhere and writes them in its patch's folder
  and in `~/Documents/Moritz`; it does not run programs, use the network,
  wait or quit, unless it is marked `@trusted` and you allow its folder.
- **How it talks to your patches**: `buffer "name"` gives the channels of a
  `buffer~` as vectors, `set-buffer! "name" channels [rate]` fills it (a
  `waveform~` shows it at once, a `play~` plays it), and `send "name" values`
  reaches every `receive name`.
- **Files**: a patch is a `.moritz` file, a script a `.mu` file. A script
  loads the libraries it uses: `load "music.mu"` (a code box starts with the
  signals library loaded).

For example, a `code` box that transforms the drums of a `buffer~ demo
drums.wav` into a `buffer~ out`, with the gain from its left inlet:

```lisp
; reverse the drums, darken them, fade out, and put them in out
var x (head (buffer "demo"))              ; the first channel, as a vector
var sr (buffer-rate "demo")
var y (normalize-peak (fade-out (lowpass (reverse x) sr 800 0.7) 4410))
set-buffer! "out" (* in1 y) sr
out 1 (length y)                          ; the number of samples, out of outlet 1
```

Tutorial 11 goes from the Console to the code box; `examples/12-code` has
sounds transformed, analysed and made by scripts, and Maple and SoundTypes
from code; `help/code.help.moritz` sums it up. The Musil manual is in Help >
Documents.

**Musil on its own.** Musil is the language its author wrote before Moritz,
and it lives in [`musil/`](musil/), complete and independent: nothing in it
uses Moritz (CI checks it). On its own (`musil` in the downloads, or
`build/musil/musil`) it also has its live audio engine, plot windows, sliders and
score roll; inside Moritz those are left out, since Moritz plays and draws,
and a script renders into a buffer that a patch plays. Musil is the
reference: where an object and a Musil function do the same thing, the object
runs Musil's code (the analyses, Maple, SoundTypes, k-means and NMF; the phase
vocoder of `sparkle~`, the grains of `nebula~`, `svf~`, `lores~`, `conv~`,
`condec` and the ambisonic objects), and tests check that each pair agrees.

## The name

Max, the patching environment Moritz follows, was named after Max Mathews,
the pioneer of computer music. In German, Max never goes far without Moritz:
*Max und Moritz* (1865), Wilhelm Busch's story of two boys and their seven
pranks, is one of the best-known picture books in the language. Moritz is
Max's companion: it works the way Max does, and gets up to some mischief of
its own. (Busch's two boys are hiding somewhere in the program, too.)

## Getting started

Download Moritz for your system from the
[releases](https://github.com/CarmineCella/Moritz/releases), unzip it and open
it. On macOS, a downloaded app is blocked the first time: open it once, then
click **Open Anyway** in System Settings > Privacy & Security (or run
`xattr -dr com.apple.quarantine Moritz.app`).

Moritz opens on the **browser**, the place to start:

- **Tutorials**: eleven patches that explain themselves as you play with
  them: a first patch, numbers and messages, sound files, filters, the
  spectrum, grains, sound in space, separating sounds, listening machines,
  atoms and types, programming with Musil.
- **Examples**, by category (the folders of `examples/`): first steps,
  synthesis, generative music, filters and effects, samples, spectral
  processing, space, atoms and types, analysis and MIR, learning, MIDI, code.
  Each one says what it shows at the top. Musil's own examples are in
  `musil/examples`.
- **Objects**: every object with its reference (arguments, messages, inlets,
  outlets), opening its help patch, a small working example with notes.

Turn audio on with the **Audio** button in the toolbar (Cmd+/), and use the
search box to find anything. The toolbar also zooms the patch (Cmd+ and Cmd-,
Cmd+0 for 100%), opens the browser and the console, records what you hear
(the quick recorder) and shows the CPU load and the output level.

Help > Documents opens the PDFs: the reference of every object, the Musil
manual, and "Sound decomposition strategies for creative hybridizations", on
the methods of the atoms and types objects.

## Patching

Moritz follows Max's conventions:

- **Edit and locked mode**: **Cmd+E** switches. In edit mode you build; in
  locked mode you play (click buttons and messages, drag sliders and
  numbers). New patches start in edit mode, opened ones locked.
- **Making boxes**: double-click the canvas and type (`cycle~ 440`); a list of
  matching objects helps you complete it. Shortcuts with the mouse over the
  canvas: **n** object, **m** message, **f** and **i** number boxes, **b**
  button, **t** toggle, **c** comment. Patch > New lists every object by
  category.
- **Connecting**: drag from an outlet (the bottom edge of a box) to an inlet
  (the top edge). Rest the mouse on a port to see what it expects or sends.
  To move a cable, take it near one of its ends and drop that end on another
  box: it goes to that box's nearest inlet (or comes from its nearest
  outlet); dropped anywhere else, the cable stays as it was. Hold Shift while
  you drag a box over a cable and the cable lights up: drop the box there and
  it goes into the cable (A → B becomes A → box → B, through its left inlet and
  left outlet), when it has both and they suit the cable (a signal can't go
  into a control inlet).
- **Editing**: select by clicking or dragging a rectangle; Option-drag copies;
  Cmd+X, Cmd+C, Cmd+V and Cmd+D cut, copy, paste and duplicate, also between
  patches; Cmd+Z undoes.
- **Settings** (Moritz > Settings, Cmd+,; Edit > Settings on Linux and
  Windows): the font and size of the boxes' text and of code (the code
  editor, the Console, the code view), and curved or straight cables.
- **Help and reference**: right-click any box and choose Help (or
  Option-click it in locked mode) for a working example, or Reference for
  every argument, message, inlet and outlet. Help windows have a Reference
  button too. A help patch opens locked, as a patch of its own: play with it,
  or unlock it (Cmd+E) to change it or to copy its boxes into your patch. Save
  asks where to keep your version (the help patch itself is never written
  over), and closing it asks only when you edited it, not when you played.
- **Right-click** also offers what a box can do: load or save a `buffer~`,
  open a `plot`'s window or export it, choose the file of `sfplay~` and
  `sfrecord~`.
- **Sound editor**: double-click a `buffer~` (locked), or any object that
  names one, to edit its sound; File > Open Sound... does the same for a
  sound file. It shows an overview, a regions lane and a markers lane, a
  waveform per channel and the spectrogram (Wave, Spectrum or Both), with a
  time ruler; Cmd+ and Cmd- (or Cmd and the wheel, or a trackpad's pinch)
  zoom, the wheel or the overview scrolls. Drag to select, Shift-click to
  extend, click to set the playhead; Space plays the selection (or from the
  playhead) through the audio output, beside the patches, and Cmd+L loops
  it. Edit (or the right button) cuts, copies, pastes, deletes, trims,
  silences, fades in and out, changes the gain, normalizes and reverses, each
  edit one step of Undo; the Draw tool moves single samples when zoomed in.
  Process runs an offline object over the selection (pursuit, typematch,
  hpss, nmf, pvoc, sparkle~'s cross-synthesis, filterdesign's filters,
  condec), a patch with `inlet~` and `outlet~`, or a Musil script that
  defines `(process x sr)` (examples/14-sound-editor), in the background; the
  result replaces the selection or opens as a new sound. Markers come by hand
  (M, the Marker tool) or from onsets, beats and novelty; regions from the
  selection, soundtypes' units or onsets, listed in a table (rename, click to
  select, To coll... writes them as rows). A `buffer~` changes at once for
  every object that uses it (the status line says which); a sound file is
  saved back as a 24-bit WAV file with its markers beside it
  (`sound.wav.markers`), and closing it asks when it has edits. Samples
  copied in one editor paste into another (resampled when the rates differ).
- **Zoom**: Cmd+ and Cmd- (or the toolbar) zoom the patch; View > Interface
  makes the whole interface larger or smaller.
- **Code view**: Cmd+T shows the patch as text beside the drawing. Edit
  either and the other follows.
- **Subpatchers and abstractions**: `p name` holds a patch inside a box; any
  `name.moritz` file next to your patch, or in the library, becomes an
  object. Double-click to open them.
- **Several patches** play at once and share their `send`/`receive` and
  `buffer~` names, as in Max.
- **Console** (Shift+Cmd+M): what `print` prints, errors, audio and file
  events. Its toolbar button shows a red dot after an error.
- **Quick recorder** (Extras, or the toolbar's Record button): records what
  goes to the speakers into a 24-bit WAV file in Music/Moritz Recordings.
- **Links**: a `link` box opens a document, a folder or a web page with a
  click.
- **Multichannel**: the sound card opens with all its channels, so
  `dac~ 1 2 3 4 5 6 7 8` plays on eight speakers and ambisonic decoders
  reach every speaker (Audio Setup can limit the channels). A patch can also
  use more channels than the card has: they are metered and recorded by the
  quick recorder, and rendered offline, just not heard.
- **Binaural monitor** (Extras): listen to a multichannel or ambisonic
  piece on headphones. Every output channel becomes a speaker around you
  (a ring, or the directions you type), rendered with the MIT KEMAR head
  responses on channels 1 and 2.
- **Channel tester** (Extras): bursts of noise on one channel at a time, or
  stepping through all of them, to find which speaker is which.
- **Audio and MIDI**: Patch > Audio Setup picks the devices; Moritz listens
  to every MIDI input and, on macOS and Linux, offers a virtual output port
  called "Moritz" (on Windows, Patch > MIDI Setup picks an output device).
- **Your own library**: put abstractions (and their `.help.moritz`) in
  `~/Documents/Moritz/library` and they work like built-in objects.

Numbers follow Max's types: an argument written with a point (`2.`) makes an
object work in decimals, an integer argument or none in integers, so `7 / 2`
is 3 and `7 / 2.` is 3.5.

A patch is a text file that is easy to read and to write by hand:

```lisp
(patch
  (obj osc  cycle~ 440 (pos 40 130))
  (obj gain *~ 0.2     (pos 40 180))
  (obj out  dac~       (pos 40 240))
  (connect osc 0 gain 0)
  (connect gain 0 out 0)
  (connect gain 0 out 1))
```

## Building from source

One CMake build (`CMakeLists.txt`) makes Moritz and Musil on every system; the
first configure downloads FLTK 1.4.4, miniaudio and RtMidi into `deps/`, and
FLTK is built with the program.

```sh
cmake -B build && cmake --build build -j8     # or just: make
ctest --test-dir build                        # every test (make test)
cmake --build build --target dist             # the download of this system (make dist)
cmake --install build                         # Moritz and Musil into the system (make install)
```

- **macOS** (Xcode command line tools, cmake): `build/Moritz.app`, a universal
  bundle, and `build/musil/musil`; `make run` opens Moritz.
- **Linux** (a C++17 compiler, cmake, the ALSA and X11 development
  packages): `build/Moritz/moritz`, with Musil in `build/Moritz/musil/`. On Debian or Ubuntu:

  ```sh
  sudo apt install build-essential cmake curl libasound2-dev libx11-dev libxext-dev \
    libxft-dev libxinerama-dev libxcursor-dev libxrender-dev libxfixes-dev libfontconfig1-dev
  ```

- **Windows**: in the MSYS2 MINGW64 shell, after
  `pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake`,
  `make` (or `cmake -G "MSYS Makefiles" -B build`) builds `build/Moritz/moritz.exe`
  (static, no DLLs). It can also be cross-compiled from a Mac or Linux with
  MinGW-w64: `make windows` (`cmake -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`).

`cmake --install` puts Moritz in `/Applications` (macOS), `<prefix>/lib/moritz`
with `<prefix>/bin/moritz` (Linux) or `<prefix>/Moritz` (Windows), and Musil in
`<prefix>/bin/musil` with its libraries in `~/.musil`. Musil also builds on its
own, from `musil/` (`cmake -S musil -B build-musil`).
`make notarize SIGN_ID="Developer ID Application: Name (TEAMID)"` signs and
notarizes the Mac app for distribution.

Every push to GitHub builds Moritz and Musil on all three systems, runs every
example, help patch, tutorial, library abstraction and cross-test (an object
against Musil's function) through `tools/test-patches.sh`, and runs Musil's
tests through `tools/test-musil.sh`; a tag `v<version>` publishes a release.
The same tests run locally: `make test`.

## Command line

- `moritz --run script.mu [-- args...]` runs a Musil script without opening
  a window; `moritz --repl` gives a Musil prompt in the terminal. A script
  names the libraries it needs: `load "music.mu"` loads `musil/src/music.mu`.
  For Musil's own audio, plots and score roll, use `musil script.mu` (Musil
  on its own).
- `moritz --no-audio [patch...]` opens with audio off, for a machine whose
  audio device misbehaves.
- `moritz --import patch.maxpat [out.moritz]` converts a Max patch, as File >
  Import from Max does.
- `moritz --render patch.moritz seconds [out.wav] [actions...]` runs a patch
  offline, faster than real time and without a sound card, prints the level
  of each channel and can write the sound to a file. Actions:
  `bang=<id>`, `send=<id>:<message>` (both can wait: `bang=go@4.5`),
  `midi=<seconds>:<bytes>` (a MIDI message in), `png=<file>` (draws the patch
  as it looks after the render; `edit` draws it in edit mode),
  `extra=<patch>` (runs a second patch alongside, as help patches run).
  A box inside a subpatcher is named as `p1/osc`.

## Sounds

The sounds and data in `data/` serve Moritz's patches and Musil's examples
alike. Most come from the examples of Musil: a voice,
an excerpt of Beethoven's Seventh Symphony, drums, a guitar phrase, a bell, a
gong, two mixes for separation, and the impulse response of the
Concertgebouw in Amsterdam. For music information retrieval:
"The Stars and Stripes Forever" by John Philip Sousa, played by the United
States Marine Band (2017; public domain as a work of the U.S. government,
from the band's "Complete Marches of John Philip Sousa", via Wikimedia
Commons), and the preludes in C minor and D major of Bach's Well-Tempered
Clavier, Book 1, played by Kimiko Ishizaka (the Open Well-Tempered Clavier,
CC0, welltemperedclavier.org). The kitchen and people sounds in
`data/kitchen` and `data/people`, used as dictionaries
for matching pursuit, are the author's recordings. The clarinet notes in
`data/clarinet` (B-flat clarinet, ordinario, every pitch at pp,
mf and ff, shortened to 1.5 s) come from TinySOL (Cella, Ghisi, Lostanlen,
Lévy, Fineberg and Maresz, IRCAM, CC BY 4.0). Short excerpts of John Cage
speaking and of a Beach Boys song are used for the rematch example.

### Datasets

The sample databases that `orchestra`, `orchidea` and Musil's music library
use (Ircam's TinySOL, OrchideaSOL, FullSOL: a feature file `X.spectrum.db`
next to a folder `X/` of sounds) are found by name, the same way in Moritz
and in Musil: `orchestra band fullsol`, `(db-load "tinysol")`. A name is
looked for in a file registered under it, then in the datasets folder
(`~/Documents/Moritz/datasets` unless you choose another in Settings), then
in MicroSOL, which comes with Moritz (`data/microsol`: oboe, horn, violin,
cello around C4-G4, 32 sounds). Extras > Datasets... lists them and where
they are; it downloads TinySOL (about 750 MB, from Musil's release assets,
its checksum verified) into the datasets folder, registers a feature file
that lives elsewhere (FullSOL, where you keep it: choose its `.spectrum.db`
once), and analyses a folder of your own sounds, named as in the *SOL
databases (`Vn-ord-C4-mf.wav`), into a feature file next to it, registered
by the name you give (Orchidea's dbgen; `orchestra`'s `analyse folder` does
the same from a patch). The configuration is one small file both read,
`~/.musil/datasets.mu`.

## License and credits

Moritz is released under the BSD 2-Clause License; see `LICENSE`. Created by
Carmine-Emanuele Cella (2026).

`hrtf~` and `binaural~` use the MIT KEMAR head-related impulse responses by
Bill Gardner and Keith Martin, MIT Media Laboratory (1994), "HRTF Measurements
of a KEMAR Dummy-Head Microphone".

The drawing of Max and Moritz (Shift+Cmd+J in Moritz) is Wilhelm Busch's own, from *Max und Moritz*
(1865), in the public domain (scan from Wikimedia Commons, "Max und Moritz
(Busch) 001.png").
