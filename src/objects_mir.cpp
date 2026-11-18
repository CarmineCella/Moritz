// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_mir.cpp — analysis and music information retrieval, after Musil.
//
// Offline (background jobs on a buffer~, like nmf): spectrogram, melgram,
// mfccgram, chromagram (feature matrices), onsets, beats (tempo, beat times,
// tempogram), keyfind, similarity (self-similarity matrix), novelty (section
// boundaries) and curves (loudness, register, polyphony).
// Live (on a signal, a list every hop): melbands~, mfcc~, chroma~, onset~.
//
// A matrix is stored in a buffer~: one channel a row (a band, a pitch class,
// a coefficient), one sample a frame, and the buffer's sample rate is the
// number of frames a second, so a plot knows the time of every frame.
// Times given to and by the objects are in ms, as everywhere in Moritz.

#include "mir.h"
#include "offline.h"

#include <atomic>
#include <memory>

namespace moritz {
namespace {

using mir::Mat;
using mir::Vec;

int pow2In(float v, int lo, int hi) {
    int n = 1;
    while (n < (int)v) n <<= 1;
    return std::clamp(n, lo, hi);
}

mir::Tick tickOf(Job& job, float from, float to) {
    return [&job, from, to](double p) {
        job.progress(from + (to - from) * (float)p);
        return !job.cancelled();
    };
}

// a matrix (rows) into a buffer~, frames at fps a second
bool storeMatrix(Patch* patch, const std::string& name, const Mat& rows, double fps) {
    std::vector<std::vector<float>> ch(rows.size());
    for (size_t r = 0; r < rows.size(); ++r) ch[r].assign(rows[r].begin(), rows[r].end());
    return storeBuffer(patch, name, ch, fps);
}
// a buffer~ as a matrix: its channels are the rows
Mat matrixOf(const SampleBuffer& b) {
    Mat m((size_t)b.channels, Vec((size_t)b.frames));
    for (long f = 0; f < b.frames; ++f)
        for (int c = 0; c < b.channels; ++c) m[(size_t)c][(size_t)f] = b.data[(size_t)f * b.channels + c];
    return m;
}
Message msList(const Vec& seconds) {
    Message m;
    for (double s : seconds) m.push_back(Atom((float)(std::round(s * 10000.0) / 10.0)));
    return m;
}

// The common shape of the offline analyses: `size` and `hop` (samples), and
// a buffer~ of sound to read.
class AnalysisBox : public OfflineBox {
protected:
    AnalysisBox(int extraOutlets, int size, int hop) : size_(size), hop_(hop) {
        setPorts({P::Control}, std::vector<Port>((size_t)(2 + extraOutlets), P::Control));
    }
    // common messages; true if handled
    bool common(const Message& m) {
        if (m[0].is("stop")) { job_.cancel(); return true; }
        if (m[0].is("size") && m.size() > 1) { size_ = pow2In(argf(m, 1, 2048.f), 64, 65536); return true; }
        if (m[0].is("hop") && m.size() > 1) { hop_ = std::clamp((int)argf(m, 1, 512.f), 16, 65536); return true; }
        return false;
    }
    // the sound of a buffer~, mixed to mono; false (and a message) if none
    bool sound(const std::string& name, std::shared_ptr<std::vector<float>>& x, double& sr) {
        BufferBox* b = findBuffer(patch, name);
        if (!b || b->buf.frames <= 0) { patch->print(cls + ": no sound in buffer~ " + name); return false; }
        x = std::make_shared<std::vector<float>>(monoOf(b->buf));
        sr = b->buf.sr;
        return true;
    }
    bool matrix(const std::string& name, std::shared_ptr<Mat>& m, double& fps) {
        BufferBox* b = findBuffer(patch, name);
        if (!b || b->buf.frames <= 0) { patch->print(cls + ": nothing in buffer~ " + name); return false; }
        m = std::make_shared<Mat>(matrixOf(b->buf));
        fps = b->buf.sr;
        return true;
    }
    int size_, hop_;
};

// ---------------------------------------------------------------- spectrogram
// spectrogram [size] [hop]: `run sound matrix` writes the magnitude
// spectrogram of a buffer~ (in dB, 0 dB a full-scale sine, down to -100)
// into another: one channel a bin, up to `maxfreq` Hz (all of them by
// default). Hann window of size samples (2048), a frame every hop (512).
class SpectrogramBox : public AnalysisBox {
public:
    explicit SpectrogramBox(const Message& a)
        : AnalysisBox(0, pow2In(argf(a, 0, 2048.f), 64, 65536), std::clamp((int)argf(a, 1, 512.f), 16, 65536)) {}
    void message(int, const Message& m) override {
        if (common(m)) return;
        if (m[0].is("maxfreq")) maxf_ = std::max(0.f, argf(m, 1, 0.f));
        else if (m[0].is("run") && m.size() > 2) start(m[1].str(), m[2].str());
    }

private:
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        std::shared_ptr<std::vector<float>> x;
        double sr;
        if (!sound(src, x, sr)) return;
        int n = size_, hop = hop_;
        double maxf = maxf_;
        auto out = std::make_shared<Mat>();
        run(
            [x, n, hop, sr, maxf, out](Job& job) {
                Mat mag = mir::magSpectrogram(*x, n, hop, tickOf(job, 0, .9f));
                if (mag.empty()) return;
                int bins = n / 2 + 1;
                if (maxf > 0) bins = std::clamp((int)std::ceil(maxf * n / sr) + 1, 2, bins);
                out->assign((size_t)bins, Vec(mag.size()));
                for (size_t f = 0; f < mag.size(); ++f)
                    for (int b = 0; b < bins; ++b) (*out)[(size_t)b][f] = mir::toDb(mag[f][(size_t)b] * mag[f][(size_t)b]);
            },
            [this, dst, out, sr, hop] { if (!out->empty()) storeMatrix(patch, dst, *out, sr / hop); });
    }
    float maxf_ = 0.f;
};

// ---------------------------------------------------------------- melgram, mfccgram
// melgram [bands] [size] [hop]: `run sound matrix` writes the mel
// spectrogram: the energy (dB) in bands (40) triangular filters evenly
// spaced on the mel scale, the ear's resolution. mfccgram [coeffs] [bands]
// [size] [hop]: the mel-frequency cepstral coefficients (13 of them): the
// cosine transform of each mel frame, a smooth description of the spectral
// envelope (the timbre); coefficient 0 is the overall level.
class MelgramBox : public AnalysisBox {
public:
    MelgramBox(const Message& a, bool mfcc)
        : AnalysisBox(0, pow2In(argf(a, mfcc ? 2 : 1, 2048.f), 64, 65536),
                      std::clamp((int)argf(a, mfcc ? 3 : 2, 512.f), 16, 65536)),
          mfcc_(mfcc) {
        if (mfcc) {
            coeffs_ = std::clamp((int)argf(a, 0, 13.f), 1, 128);
            bands_ = std::clamp((int)argf(a, 1, 40.f), 2, 256);
            coeffs_ = std::min(coeffs_, bands_);
        } else {
            bands_ = std::clamp((int)argf(a, 0, 40.f), 2, 256);
        }
    }
    void message(int, const Message& m) override {
        if (common(m)) return;
        if (m[0].is("bands")) bands_ = std::clamp((int)argf(m, 1, 40.f), 2, 256);
        else if (m[0].is("coeffs")) coeffs_ = std::clamp((int)argf(m, 1, 13.f), 1, 128);
        else if (m[0].is("run") && m.size() > 2) start(m[1].str(), m[2].str());
    }

private:
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        std::shared_ptr<std::vector<float>> x;
        double sr;
        if (!sound(src, x, sr)) return;
        int n = size_, hop = hop_, bands = bands_, coeffs = std::min(coeffs_, bands_);
        bool mfcc = mfcc_;
        auto out = std::make_shared<Mat>();
        run(
            [x, n, hop, sr, bands, coeffs, mfcc, out](Job& job) {
                Mat mag = mir::magSpectrogram(*x, n, hop, tickOf(job, 0, .8f));
                if (mag.empty()) return;
                mir::MelBank bank;
                bank.init(sr, n, bands);
                int rows = mfcc ? coeffs : bands;
                out->assign((size_t)rows, Vec(mag.size()));
                Vec mel, c;
                for (size_t f = 0; f < mag.size(); ++f) {
                    bank.apply(mag[f], mel);
                    if (mfcc) mir::dct(mel, coeffs, c);
                    const Vec& v = mfcc ? c : mel;
                    for (int r = 0; r < rows; ++r) (*out)[(size_t)r][f] = v[(size_t)r];
                }
            },
            [this, dst, out, sr, hop] { if (!out->empty()) storeMatrix(patch, dst, *out, sr / hop); });
    }
    bool mfcc_;
    int bands_ = 40, coeffs_ = 13;
};

