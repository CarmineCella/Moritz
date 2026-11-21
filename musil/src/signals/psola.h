// psola.h — the harmonizer: a voice and up to eight pitch-shifted copies of it, by pitch-synchronous overlap-add
// of grains (PSOLA). The engine of Musil's harmonize (offline) and of Moritz's harmonize~ (in real time). After
// reVoice's harmonizer (Carmine-Emanuele Cella), rebuilt on Musil's grain core (grains.h) and YIN (pitch.h).
//
// Copyright (c) 2008-2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// The input goes into a ring and is heard `latency` later (12 ms at least, one longest period and 2 ms by default).
//   detection   StreamingYin on the input as it comes (not on the delayed signal): every 5 ms an f0, kept with the
//               time it describes (the centre of its frame), so the voices read the f0 of the very samples they
//               granulate, the latency later.
//   pitch marks one a period, causal: the last mark plus the period, snapped to the peak of the input low-passed
//               at 1.5 f0 within a quarter period either side (marks "period": not snapped, as reVoice did).
//   voices      each runs an output phase at its own f0 (the input's at the delayed time, times its ratio, its
//               vibrato and its humanizing drift); at every period it starts a grain of `cycles` input periods (2;
//               1 from an octave up, fewer down: never more overlap than needed) centred on the analysis mark
//               nearest it, windowed (hann; hamming, blackman), read at the speed `formant` (1 keeps the
//               formants, 1.2 moves them up 20%). The grains are scaled by the overlap of their windows, so a voice
//               keeps the input's level from an octave down to an octave up.
//   unvoiced    where the delayed input has no f0 the voices fade out in 10 ms (unvoiced "mute"), or cross to the
//               dry delayed input (unvoiced "dry": the consonants of each voice are kept); no grain is made from a
//               stale period.
//   harmony     "intervals"  each voice its interval in semitones;
//               "scale"      the input's pitch snapped to the key's scale (12-bit mask, from scales.h), each voice
//                            that many scale steps from it (2 a third, 4 a fifth, 7 an octave), keeping the
//                            singer's intonation;
//               "midi-chord" each held note gives a voice at its pitch, its velocity its level;
//               "midi-root"  the last held note is the fundamental, each voice its interval above it;
//               and `transpose` (semitones) moves them all.
// The random draws (vibrato's irregularity, the humanizing drift and onset delays) are Musil's portable ones
// (rng.h on mt19937): a seed gives the same output everywhere. init() allocates; process() and the messages
// allocate nothing and lock nothing: an audio thread runs them.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../music/scales.h"
#include "../rng.h"
#include "grains.h"
#include "pitch.h"

namespace musil {
namespace dsp {

enum class HarmonyMode { Intervals, Scale, MidiChord, MidiRoot };

inline bool harmony_mode(const std::string& s, HarmonyMode& m) {
    if (s == "intervals") m = HarmonyMode::Intervals;
    else if (s == "scale") m = HarmonyMode::Scale;
    else if (s == "midi-chord" || s == "chord") m = HarmonyMode::MidiChord;
    else if (s == "midi-root" || s == "root") m = HarmonyMode::MidiRoot;
    else return false;
    return true;
}

template <class T>
class Psola {
public:
    static constexpr int kMaxVoices = 8, kGrains = 24, kMarks = 512, kHistory = 256;

    struct Voice {
        double interval = 0;       // semitones (intervals, midi-root) or scale steps (scale)
        double level = 1;          // 0..
        double pan = 0;            // -1 left .. 1 right
        double formant = 1;        // read speed of the grains: 1 keeps the formants
        double vibDepth = 0;       // cents
        double vibRate = 5;        // Hz
        double vibRandom = 0;      // 0..1: rate and depth wander by up to this ratio
    };
    // the parameters, read by process(); a host changes them between calls
    struct Params {
        HarmonyMode mode = HarmonyMode::Intervals;
        int voices = 4;            // 1..kMaxVoices
        Voice v[kMaxVoices];
        double transpose = 0;      // semitones, every voice
        int scaleMask = 0xAB5;     // C major
        double dry = 0, wet = 1;
        double humanizeCents = 0, humanizeMs = 0;
        double cycles = 0;         // input periods per grain; 0 auto
        bool unvoicedDry = false;
        bool marksPeriod = false;  // marks one period apart, not snapped (reVoice)
    };
    Params p;

