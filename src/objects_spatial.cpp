// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_spatial.cpp — ambisonics as in Musil: ambienc~ (encoder), ambirot~
// (rotation), ambidec~ (to speakers) and binaural~ (to headphones); and
// hrtf~, a direct binaural panner on the measured head responses
//
// Conventions (AmbiX): azimuth in degrees, 0 in front, positive to the LEFT;
// elevation in degrees, positive up. A B-format signal of order N has
// (N+1)^2 channels, in ACN order with SN3D normalisation: for order 1, W Y Z X.

#include "objects.h"
#include "hrtf_kemar.h"
#include "signals/ambisonics.h"

#include <map>
#include <memory>
#include <mutex>

#include <algorithm>
#include <cmath>

namespace moritz {
namespace {

constexpr int kMaxOrder = 5;

int channelsOf(int order) { return (order + 1) * (order + 1); }

double rad(double d) { return d * dsp::kPi / 180.0; }

// the spherical harmonics (ACN, SN3D) and the decoders are Musil's (musil/src/signals/ambisonics.h)
void shGains(int order, double az, double el, float* g) {
    auto v = musil::dsp::sh_gains(order, az, el);
    for (size_t k = 0; k < v.size(); ++k) g[k] = (float)v[k];
}

}  // namespace

// The same gains as Musil's sh_gains, into g ((order+1)^2 values) without allocating: for the
// audio thread (ambienc~ follows moving sources every block). Musil's returns a std::vector;
// the engine tests compare the two (shGainsLive against musil::dsp::sh_gains).
void shGainsLive(int order, double az, double el, float* g) {
    const double pi = 3.14159265358979323846;
    double a = az * pi / 180.0, e = el * pi / 180.0;
    double x = std::cos(e) * std::cos(a), y = std::cos(e) * std::sin(a), z = std::sin(e);
    double r = std::sqrt(x * x + y * y);
    double fact[2 * kMaxOrder + 2];
    fact[0] = 1;
    for (int k = 1; k < 2 * kMaxOrder + 2; ++k) fact[k] = fact[k - 1] * k;
    for (int n = 0; n <= order; n++)
        for (int m = -n; m <= n; m++) {
            int am = std::abs(m);
            double pmm = 1;
            for (int k = 1; k <= am; k++) pmm *= (2 * k - 1) * r;
            double p = pmm;
            if (n != am) {
                double p1 = z * (2 * am + 1) * pmm;
                if (n == am + 1) p = p1;
                else {
                    double pk2 = pmm, pk1 = p1;
                    for (int k = am + 2; k <= n; k++) {
                        p = ((2 * k - 1) * z * pk1 - (k + am - 1) * pk2) / (k - am);
                        pk2 = pk1;
                        pk1 = p;
                    }
                }
            }
            double norm = std::sqrt((m == 0 ? 1.0 : 2.0) * fact[n - am] / fact[n + am]);
            double azf = m > 0 ? std::cos(m * a) : m < 0 ? std::sin(am * a) : 1.0;
            g[n * n + n + m] = (float)(norm * p * azf);
        }
}

namespace {

int orderArg(const Message& a, size_t i, float def) { return std::clamp((int)argf(a, i, def), 0, kMaxOrder); }

// ---- the KEMAR set at a sample rate, shared by every hrtf~ and binaural~

std::vector<float> resampleIr(const float* h, int n, double f, int T) {
    std::vector<float> out((size_t)T, 0.f);
    if (std::fabs(f - 1.0) < 1e-6) {
        for (int t = 0; t < T && t < n; ++t) out[(size_t)t] = h[t];
        return out;
    }
    double fc = std::min(1.0, f);                      // band limit when going down
    const int half = 16;
    for (int t = 0; t < T; ++t) {
        double c = t / f, acc = 0;
        for (int i = (int)c - half; i <= (int)c + half; ++i) {
            if (i < 0 || i >= n) continue;
            double x = (c - i) * fc;
            double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(dsp::kPi * x) / (dsp::kPi * x);
            double win = 0.5 + 0.5 * std::cos(dsp::kPi * (c - i) / (half + 1));
            acc += h[i] * fc * sinc * win;
        }
        out[(size_t)t] = (float)acc;
    }
    return out;
}

struct HrirSet {
    int taps = 0;
    std::vector<float> left, right;                    // [direction * taps + t]
    struct Ring { float el; int first, count; };
    std::vector<Ring> rings;                           // by elevation; each ring by azimuth
    std::vector<kemar::Direction> dirs;                // the measured directions (az, el; their responses at 44.1 kHz)
    const float* L(int d) const { return &left[(size_t)d * taps]; }
    const float* R(int d) const { return &right[(size_t)d * taps]; }
    int nearest(double az, double el) const {
        double a = rad(az), e = rad(el), x = std::cos(e) * std::cos(a), y = std::cos(e) * std::sin(a), z = std::sin(e);
        int best = 0;
        double bd = -2;
        for (int d = 0; d < (int)dirs.size(); ++d) {
            double a2 = rad(dirs[(size_t)d].az), e2 = rad(dirs[(size_t)d].el);
            double dot = x * std::cos(e2) * std::cos(a2) + y * std::cos(e2) * std::sin(a2) + z * std::sin(e2);
            if (dot > bd) { bd = dot; best = d; }
        }
        return best;
    }
    // the measured directions around (az, el) and their weights: two
    // azimuths on each of the two elevation rings around it
    int neighbours(double az, double el, int idx[4], float w[4]) const {
        az = std::remainder(az, 360.0);
        el = std::clamp(el, (double)rings.front().el, (double)rings.back().el);
        size_t r = 0;
        while (r + 1 < rings.size() && rings[r + 1].el <= el) ++r;
        size_t r2 = std::min(r + 1, rings.size() - 1);
        double te = r2 == r ? 0.0 : (el - rings[r].el) / (rings[r2].el - rings[r].el);
        int n = 0;
        for (int k = 0; k < 2; ++k) {
            const Ring& ring = rings[k == 0 ? r : r2];
            double wr = k == 0 ? 1.0 - te : te;
            if (wr <= 0) continue;
            if (ring.count == 1) { idx[n] = ring.first; w[n++] = (float)wr; continue; }
            // the azimuths around az on this ring (sorted, wrapping)
            int j = 0;
            while (j < ring.count && dirs[(size_t)(ring.first + j)].az <= az) ++j;
            int lo = (j - 1 + ring.count) % ring.count, hi = j % ring.count;
            double a0 = dirs[(size_t)(ring.first + lo)].az, a1 = dirs[(size_t)(ring.first + hi)].az;
            double span = std::remainder(a1 - a0, 360.0);
            if (span <= 0) span += 360.0;
            double ta = std::remainder(az - a0, 360.0);
            if (ta < 0) ta += 360.0;
            ta = std::clamp(ta / span, 0.0, 1.0);
            idx[n] = ring.first + lo; w[n++] = (float)(wr * (1 - ta));
            idx[n] = ring.first + hi; w[n++] = (float)(wr * ta);
        }
        return n;
    }
};

// the set resampled to sr, normalised so that the front direction has unit
// energy at each ear (computed once per rate, kept for the program's life)
const HrirSet& hrirsAt(double sr) {
    static std::mutex m;
    static std::map<long, std::unique_ptr<HrirSet>> cache;
    std::lock_guard<std::mutex> lk(m);
    auto& slot = cache[std::lround(sr)];
    if (slot) return *slot;
    auto set = std::make_unique<HrirSet>();
    double f = sr / kemar::kRate;
    int T = std::max(1, (int)std::ceil(kemar::kTaps * f));
    set->taps = T;
    set->dirs = kemar::directions();
    if (set->dirs.empty()) set->dirs.push_back(kemar::Direction{});   // no data: one silent direction
    const int D = (int)set->dirs.size();
    set->left.resize((size_t)D * T);
    set->right.resize((size_t)D * T);
    for (int d = 0; d < D; ++d) {
        const kemar::Direction& k = set->dirs[(size_t)d];
        auto l = resampleIr(k.left, kemar::kTaps, f, T), r = resampleIr(k.right, kemar::kTaps, f, T);
        std::copy(l.begin(), l.end(), set->left.begin() + (size_t)d * T);
        std::copy(r.begin(), r.end(), set->right.begin() + (size_t)d * T);
        if (set->rings.empty() || set->rings.back().el != k.el)
            set->rings.push_back({k.el, d, 0});
        ++set->rings.back().count;
    }
    int front = set->nearest(0, 0);
    double e = 0;
    for (int t = 0; t < T; ++t) e += set->L(front)[t] * set->L(front)[t] + set->R(front)[t] * set->R(front)[t];
    float g = (float)(1.0 / std::sqrt(std::max(e / 2, 1e-12)));
    for (auto& v : set->left) v *= g;
    for (auto& v : set->right) v *= g;
    slot = std::move(set);
    return *slot;
}

// ---------------------------------------------------------------- ambienc~
// ambienc~ [order] [azimuth] [elevation]: places a mono signal at a
// direction, as a B-format sound field of (order+1)^2 channels (order 1 by
// default: W Y Z X). Inlets: the signal, the azimuth and the elevation in
// degrees (numbers or signals; moving sources glide, the gains are
// interpolated over each block).
class AmbiencBox : public Box {
public:
    explicit AmbiencBox(const Message& a) : order_(std::max(1, orderArg(a, 0, 1.f))) {
        nch_ = channelsOf(order_);
        setPorts({P::Signal, P::Signal, P::Signal}, std::vector<Port>((size_t)nch_, P::Signal));
        setScalar(1, argf(a, 1, 0.f));
        setScalar(2, argf(a, 2, 0.f));
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *az = sigIn(1, n), *el = sigIn(2, n);
        float g1[36];
        shGainsLive(order_, az[n - 1], el[n - 1], g1);       // nothing allocated on the audio thread
        if (!started_) { std::copy(g1, g1 + nch_, g0_); started_ = true; }
        for (int k = 0; k < nch_; ++k) {
            float* y = sigOut(k);
            float g = g0_[k], dg = (g1[k] - g0_[k]) / n;
            for (int i = 0; i < n; ++i) { g += dg; y[i] = x[i] * g; }
        }
        std::copy(g1, g1 + nch_, g0_);
    }

private:
    int order_, nch_;
    float g0_[36] = {};
    bool started_ = false;
};

// ---------------------------------------------------------------- ambirot~
// ambirot~ [order]: turns a sound field about the vertical axis by the yaw
// in the last inlet (degrees, positive to the left): within each order the
// pair of channels m, -m rotates by m times the yaw, exactly. To follow a
// listener's head, rotate by minus the head's yaw.
class AmbirotBox : public Box {
public:
    explicit AmbirotBox(const Message& a) : order_(std::max(1, orderArg(a, 0, 1.f))) {
        nch_ = channelsOf(order_);
        std::vector<Port> ins((size_t)nch_, P::Signal);
        ins.push_back(P::Signal);
        setPorts(ins, std::vector<Port>((size_t)nch_, P::Signal));
        setScalar(nch_, argf(a, 1, 0.f));
    }
    void message(int in, const Message& m) override {
        if (in == nch_ && m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        double yaw = rad(sigIn(nch_, n)[0]);
        for (int l = 0; l <= order_; ++l) {
            int c0 = l * l + l;
            const float* x0 = sigIn(c0, n);
            std::copy(x0, x0 + n, sigOut(c0));
            for (int m = 1; m <= l; ++m) {
                const float *cp = sigIn(c0 + m, n), *cm = sigIn(c0 - m, n);
                float *rp = sigOut(c0 + m), *rm = sigOut(c0 - m);
                float c = (float)std::cos(m * yaw), s = (float)std::sin(m * yaw);
                for (int i = 0; i < n; ++i) {
                    float p = cp[i], q = cm[i];
                    rp[i] = c * p - s * q;
                    rm[i] = s * p + c * q;
                }
            }
        }
    }

private:
    int order_, nch_;
};

// ---------------------------------------------------------------- ambidec~
// ambidec~ order azimuth ...: decodes a sound field to loudspeakers on a
// horizontal ring, one outlet per azimuth (ambidec~ 1 45 135 -135 -45 is a
// square: front left, back left, back right, front right). A ring hears the
// horizontal (sectoral) channels only: for an evenly spaced ring of at least
// 2N+1 speakers this is the exact (mode-matching) decoder, with 2D max-rE
// weights: Musil's ambi-decode on a ring.
class AmbidecBox : public Box {
public:
    explicit AmbidecBox(const Message& a) : order_(std::max(1, orderArg(a, 0, 1.f))) {
        nch_ = channelsOf(order_);
        for (size_t k = 1; k < a.size(); ++k)
            if (a[k].isFloat()) az_.push_back(a[k].f);
        if (az_.empty()) az_ = {45.f, 135.f, -135.f, -45.f};
        std::vector<std::pair<double, double>> ring;
        for (float az : az_) ring.push_back({az, 0.0});
        gains_.clear();
        for (auto& row : musil::dsp::ambi_decoder(order_, ring))     // Musil's ambi-decode on a ring
            for (double g : row) gains_.push_back((float)g);
        setPorts(std::vector<Port>((size_t)nch_, P::Signal), std::vector<Port>(az_.size(), P::Signal));
    }
    void dsp(int n) override {
        for (size_t s = 0; s < az_.size(); ++s) {
            float* y = sigOut((int)s);
            std::fill(y, y + n, 0.f);
            for (int k = 0; k < nch_; ++k) {
                float g = gains_[s * (size_t)nch_ + (size_t)k];
                if (g == 0.f) continue;
                const float* x = sigIn(k, n);
                for (int i = 0; i < n; ++i) y[i] += g * x[i];
            }
        }
    }

private:
    int order_, nch_;
    std::vector<float> az_, gains_;
};

// ---------------------------------------------------------------- binaural~
// binaural~ [order]: a sound field for headphones, as Musil's
// binaural-decode. The field is decoded to 17 virtual speakers spread over
// the sphere (8 at ear height, 4 above, 4 below, 1 overhead) and each is
// heard through the measured head-related impulse responses of its
// direction (MIT KEMAR). The decoder (Musil's ambi-decode) samples the field
// at each speaker with the SN3D normalisation (2n+1) and 3D max-rE weights.
// All of it is folded into one filter pair per channel, so the cost does not
// depend on the sources. A source in front plays at the level of the front
// head response.
class BinauralBox : public Box {
public:
    explicit BinauralBox(const Message& a) : order_(std::max(1, orderArg(a, 0, 1.f))) {
        nch_ = channelsOf(order_);
        setPorts(std::vector<Port>((size_t)nch_, P::Signal), {P::Signal, P::Signal});
    }
    // prepared at compile (on the GUI thread) for the rate: again only when it changed
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (sr > 0 && std::fabs(sr - builtSr_) > 0.5) build(sr);
    }
    void dsp(int n) override {
        float *L = sigOut(0), *R = sigOut(1);
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
        const int T = taps_;
        // not prepared for this rate (the compile after a rate change is coming): silence, never
        // the preparation here
        if (T == 0 || std::fabs(patch->root()->sampleRate.load() - builtSr_) > 0.5) return;
        for (int k = 0; k < nch_; ++k) {
            const float* x = sigIn(k, n);
            float* h = &hist_[(size_t)k * 2 * T];           // the last T inputs, twice (no wrapping in the loop)
            const float* fl = &left_[(size_t)k * T];
            const float* fr = &right_[(size_t)k * T];
            int& p = pos_[(size_t)k];
            for (int i = 0; i < n; ++i) {
                p = (p + T - 1) % T;
                h[p] = h[p + T] = x[i];
                const float* hp = h + p;                    // hp[j] = x[i - j]
                float sl = 0.f, sr2 = 0.f;
                for (int j = 0; j < T; ++j) { sl += fl[j] * hp[j]; sr2 += fr[j] * hp[j]; }
                L[i] += sl;
                R[i] += sr2;
            }
        }
    }

private:
    // the HRIRs at rate sr (windowed-sinc resampling), folded with the decoder
    void build(double sr) {
        if (sr <= 0) return;
        builtSr_ = sr;
        const HrirSet& set = hrirsAt(sr);
        const int T = set.taps;
        // 17 virtual speakers: 8 at ear height, 4 above, 4 below, 1 overhead
        static const float kSpeakers[][2] = {{0, 0}, {45, 0}, {90, 0}, {135, 0}, {180, 0}, {-135, 0}, {-90, 0},
                                             {-45, 0}, {45, 40}, {135, 40}, {-135, 40}, {-45, 40}, {0, -30},
                                             {90, -30}, {180, -30}, {-90, -30}, {0, 90}};
        const int ns = (int)(sizeof kSpeakers / sizeof kSpeakers[0]);
        std::vector<std::vector<float>> hl((size_t)ns), hr((size_t)ns);
        for (int d = 0; d < ns; ++d) {
            int k = set.nearest(kSpeakers[d][0], kSpeakers[d][1]);
            hl[(size_t)d].assign(set.L(k), set.L(k) + T);
            hr[(size_t)d].assign(set.R(k), set.R(k) + T);
        }
        // Musil's sampling decoder for these speakers (SN3D corrected, 3D max-rE weights)
        std::vector<std::pair<double, double>> dirs;
        for (int d = 0; d < ns; ++d) dirs.push_back({kSpeakers[d][0], kSpeakers[d][1]});
        auto M = musil::dsp::ambi_decoder(order_, dirs);
        std::vector<float> fl((size_t)nch_ * T, 0.f), fr((size_t)nch_ * T, 0.f);
        for (int d = 0; d < ns; ++d) {
            for (int k = 0; k < nch_; ++k) {
                float c = (float)M[(size_t)d][(size_t)k];
                for (int t = 0; t < T; ++t) {
                    fl[(size_t)k * T + t] += c * hl[(size_t)d][(size_t)t];
                    fr[(size_t)k * T + t] += c * hr[(size_t)d][(size_t)t];
                }
            }
        }
        // level: a source in front gets the energy of the front head response
        float src[36];
        shGains(order_, 0.0, 0.0, src);
        double ef = 0, eh = 0;
        for (int t = 0; t < T; ++t) {
            double l = 0, r = 0;
            for (int k = 0; k < nch_; ++k) { l += src[k] * fl[(size_t)k * T + t]; r += src[k] * fr[(size_t)k * T + t]; }
            ef += l * l + r * r;
            eh += hl[0][(size_t)t] * hl[0][(size_t)t] + hr[0][(size_t)t] * hr[0][(size_t)t];
        }
        float scale = (float)std::sqrt(eh / std::max(ef, 1e-12));
        for (auto& v : fl) v *= scale;
        for (auto& v : fr) v *= scale;
        taps_ = T;
        left_ = std::move(fl);
        right_ = std::move(fr);
        hist_.assign((size_t)nch_ * 2 * T, 0.f);
        pos_.assign((size_t)nch_, 0);
    }
    int order_, nch_, taps_ = 0;
    double builtSr_ = 0;
    std::vector<float> left_, right_, hist_;
    std::vector<int> pos_;
};


// ---------------------------------------------------------------- hrtf~
// hrtf~ [azimuth] [elevation]: a mono sound at a direction on headphones,
// through the measured head-related impulse responses themselves (all 710
// directions of the MIT KEMAR set): the four measured directions around the
// one asked for, interpolated. Without ambisonics in between, the spectral
// cues of the outer ear stay sharp, so elevation (up, down) is clearer than
// with binaural~. Inlets: the signal, the azimuth (0 front, 90 left, 180
// behind) and the elevation (-40 to 90) in degrees, numbers or signals.
// Outlets: left, right. A moving direction crossfades within each block.
class HrtfBox : public Box {
public:
    explicit HrtfBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal, P::Signal});
        setScalar(1, argf(a, 0, 0.f));
        setScalar(2, argf(a, 1, 0.f));
    }
    // prepared at compile (on the GUI thread) for the rate: again only when it changed
    void resolve() override {
        double sr = patch->root()->sampleRate.load();
        if (sr > 0 && (!set_ || std::fabs(sr - sr_) > 0.5)) prepare(sr);
    }
    void dsp(int n) override {
        float *L = sigOut(0), *R = sigOut(1);
        if (!set_ || std::fabs(patch->root()->sampleRate.load() - sr_) > 0.5) {   // never prepared here
            std::fill(L, L + n, 0.f);
            std::fill(R, R + n, 0.f);
            return;
        }
        const float* x = sigIn(0, n);
        float az = sigIn(1, n)[n - 1], el = sigIn(2, n)[n - 1];
        bool move = !have_ || std::fabs(az - az_) > 1e-3f || std::fabs(el - el_) > 1e-3f;
        if (move) {
            az_ = az;
            el_ = el;
            filter(az, el, nl_.data(), nr_.data());
            if (!have_) { cl_ = nl_; cr_ = nr_; have_ = true; move = false; }
        }
        const int T = set_->taps;
        for (int i = 0; i < n; ++i) {
            pos_ = (pos_ + T - 1) % T;
            hist_[(size_t)pos_] = hist_[(size_t)(pos_ + T)] = x[i];
            const float* hp = &hist_[(size_t)pos_];
            float l = 0.f, r = 0.f;
            for (int j = 0; j < T; ++j) { l += cl_[(size_t)j] * hp[j]; r += cr_[(size_t)j] * hp[j]; }
            if (move) {                                  // fade to the new direction over the block
                float l2 = 0.f, r2 = 0.f;
                for (int j = 0; j < T; ++j) { l2 += nl_[(size_t)j] * hp[j]; r2 += nr_[(size_t)j] * hp[j]; }
                float t = (float)(i + 1) / n;
                l += (l2 - l) * t;
                r += (r2 - r) * t;
            }
            L[i] = l;
            R[i] = r;
        }
        if (move) { cl_ = nl_; cr_ = nr_; }
    }