// ---------------------------------------------------------------- chromagram
// chromagram [size] [hop]: `run sound matrix` writes the chromagram: 12
// channels, C C# D ... B: every note from C2 to B6 read in the spectrum and
// added to its pitch class (log-compressed), every frame scaled to a maximum
// of 1 (silence is 0).
// A large window (4096) resolves the low notes; a frame every 1024 samples.
class ChromagramBox : public AnalysisBox {
public:
    explicit ChromagramBox(const Message& a)
        : AnalysisBox(0, pow2In(argf(a, 0, 4096.f), 256, 65536), std::clamp((int)argf(a, 1, 1024.f), 16, 65536)) {}
    void message(int, const Message& m) override {
        if (common(m)) return;
        if (m[0].is("run") && m.size() > 2) start(m[1].str(), m[2].str());
    }

private:
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        std::shared_ptr<std::vector<float>> x;
        double sr;
        if (!sound(src, x, sr)) return;
        int n = size_, hop = hop_;
        auto out = std::make_shared<Mat>();
        run(
            [x, n, hop, sr, out](Job& job) {
                Mat mag = mir::magSpectrogram(*x, n, hop, tickOf(job, 0, .9f));
                if (mag.empty()) return;
                auto pc = mir::chromaMap(sr, n);
                out->assign(12, Vec(mag.size()));
                Vec c;
                for (size_t f = 0; f < mag.size(); ++f) {
                    mir::chroma(mag[f], pc, c);
                    for (int r = 0; r < 12; ++r) (*out)[(size_t)r][f] = c[(size_t)r];
                }
            },
            [this, dst, out, sr, hop] { if (!out->empty()) storeMatrix(patch, dst, *out, sr / hop); });
    }
};

