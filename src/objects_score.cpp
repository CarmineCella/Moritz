// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_score.cpp — transport, the patch's clock, and score, a score that plays itself on it.
//
// The score is Musil's C++ structure (musil/src/music/score.h), read from .score files with Musil's reader as data
// (score_text.h: nothing runs). The score object turns it into a plan off the audio thread: its sounds, by onset,
// for the audio thread (Published: never the interpreter, never an allocation there), its notes and messages, by
// onset, for the patch's scheduler. The audio thread renders a sound at the transport's position (a pure function
// of the position: a locate, a loop or a replaced score needs no state), so a sound starts on its sample; a note
// leaves its outlet at the logical time whose stamp is its sample, so a signal object it reaches starts there too.

#include "gui.h"
#include "multitrack.h"
#include "preview.h"
#include "roll.h"
#include "score_doc.h"
#include "score_window.h"
#include "transport.h"
#include "zoom.h"

#include "music/score.h"
#include "music/score_layout.h"
#include "music/score_text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <mutex>

namespace moritz {

namespace sc = musil::score;

// ---------------------------------------------------------------- scores handed between threads and objects
// A code box's (out k score) sends `score <token>`: the token names a copy of the score made on the code's thread
// (without what only Musil holds), kept among the last 64 handed over; a score box takes it.
namespace {
std::mutex& handoffMutex() { static std::mutex m; return m; }
std::deque<std::pair<std::string, std::shared_ptr<const sc::Score>>>& handoff() {
    static std::deque<std::pair<std::string, std::shared_ptr<const sc::Score>>> d;
    return d;
}
}  // namespace

std::string shareScore(std::shared_ptr<const sc::Score> s) {
    std::lock_guard<std::mutex> g(handoffMutex());
    static unsigned long n = 0;
    std::string token = "score-" + std::to_string(++n);
    auto& d = handoff();
    d.emplace_back(token, std::move(s));
    while (d.size() > 64) d.pop_front();
    return token;
}

std::shared_ptr<const sc::Score> sharedScore(const std::string& token) {
    std::lock_guard<std::mutex> g(handoffMutex());
    for (auto& kv : handoff())
        if (kv.first == token) return kv.second;
    return nullptr;
}

namespace {

std::string fmtTime(double sec) {
    char b[32];
    int m = (int)(sec / 60);
    std::snprintf(b, sizeof b, "%d'%02d\"", m, (int)std::floor(sec - 60 * m));
    return b;
}
Atom atomOf(const sc::Datum& d) {
    if (d.kind == sc::Datum::Num) return Atom((float)d.num);
    return Atom(d.s);
}
void appendDatum(Message& m, const sc::Datum& d) {
    if (d.kind == sc::Datum::List) { for (auto& x : d.list) appendDatum(m, x); return; }
    m.push_back(atomOf(d));
}
bool endsWith(const std::string& s, const char* e) {
    size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

// ---------------------------------------------------------------- transport
// transport [bpm] [num den | num/den]: the patch's clock (one for each top document: a patch's window, a help patch,
// a score's own player; its subpatches and abstractions follow it, several transport objects in it show and drive the
// same one, and another patch open has its own). Times in the scores are seconds; bars and beats are
// counted at the tempo (beats a minute; a beat is 1/den) and meter.
//   play, 1 / stop, 0        start (from where it is) and stop
//   locate bar [beat [tick]] go to a bar (from 1; 480 ticks a beat); seek ms: to a time
//   loop bar1 bar2           play from bar1 up to bar2 again and again; loop 0 (or off): no loop
//   tempo bpm, meter n d     the tempo and meter bars and beats are counted in
//   bang                     the position now
// Outlets: the bar, the beat, the position in ms (at every beat while it plays, and at every change), and 1 or 0
// when it starts or stops.
class TransportBox : public Box {
public:
    explicit TransportBox(const Message& a) {
        setPorts({P::Control}, {P::Control, P::Control, P::Control, P::Control});
        std::vector<float> nums;
        for (auto& x : a) {
            if (x.isFloat()) nums.push_back(x.f);
            else if (auto k = x.s.find('/'); k != std::string::npos) {
                nums.resize(std::max<size_t>(nums.size(), 1), 120.f);
                nums.push_back((float)std::atof(x.s.substr(0, k).c_str()));
                nums.push_back((float)std::atof(x.s.substr(k + 1).c_str()));
            }
        }
        if (!nums.empty() && nums[0] > 0) bpm_ = nums[0];
        if (nums.size() >= 3 && nums[1] >= 1 && nums[2] >= 1) { num_ = (int)nums[1]; den_ = (int)nums[2]; }
    }
    ~TransportBox() override { if (t_) t_->unlisten(this); }
    void attached() override {
        t_ = transportOf(patch);
        if (bpm_ > 0) t_->setTempo(patch, bpm_);
        if (num_ > 0) t_->setMeter(patch, num_, den_);
        t_->listen(this, [this] { changed(); });
    }
    void audioChanged() override { if (t_ && t_->playing()) t_->reanchor(patch); }
    void message(int, const Message& m) override {
        if (!t_ || m.empty()) return;
        const Atom& h = m[0];
        if (h.is("play") || h.is("start") || (h.isFloat() && h.f != 0.f)) t_->play(patch);
        else if (h.is("stop") || h.isFloat()) t_->stop(patch);
        else if (h.is("bang")) report();
        else if (h.is("locate")) t_->locate(patch, t_->secondsOf(argf(m, 1, 1.f), argf(m, 2, 1.f), argf(m, 3, 0.f)));
        else if (h.is("seek")) t_->locate(patch, argf(m, 1, 0.f) / 1000.0);
        else if (h.is("rewind")) t_->locate(patch, 0);
        else if (h.is("loop")) {
            if (m.size() >= 3 && m[1].isFloat() && m[2].isFloat() && m[2].f > m[1].f)
                t_->setLoop(patch, true, t_->secondsOf(m[1].f, 1, 0), t_->secondsOf(m[2].f, 1, 0));
            else t_->setLoop(patch, false);
        }
        else if (h.is("tempo") && m.size() > 1 && m[1].isFloat()) t_->setTempo(patch, m[1].f);
        else if (h.is("meter") && m.size() > 2 && m[1].isFloat() && m[2].isFloat()) t_->setMeter(patch, (int)m[1].f, (int)m[2].f);
        else patch->print("transport: unknown message " + h.str());
    }

private:
    std::shared_ptr<Transport> t_;
    float bpm_ = 0;
    int num_ = 0, den_ = 4;
    bool wasPlaying_ = false;

    void report() {
        int lap; double pos;
        t_->where(patch, lap, pos);
        long bar; int beat, tick;
        t_->bbt(pos, bar, beat, tick);
        outletFloat(2, (float)(pos / t_->sampleRate() * 1000.0));
        outletFloat(1, (float)beat);
        outletFloat(0, (float)bar);
    }
    void changed() {
        bool here = false;                       // a removed box is kept until the audio thread lets go: it is quiet then
        for (auto& b : patch->boxes) here = here || b.get() == this;
        if (!here) return;
        patch->cancel(this);
        if (t_->playing() != wasPlaying_) { wasPlaying_ = t_->playing(); outletFloat(3, wasPlaying_ ? 1.f : 0.f); }
        report();
        if (t_->playing()) next();
    }
    // the next beat (or the loop's start, when the loop comes first), on its sample
    void next() {
        int lap; double pos;
        t_->where(patch, lap, pos);
        double now = patch->logicalTime();
        double beat = t_->beatSamples();
        double b = (std::floor(pos / beat + 1e-9) + 1) * beat;
        for (int guard = 0; guard < 8; ++guard) {
            double t = t_->timeAt(std::floor(b + 1e-6), lap);
            if (std::isnan(t) && t_->looping()) {               // past the loop's end: its start, the next pass
                ++lap;
                b = (double)t_->loopStart();
                t = t_->timeAt(b, lap);
            }
            if (std::isnan(t)) return;
            if (t > now) {                                      // always later: the clock never stands still
                patch->scheduleAt(this, t, [this] { report(); next(); });
                return;
            }
            b += beat;
        }
    }
};

// ---------------------------------------------------------------- score
// What the audio thread plays (multitrack.h): a channel for every track (a row of the score) and bus, each with its
// voices (the sounds on it, by onset), the patches it runs (its insert, its instrument, its patch clips), its strip
// (parameters the interface changes through atomics: no new plan for a fader moved) and where it goes (a bus, the
// master); the channels in an order where a channel comes after every channel that feeds it, those fed by none first
// (they may run on several cores). The master sums what goes to it, through its own strip and insert, into the outlets.
struct ScorePlan {
    struct Voice {
        int64_t start = 0, len = 0;      // positions, samples
        double from = 0, to = -1;        // seconds in the source
        float gain = 1.f;
        double speed = 1;                // a note's sound: faster or slower by its pitch against the sample's
        int file = -1, buf = -1, stream = -1;   // a decoded file, a buffer~ by name, or a file read from disk
        int gains = 0;                   // where its channel gains start in `gains`
        int64_t fadeIn = 0, fadeOut = 0; // its fades (the clip's fade-in and fade-out, or a crossfade), samples
        uint8_t inShape = 0, outShape = 0;   // 0 linear, 1 equal power, 2 slow (squared), 3 fast (square root)
    };
    struct Clip {                        // a patch clip: its patch runs while the transport is inside it
        int slot = -1;
        int64_t start = 0, end = 0;
    };
    struct Curve {                       // a strip lane: ramps between its points
        enum What : uint8_t { Gain, Pan, Mute, Send } what = Gain;
        int send = 0;
        std::vector<std::pair<int64_t, float>> pts;   // by position
        float at(int64_t pos) const {
            if (pts.empty()) return 0.f;
            if (pos <= pts.front().first) return pts.front().second;
            if (pos >= pts.back().first) return pts.back().second;
            auto it = std::upper_bound(pts.begin(), pts.end(), pos,
                                       [](int64_t p, const std::pair<int64_t, float>& q) { return p < q.first; });
            auto a = *(it - 1), b = *it;
            if (what == Mute || b.first == a.first) return a.second;
            return a.second + (b.second - a.second) * (float)((double)(pos - a.first) / (double)(b.first - a.first));
        }
    };
    struct Source {                      // what a channel takes from another: its output (send -1) or a send of it
        int ch = 0, send = -1;
    };
    struct Channel {
        std::string name;
        mt::StripParams* strip = nullptr;
        std::vector<Voice> voices;
        int64_t maxLen = 0;
        std::vector<Clip> clips;
        int insert = -1, instrument = -1;   // the box's slots
        std::vector<Source> inputs;
        bool bus = false, toMaster = true;
        int input = -1, inputCh = 1;     // the device's channel it records from (from 0; -1 none)
        bool armed = false;
        bool balance = true;             // its pan a balance on its two channels (else placed in its voices)
        std::vector<Curve> curves;
        float* buf = nullptr;            // nOut x kBlock: what it makes in a block, after its strip
        float* tmp = nullptr;            // its insert's output
    };
    std::vector<Channel> channels;
    int nFree = 0;                       // the first nFree take nothing from others
    mt::StripParams* master = nullptr;
    int masterInsert = -1;
    float* masterBuf = nullptr;
    float* masterTmp = nullptr;
    std::vector<float> gains;            // per voice: one per outlet (a mono source's placement), then its balance
    std::vector<float> arena;            // the channels' buffers
    double sr = 44100.0;
    int nOut = 2;
    std::vector<std::shared_ptr<const SampleBuffer>> files;
    std::vector<std::unique_ptr<BufferRef>> bufs;
    std::vector<std::shared_ptr<mt::DiskStream>> streams;
    std::vector<std::shared_ptr<mt::StripParams>> strips;   // kept alive while the plan plays
    bool anyArmed = false;
};

// the thread a box's inner patch runs on: what its inlet~s read and its outlet~s add to (the channel's buffers)
struct InnerIO {
    const float* in[mt::kMaxChannels] = {};
    float* out[mt::kMaxChannels] = {};
    int nin = 0, nout = 0;
};
thread_local const InnerIO* tlInner = nullptr;
const float kSilence[dsp::kBlock] = {};

class ScoreBox;
struct BlockCtx;

// score name [file.score] [channels] [@size w h] [@view timeline|roll|piano|list]: a score (rows, notes, sounds,
// messages, scores inside it) that plays on the patch's transport. Without a file it is kept in the patch. The box
// draws its score as a roll (roll.h), edited in place when the patch is locked (select, drag to move, drag an end to
// resize, Alt-drag copies, Delete, the arrows nudge, Alt+Up and Alt+Down detune by a cent); a double-click (or its
// menu's Open) opens the score's window (score_window.h) in the view used last for it, the first time in its @view
// (Roll when none); in edit mode its corner sizes it. Outlets: one signal for each
// channel (2 by default: its sounds, and the notes that carry a sound of their own, placed at their azimuths), then
// notes (pitch velocity duration-ms row dynamics technique; - for none), then messages (name values...; a patch event
// as patch name duration-ms args...), then info (end when it plays past its last event; the summary for info). A
// message is also sent to the receive objects of its name (automation lanes drive a patch's receives). The rows are
// tracks, and the score plays as a multitrack session (multitrack.h): each track a channel through its mixer strip (gain,
// az as a pan, mute, solo: values the mixer changes through atomics, no new plan), its insert patch, into a bus or the
// master, with sends to buses; a track's instrument patch plays its notes (they leave the notes outlet otherwise), a
// patch clip runs while it plays; lanes named strip.gain, strip.pan, strip.mute, strip.send.<bus> drive the strip; long
// files are read from disk; a sound's fades (with their shapes) are played; armed tracks record (the arranger's Record).
//   read [file], write [file]   a .score file (Musil data, read without running anything); write: in the patch's
//                               folder or ~/Documents/Moritz
//   clear                       no events
//   add [note] at dur row pitch [dyn [tech]] | add [note] at row pitch [dyn [tech]] (a beat long)
//   add sound at dur file [from [to]] | add msg at name values...
//   select all | none | id... | row name | time t0 t1      what the transformations below change (none: all of it)
//   transpose n, invert [pitch], retrograde, augment k, diminish k, shift seconds, quantize [grid-seconds [strength]],
//   humanize [ms [dB [seed]]], thin keep [seed], density v... [seed]   (music/score_transform.h, as Musil's)
//   play, stop, locate bar [beat], seek ms   the transport's (the same for every score of the patch); play at the
//                               score's end or past it plays from the start, and a play sent here stops at the score's
//                               end (unless the transport loops, or something else started, stopped or located it since)
//   info                        rows events seconds, out of the info outlet
//   score <token>               a score a code box (or orchidea) sent ((out k s) with s a Musil score)
//   open [view]                 the score's window (timeline, roll, piano, list; none: the view used last)
// The box's roll and its window's views are views of one document (score_doc.h): an edit in the box is
// an undo step of the score's history (Edit > Undo with the box clicked last takes it back, the timeline's and the
// roll's Undo too), and marks the patch edited; the messages above change the score from outside (the document
// follows them, its history starting again), as the code view and the patch's undo do.
class ScoreBox : public Box {
public:
    explicit ScoreBox(const Message& a) {
        kind = BoxKind::UI;
        for (size_t k = 0; k < a.size(); ++k) {
            const Atom& x = a[k];
            if (x.is("@size")) {
                w_ = std::clamp((int)argf(a, k + 1, (float)kDefaultW), 120, 4000);
                h_ = std::clamp((int)argf(a, k + 2, (float)kDefaultH), 50, 3000);
                k += 2;
            }
            else if (x.is("@view")) {                    // the view its window opens in the first time
                ScoreView v;
                if (k + 1 < a.size() && parseScoreView(a[k + 1].str(), v)) view_ = (int)v;
                ++k;
            }
            else if (x.isFloat()) nOut_ = std::clamp((int)x.f, 1, 16);
            else if (endsWith(x.s, ".score")) file_ = x.s;
            else if (name_.empty()) name_ = x.s;
        }
        std::vector<Port> outs((size_t)nOut_, P::Signal);
        outs.insert(outs.end(), {P::Control, P::Control, P::Control});
        setPorts({P::Control}, outs);
        score_ = std::make_shared<sc::Score>(name_.empty() ? std::string("score") : name_, 44100);
    }
    ~ScoreBox() override {
        if (t_) t_->unlisten(this);
        inline_.reset();                         // its roll's target lets go of the document (the windows keep theirs)
    }

    Message saveArgs() const override {
        Message m;
        if (!name_.empty()) m.push_back(Atom(name_));
        if (!file_.empty()) m.push_back(Atom(file_));
        if (nOut_ != 2) m.push_back(Atom((float)nOut_));
        if (w_ != kDefaultW || h_ != kDefaultH) { m.push_back(Atom("@size")); m.push_back(Atom((float)w_)); m.push_back(Atom((float)h_)); }
        if (view_ >= 0) {
            std::string v = scoreViewName((ScoreView)view_);
            m.push_back(Atom("@view"));
            m.push_back(Atom(v == "piano roll" ? std::string("piano") : v));
        }
        return m;
    }
    // kept in the patch when it has no file of its own
    std::string script() const override { return file_.empty() && !score_->empty() ? sc::toText(*score_) : ""; }
    void setScript(const std::string& text) override {
        try {
            auto s = sc::readText(text, here());
            if (!name_.empty()) s->name = name_;
            score_ = s;
        } catch (const std::exception& e) { if (patch) complain(e); }
    }
    void attached() override {
        t_ = transportOf(patch);
        t_->listen(this, [this] { follow(); });
        if (!file_.empty()) readFile(file_);
        else rebuild();
    }
    void resolve() override {
        // the buffer~s named by the plan's sounds may have come or gone: looked up again (atomics)
        if (ScorePlan* p = plan_.get()) for (auto& b : p->bufs) b->resolve(patch);
        if (plan_.get() && plan_.get()->sr != patch->root()->sampleRate.load()) rebuild();
    }
    void audioChanged() override { if (t_ && t_->playing()) t_->reanchor(patch); }
    void message(int, const Message& m) override {
        if (m.empty()) return;
        const Atom& h = m[0];
        if (h.is("read")) readFile(m.size() > 1 ? m[1].str() : file_);
        else if (h.is("write")) writeFile(m.size() > 1 ? m[1].str() : file_);
        else if (h.is("clear")) { auto s = score_->clone(); s->clear(); dropSelection(); replace(s); }
        else if (h.is("add")) add(m);
        else if (h.is("select")) select(m);
        else if (h.is("transpose") || h.is("invert") || h.is("retrograde") || h.is("augment") || h.is("diminish") ||
                 h.is("shift") || h.is("quantize") || h.is("humanize") || h.is("thin") || h.is("density")) transformBy(m);
        else if (h.is("open")) {
            ScoreView v;
            bool given = m.size() > 1 && parseScoreView(m[1].str(), v);
            openScoreBox(this, given ? (int)v : -1);
        }
        else if (h.is("play") || h.is("start")) playHere();
        else if (h.is("stop")) { if (t_) t_->stop(patch); }
        else if (h.is("locate") && t_) t_->locate(patch, t_->secondsOf(argf(m, 1, 1.f), argf(m, 2, 1.f), argf(m, 3, 0.f)));
        else if (h.is("seek") && t_) t_->locate(patch, argf(m, 1, 0.f) / 1000.0);
        else if (h.is("info")) outlet(nOut_ + 2, {Atom("info"), Atom((float)rows_), Atom((float)score_->size()), Atom((float)score_->duration())});
        else if (h.is("score") && m.size() > 1) {
            auto s = sharedScore(m[1].str());
            if (!s) { patch->print("score: no score " + m[1].str() + " (a code box sends one with (out k s))"); return; }
            dropSelection();
            replace(s->clone());
        }
        else if (h.is("merge") && m.size() > 1) {      // another score's events added (an orchestration's sections)
            auto s = sharedScore(m[1].str());
            if (!s) { patch->print("score: no score " + m[1].str() + " (a code box sends one with (out k s))"); return; }
            auto c = score_->clone();
            for (auto& r : s->rows) {                    // a row it has not: before the first one after it in orchestral order
                if (std::any_of(c->rows.begin(), c->rows.end(), [&](const auto& q) { return q.name == r.name; })) continue;
                int rank = sc::layout::instrumentRank(r.name);
                auto at = std::find_if(c->rows.begin(), c->rows.end(), [&](const auto& q) { return sc::layout::instrumentRank(q.name) > rank; });
                c->rows.insert(at, r);
            }
            for (auto& e : s->events()) {
                auto x = std::make_shared<sc::Event>(*e);
                x->id = 0;
                c->insert(x);
            }
            dropSelection();
            replace(c);
        }
        else patch->print("score: unknown message " + h.str());
    }
    void dsp(int n) override;
    bool wantsInput() const override { return armed_; }
    // ---- the patches it runs (its tracks' inserts and instruments, its patch clips): its inner patches, in a fixed order
    Patch* inner() const override { return slots_.empty() ? nullptr : slots_[0]->p.get(); }
    std::vector<Patch*> inners() const override {
        std::vector<Patch*> v;
        for (auto& sl : slots_) v.push_back(sl->p.get());
        return v;
    }
    void syncPorts() override { for (auto& sl : slots_) numberPatchPorts(*sl->p); }   // the box's own ports stay
    const float* innerInput(const Patch*, int k, int) override {
        const InnerIO* io = tlInner;
        return io && k >= 0 && k < io->nin && io->in[k] ? io->in[k] : kSilence;
    }
    float* innerOutput(const Patch*, int k) override {
        const InnerIO* io = tlInner;
        return io && k >= 0 && k < io->nout ? io->out[k] : nullptr;
    }
    void innerOutlet(const Patch*, int, const Message&) override {}   // a track patch's control outlets lead nowhere
    void processChannel(const BlockCtx& bc, int index);
    void runInsert(int slot, float* buf, float* tmp, int nOut, int n);
    static void applyStrip(mt::StripParams& st, float* buf, int nOut, int n, float gain, float az, bool balance);
    static void meter(mt::StripParams& st, const float* buf, int nOut, int n, double sr);
    // ---- for the multitrack windows (multitrack.h)
    mt::StripParams* strip(const std::string& name) { return stripNamed(name).get(); }
    bool startRecording(std::unique_ptr<mt::Recording> r, std::string& err) {
        if (recordingOwned_) { err = "already recording"; return false; }
        if (!r->start(err)) return false;
        recordingOwned_ = std::move(r);
        recording_.store(recordingOwned_.get(), std::memory_order_release);
        return true;
    }
    std::unique_ptr<mt::Recording> stopRecording() {
        recording_.store(nullptr, std::memory_order_seq_cst);
        if (recordingOwned_) recordingOwned_->stop();       // waits for the audio thread to leave push()
        return std::move(recordingOwned_);
    }
    mt::Recording* recording() const { return recordingOwned_.get(); }
    void setStemTap(bool on) { stemTap_ = on; }
    bool stems(mt::StemTap& out) {
        const ScorePlan* p = plan_.get();
        if (!p) return false;
        out.names.clear();
        out.data.clear();
        out.channels = nOut_;
        out.n = lastN_;
        for (auto& ch : p->channels) {
            out.names.push_back(ch.name);
            for (int c = 0; c < nOut_; ++c) out.data.push_back(ch.buf + c * dsp::kBlock);
        }
        return true;
    }
    mt::PlanInfo info() const {
        mt::PlanInfo i;
        const ScorePlan* p = plan_.get();
        i.plans = plans_;
        if (!p) return i;
        i.channels = (int)p->channels.size();
        for (auto& ch : p->channels) {
            (ch.bus ? i.buses : i.tracks)++;
            i.voices += (int)ch.voices.size();
        }
        i.streamed = (int)p->streams.size();
        i.slots = (int)slots_.size();
        i.parallel = p->nFree >= 2;
        return i;
    }
    Patch* slotPatch(const std::string& key) const {
        for (auto& sl : slots_)
            if (sl->key == key && !sl->file.empty()) return sl->p.get();
        return nullptr;
    }
    Patch* clipSlot(uint64_t id) const {
        std::string k = "clip:" + std::to_string(id) + ":";
        for (auto& sl : slots_)
            if (sl->key.rfind(k, 0) == 0 && !sl->file.empty()) return sl->p.get();
        return nullptr;
    }

    // ---- the interface: a header (name and summary) and the score as a roll, edited in place when locked
    void uiSize(int& w, int& h) const override {      // measured without the fonts: made where there is no display too
        w = w_;
        h = h_;
    }
    bool uiResizable() const override { return true; }
    void uiResize(int w, int h) override {
        w_ = std::clamp(w, 120, 4000);
        h_ = std::clamp(h, 50, 3000);
    }
    void drawUI(int X, int Y) override {
        zd::font(kFont, kFontSize);
        fl_color(kText);
        int head = kBoxH;
        std::string title = "score" + (name_.empty() ? std::string() : " " + name_) + (file_.empty() ? std::string() : " " + file_);
        zd::draw(title.c_str(), X + 8, Y + head / 2 + zd::height() / 2 - zd::descent());
        zd::font(kFont, std::max(8, kFontSize - 3));
        fl_color(kDim);
        std::string s = std::to_string(rows_) + " rows · " + std::to_string(score_->size()) + " events · " + fmtTime(score_->duration());
        int sw = (int)zd::width(s.c_str());
        zd::font(kFont, kFontSize);
        int tw = (int)zd::width(title.c_str());
        zd::font(kFont, std::max(8, kFontSize - 3));
        zd::draw(s.c_str(), X + std::max(8 + tw + 12, w_ - sw - 8), Y + head / 2 + 4);
        RollCore& c = roll();
        c.setSize(w_ - 2, h_ - head - 6);
        if (t_) {
            int lap;
            double pos;
            t_->where(patch, lap, pos);
            c.playhead = t_->playing() || pos > 0 ? t_->seconds(pos) : NAN;
        }
        c.draw(X + 1, Y + head);
    }
    // locked: the roll edited in place; a double-click opens the score's window (on a score placed inside: the roll,
    // into it)
    void uiMouse(int event, int mx, int my) override {
        RollCore& c = roll();
        bool dbl = event == FL_PUSH && Fl::event_clicks();
        if (dbl) Fl::event_clicks(0);
        if (event == FL_PUSH && my < kBoxH) {              // the header
            if (dbl) openScoreBox(this);
            return;
        }
        if (!c.mouse(event, mx - 1, my - kBoxH, Fl::event_state(), dbl ? 1 : 0) && dbl) openScoreBox(this);
    }
    bool uiKey(int key) override { return roll().key(key, Fl::event_state()); }
    bool uiUndo(bool redo) override {                  // the score's own history (its edits in the box, the roll, the timeline)
        RollEditTarget* t = roll().target();
        if (!t || !(redo ? t->canRedo() : t->canUndo())) return false;
        if (redo) t->redo();
        else t->undo();
        if (patch) patch->redraw();
        return true;
    }
    // (the canvas's menu has Open for a score box: its window, in the view used last)
    std::vector<std::string> menuItems() override { return {"Read...", "Write..."}; }
    void menuAction(int k) override {
        std::string f = chooseFile(k == 1, k == 0 ? "Read a score" : "Write the score", "Score\t*.score", file_.empty() ? name_ + ".score" : file_);
        if (!f.empty()) message(0, {Atom(k == 0 ? "read" : "write"), Atom(f)});
    }

    // the score from elsewhere (GUI thread): what plays changes at once, the transport going on
    void replace(std::shared_ptr<sc::Score> s) {
        if (!name_.empty() && s->name != name_) s->name = name_;
        score_ = std::move(s);
        edited();
    }
    // a click on a note in a roll (audition, GUI thread): e as it sounds (its placements applied), origin the score it is
    // in (where its sample is looked for). A note with a sample of its own plays it through the preview voice (preview.h,
    // the sound editor's: real-time safe) at its pitch, for its duration, faded in and out; a note without one leaves
    // the notes outlet as playing sends it, then at its end the same with velocity 0 (a note-off), or, when nothing is
    // connected there (a score of its own on its player), sounds as a tone at its pitch through the preview; a sound
    // event plays its region. What was done, for the status line ("" nothing: not a note or a sound)
    std::string audition(const sc::Event& e, const sc::Score* origin) {
        App& app = App::get();
        std::string said;
        double pitch = e.sounding();
        char pn[32];
        std::snprintf(pn, sizeof pn, "%g", std::round(pitch * 100) / 100);
        if (e.kind == sc::Kind::Note && e.variant != "chord") {
            const sc::Datum* snd = sc::field(e.extra, "sound");
            if (snd && (snd->kind == sc::Datum::Str || snd->kind == sc::Datum::Sym) && !snd->s.empty()) {
                std::string path = sourcePath(snd->s, origin);
                std::shared_ptr<const SampleBuffer> buf = soundOf(path);
                if (!buf) return "can't read the note's sample " + snd->s;
                const sc::Datum* sp = sc::field(e.extra, "sound-pitch");
                double base = sp && sp->kind == sc::Datum::Num ? sp->num : e.pitch;
                double speed = std::pow(2.0, (pitch - base) / 12.0);
                long to = (long)std::ceil(std::max(0.02, e.dur) * buf->sr * speed);
                app.audio.preview().playNote(buf, 0, std::min<long>(to, buf->frames), speed, (float)e.gain, 0.008);
                said = "the note's sample at " + std::string(pn) + ": " + fileName(snd->s);
            } else {
                int vel = std::clamp((int)std::lround(sc::velocityOf(e.dyn) * e.gain), 1, 127);
                Atom row(e.row.empty() ? std::string("-") : e.row), dyn(e.dyn.empty() ? std::string("-") : e.dyn),
                    tech(e.tech.empty() ? std::string("-") : e.tech);
                if (notesConnected()) {
                    outlet(nOut_, {Atom((float)pitch), Atom((float)vel), Atom((float)(e.dur * 1000.0)), row, dyn, tech});
                    offs_.push_back({patch->logicalTime() + std::max(0.0, e.dur), Message{Atom((float)pitch), Atom(0.f), Atom(0.f), row, dyn, tech}});
                    armOffs();
                    said = "the note " + std::string(pn) + " out of the notes outlet";
                } else {
                    app.audio.preview().playNote(tone(pitch, vel, e.dur), 0, -1, 1.0, 1.f, 0.008);
                    said = "the note " + std::string(pn) + " as a tone (nothing is connected to the score's notes outlet)";
                }
            }
        } else if (e.kind == sc::Kind::Sound && !e.buffer && !e.source.empty()) {
            std::shared_ptr<const SampleBuffer> buf = soundOf(sourcePath(e.source, origin));
            if (!buf) return "can't read the sound " + e.source;
            long from = (long)std::floor(std::max(0.0, e.from) * buf->sr);
            long to = from + (long)std::ceil(std::max(0.02, e.dur) * buf->sr);
            if (e.to >= 0) to = std::min(to, (long)std::ceil(e.to * buf->sr));
            app.audio.preview().playNote(buf, from, std::min<long>(to, buf->frames), 1.0, (float)e.gain, 0.008);
            said = "the sound " + fileName(e.source);
        } else {
            return "";
        }
        if (!app.audio.running()) said += ", but audio is off: turn it on (the Audio button, " + shortcutText(FL_COMMAND + '/') + ") to hear it";
        return said;
    }
    // what an editor binds to (score_doc.h): the score now, and whether it is kept in the patch (no file of its own)
    std::shared_ptr<sc::Score> current() const { return score_; }
    bool keptInPatch() const { return file_.empty(); }
    // the view its window opens in: the one used last, else its @view, else Roll
    int openView() const { return lastView_ >= 0 ? lastView_ : view_ >= 0 ? view_ : (int)ScoreView::Roll; }
    void setLastView(int v) { lastView_ = v; }
    // the roll inside the box (made when first drawn, clicked or asked for its selection), on the score's document
    RollCore& roll() {
        if (!inline_) {
            inline_ = std::make_unique<RollCore>(nullptr);
            inline_->compact = true;
            inline_->onChange = [this] { if (patch) patch->redraw(); };
            inline_->onAudition = [this](uint64_t id) {    // a click on a note sounds it
                std::string m = audition(inline_->sounding(id), inline_->shown().get());
                if (!m.empty() && patch) patch->status(m);
            };
            inline_->onEnter = [this](uint64_t sub) {
                ScoreWindow* w = openScoreBox(this, (int)ScoreView::Roll);
                if (w && sub) { w->roll()->core().enter(sub); w->roll()->changed(); }
            };
        }
        if (!inline_->target() && patch)
            if (auto t = makeRollTarget(ScoreDoc::ofBox(this))) {
                t->setListener([this](unsigned) { if (patch) patch->redraw(); });   // the timeline's edits, its selection
                inline_->setTarget(t);
            }
        return *inline_;
    }

private:
    struct Ctl {
        int64_t pos;
        int outlet;
        Message msg;
        bool send = false;                       // a message: to the receives of its name too
        enum Kind : uint8_t { Plain, Note, ClipStart, ClipEnd } kind = Plain;
        std::string track;                       // a note's: its strip says whether it sounds; its instrument plays it
        int slot = -1;                           // a patch clip's
    };
    static constexpr int kDefaultW = 360, kDefaultH = 150;
    int nOut_ = 2;
    int w_ = kDefaultW, h_ = kDefaultH;
    int view_ = -1, lastView_ = -1;              // @view (saved), the view used last (not saved)
    std::string name_, file_;
    std::shared_ptr<sc::Score> score_;           // never changed in place: a new one for every change (the views keep theirs)
    std::unique_ptr<RollCore> inline_;
    std::shared_ptr<Transport> t_;
    Published<ScorePlan> plan_;
    std::vector<Ctl> ctl_;                       // notes and messages by onset (GUI)
    int64_t end_ = 0;                            // where the last event ends
    std::map<std::string, std::shared_ptr<const SampleBuffer>> decoded_;   // files read once (GUI)
    // what the scheduler is on: the pass through the loop and the next control event
    int lap_ = 0;
    size_t cursor_ = 0;
    bool ended_ = false;
    bool firing_ = false;
    bool ownPlay_ = false;                       // the transport started by its play message (it stops at the end)
    uint64_t ownChanges_ = 0;
    int lastN_ = 0;                              // the last block's length (stems)
    int64_t firedPos_ = -1;
    int firedLap_ = 0;
    size_t rows_ = 0;                            // the rows declared or used
    // the multitrack side (multitrack.h): the strips, the patches it runs, the files read from disk, a recording
    struct Slot;
    std::map<std::string, std::shared_ptr<mt::StripParams>> strips_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::map<std::string, std::shared_ptr<mt::DiskStream>> streams_;
    std::map<std::string, double> lengths_;
    std::atomic<mt::Recording*> recording_{nullptr};
    std::unique_ptr<mt::Recording> recordingOwned_;
    uint64_t planKey_ = 0, plans_ = 0;
    bool armed_ = false;
    bool stemTap_ = false;

    // where the score kept in the patch is, for its errors and the files it places: the patch's folder
    std::string here() const {
        std::string dir = patch ? dirOf(patch->resolvePath("x")) : std::string();
        return (dir.empty() ? std::string(".") : dir) + "/score " + id;
    }
    std::string folderOf(const sc::Score* s) const { return s && !s->path.empty() ? sc::folderOf(s->path) : std::string(); }
    std::string sourcePath(const std::string& src, const sc::Score* origin) const {
        if (src.empty() || isAbsolutePath(src)) return src;
        std::string dir = folderOf(origin);
        if (dir.empty()) dir = folderOf(score_.get());
        if (!dir.empty()) {
            std::string f = dir + "/" + src;
            std::error_code ec;
            if (std::filesystem::exists(f, ec)) return f;
        }
        return patch->resolvePath(src);
    }

    // an error said once (Musil's errors already start with "score: ")
    void complain(const std::exception& e) const {
        if (!patch) return;
        std::string m = e.what();
        patch->print(m.rfind("score: ", 0) == 0 ? m : "score: " + m);
    }
    // the folder the score's relative references are named from: its file's, else the patch's
    std::string referenceFolder() const {
        if (!score_->path.empty()) return sc::folderOf(score_->path);
        return patch ? dirOf(patch->resolvePath("x")) : std::string();
    }
    void readFile(const std::string& f) {
        if (f.empty()) { patch->print("score: read what? (read file.score)"); return; }
        try {
            auto s = sc::readFile(patch->resolvePath(f));
            if (!name_.empty()) s->name = name_;
            dropSelection();
            score_ = s;
            edited();
        } catch (const std::exception& e) { complain(e); }
    }
    bool writeFile(const std::string& f, std::string* errOut = nullptr) {
        auto fail = [&](const std::string& e) { if (errOut) *errOut = e; else patch->print("score: " + e); return false; };
        if (f.empty()) return fail("write where? (write file.score)");
        std::string path = patch->resolvePath(f);
        // a patch from elsewhere does not write anywhere: its folder, or ~/Documents/Moritz
        std::vector<std::string> roots;
        const Patch* q = patch;
        while (q && q->dir.empty() && q->parent) q = q->parent;
        if (q && !q->dir.empty()) roots.push_back(q->dir);
        if (std::string home = homeDir(); !home.empty()) roots.push_back(home + "/Documents/Moritz");
        if (!musil::path_under(musil::resolved_path(path), roots))
            return fail("not written: " + path + " is outside the patch's folder and ~/Documents/Moritz");
        std::string from = referenceFolder();
        try {
            sc::writeFile(*score_, path, from);      // its references named from the file's folder
        } catch (const std::exception& e) {
            std::string m = e.what();
            return fail(m.rfind("score: ", 0) == 0 ? m.substr(7) : m);
        }
        if (sc::folderOf(sc::normalPath(path)) != sc::normalPath(from)) {   // the score is that file's now: named from there
            auto c = score_->clone();
            sc::rebasePaths(*c, from, sc::folderOf(sc::normalPath(path)));
            c->path = path;
            replace(c);
        } else {
            score_->path = path;                     // (its document, if one is open, keeps its history)
        }
        return true;
    }
    // add [note] at dur row pitch [dyn [tech]] | add [note] at row pitch ... : the words read as the score file's (note
    // ...) form (a note without its duration is a beat long)
    void add(const Message& m0) {
        Message m = m0;
        if (m.size() > 1 && m[1].isFloat()) m.insert(m.begin() + 1, Atom("note"));            // the kind left out: a note
        if (m.size() > 4 && m[1].is("note") && m[2].isFloat() && !m[3].isFloat())              // no duration: a beat
            m.insert(m.begin() + 3, Atom((float)(60.0 / std::max(1.0, score_->bpm))));
        if (m.size() < 3 || !m[1].isSymbol()) { patch->print("score: add note|sound|msg at ..."); return; }
        std::string form = "(score \"x\" (";
        for (size_t k = 1; k < m.size(); ++k) {
            if (k > 1) form += ' ';
            if (k == 4 && m[1].is("sound")) form += sc::quoted(m[k].str());     // add sound at dur file: the file
            else form += m[k].isFloat() ? sc::numText(m[k].f) : sc::wordText(m[k].s);
        }
        form += "))";
        try {
            auto one = sc::readText(form, here());
            auto s = score_->clone();
            for (auto& e : one->events()) {
                auto c = std::make_shared<sc::Event>(*e);
                c->id = 0;
                s->insert(c);
            }
            replace(s);
        } catch (const std::exception& e) { complain(e); }
    }
    // select all | none | id... | row name | time t0 t1
    void select(const Message& m) {
        RollCore& c = roll();
        c.syncSelection();                       // the document's selection, then this one in its place
        c.path.clear();
        c.selection.clear();
        std::string how = m.size() > 1 && m[1].isSymbol() ? m[1].s : "";
        if (how == "all") c.selectAll();
        else if (how == "row" && m.size() > 2) { for (auto& e : score_->events()) if (e->row == m[2].str()) c.selection.insert(e->id); }
        else if (how == "time" && m.size() > 2) {
            double t0 = argf(m, 2, 0.f), t1 = argf(m, 3, 1e9f);
            score_->query(t0, t1, [&](const sc::Event& e) { c.selection.insert(e.id); });
        } else if (how.empty()) {
            for (size_t k = 1; k < m.size(); ++k) if (m[k].isFloat() && m[k].f >= 1 && score_->find((uint64_t)m[k].f)) c.selection.insert((uint64_t)m[k].f);
        } else if (how != "none") { patch->print("score: select all, none, ids, row name or time t0 t1"); return; }
        c.syncSelection();                       // the views of the score's window show it too
        if (patch) patch->redraw();
    }
    // a transformation of the selection (all of it when nothing is selected), as Musil's (music/score_transform.h)
    void transformBy(const Message& m) {
        ScoreTransform t;
        const Atom& h = m[0];
        auto num = [&](size_t k, double def) { float v = argf(m, k, (float)def); return std::isfinite(v) ? (double)v : def; };
        t.seed = (uint32_t)num(h.is("humanize") ? 3 : 2, 1);
        if (h.is("transpose")) { t.kind = ScoreTransform::Transpose; t.a = num(1, 0); }
        else if (h.is("invert")) {
            t.kind = ScoreTransform::Invert;
            t.a = m.size() > 1 ? (m[1].isFloat() ? m[1].f : sc::pitchOfName(m[1].str())) : NAN;
            if (m.size() > 1 && t.a < 0) { patch->print("score: invert around what? (a pitch: 60 or C4)"); return; }
        }
        else if (h.is("retrograde")) t.kind = ScoreTransform::Retrograde;
        else if (h.is("augment") || h.is("diminish")) {
            t.kind = ScoreTransform::Augment;
            double k = num(1, 2);
            if (!(k > 0)) { patch->print("score: " + h.s + " by a factor > 0"); return; }
            t.a = h.is("augment") ? k : 1 / k;
        }
        else if (h.is("shift")) { t.kind = ScoreTransform::Shift; t.a = num(1, 0); }
        else if (h.is("quantize")) { t.kind = ScoreTransform::Quantize; t.a = num(1, roll().snapSeconds() > 0 ? roll().snapSeconds() : 0.25); t.b = num(2, 1); }
        else if (h.is("humanize")) { t.kind = ScoreTransform::Humanize; t.a = num(1, 20) / 1000; t.b = num(2, 1.5); }
        else if (h.is("thin")) { t.kind = ScoreTransform::Thin; t.a = num(1, 0.5); }
        else if (h.is("density")) {
            t.kind = ScoreTransform::Density;
            for (size_t k = 1; k < m.size(); ++k) if (m[k].isFloat()) t.curve.push_back(std::clamp((double)m[k].f, 0.0, 1.0));
            t.seed = 1;
        }
        RollCore& c = roll();
        c.syncSelection();                       // what is selected in any view of the score
        Ids ids;                                 // what of it is in the score still (none of it: all of the score)
        if (c.path.empty())
            for (uint64_t id : c.selectedIds()) if (score_->find(id)) ids.push_back(id);
        if (ids.empty() && !c.selection.empty()) {
            c.selection.clear();
            c.syncSelection();
        }
        auto s = score_->clone();
        try {
            if (!applyTransform(*s, ids, t)) return;
        } catch (const std::exception& e) { complain(e); return; }
        for (auto it = c.selection.begin(); it != c.selection.end();) it = s->find(*it) ? std::next(it) : c.selection.erase(it);
        replace(s);
    }
    // play: the transport from where it is, or from the start when it is at the score's end or past it (it would play
    // nothing); already playing the score: nothing. A play started here stops at the score's end, unless the transport
    // loops or anyone else started, stopped or located it since (then it is the patch's clock, and goes on)
    void playHere() {
        if (!t_) return;
        int lap;
        double pos;
        t_->where(patch, lap, pos);
        bool past = end_ > 0 && !t_->looping() && pos >= (double)end_ - 0.5;
        if (t_->playing() && !past) return;
        if (past) t_->locate(patch, 0);
        t_->play(patch);
        ownPlay_ = end_ > 0;
        ownChanges_ = t_->changes();
    }
    bool ownPlay() const { return ownPlay_ && t_ && t_->playing() && t_->changes() == ownChanges_; }
    // ---- audition: the files it plays (decoded once: the plan's already are), the notes-off to send
    std::shared_ptr<const SampleBuffer> soundOf(const std::string& path) {
        auto d = decoded_.find(path);
        if (d != decoded_.end()) return d->second;
        auto b = std::make_shared<SampleBuffer>();
        std::string err;
        if (!decodeAudioFile(path, *b, err)) return nullptr;
        decoded_.emplace(path, b);                   // (let go at the next rebuild unless the plan reads it)
        return b;
    }
    static std::string fileName(const std::string& p) {
        size_t k = p.find_last_of("/\\");
        return k == std::string::npos ? p : p.substr(k + 1);
    }
    bool notesConnected() const {
        for (auto& c : patch->conns)
            if (c.from == this && c.outlet == nOut_) return true;
        return false;
    }
    // a tone at a pitch (a note with no sample and no synth to play it): a few partials, faded by the preview
    static std::shared_ptr<const SampleBuffer> tone(double pitch, int vel, double dur) {
        auto b = std::make_shared<SampleBuffer>();
        b->channels = 1;
        b->sr = 44100;
        b->frames = (long)(std::clamp(dur, 0.05, 4.0) * b->sr);
        b->data.assign((size_t)b->frames, 0.f);
        double f = 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0), amp = 0.25 * vel / 127.0;
        for (long i = 0; i < b->frames; ++i) {
            double t = (double)i / b->sr, decay = std::exp(-3.0 * t);
            double x = std::sin(2 * dsp::kPi * f * t) + 0.3 * std::sin(4 * dsp::kPi * f * t) + 0.12 * std::sin(6 * dsp::kPi * f * t);
            b->data[(size_t)i] = (float)(amp * decay * x / 1.42);
        }
        return b;
    }
    std::vector<std::pair<double, Message>> offs_;   // auditioned notes' ends: when, and the note-off
    void armOffs() {
        for (auto& o : offs_) {
            double t = o.first;
            patch->scheduleAt(this, t, [this, t] {
                for (size_t k = 0; k < offs_.size();) {
                    if (offs_[k].first == t) {
                        Message m = offs_[k].second;
                        offs_.erase(offs_.begin() + (long)k);
                        outlet(nOut_, m);
                    } else {
                        ++k;
                    }
                }
            });
        }
    }
    void edited() {
        rebuild();
        if (patch) patch->redraw();
    }
    // the score is replaced whole (clear, read, a score sent): the selection, which named events of the old one, goes,
    // in the box and in every view of its document
    void dropSelection() {
        if (inline_) {
            inline_->selection.clear();
            if (inline_->target()) inline_->syncSelection();
        }
        if (auto d = ScoreDoc::find(score_.get())) d->clearSelection();
    }

    // ---- the plan: made on the GUI thread, published; the control events kept here
    // a decoded file (read once, kept while an event reads it); nullptr when it can't be read (said once)
    std::shared_ptr<const SampleBuffer> decodedFile(const std::string& path) {
        auto d = decoded_.find(path);
        if (d == decoded_.end()) {
            auto b = std::make_shared<SampleBuffer>();
            std::string err;
            if (!decodeAudioFile(path, *b, err)) { patch->print("score: " + err); b.reset(); }
            d = decoded_.emplace(path, b).first;
        }
        return d->second;
    }
    // a file's length in seconds (asked once: -1 when it can't be read)
    double fileLength(const std::string& path) {
        auto it = lengths_.find(path);
        if (it != lengths_.end()) return it->second;
        double s = mt::fileSeconds(path);
        lengths_[path] = s;
        return s;
    }
    // a voice's gains: its pan (a source mixed down to one channel), then its balance (a source with a channel for each
    // output: a stereo file panned by its clip's azimuth, and by its track's when that is not a balance of the mix)
    int pushGains(ScorePlan& plan, double az) const {
        int at = (int)plan.gains.size();
        double g[16];
        sc::panGains(az, nOut_, g);
        for (int c = 0; c < nOut_; ++c) plan.gains.push_back((float)g[c]);
        sc::balanceGains(az, nOut_, g);
        for (int c = 0; c < nOut_; ++c) plan.gains.push_back((float)g[c]);
        return at;
    }
    // a voice playing a region of a file: decoded into memory, or read from disk while it plays when it is long (the
    // stream kept for the clip from plan to plan, so what is read stays read)
    bool fileVoice(ScorePlan& plan, std::map<std::string, int>& files, std::map<std::string, int>& streams, ScorePlan::Voice& v,
                   const std::string& path, const std::string& streamKey) {
        if (v.len <= 0) return false;
        double len = streamKey.empty() ? -1 : fileLength(path);
        if (len > mt::streamThreshold.load()) {
            auto it = streams.find(streamKey);
            if (it == streams.end()) {
                auto& s = streams_[streamKey];
                if (!s || s->path() != path) {
                    std::string err;
                    s = mt::DiskStream::open(path, err);
                    if (!s) { patch->print("score: " + err); return false; }
                }
                s->pin((int64_t)std::floor(v.from * s->sr()));          // where the clip starts: always there
                it = streams.emplace(streamKey, (int)plan.streams.size()).first;
                plan.streams.push_back(s);
            }
            v.stream = it->second;
            return true;
        }
        auto it = files.find(path);
        if (it == files.end()) {
            auto d = decodedFile(path);
            if (!d) return false;
            it = files.emplace(path, (int)plan.files.size()).first;
            plan.files.push_back(d);
        }
        v.file = it->second;
        return true;
    }
    // the strip of a track, a bus or the master (kept from plan to plan: the interface writes its atomics)
    std::shared_ptr<mt::StripParams> stripNamed(const std::string& name) {
        auto& p = strips_[name];
        if (!p) p = std::make_shared<mt::StripParams>();
        return p;
    }
    // ---- the patches the box runs: a track's insert or instrument, a patch clip (slots, never reordered: the plan
    // names them by their place, which is the place of their plans among the box's inner patches)
    struct Slot {
        std::string key, file;             // what it is for; the file loaded ("" empty)
        std::unique_ptr<Patch> p;
        bool used = false;
    };
    int slotFor(const std::string& key, const std::string& file, bool& changed) {
        Slot* s = nullptr;
        int k = 0;
        for (; k < (int)slots_.size(); ++k)
            if (slots_[(size_t)k]->key == key) { s = slots_[(size_t)k].get(); break; }
        if (!s) {
            auto ns = std::make_unique<Slot>();
            ns->key = key;
            ns->p = std::make_unique<Patch>();
            ns->p->parent = patch;
            ns->p->owner = this;
            s = ns.get();
            k = (int)slots_.size();
            slots_.push_back(std::move(ns));
            changed = true;
        }
        s->used = true;
        if (s->file != file) {
            if (!s->file.empty()) s->p->clear();
            s->file = file;
            std::string err;
            if (!file.empty() && !loadPatchInto(*s->p, file, err)) {
                patch->print("score: " + file + ": " + err);
            }
            numberPatchPorts(*s->p);
            if (key.rfind("clip:", 0) != 0) s->p->loadbangAll();   // an insert or an instrument starts now; a clip at its start
            changed = true;
        }
        return k;
    }
    // what the plan depends on but for the strips' values (a fader, a pan on two channels, mute, solo, a send's level):
    // when only those changed, the strips take them and the plan stays
    uint64_t planKey(const std::vector<sc::Placed>& flat, double sr) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const std::string& s) {
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
            h ^= 0xff;
            h *= 1099511628211ull;
        };
        std::string t;
        mix(std::to_string(sr) + " " + std::to_string(nOut_) + " " + referenceFolder() + " " + std::to_string(mt::streamThreshold.load()));
        for (auto& r : score_->rows) {
            sc::Row c = r;
            for (const char* k : {"gain", "mute", "solo", "monitor", "height", "lanes"}) sc::eraseField(c.extra, k);
            if (nOut_ == 2) sc::eraseField(c.extra, "az");
            mt::RowSettings rs = mt::rowSettings(r);
            for (auto& x : rs.sends) x.gain = 0;
            rs.monitor = false;
            mt::setRowSettings(c, rs);
            t = c.name;
            sc::writeFields(c.extra, t);
            mix(t);
        }
        mix(mt::sessionSettings(*score_).masterInsert);
        for (auto& p : flat) {
            t.clear();
            sc::writeEvent(p.ev, t, 0);
            mix(t);
            mix(p.top ? p.top->row : std::string());
            mix(p.origin ? p.origin->path : std::string());
        }
        return h;
    }
    // the strips' values from the score (the fast way: nothing else changed)
    void loadStrips() {
        for (auto& r : score_->rows) mt::loadStrip(*stripNamed(r.name), r);
        stripNamed(mt::kMaster)->gain.store((float)mt::sessionSettings(*score_).masterGain, std::memory_order_relaxed);
    }
    void rebuild() {
        if (!patch) return;
        double sr = patch->root()->sampleRate.load();
        std::vector<sc::Placed> flat;
        try { sc::flatten(*score_, flat); }
        catch (const std::exception& e) { complain(e); flat.clear(); }
        uint64_t key = planKey(flat, sr);
        if (plan_.get() && key == planKey_) {           // the strips only: their atomics, no new plan
            loadStrips();
            return;
        }
        planKey_ = key;
        ++plans_;
        auto plan = std::make_unique<ScorePlan>();
        plan->sr = sr;
        plan->nOut = nOut_;
        auto at = [sr](double sec) { return (int64_t)std::floor(sec * sr); };   // as Musil's score-render places them
        // the channels: the rows (tracks and buses), then the rows only events name, then the events with none
        std::vector<std::string> names;
        std::map<std::string, int> index;
        auto channelOf = [&](const std::string& n) {
            auto it = index.find(n);
            if (it != index.end()) return it->second;
            index[n] = (int)names.size();
            names.push_back(n);
            return (int)names.size() - 1;
        };
        for (auto& r : score_->rows) channelOf(r.name);
        for (auto& p : flat) {
            const std::string& track = p.top ? p.top->row : p.ev.row;
            if (p.ev.kind == sc::Kind::Message && p.ev.source.rfind("strip.", 0) != 0) continue;
            channelOf(track);
        }
        std::vector<ScorePlan::Channel> chans(names.size());
        std::vector<mt::RowSettings> settings(names.size());
        for (size_t c = 0; c < names.size(); ++c) {
            ScorePlan::Channel& ch = chans[c];
            ch.name = names[c];
            auto sp = stripNamed(ch.name);
            const sc::Row* row = nullptr;
            for (auto& r : score_->rows) if (r.name == ch.name) row = &r;
            sc::Row none;
            none.name = ch.name;
            mt::loadStrip(*sp, row ? *row : none);
            settings[c] = mt::rowSettings(row ? *row : none);
            ch.strip = sp.get();
            plan->strips.push_back(sp);
            ch.bus = settings[c].bus;
            ch.armed = settings[c].arm;
            ch.input = settings[c].input - 1;
            ch.inputCh = settings[c].inputCh;
            ch.balance = nOut_ == 2;
            plan->anyArmed = plan->anyArmed || (ch.armed && ch.input >= 0);
        }
        auto masterStrip = stripNamed(mt::kMaster);
        mt::SessionSettings ss = mt::sessionSettings(*score_);
        masterStrip->gain.store((float)ss.masterGain, std::memory_order_relaxed);
        plan->master = masterStrip.get();
        plan->strips.push_back(masterStrip);
        // the events
        const sc::Mixer mixer(*score_);
        std::vector<Ctl> ctl;
        std::map<std::string, int> files, bufs, streams;
        std::map<std::pair<int, std::string>, ScorePlan::Curve> curves;
        for (auto& s : slots_) s->used = false;
        bool slotsChanged = false;
        end_ = 0;
        for (auto& p : flat) {
            const sc::Event& e = p.ev;
            end_ = std::max(end_, at(e.end()));
            const std::string& track = p.top ? p.top->row : e.row;
            int c = index.count(track) ? index[track] : -1;
            const sc::Strip strip = mixer.of(track);
            double baked = c >= 0 && !chans[(size_t)c].balance ? strip.az : 0;   // the track's pan, when not a balance
            if (e.kind == sc::Kind::Sound && c >= 0) {
                ScorePlan::Voice v;
                v.start = at(e.at);
                v.len = at(e.dur);
                if (v.len <= 0) continue;
                v.from = e.from;
                v.to = e.to;
                v.gain = (float)e.gain;
                v.fadeIn = std::clamp<int64_t>(at(sc::numField(e.extra, "fade-in", 0)), 0, v.len);
                v.fadeOut = std::clamp<int64_t>(at(sc::numField(e.extra, "fade-out", 0)), 0, v.len);
                v.inShape = fadeShape(e.extra, "fade-in-shape");
                v.outShape = fadeShape(e.extra, "fade-out-shape");
                if (e.buffer) {
                    auto it = bufs.find(e.source);
                    if (it == bufs.end()) {
                        it = bufs.emplace(e.source, (int)plan->bufs.size()).first;
                        plan->bufs.push_back(std::make_unique<BufferRef>(e.source));
                        plan->bufs.back()->resolve(patch);
                    }
                    v.buf = it->second;
                } else if (!fileVoice(*plan, files, streams, v, sourcePath(e.source, p.origin),
                                      std::to_string(e.id) + "|" + e.source + "|" + sc::numText(e.from) + "|" + std::to_string((uintptr_t)p.origin))) {
                    continue;
                }
                v.gains = pushGains(*plan, e.az + baked);
                chans[(size_t)c].voices.push_back(v);
            } else if (e.kind == sc::Kind::Note) {
                if (e.variant == "chord") continue;
                // a note with a sound of its own (orchidea's: its sample) plays it, faster or slower by its pitch against
                // the sample's (sound-pitch; without it, by its cents)
                if (const sc::Datum* snd = sc::field(e.extra, "sound"); c >= 0 && snd && (snd->kind == sc::Datum::Str || snd->kind == sc::Datum::Sym) && !snd->s.empty()) {
                    const sc::Datum* sp = sc::field(e.extra, "sound-pitch");
                    double base = sp && sp->kind == sc::Datum::Num ? sp->num : e.pitch;
                    ScorePlan::Voice v;
                    v.start = at(e.at);
                    v.len = at(e.dur);
                    v.to = -1;
                    v.gain = (float)e.gain;
                    double speed = std::pow(2.0, (e.sounding() - base) / 12.0);
                    v.speed = std::isfinite(speed) && speed > 0 ? speed : 1;
                    if (fileVoice(*plan, files, streams, v, sourcePath(snd->s, p.origin), "")) {
                        v.gains = pushGains(*plan, e.az + baked);
                        chans[(size_t)c].voices.push_back(v);
                    }
                }
                int vel = (int)std::lround(sc::velocityOf(e.dyn) * e.gain);
                Message msg{Atom((float)e.sounding()), Atom((float)std::clamp(vel, 1, 127)), Atom((float)(e.dur * 1000.0)),
                            Atom(e.row.empty() ? std::string("-") : e.row), Atom(e.dyn.empty() ? std::string("-") : e.dyn),
                            Atom(e.tech.empty() ? std::string("-") : e.tech)};
                ctl.push_back(Ctl{at(e.at), nOut_, std::move(msg), false, Ctl::Note, track, -1});
            } else if (e.kind == sc::Kind::Message) {
                if (e.source.rfind("strip.", 0) == 0) {          // a lane of the track's strip: played on the audio thread
                    if (c < 0 || e.values.empty() || e.values[0].kind != sc::Datum::Num) continue;
                    std::string what = e.source.substr(6);
                    ScorePlan::Curve cv;
                    if (what == "gain") cv.what = ScorePlan::Curve::Gain;
                    else if (what == "pan") cv.what = ScorePlan::Curve::Pan;
                    else if (what == "mute") cv.what = ScorePlan::Curve::Mute;
                    else if (what.rfind("send.", 0) == 0) {
                        cv.what = ScorePlan::Curve::Send;
                        cv.send = -1;
                        auto& sends = settings[(size_t)c].sends;
                        for (size_t k = 0; k < sends.size(); ++k) if (sends[k].bus == what.substr(5)) cv.send = (int)k;
                        if (cv.send < 0) continue;
                    } else continue;
                    auto& slot = curves[{c, what}];
                    if (slot.pts.empty()) { slot.what = cv.what; slot.send = cv.send; }
                    slot.pts.push_back({at(e.at), (float)e.values[0].num});
                    continue;
                }
                Message msg{Atom(e.source.empty() ? std::string("-") : e.source)};
                for (auto& d : e.values) appendDatum(msg, d);
                ctl.push_back(Ctl{at(e.at), nOut_ + 1, std::move(msg), !e.source.empty(), Ctl::Plain, std::string(), -1});
            } else if (e.kind == sc::Kind::Patch) {
                // a patch clip: its patch (a .moritz file) loaded now, run while the clip plays (loadbang at its start,
                // its timers stopped at its end); it leaves the messages outlet too, as `patch name duration-ms args...`
                Message msg{Atom("patch"), Atom(e.source), Atom((float)(e.dur * 1000.0))};
                for (auto& d : e.values) appendDatum(msg, d);
                ctl.push_back(Ctl{at(e.at), nOut_ + 1, std::move(msg), false, Ctl::Plain, std::string(), -1});
                std::string file = sourcePath(e.source, p.origin);
                if (c >= 0 && endsWith(file, ".moritz") && std::filesystem::exists(std::filesystem::u8path(file))) {
                    int k = slotFor("clip:" + std::to_string(e.id) + ":" + std::to_string((uintptr_t)p.origin), file, slotsChanged);
                    ScorePlan::Clip cl;
                    cl.slot = k;
                    cl.start = at(e.at);
                    cl.end = at(e.end());
                    chans[(size_t)c].clips.push_back(cl);
                    ctl.push_back(Ctl{cl.start, -1, {}, false, Ctl::ClipStart, std::string(), k});
                    ctl.push_back(Ctl{cl.end, -1, {}, false, Ctl::ClipEnd, std::string(), k});
                }
            }
        }
        for (auto& kv : curves) {
            auto& cv = kv.second;
            std::stable_sort(cv.pts.begin(), cv.pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            chans[(size_t)kv.first.first].curves.push_back(std::move(cv));
        }
        // the sounds of a channel by onset
        for (auto& ch : chans) {
            std::stable_sort(ch.voices.begin(), ch.voices.end(), [](const ScorePlan::Voice& a, const ScorePlan::Voice& b) { return a.start < b.start; });
            for (auto& v : ch.voices) ch.maxLen = std::max(ch.maxLen, v.len);
        }
        // routing: a track goes to its bus (out) or the master, and sends to buses; the channels ordered so that each
        // comes after those it takes from (a loop of buses is broken: to the master)
        std::vector<std::vector<ScorePlan::Source>> inputs(chans.size());
        for (size_t c = 0; c < chans.size(); ++c) {
            const mt::RowSettings& rs = settings[c];
            if (!rs.out.empty() && index.count(rs.out) && (size_t)index[rs.out] != c) {
                inputs[(size_t)index[rs.out]].push_back({(int)c, -1});
                chans[c].toMaster = false;
            }
            for (size_t k = 0; k < rs.sends.size(); ++k)
                if (index.count(rs.sends[k].bus) && (size_t)index[rs.sends[k].bus] != c)
                    inputs[(size_t)index[rs.sends[k].bus]].push_back({(int)c, (int)k});
        }
        std::vector<int> order, indeg(chans.size(), 0);
        for (size_t c = 0; c < chans.size(); ++c) indeg[c] = (int)inputs[c].size();
        std::vector<char> placed(chans.size(), 0);
        while (order.size() < chans.size()) {
            bool progress = false;
            for (size_t c = 0; c < chans.size(); ++c) {
                if (placed[c] || indeg[c] > 0) continue;
                placed[c] = 1;
                order.push_back((int)c);
                progress = true;
                for (size_t d = 0; d < chans.size(); ++d)
                    for (auto& s : inputs[d]) if (s.ch == (int)c) --indeg[d];
            }
            if (progress) continue;
            // a loop: the first channel left takes nothing; what went to it goes to the master
            size_t c = 0;
            while (placed[c]) ++c;
            patch->print("score: the buses of " + chans[c].name + " go round in a loop: what goes to it goes to the master");
            for (auto& s : inputs[c]) if (s.send < 0) chans[(size_t)s.ch].toMaster = true;
            inputs[c].clear();
            indeg[c] = 0;
        }
        // the channels' places once ordered: those fed by none first (they may run on several cores)
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return inputs[(size_t)a].empty() && !inputs[(size_t)b].empty(); });
        std::vector<int> where(chans.size());
        for (size_t k = 0; k < order.size(); ++k) where[(size_t)order[k]] = (int)k;
        for (size_t k = 0; k < order.size(); ++k) {
            ScorePlan::Channel ch = std::move(chans[(size_t)order[k]]);
            for (auto s : inputs[(size_t)order[k]]) ch.inputs.push_back({where[(size_t)s.ch], s.send});
            if (ch.inputs.empty()) plan->nFree = (int)k + 1;
            plan->channels.push_back(std::move(ch));
        }
        // the track patches: an insert, an instrument (and the master's insert)
        for (size_t k = 0; k < plan->channels.size(); ++k) {
            ScorePlan::Channel& ch = plan->channels[k];
            const mt::RowSettings& rs = settings[(size_t)order[k]];
            if (!rs.insert.empty()) ch.insert = slotFor("insert:" + ch.name, sourcePath(rs.insert, score_.get()), slotsChanged);
            if (!rs.instrument.empty()) ch.instrument = slotFor("instrument:" + ch.name, sourcePath(rs.instrument, score_.get()), slotsChanged);
        }
        if (!ss.masterInsert.empty()) plan->masterInsert = slotFor("insert:" + std::string(mt::kMaster), sourcePath(ss.masterInsert, score_.get()), slotsChanged);
        for (auto& s : slots_)
            if (!s->used && !s->file.empty()) {               // no longer used: emptied (its plan let go of first)
                s->p->clear();
                s->file.clear();
                slotsChanged = true;
            }
        // the buffers: a channel's output and its insert's, the master's
        size_t per = (size_t)nOut_ * dsp::kBlock;
        plan->arena.assign(per * 2 * (plan->channels.size() + 1), 0.f);
        for (size_t k = 0; k < plan->channels.size(); ++k) {
            plan->channels[k].buf = plan->arena.data() + per * 2 * k;
            plan->channels[k].tmp = plan->channels[k].buf + per;
        }
        plan->masterBuf = plan->arena.data() + per * 2 * plan->channels.size();
        plan->masterTmp = plan->masterBuf + per;
        if (plan->nFree >= 2) mt::TrackPool::get();     // the workers made here, not on the audio thread
        std::stable_sort(ctl.begin(), ctl.end(), [](const Ctl& a, const Ctl& b) { return a.pos < b.pos; });
        ctl_ = std::move(ctl);
        std::set<std::string> rows;
        for (auto& r : score_->rows) rows.insert(r.name);
        for (auto& e : score_->events()) if (!e->row.empty()) rows.insert(e->row);
        rows_ = rows.size();
        // files no event reads any more are let go (a plan the audio thread still uses keeps its own)
        for (auto it = decoded_.begin(); it != decoded_.end();) it = files.count(it->first) ? std::next(it) : decoded_.erase(it);
        for (auto it = streams_.begin(); it != streams_.end();) it = streams.count(it->first) ? std::next(it) : streams_.erase(it);
        bool armed = plan->anyArmed;
        // the slots' plans first (the box's inner patches compiled), then the plan naming them
        if (slotsChanged) patch->rebuild();
        plan_.set(patch, std::move(plan));
        if (armed != armed_) {                            // input wanted or no longer: the device follows
            armed_ = armed;
            if (mt::inputNeedChanged) mt::inputNeedChanged();
        }
        follow();
    }
    static uint8_t fadeShape(const sc::Fields& f, const char* key) { return (uint8_t)sc::fadeShapeOf(f, key); }

    // the files read from disk told where the transport is (a locate: read there now) and where its loop starts (kept)
    void prepareStreams() {
        const ScorePlan* p = plan_.get();
        if (!p || p->streams.empty() || !t_) return;
        int lap;
        double pos;
        t_->where(patch, lap, pos);
        int64_t here = (int64_t)pos, loopAt = t_->looping() ? t_->loopStart() : -1;
        for (auto& ch : p->channels)
            for (auto& v : ch.voices) {
                if (v.stream < 0) continue;
                mt::DiskStream* s = p->streams[(size_t)v.stream].get();
                double rate = s->sr() / p->sr * v.speed;
                auto frameAt = [&](int64_t q) { return (int64_t)std::floor(v.from * s->sr() + (double)(q - v.start) * rate); };
                if (here >= v.start && here < v.start + v.len) s->hint(frameAt(here));
                if (loopAt >= v.start && loopAt < v.start + v.len) s->pin(frameAt(loopAt));
            }
    }
    // ---- the control events on the scheduler, following the transport
    // in its patch still (a removed box is kept until the audio thread lets go of it: it follows nothing then)
    bool inPatch() const {
        for (auto& b : patch->boxes) if (b.get() == this) return true;
        return false;
    }
    void follow() {
        if (!inPatch()) return;
        patch->cancel(this);
        prepareStreams();
        armOffs();                               // an auditioned note's end still to come
        if (!t_ || !t_->playing()) return;
        double pos;
        t_->where(patch, lap_, pos);
        ended_ = !t_->looping() && pos >= (double)end_ && end_ > 0;
        int64_t from = (int64_t)std::ceil(pos - 1e-6);
        if (firing_ && lap_ == firedLap_) from = std::max(from, firedPos_ + 1);   // what was just sent is not sent again
        cursor_ = (size_t)(std::lower_bound(ctl_.begin(), ctl_.end(), from, [](const Ctl& c, int64_t p) { return c.pos < p; }) - ctl_.begin());
        schedule();
    }
    void schedule() {
        bool looping = t_->looping();
        for (int guard = 0; guard < 2; ++guard) {
            if (cursor_ < ctl_.size() && (!looping || ctl_[cursor_].pos < t_->loopEnd())) {
                double t = t_->timeAt((double)ctl_[cursor_].pos, lap_);
                if (!std::isnan(t)) { patch->scheduleAt(this, t, [this] { fire(); }); return; }
            }
            if (!looping) break;
            // the rest of this pass has nothing: the next pass, from the loop's start
            ++lap_;
            cursor_ = (size_t)(std::lower_bound(ctl_.begin(), ctl_.end(), t_->loopStart(), [](const Ctl& c, int64_t p) { return c.pos < p; }) - ctl_.begin());
            if (cursor_ >= ctl_.size() || ctl_[cursor_].pos >= t_->loopEnd()) return;   // nothing in the loop
        }
        if (!looping && !ended_ && end_ > 0) {             // after the last event: its end, once
            double t = t_->timeAt((double)end_, 0);
            if (!std::isnan(t))
                patch->scheduleAt(this, t, [this] {
                    ended_ = true;
                    if (ownPlay()) {                       // its own play: the transport stops on the end's sample
                        ownPlay_ = false;
                        t_->stop(patch);
                    }
                    outlet(nOut_ + 2, {Atom("end")});
                });
        }
    }
    // a track sounds: not muted, and no other soloed (its strip as the interface left it)
    bool sounds(const std::string& track) const {
        bool any = false;
        for (auto& kv : strips_) if (kv.first != mt::kMaster) any = any || kv.second->solo.load(std::memory_order_relaxed);
        auto it = strips_.find(track);
        if (it == strips_.end()) return !any;
        return !it->second->mute.load(std::memory_order_relaxed) && (!any || it->second->solo.load(std::memory_order_relaxed));
    }
    void fire() {
        if (cursor_ >= ctl_.size()) return;
        int64_t p = ctl_[cursor_].pos;
        std::vector<Ctl> due;                   // copied: an outlet may replace the score (and the plan)
        while (cursor_ < ctl_.size() && ctl_[cursor_].pos == p) due.push_back(ctl_[cursor_++]);
        uint64_t version = score_->version();
        auto keep = score_;
        firing_ = true;
        firedPos_ = p;
        firedLap_ = lap_;
        for (auto& c : due) {
            if (c.kind == Ctl::ClipStart || c.kind == Ctl::ClipEnd) {        // a patch clip starts (loadbang) or stops
                if (c.slot >= 0 && c.slot < (int)slots_.size()) {
                    Patch* sp = slots_[(size_t)c.slot]->p.get();
                    if (c.kind == Ctl::ClipStart) sp->loadbangAll();
                    else sp->forEachBox([this](Box* b) { patch->cancel(b); });
                }
                continue;
            }
            if (c.kind == Ctl::Note) {                   // its track's strip says whether it sounds; its instrument plays it
                if (!sounds(c.track)) continue;
                Patch* inst = slotPatch("instrument:" + c.track);
                Message msg = c.msg;
                auto it = strips_.find(c.track);
                if (it != strips_.end() && msg.size() > 1) {
                    float g = it->second->gain.load(std::memory_order_relaxed);
                    msg[1] = Atom((float)std::clamp((int)std::lround(msg[1].f * g), 1, 127));
                }
                if (inst) {
                    Message nm{Atom("note")};
                    nm.insert(nm.end(), msg.begin(), msg.begin() + std::min<size_t>(3, msg.size()));
                    sendToPatchInlet(*inst, 0, nm);
                } else {
                    outlet(c.outlet, msg);
                }
                if (score_ != keep || score_->version() != version || !t_->playing()) { firing_ = false; return; }
                continue;
            }
            if (c.send) patch->sendTo(c.msg[0].str(), Message(c.msg.begin() + 1, c.msg.end()));   // the lanes' receives
            outlet(c.outlet, c.msg);
            if (score_ != keep || score_->version() != version || !t_->playing()) { firing_ = false; return; }   // rebuild() or the transport took over
        }
        firing_ = false;
        schedule();
    }
};

