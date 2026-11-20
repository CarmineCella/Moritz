// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_doc.h — a sound being edited, without the interface: its samples, where they come from (a file, or a
// buffer~ of the open patches), the selection and the playhead, the edits and their undo history.
//
// The samples are a snapshot that never changes (Sound, a shared pointer to a const SampleBuffer): an edit makes a
// new one. So the waveform's summary, the spectrogram computed on a worker thread and the preview the audio
// thread plays (preview.h) all read a snapshot of their own without a lock, and it lives as long as one of them
// still holds it.
//
// Undo keeps only what an edit changed: a step is a list of splices, each the frames it removed (and where) and
// what it put there. What a process (gain, fades, reverse ...) put there is not kept: it is made again from the
// removed frames when the step is redone. A paste keeps the clipboard's snapshot, shared. The history is bounded
// (undoBudget bytes; the oldest steps go first, the last one is always kept).
//
// Bound to a buffer~, the document changes the buffer~ in place for every object that uses it after each edit
// (BufferBox::replace: swapped under the patch lock, the old samples freed after it); a buffer~ is patch memory, so
// nothing is marked unsaved. When the buffer~ is replaced from elsewhere (read, size, clear), followBuffer() shows
// its new contents (the history, which describes the old ones, is cleared); so does a write of record~ or poke~
// into it (seen by the count of their writes, read now and then).
//
// The markers and regions (sound_marks.h) are a snapshot too. An edit of them is an undo step like an edit of the
// samples (the snapshots before and after); an edit of the samples moves them with the frames, in the same step.
// A file's are kept beside it (<file>.markers) and saved with it; a buffer~'s are kept with the buffer~ box.
#pragma once

#include "objects.h"
#include "sound_marks.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moritz {

using Sound = std::shared_ptr<const SampleBuffer>;

// the internal clipboard of samples: copy in one editor, paste in another (GUI thread)
Sound& soundClipboard();

// frames [start, end); empty when end <= start
struct Selection {
    long start = 0, end = 0;
    long length() const { return std::max(0L, end - start); }
    bool empty() const { return end <= start; }
};

// The waveform at every zoom: per channel, the minimum and maximum of blocks of kBase frames, and of blocks
// kFactor times larger at each level above. A pixel column reads a few blocks of the level that fits it.
class WaveSummary {
public:
    static constexpr long kBase = 64;
    static constexpr int kFactor = 4;
    void build(const SampleBuffer& s);
    // the lowest and highest sample of channel c over frames [a, b) (about: a column may read a block more)
    void range(const SampleBuffer& s, int c, long a, long b, float& lo, float& hi) const;

private:
    struct Level {
        long block = 0;
        std::vector<float> mn, mx;      // [block index * channels + c]
    };
    std::vector<Level> levels_;
    int ch_ = 1;
};

// The magnitude spectrogram of a sound (its channels mixed), on a logarithmic frequency axis: cols frames of rows
// values, 0 (-100 dB or less) to 255 (0 dB, a full-scale sine). Frame k is centred on frame k * hop of the sound.
struct Spectrogram {
    int cols = 0, rows = 0, n = 0;
    long hop = 1;
    double sr = 44100.0, fmin = 40.0, fmax = 22050.0;
    std::vector<unsigned char> v;       // [col * rows + row], row 0 the lowest frequency
    double rowFreq(double row) const { return fmin * std::pow(fmax / fmin, row / std::max(1, rows)); }
};
// Musil's STFT frame by frame (mir::FrameSpectrum, Hann n), at most maxCols frames; tick(0..1) returns false to
// stop (false is returned). Runs on a worker thread: it reads only s.
bool computeSpectrogram(const SampleBuffer& s, Spectrogram& out, const std::function<bool(double)>& tick = {},
                        int maxCols = 8192, int rows = 256);

class SoundDoc {
public:
    SoundDoc();

    // ---- the samples
    const Sound& sound() const { return sound_; }
    long frames() const { return sound_->frames; }
    int channels() const { return sound_->channels; }
    double sr() const { return sound_->sr; }
    double seconds(long frame) const { return sr() > 0 ? frame / sr() : 0.0; }
    const WaveSummary& summary() const { return summary_; }
    uint64_t generation() const { return generation_; }     // grows with every change of the samples
    // new contents (a sound made elsewhere): the history is cleared, the selection kept where it still fits
    void setSound(SampleBuffer&& b);

    // ---- a file
    bool load(const std::string& path, std::string& err);   // the document is that file
    const std::string& path() const { return path_; }
    bool savesInPlace() const;                               // its file is a WAV file (Save writes it)
    bool save(std::string& err);                             // to path(): a temporary file, renamed
    bool saveTo(const std::string& path, std::string& err);  // Save As: the document's file is then path
    bool exportTo(const std::string& path, std::string& err) const;   // a copy (buffer~: Save as WAV)
    bool dirty() const { return topSerial() != savedSerial_; }   // undone back to the file: not dirty
    // closing it loses something: a file's (or a new sound's) unsaved edits; never a buffer~'s
    bool needsSaving() const { return dirty() && !bound_; }
    void markUnsaved() { savedSerial_ = ~uint64_t(0); }      // a new sound made in the editor (a process's result)

