# Moritz and Musil: rules for every change

Moritz (`src/`) is a Max-like patching environment; Musil (`musil/`) is its language, independent of
Moritz and shipped on its own too. The plan and the design decisions are in `ROADMAP.md`; technical notes
in `NOTES.md`. One CMake build makes both (`cmake -B build && cmake --build build`,
or `make`); `ctest --test-dir build` runs every test. These rules come from a full review of the code
(0.10b); a change that breaks one of them is not done.

## Real time
- The audio thread (`Box::dsp`, `Patch::process`, Musil's live callback) never allocates, frees, locks a
  mutex that another thread can hold for long, reads files, or calls the interpreter. Prepare buffers when
  the graph is built (`resolve`, compile); hand results back to another thread to be freed.
- Nothing heavy happens under the patch lock (`dspMutex`): build outside it, swap under it, destroy after
  releasing it. A message that only changes a name or a value must not recompile the whole graph.
- Values reach the audio thread through atomics or a single-producer queue, never through plain fields.

## Threads and lifetimes
- Musil has no mutable process-wide state: a cache or a table belongs to the `Interp`, or is
  `thread_local`, or is guarded by a mutex. Several interpreters run at once on threads (Moritz's code boxes).
- A callback that can run after its object is gone (timers, `Fl::awake`, jobs, interpreter output) checks
  that the object is still alive (an alive flag, `Fl_Widget_Tracker`, a token) or is removed in the destructor.
- A widget that schedules timeouts removes them in its destructor.

## Files and data
- Every save writes a temporary file in the same folder, checks the write and the close, then renames it.
- Undo, reload and code-view edits keep what did not change (`Patch::applyText`); nothing closes an edited
  script or patch without asking.
- Parsing untrusted input (patches, Max files, MIDI, CSV, WAV, Musil source) checks bounds and depth and
  never crashes; Musil turns every C++ exception into a Musil error.

## Safety
- What a script may do is decided by the interpreter's capability profile, checked inside each builtin that
  touches the system (files, programs, network) — never by removing names.
- A patch opened from elsewhere must not be able to change the machine without the user agreeing.

## Coherence
- One implementation per algorithm, in Musil (`musil/src/signals`, `music`, `scientific`): Moritz's objects
  call it. Nothing in `musil/` includes Moritz (`tools/check-musil.sh`).
- Random choices use Musil's portable random module (raw `mt19937` draws), never `std::*_distribution`:
  the same seed gives the same result on every system.
- An object that matches a Musil function has a cross-test in `tests/cross/` that prints `PASS` when they
  agree.

## Tests
- A bug fix comes with a test that fails without it. Threaded paths (code boxes running asynchronously, Stop,
  deleting a running box) are tested, not only `--render`.
- Run `ctest --test-dir build` before saying a change works; the sanitizer builds (`-DMORITZ_SANITIZE=address`
  or `thread`) must stay clean. Run them once at the end of each feature, and always after touching the audio
  engine or threads, not for each small fix (under TSan, `MUSIL_SKIP_TESTS=test_robust`).
- Tests and manual runs use a scratch `HOME`, `XDG_CONFIG_HOME` and `APPDATA`, never the user's preferences or
  `~/.musil`.

## Commits and releases
- Work happens on `main`, on macOS, validated by hand; work in parallel goes on short branches merged into
  `main` when ready. CI runs only for a release tag (`v*`) or when asked (`gh workflow run build.yml`), never to
  try a fix. A release: CI passes on all three systems, then the tag, then release notes written by hand.
- `ROADMAP.md` is kept current: a new request goes into it when it comes, an item is ticked when it lands.
- No `Co-Authored-By` or other assistant attribution in commits or pull requests; no plan "weeks" or "steps"
  in messages or release notes.
- `docs/` holds the shipped PDFs (committed); `tmp_work/` is scratch, never committed.
