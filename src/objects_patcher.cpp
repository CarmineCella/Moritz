// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_patcher.cpp — patches inside patches and spectral processing
//
//   p name            subpatcher, saved inline in the parent's file
//   <name>            abstraction: any name.moritz next to the patch; its
//                     arguments replace #1..#9 inside, #0 is unique per copy
//   inlet, inlet~     a subpatcher's inlets, ordered left to right
//   outlet, outlet~   its outlets
//   poly~ name n      n copies of an abstraction, with voice allocation
//   pfft~ name N ov   runs abstraction name.moritz once per FFT frame (size N,
//                     overlap ov); inside, fftin~ / fftout~ give and take
//                     the spectrum, one bin per sample
//   cartopol~ poltocar~ framedelta~ frameaccum~ phasewrap~   spectral tools
//   expr~ sah~ count~ index~ poke~                             signal utilities

#include "expr.h"
#include "signals/fft.h"   // Musil's FFT
#include "objects.h"

#include <atomic>
#include <map>

namespace moritz {
namespace {

// ================================================================ inlets and outlets

// inlet / inlet~: the subpatcher's inlet number `index` (set by the owner)
class InletBox : public Box {
public:
    explicit InletBox(bool sig) : sig_(sig) { setPorts({}, {sig ? P::Signal : P::Control}); }
    bool isSig() const { return sig_; }
    std::atomic<int> index{-1};        // set at compile; the audio thread reads it
    void receive(const Message& m) { outlet(0, m); }
    void dsp(int n) override {
        Box* o = patch->owner;             // its owner is running (it runs this patch): its inputs
        int k = index.load(std::memory_order_relaxed);
        if (o && k >= 0) std::copy_n(o->innerInput(patch, k, n), n, sigOut(0));   // zeros for a control inlet
        else dsp::fill(sigOut(0), 0.f, n);
    }

private:
    bool sig_;
};

// outlet / outlet~
class OutletBox : public Box {
public:
    explicit OutletBox(bool sig) : sig_(sig) { setPorts({sig ? P::Signal : P::Control}, {}); }
    bool isSig() const { return sig_; }
    std::atomic<int> index{-1};        // set at compile; the audio thread reads it
    void message(int, const Message& m) override {
        Box* o = patch->owner;
        int k = index.load();
        if (!sig_ && o && k >= 0) o->innerOutlet(patch, k, m);
    }
    void dsp(int n) override {
        Box* o = patch->owner;
        int k = index.load(std::memory_order_relaxed);
        if (float* y = o && k >= 0 ? o->innerOutput(patch, k) : nullptr) {   // the owner's outlet, in its plan
            const float* x = sigIn(0, n);
            for (int i = 0; i < n; ++i) y[i] += x[i];
        }
    }

private:
    bool sig_;
};

// ================================================================ p and abstractions

// a patch's inlet and outlet boxes, ordered left to right; numbered ones
// (in 1, out~ 2, as in Max's poly~ voices) by their number, before the others
void collectPorts(Patch& p, std::vector<InletBox*>& in, std::vector<OutletBox*>& out) {
    for (auto& b : p.boxes) {
        if (auto i = dynamic_cast<InletBox*>(b.get())) in.push_back(i);
        if (auto o = dynamic_cast<OutletBox*>(b.get())) out.push_back(o);
    }
    auto number = [](const Box* b) {
        return !b->args.empty() && b->args[0].isFloat() && b->args[0].f >= 1.f ? b->args[0].f : 1e9f;
    };
    auto order = [&](const Box* a, const Box* b) {
        float na = number(a), nb = number(b);
        return na != nb ? na < nb : a->x < b->x;
    };
    std::stable_sort(in.begin(), in.end(), order);
    std::stable_sort(out.begin(), out.end(), order);
}

class SubpatchBox : public Box {
public:
    explicit SubpatchBox(Patch* context) : child_(std::make_unique<Patch>()) {
        child_->parent = context;
        child_->owner = this;
        setPorts({}, {});
    }
    ~SubpatchBox() override { child_.reset(); }   // closes its window (Patch::onDestroyed)

    Patch* inner() const override { return child_.get(); }
    bool savesInner() const override { return !child_->isAbstraction; }

    void loadInner(const SExpr& form, std::vector<std::string>& warnings) override {
        std::string err;
        if (!child_->load(form, err)) warnings.push_back(err);
        else if (!err.empty()) warnings.push_back(err);
        syncPorts();
    }
    void attached() override { if (child_->parent != patch) child_->parent = patch; }
    void loadbang() override { child_->loadbangAll(); }

    // Ports follow the inner inlet/outlet boxes, ordered by x like Max.
    void syncPorts() override {
        std::vector<InletBox*> in;
        std::vector<OutletBox*> out;
        collect(in, out);
        std::vector<Port> ip, op;
        for (size_t k = 0; k < in.size(); ++k) {
            in[k]->index.store((int)k);
            ip.push_back(in[k]->isSig() ? P::Signal : P::Control);
        }
        for (size_t k = 0; k < out.size(); ++k) {
            out[k]->index.store((int)k);
            op.push_back(out[k]->isSig() ? P::Signal : P::Control);
        }
        if (ip != ins || op != outs) setPorts(ip, op);
    }

    void message(int in, const Message& m) override {
        if (in < 0 || in >= numIns()) return;
        if (ins[in] == P::Signal && m[0].isFloat()) setScalar(in, m[0].f);
        for (auto& b : child_->boxes)
            if (auto ib = dynamic_cast<InletBox*>(b.get()))
                if (ib->index.load() == in && !ib->isSig()) ib->receive(m);
    }