// ---------------------------------------------------------------- onsets
// onsets [threshold] [size] [hop]: `run sound [curve]` finds where the notes
// and hits begin: the peaks of the spectral flux (how much the spectrum
// grows from frame to frame) above threshold (2) times its moving median
// over `width` frames (21), at least `gap` ms apart (50), each placed at the
// sample where the energy jumps. Outlet 3: the onset times (ms), a list; the
// flux goes to the curve buffer~ if one is named. Size 1024, hop 256.
// The onsets cut the sound into segments (the first from the beginning, the
// last to the end): outlet 5 gives their number after a run, and `segment k`
// outputs the start and end of segment k (ms, from 0; k wraps around) from
// outlet 4, ready for play~ (start $1 $2).
class OnsetsBox : public AnalysisBox {
public:
    explicit OnsetsBox(const Message& a)
        : AnalysisBox(3, pow2In(argf(a, 1, 1024.f), 64, 65536), std::clamp((int)argf(a, 2, 256.f), 16, 65536)) {
        thr_ = std::max(0.f, argf(a, 0, 2.f));
    }
    void message(int, const Message& m) override {
        if (common(m)) return;
        if (m[0].is("threshold")) thr_ = std::max(0.f, argf(m, 1, 2.f));
        else if (m[0].is("width")) width_ = std::clamp((int)argf(m, 1, 21.f), 1, 1001) | 1;
        else if (m[0].is("gap")) gap_ = std::max(0.f, argf(m, 1, 50.f));
        else if (m[0].is("run") && m.size() > 1) start(m[1].str(), argstr(m, 2));
        else if (m[0].is("segment") && m.size() > 1 && bounds_.size() > 1) {
            long count = (long)bounds_.size() - 1, k = ((long)argf(m, 1, 0.f) % count + count) % count;
            outlet(3, {Atom((float)(bounds_[(size_t)k] * 1000.0)), Atom((float)(bounds_[(size_t)k + 1] * 1000.0))});
        }
    }

private:
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        std::shared_ptr<std::vector<float>> x;
        double sr;
        if (!sound(src, x, sr)) return;
        double dur = (double)x->size() / sr;
        int n = size_, hop = hop_, width = width_;
        double thr = thr_, gap = gap_ / 1000.0;
        auto flux = std::make_shared<Vec>();
        auto times = std::make_shared<Vec>();
        run(
            [x, n, hop, sr, width, thr, gap, flux, times](Job& job) {
                Mat mag = mir::magSpectrogram(*x, n, hop, tickOf(job, 0, .9f));
                if (mag.empty()) return;
                *flux = mir::fluxOf(mag);
                *times = mir::onsetTimes(*x, sr, n, hop, *flux, thr, width, gap);
            },
            [this, dst, flux, times, sr, hop, dur] {
                if (!dst.empty() && !flux->empty()) storeMatrix(patch, dst, Mat{*flux}, sr / hop);
                bounds_.clear();
                if (times->empty() || times->front() > .02) bounds_.push_back(0.0);
                for (double t : *times) bounds_.push_back(t);
                bounds_.push_back(dur);
                outletFloat(4, (float)(bounds_.size() - 1));
                if (times->empty()) patch->print("onsets: none found");
                else outlet(2, msList(*times));
            });
    }
    Vec bounds_;
    float thr_, gap_ = 50.f;
    int width_ = 21;
};