    struct Setup {
        double sr = 44100, fmin = 70, fmax = 1000;
        double latencyMs = 0;      // 0: max(1000 / fmin + 2, 12)
        double threshold = 0.15;   // YIN's
        double gate = 0.003;       // RMS under which the input is unvoiced
        int decimate = 1;
        bool local = false;        // YIN's step 6
    };

    Psola() {
        for (int k = 0; k < kMaxVoices; ++k) {
            p.v[k].interval = k == 0 ? 4 : k == 1 ? 7 : k == 2 ? 12 : -12;
            p.v[k].pan = kMaxVoices > 1 ? -0.6 + 1.2 * (k % 4) / 3.0 : 0;
        }
        Setup s;
        init(s);
    }

    // allocates (not on an audio thread); the voices, notes and random state start again
    void init(const Setup& s) {
        set_ = s;
        set_.sr = std::isfinite(s.sr) && s.sr > 0 ? s.sr : 44100;
        set_.fmin = std::clamp(s.fmin, 20.0, 2000.0);
        set_.fmax = std::clamp(s.fmax, set_.fmin * 1.5, 0.45 * set_.sr);
        const double sr = set_.sr;
        StreamingYin::Config yc;
        yc.sr = sr; yc.fmin = set_.fmin; yc.fmax = set_.fmax; yc.threshold = s.threshold; yc.gate = s.gate;
        yc.decimate = s.decimate; yc.local = s.local; yc.hopMs = 5;
        yin_.init(yc);
        double lat = s.latencyMs > 0 ? s.latencyMs : std::max(1000.0 / set_.fmin + 2.0, 12.0);
        delay_ = std::max((long)yin_.lag(), (long)std::lround(lat * 1e-3 * sr));
        set_.latencyMs = 1000.0 * delay_ / sr;
        pmax_ = sr / set_.fmin;
        size_t need = (size_t)(delay_ + 16 * pmax_ + 4096), n = 1;
        while (n < need) n <<= 1;
        ring_.assign(n, 0.0);
        lp_.assign(n, 0.0);
        mask_ = n - 1;
        marks_.assign(kMarks, Mark{});
        hist_.assign(kHistory, Hist{});
        for (auto& v : vs_) v.pool.allocate(kGrains);
        reset();
    }
    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0);
        std::fill(lp_.begin(), lp_.end(), 0.0);
        yin_.reset();
        markLp_.reset();
        markLp_.set(rbj(BiquadType::Lowpass, set_.sr, 200, 0.7071));
        t_ = 0;
        nMarks_ = 0; markHead_ = 0;
        nHist_ = 0; histHead_ = 0; histCursor_ = 0;
        voicedEnv_ = 0;
        f0Now_ = f0Delayed_ = 0;
        held_ = 0;
        for (int k = 0; k < 128; ++k) noteOrder_[k] = 0;
        order_ = 0;
        for (auto& v : vs_) {
            v.pool.clear();
            v.phase = 1;           // the first grain at once
            v.gate = 0; v.note = -1; v.vel = 0; v.wait = 0; v.on = false;
            v.vibPhase = 0; v.vibRateNow = 0; v.vibDepthNow = 0;
            v.drift = v.driftTarget = 0; v.driftCount = 0;
            v.pin = v.pout = 0; v.agc = 1; v.agcAge = 0;
        }
        q_ = -1000;
        seed(seed_);
    }
    void seed(uint32_t s) {
        seed_ = s;
        rng_.seed(s);
        for (auto& v : vs_) v.drift = v.driftTarget = 0;
    }
    const Setup& setup() const { return set_; }
    double sampleRate() const { return set_.sr; }
    long latency() const { return delay_; }            // samples
    void setWindow(const std::string& w) { window_.set(w); windowType_ = w == "hamming" ? 1 : w == "blackman" ? 2 : 0; }
    // 0 hann, 1 hamming, 2 blackman: the table is rewritten in place (no allocation: an audio thread may call it)
    void setWindowType(int k) {
        windowType_ = k == 1 || k == 2 ? k : 0;
        window_.set(windowType_ == 1 ? "hamming" : windowType_ == 2 ? "blackman" : "hann");
    }
    int windowType() const { return windowType_; }
    void setThreshold(double t) { yin_.setThreshold(t); }
    void setGate(double g) { yin_.setGate(g); }
    // the newest f0 detected (Hz, 0 unvoiced), and the one the voices follow now (the latency earlier)
    double f0() const { return f0Now_; }
    double f0Delayed() const { return f0Delayed_; }

