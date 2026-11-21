// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// transport.h — the clock of a patch: where its scores are, whether they play, a tempo and a meter to count bars
// and beats. One for each top document (Patch::topDocument: a patch in a window of its own, a help patch, a score's
// own player, the patch --render runs) and everything inside it (its subpatches and abstractions): the transport
// object shows and drives it, the score object plays on it. Two documents open play and stop on their own: Play in
// one, or a score's window playing its score, never starts the other.
//
// Positions are samples at the patch's rate (seconds in the score times the rate). A change (play, stop, locate,
// loop) takes effect at the sample a message sent now lands on (Patch::stamp): the audio thread reads the clock
// through a published state that says where the transport is at any sample, before and after the change, with
// the loop folded in, so a score's sounds start on their sample. The GUI thread maps the same positions onto the
// patch's logical time: what a score sends from its control outlets is scheduled at the time whose stamp is that
// sample (offline, exactly; in real time, as exactly as the patch's clock follows the device's).
#pragma once

#include "objects.h"
#include "music/score.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace moritz {

// What the audio thread reads: immutable once published.
struct TransportState {
    struct Seg {
        bool playing = false;
        uint64_t anchor = 0;     // the sample where this part starts
        int64_t pos0 = 0;        // the position there
    };
    double sr = 44100.0;
    Seg prev, cur;               // prev before `since`, cur from it on
    uint64_t since = 0;
    bool loop = false;
    int64_t loopStart = 0, loopEnd = 0;
    // the position at sample t, folded into the loop; false when the transport does not play there
    bool at(uint64_t t, int64_t& pos) const {
        const Seg& s = t < since ? prev : cur;
        pos = s.pos0;
        if (!s.playing || t < s.anchor) return false;
        int64_t p = s.pos0 + (int64_t)(t - s.anchor);
        if (loop && loopEnd > loopStart && s.pos0 < loopEnd && p >= loopEnd)
            p = loopStart + (p - loopStart) % (loopEnd - loopStart);
        pos = p;
        return true;
    }
    // how many samples from t the position goes on by one a sample (until the next change or the loop's end)
    int64_t run(uint64_t t, int64_t pos, int64_t most) const {
        int64_t n = most;
        if (t < since) n = std::min<int64_t>(n, (int64_t)(since - t));
        const Seg& s = t < since ? prev : cur;
        if (loop && loopEnd > loopStart && s.pos0 < loopEnd && pos < loopEnd) n = std::min<int64_t>(n, loopEnd - pos);
        return std::max<int64_t>(1, n);
    }
};

class Transport {
public:
    // ---- GUI thread
    double bpm = 120.0;
    int num = 4, den = 4;

    bool playing() const { return seg_.playing; }
    double sampleRate() const { return sr_; }
    // the position at the patch's logical time now (samples; folded into the loop), and the lap: 0 before the loop
    // first wraps, k for the k-th pass through it
    void where(Patch* p, int& lap, double& pos) const { whereAt(p->logicalTime(), lap, pos); }
    void whereAt(double t, int& lap, double& pos) const {
        lap = 0;
        pos = (double)seg_.pos0;
        if (!seg_.playing) return;
        double u = seg_.pos0 + std::max(0.0, t - anchorTime_) * sr_;
        // a time timeAt gave for a sample is that sample again, whatever the anchor (a time not exact in binary): the
        // way back from a time to a position is never a hair short of it (at the loop's end it was the pass before, and
        // what follows the transport scheduled the same moment again and again)
        if (double r = std::round(u); std::fabs(u - r) < 1e-3) u = r;
        if (!looping() || u < loopEnd_) { pos = u; return; }
        double len = (double)(loopEnd_ - loopStart_), over = u - loopEnd_;
        double k = std::floor(over / len);
        lap = 1 + (int)std::min(k, 1e9);
        pos = loopStart_ + (over - k * len);
    }
    // the logical time at which the transport, playing, is at position pos on lap (NaN: it never is)
    double timeAt(double pos, int lap) const {
        if (!seg_.playing) return NAN;
        if (lap == 0) {
            if (pos < seg_.pos0 || (looping() && pos >= loopEnd_)) return NAN;
            return anchorTime_ + (pos - seg_.pos0) / sr_;
        }
        if (!looping() || pos < loopStart_ || pos >= loopEnd_) return NAN;
        return anchorTime_ + ((double)(loopEnd_ - seg_.pos0) + (lap - 1) * (double)(loopEnd_ - loopStart_) + (pos - loopStart_)) / sr_;
    }
    bool looping() const { return loop_ && loopEnd_ > loopStart_ && seg_.pos0 < loopEnd_; }
    // how many times it was started, stopped, located or looped (not its tempo or meter): who changed it last can tell
    // whether anyone else has since
    uint64_t changes() const { return changes_; }
    bool loopOn() const { return loop_; }
    int64_t loopStart() const { return loopStart_; }
    int64_t loopEnd() const { return loopEnd_; }
    double seconds(double pos) const { return pos / sr_; }
    double beatSamples() const { return 60.0 / bpm * sr_; }