    // the outlets of the plan (the ports may be changing on the GUI thread), then the inner patch
    void dsp(int n) override {
        for (int k = 0; k < outCount(); ++k)
            if (float* y = sigOutIf(k)) dsp::fill(y, 0.f, n);
        runInner(0, n);
    }

protected:
    void collect(std::vector<InletBox*>& in, std::vector<OutletBox*>& out) {
        collectPorts(*child_, in, out);
    }
    std::unique_ptr<Patch> child_;
};

int gInstances = 1000;      // #0 values
std::vector<std::string> gLoading;   // abstraction files being loaded, outermost first

bool loadAbstractionInto(Patch& child, const std::string& path, const Message& args,
                         std::string& err) {
    std::string src;
    if (!readFile(path, src)) { err = "can't read " + path; return false; }
    // an abstraction containing itself (directly or through others) would never end
    if (std::find(gLoading.begin(), gLoading.end(), path) != gLoading.end()) {
        err = "contains itself";
        return false;
    }
    if (gLoading.size() > 16) { err = "abstractions nested too deeply"; return false; }
    child.isAbstraction = true;
    child.file = path;
    child.absArgs = args;
    child.instance = ++gInstances;
    child.dir = dirOf(path);
    gLoading.push_back(path);
    bool ok = child.load(src, err);
    gLoading.pop_back();
    return ok;
}

class AbstractionBox : public SubpatchBox {
public:
    AbstractionBox(const std::string& path, const Message& args, Patch* context)
        : SubpatchBox(context) {
        std::string err;
        if (!loadAbstractionInto(*child_, path, args, err) || !err.empty())
            child_->print(path + ": " + err);
        syncPorts();
    }
};

// ================================================================ pfft~

class FftinBox;
class FftoutBox;

// pfft~ name [fftsize 512] [overlap 4]: analyses each signal inlet with a
// Hann-windowed FFT every fftsize/overlap samples, runs the inner patch
// over the frame (fftsize/2 bins, DC first) and resynthesises the spectra
// written to fftout~ by overlap-add. The work of a frame (the FFT of each
// inlet, the inner patch, the inverse FFT of each outlet) is spread over the
// blocks of the next hop, so no block carries a whole frame: the latency is
// fftsize + fftsize/overlap samples (one hop more than doing it all at once).
class PfftBox : public SubpatchBox {
public:
    PfftBox(const Message& a, Patch* context) : SubpatchBox(context) {
        name_ = argstr(a, 0);
        int n = (int)argf(a, 1, 512.f), ov = (int)argf(a, 2, 4.f);
        N_ = 128;
        while (N_ < n && N_ < 16384) N_ <<= 1;
        ov = std::clamp(ov, 1, N_);
        hop_ = std::max(1, N_ / ov);
        bins_ = N_ / 2;
        L_ = N_ + hop_;
        child_->spectral.fftSize = N_;
        child_->spectral.bins = bins_;
        fft_.init(N_);
        win_.resize((size_t)N_);
        double sumSq = 0;
        for (int t = 0; t < N_; ++t) {
            win_[(size_t)t] = (float)(0.5 - 0.5 * std::cos(dsp::kTwoPi * t / N_));
            sumSq += (double)win_[(size_t)t] * win_[(size_t)t];
        }
        // analysis and synthesis windows overlap-add to sumSq / hop per sample
        scale_ = (float)(hop_ / (sumSq * N_));
        std::string path = findAbstraction(name_, context);
        std::string err;
        if (path.empty()) child_->print("pfft~: can't find " + name_ + ".moritz");
        else if (!loadAbstractionInto(*child_, path, tailOf(a, 3), err) || !err.empty())
            child_->print(path + ": " + err);
        syncPorts();
    }

    void syncPorts() override;

    // spectral I/O for fftin~ / fftout~ (audio thread, while the inner patch runs)
    const float* inRe(int k) const { return cur_ && k < cur_->nIn ? cur_->sre[(size_t)k].data() : nullptr; }
    const float* inIm(int k) const { return cur_ && k < cur_->nIn ? cur_->sim[(size_t)k].data() : nullptr; }
    float* outRe(int k) { return cur_ && k < cur_->nOut ? cur_->ore[(size_t)k].data() : nullptr; }
    float* outIm(int k) { return cur_ && k < cur_->nOut ? cur_->oim[(size_t)k].data() : nullptr; }
    int bins() const { return bins_; }