// ---- the audio thread
// where the transport is in a block: runs of positions going on by one a sample
struct Run {
    int i = 0, len = 0;
    int64_t pos = 0;
};
// a block as the channels share it
struct BlockCtx {
    ScoreBox* box;
    const ScorePlan* plan;
    Run runs[dsp::kBlock];
    int nruns = 0, n = 0;
    int64_t pos0 = 0;                    // where the block starts (or the transport stands)
    bool anySolo = false;
    const float* in = nullptr;           // the device's input in this block (interleaved inCh), or none
    int inCh = 0;
};

// a file read from disk, sample by sample: the chunk holding each frame (a miss: false, the voice is silent then)
struct StreamSource {
    mt::DiskStream* s;
    mt::DiskStream::View v[2];
    int nv = 0;
    bool failed = false;
    explicit StreamSource(mt::DiskStream* st) : s(st) {}
    ~StreamSource() {
        for (int k = 0; k < nv; ++k) mt::DiskStream::release(v[k]);
    }
    const float* frame(int64_t i) {
        if (i < 0 || i >= s->frames()) return nullptr;
        for (int k = 0; k < nv; ++k)
            if (i >= v[k].lo && i <= v[k].hi) return v[k].data + (i - v[k].lo) * s->channels();
        if (nv == 2) {
            mt::DiskStream::release(v[0]);
            v[0] = v[1];
            nv = 1;
        }
        if (!s->view(i, v[nv])) {
            failed = true;
            return nullptr;
        }
        ++nv;
        return v[nv - 1].data + (i - v[nv - 1].lo) * s->channels();
    }
};

