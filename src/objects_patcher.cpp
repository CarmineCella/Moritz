// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_patcher.cpp — patches inside patches and spectral processing
//
//   p name            subpatcher, saved inline in the parent's file
//   <name>            abstraction: any name.mlsp next to the patch; its
//                     arguments replace #1..#9 inside, #0 is unique per copy
//   inlet, inlet~     a subpatcher's inlets, ordered left to right
//   outlet, outlet~   its outlets
//   poly~ name n      n copies of an abstraction, with voice allocation
//   pfft~ name N ov   runs abstraction name.mlsp once per FFT frame (size N,
//                     overlap ov); inside, fftin~ / fftout~ give and take
//                     the spectrum, one bin per sample
//   cartopol~ poltocar~ framedelta~ frameaccum~ phasewrap~   spectral tools
//   expr~ sah~ count~ index~ poke~                             signal utilities

#include "expr.h"
#include "objects.h"

#include <atomic>
#include <map>

namespace mlsp {
namespace {

// ================================================================ inlets and outlets

// inlet / inlet~: the subpatcher's inlet number `index` (set by the owner)
class InletBox : public Box {
public:
    explicit InletBox(bool sig) : sig_(sig) { setPorts({}, {sig ? P::Signal : P::Control}); }
    bool isSig() const { return sig_; }
    int index = -1;
    void receive(const Message& m) { outlet(0, m); }
    void dsp(int n) override {
        Box* o = patch->owner;
        if (o && index >= 0 && index < o->numIns() && o->ins[index] == P::Signal)
            std::copy_n(o->sigIn(index, n), n, sigOut(0));
        else
            dsp::fill(sigOut(0), 0.f, n);
    }

private:
    bool sig_;
};

// outlet / outlet~
class OutletBox : public Box {
public:
    explicit OutletBox(bool sig) : sig_(sig) { setPorts({sig ? P::Signal : P::Control}, {}); }
    bool isSig() const { return sig_; }
    int index = -1;
    void message(int, const Message& m) override {
        Box* o = patch->owner;
        if (!sig_ && o && index >= 0 && index < o->numOuts()) o->outlet(index, m);
    }
    void dsp(int n) override {
        Box* o = patch->owner;
        if (o && index >= 0 && index < o->numOuts() && o->outs[index] == P::Signal) {
            const float* x = sigIn(0, n);
            float* y = o->sigOut(index);
            for (int i = 0; i < n; ++i) y[i] += x[i];
        }
    }

private:
    bool sig_;
};

// ================================================================ p and abstractions

// a patch's inlet and outlet boxes, ordered left to right
void collectPorts(Patch& p, std::vector<InletBox*>& in, std::vector<OutletBox*>& out) {
    for (auto& b : p.boxes) {
        if (auto i = dynamic_cast<InletBox*>(b.get())) in.push_back(i);
        if (auto o = dynamic_cast<OutletBox*>(b.get())) out.push_back(o);
    }
    auto byX = [](const Box* a, const Box* b) { return a->x < b->x; };
    std::stable_sort(in.begin(), in.end(), byX);
    std::stable_sort(out.begin(), out.end(), byX);
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
    void attached() override { child_->parent = patch; }
    void loadbang() override { child_->loadbangAll(); }

    // Ports follow the inner inlet/outlet boxes, ordered by x like Max.
    void syncPorts() override {
        std::vector<InletBox*> in;
        std::vector<OutletBox*> out;
        collect(in, out);
        std::vector<Port> ip, op;
        for (size_t k = 0; k < in.size(); ++k) {
            in[k]->index = (int)k;
            ip.push_back(in[k]->isSig() ? P::Signal : P::Control);
        }
        for (size_t k = 0; k < out.size(); ++k) {
            out[k]->index = (int)k;
            op.push_back(out[k]->isSig() ? P::Signal : P::Control);
        }
        if (ip != ins || op != outs) setPorts(ip, op);
    }

    void message(int in, const Message& m) override {
        if (in < 0 || in >= numIns()) return;
        if (ins[in] == P::Signal && m[0].isFloat()) setScalar(in, m[0].f);
        for (auto& b : child_->boxes)
            if (auto ib = dynamic_cast<InletBox*>(b.get()))
                if (ib->index == in && !ib->isSig()) ib->receive(m);
    }

    void dsp(int n) override {
        for (int k = 0; k < numOuts(); ++k)
            if (outs[k] == P::Signal) dsp::fill(sigOut(k), 0.f, n);
        child_->runChain(n);
    }

protected:
    void collect(std::vector<InletBox*>& in, std::vector<OutletBox*>& out) {
        collectPorts(*child_, in, out);
    }
    std::unique_ptr<Patch> child_;
};

int gInstances = 1000;      // #0 values
int gAbsDepth = 0;          // guards against an abstraction containing itself

bool loadAbstractionInto(Patch& child, const std::string& path, const Message& args,
                         std::string& err) {
    std::string src;
    if (!readFile(path, src)) { err = "can't read " + path; return false; }
    if (gAbsDepth > 16) { err = "abstractions nested too deeply (does one contain itself?)"; return false; }
    child.isAbstraction = true;
    child.file = path;
    child.absArgs = args;
    child.instance = ++gInstances;
    child.dir = path.substr(0, path.find_last_of('/'));
    ++gAbsDepth;
    bool ok = child.load(src, err);
    --gAbsDepth;
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
// written to fftout~ by overlap-add. Latency is fftsize samples.
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
        re_.resize((size_t)N_);
        im_.resize((size_t)N_);
        std::string path = findAbstraction(name_, context);
        std::string err;
        if (path.empty()) child_->print("pfft~: can't find " + name_ + ".mlsp");
        else if (!loadAbstractionInto(*child_, path, tailOf(a, 3), err) || !err.empty())
            child_->print(path + ": " + err);
        syncPorts();
    }