// ---------------------------------------------------------------- beats
// beats [size] [hop]: `run sound [curve]` estimates the tempo and tracks the
// beats: the onset strength (spectral flux), its autocorrelation read at
// 30..300 bpm and weighted by a prior on the tempo listeners tap (`prior
// centre width`: 100 bpm, 0.6 octaves), then dynamic programming for the
// beats that are strong and regular (`tightness`, 100; Ellis 2007).
// Outlets: progress, done, tempo (bpm), beat times (ms, a list). The onset
// strength goes to curve if named. `tempogram sound matrix` writes the
// tempogram: 4 times a second, how periodic the onsets are at each tempo
// (1: perfectly), one channel a bpm from 30 (channel 0) to 300. After a run, `clicks buffer`
// writes a click track into a buffer~ (as long as the sound, a short 1 kHz
// click on every beat): play it with the sound, both started together.
class BeatsBox : public AnalysisBox {
public:
    explicit BeatsBox(const Message& a)
        : AnalysisBox(2, pow2In(argf(a, 0, 1024.f), 64, 65536), std::clamp((int)argf(a, 1, 256.f), 16, 65536)) {}
    void message(int, const Message& m) override {
        if (common(m)) return;
        if (m[0].is("prior")) { centre_ = std::clamp(argf(m, 1, 100.f), 30.f, 300.f); width_ = std::max(.05f, argf(m, 2, .6f)); }
        else if (m[0].is("tightness")) tight_ = std::max(0.f, argf(m, 1, 100.f));
        else if (m[0].is("run") && m.size() > 1) start(m[1].str(), argstr(m, 2), false);
        else if (m[0].is("tempogram") && m.size() > 2) start(m[1].str(), m[2].str(), true);
        else if (m[0].is("clicks") && m.size() > 1) clicks(m[1].str());
    }

private:
    void clicks(const std::string& dst) {
        if (beats_.empty()) { patch->print("beats: no beats yet (run sound first)"); return; }
        std::vector<float> out((size_t)std::max(1.0, len_ * sr_), 0.f);
        long n = (long)(.02 * sr_);
        for (double t : beats_) {
            long at = (long)(t * sr_);
            for (long k = 0; k < n && at + k < (long)out.size(); ++k)
                out[(size_t)(at + k)] += (float)(.8 * std::sin(dsp::kTwoPi * 1000.0 * k / sr_) * std::exp(-200.0 * k / sr_));
        }
        storeBuffer(patch, dst, {out}, sr_);
    }
    void start(const std::string& src, const std::string& dst, bool tempogram) {
        if (busy()) return;
        std::shared_ptr<std::vector<float>> x;
        double sr;
        if (!sound(src, x, sr)) return;
        if (!tempogram) { sr_ = sr; len_ = (double)x->size() / sr; }
        int n = size_, hop = hop_;
        double centre = centre_, width = width_, tight = tight_, fps = sr / hop;
        auto flux = std::make_shared<Vec>();
        auto beats = std::make_shared<Vec>();
        auto tg = std::make_shared<Mat>();
        auto bpm = std::make_shared<double>(0);
        run(
            [x, n, hop, fps, centre, width, tight, tempogram, flux, beats, tg, bpm](Job& job) {
                Mat mag = mir::magSpectrogram(*x, n, hop, tickOf(job, 0, .7f));
                if (mag.empty()) return;
                *flux = mir::fluxOf(mag);
                if (tempogram) {                       // 4 columns a second, over 8 s
                    *tg = mir::tempogram(*flux, fps, (int)std::lround(fps * 8), std::max(1, (int)std::lround(fps / 4)));
                    return;
                }
                *bpm = mir::tempoEstimate(*flux, fps, centre, width);
                for (int f : mir::beatTrack(*flux, fps, *bpm, tight)) beats->push_back(f / fps);
            },
            [this, dst, tempogram, flux, beats, tg, bpm, fps] {
                if (tempogram) {
                    if (!tg->empty()) storeMatrix(patch, dst, *tg, fps / std::max(1, (int)std::lround(fps / 4)));
                    return;
                }
                if (!dst.empty() && !flux->empty()) storeMatrix(patch, dst, Mat{*flux}, fps);
                beats_ = *beats;
                if (!beats->empty()) outlet(3, msList(*beats));
                if (*bpm > 0) outletFloat(2, (float)*bpm);
            });
    }
    float centre_ = 100.f, width_ = .6f, tight_ = 100.f;
    Vec beats_;
    double sr_ = 44100, len_ = 0;
};