inline float shapeOf(uint8_t shape, float x) { return sc::fadeGain((sc::FadeShape)shape, x); }

// a voice's samples in [pos, pos + len) added into buf (channel-major, kBlock a channel) from offset i. Src: interp(sp,
// c) as SampleBuffer's, frames, channels, sr; false from ok() stops it (a stream's miss)
template <class Src>
void renderVoice(const ScorePlan::Voice& v, Src& src, const float* g, int nOut, float* buf, int i, int64_t pos, int len, double sr) {
    double rate = src.sr() / sr * v.speed;                // source frames a sample
    double first = std::floor(v.from * src.sr());
    double last = v.to >= 0 ? std::min((double)src.frames(), std::floor(v.to * src.sr())) : (double)src.frames();
    double avail = (last - first) / rate;                 // samples of the source after its start
    int64_t fade = v.fadeOut;                             // a clip shorter than its source: faded out at least a little
    const uint8_t outShape = v.outShape;
    if (avail > (double)v.len) fade = std::max(fade, std::max<int64_t>(1, std::min<int64_t>((int64_t)std::floor(0.02 * sr), v.len / 4)));
    int64_t p1 = pos + len;
    int64_t k0 = std::max<int64_t>(0, pos - v.start), k1 = std::min<int64_t>(v.len, p1 - v.start);
    bool direct = src.channels() == nOut;
    for (int64_t k = k0; k < k1; ++k) {
        if ((double)k >= avail) break;
        double sp = first + (double)k * rate;
        if (sp >= last) break;
        float amp = v.gain;
        if (fade && k >= v.len - fade) amp *= shapeOf(outShape, (float)(v.len - 1 - k) / (float)fade);
        if (k < v.fadeIn) amp *= shapeOf(v.inShape, (float)k / (float)v.fadeIn);
        int o = i + (int)(v.start + k - pos);
        if (direct) {                                    // balanced by its azimuth (score.h's balanceGains)
            for (int c = 0; c < nOut; ++c) buf[c * dsp::kBlock + o] += amp * g[nOut + c] * src.interp(sp, c);
        } else {
            float x = 0.f;
            for (int c = 0; c < src.channels(); ++c) x += src.interp(sp, c);
            x *= amp / (float)src.channels();
            for (int c = 0; c < nOut; ++c) buf[c * dsp::kBlock + o] += g[c] * x;
        }
        if (!src.ok()) return;
    }
}
struct MemSrc {
    const SampleBuffer* b;
    double sr() const { return b->sr; }
    double frames() const { return (double)b->frames; }
    int channels() const { return b->channels; }
    float interp(double sp, int c) const { return b->interp(sp, c); }
    bool ok() const { return true; }
};
struct DiskSrc {
    StreamSource s;
    double sr() const { return s.s->sr(); }
    double frames() const { return (double)s.s->frames(); }
    int channels() const { return s.s->channels(); }
    float interp(double pos, int c) {                     // as SampleBuffer::interp, from the chunks
        if (!(pos >= -1.0 && pos < frames())) return 0.f;
        long i = (long)std::floor(pos);
        float f = (float)(pos - i);
        const float* fa = s.frame(i);
        const float* fb = s.frame(i + 1);
        int ch = channels();
        float a = fa ? fa[c % ch] : 0.f, b = fb ? fb[c % ch] : 0.f;
        return a + f * (b - a);
    }
    bool ok() const { return !s.failed; }
};