    // ---- a buffer~ of the open patches (root: the patch the names live in)
    void bindBuffer(Patch* root, const std::string& name);  // its samples now, copied
    bool boundToBuffer() const { return bound_; }
    const std::string& bufferName() const { return bufName_; }
    BufferBox* buffer() const;                               // nullptr: no buffer~ of that name now
    // the buffer~ was replaced from elsewhere (read, size, clear) or is another box now, or (writes) record~ or
    // poke~ wrote into it: its samples again, the history cleared; true if so
    bool followBuffer(bool writes = true);
    // the boxes that name the buffer~ (play~ groove~ ...), and in how many patches
    std::vector<std::string> users(int* patches = nullptr) const;

    // ---- where one is (frames); others (the editor, later the Process menu) read and set them
    Selection sel;
    long playhead = 0;
    void select(long a, long b);        // in any order, clamped to the sound
    void selectAll() { select(0, frames()); }

    // ---- edits on the selection: one undo step each; false when nothing was done (message says why)
    bool copy();                        // to the clipboard
    bool cut();
    bool paste();                       // over the selection, or at the playhead when none
    bool erase();
    bool trim();                        // keeps only the selection
    bool silence();
    bool fadeIn();
    bool fadeOut();
    bool gain(double db);
    bool normalize(double db = 0.0);    // its peak to db
    bool reverse();
    // a process's result in place of the selection (all of the sound when nothing is selected): its channels and
    // rate made the sound's (Musil's resample), one undo step; the result is selected
    bool replaceSelection(const std::string& name, const SampleBuffer& result);
    // frames (interleaved, as many channels as the sound) from frame at, in place of as many (the Draw tool's
    // stroke): one undo step
    bool replaceFrames(const std::string& name, long at, const std::vector<float>& frames);
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoName() const { return undo_.empty() ? "" : undo_.back().name; }
    std::string redoName() const { return redo_.empty() ? "" : redo_.back().name; }
    size_t historyBytes() const;        // what undo and redo keep
    static size_t undoBudget;           // bytes the history may keep (the last step is kept whatever its size)

    // ---- markers and regions
    const SoundMarks& marks() const { return *marks_; }
    const MarksPtr& marksPtr() const { return marks_; }
    bool setMarks(const std::string& name, SoundMarks next);   // one undo step; false when nothing changed
    uint64_t marksGeneration() const { return marksGen_; }
    bool marksUnreadable() const { return keepSidecar_; }      // its .markers file could not be read (kept as it is)

    std::string message;                // what the last operation did, or why it did nothing
    std::function<void()> onChanged;    // the samples changed (an edit, undo, a reload)
    std::function<void()> onMarks;      // the markers or regions changed (an edit of them, of the samples, undo)

private:
    struct Splice {
        long at = 0;
        long removedFrames = 0;
        std::vector<float> removed;                         // interleaved
        long insertedFrames = 0;
        std::shared_ptr<const std::vector<float>> inserted;   // or made again by regen from removed
        std::function<void(std::vector<float>&)> regen;
    };
    struct Step {
        std::string name;
        uint64_t serial = 0;
        std::vector<Splice> splices;    // applied in order (undone in reverse)
        Selection selBefore, selAfter;
        long playBefore = 0, playAfter = 0;
        MarksPtr marksBefore, marksAfter;
        size_t bytes() const;
    };
    // the sound with frames [at, at + n) replaced by ins (insFrames frames), as a new snapshot
    Sound spliced(const Sound& s, long at, long n, const float* ins, long insFrames) const;
    bool needSelection(const char* what);
    // an edit that changes the selection's samples in place: op on a copy of them
    bool process(const std::string& name, std::function<void(std::vector<float>&)> op);
    void commit(Step&& st, Sound next);                     // a new step done
    void setMarksNow(MarksPtr m);                           // the marks shown: the buffer~ keeps them, listeners
    bool writeSidecar(const std::string& soundPath, std::string& err) const;
    void changed(Sound next);                               // new samples: summary, buffer~, listeners
    void trimHistory();
    Sound sound_;
    WaveSummary summary_;
    uint64_t generation_ = 0;
    std::vector<Step> undo_, redo_;
    std::string path_;
    uint64_t nextSerial_ = 1, savedSerial_ = 0;   // each step its number; the one the file holds
    uint64_t topSerial() const { return undo_.empty() ? 0 : undo_.back().serial; }
    bool bound_ = false;
    Patch* root_ = nullptr;
    std::string bufName_;
    BufferBox* seenBox_ = nullptr;      // the buffer~ and its version we last showed or wrote
    int seenVersion_ = -1;
    unsigned seenWrites_ = 0;           // its writes by record~ and poke~, then
    MarksPtr marks_;
    uint64_t marksGen_ = 0;
    bool keepSidecar_ = false;          // the file's .markers could not be read: not overwritten with nothing
};

}  // namespace moritz