    void syncPorts() override;

    // spectral I/O for fftin~ / fftout~ (audio thread)
    const float* inRe(int k) const { return k < nIn_ ? sre_[(size_t)k].data() : nullptr; }
    const float* inIm(int k) const { return k < nIn_ ? sim_[(size_t)k].data() : nullptr; }
    float* outRe(int k) { return k < nOut_ ? ore_[(size_t)k].data() : nullptr; }
    float* outIm(int k) { return k < nOut_ ? oim_[(size_t)k].data() : nullptr; }
    int bins() const { return bins_; }

    void dsp(int n) override {
        std::vector<const float*> in((size_t)nIn_);
        for (int k = 0; k < nIn_; ++k) in[(size_t)k] = sigIn(k, n);
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < nIn_; ++k) ring_[(size_t)k][(size_t)w_] = in[(size_t)k][i];
            for (int j = 0; j < nOut_; ++j) {
                float& s = acc_[(size_t)j][(size_t)w_];
                sigOut(j)[i] = s;
                s = 0.f;
            }
            w_ = (w_ + 1) % N_;
            if (++count_ >= hop_) {
                count_ = 0;
                frame();
            }
        }
    }

private:
    void frame() {
        for (int k = 0; k < nIn_; ++k) {
            const float* r = ring_[(size_t)k].data();
            for (int t = 0; t < N_; ++t) {               // oldest sample first
                re_[(size_t)t] = r[(w_ + t) % N_] * win_[(size_t)t];
                im_[(size_t)t] = 0.f;
            }
            fft_.forward(re_.data(), im_.data());
            std::copy_n(re_.begin(), bins_, sre_[(size_t)k].begin());
            std::copy_n(im_.begin(), bins_, sim_[(size_t)k].begin());
        }
        for (int j = 0; j < nOut_; ++j) {
            std::fill(ore_[(size_t)j].begin(), ore_[(size_t)j].end(), 0.f);
            std::fill(oim_[(size_t)j].begin(), oim_[(size_t)j].end(), 0.f);
        }
        child_->runChain(bins_);                         // the whole frame is one vector
        for (int j = 0; j < nOut_; ++j) {
            const float* r = ore_[(size_t)j].data();
            const float* m = oim_[(size_t)j].data();
            re_[0] = r[0];
            im_[0] = 0.f;
            for (int b = 1; b < bins_; ++b) {             // a real signal's spectrum
                re_[(size_t)b] = r[b];
                im_[(size_t)b] = m[b];
                re_[(size_t)(N_ - b)] = r[b];
                im_[(size_t)(N_ - b)] = -m[b];
            }
            re_[(size_t)bins_] = 0.f;
            im_[(size_t)bins_] = 0.f;
            fft_.inverse(re_.data(), im_.data());
            float* a = acc_[(size_t)j].data();
            for (int t = 0; t < N_; ++t) a[(w_ + t) % N_] += re_[(size_t)t] * win_[(size_t)t] * scale_;
        }
    }

    std::string name_;
    int N_ = 512, hop_ = 128, bins_ = 256;
    int nIn_ = 0, nOut_ = 0;
    int w_ = 0, count_ = 0;
    float scale_ = 1.f;
    dsp::FFT fft_;
    std::vector<float> win_, re_, im_;
    std::vector<std::vector<float>> ring_, acc_, sre_, sim_, ore_, oim_;
};