// one channel's block: its voices, patch clips and instrument, its input when monitored, what it takes from others,
// its insert, then its strip (fader, pan, mute and solo, ramped over the block) and its meters
void ScoreBox::processChannel(const BlockCtx& bc, int index) {
    const ScorePlan& plan = *bc.plan;
    const ScorePlan::Channel& ch = plan.channels[(size_t)index];
    const int n = bc.n, nOut = plan.nOut;
    float* buf = ch.buf;
    for (int c = 0; c < nOut; ++c) dsp::fill(buf + c * dsp::kBlock, 0.f, n);
    for (int r = 0; r < bc.nruns; ++r) {
        const Run& run = bc.runs[r];
        int64_t p1 = run.pos + run.len;
        auto lo = std::lower_bound(ch.voices.begin(), ch.voices.end(), run.pos - ch.maxLen,
                                   [](const ScorePlan::Voice& v, int64_t q) { return v.start < q; });
        for (auto it = lo; it != ch.voices.end() && it->start < p1; ++it) {
            const ScorePlan::Voice& v = *it;
            if (v.start + v.len <= run.pos) continue;
            const float* g = &plan.gains[(size_t)v.gains];
            if (v.stream >= 0) {
                mt::DiskStream* s = plan.streams[(size_t)v.stream].get();
                double rate = s->sr() / plan.sr * v.speed;
                s->want((int64_t)std::floor(v.from * s->sr() + (double)std::max<int64_t>(0, run.pos - v.start) * rate));
                DiskSrc src{StreamSource(s)};
                renderVoice(v, src, g, nOut, buf, run.i, run.pos, run.len, plan.sr);
                continue;
            }
            const SampleBuffer* sb = v.file >= 0 ? plan.files[(size_t)v.file].get() : plan.bufs[(size_t)v.buf]->get();
            if (!sb || sb->frames <= 0) continue;
            MemSrc src{sb};
            renderVoice(v, src, g, nOut, buf, run.i, run.pos, run.len, plan.sr);
        }
    }
    // the patches it runs add into it
    InnerIO io;
    for (int c = 0; c < nOut; ++c) io.out[c] = buf + c * dsp::kBlock;
    io.nout = nOut;
    const InnerIO* prev = tlInner;
    for (const auto& cl : ch.clips) {                     // a patch clip: heard from its start to its end, to the sample
        bool inside = false;
        for (int r = 0; r < bc.nruns && !inside; ++r) inside = bc.runs[r].pos < cl.end && bc.runs[r].pos + bc.runs[r].len > cl.start;
        if (!inside) continue;
        InnerIO cio;
        for (int c = 0; c < nOut; ++c) {
            cio.out[c] = ch.tmp + c * dsp::kBlock;
            dsp::fill(ch.tmp + c * dsp::kBlock, 0.f, n);
        }
        cio.nout = nOut;
        tlInner = &cio;
        runInner(cl.slot, n);
        for (int r = 0; r < bc.nruns; ++r) {
            const Run& run = bc.runs[r];
            int a = (int)std::clamp<int64_t>(cl.start - run.pos, 0, run.len), b = (int)std::clamp<int64_t>(cl.end - run.pos, 0, run.len);
            for (int c = 0; c < nOut; ++c)
                for (int i = run.i + a; i < run.i + b; ++i) buf[c * dsp::kBlock + i] += ch.tmp[c * dsp::kBlock + i];
        }
    }
    if (ch.instrument >= 0) {
        tlInner = &io;
        runInner(ch.instrument, n);
    }
    tlInner = prev;
    // its input, heard while it is armed and monitored
    mt::StripParams& st = *ch.strip;
    if (ch.armed && ch.input >= 0 && bc.in && st.monitor.load(std::memory_order_relaxed)) {
        if (ch.inputCh == 2 && nOut >= 2) {
            for (int c = 0; c < 2; ++c)
                if (ch.input + c < bc.inCh)
                    for (int i = 0; i < n; ++i) buf[c * dsp::kBlock + i] += bc.in[(size_t)i * bc.inCh + ch.input + c];
        } else if (ch.input < bc.inCh) {
            double g[mt::kMaxChannels];
            sc::panGains(0, nOut, g);
            for (int c = 0; c < nOut; ++c)
                for (int i = 0; i < n; ++i) buf[c * dsp::kBlock + i] += (float)g[c] * bc.in[(size_t)i * bc.inCh + ch.input];
        }
    }
    // what it takes from others: their output, or one of their sends (ramped)
    for (const auto& src : ch.inputs) {
        const ScorePlan::Channel& from = plan.channels[(size_t)src.ch];
        if (src.send < 0) {
            for (int c = 0; c < nOut; ++c)
                for (int i = 0; i < n; ++i) buf[c * dsp::kBlock + i] += from.buf[c * dsp::kBlock + i];
            continue;
        }
        mt::StripParams& fs = *from.strip;
        float g0 = fs.sendApplied[src.send], g1 = fs.sendTarget[src.send];
        for (int c = 0; c < nOut; ++c)
            for (int i = 0; i < n; ++i) buf[c * dsp::kBlock + i] += from.buf[c * dsp::kBlock + i] * (g0 + (g1 - g0) * (float)(i + 1) / (float)n);
        fs.sendApplied[src.send] = g1;
    }
    // its insert: the channel through a patch
    if (ch.insert >= 0) runInsert(ch.insert, buf, ch.tmp, nOut, n);
    // its strip
    float target = st.gain.load(std::memory_order_relaxed), az = st.az.load(std::memory_order_relaxed);
    bool mute = st.mute.load(std::memory_order_relaxed);
    float sendTarget[mt::kMaxSends];
    for (int k = 0; k < mt::kMaxSends; ++k) sendTarget[k] = st.send[k].load(std::memory_order_relaxed);
    for (const auto& cv : ch.curves) {
        float v = cv.at(bc.pos0);
        switch (cv.what) {
        case ScorePlan::Curve::Gain: target = (float)mt::dbToGain(v); break;
        case ScorePlan::Curve::Pan: az = v; break;
        case ScorePlan::Curve::Mute: mute = v >= 0.5f; break;
        case ScorePlan::Curve::Send: sendTarget[cv.send] = (float)mt::dbToGain(v); break;
        }
    }
    bool audible = !mute && (!bc.anySolo || st.solo.load(std::memory_order_relaxed) || ch.bus || !ch.inputs.empty());
    applyStrip(st, buf, nOut, n, audible ? target : 0.f, ch.balance ? az : 0.f, ch.balance);
    for (int k = 0; k < mt::kMaxSends; ++k) {
        st.sendTarget[k] = sendTarget[k];
        if (st.fresh) st.sendApplied[k] = sendTarget[k];
    }
    st.fresh = false;
    meter(st, buf, nOut, n, plan.sr);
}

