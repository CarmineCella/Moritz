// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_offline.cpp — offline processing as background jobs: moritz~
// (render another patch into a buffer~), nmf (non-negative matrix
// factorisation: separate a sound into parts) and hpss (harmonic/percussive
// separation). The algorithms of nmf and hpss are Musil's.
//
// Each object takes copies of its input on the GUI thread, works on a
// worker thread (jobs.h) and writes its result into buffer~s on the GUI
// thread. Outlets: progress (0..1), then a bang when the result is ready;
// `stop` cancels.

#include "jobs.h"
#include "objects.h"

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
        dsp::FFT fft;
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
        dsp::FFT fft;
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

std::vector<float> channelOf(const SampleBuffer& b, int c) {
    std::vector<float> x((size_t)b.frames);
    for (long f = 0; f < b.frames; ++f) x[(size_t)f] = b.at(f, c);
    return x;
}
std::vector<float> monoOf(const SampleBuffer& b) {
    std::vector<float> x((size_t)b.frames, 0.f);
    for (long f = 0; f < b.frames; ++f) {
        float s = 0.f;
        for (int c = 0; c < b.channels; ++c) s += b.at(f, c);
        x[(size_t)f] = s / b.channels;
    }
    return x;
}

// Writes channels into the buffer~ name (GUI thread); false if there is none.
bool storeBuffer(Patch* patch, const std::string& name, const std::vector<std::vector<float>>& chans, double sr) {
    BufferBox* b = findBuffer(patch, name);
    if (!b) { patch->print("no buffer~ " + name); return false; }
    SampleBuffer out;
    out.channels = std::max(1, (int)chans.size());
    out.frames = chans.empty() ? 0 : (long)chans[0].size();
    out.sr = sr;
    out.data.assign((size_t)out.frames * out.channels, 0.f);
    for (int c = 0; c < (int)chans.size(); ++c)
        for (long f = 0; f < out.frames && f < (long)chans[(size_t)c].size(); ++f)
            out.data[(size_t)f * out.channels + c] = chans[(size_t)c][(size_t)f];
    {
        std::lock_guard<std::mutex> lk(patch->mutex());
        b->buf = std::move(out);
        ++b->version;
    }
    patch->rebuild();
    return true;
}

// The common shape: outlets progress and done, `stop`, a job.
class OfflineBox : public Box {
public:
    OfflineBox() {
        job_.onProgress = [this](float p) { outletFloat(0, p); };
    }

protected:
    void run(std::function<void(Job&)> work, std::function<void()> done) {
        outletFloat(0, 0.f);
        job_.start(std::move(work), [this, done = std::move(done)] {
            done();
            outletFloat(0, 1.f);
            outletBang(1);
            finished();
        });
    }
    virtual void finished() {}      // after the outlets: moritz~ starts its next render
    bool busy() {
        if (job_.running()) { patch->print(cls + ": still working (stop cancels)"); return true; }
        return false;
    }
    Job job_;
};