// fftin~ n: outlets real, imaginary, bin index of pfft~ signal inlet n (1-based)
class FftinBox : public Box {
public:
    explicit FftinBox(const Message& a) : n_(std::max(1, (int)argf(a, 0, 1.f))) {
        setPorts({}, {P::Signal, P::Signal, P::Signal});
    }
    int number() const { return n_; }
    void resolve() override { pf_ = findPfft(patch); }
    void dsp(int n) override {
        const float* re = pf_ ? pf_->inRe(n_ - 1) : nullptr;
        const float* im = pf_ ? pf_->inIm(n_ - 1) : nullptr;
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
    PfftBox* pf_ = nullptr;
};

// fftout~ n: inlets real, imaginary for pfft~ signal outlet n (1-based)
class FftoutBox : public Box {
public:
    explicit FftoutBox(const Message& a) : n_(std::max(1, (int)argf(a, 0, 1.f))) {
        setPorts({P::Signal, P::Signal}, {});
    }
    int number() const { return n_; }
    void resolve() override { pf_ = FftinBox::findPfft(patch); }
    void dsp(int n) override {
        const Patch::Spectral* sp = patch->spectralContext();
        float* re = pf_ ? pf_->outRe(n_ - 1) : nullptr;
        float* im = pf_ ? pf_->outIm(n_ - 1) : nullptr;
        if (!re || !sp) return;
        const float* xr = sigIn(0, n);
        const float* xi = sigIn(1, n);
        for (int i = 0; i < n; ++i) {
            re[i] += xr[i];
            im[i] += xi[i];
        }
    }

private:
    int n_;
    PfftBox* pf_ = nullptr;
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
        b->index = b->isSig() ? -1 : (int)ip.size();      // inlet~ has no meaning here
        if (!b->isSig()) ip.push_back(P::Control);
    }
    for (auto* b : out) {
        b->index = b->isSig() ? -1 : (int)op.size();
        if (!b->isSig()) op.push_back(P::Control);
    }
    if (ip != ins || op != outs) setPorts(ip, op);
    if (nin != nIn_ || nout != nOut_) {
        nIn_ = nin;
        nOut_ = nout;
        ring_.assign((size_t)nIn_, std::vector<float>((size_t)N_, 0.f));
        sre_.assign((size_t)nIn_, std::vector<float>((size_t)bins_, 0.f));
        sim_ = sre_;
        acc_.assign((size_t)nOut_, std::vector<float>((size_t)N_, 0.f));
        ore_.assign((size_t)nOut_, std::vector<float>((size_t)bins_, 0.f));
        oim_ = ore_;
    }
}

// ================================================================ poly~