void ScoreBox::runInsert(int slot, float* buf, float* tmp, int nOut, int n) {
    InnerIO io;
    for (int c = 0; c < nOut; ++c) {
        io.in[c] = buf + c * dsp::kBlock;
        io.out[c] = tmp + c * dsp::kBlock;
        dsp::fill(tmp + c * dsp::kBlock, 0.f, n);
    }
    io.nin = io.nout = nOut;
    const InnerIO* prev = tlInner;
    tlInner = &io;
    runInner(slot, n);
    tlInner = prev;
    for (int c = 0; c < nOut; ++c) std::copy_n(tmp + c * dsp::kBlock, n, buf + c * dsp::kBlock);
}

// the fader (and, on two channels, the pan as a constant-power balance) ramped from what was applied over the block
void ScoreBox::applyStrip(mt::StripParams& st, float* buf, int nOut, int n, float gain, float az, bool balance) {
    float l1 = gain, r1 = gain;
    if (balance && nOut == 2) {
        double b[2];
        sc::balanceGains(az, 2, b);
        l1 = gain * (float)b[0];
        r1 = gain * (float)b[1];
    }
    if (st.fresh) {
        st.appliedL = l1;
        st.appliedR = r1;
        st.applied = gain;
    }
    float l0 = st.appliedL, r0 = st.appliedR;
    for (int c = 0; c < nOut; ++c) {
        float a = c == 1 ? r0 : l0, b = c == 1 ? r1 : l1;
        float* x = buf + c * dsp::kBlock;
        if (a == b) {
            if (a != 1.f) for (int i = 0; i < n; ++i) x[i] *= a;
        } else {
            for (int i = 0; i < n; ++i) x[i] *= a + (b - a) * (float)(i + 1) / (float)n;
        }
    }
    st.appliedL = l1;
    st.appliedR = r1;
    st.applied = gain;
}

