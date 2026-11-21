// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// multitrack.h — what a score needs to play as a multitrack session (as in a DAW): the strips' values for the audio
// thread, the tracks processed on several cores, long sound files read from disk while they play, recording into
// tracks, and the overviews of long files. The score box (objects_score.cpp) plays through these; the arranger window
// (arranger.h) and the mixer (mixer_panel.h) show and change them.
//
// The session is the score (musil/src/music/score.h): a track is a row, a clip an event. What the multitrack mode
// keeps is on the rows (their extra, so saved with the score) and on the score's extra:
//   row   gain az mute solo   the strip (score.h's Strip): the fader (linear), the pan (degrees), mute and solo
//         sends               (list bus gain bus gain ...): post-fader sends to buses (at most kMaxSends)
//         out                 the bus it goes to ("" or none: the master)
//         bus                 1: a bus (it takes other tracks' outputs and sends; drawn as one)
//         insert              a patch file (inlet~ / outlet~) the track's audio goes through
//         instrument          a patch file playing the track's notes (its first inlet takes "note pitch velocity ms")
//         input input-ch      the device's input channel it records from (from 1; 0 none) and how many (1 or 2)
//         arm monitor         armed for recording; its input heard through the track while armed
//   score master-gain master-insert   the master strip
//         count-in punch punch-in punch-out latency   recording: bars before, a punch range (seconds), an offset in
//                             samples added to the device's latency
//         auto-crossfade      clips overlapping on a track crossfade (written as their fades when they are moved)
// Lanes named strip.gain (dB), strip.pan (degrees), strip.mute (0 or 1) and strip.send.<bus> (dB) drive the strip
// of their track, ramping between their points, on the audio thread.
//
// Threads. GUI: the plans, the parameters' values (StripParams: atomics), the streams opened, recording started and
// finished. Audio: reads the atomics, the streams' chunks, pushes the input into the takes' rings. The disk reader
// (one thread) fills the streams' chunks ahead of where the audio reads; the take writer (one thread while recording)
// drains the rings into WAV files (recorder.h's WavWriter). The track pool's workers run tracks for the audio thread
// within its callback (lock-free: an atomic ticket; a semaphore wakes the sleeping ones).
#pragma once

#include "objects.h"
#include "recorder.h"

