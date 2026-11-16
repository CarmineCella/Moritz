# Moritz

<p align="center">
  <img src="resources/maxundmoritz.png" width="360" alt="Max and Moritz, drawn by Wilhelm Busch (1865)">
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
- **Sound files**: buffers, players, looping, recording, waveform views, and
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
- **Separation and learning**: non-negative matrix factorisation and
  harmonic/percussive separation, nearest neighbours, k-means, PCA and
  regression, to map gestures to sounds or to let the computer sort sounds by
  itself.
- **MIDI**: keyboards, controllers, notes in and out, polyphonic voices.
- **Max patches**: File > Import from Max opens `.maxpat` files, converting
  what Moritz knows and reporting the rest.

The complete list of objects is in [OBJECTS.md](OBJECTS.md).

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

- **Tutorials**: nine patches that explain themselves as you play with them:
  a first patch, numbers and messages, sound files, filters, the spectrum,
  grains, sound in space, separating sounds, listening machines.
- **Examples**, by topic: synthesis, samples, spectral processing, space,
  music information retrieval, learning, MIDI. Each one says what it shows
  at the top.
- **Objects**: every object, opening its help patch, a small working example
  with notes.

Turn audio on with the **Audio** button in the toolbar (Cmd+/), and use the
search box to find anything.

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
- **Editing**: select by clicking or dragging a rectangle; Option-drag copies;
  Cmd+X, Cmd+C, Cmd+V and Cmd+D cut, copy, paste and duplicate, also between
  patches; Cmd+Z undoes.
- **Help**: right-click any box and choose Help (or Option-click it in locked
  mode).
- **Code view**: Cmd+T shows the patch as text beside the drawing. Edit
  either and the other follows.
- **Subpatchers and abstractions**: `p name` holds a patch inside a box; any
  `name.moritz` file next to your patch, or in the library, becomes an
  object. Double-click to open them.
- **Several patches** play at once and share their `send`/`receive` and
  `buffer~` names, as in Max.
- **Console** (Shift+Cmd+M): what `print` prints, errors, audio and file
  events.
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

Moritz builds with `make`; the first run downloads FLTK 1.4.4, miniaudio and
RtMidi into `deps/`.

- **macOS** (Xcode command line tools, cmake, curl): `make` builds
  `Moritz.app`, a universal bundle; `make run` opens it.
- **Linux** (a C++17 compiler, cmake, curl, the ALSA and X11 development
  packages): `make` builds `build/linux/Moritz/moritz`. On Debian or Ubuntu:

  ```sh
  sudo apt install build-essential cmake curl libasound2-dev libx11-dev libxext-dev \
    libxft-dev libxinerama-dev libxcursor-dev libxrender-dev libxfixes-dev libfontconfig1-dev
  ```

- **Windows**: in the MSYS2 MINGW64 shell, after
  `pacman -S make curl zip mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake`,
  `make` builds `build/windows/Moritz/moritz.exe` (static, no DLLs). It can
  also be cross-compiled from a Mac or Linux with MinGW-w64: `make windows`.

`make dist`, `make linux-dist` and `make windows-zip` package each system.
`make notarize SIGN_ID="Developer ID Application: Name (TEAMID)"` signs and
notarizes the Mac app for distribution.

Every push to GitHub builds Moritz on all three systems and runs every
example, help patch, tutorial and library abstraction through
`tools/test-patches.sh`; a tag `v<version>` publishes a release. The same
test runs locally: `tools/test-patches.sh ./moritz`.

## Command line

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

The sounds in `examples/sounds/` come from the examples of Musil: a voice,
an excerpt of Beethoven's Seventh Symphony, drums, a guitar phrase, a bell, a
gong, two mixes for separation, and the impulse response of the
Concertgebouw in Amsterdam. For music information retrieval:
"The Stars and Stripes Forever" by John Philip Sousa, played by the United
States Marine Band (2017; public domain as a work of the U.S. government,
from the band's "Complete Marches of John Philip Sousa", via Wikimedia
Commons), and the preludes in C minor and D major of Bach's Well-Tempered
Clavier, Book 1, played by Kimiko Ishizaka (the Open Well-Tempered Clavier,
CC0, welltemperedclavier.org).

## License and credits

Moritz is released under the BSD 2-Clause License; see `LICENSE`. Created by
Carmine-Emanuele Cella (2026).

`hrtf~` and `binaural~` use the MIT KEMAR head-related impulse responses by
Bill Gardner and Keith Martin, MIT Media Laboratory (1994), "HRTF Measurements
of a KEMAR Dummy-Head Microphone".

The drawing of Max and Moritz is Wilhelm Busch's own, from *Max und Moritz*
(1865), in the public domain (scan from Wikimedia Commons, "Max und Moritz
(Busch) 001.png").