// ---------------------------------------------------------------- keyfind
// keyfind [size] [hop]: `run sound [from to]` estimates the key (of the
// whole sound, or between from and to ms): the chromagram
// averaged over the sound (a pitch-class distribution) correlated with the
// Krumhansl-Kessler profile of every major and minor key (Krumhansl 1990).
// Outlets: progress, done, the key ("A minor"), its correlation (-1..1),
// the pitch-class distribution (12 values, C first, a maximum of 1), the
// correlation with all 24 keys (C major, C minor, C# major, ... B minor).
class KeyfindBox : public AnalysisBox {
public:
    explicit KeyfindBox(const Message& a)
        : AnalysisBox(4, pow2In(argf(a, 0, 4096.f), 256, 65536), std::clamp((int)argf(a, 1, 1024.f), 16, 65536)) {}
    void message(int, const Message& m) override {
        if (common(m)) return;
        if (m[0].is("run") && m.size() > 1) start(m[1].str(), argf(m, 2, 0.f), argf(m, 3, 0.f));
    }

private:
    void start(const std::string& src, float from, float to) {
        if (busy()) return;
        std::shared_ptr<std::vector<float>> x;
        double sr;
        if (!sound(src, x, sr)) return;
        if (to > from) {                                   // a part of the sound
            long a = std::clamp((long)(from / 1000.0 * sr), 0L, (long)x->size());
            long b = std::clamp((long)(to / 1000.0 * sr), a, (long)x->size());
            x = std::make_shared<std::vector<float>>(x->begin() + a, x->begin() + b);
            if (x->empty()) { patch->print("keyfind: nothing between " + std::to_string((int)from) + " and " + std::to_string((int)to) + " ms"); return; }
        }
        int n = size_, hop = hop_;
        auto pcd = std::make_shared<Vec>(12, 0.0);
        auto scores = std::make_shared<Vec>();
        run(
            [x, n, hop, sr, pcd, scores](Job& job) {
                Mat mag = mir::magSpectrogram(*x, n, hop, tickOf(job, 0, .9f));
                if (mag.empty()) return;
                auto pc = mir::chromaMap(sr, n);
                Vec c;
                for (auto& frame : mag) {
                    mir::chroma(frame, pc, c);
                    for (int k = 0; k < 12; ++k) (*pcd)[(size_t)k] += c[(size_t)k];
                }
                double mx = mir::maxOf(*pcd);
                if (mx > 0)
                    for (auto& v : *pcd) v /= mx;
                *scores = mir::keyScores(*pcd);
            },
            [this, pcd, scores] {
                if (scores->size() != 24) return;
                int best = (int)(std::max_element(scores->begin(), scores->end()) - scores->begin());
                Message s, p;
                for (double v : *scores) s.push_back(Atom((float)v));
                for (double v : *pcd) p.push_back(Atom((float)v));
                outlet(5, s);
                outlet(4, p);
                outletFloat(3, (float)(*scores)[(size_t)best]);
                outlet(2, {Atom(mir::keyName(best / 2, best % 2)), Atom(best % 2 ? "minor" : "major")});
            });
    }
};

// ---------------------------------------------------------------- similarity
// similarity [frames]: `run features matrix` writes the self-similarity
// matrix of a feature matrix (a chromagram, an MFCC matrix...): the cosine
// similarity of every frame with every other, 1 where two moments sound
// alike (Foote 1999). The structure of a piece becomes a picture: sections
// are squares on the diagonal, repeats are stripes beside it. Frames are
// averaged in groups so the matrix has at most `frames` of them (1000).
// `standardize 1` first takes each feature's mean off and scales it to unit
// variance (useful for MFCCs, whose coefficient 0 would dominate).
class SimilarityBox : public AnalysisBox {
public:
    explicit SimilarityBox(const Message& a) : AnalysisBox(0, 0, 0) {
        max_ = std::clamp((int)argf(a, 0, 1000.f), 16, 4000);
    }
    void message(int, const Message& m) override {
        if (m[0].is("stop")) { job_.cancel(); return; }
        if (m[0].is("frames")) max_ = std::clamp((int)argf(m, 1, 1000.f), 16, 4000);
        else if (m[0].is("standardize")) std_ = argf(m, 1, 1.f) != 0.f;
        else if (m[0].is("run") && m.size() > 2) start(m[1].str(), m[2].str());
    }

private:
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        std::shared_ptr<Mat> F;
        double fps;
        if (!matrix(src, F, fps)) return;
        int mx = max_;
        bool standardize = std_;
        size_t frames = (*F)[0].size();
        int group = (int)((frames + (size_t)mx - 1) / (size_t)mx);
        double outFps = fps / std::max(1, group);
        auto S = std::make_shared<Mat>();
        run(
            [F, group, standardize, S](Job& job) {
                Mat G;
                for (auto& row : *F) {                   // average groups of frames
                    Vec r;
                    for (size_t f = 0; f < row.size(); f += (size_t)group) {
                        double s = 0;
                        size_t k = f;
                        for (; k < row.size() && k < f + (size_t)group; ++k) s += row[k];
                        r.push_back(s / (double)(k - f));
                    }
                    G.push_back(std::move(r));
                }
                if (standardize)
                    for (auto& r : G) {
                        double m = mir::meanOf(r), v = 0;
                        for (double x : r) v += (x - m) * (x - m);
                        double sd = std::sqrt(v / std::max<size_t>(1, r.size()));
                        for (auto& x : r) x = sd > 1e-12 ? (x - m) / sd : 0;
                    }
                *S = mir::selfSimilarity(G, tickOf(job, 0, 1));
            },
            [this, dst, S, outFps] { if (!S->empty()) storeMatrix(patch, dst, *S, outFps); });
    }
    int max_;
    bool std_ = false;
};