    void dsp(int n) override {
        State* s = st_.get();
        for (int j = 0; j < outCount(); ++j)
            if (float* y = sigOutIf(j); y && (!s || j >= s->nOut)) dsp::fill(y, 0.f, n);
        if (!s) return;
        if (s != last_) { last_ = s; stage_ = -1; }        // new ports: a frame of the old ones is dropped
        cur_ = s;
        for (int k = 0; k < s->nIn; ++k) s->in[(size_t)k] = sigIn(k, n);
        for (int j = 0; j < s->nOut; ++j) s->out[(size_t)j] = sigOut(j);
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < s->nIn; ++k) s->ring[(size_t)k][(size_t)w_] = s->in[(size_t)k][i];
            for (int j = 0; j < s->nOut; ++j) {
                float& a = s->acc[(size_t)j][(size_t)w_];
                s->out[(size_t)j][i] = a;
                a = 0.f;
            }
            w_ = (w_ + 1) % L_;
            if (++count_ >= hop_) {
                count_ = 0;
                finish(*s);                              // the last frame is out before it is heard
                capture(*s);
            }
        }
        // the stages of the frame due by now, spread evenly over the hop
        const int S = stages(*s);
        const int due = (int)((long long)S * count_ / hop_);
        while (stage_ >= 0 && stage_ < due) runStage(*s, stage_++);
        cur_ = nullptr;
    }

private:
    // what depends on the ports (made on the GUI thread, published)
    struct State {
        int nIn = 0, nOut = 0;
        std::vector<std::vector<float>> ring, acc, frame, sre, sim, ore, oim;
        std::vector<const float*> in;
        std::vector<float*> out;
        std::vector<float> re, im;
    };
    int stages(const State& s) const { return s.nIn + 1 + s.nOut; }
    void finish(State& s) {
        if (stage_ >= 0)
            while (stage_ < stages(s)) runStage(s, stage_++);
        stage_ = -1;
    }
    // the last N samples of each inlet, windowed, oldest first
    void capture(State& s) {
        for (int k = 0; k < s.nIn; ++k) {
            const float* r = s.ring[(size_t)k].data();
            float* f = s.frame[(size_t)k].data();
            for (int t = 0; t < N_; ++t) f[t] = r[(w_ + L_ - N_ + t) % L_] * win_[(size_t)t];
        }
        at_ = w_;
        stage_ = 0;
    }
    // stage st of a frame: an inlet's FFT, the inner patch, or an outlet's inverse FFT
    void runStage(State& s, int st) {
        float *re = s.re.data(), *im = s.im.data();
        if (st < s.nIn) {
            std::copy_n(s.frame[(size_t)st].begin(), N_, re);
            std::fill_n(im, N_, 0.f);
            fft_.forward(re, im);
            std::copy_n(re, bins_, s.sre[(size_t)st].begin());
            std::copy_n(im, bins_, s.sim[(size_t)st].begin());
        } else if (st == s.nIn) {
            for (int j = 0; j < s.nOut; ++j) {
                std::fill(s.ore[(size_t)j].begin(), s.ore[(size_t)j].end(), 0.f);
                std::fill(s.oim[(size_t)j].begin(), s.oim[(size_t)j].end(), 0.f);
            }
            runInner(0, bins_);                          // the whole frame is one vector
        } else {
            int j = st - s.nIn - 1;
            const float* r = s.ore[(size_t)j].data();
            const float* m = s.oim[(size_t)j].data();
            re[0] = r[0];
            im[0] = 0.f;
            for (int b = 1; b < bins_; ++b) {             // a real signal's spectrum
                re[b] = r[b];
                im[b] = m[b];
                re[N_ - b] = r[b];
                im[N_ - b] = -m[b];
            }
            re[bins_] = 0.f;
            im[bins_] = 0.f;
            fft_.inverse(re, im);
            float* a = s.acc[(size_t)j].data();         // heard from the next hop on
            for (int t = 0; t < N_; ++t) a[(at_ + hop_ + t) % L_] += re[t] * win_[(size_t)t] * scale_;
        }
    }

    std::string name_;
    int N_ = 512, hop_ = 128, bins_ = 256, L_ = 640;
    int nIn_ = 0, nOut_ = 0;                             // GUI side: the ports of the published state
    int w_ = 0, count_ = 0, at_ = 0, stage_ = -1;        // audio side
    float scale_ = 1.f;
    musil::dsp::FFT<float> fft_;
    std::vector<float> win_;
    Published<State> st_;
    State* last_ = nullptr;                              // audio side
    State* cur_ = nullptr;                               // the state of the frame being run
};

// fftin~ n: outlets real, imaginary, bin index of pfft~ signal inlet n (1-based)
class FftinBox : public Box {
public:
    explicit FftinBox(const Message& a) : n_(std::max(1, (int)argf(a, 0, 1.f))) {
        setPorts({}, {P::Signal, P::Signal, P::Signal});
    }
    int number() const { return n_; }
    void resolve() override { pf_.store(findPfft(patch), std::memory_order_relaxed); }
    void dsp(int n) override {
        PfftBox* pf = pf_.load(std::memory_order_relaxed);
        const float* re = pf ? pf->inRe(n_ - 1) : nullptr;
        const float* im = pf ? pf->inIm(n_ - 1) : nullptr;
        float *yr = sigOut(0), *yi = sigOut(1), *yb = sigOut(2);
        for (int i = 0; i < n; ++i) {
            yr[i] = re ? re[i] : 0.f;
            yi[i] = im ? im[i] : 0.f;
            yb[i] = (float)i;
        }
    }
    static PfftBox* findPfft(Patch* p) {
        for (; p; p = p->parent)
            if (auto pf = dynamic_cast<PfftBox*>(p->owner)) return pf;
        return nullptr;
    }

private:
    int n_;
    std::atomic<PfftBox*> pf_{nullptr};
};

// fftout~ n: inlets real, imaginary for pfft~ signal outlet n (1-based)
class FftoutBox : public Box {
public:
    explicit FftoutBox(const Message& a) : n_(std::max(1, (int)argf(a, 0, 1.f))) {
        setPorts({P::Signal, P::Signal}, {});
    }
    int number() const { return n_; }
    void resolve() override { pf_.store(FftinBox::findPfft(patch), std::memory_order_relaxed); }
    void dsp(int n) override {
        PfftBox* pf = pf_.load(std::memory_order_relaxed);
        float* re = pf ? pf->outRe(n_ - 1) : nullptr;
        float* im = pf ? pf->outIm(n_ - 1) : nullptr;
        if (!re) return;
        const float* xr = sigIn(0, n);
        const float* xi = sigIn(1, n);
        for (int i = 0; i < n; ++i) {
            re[i] += xr[i];
            im[i] += xi[i];
        }
    }

private:
    int n_;
    std::atomic<PfftBox*> pf_{nullptr};
};