// poly~ name voices [args...]: `voices` copies of the abstraction name.mlsp
// (args become #1.. in each). Ports come from the inlets/outlets inside
// (in, in~, out, out~ are the same objects); signal outlets of the voices
// are summed. Messages to a control inlet:
//   note ...        the rest goes to a free voice (or steals the oldest)
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
    };

    PolyBox(const Message& a, Patch* context) : name_(argstr(a, 0)) {
        int n = std::clamp((int)argf(a, 1, 1.f), 1, 128);
        std::string path = findAbstraction(name_, context);
        if (path.empty() && context) context->print("poly~: can't find " + name_ + ".mlsp");
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
    void attached() override { for (auto& v : voices_) v->patch->parent = patch; }
    void loadbang() override { for (auto& v : voices_) v->patch->loadbangAll(); }

    void syncPorts() override {
        std::vector<Port> ip, op;
        for (size_t k = 0; k < voices_.size(); ++k) {
            std::vector<InletBox*> in;
            std::vector<OutletBox*> out;
            collectPorts(*voices_[k]->patch, in, out);
            for (size_t j = 0; j < in.size(); ++j) {
                in[j]->index = (int)j;
                if (k == 0) ip.push_back(in[j]->isSig() ? P::Signal : P::Control);
            }
            for (size_t j = 0; j < out.size(); ++j) {
                out[j]->index = (int)j;
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
            x.loud.store(false);
            x.busy.store(true);
            deliver(v, in, tailOf(m));
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
        for (int k = 0; k < numOuts(); ++k)
            if (outs[k] == P::Signal) dsp::fill(sigOut(k), 0.f, n);
        for (auto& v : voices_)
            if (!v->muted.load(std::memory_order_relaxed)) v->patch->runChain(n);
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
        int best = -1;
        for (int k = 0; k < (int)voices_.size(); ++k)       // the free voice idle longest
            if (!voices_[(size_t)k]->busy.load() &&
                (best < 0 || voices_[(size_t)k]->used < voices_[(size_t)best]->used))
                best = k;
        if (best >= 0) return best;
        best = 0;                                           // all busy: steal the oldest note
        for (int k = 1; k < (int)voices_.size(); ++k)
            if (voices_[(size_t)k]->used < voices_[(size_t)best]->used) best = k;
        return best;
    }
    void deliver(int v, int in, const Message& m) {
        if (m.empty()) return;
        for (auto& b : voices_[(size_t)v]->patch->boxes)
            if (auto ib = dynamic_cast<InletBox*>(b.get()))
                if (ib->index == in && !ib->isSig()) ib->receive(m);
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
        poly_ = nullptr;
        for (Patch* p = patch; p && !poly_; p = p->parent) poly_ = dynamic_cast<PolyBox*>(p->owner);
        voice_ = poly_ ? poly_->voiceOf(patch) : -1;
    }
    void loadbang() override { outletFloat(0, (float)(voice_ + 1)); }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) { outletFloat(0, (float)(voice_ + 1)); return; }
        PolyBox::Voice* v = poly_ ? poly_->voice(voice_) : nullptr;
        if (!v) return;
        if (m[0].isFloat()) v->busy.store(m[0].f != 0.f);
        else if (m[0].is("mute")) v->muted.store(argf(m, 1, 1.f) != 0.f);
    }
    void dsp(int n) override {
        PolyBox::Voice* v = poly_ ? poly_->voice(voice_) : nullptr;
        if (!v || inSrc[0].empty()) return;
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
    PolyBox* poly_ = nullptr;
    int voice_ = -1;
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
protected:
    float& slot(int i) {
        const Patch::Spectral* sp = patch->spectralContext();
        int size = sp ? sp->bins : 1;
        if ((int)mem_.size() != size) mem_.assign((size_t)size, 0.f);
        return mem_[(size_t)(sp ? i % size : 0)];
    }
    std::vector<float> mem_;
};

// framedelta~: difference from the previous frame (phase -> frequency)
class FramedeltaBox : public FrameMemory {
public:
    FramedeltaBox() { setPorts({P::Signal}, {P::Signal}); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) {
            float& m = slot(i);
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
        for (int i = 0; i < n; ++i) {
            float& m = slot(i);
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
    void dsp(int n) override {
        const float *a = sigIn(0, n), *ph = sigIn(1, n), *d = sigIn(2, n), *rs = sigIn(3, n);
        float* y = sigOut(0);
        if ((int)prev_.size() != n) prev_.assign((size_t)n, 0.f);
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
    std::vector<float> prev_;
    std::vector<int> peaks_;
};

// lrshift~ n: shifts the signal vector n samples (positive = right); inside
// pfft~ it moves each bin's value to the bin n above, so bins can be
// combined with their neighbours. Samples shifted in are 0.
class LrshiftBox : public Box {
public:
    explicit LrshiftBox(const Message& a) : n_((int)argf(a, 0, 0.f)) {
        setPorts({P::Signal}, {P::Signal});
    }
    void message(int, const Message& m) override {
        if (m[0].isFloat()) setScalar(0, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) {
            int j = i - n_;
            y[i] = j >= 0 && j < n ? x[j] : 0.f;
        }
    }

private:
    int n_;
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
// which take signals or floats.
class ExprSigBox : public Box {
public:
    explicit ExprSigBox(const Message& a) : src_(messageToString(a)) {
        int nv = 0;
        ok_ = ExprParser(src_).parse(fn_, nv, err_);
        nv_ = std::max(1, nv);
        setPorts(std::vector<Port>((size_t)nv_, P::Signal), {P::Signal});
    }
    Message saveArgs() const override { return {Atom(src_)}; }
    std::string text() const override { return "expr~ " + src_; }
    void attached() override {
        if (!ok_) patch->print("expr~: " + err_ + " in \"" + src_ + "\"");
    }
    void message(int in, const Message& m) override {
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        float* y = sigOut(0);
        if (!ok_) { dsp::fill(y, 0.f, n); return; }
        const float* in[9];
        for (int k = 0; k < nv_; ++k) in[k] = sigIn(k, n);
        double v[9] = {0};
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < nv_; ++k) v[k] = in[k][i];
            double r = fn_(v);
            y[i] = std::isfinite(r) ? (float)r : 0.f;
        }
    }

private:
    std::string src_, err_;
    bool ok_ = false;
    int nv_ = 1;
    ExprParser::Fn fn_;
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
// number from that value, stop stops it. It wraps to min at max when loop
// is 1 (default) and otherwise stops there. Inside pfft~ it restarts at a
// frame boundary, so the count stays aligned to bins.
class CountBox : public Box {
public:
    explicit CountBox(const Message& a) {
        setPorts({P::Control}, {P::Signal});
        min_ = (long)argf(a, 0, 0.f);
        max_ = (long)argf(a, 1, 0.f);
        loop_ = argf(a, 2, 1.f) != 0.f;
        v_ = min_;
    }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) startAt_.store((double)min_);
        else if (m[0].isFloat()) startAt_.store(m[0].f);
        else if (m[0].is("stop")) stop_.store(true);
    }
    void dsp(int n) override {
        double s = startAt_.exchange(-1e30);    // inside pfft~ a call is a whole frame
        if (s > -1e29) { v_ = (long)s; running_ = true; }
        if (stop_.exchange(false)) running_ = false;
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) {
            y[i] = (float)v_;
            if (!running_) continue;
            ++v_;
            if (max_ > min_ && v_ >= max_) {
                if (loop_) v_ = min_;
                else { v_ = max_; running_ = false; }
            }
        }
    }

private:
    long min_, max_, v_;
    bool loop_, running_ = false;
    std::atomic<double> startAt_{-1e30};
    std::atomic<bool> stop_{false};
};

// index~ name [channel]: reads a buffer~ at a sample index (no interpolation)
class IndexBox : public Box {
public:
    explicit IndexBox(const Message& a) : name_(argstr(a, 0)), ch_((int)argf(a, 1, 1.f) - 1) {
        setPorts({P::Signal}, {P::Signal});
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
        else if (m[0].isFloat()) setScalar(0, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) y[i] = b_ ? b_->buf.at((long)x[i], std::max(0, ch_)) : 0.f;
    }

private:
    std::string name_;
    int ch_;
    BufferBox* b_ = nullptr;
};

// poke~ name [channel]: writes the left signal into a buffer~ at the sample
// index given by the right signal; indices outside the buffer are ignored
class PokeBox : public Box {
public:
    explicit PokeBox(const Message& a) : name_(argstr(a, 0)), ch_((int)argf(a, 1, 1.f) - 1) {
        setPorts({P::Signal, P::Signal}, {});
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int in, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
        else if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        if (!b_) return;
        SampleBuffer& b = b_->buf;
        const float *x = sigIn(0, n), *idx = sigIn(1, n);
        int c = std::clamp(ch_, 0, b.channels - 1);
        for (int i = 0; i < n; ++i) {
            long k = (long)idx[i];
            if (k >= 0 && k < b.frames) b.data[(size_t)k * b.channels + c] = x[i];
        }
    }

private:
    std::string name_;
    int ch_;
    BufferBox* b_ = nullptr;
};

}  // namespace

std::unique_ptr<Box> makeAbstraction(const std::string& path, const Message& args, Patch* context) {
    return std::make_unique<AbstractionBox>(path, args, context);
}

void registerPatcherObjects(ClassTable& t) {
    using M = const Message&;
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

}  // namespace mlsp