// ---------------------------------------------------------------- novelty
// novelty [width] [threshold]: `run similarity curve` slides a checkerboard
// kernel of width frames (32) along the diagonal of a self-similarity
// matrix (Foote 2000): high where what comes before is unlike what comes
// after. The curve goes to a buffer~; outlet 3 gives the boundaries of the
// sections (ms): the peaks of the curve above threshold (1.5) times its
// moving median, at least `gap` ms apart (2000). The boundaries cut the
// piece into sections: outlet 5 gives their number, and `section k` outputs
// the start and end of section k (ms, from 0; k wraps around) from outlet 4.
class NoveltyBox : public AnalysisBox {
public:
    explicit NoveltyBox(const Message& a) : AnalysisBox(3, 0, 0) {
        width_ = std::clamp((int)argf(a, 0, 32.f), 2, 512);
        thr_ = std::max(0.f, argf(a, 1, 1.5f));
    }
    void message(int, const Message& m) override {
        if (m[0].is("stop")) { job_.cancel(); return; }
        if (m[0].is("width")) width_ = std::clamp((int)argf(m, 1, 32.f), 2, 512);
        else if (m[0].is("threshold")) thr_ = std::max(0.f, argf(m, 1, 1.5f));
        else if (m[0].is("gap")) gap_ = std::max(0.f, argf(m, 1, 2000.f));
        else if (m[0].is("run") && m.size() > 2) start(m[1].str(), m[2].str());
        else if (m[0].is("section") && m.size() > 1 && edges_.size() > 1) {
            long count = (long)edges_.size() - 1, k = ((long)argf(m, 1, 0.f) % count + count) % count;
            outlet(3, {Atom((float)(edges_[(size_t)k] * 1000.0)), Atom((float)(edges_[(size_t)k + 1] * 1000.0))});
        }
    }

private:
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        std::shared_ptr<Mat> S;
        double fps;
        if (!matrix(src, S, fps)) return;
        if (S->size() != (*S)[0].size()) { patch->print("novelty: " + src + " is not a square matrix (use similarity)"); return; }
        int width = width_, gap = std::max(1, (int)std::lround(gap_ / 1000.0 * fps));
        double thr = thr_;
        auto nov = std::make_shared<Vec>();
        auto bounds = std::make_shared<Vec>();
        run(
            [S, width, thr, gap, fps, nov, bounds](Job&) {
                *nov = mir::footeNovelty(*S, width);
                for (int f : mir::noveltyPeaks(*nov, thr, 2 * width + 1, gap)) bounds->push_back(f / fps);
            },
            [this, dst, nov, bounds, fps] {
                if (!nov->empty()) storeMatrix(patch, dst, Mat{*nov}, fps);
                edges_ = {0.0};
                for (double t : *bounds)
                    if (t > .5) edges_.push_back(t);
                edges_.push_back((double)nov->size() / fps);
                outletFloat(4, (float)(edges_.size() - 1));
                if (bounds->empty()) patch->print("novelty: no boundaries found");
                else outlet(2, msList(*bounds));
            });
    }
    Vec edges_;
    int width_;
    float thr_, gap_ = 2000.f;
};

// ---------------------------------------------------------------- curves
// curves [size] [hop]: curves of a sound over time, written to a buffer~
// (a frame every hop samples):
//   loudness sound curve    level in dB, as the ear weighs it roughly
//                           (below 150 Hz and above 8 kHz taken out)
//   register sound curves   3 channels: the spectral centroid in MIDI pitch,
//                           the spread around it in octaves, the lowest
//                           strong partial in MIDI pitch
//   polyphony sound curve   how many voices seem to sound (a rough estimate:
//                           the strong spectral peaks, three a voice)
// `smooth n` passes the curves through a moving median of n frames (odd;
// 1, no smoothing, by default): frame by frame they are jumpy.
class CurvesBox : public AnalysisBox {
public:
    explicit CurvesBox(const Message& a)
        : AnalysisBox(0, pow2In(argf(a, 0, 2048.f), 64, 65536), std::clamp((int)argf(a, 1, 512.f), 16, 65536)) {}
    void message(int, const Message& m) override {
        if (common(m)) return;
        if (m[0].is("smooth")) { smooth_ = std::clamp((int)argf(m, 1, 1.f), 1, 1001) | 1; return; }
        if (m.size() < 3) return;
        if (m[0].is("loudness")) start(0, m[1].str(), m[2].str());
        else if (m[0].is("register")) start(1, m[1].str(), m[2].str());
        else if (m[0].is("polyphony")) start(2, m[1].str(), m[2].str());
    }

private:
    void start(int what, const std::string& src, const std::string& dst) {
        if (busy()) return;
        std::shared_ptr<std::vector<float>> x;
        double sr;
        if (!sound(src, x, sr)) return;
        int n = size_, hop = hop_, smooth = smooth_;
        auto out = std::make_shared<Mat>();
        run(
            [x, n, hop, sr, what, smooth, out](Job& job) {
                if (what == 0) *out = Mat{mir::loudnessCurve(*x, sr, hop, n)};
                else {
                    Mat mag = mir::magSpectrogram(*x, n, hop, tickOf(job, 0, .9f));
                    if (mag.empty()) return;
                    *out = what == 1 ? mir::registerCurve(mag, sr, n) : Mat{mir::polyphonyCurve(mag)};
                }
                if (smooth > 1)
                    for (auto& row : *out) row = mir::medianFilter(row, smooth);
            },
            [this, dst, out, sr, hop] { if (!out->empty()) storeMatrix(patch, dst, *out, sr / hop); });
    }
    int smooth_ = 1;
};