    // MIDI, for the midi modes (velocity 0 is a note off)
    void noteOn(int pitch, double velocity) {
        if (pitch < 0 || pitch > 127) return;
        if (velocity <= 0) { noteOff(pitch); return; }
        noteOrder_[pitch] = ++order_;
        int nv = voices();
        int k = -1;
        for (int j = 0; j < nv; ++j) if (vs_[j].note == pitch) { k = j; break; }
        if (k < 0) for (int j = 0; j < nv; ++j) if (vs_[j].note < 0) { k = j; break; }
        if (k < 0) {                                     // all busy: the oldest note gives its voice
            uint64_t oldest = ~0ull;
            for (int j = 0; j < nv; ++j)
                if (noteOrder_[vs_[j].note] < oldest) { oldest = noteOrder_[vs_[j].note]; k = j; }
        }
        if (k >= 0) { vs_[k].note = pitch; vs_[k].vel = std::clamp(velocity, 0.0, 127.0) / 127.0; }
    }
    void noteOff(int pitch) {
        if (pitch < 0 || pitch > 127) return;
        noteOrder_[pitch] = 0;
        for (auto& v : vs_) if (v.note == pitch) v.note = -1;
    }
    void allNotesOff() {
        for (int k = 0; k < 128; ++k) noteOrder_[k] = 0;
        for (auto& v : vs_) v.note = -1;
    }

    // n samples of input into L and R (overwritten); voiceOut[k], when given and not null, gets voice k alone
    // (its level, not panned); L and R may be null
    void process(const T* in, T* L, T* R, int n, T* const* voiceOut = nullptr) {
        const int nv = voices();
        const double sr = set_.sr;
        const double gateStep = 1.0 / (0.010 * sr);
        const double powerStep = 1.0 / (0.15 * sr), agcStep = 1.0 / (0.05 * sr);
        const long agcHold = (long)(0.05 * sr);
        double panL[kMaxVoices], panR[kMaxVoices];
        for (int k = 0; k < nv; ++k) {
            double th = (std::clamp(p.v[k].pan, -1.0, 1.0) + 1) * 0.25 * grains::kPi;
            panL[k] = std::cos(th);
            panR[k] = std::sin(th);
        }
        const double dry = std::max(0.0, p.dry), wet = std::max(0.0, p.wet);
        for (int i = 0; i < n; ++i) {
            double x = in ? (double)in[i] : 0.0;
            if (!std::isfinite(x)) x = 0;
            const int64_t t = t_;
            ring_[(size_t)t & mask_] = x;
            lp_[(size_t)t & mask_] = markLp_.tick(x);
            ++t_;
            if (yin_.push(x)) newEstimate();
            placeMarks();
            // the delayed time: what is heard now
            const double td = (double)(t - delay_);
            f0Delayed_ = f0At(td);
            const bool voiced = f0Delayed_ > 0;
            voicedEnv_ = std::clamp(voicedEnv_ + (voiced ? gateStep : -gateStep), 0.0, 1.0);
            if (voiced && p.mode == HarmonyMode::Scale) snap(f0Delayed_);
            const double xd = t >= delay_ ? ring_[(size_t)(t - delay_) & mask_] : 0.0;
            double outL = dry * xd, outR = dry * xd;
            for (int k = 0; k < nv; ++k) {
                VoiceState& v = vs_[k];
                const Voice& pv = p.v[k];
                double ratio = 0;
                const bool active = voiceRatio(k, voiced ? f0Delayed_ : 0, ratio) && voiced;
                // the voice's own gate: a note or the mode lets it sound, on voiced input; each onset waits the
                // humanizing delay
                if (active && !v.on) {
                    v.on = true;
                    v.wait = p.humanizeMs > 0 ? (long)(rnd::uniform01(rng_) * p.humanizeMs * 1e-3 * sr) : 0;
                } else if (!active) v.on = false;
                bool sounding = v.on && v.wait <= 0;
                if (v.on && v.wait > 0) --v.wait;
                v.gate = std::clamp(v.gate + (sounding ? gateStep : -gateStep), 0.0, 1.0);
                double s = 0;
                if (sounding && voiced) {
                    double cents = modulation(v, pv, sr);
                    double fo = f0Delayed_ * ratio * std::exp2(cents / 1200.0);
                    v.phase += fo / sr;
                    if (v.phase >= 1.0) {
                        v.phase -= std::floor(v.phase);
                        spawn(v, pv, td, ratio, fo);
                    }
                } else if (!voiced) v.phase = 1.0;       // the next voiced sample starts a grain
                s = renderGrains(v);
                // the level match: once the voice has sounded 50 ms, its gain follows the ratio of the input's
                // power to its grains' (150 ms each), within -12..+12 dB, in 50 ms: what the overlap of the windows
                // does not settle (a pure tone shifted up loses the energy its grains cannot carry) is made up
                if (sounding && voiced && v.gate >= 1.0) {
                    if (v.agcAge == 0) v.pin = v.pout = 0;
                    v.pin += powerStep * (xd * xd - v.pin);
                    v.pout += powerStep * (s * s - v.pout);
                    if (++v.agcAge > agcHold && v.pout > 1e-14 && v.pin > 1e-14)
                        v.agc += agcStep * (std::clamp(std::sqrt(v.pin / v.pout), 0.25, 4.0) - v.agc);
                } else v.agcAge = 0;
                double level = std::max(0.0, pv.level) * (p.mode == HarmonyMode::MidiChord ? v.vel : 1.0);
                double y = v.gate * level * (voicedEnv_ * s * v.agc + (p.unvoicedDry ? (1.0 - voicedEnv_) * xd : 0.0));
                if (voiceOut && voiceOut[k]) voiceOut[k][i] = (T)y;
                outL += wet * y * panL[k];
                outR += wet * y * panR[k];
            }
            if (voiceOut)
                for (int k = nv; k < kMaxVoices; ++k) if (voiceOut[k]) voiceOut[k][i] = (T)0;
            if (L) L[i] = (T)outL;
            if (R) R[i] = (T)outR;
        }
    }