void ScoreBox::meter(mt::StripParams& st, const float* buf, int nOut, int n, double sr) {
    double a = std::exp(-(double)n / (0.3 * sr));
    for (int m = 0; m < 2; ++m) {
        const float* x = buf + std::min(m, nOut - 1) * dsp::kBlock;
        float pk = 0.f;
        double ss = 0;
        for (int i = 0; i < n; ++i) {
            pk = std::max(pk, std::fabs(x[i]));
            ss += (double)x[i] * x[i];
        }
        if (pk > st.peak[m].load(std::memory_order_relaxed)) st.peak[m].store(pk, std::memory_order_relaxed);
        st.rmsState[m] = a * st.rmsState[m] + (1 - a) * (ss / std::max(1, n));
        st.rms[m].store((float)std::sqrt(st.rmsState[m]), std::memory_order_relaxed);
    }
    st.blocks.fetch_add(1, std::memory_order_relaxed);
}

void channelJob(void* ctx, int k) {
    auto* bc = static_cast<BlockCtx*>(ctx);
    bc->box->processChannel(*bc, k);
}

void ScoreBox::dsp(int n) {
    float* out[16];
    for (int c = 0; c < nOut_; ++c) {
        out[c] = sigOut(c);
        if (out[c]) dsp::fill(out[c], 0.f, n);
    }
    const ScorePlan* plan = plan_.get();
    lastN_ = n;
    if (!plan || n > dsp::kBlock || plan->nOut != nOut_) return;
    const TransportState* ts = t_ ? t_->state() : nullptr;
    BlockCtx bc;
    bc.box = this;
    bc.plan = plan;
    bc.n = n;
    // where the transport is, sample by sample (for the takes being recorded)
    int64_t posAt[dsp::kBlock];
    bool playAt[dsp::kBlock];
    const uint64_t t0 = patch->sampleTime();
    int64_t stood = 0;
    if (ts) {
        int i = 0;
        while (i < n) {
            int64_t pos;
            uint64_t t = t0 + (uint64_t)i;
            if (!ts->at(t, pos)) {                              // not playing here
                stood = pos;
                posAt[i] = pos;
                playAt[i] = false;
                ++i;
                continue;
            }
            int len = (int)std::min<int64_t>(n - i, ts->run(t, pos, n - i));
            bc.runs[bc.nruns++] = Run{i, len, pos};
            for (int k = 0; k < len; ++k) {
                posAt[i + k] = pos + k;
                playAt[i + k] = true;
            }
            i += len;
        }
    } else {
        for (int i = 0; i < n; ++i) { posAt[i] = 0; playAt[i] = false; }
    }
    bc.pos0 = bc.nruns ? bc.runs[0].pos : stood;
    Patch* root = patch->root();
    if (root->ioIn && root->ioInCh > 0) {
        bc.in = root->ioIn + (size_t)root->ioOffset * (size_t)root->ioInCh;
        bc.inCh = root->ioInCh;
    }
    if (mt::Recording* rec = recording_.load(std::memory_order_acquire)) rec->push(bc.in, bc.inCh, n, posAt, playAt);
    for (const auto& ch : plan->channels) bc.anySolo = bc.anySolo || ch.strip->solo.load(std::memory_order_relaxed);
    // the channels fed by none, on several cores when there are enough of them; then the others, in order
    int want = mt::TrackPool::threads.load(std::memory_order_relaxed);
    bool parallel = bc.nruns > 0 && plan->nFree >= 2 && want != 1 && (want >= 2 || plan->nFree >= mt::TrackPool::minTracks.load(std::memory_order_relaxed));
    if (parallel) mt::TrackPool::get().run(plan->nFree, channelJob, &bc);
    else for (int k = 0; k < plan->nFree; ++k) processChannel(bc, k);
    for (int k = plan->nFree; k < (int)plan->channels.size(); ++k) processChannel(bc, k);
    // the master: what goes to it, in the channels' order, through its insert and strip
    float* mb = plan->masterBuf;
    for (int c = 0; c < nOut_; ++c) dsp::fill(mb + c * dsp::kBlock, 0.f, n);
    for (const auto& ch : plan->channels)
        if (ch.toMaster)
            for (int c = 0; c < nOut_; ++c)
                for (int i = 0; i < n; ++i) mb[c * dsp::kBlock + i] += ch.buf[c * dsp::kBlock + i];
    if (plan->masterInsert >= 0) runInsert(plan->masterInsert, mb, plan->masterTmp, nOut_, n);
    mt::StripParams& ms = *plan->master;
    applyStrip(ms, mb, nOut_, n, ms.mute.load(std::memory_order_relaxed) ? 0.f : ms.gain.load(std::memory_order_relaxed), 0.f, false);
    ms.fresh = false;
    meter(ms, mb, nOut_, n, plan->sr);
    // the count-in's clicks, before a recording's start
    if (mt::Recording* rec = recording_.load(std::memory_order_acquire); rec && rec->clickBeat > 0)
        for (int i = 0; i < n; ++i) {
            if (!playAt[i] || posAt[i] < rec->clickFrom || posAt[i] >= rec->from()) continue;
            int64_t k = (posAt[i] - rec->clickFrom) % rec->clickBeat;
            if (k >= (int64_t)(0.03 * plan->sr)) continue;
            bool bar = (posAt[i] - rec->clickFrom) / rec->clickBeat % std::max(1, rec->clickBar) == 0;
            float x = 0.3f * std::exp(-(float)k / (float)(0.006 * plan->sr)) * std::sin(6.2831853f * (bar ? 1600.f : 1000.f) * (float)k / (float)plan->sr);
            for (int c = 0; c < nOut_; ++c) mb[c * dsp::kBlock + i] += x;
        }
    for (int c = 0; c < nOut_; ++c)
        if (out[c]) std::copy_n(mb + c * dsp::kBlock, n, out[c]);
}
}  // namespace