// ---------------------------------------------------------------- moritz~
// moritz~ [patch] [channels]: renders another patch offline, faster than real
// time, into a buffer~. `open patch.moritz` chooses it (relative to this
// patch); `send name value...` sets its receive name before every render
// (kept until `clear`); `input buffer` feeds its adc~; `render buffer
// seconds` runs it (from scratch each time: loaded, the sends applied,
// loadbang) and writes its dac~ (channels, 2 by default) into the buffer~.
// The rendered patch has its own names: its send/receive and buffer~ are
// not those of this patch. Its print output appears when the render ends.
// A render asked for while another runs waits its turn, with the sends and
// input of the moment it was asked for; `stop` cancels them all.
class MoritzBox : public OfflineBox {
public:
    ~MoritzBox() override { job_.cancel(); }   // before inner_ goes: the worker may be using it
    explicit MoritzBox(const Message& a) {
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
        if (file_.empty()) { patch->print("moritz~: no patch; send \"open name.moritz\""); return; }
        if (depth() > 8) { patch->print("moritz~: rendering nested more than 8 deep"); return; }
        std::string path = patch->resolvePath(file_), src, err;
        if (!readFile(path, src)) { patch->print("moritz~: can't read " + path); return; }
        double sr = patch->root()->sampleRate.load();
        // the patch is made and destroyed on the GUI thread; only processing runs on the worker
        inner_ = std::make_unique<Patch>();
        Patch& p = *inner_;
        p.dir = dirOf(path);
        p.sampleRate = sr;
        p.dspRunning = true;
        p.ioBufferFrames = dsp::kBlock;
        p.renderDepth = depth() + 1;
        auto prints = std::make_shared<std::pair<std::mutex, std::vector<std::string>>>();
        p.onPrint = [prints](const std::string& s) {
            std::lock_guard<std::mutex> lk(prints->first);
            prints->second.push_back(s);
        };
        auto frame = std::make_shared<long>(0);
        p.clock = [frame, sr] { return *frame / sr; };
        if (!p.load(src, err)) { patch->print("moritz~: " + path + ": " + err); inner_.reset(); return; }
        if (!err.empty()) patch->print("moritz~: " + err);
        for (auto& [name, msg] : sends) p.sendTo(name, msg);
        p.loadbangAll();
        // the input, interleaved
        std::vector<float> in;
        int inCh = 0;
        if (!input.empty()) {
            if (BufferBox* b = findBuffer(patch, input)) { in = b->buf.data; inCh = b->buf.channels; }
            else patch->print("moritz~: no buffer~ " + input);
        }
        long total = (long)(seconds * sr);
        int ch = ch_;
        auto result = std::make_shared<std::vector<std::vector<float>>>((size_t)ch, std::vector<float>((size_t)total, 0.f));
        Patch* ip = inner_.get();
        run(
            [ip, frame, total, ch, in, inCh, result](Job& job) {
                const int blk = dsp::kBlock;
                std::vector<float> out((size_t)blk * ch), inBlock((size_t)blk * std::max(1, inCh), 0.f);
                for (long f = 0; f < total; f += blk) {
                    if (job.cancelled()) return;
                    *frame = f;
                    ip->runDue((double)f / ip->sampleRate.load());
                    int n = (int)std::min<long>(blk, total - f);
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
// W H), so the components add up to the sound. `run source dest` writes one
// component per channel of the buffer~ dest (the source is mixed to mono).
// `components k`, `iterations n`, `size n` (FFT, 2048), `seed n`.
class NmfBox : public OfflineBox {
public:
    explicit NmfBox(const Message& a) {
        k_ = std::clamp((int)argf(a, 0, 3.f), 1, 32);
        iters_ = std::clamp((int)argf(a, 1, 100.f), 1, 10000);
        setPorts({P::Control}, {P::Control, P::Control});
    }
    void message(int, const Message& m) override {
        if (m[0].is("stop")) { job_.cancel(); return; }
        if (m[0].is("components")) k_ = std::clamp((int)argf(m, 1, 3.f), 1, 32);
        else if (m[0].is("iterations")) iters_ = std::clamp((int)argf(m, 1, 100.f), 1, 10000);
        else if (m[0].is("size")) size_ = std::clamp(nextPow2((int)argf(m, 1, 2048.f)), 256, 16384);
        else if (m[0].is("seed")) seed_ = (unsigned)argf(m, 1, 1.f);
        else if (m[0].is("run") && m.size() > 2) start(m[1].str(), m[2].str());
    }

private:
    static int nextPow2(int n) { int p = 1; while (p < n) p <<= 1; return p; }
    void start(const std::string& src, const std::string& dst) {
        if (busy()) return;
        BufferBox* b = findBuffer(patch, src);
        if (!b || b->buf.frames <= 0) { patch->print("nmf: no sound in buffer~ " + src); return; }
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
                std::vector<float> V((size_t)F * T);                 // V[b * T + t]
                for (int t = 0; t < T; ++t)
                    for (int b = 0; b < F; ++b) V[(size_t)b * T + t] = s.mag(t, b);
                std::mt19937 rng(seed);
                std::uniform_real_distribution<float> u(.5f, 1.f);
                std::vector<float> W((size_t)F * K), H((size_t)K * T), WH((size_t)F * T), Q((size_t)F * T);
                for (auto& w : W) w = u(rng);
                for (auto& h : H) h = u(rng);
                const float eps = 1e-9f;
                auto product = [&] {
                    for (int b = 0; b < F; ++b)
                        for (int t = 0; t < T; ++t) {
                            float v = 0.f;
                            for (int k = 0; k < K; ++k) v += W[(size_t)b * K + k] * H[(size_t)k * T + t];
                            WH[(size_t)b * T + t] = v + eps;
                        }
                };
                for (int it = 0; it < iters; ++it) {
                    if (job.cancelled()) return;
                    // H <- H * (W^T (V / WH)) / (W^T 1)
                    product();
                    for (size_t i = 0; i < Q.size(); ++i) Q[i] = V[i] / WH[i];
                    for (int k = 0; k < K; ++k) {
                        float colsum = 0.f;
                        for (int b = 0; b < F; ++b) colsum += W[(size_t)b * K + k];
                        for (int t = 0; t < T; ++t) {
                            float num = 0.f;
                            for (int b = 0; b < F; ++b) num += W[(size_t)b * K + k] * Q[(size_t)b * T + t];
                            H[(size_t)k * T + t] *= num / (colsum + eps);
                        }
                    }
                    // W <- W * ((V / WH) H^T) / (1 H^T)
                    product();
                    for (size_t i = 0; i < Q.size(); ++i) Q[i] = V[i] / WH[i];
                    for (int k = 0; k < K; ++k) {
                        float rowsum = 0.f;
                        for (int t = 0; t < T; ++t) rowsum += H[(size_t)k * T + t];
                        for (int b = 0; b < F; ++b) {
                            float num = 0.f;
                            for (int t = 0; t < T; ++t) num += Q[(size_t)b * T + t] * H[(size_t)k * T + t];
                            W[(size_t)b * K + k] *= num / (rowsum + eps);
                        }
                    }
                    // parts of unit sum, the scale moved into H (as Musil)
                    for (int k = 0; k < K; ++k) {
                        float sum = 0.f;
                        for (int b = 0; b < F; ++b) sum += W[(size_t)b * K + k];
                        sum = std::max(sum, eps);
                        for (int b = 0; b < F; ++b) W[(size_t)b * K + k] /= sum;
                        for (int t = 0; t < T; ++t) H[(size_t)k * T + t] *= sum;
                    }
                    job.progress(.1f + .8f * (it + 1) / iters);
                }
                product();
                for (int k = 0; k < K; ++k) {
                    if (job.cancelled()) return;
                    result->push_back(s.synthesise([&](int t, int b) {
                        return W[(size_t)b * K + k] * H[(size_t)k * T + t] / WH[(size_t)b * T + t];
                    }));
                    job.progress(.9f + .1f * (k + 1) / K);
                }
            },
            [this, dst, result, sr] { storeBuffer(patch, dst, *result, sr); });
    }
    int k_, iters_, size_ = 2048;
    unsigned seed_ = 1;
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
                    float from = (float)c / C, to = (float)(c + 1) / C;
                    Stft s;
                    s.analyse((*chans)[(size_t)c], n, n / 4, &job, from, from + .3f * (to - from));
                    int F = s.bins, T = s.frames, half = kernel / 2;
                    std::vector<float> M((size_t)T * F), Hm((size_t)T * F), Pm((size_t)T * F), buf;
                    for (int t = 0; t < T; ++t)
                        for (int q = 0; q < F; ++q) M[(size_t)t * F + q] = s.mag(t, q);
                    auto median = [&buf]() {
                        size_t m = buf.size() / 2;
                        std::nth_element(buf.begin(), buf.begin() + (long)m, buf.end());
                        return buf[m];
                    };
                    for (int q = 0; q < F; ++q) {
                        if (job.cancelled()) return;
                        for (int t = 0; t < T; ++t) {
                            buf.clear();
                            for (int u = t - half; u <= t + half; ++u) buf.push_back(u >= 0 && u < T ? M[(size_t)u * F + q] : 0.f);
                            Hm[(size_t)t * F + q] = median();
                        }
                    }
                    job.progress(from + .6f * (to - from));
                    for (int t = 0; t < T; ++t) {
                        if (job.cancelled()) return;
                        for (int q = 0; q < F; ++q) {
                            buf.clear();
                            for (int v = q - half; v <= q + half; ++v) buf.push_back(v >= 0 && v < F ? M[(size_t)t * F + v] : 0.f);
                            Pm[(size_t)t * F + q] = median();
                        }
                    }
                    job.progress(from + .8f * (to - from));
                    auto mask = [&](bool harmonic) {
                        return [&, harmonic](int t, int q) {
                            float h2 = Hm[(size_t)t * F + q] * Hm[(size_t)t * F + q], p2 = Pm[(size_t)t * F + q] * Pm[(size_t)t * F + q];
                            float d = h2 + p2 + 1e-12f;
                            return (harmonic ? h2 : p2) / d;
                        };
                    };
                    harm->push_back(s.synthesise(mask(true)));
                    perc->push_back(s.synthesise(mask(false)));
                    job.progress(to);
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
        {"moritz~", "Samples", "render a patch offline into a buffer~: [patch] [channels]", with<MoritzBox>()},
        {"nmf", "Spectral", "separate a buffer~ into parts (NMF): [components] [iterations]", with<NmfBox>()},
        {"hpss", "Spectral", "harmonic/percussive separation of a buffer~: [kernel] [size]", with<HpssBox>()},
    });
}

}  // namespace moritz