// pfft~ ports: signal inlets for fftin~ 1..n, then the control inlets of
// `inlet` boxes; signal outlets for fftout~ 1..m, then `outlet` boxes.
void PfftBox::syncPorts() {
    int nin = 0, nout = 0;
    for (auto& b : child_->boxes) {
        if (auto f = dynamic_cast<FftinBox*>(b.get())) nin = std::max(nin, f->number());
        if (auto f = dynamic_cast<FftoutBox*>(b.get())) nout = std::max(nout, f->number());
    }
    std::vector<InletBox*> in;
    std::vector<OutletBox*> out;
    collect(in, out);
    std::vector<Port> ip((size_t)nin, P::Signal), op((size_t)nout, P::Signal);
    for (auto* b : in) {
        b->index.store(b->isSig() ? -1 : (int)ip.size());      // inlet~ has no meaning here
        if (!b->isSig()) ip.push_back(P::Control);
    }
    for (auto* b : out) {
        b->index.store(b->isSig() ? -1 : (int)op.size());
        if (!b->isSig()) op.push_back(P::Control);
    }
    if (ip != ins || op != outs) setPorts(ip, op);
    if (st_.get() && nin == nIn_ && nout == nOut_) return;
    nIn_ = nin;
    nOut_ = nout;
    auto s = std::make_unique<State>();                  // made here, published: dsp() never resizes
    s->nIn = nin;
    s->nOut = nout;
    s->ring.assign((size_t)nin, std::vector<float>((size_t)L_, 0.f));
    s->frame.assign((size_t)nin, std::vector<float>((size_t)N_, 0.f));
    s->sre.assign((size_t)nin, std::vector<float>((size_t)bins_, 0.f));
    s->sim = s->sre;
    s->acc.assign((size_t)nout, std::vector<float>((size_t)L_, 0.f));
    s->ore.assign((size_t)nout, std::vector<float>((size_t)bins_, 0.f));
    s->oim = s->ore;
    s->in.assign((size_t)nin, nullptr);
    s->out.assign((size_t)nout, nullptr);
    s->re.assign((size_t)N_, 0.f);
    s->im.assign((size_t)N_, 0.f);
    st_.set(patch, std::move(s));
}

// ================================================================ poly~

// poly~ name voices [args...]: `voices` copies of the abstraction name.moritz
// (args become #1.. in each). Ports come from the inlets/outlets inside
// (in, in~, out, out~ are the same objects); signal outlets of the voices
// are summed. Messages to a control inlet:
//   note ...        the rest goes to a free voice (or steals the oldest)
//   midinote p v    v > 0: "p v" to a free voice (or the one already playing
//                   p); v = 0: "p 0" to the voice playing p, so note-offs
//                   reach the voice their note-on went to
//   target n        later messages go to voice n only (0 = all voices)
//   mute n 1|0      stop/restart processing voice n (0 = all)
//   anything else   to the target voice, or to all voices
// A voice is busy from its note until the signal into its thispoly~
// returns to zero (or thispoly~ gets 0).
class PolyBox : public Box {
public:
    struct Voice {
        std::unique_ptr<Patch> patch;
        std::atomic<bool> busy{false}, muted{false}, loud{false};
        uint64_t used = 0;              // note order, for stealing the oldest
        int pitch = -1;                 // held midinote pitch (GUI thread), -1 = none
    };

    PolyBox(const Message& a, Patch* context) : name_(argstr(a, 0)) {
        int n = std::clamp((int)argf(a, 1, 1.f), 1, 128);
        std::string path = findAbstraction(name_, context);
        if (path.empty() && context) context->print("poly~: can't find " + name_ + ".moritz");
        for (int k = 0; k < n; ++k) {
            auto v = std::make_unique<Voice>();
            v->patch = std::make_unique<Patch>();
            v->patch->parent = context;
            v->patch->owner = this;
            std::string err;
            if (!path.empty() && (!loadAbstractionInto(*v->patch, path, tailOf(a, 2), err) || !err.empty()) && k == 0)
                v->patch->print(path + ": " + err);
            voices_.push_back(std::move(v));
        }
        setPorts({}, {});
        syncPorts();
    }
    ~PolyBox() override { voices_.clear(); }

    Patch* inner() const override { return voices_[0]->patch.get(); }
    std::vector<Patch*> inners() const override {
        std::vector<Patch*> v;
        for (auto& x : voices_) v.push_back(x->patch.get());
        return v;
    }
    void attached() override {
        for (auto& v : voices_)
            if (v->patch->parent != patch) v->patch->parent = patch;
    }
    void loadbang() override { for (auto& v : voices_) v->patch->loadbangAll(); }

    void syncPorts() override {
        std::vector<Port> ip, op;
        for (size_t k = 0; k < voices_.size(); ++k) {
            std::vector<InletBox*> in;
            std::vector<OutletBox*> out;
            collectPorts(*voices_[k]->patch, in, out);
            for (size_t j = 0; j < in.size(); ++j) {
                in[j]->index.store((int)j);
                if (k == 0) ip.push_back(in[j]->isSig() ? P::Signal : P::Control);
            }
            for (size_t j = 0; j < out.size(); ++j) {
                out[j]->index.store((int)j);
                if (k == 0) op.push_back(out[j]->isSig() ? P::Signal : P::Control);
            }
        }
        if (ip != ins || op != outs) setPorts(ip, op);
    }

    void message(int in, const Message& m) override {
        if (in < 0 || in >= numIns()) return;
        if (ins[in] == P::Signal) {
            if (m[0].isFloat()) setScalar(in, m[0].f);
            return;
        }
        if (m[0].is("note")) {
            int v = pick();
            Voice& x = *voices_[(size_t)v];
            x.used = ++clock_;
            x.pitch = -1;                           // a stolen voice no longer holds its midinote
            x.loud.store(false);
            x.busy.store(true);
            deliver(v, in, tailOf(m));
        } else if (m[0].is("midinote") && m.size() > 2 && m[1].isFloat() && m[2].isFloat()) {
            int p = (int)m[1].f;
            int held = -1;
            for (int k = 0; k < (int)voices_.size(); ++k)
                if (voices_[(size_t)k]->pitch == p) held = k;
            if (m[2].f > 0.f) {                     // note-on: retrigger p's voice, or a free one
                int v = held >= 0 ? held : pick();
                Voice& x = *voices_[(size_t)v];
                for (auto& o : voices_) if (o.get() != &x && o->pitch == p) o->pitch = -1;
                x.pitch = p;
                x.used = ++clock_;
                x.loud.store(false);
                x.busy.store(true);
                deliver(v, in, tailOf(m));
            } else if (held >= 0) {                 // note-off: to the voice playing p
                voices_[(size_t)held]->pitch = -1;
                deliver(held, in, tailOf(m));
            }
        } else if (m[0].is("target")) {
            target_ = std::clamp((int)argf(m, 1, 0.f), 0, (int)voices_.size());
        } else if (m[0].is("mute")) {
            int v = (int)argf(m, 1, 0.f);
            bool on = argf(m, 2, 1.f) != 0.f;
            for (int k = 0; k < (int)voices_.size(); ++k)
                if (v == 0 || v == k + 1) voices_[(size_t)k]->muted.store(on);
        } else if (target_ > 0) {
            deliver(target_ - 1, in, m);
        } else {
            for (int k = 0; k < (int)voices_.size(); ++k) deliver(k, in, m);
        }
    }

