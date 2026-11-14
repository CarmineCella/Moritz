// objects_patcher.cpp — patches inside patches and spectral processing
//
//   p name            subpatcher, saved inline in the parent's file
//   <name>            abstraction: any name.mlsp next to the patch; its
//                     arguments replace #1..#9 inside, #0 is unique per copy
//   inlet, inlet~     a subpatcher's inlets, ordered left to right
//   outlet, outlet~   its outlets
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
    template <class In, class Out>
    void collectAs(std::vector<In*>& in, std::vector<Out*>& out) {
        for (auto& b : child_->boxes) {
            if (auto i = dynamic_cast<In*>(b.get())) in.push_back(i);
            if (auto o = dynamic_cast<Out*>(b.get())) out.push_back(o);
        }
        auto byX = [](const Box* a, const Box* b) { return a->x < b->x; };
        std::stable_sort(in.begin(), in.end(), byX);
        std::stable_sort(out.begin(), out.end(), byX);
    }
    void collect(std::vector<InletBox*>& in, std::vector<OutletBox*>& out) {
        collectAs(in, out);
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
        for (int pos = 0; pos < bins_; pos += dsp::kBlock) {
            child_->spectral.framePos = pos;
            child_->runChain(std::min(dsp::kBlock, bins_ - pos));
        }
        child_->spectral.framePos = 0;
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
        const Patch::Spectral* sp = patch->spectralContext();
        int pos = sp ? sp->framePos : 0;
        const float* re = pf_ ? pf_->inRe(n_ - 1) : nullptr;
        const float* im = pf_ ? pf_->inIm(n_ - 1) : nullptr;
        float *yr = sigOut(0), *yi = sigOut(1), *yb = sigOut(2);
        for (int i = 0; i < n; ++i) {
            yr[i] = re ? re[pos + i] : 0.f;
            yi[i] = im ? im[pos + i] : 0.f;
            yb[i] = (float)(pos + i);
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
            re[sp->framePos + i] += xr[i];
            im[sp->framePos + i] += xi[i];
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
        return mem_[(size_t)(sp ? (sp->framePos + i) % size : 0)];
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
// in -pi..pi (phase accumulation for a phase vocoder)
class FrameaccumBox : public FrameMemory {
public:
    explicit FrameaccumBox(const Message& a) : wrap_(argf(a, 0, 0.f) != 0.f) {
        setPorts({P::Signal}, {P::Signal});
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) {
            float& m = slot(i);
            m += x[i];
            if (wrap_) m = dsp::wrapPhase(m);
            y[i] = m;
        }
    }

private:
    bool wrap_;
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
// is 1 (default) and otherwise stops there. Inside pfft~ a restart waits
// for the start of the next frame so the count stays aligned to bins.
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
        const Patch::Spectral* sp = patch->spectralContext();
        if (!sp || sp->framePos == 0) {
            double s = startAt_.exchange(-1e30);
            if (s > -1e29) { v_ = (long)s; running_ = true; }
        }
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
        {"pfft~", "Spectral", "FFT patcher: name fftsize overlap", [](M a, Patch* c) { return std::make_unique<PfftBox>(a, c); }},
        {"fftin~", "Spectral", "pfft~ input: real imag bin", with<FftinBox>()},
        {"fftout~", "Spectral", "pfft~ output: real imag", with<FftoutBox>()},
        {"cartopol~", "Spectral", "real/imag to amplitude/phase", plain<CartopolBox>()},
        {"poltocar~", "Spectral", "amplitude/phase to real/imag", plain<PoltocarBox>()},
        {"framedelta~", "Spectral", "difference from the previous frame", plain<FramedeltaBox>()},
        {"frameaccum~", "Spectral", "running sum per bin [wrap]", with<FrameaccumBox>()},
        {"phasewrap~", "Spectral", "wrap a phase to -pi..pi", plain<PhasewrapBox>()},
        {"expr~", "Signal Math", "signal expression: $v1 * 2", with<ExprSigBox>()},
        {"sah~", "Signal Math", "sample and hold on a rising trigger", with<SahBox>()},
        {"count~", "Samples", "sample counter: min max [loop]", with<CountBox>()},
        {"index~", "Samples", "read a buffer~ at a sample index", with<IndexBox>()},
        {"poke~", "Samples", "write a buffer~ at a sample index", with<PokeBox>()},
    });
}

}  // namespace mlsp