    int voices() const { return std::clamp(p.voices, 1, kMaxVoices); }
    // the auto number of input periods per grain at a ratio
    static double autoCycles(double r) {
        if (r < 1) return std::clamp(2.0 * r, 1.0, 2.0);
        if (r < 1.5) return 2.0;
        return std::max(1.0, 3.0 / r);
    }

private:
    struct Mark { double pos = 0, period = 0; };
    struct Hist { double at = 0, f0 = 0; };
    struct Grain {
        bool active = false;
        double read = 0, inc = 1, envPos = 0, envStep = 0, amp = 0;
    };
    struct VoiceState {
        grains::GrainPool<Grain> pool;
        double phase = 1, gate = 0, vel = 0;
        int note = -1;
        long wait = 0;
        bool on = false;
        double vibPhase = 0, vibRateNow = 0, vibDepthNow = 0;
        double drift = 0, driftTarget = 0;
        long driftCount = 0;
        double pin = 0, pout = 0, agc = 1;   // the level match: the powers of the input and of the grains
        long agcAge = 0;
    };

    void newEstimate() {
        f0Now_ = yin_.f0();
        Hist h;
        h.at = (double)(t_ - yin_.lag());
        h.f0 = f0Now_;
        hist_[(size_t)histHead_] = h;
        histHead_ = (histHead_ + 1) % kHistory;
        nHist_ = std::min(kHistory, nHist_ + 1);
        if (f0Now_ > 0) markLp_.set(rbj(BiquadType::Lowpass, set_.sr, std::min(1.5 * f0Now_, 0.4 * set_.sr), 0.7071));
    }
    const Hist& hist(int k) const { return hist_[(size_t)((histHead_ - nHist_ + k + kHistory) % kHistory)]; }   // 0 oldest
    // the f0 at time x (a step between frames: unvoiced on either side is unvoiced)
    double f0At(double x) {
        if (nHist_ == 0) return 0;
        if (histCursor_ >= nHist_) histCursor_ = nHist_ - 1;
        while (histCursor_ > 0 && hist(histCursor_).at > x) --histCursor_;
        while (histCursor_ + 1 < nHist_ && hist(histCursor_ + 1).at <= x) ++histCursor_;
        const Hist& a = hist(histCursor_);
        if (a.at > x) return 0;                          // before the first frame
        if (histCursor_ + 1 >= nHist_) return a.f0;
        const Hist& b = hist(histCursor_ + 1);
        if (!(a.f0 > 0) || !(b.f0 > 0)) return x - a.at < 0.5 * (b.at - a.at) ? a.f0 : b.f0;   // a voicing edge: the nearer
        double u = (x - a.at) / std::max(1e-9, b.at - a.at);
        return a.f0 * std::exp2(u * std::log2(b.f0 / a.f0));
    }
    const Mark& mark(int k) const { return marks_[(size_t)((markHead_ - nMarks_ + k + kMarks) % kMarks)]; }   // 0 oldest
    void addMark(double pos, double period) {
        marks_[(size_t)markHead_] = Mark{pos, period};
        markHead_ = (markHead_ + 1) % kMarks;
        nMarks_ = std::min(kMarks, nMarks_ + 1);
    }
    // the index of the lp ring's largest value in [a, b] (clamped to what is written), refined by a parabola
    double peak(int64_t a, int64_t b) const {
        a = std::max<int64_t>(a, t_ - (int64_t)ring_.size() + 2);
        b = std::min<int64_t>(b, t_ - 2);
        if (b < a) return (double)b;
        int64_t best = a;
        double bv = lp_[(size_t)a & mask_];
        for (int64_t j = a + 1; j <= b; ++j) {
            double v = lp_[(size_t)j & mask_];
            if (v > bv) { bv = v; best = j; }
        }
        double y0 = lp_[(size_t)(best - 1) & mask_], y1 = bv, y2 = lp_[(size_t)(best + 1) & mask_], den = y0 - 2 * y1 + y2;
        double off = den < 0 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
        return (double)best + off;
    }
    void placeMarks() {
        if (!(f0Now_ > 0)) return;
        const double P = set_.sr / f0Now_;
        const double now = (double)t_ - 1;
        if (nMarks_ == 0 || now - mark(nMarks_ - 1).pos > 2.5 * pmax_) {   // a new voiced stretch: the peak of the last period
            if (now < P) return;
            addMark(peak((int64_t)(now - P), (int64_t)now), P);
            return;
        }
        double last = mark(nMarks_ - 1).pos;
        while (now >= last + 1.25 * P) {
            double next = p.marksPeriod ? last + P : peak((int64_t)std::ceil(last + 0.75 * P), (int64_t)std::floor(last + 1.25 * P));
            if (next <= last + 0.5 * P) next = last + P;
            addMark(next, P);
            last = next;
        }
    }
    // the mark nearest to time s
    bool nearestMark(double s, Mark& out) const {
        if (nMarks_ == 0) return false;
        int k = nMarks_ - 1;
        while (k > 0 && mark(k).pos > s) --k;
        out = mark(k);
        if (k + 1 < nMarks_ && std::fabs(mark(k + 1).pos - s) < std::fabs(out.pos - s)) out = mark(k + 1);
        return true;
    }
    static int pitchClass(int m) { return ((m % 12) + 12) % 12; }
    bool inScale(int m) const { int mask = p.scaleMask & 0xFFF; return mask == 0 || ((mask >> pitchClass(m)) & 1); }
    // the scale note the input sings, with a little hysteresis
    void snap(double f0) {
        double m = 69 + 12 * std::log2(f0 / 440.0);
        if (q_ > -1000 && inScale(q_) && std::fabs(m - q_) < 0.65) return;
        int c = (int)std::lround(m), best = c;
        double bd = 1e9;
        for (int d = -6; d <= 6; ++d)
            if (inScale(c + d) && std::fabs(c + d - m) < bd) { bd = std::fabs(c + d - m); best = c + d; }
        q_ = best;
    }
    int stepScale(int from, int steps) const {
        int m = from;
        for (int k = 0; k < std::abs(steps); ++k) {
            int d = steps > 0 ? 1 : -1;
            m += d;
            for (int j = 0; j < 12 && !inScale(m); ++j) m += d;
        }
        return m;
    }
    // voice k's ratio to the input now; false when it is silent (no note held, a midi mode)
    bool voiceRatio(int k, double f0, double& ratio) {
        const VoiceState& v = vs_[k];
        double tr = p.transpose;
        double in = f0 > 0 ? 69 + 12 * std::log2(f0 / 440.0) : 60;
        switch (p.mode) {
            case HarmonyMode::Intervals: ratio = std::exp2((p.v[k].interval + tr) / 12.0); break;
            case HarmonyMode::Scale: {
                if (q_ <= -1000) { ratio = 1; return false; }
                int target = stepScale(q_, (int)std::lround(p.v[k].interval));
                ratio = std::exp2((target - q_ + tr) / 12.0);
                break;
            }
            case HarmonyMode::MidiChord:
                if (v.note < 0) { ratio = 1; return false; }
                ratio = std::exp2((v.note + tr - in) / 12.0);
                break;
            case HarmonyMode::MidiRoot: {
                int root = -1;
                uint64_t latest = 0;
                for (int j = 0; j < 128; ++j) if (noteOrder_[j] > latest) { latest = noteOrder_[j]; root = j; }
                if (root < 0) { ratio = 1; return false; }
                ratio = std::exp2((root + p.v[k].interval + tr - in) / 12.0);
                break;
            }
        }
        ratio = std::clamp(ratio, 0.125, 8.0);
        return true;
    }
    // vibrato and the humanizing drift of a voice, in cents, a sample on
    double modulation(VoiceState& v, const Voice& pv, double sr) {
        double cents = 0;
        if (pv.vibDepth > 0 && pv.vibRate > 0) {
            if (v.vibRateNow <= 0) newVibratoCycle(v, pv);
            cents += v.vibDepthNow * std::sin(2 * grains::kPi * v.vibPhase);
            v.vibPhase += v.vibRateNow / sr;
            if (v.vibPhase >= 1) { v.vibPhase -= 1; newVibratoCycle(v, pv); }
        }
        if (p.humanizeCents > 0) {
            if (--v.driftCount <= 0) {                   // a new target every 80-200 ms, approached smoothly
                v.driftTarget = (rnd::uniform01(rng_) * 2 - 1) * p.humanizeCents;
                v.driftCount = (long)((0.08 + 0.12 * rnd::uniform01(rng_)) * sr);
            }
            v.drift += (v.driftTarget - v.drift) * (1.0 / (0.05 * sr));
            cents += v.drift;
        }
        return cents;
    }
    void newVibratoCycle(VoiceState& v, const Voice& pv) {
        double r = std::clamp(pv.vibRandom, 0.0, 1.0);
        v.vibRateNow = pv.vibRate * (1 + r * 0.3 * (rnd::uniform01(rng_) * 2 - 1));
        v.vibDepthNow = pv.vibDepth * (1 + r * 0.5 * (rnd::uniform01(rng_) * 2 - 1));
    }
    // a grain for voice v, starting now (the delayed time td), at output frequency fo
    void spawn(VoiceState& v, const Voice& pv, double td, double ratio, double fo) {
        const double P = set_.sr / f0Delayed_;
        double c = p.cycles > 0 ? std::clamp(p.cycles, 0.5, 4.0) : autoCycles(ratio);
        double L = c * P;                                 // output samples
        Mark m;
        if (!nearestMark(td + 0.5 * L, m)) return;
        int slot = v.pool.firstFree(kGrains);
        if (slot < 0) return;
        L = std::max(4.0, c * m.period);
        double fmt = std::clamp(pv.formant, 0.25, 2.0);
        Grain& g = v.pool[slot];
        g.read = m.pos - 0.5 * L * fmt;
        g.inc = fmt;
        g.envPos = 0;
        g.envStep = grains::GrainWindow::kSize / L;
        // the windows overlap L / H times (H the output period): their sum, on average, is that times the
        // window's mean; a voice keeps the input's level when the grains add up to it
        double overlap = L * fo / set_.sr;
        g.amp = 1.0 / std::sqrt(std::max(0.05, overlap * window_.meanSquare()));
        g.active = true;
    }
    double renderGrains(VoiceState& v) {
        double s = 0;
        const double newest = (double)(t_ - 2);
        for (auto& g : v.pool) {
            if (!g.active) continue;
            if (g.envPos >= grains::GrainWindow::kSize) { g.active = false; continue; }
            double r = std::min(g.read, newest);
            double fl = std::floor(r);
            int64_t i0 = (int64_t)fl;
            double a = ring_[(size_t)i0 & mask_], b = ring_[(size_t)(i0 + 1) & mask_];
            s += window_.at(g.envPos) * g.amp * (a + (r - fl) * (b - a));
            g.read += g.inc;
            g.envPos += g.envStep;
        }
        return s;
    }

    Setup set_;
    StreamingYin yin_;
    BiquadSection markLp_;
    grains::GrainWindow window_;
    int windowType_ = 0;
    std::vector<double> ring_, lp_;
    size_t mask_ = 0;
    int64_t t_ = 0;
    long delay_ = 0;
    double pmax_ = 0;
    std::vector<Mark> marks_;
    int nMarks_ = 0, markHead_ = 0;
    std::vector<Hist> hist_;
    int nHist_ = 0, histHead_ = 0, histCursor_ = 0;
    double voicedEnv_ = 0, f0Now_ = 0, f0Delayed_ = 0;
    VoiceState vs_[kMaxVoices];
    uint64_t noteOrder_[128] = {};
    uint64_t order_ = 0;
    int held_ = 0, q_ = -1000;
    std::mt19937 rng_{1};
    uint32_t seed_ = 1;
};

}  // namespace dsp
}  // namespace musil