    void dsp(int n) override {
        for (int k = 0; k < outCount(); ++k)
            if (float* y = sigOutIf(k)) dsp::fill(y, 0.f, n);
        for (size_t k = 0; k < voices_.size(); ++k)
            if (!voices_[k]->muted.load(std::memory_order_relaxed)) runInner((int)k, n);
    }

    // for thispoly~
    int voiceOf(const Patch* p) const {
        for (; p; p = p->parent)
            for (size_t k = 0; k < voices_.size(); ++k)
                if (voices_[k]->patch.get() == p) return (int)k;
        return -1;
    }
    Voice* voice(int k) { return k >= 0 && k < (int)voices_.size() ? voices_[(size_t)k].get() : nullptr; }
    int busyCount() const {
        int c = 0;
        for (auto& v : voices_) c += v->busy.load();
        return c;
    }

private:
    int pick() {
        auto playable = [&](int k) { return !voices_[(size_t)k]->muted.load(); };   // muted voices are skipped
        int best = -1;
        for (int k = 0; k < (int)voices_.size(); ++k)       // the free voice idle longest
            if (playable(k) && !voices_[(size_t)k]->busy.load() &&
                (best < 0 || voices_[(size_t)k]->used < voices_[(size_t)best]->used))
                best = k;
        if (best >= 0) return best;
        for (int k = 0; k < (int)voices_.size(); ++k)       // all busy: steal the oldest note
            if (playable(k) && (best < 0 || voices_[(size_t)k]->used < voices_[(size_t)best]->used))
                best = k;
        return best >= 0 ? best : 0;                        // every voice muted
    }
    void deliver(int v, int in, const Message& m) {
        if (m.empty()) return;
        for (auto& b : voices_[(size_t)v]->patch->boxes)
            if (auto ib = dynamic_cast<InletBox*>(b.get()))
                if (ib->index.load() == in && !ib->isSig()) ib->receive(m);
    }

    std::string name_;
    std::vector<std::unique_ptr<Voice>> voices_;
    int target_ = 0;
    uint64_t clock_ = 0;
};

// thispoly~: inside a poly~ voice. Outputs the voice number (from 1) on
// bang and at load. The voice stays busy while the signal in its inlet is
// not zero (connect the amplitude envelope); a number sets busy directly,
// "mute 1|0" mutes the voice.
class ThispolyBox : public Box {
public:
    ThispolyBox() { setPorts({P::Signal}, {P::Control}); }
    void resolve() override {
        PolyBox* poly = nullptr;
        for (Patch* p = patch; p && !poly; p = p->parent) poly = dynamic_cast<PolyBox*>(p->owner);
        poly_.store(poly);
        voice_.store(poly ? poly->voiceOf(patch) : -1);
    }
    void loadbang() override { outletFloat(0, (float)(voice_ + 1)); }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) { outletFloat(0, (float)(voice_ + 1)); return; }
        PolyBox* poly = poly_.load();
        PolyBox::Voice* v = poly ? poly->voice(voice_) : nullptr;
        if (!v) return;
        if (m[0].isFloat()) v->busy.store(m[0].f != 0.f);
        else if (m[0].is("mute")) v->muted.store(argf(m, 1, 1.f) != 0.f);
    }
    void dsp(int n) override {
        PolyBox* poly = poly_.load(std::memory_order_relaxed);
        PolyBox::Voice* v = poly ? poly->voice(voice_.load(std::memory_order_relaxed)) : nullptr;
        if (!v || !connected(0)) return;
        const float* x = sigIn(0, n);
        bool sound = false;
        for (int i = 0; i < n && !sound; ++i) sound = std::fabs(x[i]) > 1e-6f;
        if (sound) {
            v->loud.store(true);
            v->busy.store(true);
        } else if (v->loud.exchange(false)) {
            v->busy.store(false);                // the envelope has finished
        }
    }

private:
    std::atomic<PolyBox*> poly_{nullptr};   // set at compile; the audio thread reads them
    std::atomic<int> voice_{-1};
};

// ================================================================ spectral tools

// cartopol~: real, imaginary -> amplitude, phase
class CartopolBox : public Box {
public:
    CartopolBox() { setPorts({P::Signal, P::Signal}, {P::Signal, P::Signal}); }
    void dsp(int n) override {
        const float *re = sigIn(0, n), *im = sigIn(1, n);
        float *a = sigOut(0), *p = sigOut(1);
        for (int i = 0; i < n; ++i) {
            float r = re[i], m = im[i];
            a[i] = std::sqrt(r * r + m * m);
            p[i] = std::atan2(m, r);
        }
    }
};

// poltocar~: amplitude, phase -> real, imaginary
class PoltocarBox : public Box {
public:
    PoltocarBox() { setPorts({P::Signal, P::Signal}, {P::Signal, P::Signal}); }
    void dsp(int n) override {
        const float *a = sigIn(0, n), *p = sigIn(1, n);
        float *re = sigOut(0), *im = sigOut(1);
        for (int i = 0; i < n; ++i) {
            float aa = a[i], pp = p[i];
            re[i] = aa * std::cos(pp);
            im[i] = aa * std::sin(pp);
        }
    }
};

