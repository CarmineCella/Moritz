// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_offline.cpp — offline processing as background jobs: render
// (render another patch into a buffer~), nmf (non-negative matrix
// factorisation: separate a sound into parts) and hpss (harmonic/percussive
// separation). The algorithms of nmf and hpss are Musil's.
//
// Each object takes copies of its input on the GUI thread, works on a
// worker thread (jobs.h) and writes its result into buffer~s on the GUI
// thread. Outlets: progress (0..1), then a bang when the result is ready;
// `stop` cancels.

#include "offline.h"
#include "signals/fft.h"   // Musil's FFT
#include "scientific/nmf.h"    // Musil's nmf
#include "signals/hpss.h"      // Musil's hpss

#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>

namespace moritz {
namespace {

// ---------------------------------------------------------------- STFT
// Hann-windowed short-time spectra of a mono signal, padded by n on both
// sides so its edges are covered by full overlaps, and their inverse with
// the exact overlap gain (the summed squared window), after Musil's hpss.
struct Stft {
    int n = 2048, hop = 512, frames = 0, bins = 0;
    std::vector<std::vector<float>> re, im;   // frames x n
    std::vector<float> w;
    long length = 0;                           // of the original signal

    void analyse(const std::vector<float>& x, int size, int hopSize, Job* job = nullptr, float from = 0, float to = 1) {
        n = size;
        hop = hopSize;
        length = (long)x.size();
        bins = n / 2 + 1;
        w.resize((size_t)n);
        for (int k = 0; k < n; ++k) w[(size_t)k] = (float)(.5 - .5 * std::cos(dsp::kTwoPi * k / n));
        long padded = length + 2L * n;
        frames = (int)((padded - n) / hop + 1);
        re.assign((size_t)frames, std::vector<float>((size_t)n));
        im.assign((size_t)frames, std::vector<float>((size_t)n));
        musil::dsp::FFT<float> fft;
        fft.init(n);
        for (int f = 0; f < frames; ++f) {
            for (int k = 0; k < n; ++k) {
                long i = (long)f * hop + k - n;
                re[(size_t)f][(size_t)k] = (i >= 0 && i < length) ? x[(size_t)i] * w[(size_t)k] : 0.f;
                im[(size_t)f][(size_t)k] = 0.f;
            }
            fft.forward(re[(size_t)f].data(), im[(size_t)f].data());
            if (job && f % 64 == 0) job->progress(from + (to - from) * f / frames);
        }
    }
    float mag(int f, int b) const { return std::hypot(re[(size_t)f][(size_t)b], im[(size_t)f][(size_t)b]); }
    // inverse of the spectra scaled per bin by mask(f, b) (b in 0..bins-1)
    std::vector<float> synthesise(const std::function<float(int, int)>& mask) const {
        long outLen = (long)(frames - 1) * hop + n;
        std::vector<double> y((size_t)outLen, 0.0), wsum((size_t)outLen, 0.0);
        std::vector<float> r((size_t)n), i((size_t)n);
        musil::dsp::FFT<float> fft;
        fft.init(n);
        for (int f = 0; f < frames; ++f) {
            for (int k = 0; k < n; ++k) {
                int b = k <= n / 2 ? k : n - k;
                float m = mask(f, b);
                r[(size_t)k] = re[(size_t)f][(size_t)k] * m;
                i[(size_t)k] = im[(size_t)f][(size_t)k] * m;
            }
            fft.inverse(r.data(), i.data());
            for (int k = 0; k < n; ++k) {
                size_t t = (size_t)f * hop + k;
                y[t] += r[(size_t)k] / n * w[(size_t)k];
                wsum[t] += (double)w[(size_t)k] * w[(size_t)k];
            }
        }
        std::vector<float> out((size_t)length, 0.f);
        for (long t = 0; t < length; ++t) {
            size_t s = (size_t)(t + n);
            out[(size_t)t] = wsum[s] > 1e-9 ? (float)(y[s] / wsum[s]) : 0.f;
        }
        return out;
    }
};

// ---------------------------------------------------------------- render
// render [patch] [channels]: renders another patch offline, faster than real
// time, into a buffer~. `open patch.moritz` chooses it (relative to this
// patch); `send name value...` sets its receive name before every render
// (kept until `clear`); `input buffer` feeds its adc~; `render buffer
// seconds` runs it (from scratch each time: loaded, the sends applied,
// loadbang) and writes its dac~ (channels, 2 by default) into the buffer~.
// The rendered patch has its own names: its send/receive and buffer~ are
// not those of this patch. Its print output appears when the render ends.
// A render asked for while another runs waits its turn, with the sends and
// input of the moment it was asked for; `stop` cancels them all.
class RenderBox : public OfflineBox {
public:
    ~RenderBox() override { job_.cancel(); }   // before inner_ goes: the worker may be using it
    explicit RenderBox(const Message& a) {
        size_t k = 0;
        if (k < a.size() && a[k].isSymbol()) file_ = a[k++].s;
        ch_ = std::clamp((int)argf(a, k, 2.f), 1, 16);
        setPorts({P::Control}, {P::Control, P::Control});
    }
    void message(int, const Message& m) override {
        if (m[0].is("stop")) { job_.cancel(); inner_.reset(); queue_.clear(); return; }
        if (m[0].is("open") && m.size() > 1) { file_ = m[1].str(); return; }
        if (m[0].is("send") && m.size() > 2) { sends_[m[1].str()] = tailOf(m, 2); return; }
        if (m[0].is("clear")) { sends_.clear(); input_.clear(); return; }
        if (m[0].is("input") && m.size() > 1) { input_ = m[1].str(); return; }
        if (m[0].is("render") && m.size() > 2) {
            queue_.push_back({m[1].str(), std::max(0.f, argf(m, 2, 1.f)), sends_, input_});
            if (!job_.running()) next();
        }
    }

private:
    struct Request {
        std::string out;
        float seconds;
        std::map<std::string, Message> sends;
        std::string input;
    };
    void finished() override { next(); }
    void next() {
        while (!queue_.empty() && !job_.running()) {
            Request r = queue_.front();
            queue_.erase(queue_.begin());
            render(r);
        }
    }
    void render(const Request& req) {
        const std::string& out = req.out;
        float seconds = req.seconds;
        const auto& sends = req.sends;
        const std::string& input = req.input;
        if (file_.empty()) { patch->print("render: no patch; send \"open name.moritz\""); return; }
        if (depth() > 8) { patch->print("render: rendering nested more than 8 deep"); return; }
        std::string path = patch->resolvePath(file_), src, err;
        if (!readFile(path, src)) { patch->print("render: can't read " + path); return; }
        double sr = patch->root()->sampleRate.load();
        // the patch is made and destroyed on the GUI thread; only processing runs on the worker
        inner_ = std::make_unique<Patch>();
        Patch& p = *inner_;
        p.dir = dirOf(path);
        p.sampleRate = sr;
        p.dspRunning = true;
        p.offline = true;                     // the clock is the sample count
        p.ioBufferFrames = dsp::kBlock;
        p.renderDepth = depth() + 1;
        auto prints = std::make_shared<std::pair<std::mutex, std::vector<std::string>>>();
        p.onPrint = [prints](const std::string& s) {
            std::lock_guard<std::mutex> lk(prints->first);
            prints->second.push_back(s);
        };
        auto frame = std::make_shared<long>(0);
        p.clock = [frame, sr] { return *frame / sr; };
        if (!p.load(src, err)) { patch->print("render: " + path + ": " + err); inner_.reset(); return; }
        if (!err.empty()) patch->print("render: " + err);
        for (auto& [name, msg] : sends) p.sendTo(name, msg);
        p.loadbangAll();
        // the input, interleaved
        std::vector<float> in;
        int inCh = 0;
        if (!input.empty()) {
            if (BufferBox* b = findBuffer(patch, input)) { in = b->buf.data; inCh = b->buf.channels; }
            else patch->print("render: no buffer~ " + input);
        }
        long total = (long)(seconds * sr);
        int ch = ch_;
        auto result = std::make_shared<std::vector<std::vector<float>>>((size_t)ch, std::vector<float>((size_t)total, 0.f));
        Patch* ip = inner_.get();
        run(
            [ip, frame, total, ch, in, inCh, result](Job& job) {
                dsp::FlushDenormalsScope ftz;         // as the audio callback: no slow denormals
                // the worker dispatches the patch's messages while it renders: what other
                // threads send it (a code box's output) waits for the next block
                struct Owner {
                    Patch* p;
                    explicit Owner(Patch* q) : p(q) { if (!jobsSynchronous) p->setDispatchThread(std::this_thread::get_id()); }
                    ~Owner() { p->setDispatchThread(std::thread::id()); }
                } owner(ip);
                const int blk = dsp::kBlock;
                std::vector<float> out((size_t)blk * ch), inBlock((size_t)blk * std::max(1, inCh), 0.f);
                for (long f = 0; f < total; f += blk) {
                    if (job.cancelled()) return;
                    *frame = f;
                    ip->drainForeign();
                    int n = (int)std::min<long>(blk, total - f);
                    ip->runDue((double)(f + n) / ip->sampleRate.load());   // timers due in the block
                    for (int i = 0; i < n * inCh; ++i) {
                        size_t s = (size_t)f * inCh + i;
                        inBlock[(size_t)i] = s < in.size() ? in[s] : 0.f;
                    }
                    ip->process(inCh ? inBlock.data() : nullptr, inCh, out.data(), ch, n);
                    for (int c = 0; c < ch; ++c)
                        for (int i = 0; i < n; ++i) (*result)[(size_t)c][(size_t)(f + i)] = out[(size_t)i * ch + c];
                    if ((f / blk) % 256 == 0) job.progress((float)f / total);
                }
            },
            [this, out, result, prints, sr] {
                inner_.reset();
                for (auto& s : prints->second) patch->print(s);
                storeBuffer(patch, out, *result, sr);
            });
    }
    int depth() const { return patch ? patch->root()->renderDepth : 0; }

