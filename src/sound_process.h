// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_process.h — what the sound editor's Process and Markers menus run: the engines of Moritz's offline objects
// over a part of a sound, and the analyses that place markers and regions.
//
// Every engine is the one the object runs (and Musil's, where the object calls Musil): pursuit and typematch are
// Musil's Maple and SoundTypes, hpss Musil's hpss, nmf the nmf object's separation (Musil's nmf), pvoc and
// sparkle~'s cross-synthesis Musil's phase vocoder (dsp::pvoc), the filter filterdesign's RBJ biquad run by Musil's
// iir, condec Musil's convolution; onsets, beats and novelty are the onsets, beats and novelty objects' analyses
// (Musil's mir.h), the regions from types the soundtypes object's units. Nothing here is a second implementation.
//
// They run on a job's thread (jobs.h) and read only what they are given: a copy of the selection (in), and for
// some a snapshot of another sound or a dictionary taken on the GUI thread before the job starts.
//
// Two processes run something the user chose from a file:
//   - a patch (PatchProcess): a .moritz patch with inlet~ and outlet~, rendered offline as the render object
//     renders one: it is loaded (on the GUI thread) inside a patch of its own whose adc~ plays the selection
//     into its inlets and whose dac~ takes its outlets; the job runs the blocks (renderBlocks, as render).
//   - a Musil script (ScriptProcess): a .mu file that defines (process x sr), x a list of channel vectors and
//     sr the rate, and returns the processed sound, a vector (every channel the same) or a list of channel
//     vectors, at the same rate. It runs in an interpreter of its own on a thread with a large stack, with a
//     code box's permissions (no programs, no network; files written only under ~/Documents/Moritz).
#pragma once

#include "jobs.h"
#include "maple.h"
#include "sound_doc.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace moritz {

class LangContext;

// ---- the Process menu
struct ProcessField {
    std::string key, label, def;
    std::vector<std::string> choices;    // empty: a number
    std::string tip;
};
struct ProcessInfo {
    std::string kind;                    // pursuit typematch hpss nmf pvoc cross filter condec
    std::string menu;                    // "pvoc: stretch or transpose"
    std::string title;                   // the dialog's title
    std::vector<ProcessField> fields;
    bool other = false;                  // it needs another sound (the field "with")
};
const std::vector<ProcessInfo>& processKinds();
const ProcessInfo* processInfo(const std::string& kind);

struct ProcessSpec {
    std::string kind;
    std::map<std::string, std::string> p;           // the dialog's answers (the defaults for what is missing)
    double num(const std::string& k) const;         // a number field (its default when missing or not a number)
    std::string word(const std::string& k) const;
    Sound other;                                    // typematch, cross, condec: the other sound
    std::shared_ptr<const maple::Dictionary> dict;  // pursuit with an atoms object's dictionary
};

// in (the selection) processed into out, on the job's thread; false with err when it can't, or was stopped
bool runProcess(const ProcessSpec& s, const SampleBuffer& in, SampleBuffer& out, std::string& err, Job& job);

// interleaved frames resampled by Musil's resample (windowed sinc) by factor (new length / old)
std::vector<float> resampleFrames(const std::vector<float>& x, int channels, double factor);

// ---- a patch, rendered offline over the selection
class PatchProcess {
public:
    PatchProcess() = default;
    ~PatchProcess();                     // GUI thread, its job stopped
    // GUI thread: the patch read and set up around the selection in (tail: seconds more, for echoes); false
    // with err (no inlet~ or outlet~, can't read it ...)
    bool prepare(const std::string& path, const SampleBuffer& in, double tail, std::string& err);
    void run(Job& job);                  // the job's thread
    // GUI thread, after the job: the sound it made, what it printed; the patch is destroyed
    SampleBuffer take(std::vector<std::string>& prints);

private:
    std::unique_ptr<Patch> patch_;
    std::shared_ptr<long> frame_ = std::make_shared<long>(0);
    std::vector<float> in_;
    int inCh_ = 1;
    long total_ = 0;
    double sr_ = 44100;
    std::vector<std::vector<float>> out_;
    std::shared_ptr<std::pair<std::mutex, std::vector<std::string>>> prints_;
};

// ---- a Musil script that defines (process x sr)
class ScriptProcess {
public:
    ScriptProcess();
    ~ScriptProcess();
    // starts it (GUI thread): done(ok, the sound or the error) on the GUI thread, unless stop() came first
    bool start(const std::string& path, const SampleBuffer& in, std::function<void(bool, SampleBuffer, std::string)> done);
    void stop();                         // the script stops at its next step; done is not called
    bool running() const;

private:
    std::shared_ptr<LangContext> ctx_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

// ---- markers and regions (the job's thread; frames of in)
struct MarkOptions {
    double threshold = 2.0;              // onsets: the flux over its median
    double gap = 0.05;                   // onsets: seconds between two at least
    int types = 6;                       // regions from types: types at level 1
    bool onsetUnits = false;             // regions from types: units between onsets (as soundtypes' segment onsets)
};
std::vector<long> onsetFrames(const SampleBuffer& in, const MarkOptions& o, Job& job);
std::vector<long> beatFrames(const SampleBuffer& in, Job& job, double* bpm = nullptr);
std::vector<long> noveltyFrames(const SampleBuffer& in, Job& job);
// soundtypes' units at level 1, each a region of its type (energy and pitch measured)
std::vector<Region> typeRegions(const SampleBuffer& in, const MarkOptions& o, Job& job);
// the regions between consecutive onsets (and the start and the end)
std::vector<Region> onsetRegions(const SampleBuffer& in, const MarkOptions& o, Job& job);
// its energy (RMS) and pitch (YIN's median over its voiced frames, MIDI; 0 when none)
void measureRegion(const SampleBuffer& in, Region& r);
// "D4", "F#4"; "—" for 0
std::string pitchName(double midi);

}  // namespace moritz