// Per-bin memory: inside pfft~ it holds one value per bin, so each bin is
// compared with the same bin of the previous frame; elsewhere one value.
class FrameMemory : public Box {
public:
    // sized at compile (one value per bin) and published, never in dsp()
    void resolve() override {
        const Patch::Spectral* sp = patch->spectralContext();
        size_t size = sp ? (size_t)sp->bins : 1;
        if (size_ != size) {
            size_ = size;
            mem_.set(patch, std::make_unique<std::vector<float>>(size, 0.f));
        }
    }

protected:
    std::vector<float>* memory() { return mem_.get(); }   // audio thread: for this block
    static float& slot(std::vector<float>& m, int i) { return m[m.size() > 1 ? (size_t)i % m.size() : 0]; }

private:
    Published<std::vector<float>> mem_;
    size_t size_ = 0;
};

// framedelta~: difference from the previous frame (phase -> frequency)
class FramedeltaBox : public FrameMemory {
public:
    FramedeltaBox() { setPorts({P::Signal}, {P::Signal}); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        std::vector<float>* mem = memory();
        if (!mem) { dsp::fill(y, 0.f, n); return; }
        for (int i = 0; i < n; ++i) {
            float& m = slot(*mem, i);
            y[i] = x[i] - m;
            m = x[i];
        }
    }
};

// frameaccum~ [wrap]: running sum per bin; with wrap 1 the sum is kept
// in -pi..pi (phase accumulation for a phase vocoder). Where the third
// inlet is not zero, that bin restarts from the value in the second inlet
// instead (for example the analysed phase, after a jump in the sound).
class FrameaccumBox : public FrameMemory {
public:
    explicit FrameaccumBox(const Message& a) : wrap_(argf(a, 0, 0.f) != 0.f) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
    }
    void message(int in, const Message& m) override {
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        const float* to = sigIn(1, n);
        const float* reset = sigIn(2, n);
        float* y = sigOut(0);
        std::vector<float>* mem = memory();
        if (!mem) { dsp::fill(y, 0.f, n); return; }
        for (int i = 0; i < n; ++i) {
            float& m = slot(*mem, i);
            m = reset[i] != 0.f ? to[i] : m + x[i];
            if (wrap_) m = dsp::wrapPhase(m);
            y[i] = m;
        }
    }

private:
    bool wrap_;
};

// pvlock~: phase vocoder resynthesis phase with identity phase locking
// (Laroche and Dolson). Inlets: amplitude, analysed phase, phase difference
// from the previous analysis frame, reset. For each frame it finds the
// amplitude peaks; a peak advances its own previous output phase by its
// phase difference, and every other bin takes the phase of its peak plus
// the analysed offset between the two. That keeps the bins of one partial
// in step, so time-stretched sounds stay clean. Where reset is not zero the
// output is the analysed phase. Needs a whole frame: use it inside pfft~.
class PvlockBox : public Box {
public:
    PvlockBox() { setPorts({P::Signal, P::Signal, P::Signal, P::Signal}, {P::Signal}); }
    void message(int in, const Message& m) override {
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    // the memory of a whole frame (or block), made at compile and published, never in dsp()
    void resolve() override {
        size_t vs = (size_t)patch->vectorSize();
        if (vs == size_) return;
        size_ = vs;
        auto m = std::make_unique<Memory>();
        m->prev.assign(vs, 0.f);
        m->peaks.reserve(vs);
        mem_.set(patch, std::move(m));
    }
    void dsp(int n) override {
        const float *a = sigIn(0, n), *ph = sigIn(1, n), *d = sigIn(2, n), *rs = sigIn(3, n);
        float* y = sigOut(0);
        Memory* mem = mem_.get();
        if (!mem) { dsp::fill(y, 0.f, n); return; }
        std::vector<float>& prev_ = mem->prev;
        std::vector<int>& peaks_ = mem->peaks;
        n = std::min(n, (int)prev_.size());
        bool reset = false;
        for (int k = 0; k < n && !reset; ++k) reset = rs[k] != 0.f;
        if (reset) {
            for (int k = 0; k < n; ++k) y[k] = prev_[(size_t)k] = ph[k];
            return;
        }
        peaks_.clear();
        for (int k = 1; k + 1 < n; ++k)
            if (a[k] > a[k - 1] && a[k] >= a[k + 1]) peaks_.push_back(k);
        if (peaks_.empty()) {
            for (int k = 0; k < n; ++k) y[k] = prev_[(size_t)k] = dsp::wrapPhase(prev_[(size_t)k] + d[k]);
            return;
        }
        int start = 0;
        for (size_t j = 0; j < peaks_.size(); ++j) {
            int p = peaks_[j], end = n;
            if (j + 1 < peaks_.size()) {                 // region ends at the lowest bin between peaks
                int q = peaks_[j + 1];
                end = p;
                for (int k = p; k <= q; ++k)
                    if (a[k] < a[end]) end = k;
            }
            float th = dsp::wrapPhase(prev_[(size_t)p] + d[p]);
            for (int k = start; k < end; ++k) y[k] = dsp::wrapPhase(th + ph[k] - ph[p]);
            start = end;
        }
        std::copy(y, y + n, prev_.begin());
    }

private:
    struct Memory {
        std::vector<float> prev;
        std::vector<int> peaks;
    };
    Published<Memory> mem_;
    size_t size_ = 0;
};

// lrshift~ n: shifts the signal vector n samples (positive = right); inside
// pfft~ it moves each bin's value to the bin n above, so bins can be
// combined with their neighbours. Samples shifted in are 0.
class LrshiftBox : public Box {
public:
    explicit LrshiftBox(const Message& a) : n_((int)argf(a, 0, 0.f)) {
        setPorts({P::Signal}, {P::Signal});
    }
    // a number sets the shift (as Max's lrshift~), not the signal
    void message(int, const Message& m) override {
        if (m[0].isFloat() && std::isfinite(m[0].f)) n_.store((int)std::clamp(m[0].f, -1e6f, 1e6f));
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        int s = n_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            int j = i - s;
            y[i] = j >= 0 && j < n ? x[j] : 0.f;
        }
    }

private:
    std::atomic<int> n_;
};