// ---------------------------------------------------------------- live analysis
// The common shape of the live analyses: the signal goes into a ring; every
// hop samples, while audio runs, the last size samples are analysed (bang
// analyses now). As descriptors~.
class LiveBox : public Box {
public:
    LiveBox(int size, int hop, int outlets) : ring_(new std::atomic<float>[kRing]), hops_(std::clamp(hop, 16, 65536)) {
        n_ = std::clamp(size, 64, kRing / 2);
        setPorts({P::Signal}, std::vector<Port>((size_t)outlets, P::Control));
        for (int i = 0; i < kRing; ++i) ring_[i].store(0.f);
        spec_.init(n_);
    }
    void attached() override { tick(); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        size_t w = wpos_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) ring_[(w + i) & (kRing - 1)].store(x[i], std::memory_order_relaxed);
        wpos_.store(w + n, std::memory_order_release);
        hops_.advance(w + n, n);                         // hops counted in samples, not GUI time
    }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) analyse(wpos_.load(std::memory_order_acquire));
        else if (m[0].is("hop") && m.size() > 1) hops_.setHop(std::clamp((int)argf(m, 1, 512.f), 16, 65536));
        else option(m);
    }

protected:
    static constexpr int kRing = 65536;
    virtual void option(const Message&) {}
    virtual void frame(const std::vector<float>& x, const Vec& mag, double sr) = 0;
    double sampleRate() const { double sr = patch->root()->sampleRate.load(); return sr > 0 ? sr : 44100.0; }
    int n_;
    int hopSize() const { return hops_.hop(); }

private:
    // the frames that ended since the last look, each analysed where it ended
    void tick() {
        size_t end;
        while (hops_.next(end)) {
            size_t w = wpos_.load(std::memory_order_acquire);
            if (patch->root()->dspRunning && w - end + (size_t)n_ <= (size_t)kRing) analyse(end);
        }
        patch->schedule(this, hops_.interval(sampleRate()), [this] { tick(); });
    }
    void analyse(size_t end) {
        std::vector<float> x((size_t)n_);
        for (int i = 0; i < n_; ++i) x[(size_t)i] = ring_[(end - (size_t)n_ + (size_t)i) & (kRing - 1)].load(std::memory_order_relaxed);
        spec_.magnitudes(x.data(), mag_);
        frame(x, mag_, sampleRate());
    }
    std::unique_ptr<std::atomic<float>[]> ring_;
    std::atomic<size_t> wpos_{0};
    HopFrames hops_;
    mir::FrameSpectrum spec_;
    Vec mag_;
};

Message listOf(const Vec& v) {
    Message m;
    for (double x : v) m.push_back(Atom((float)x));
    return m;
}

// melbands~ [bands] [size] [hop]: the mel spectrum of the signal, a list of
// bands (40) values in dB, every hop samples (512) over size (2048).
// mfcc~ [coeffs] [bands] [size] [hop]: its MFCCs (13).
class MelbandsBox : public LiveBox {
public:
    MelbandsBox(const Message& a, bool mfcc)
        : LiveBox(pow2In(argf(a, mfcc ? 2 : 1, 2048.f), 64, 32768), (int)argf(a, mfcc ? 3 : 2, 512.f), 1), mfcc_(mfcc) {
        bands_ = std::clamp((int)argf(a, mfcc ? 1 : 0, 40.f), 2, 256);
        coeffs_ = mfcc ? std::clamp((int)argf(a, 0, 13.f), 1, bands_) : 0;
    }

private:
    void frame(const std::vector<float>&, const Vec& mag, double sr) override {
        if (!bank_.matches(sr, n_, bands_)) bank_.init(sr, n_, bands_);
        bank_.apply(mag, mel_);
        if (!mfcc_) { outlet(0, listOf(mel_)); return; }
        mir::dct(mel_, coeffs_, c_);
        outlet(0, listOf(c_));
    }
    bool mfcc_;
    int bands_, coeffs_;
    mir::MelBank bank_;
    Vec mel_, c_;
};

// chroma~ [size] [hop]: the pitch classes of the signal, a list of 12 (C
// first, a maximum of 1; zeros in silence), every hop samples (1024) over
// size (4096).
class ChromaBox : public LiveBox {
public:
    explicit ChromaBox(const Message& a) : LiveBox(pow2In(argf(a, 0, 4096.f), 256, 32768), (int)argf(a, 1, 1024.f), 1) {}

private:
    void frame(const std::vector<float>&, const Vec& mag, double sr) override {
        if (!pc_.matches(sr, n_)) pc_.init(sr, n_);
        mir::chroma(mag, pc_, c_);
        outlet(0, listOf(c_));
    }
    mir::ChromaMap pc_;
    Vec c_;
};