    // ---- changes (GUI thread): each takes effect at the sample a message sent now lands on
    void play(Patch* p) { if (!seg_.playing) change(p, true, nowPos(p)); }
    void stop(Patch* p) { if (seg_.playing) change(p, false, nowPos(p)); }
    void locate(Patch* p, double seconds) { change(p, seg_.playing, (int64_t)std::llround(std::max(0.0, seconds) * rate(p))); }
    void setLoop(Patch* p, bool on, double fromSec = 0, double toSec = 0) {
        int64_t here = nowPos(p);
        loop_ = on;
        if (on) {
            double r = rate(p);
            loopStart_ = (int64_t)std::llround(std::max(0.0, fromSec) * r);
            loopEnd_ = (int64_t)std::llround(std::max(0.0, toSec) * r);
        }
        change(p, seg_.playing, here);
    }
    void setTempo(Patch* p, double b) { if (b > 0 && std::isfinite(b)) { bpm = b; notify(); } (void)p; }
    void setMeter(Patch* p, int n, int d) { num = std::max(1, n); den = std::max(1, d); notify(); (void)p; }
    // the device started or stopped, or the rate changed: the same position, anchored again on the sample count
    void reanchor(Patch* p) { change(p, seg_.playing, nowPos(p), false); }

    // bars, beats (from 1) and ticks of a position in samples, at the tempo and meter
    void bbt(double pos, long& bar, int& beat, int& tick) const {
        auto b = musil::score::toBBT(pos / sr_, bpm, num);       // as Musil counts them (score-bbt)
        bar = b.bar; beat = b.beat; tick = b.tick;
    }
    // seconds of a position given as bars, beats (from 1) and ticks
    double secondsOf(double bar, double beat, double tick) const { return musil::score::fromBBT(bar, beat, tick, bpm, num); }

    // ---- who follows it (GUI thread): told after every change
    void listen(void* who, std::function<void()> f) { listeners_.push_back({who, std::move(f)}); }
    void unlisten(void* who) {
        for (auto& l : listeners_) if (l.who == who) l.who = nullptr, l.f = nullptr;
        if (!notifying_) prune();
    }

    // ---- audio thread: the state now (never keep it past the callback)
    const TransportState* state() const { return state_.get(); }

    // top: the document it is the clock of (the state is handed to the audio thread through its root)
    explicit Transport(Patch* top) : root_(top->root()) {
        sr_ = root_->sampleRate.load();
        publish(0);
    }

private:
    struct Listener { void* who; std::function<void()> f; };
    Patch* root_;
    double sr_ = 44100.0;
    TransportState::Seg seg_;          // the part in force (GUI side)
    double anchorTime_ = 0.0;          // the logical time of seg_.anchor
    uint64_t changes_ = 0;
    bool loop_ = false;
    int64_t loopStart_ = 0, loopEnd_ = 0;
    Published<TransportState> state_;
    std::vector<Listener> listeners_;
    bool notifying_ = false;

    double rate(Patch* p) const { (void)p; return sr_; }
    int64_t nowPos(Patch* p) const { int lap; double pos; where(p, lap, pos); return (int64_t)std::floor(pos + 1e-6); }
    void change(Patch* p, bool playing, int64_t pos, bool counts = true) {
        TransportState::Seg old = seg_;
        uint64_t at = p->stamp();
        sr_ = p->root()->sampleRate.load();
        anchorTime_ = p->logicalTime();
        seg_.playing = playing;
        seg_.anchor = at;
        seg_.pos0 = std::max<int64_t>(0, pos);
        if (counts) ++changes_;
        publish(at, &old);
        notify();
    }
    void publish(uint64_t since, const TransportState::Seg* prev = nullptr) {
        auto s = std::make_unique<TransportState>();
        s->sr = sr_;
        s->prev = prev ? *prev : seg_;
        s->cur = seg_;
        s->since = since;
        s->loop = loop_;
        s->loopStart = loopStart_;
        s->loopEnd = loopEnd_;
        state_.set(root_, std::move(s));
    }
    void notify() {
        notifying_ = true;
        for (size_t k = 0; k < listeners_.size(); ++k)       // a listener may add another: by index
            if (listeners_[k].who && listeners_[k].f) { auto f = listeners_[k].f; f(); }
        notifying_ = false;
        prune();
    }
    void prune() {
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [](const Listener& l) { return !l.who; }), listeners_.end());
    }
};

// A score handed from a code box to a score object ((out k s) sends `score <token>`): the token names an immutable
// copy, kept among the last 64 handed over (objects_score.cpp). Any thread.
std::string shareScore(std::shared_ptr<const musil::score::Score> s);
std::shared_ptr<const musil::score::Score> sharedScore(const std::string& token);

// A score box, for the editors (score_doc.h): the score it holds now, the same object it plays; replaceScoreOfBox
// hands it a score (or the same one, edited in place) and it plays that at once (ScoreBox::replace, GUI thread);
// a score kept in the patch has no file of its own (it is saved inside the patch).
bool isScoreBox(const Box* b);
std::shared_ptr<musil::score::Score> scoreOfBox(Box* b);
bool replaceScoreOfBox(Box* b, std::shared_ptr<musil::score::Score> s);
bool scoreBoxKeptInPatch(const Box* b);
// a note (or a sound) sounded on its own, as a click on it in a roll does (GUI thread): e as it sounds (its placements
// applied), origin the score it is in (where its sample is found). A note with a sample of its own (data sound, as
// orchidea's) plays it through the preview voice at its pitch for its duration; a note without one leaves the box's
// notes outlet, and at its end a note-off (velocity 0), or sounds as a tone when nothing is connected there; a sound its
// region. What was done, for the status line ("" nothing)
std::string auditionInBox(Box* box, const musil::score::Event& e, const musil::score::Score* origin);

// the clock of the patch p is in (its top document's), made on first use
inline std::shared_ptr<Transport> transportOf(Patch* p) {
    Patch* d = p->topDocument();
    if (!d->transport) d->transport = std::make_shared<Transport>(d);
    return d->transport;
}

}  // namespace moritz