#include "music/score.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace moritz {
namespace mt {

namespace sc = musil::score;

constexpr int kMaxSends = 4;
constexpr int kMaxChannels = 16;       // a score box's signal outlets, at most

// ---------------------------------------------------------------- a strip's values
// GUI writes (the mixer, a fader dragged; the score's rows when the plan is made), audio reads. The meters go the other
// way. The fields under "audio" belong to whichever thread processes the channel in a block (one at a time).
struct StripParams {
    std::atomic<float> gain{1.f};          // linear: the fader
    std::atomic<float> az{0.f};            // degrees: the pan (a balance on two channels)
    std::atomic<bool> mute{false}, solo{false}, monitor{false};
    std::atomic<float> send[kMaxSends];    // the sends' gains (linear); their buses are the plan's
    std::atomic<float> peak[2], rms[2];    // the meters: since the interface last took the peak; RMS smoothed
    std::atomic<uint32_t> blocks{0};       // blocks processed (the tests see a channel was run)
    // audio
    float applied = 1.f, appliedL = 1.f, appliedR = 1.f;
    float sendApplied[kMaxSends] = {}, sendTarget[kMaxSends] = {};
    double rmsState[2] = {0, 0};
    bool fresh = true;                     // nothing applied yet: the values at once, without a ramp
    StripParams();
    float takePeak(int c) { return c >= 0 && c < 2 ? peak[c].exchange(0.f, std::memory_order_relaxed) : 0.f; }
};

// what a row says, beyond score.h's Strip (GUI thread)
struct RowSettings {
    std::string out;                       // "" the master
    bool bus = false;
    struct Send {
        std::string bus;
        double gain = 0;
    };
    std::vector<Send> sends;
    std::string insert, instrument;
    int input = 0, inputCh = 1;            // from 1; 0 none
    bool arm = false, monitor = false;
};
RowSettings rowSettings(const sc::Row& r);
// written into a row's extra (a value at its default is left out); the strip (gain az mute solo) is not touched
void setRowSettings(sc::Row& r, const RowSettings& s);
// the master strip and the recording options on the score's extra
struct SessionSettings {
    double masterGain = 1;
    std::string masterInsert;
    int countIn = 0;                       // bars
    bool punch = false;
    double punchIn = 0, punchOut = 0;      // seconds
    int latency = 0;                       // samples, added to the device's
    bool crossfade = false;                // clips overlapping on a track crossfade (timeline_ops.h)
};
SessionSettings sessionSettings(const sc::Score& s);
void setSessionSettings(sc::Score& s, const SessionSettings& v);
// the strip's values put into its parameters (the GUI side of a fader, a button)
void loadStrip(StripParams& p, const sc::Row& r);

// a gain in dB as text ("-inf" below -120), and back
std::string dbText(double gain);
double dbToGain(double db);
double gainToDb(double gain);

// ---------------------------------------------------------------- tracks on several cores
// A small fixed pool of workers that run f(ctx, k) for k in [0, n) with the calling (audio) thread, which returns
// once all are done. No allocation, no lock: items are taken through one atomic ticket (the job's generation and the
// next index); a worker that finds nothing spins a little, then sleeps on a semaphore the caller posts. Results are
// the same whichever thread ran an item: each writes only its own.
class TrackPool {
public:
    static TrackPool& get();               // made on first use (GUI thread), never destroyed
    int workers() const { return (int)threads_.size(); }
    // audio thread: f for every k; ctx is the caller's (its block start and buffer lock go to the workers)
    void run(int n, void (*f)(void* ctx, int k), void* ctx);
    // how many threads tracks use: 0 automatic (the cores, at most 8), 1 one (the audio thread), n
    static std::atomic<int> threads;
    // tracks fewer than this (automatic): one thread, the overhead would not pay
    static std::atomic<int> minTracks;
    uint64_t jobs() const { return jobs_.load(); }          // run() calls that used the workers
    uint64_t helped() const { return helped_.load(); }      // items the workers ran

private:
    TrackPool();
    struct Job {
        std::atomic<void (*)(void*, int)> f{nullptr};
        std::atomic<void*> ctx{nullptr};
        std::atomic<int> n{0}, done{0};
        std::atomic<uint64_t> blockStart{0};
        std::atomic<bool> buffers{false};
    };
    void worker();
    bool help(uint64_t ticket, bool& ran);  // false: nothing left in the job the ticket names
    Job jobs2_[2];
    std::atomic<uint64_t> ticket_{0};      // generation << 32 | next index
    std::atomic<int> sleepers_{0};
    std::atomic<bool> busy_{false};        // a caller is running a job (another one, an offline render: alone)
    std::atomic<uint64_t> jobs_{0}, helped_{0};
    std::vector<std::thread> threads_;
    struct Sem;
    Sem* sem_;
};

// ---------------------------------------------------------------- long files from disk
// A sound file read while it plays: the frames near where the audio reads are kept in chunks the disk reader fills
// ahead of it; the start of the region a clip plays and of the transport's loop are pinned (always there), so a clip
// starting or a loop going round never waits. A chunk not there when the audio wants it plays as silence (a miss) and
// is read at once; a locate tells the reader beforehand (hint).
class DiskStream {
public:
    static constexpr int64_t kChunk = 8192;            // frames a chunk (plus one: the next chunk's first)
    static constexpr int kSlots = 24;                  // about 4.5 s at 44.1 kHz ahead of the reader
    static constexpr int kPins = 4;
    // GUI thread: the file opened (its decoder kept for the reader); nullptr and err when it can't be read
    static std::shared_ptr<DiskStream> open(const std::string& path, std::string& err);
    ~DiskStream();
    DiskStream(const DiskStream&) = delete;
    DiskStream& operator=(const DiskStream&) = delete;
    const std::string& path() const { return path_; }
    int channels() const { return channels_; }
    double sr() const { return sr_; }
    int64_t frames() const { return frames_; }
    // GUI thread: these frames kept (the start of a region, of the loop): read now, before the plan plays them
    void pin(int64_t frame);
    void hint(int64_t frame);                          // a locate: read from there next
    // audio thread: the chunk holding frame (and frame + 1), held until release(); false when it is not there (it is
    // asked for, and counted as a miss)
    struct View {
        const float* data = nullptr;                   // interleaved, from frame lo
        int64_t lo = 0, hi = -1;                        // frames lo..hi in data
        void* slot = nullptr;
        int64_t state = 0;
    };
    bool view(int64_t frame, View& v);
    static void release(View& v);
    void want(int64_t frame) { want_.store(frame, std::memory_order_relaxed); }
    // counts (any thread)
    uint64_t misses() const { return misses_.load(); }
    uint64_t reads() const { return reads_.load(); }
    // the tests: every chunk read waits this long (a slow disk)
    static std::atomic<int> testDelayMs;
    // reader thread: chunks read where wanted; true when it did something
    bool serve();
    bool filling() const;                              // a chunk is being read

private:
    friend class DiskReader;
    DiskStream() = default;
    struct Slot {
        std::atomic<int64_t> chunk{0};                 // (its chunk + 1) << 2, and 1 read, 2 being filled (0: empty)
        std::atomic<uint32_t> seq{0};                  // odd while being written
        std::vector<float> data;                       // (kChunk + 1) * channels
    };
    bool fill(Slot& s, int64_t c);                     // reader: chunk c into s
    Slot* find(int64_t c);
    std::string path_;
    int channels_ = 1;
    double sr_ = 44100;
    int64_t frames_ = 0;
    struct Dec;
    std::unique_ptr<Dec> dec_;                         // the reader's
    std::mutex decMutex_;                              // reader and pin (GUI) both read the file
    Slot slots_[kSlots];
    Slot pins_[kPins];
    std::atomic<int> npins_{0};
    std::atomic<int64_t> want_{-1}, hint_{-1};
    std::atomic<uint64_t> misses_{0}, reads_{0};
    std::vector<float> scratch_;
};

// the disk reader: one thread for every stream open, made on first use (GUI thread)
class DiskReader;
class DiskReader {
public:
    static DiskReader& get();
    void add(const std::shared_ptr<DiskStream>& s);    // read for as long as anything keeps it
    void wake();
    void flush();                                       // the tests: until nothing is left to read
private:
    DiskReader();
    void loop();
    std::mutex m_;
    std::vector<std::weak_ptr<DiskStream>> streams_;
    std::atomic<bool> poke_{false};
    std::thread thread_;
};
// clips longer than this (seconds) are read from disk; shorter ones decoded into memory
extern std::atomic<double> streamThreshold;
// the length of a file in seconds without decoding it (-1: can't read it)
double fileSeconds(const std::string& path);

// ---------------------------------------------------------------- the overview of a long file
// min and max of blocks of kBlock frames per channel, computed on a background thread reading the file in pieces (it
// is never decoded whole) and cached next to it (<file>.peaks: written atomically, read back while the file is
// unchanged: its size and time)
struct Peaks {
    static constexpr long kBlock = 256;
    int channels = 1;
    double sr = 44100;
    long frames = 0;
    std::vector<float> mn, mx;             // [block * channels + c]
    // the lowest and highest of channel c over frames [a, b)
    void range(int c, long a, long b, float& lo, float& hi) const;
};
bool computePeaks(const std::string& path, Peaks& out, std::string& err);   // reading the file
bool loadPeaks(const std::string& path, Peaks& out);                         // the cache, if still valid
bool savePeaks(const std::string& path, const Peaks& p);

// ---------------------------------------------------------------- recording
// The takes of one recording: a ring per armed track the audio thread pushes the input into (lock-free), drained by
// the writer thread into a WAV file each (24 bits), in the session's media folder. Made and published on the GUI thread
// before the transport starts; finished there after it stops (the files renamed into place).
struct Take {
    std::string track, path;
    int input = 0, channels = 1;           // the device's channels it records (from 0)
    std::vector<float> ring;               // interleaved
    std::atomic<size_t> head{0}, tail{0};
    std::atomic<int64_t> firstPos{-1};     // the transport's position at its first frame (samples)
    std::atomic<long long> frames{0};      // pushed
    WavWriter file;
    long long written = 0;
};
class Recording {
public:
    // tracks: the armed ones; folder: where the takes go (made); punch range in samples (to <= from: open)
    Recording(std::vector<std::unique_ptr<Take>> takes, int64_t from, int64_t to, double sr);
    ~Recording();
    bool start(std::string& err);          // the files opened, the writer started
    // audio thread: the input of this block, at the transport positions pos[i] (playing[i]); in: interleaved inCh
    void push(const float* in, int inCh, int n, const int64_t* pos, const bool* playing);
    // GUI thread, once the audio thread no longer pushes (stop() then wait for it): everything written, the files
    // finished; the takes that got nothing removed
    void stop();
    bool finish(std::string& err);
    const std::vector<std::unique_ptr<Take>>& takes() const { return takes_; }
    bool overflowed() const { return overflow_.load(); }
    bool active() const { return on_.load(std::memory_order_acquire); }
    int64_t from() const { return from_; }
    int64_t to() const { return to_; }
    double sampleRate() const { return sr_; }
    std::atomic<int> pushing{0};           // the audio thread is in push()
    // the count-in: clicks from clickFrom (samples) up to from(), a beat every clickBeat, a bar every clickBar beats
    int64_t clickFrom = 0, clickBeat = 0;
    int clickBar = 4;

private:
    void drain();
    std::vector<std::unique_ptr<Take>> takes_;
    int64_t from_, to_;
    double sr_;
    std::atomic<bool> on_{false}, quit_{false}, overflow_{false};
    std::thread writer_;
    std::vector<float> chunk_;
};
// a fresh name for a take in folder: "<track> take <n>.wav"
std::string takePath(const std::string& folder, const std::string& track);
// where a session's recordings go: next to its file, <name>_media/
std::string mediaFolder(const std::string& scorePath);
// where a clip recorded from position firstPos (samples) goes once the latency is taken off: its onset and the
// frames of its file it skips (when the take started before 0)
void placeTake(int64_t firstPos, int64_t latency, double sr, double& at, double& from);

// the device should open its input or not (a track armed or disarmed): set by the application
extern std::function<void()> inputNeedChanged;

// ---------------------------------------------------------------- a score box seen by the multitrack windows
// (objects_score.cpp; GUI thread, nullptr / false when b is not a score box)
inline constexpr const char* kMaster = "@master";             // the master strip's name
StripParams* stripOf(Box* b, const std::string& row);      // a track's or a bus's strip, or kMaster's
// recording on a score box: what plays and records; started at once (the transport is the caller's)
bool startRecording(Box* b, std::unique_ptr<Recording> r, std::string& err);
// unpublished, and no longer pushed into (the audio thread may still hold it for the callback it is in: retire it
// through the box's patch once finished, as SessionRecorder does)
std::unique_ptr<Recording> stopRecording(Box* b);
Recording* recordingOf(Box* b);
// the stems of the last block an offline render ran (each track's output, after its strip): channel-major
// pointers, tracks x channels; false when none
struct StemTap {
    std::vector<std::string> names;
    int channels = 0;
    std::vector<const float*> data;        // [track * channels + c], n frames
    int n = 0;
};
bool stemsOf(Box* b, StemTap& out);
void setStemTap(Box* b, bool on);           // keep each track's output (GUI thread, before rendering)
// what a score box plays now, for the tests and the status line
struct PlanInfo {
    int channels = 0, tracks = 0, buses = 0, streamed = 0, slots = 0, voices = 0;
    bool parallel = false;
    uint64_t plans = 0;                    // plans made (a fader moved must not make one)
};
PlanInfo planInfo(Box* b);
// the patch a track's insert, instrument or a patch clip runs in (for the drawer: edited live); nullptr none
Patch* trackPatch(Box* b, const std::string& row, const std::string& role);   // role: insert, instrument
Patch* clipPatch(Box* b, uint64_t clipId);

}  // namespace mt
}  // namespace moritz