    std::string file_, input_;
    int ch_;
    std::map<std::string, Message> sends_;
    std::unique_ptr<Patch> inner_;
    std::vector<Request> queue_;
};

// ---------------------------------------------------------------- nmf
// nmf [components] [iterations]: separates a sound into parts that repeat
// (notes, sources) by non-negative matrix factorisation of its magnitude
// spectrogram, V ~ W H: W holds a spectrum per component, H when and how
// loud it sounds. KL multiplicative updates, as Musil's nmf. Each component
// is resynthesised with the original phases through a soft mask (W_k H_k /
// W H), so the components add up to the sound.
//
// Unsupervised: `run source dest` writes one component per channel of the
// buffer~ dest (the source is mixed to mono); which part is which sound, the
// factorisation decides.
// Supervised, as Musil's nmf-learn-parts and nmf-separate-with: `learn
// example` learns `components` parts from a recording of one source alone
// (call it once per source); `separate mix dest` then explains the mix with
// those parts only (just H is learned) and writes one source per channel of
// dest, in the order they were learned. `forget` clears what was learned.
// `components k`, `iterations n`, `size n` (FFT, 2048), `seed n`.

// V (F x T, V[b * T + t]) ~ W (F x K) H (K x T), KL updates; W stays as given
// when fixW. Returns false if cancelled.
bool nmfFactor(const std::vector<float>& V, int F, int T, int K, int iters, std::vector<float>& W,
               std::vector<float>& H, bool fixW, Job& job, float p0, float p1) {
    return nmf_kl(V, F, T, K, iters, W, H, fixW, [&](int it) {       // Musil's nmf (scientific/nmf.h)
        job.progress(p0 + (p1 - p0) * (it + 1) / iters);
        return !job.cancelled();
    });
}

class NmfBox : public OfflineBox {
public:
    explicit NmfBox(const Message& a) {
        k_ = std::clamp((int)argf(a, 0, 3.f), 1, 32);
        iters_ = std::clamp((int)argf(a, 1, 100.f), 1, 10000);
        setPorts({P::Control}, {P::Control, P::Control});
    }
    void message(int, const Message& m) override {
        if (m[0].is("stop")) { job_.cancel(); queue_.clear(); return; }
        // while a job runs, everything waits its turn, in order (so one message
        // box can say "learn a, learn b, separate mix out")
        if (job_.running() || !queue_.empty()) {
            if (job_.running() || !dispatching_) { queue_.push_back(m); return; }
        }
        if (m[0].is("components")) k_ = std::clamp((int)argf(m, 1, 3.f), 1, 32);
        else if (m[0].is("iterations")) iters_ = std::clamp((int)argf(m, 1, 100.f), 1, 10000);
        else if (m[0].is("size")) {
            int n = std::clamp(nextPow2((int)argf(m, 1, 2048.f)), 256, 16384);
            if (n != size_) { size_ = n; forget(); }
        }
        else if (m[0].is("seed")) seed_ = (unsigned)argf(m, 1, 1.f);
        else if (m[0].is("run") && m.size() > 2) start(m[1].str(), m[2].str());
        else if (m[0].is("learn") && m.size() > 1) learn(m[1].str());
        else if (m[0].is("separate") && m.size() > 2) separate(m[1].str(), m[2].str());
        else if (m[0].is("forget")) forget();
    }

private:
    void finished() override {                // the next waiting messages, up to the next job
        dispatching_ = true;
        while (!queue_.empty() && !job_.running()) {
            Message m = queue_.front();
            queue_.erase(queue_.begin());
            message(0, m);
        }
        dispatching_ = false;
    }
    std::vector<Message> queue_;
    bool dispatching_ = false;
    static int nextPow2(int n) { int p = 1; while (p < n) p <<= 1; return p; }
    void forget() {                           // a running job keeps the old list
        parts_ = std::make_shared<std::vector<std::vector<float>>>();
        groups_.clear();
    }
    BufferBox* source(const std::string& name) {
        BufferBox* b = findBuffer(patch, name);
        if (!b || b->buf.frames <= 0) { patch->print("nmf: no sound in buffer~ " + name); return nullptr; }
        return b;
    }
    // the magnitude spectrogram V (bins x frames)
    static std::vector<float> spectrogram(const Stft& s) {
        int F = s.bins, T = s.frames;
        std::vector<float> V((size_t)F * T);
        for (int t = 0; t < T; ++t)
            for (int b = 0; b < F; ++b) V[(size_t)b * T + t] = s.mag(t, b);
        return V;
    }
    static void randomise(std::vector<float>& v, unsigned seed) { nmf_init(v, seed); }
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        BufferBox* b = source(src);
        if (!b) return;
        auto x = std::make_shared<std::vector<float>>(monoOf(b->buf));
        double sr = b->buf.sr;
        int K = k_, iters = iters_, n = size_;
        unsigned seed = seed_;
        auto result = std::make_shared<std::vector<std::vector<float>>>();
        run(
            [x, K, iters, n, seed, result](Job& job) {
                Stft s;
                s.analyse(*x, n, n / 4, &job, 0.f, .1f);
                int F = s.bins, T = s.frames;
                std::vector<float> V = spectrogram(s), W((size_t)F * K), H((size_t)K * T);
                randomise(W, seed);
                randomise(H, seed + 1);
                if (!nmfFactor(V, F, T, K, iters, W, H, false, job, .1f, .9f)) return;
                synthesise(s, W, H, F, T, K, std::vector<std::vector<int>>(), *result, job);
            },
            [this, dst, result, sr] { storeBuffer(patch, dst, *result, sr); });
    }
    void learn(const std::string& src) {
        if (busy()) return;
        BufferBox* b = source(src);
        if (!b) return;
        auto x = std::make_shared<std::vector<float>>(monoOf(b->buf));
        int K = k_, iters = iters_, n = size_;
        unsigned seed = seed_;
        auto learned = std::make_shared<std::vector<std::vector<float>>>();
        run(
            [x, K, iters, n, seed, learned](Job& job) {
                Stft s;
                s.analyse(*x, n, n / 4, &job, 0.f, .1f);
                int F = s.bins, T = s.frames;
                std::vector<float> V = spectrogram(s), W((size_t)F * K), H((size_t)K * T);
                randomise(W, seed);
                randomise(H, seed + 1);
                if (!nmfFactor(V, F, T, K, iters, W, H, false, job, .1f, 1.f)) return;
                for (int k = 0; k < K; ++k) {
                    std::vector<float> col((size_t)F);
                    for (int b = 0; b < F; ++b) col[(size_t)b] = W[(size_t)b * K + k];
                    learned->push_back(std::move(col));
                }
            },
            [this, learned, src] {
                std::vector<int> group;
                for (auto& col : *learned) {
                    group.push_back((int)parts_->size());
                    parts_->push_back(col);
                }
                groups_.push_back(group);
                patch->print("nmf: learned " + std::to_string(group.size()) + " parts of " + src + " (source " +
                             std::to_string(groups_.size()) + ")");
            });
    }
    void separate(const std::string& src, const std::string& dst) {
        if (busy()) return;
        if (groups_.empty()) { patch->print("nmf: nothing learned yet (learn example first)"); return; }
        BufferBox* b = source(src);
        if (!b) return;
        auto x = std::make_shared<std::vector<float>>(monoOf(b->buf));
        double sr = b->buf.sr;
        int iters = iters_, n = size_;
        unsigned seed = seed_;
        auto parts = parts_;                     // shared: the job reads them while the GUI may learn more
        auto groups = groups_;
        auto result = std::make_shared<std::vector<std::vector<float>>>();
        run(
            [x, iters, n, seed, parts, groups, result](Job& job) {
                Stft s;
                s.analyse(*x, n, n / 4, &job, 0.f, .1f);
                int F = s.bins, T = s.frames, K = (int)parts->size();
                std::vector<float> V = spectrogram(s), W((size_t)F * K), H((size_t)K * T);
                for (int k = 0; k < K; ++k)
                    for (int b = 0; b < F; ++b) W[(size_t)b * K + k] = (*parts)[(size_t)k][(size_t)b];
                randomise(H, seed + 1);
                if (!nmfFactor(V, F, T, K, iters, W, H, true, job, .1f, .9f)) return;
                synthesise(s, W, H, F, T, K, groups, *result, job);
            },
            [this, dst, result, sr] { storeBuffer(patch, dst, *result, sr); });
    }
    // one sound per group of parts (each part alone when groups is empty),
    // through soft masks of W H
    static void synthesise(Stft& s, const std::vector<float>& W, const std::vector<float>& H, int F, int T, int K,
                           std::vector<std::vector<int>> groups, std::vector<std::vector<float>>& out, Job& job) {
        if (groups.empty())
            for (int k = 0; k < K; ++k) groups.push_back({k});
        std::vector<float> WH((size_t)F * T);
        for (int b = 0; b < F; ++b)
            for (int t = 0; t < T; ++t) {
                float v = 0.f;
                for (int k = 0; k < K; ++k) v += W[(size_t)b * K + k] * H[(size_t)k * T + t];
                WH[(size_t)b * T + t] = v + 1e-9f;
            }
        for (size_t g = 0; g < groups.size(); ++g) {
            if (job.cancelled()) return;
            const auto& grp = groups[g];
            out.push_back(s.synthesise([&](int t, int b) {
                float v = 0.f;
                for (int k : grp) v += W[(size_t)b * K + k] * H[(size_t)k * T + t];
                return v / WH[(size_t)b * T + t];
            }));
            job.progress(.9f + .1f * (float)(g + 1) / (float)groups.size());
        }
    }
    int k_, iters_, size_ = 2048;
    unsigned seed_ = 1;
    std::shared_ptr<std::vector<std::vector<float>>> parts_ = std::make_shared<std::vector<std::vector<float>>>();
    std::vector<std::vector<int>> groups_;
};

// ---------------------------------------------------------------- hpss
// hpss [kernel] [size]: harmonic/percussive separation (Musil's hpss). The
// magnitude spectrogram is median-filtered along time (sustained, harmonic
// sounds are horizontal lines) and along frequency (attacks, percussive
// sounds are vertical ones); soft masks from the two weight the original
// spectra. `run source harmonic percussive` writes the two buffer~s (each
// channel separately); they add up to the source. kernel: the filter length
// in frames and bins (odd, 17 by default); size: the FFT (2048).
class HpssBox : public OfflineBox {
public:
    explicit HpssBox(const Message& a) {
        kernel_ = std::clamp((int)argf(a, 0, 17.f), 3, 101) | 1;
        size_ = std::clamp((int)argf(a, 1, 2048.f), 256, 16384);
        setPorts({P::Control}, {P::Control, P::Control});
    }
    void message(int, const Message& m) override {
        if (m[0].is("stop")) { job_.cancel(); return; }
        if (m[0].is("kernel")) kernel_ = std::clamp((int)argf(m, 1, 17.f), 3, 101) | 1;
        else if (m[0].is("size")) size_ = std::clamp((int)argf(m, 1, 2048.f), 256, 16384);
        else if (m[0].is("run") && m.size() > 3) start(m[1].str(), m[2].str(), m[3].str());
    }

private:
    void start(const std::string& src, const std::string& hname, const std::string& pname) {
        if (busy()) return;
        BufferBox* b = findBuffer(patch, src);
        if (!b || b->buf.frames <= 0) { patch->print("hpss: no sound in buffer~ " + src); return; }
        int n = 1;
        while (n < size_) n <<= 1;
        auto chans = std::make_shared<std::vector<std::vector<float>>>();
        for (int c = 0; c < b->buf.channels; ++c) chans->push_back(channelOf(b->buf, c));
        double sr = b->buf.sr;
        int kernel = kernel_;
        auto harm = std::make_shared<std::vector<std::vector<float>>>(), perc = std::make_shared<std::vector<std::vector<float>>>();
        run(
            [chans, n, kernel, harm, perc](Job& job) {
                int C = (int)chans->size();
                for (int c = 0; c < C; ++c) {
                    const std::vector<float>& x = (*chans)[(size_t)c];
                    std::vector<float> h, p;
                    int calls = 0;
                    // Musil's hpss (signals/hpss.h): progress and Stop at each of its pauses
                    bool done = musil::dsp::hpss<float>(x.data(), x.size(), (size_t)n, (size_t)n / 4, kernel, h, p, [&] {
                        job.progress(((float)c + std::min(0.9f, 0.1f * (float)++calls)) / (float)C);
                        return !job.cancelled();
                    });
                    if (!done) return;
                    harm->push_back(std::move(h));
                    perc->push_back(std::move(p));
                    job.progress((float)(c + 1) / (float)C);
                }
            },
            [this, hname, pname, harm, perc, sr] {
                storeBuffer(patch, hname, *harm, sr);
                storeBuffer(patch, pname, *perc, sr);
            });
    }
    int kernel_, size_;
};

}  // namespace

void registerOfflineObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"render", "Samples", "render a patch offline into a buffer~: [patch] [channels]", with<RenderBox>()},
        {"nmf", "Spectral", "separate a buffer~ into parts (NMF): [components] [iterations]", with<NmfBox>()},
        {"hpss", "Spectral", "harmonic/percussive separation of a buffer~: [kernel] [size]", with<HpssBox>()},
    });
}

}  // namespace moritz