// onset~ [threshold] [size] [hop]: a bang when a note or a hit begins: the
// spectral flux peaks above threshold (2) times its median over the last
// 21 frames, at least `gap` ms (50) after the previous onset, and the level
// is above `gate` dB (-50). Right outlet: the flux, every frame. Size 1024,
// hop 256: the bang comes about 6 ms after the attack enters the window.
class OnsetBox : public LiveBox {
public:
    explicit OnsetBox(const Message& a) : LiveBox(pow2In(argf(a, 1, 1024.f), 64, 32768), (int)argf(a, 2, 256.f), 2) {
        thr_ = std::max(0.f, argf(a, 0, 2.f));
    }

private:
    void option(const Message& m) override {
        if (m[0].is("threshold")) thr_ = std::max(0.f, argf(m, 1, 2.f));
        else if (m[0].is("gap")) gap_ = std::max(0.f, argf(m, 1, 50.f));
        else if (m[0].is("gate")) gate_ = argf(m, 1, -50.f);
    }
    void frame(const std::vector<float>& x, const Vec& mag, double sr) override {
        double flux = mir::spectralFlux(mag, prev_);
        prev_ = mag;
        double e = 0;
        for (float v : x) e += (double)v * v;
        double db = mir::toDb(2 * e / std::max<size_t>(1, x.size()));
        hist_.push_back(flux);
        if (hist_.size() > 21) hist_.erase(hist_.begin());
        peak_ = std::max(flux, peak_ * .999);
        // the previous frame is an onset if it is a peak above the level
        size_t k = hist_.size();
        if (k >= 3) {
            double a = hist_[k - 3], b = hist_[k - 2], c = hist_[k - 1];
            Vec sorted(hist_.begin(), hist_.end());
            std::nth_element(sorted.begin(), sorted.begin() + (long)(sorted.size() / 2), sorted.end());
            double level = thr_ * (sorted[sorted.size() / 2] + .01 * peak_);
            double t = frames_ * (double)hopSize() / sr;
            if (b > a && b >= c && b > level && prevDb_ > gate_ && (t - last_) * 1000.0 >= gap_) {
                last_ = t;
                outletBang(0);
            }
        }
        prevDb_ = db;                                        // the gate looks at the frame of the peak
        ++frames_;
        outletFloat(1, (float)flux);
    }
    float thr_, gap_ = 50.f, gate_ = -50.f;
    Vec prev_, hist_;
    double peak_ = 0, last_ = -1e9, prevDb_ = -100;
    long frames_ = 0;
};

}  // namespace

void registerMirObjects(ClassTable& t) {
    using M = const Message&;
    t.insert(t.end(), {
        {"spectrogram", "Analysis", "magnitude spectrogram of a buffer~ (dB) into a buffer~: [size] [hop]", with<SpectrogramBox>()},
        {"melgram", "Analysis", "mel spectrogram of a buffer~ into a buffer~: [bands] [size] [hop]",
         [](M a, Patch*) { return std::make_unique<MelgramBox>(a, false); }},
        {"mfccgram", "Analysis", "MFCCs of a buffer~ into a buffer~: [coeffs] [bands] [size] [hop]",
         [](M a, Patch*) { return std::make_unique<MelgramBox>(a, true); }},
        {"chromagram", "Analysis", "chromagram (12 pitch classes) of a buffer~ into a buffer~: [size] [hop]", with<ChromagramBox>()},
        {"onsets", "Analysis", "onset times of a buffer~ (ms): [threshold] [size] [hop]", with<OnsetsBox>()},
        {"beats", "Analysis", "tempo and beat times of a buffer~, tempogram: [size] [hop]", with<BeatsBox>()},
        {"keyfind", "Analysis", "key of a buffer~ (Krumhansl-Kessler profiles): [size] [hop]", with<KeyfindBox>()},
        {"similarity", "Analysis", "self-similarity matrix of a feature matrix: [frames]", with<SimilarityBox>()},
        {"novelty", "Analysis", "novelty curve and section boundaries of a similarity matrix: [width] [threshold]", with<NoveltyBox>()},
        {"curves", "Analysis", "loudness, register and polyphony curves of a buffer~: [size] [hop]", with<CurvesBox>()},
        {"melbands~", "Analysis", "live mel spectrum (dB) as a list: [bands] [size] [hop]",
         [](M a, Patch*) { return std::make_unique<MelbandsBox>(a, false); }},
        {"mfcc~", "Analysis", "live MFCCs as a list: [coeffs] [bands] [size] [hop]",
         [](M a, Patch*) { return std::make_unique<MelbandsBox>(a, true); }},
        {"chroma~", "Analysis", "live pitch classes (12 values) as a list: [size] [hop]", with<ChromaBox>()},
        {"onset~", "Analysis", "bang at every onset of a signal: [threshold] [size] [hop]", with<OnsetBox>()},
    });
}

}  // namespace moritz