private:
    void prepare(double sr) {
        if (sr <= 0) return;
        set_ = &hrirsAt(sr);
        sr_ = sr;
        const size_t T = (size_t)set_->taps;
        hist_.assign(2 * T, 0.f);
        pos_ = 0;
        cl_.assign(T, 0.f); cr_.assign(T, 0.f); nl_.assign(T, 0.f); nr_.assign(T, 0.f);
        have_ = false;
    }
    void filter(double az, double el, float* l, float* r) const {
        int idx[4];
        float w[4];
        int k = set_->neighbours(az, el, idx, w);
        const int T = set_->taps;
        std::fill(l, l + T, 0.f);
        std::fill(r, r + T, 0.f);
        for (int m = 0; m < k; ++m) {
            const float *hl = set_->L(idx[m]), *hr = set_->R(idx[m]);
            for (int t = 0; t < T; ++t) { l[t] += w[m] * hl[t]; r[t] += w[m] * hr[t]; }
        }
    }
    const HrirSet* set_ = nullptr;
    double sr_ = 0;
    std::vector<float> hist_, cl_, cr_, nl_, nr_;
    int pos_ = 0;
    float az_ = 0.f, el_ = 0.f;
    bool have_ = false;
};

}  // namespace

// The head-related impulse responses of a direction (the measured ones
// around it, weighted), at a sample rate: for the binaural monitor.
void hrirFor(double sr, double az, double el, std::vector<float>& left, std::vector<float>& right) {
    const HrirSet& set = hrirsAt(sr);
    int idx[4];
    float w[4];
    int n = set.neighbours(az, el, idx, w);
    left.assign((size_t)set.taps, 0.f);
    right.assign((size_t)set.taps, 0.f);
    for (int k = 0; k < n; ++k)
        for (int t = 0; t < set.taps; ++t) {
            left[(size_t)t] += w[k] * set.L(idx[k])[t];
            right[(size_t)t] += w[k] * set.R(idx[k])[t];
        }
}

void registerSpatialObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"ambienc~", "Spatial", "ambisonic encoder: [order] [azimuth] [elevation]", with<AmbiencBox>()},
        {"ambirot~", "Spatial", "turn a sound field: [order], yaw in the last inlet", with<AmbirotBox>()},
        {"ambidec~", "Spatial", "decode to speakers: order azimuth ...", with<AmbidecBox>()},
        {"binaural~", "Spatial", "sound field to headphones (KEMAR): [order]", with<BinauralBox>()},
        {"hrtf~", "Spatial", "a sound at a direction on headphones (KEMAR): [azimuth] [elevation]", with<HrtfBox>()},
    });
}

}  // namespace moritz