// phasewrap~: wraps into -pi..pi
class PhasewrapBox : public Box {
public:
    PhasewrapBox() { setPorts({P::Signal}, {P::Signal}); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) y[i] = dsp::wrapPhase(x[i]);
    }
};

// ================================================================ signal utilities

// expr~: expr evaluated per sample. $v1..$v9 (or $f/$i) read the inlets,
// which take signals or floats. `seed n` seeds random() (at the next block).
class ExprSigBox : public Box {
public:
    explicit ExprSigBox(const Message& a) : src_(messageToString(a)) {
        int nv = 0;
        ok_ = ExprParser(src_).parse(prog_, nv, err_);
        nv_ = std::max(1, nv);
        setPorts(std::vector<Port>((size_t)nv_, P::Signal), {P::Signal});
    }
    Message saveArgs() const override { return {Atom(src_)}; }
    std::string text() const override { return "expr~ " + src_; }
    void attached() override {
        if (!ok_) patch->print("expr~: " + err_ + " in \"" + src_ + "\"");
        prog_.seed(ExprProgram::seedOf(id));        // random(): the same numbers at each run (not running yet)
    }
    void message(int in, const Message& m) override {
        if (in == 0 && m[0].is("seed") && m.size() > 1 && m[1].isFloat()) {   // at the next block
            post(kSeed, m[1].f);
            return;
        }
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        float* y = sigOut(0);
        for (int k = 0, ne = events(); k < ne; ++k)
            if (event(k).kind == kSeed) prog_.seed(ExprProgram::seedOf(event(k).a));
        if (!ok_) { dsp::fill(y, 0.f, n); return; }
        const float* in[9];
        for (int k = 0; k < 9; ++k) in[k] = sigIn(std::min(k, nv_ - 1), n);
        prog_.evalBlock(in, n, y);                   // a flat program, an instruction over many samples
    }

private:
    std::string src_, err_;
    bool ok_ = false;
    int nv_ = 1;
    enum : uint8_t { kSeed = kEvUser };
    std::unique_ptr<ExprProgram> progPtr_ = std::make_unique<ExprProgram>();   // 32 KB: on the heap
    ExprProgram& prog_ = *progPtr_;
};

// sah~ [threshold 0]: holds the left signal each time the right signal
// rises above the threshold
class SahBox : public Box {
public:
    explicit SahBox(const Message& a) {
        setPorts({P::Signal, P::Signal}, {P::Signal});
        th_.store(argf(a, 0, 0.f));
    }
    void message(int in, const Message& m) override {
        if (in == 1 && m[0].isFloat()) th_.store(m[0].f);
        else if (in == 0 && m[0].isFloat()) setScalar(0, m[0].f);
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *t = sigIn(1, n);
        float* y = sigOut(0);
        float th = th_.load();
        for (int i = 0; i < n; ++i) {
            if (prev_ <= th && t[i] > th) held_ = x[i];
            prev_ = t[i];
            y[i] = held_;
        }
    }

private:
    std::atomic<float> th_{0.f};
    float prev_ = 0.f, held_ = 0.f;
};

// count~ [min max loop]: a sample counter. bang starts it from min, a
// number from that value, stop stops it (at the sample the message is for).
// It wraps to min at max when loop is 1 (default) and otherwise stops there.
// Inside pfft~ it restarts at a frame boundary, so the count stays aligned
// to bins. Its count is exact: an index~ or poke~ it feeds directly gets it
// in double precision too, so they address every sample of a buffer longer
// than 2^24 samples (6 minutes at 48 kHz), which a float signal cannot.
class CountBox : public Box {
public:
    explicit CountBox(const Message& a) {
        setPorts({P::Control}, {P::Signal});
        min_ = (int64_t)argf(a, 0, 0.f);
        max_ = (int64_t)argf(a, 1, 0.f);
        loop_ = argf(a, 2, 1.f) != 0.f;
        v_ = min_;
    }
    bool preciseOut(int) const override { return true; }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) post(kStart, 0.f, 0.f, 1);           // inlet 1: from min
        else if (m[0].isFloat()) post(kStart, m[0].f);
        else if (m[0].is("stop")) post(kStop);
    }
    void dsp(int n) override {
        float* y = sigOut(0);
        double* yd = sigOutPrecise(0);
        eventPieces(*this, n, [&](int from, int to) {
            for (int i = from; i < to; ++i) {
                y[i] = (float)v_;
                if (yd) yd[i] = (double)v_;
                if (!running_) continue;
                ++v_;
                if (max_ > min_ && v_ >= max_) {
                    if (loop_) v_ = min_;
                    else { v_ = max_; running_ = false; }
                }
            }
        }, [&](const Event& e) {
            if (e.kind == kStart) { v_ = e.inlet == 1 ? min_ : (int64_t)e.a; running_ = true; }
            else if (e.kind == kStop) running_ = false;
        });
    }

private:
    enum : uint8_t { kStart = kEvUser, kStop };
    int64_t min_, max_, v_;                     // audio side (min, max: fixed)
    bool loop_, running_ = false;
};

// index~ name [channel]: reads a buffer~ at a sample index (no interpolation)
class IndexBox : public Box {
public:
    explicit IndexBox(const Message& a) : ref_(argstr(a, 0)), ch_((int)argf(a, 1, 1.f) - 1) {
        setPorts({P::Signal}, {P::Signal});
    }
    void resolve() override { ref_.resolve(patch); }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { ref_.set(patch, m[1].str()); }
        else if (m[0].isFloat()) setScalar(0, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        const double* xd = sigInPrecise(0);      // straight from count~: every sample of a long buffer
        float* y = sigOut(0);
        const SampleBuffer* b = ref_.get();
        for (int i = 0; i < n; ++i) y[i] = b ? b->at(xd ? (long)xd[i] : (long)x[i], std::max(0, ch_)) : 0.f;
    }

private:
    BufferRef ref_;
    int ch_;
};