// ---------------------------------------------------------------- what editors use (transport.h)
bool isScoreBox(const Box* b) { return dynamic_cast<const ScoreBox*>(b) != nullptr; }
std::shared_ptr<sc::Score> scoreOfBox(Box* b) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->current() : nullptr;
}
bool replaceScoreOfBox(Box* b, std::shared_ptr<sc::Score> score) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    if (!s || !score) return false;
    s->replace(std::move(score));
    return true;
}
bool scoreBoxKeptInPatch(const Box* b) {
    auto* s = dynamic_cast<const ScoreBox*>(b);
    return s && s->keptInPatch();
}
int scoreBoxView(const Box* b) {
    auto* s = dynamic_cast<const ScoreBox*>(b);
    return s ? s->openView() : (int)ScoreView::Roll;
}
void setScoreBoxView(Box* b, int view) {
    if (auto* s = dynamic_cast<ScoreBox*>(b)) s->setLastView(view);
}
std::string auditionInBox(Box* b, const sc::Event& e, const sc::Score* origin) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->audition(e, origin) : std::string();
}
RollCore* inlineRollOf(Box* b) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? &s->roll() : nullptr;
}

namespace mt {
std::function<void()> inputNeedChanged;
StripParams* stripOf(Box* b, const std::string& row) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->strip(row) : nullptr;
}
bool startRecording(Box* b, std::unique_ptr<Recording> r, std::string& err) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    if (!s) { err = "no score box"; return false; }
    return s->startRecording(std::move(r), err);
}
std::unique_ptr<Recording> stopRecording(Box* b) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->stopRecording() : nullptr;
}
Recording* recordingOf(Box* b) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->recording() : nullptr;
}
bool stemsOf(Box* b, StemTap& out) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s && s->stems(out);
}
void setStemTap(Box* b, bool on) {
    if (auto* s = dynamic_cast<ScoreBox*>(b)) s->setStemTap(on);
}
PlanInfo planInfo(Box* b) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->info() : PlanInfo{};
}
Patch* trackPatch(Box* b, const std::string& row, const std::string& role) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->slotPatch(role + ":" + row) : nullptr;
}
Patch* clipPatch(Box* b, uint64_t clipId) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->clipSlot(clipId) : nullptr;
}
}  // namespace mt

void registerScoreObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"transport", "Timing", "the patch's clock: play, stop, locate bar, loop bar1 bar2, tempo; bars, beats and ms out: [bpm] [num den]",
         with<TransportBox>()},
        {"score", "Timing", "a score that plays on the transport, its tracks mixed as a session (strips, buses, inserts, instruments): notes, messages and sounds out; read/write .score: name [file.score] [channels] [@view timeline]",
         with<ScoreBox>()},
    });
}

}  // namespace moritz