// poke~ name [channel]: writes the left signal into a buffer~ at the sample
// index given by the right signal; indices outside the buffer are ignored
class PokeBox : public Box {
public:
    explicit PokeBox(const Message& a) : ref_(argstr(a, 0)), ch_((int)argf(a, 1, 1.f) - 1) {
        setPorts({P::Signal, P::Signal}, {});
    }
    void resolve() override { ref_.resolve(patch); }
    void message(int in, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { ref_.set(patch, m[1].str()); }
        else if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        SampleBuffer* bp = ref_.get();
        if (!bp) return;
        SampleBuffer& b = *bp;
        const float *x = sigIn(0, n), *idx = sigIn(1, n);
        const double* idxd = sigInPrecise(1);    // straight from count~: every sample of a long buffer
        int c = std::clamp(ch_, 0, b.channels - 1);
        bool wrote = false;
        for (int i = 0; i < n; ++i) {
            long k = idxd ? (long)idxd[i] : (long)idx[i];
            if (k >= 0 && k < b.frames) { b.data[(size_t)k * b.channels + c] = x[i]; wrote = true; }
        }
        if (wrote)                                           // an open sound editor reads the samples again
            if (BufferBox* box = ref_.box()) box->writes.fetch_add(1, std::memory_order_relaxed);
    }

private:
    BufferRef ref_;
    int ch_;
};

}  // namespace

std::unique_ptr<Box> makeAbstraction(const std::string& path, const Message& args, Patch* context) {
    return std::make_unique<AbstractionBox>(path, args, context);
}

// ---- patches a box runs that are not subpatchers (objects.h)
bool loadPatchInto(Patch& p, const std::string& path, std::string& err) {
    return loadAbstractionInto(p, path, {}, err) && err.empty();
}
void numberPatchPorts(Patch& p, int* sigIns, int* sigOuts) {
    std::vector<InletBox*> in;
    std::vector<OutletBox*> out;
    collectPorts(p, in, out);
    int si = 0, so = 0;
    for (size_t k = 0; k < in.size(); ++k) {
        in[k]->index.store((int)k);
        si += in[k]->isSig();
    }
    for (size_t k = 0; k < out.size(); ++k) {
        out[k]->index.store((int)k);
        so += out[k]->isSig();
    }
    if (sigIns) *sigIns = si;
    if (sigOuts) *sigOuts = so;
}
void sendToPatchInlet(Patch& p, int inlet, const Message& m) {
    if (m.empty()) return;
    for (auto& b : p.boxes)
        if (auto ib = dynamic_cast<InletBox*>(b.get()))
            if (ib->index.load() == inlet && !ib->isSig()) ib->receive(m);
}

void registerPatcherObjects(ClassTable& t) {
    using M = const Message&;
    wholeTextClasses().insert("expr~");
    t.insert(t.end(), {
        {"p", "Patching", "subpatcher: p name", [](M, Patch* c) { return std::make_unique<SubpatchBox>(c); }},
        {"inlet", "Patching", "subpatcher inlet", [](M, Patch*) { return std::make_unique<InletBox>(false); }},
        {"inlet~", "Patching", "subpatcher signal inlet", [](M, Patch*) { return std::make_unique<InletBox>(true); }},
        {"outlet", "Patching", "subpatcher outlet", [](M, Patch*) { return std::make_unique<OutletBox>(false); }},
        {"outlet~", "Patching", "subpatcher signal outlet", [](M, Patch*) { return std::make_unique<OutletBox>(true); }},
        {"poly~", "Patching", "polyphony: name voices [args]", [](M a, Patch* c) { return std::make_unique<PolyBox>(a, c); }},
        {"thispoly~", "Patching", "voice number; busy while its signal sounds", plain<ThispolyBox>()},
        {"in", "", "inlet (poly~ name)", [](M, Patch*) { return std::make_unique<InletBox>(false); }},
        {"in~", "", "inlet~ (poly~ name)", [](M, Patch*) { return std::make_unique<InletBox>(true); }},
        {"out", "", "outlet (poly~ name)", [](M, Patch*) { return std::make_unique<OutletBox>(false); }},
        {"out~", "", "outlet~ (poly~ name)", [](M, Patch*) { return std::make_unique<OutletBox>(true); }},
        {"pfft~", "Spectral", "FFT patcher: name fftsize overlap", [](M a, Patch* c) { return std::make_unique<PfftBox>(a, c); }},
        {"fftin~", "Spectral", "pfft~ input: real imag bin", with<FftinBox>()},
        {"fftout~", "Spectral", "pfft~ output: real imag", with<FftoutBox>()},
        {"cartopol~", "Spectral", "real/imag to amplitude/phase", plain<CartopolBox>()},
        {"poltocar~", "Spectral", "amplitude/phase to real/imag", plain<PoltocarBox>()},
        {"framedelta~", "Spectral", "difference from the previous frame", plain<FramedeltaBox>()},
        {"frameaccum~", "Spectral", "running sum per bin [wrap]; reset value, reset", with<FrameaccumBox>()},
        {"pvlock~", "Spectral", "phase vocoder phase with peak locking", plain<PvlockBox>()},
        {"lrshift~", "Spectral", "shift a frame by n bins (neighbours)", with<LrshiftBox>()},
        {"phasewrap~", "Spectral", "wrap a phase to -pi..pi", plain<PhasewrapBox>()},
        {"expr~", "Signal Math", "signal expression: $v1 * 2", with<ExprSigBox>()},
        {"sah~", "Signal Math", "sample and hold on a rising trigger", with<SahBox>()},
        {"count~", "Samples", "sample counter: min max [loop]", with<CountBox>()},
        {"index~", "Samples", "read a buffer~ at a sample index", with<IndexBox>()},
        {"poke~", "Samples", "write a buffer~ at a sample index", with<PokeBox>()},
    });
}

}  // namespace moritz
