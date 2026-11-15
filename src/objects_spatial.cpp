// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_spatial.cpp — ambisonics as in Musil: ambienc~ (encoder), ambirot~
// (rotation), ambidec~ (to speakers) and binaural~ (to headphones)
//
// Conventions (AmbiX): azimuth in degrees, 0 in front, positive to the LEFT;
// elevation in degrees, positive up. A B-format signal of order N has
// (N+1)^2 channels, in ACN order with SN3D normalisation: for order 1, W Y Z X.

#include "objects.h"
#include "hrir_kemar.h"

#include <algorithm>
#include <cmath>

namespace moritz {
namespace {

constexpr int kMaxOrder = 5;

int channelsOf(int order) { return (order + 1) * (order + 1); }

double rad(double d) { return d * dsp::kPi / 180.0; }

// the real spherical harmonics up to order at (az, el): (order+1)^2 gains,
// ACN order, SN3D (Musil's sh_gains)
void shGains(int order, double az, double el, float* g) {
    double a = rad(az), e = rad(el);
    double z = std::sin(e), r = std::cos(e);
    auto fact = [](int n) { double f = 1; for (int k = 2; k <= n; ++k) f *= k; return f; };
    for (int n = 0; n <= order; ++n)
        for (int m = -n; m <= n; ++m) {
            int am = std::abs(m);
            double pmm = 1;                                   // P_m^m = (2m-1)!! (1-z^2)^(m/2)
            for (int k = 1; k <= am; ++k) pmm *= (2 * k - 1) * r;
            double p = pmm;
            if (n > am) {
                double p1 = z * (2 * am + 1) * pmm;
                p = p1;
                double pk2 = pmm, pk1 = p1;
                for (int k = am + 2; k <= n; ++k) {
                    p = ((2 * k - 1) * z * pk1 - (k + am - 1) * pk2) / (k - am);
                    pk2 = pk1;
                    pk1 = p;
                }
            }
            double norm = std::sqrt((m == 0 ? 1.0 : 2.0) * fact(n - am) / fact(n + am));
            double azf = m > 0 ? std::cos(m * a) : m < 0 ? std::sin(am * a) : 1.0;
            g[n * n + n + m] = (float)(norm * p * azf);
        }
}

// max-rE weights (Zotter & Frank): the energy of a decoded source
// concentrated towards its direction. On a horizontal ring (2D) the weight
// of order n is cos(n pi / (2N + 2)); on the sphere (3D) it is the Legendre
// polynomial P_n(r_E), r_E = cos(137.9 degrees / (N + 1.51)).
void maxReWeights(int order, float* w) {          // 2D, per channel
    for (int n = 0; n <= order; ++n) {
        float wn = (float)std::cos(n * dsp::kPi / (2 * order + 2));
        for (int k = 0; k < 2 * n + 1; ++k) w[n * n + k] = wn;
    }
}
void maxReWeights3D(int order, float* w) {        // 3D, per channel
    double rE = std::cos(137.9 * dsp::kPi / 180.0 / (order + 1.51));
    double p0 = 1, p1 = rE;
    for (int n = 0; n <= order; ++n) {
        double pn = n == 0 ? 1.0 : n == 1 ? rE : ((2 * n - 1) * rE * p1 - (n - 1) * p0) / n;
        if (n >= 2) { p0 = p1; p1 = pn; }
        for (int k = 0; k < 2 * n + 1; ++k) w[n * n + k] = (float)pn;
    }
}

int orderArg(const Message& a, size_t i, float def) { return std::clamp((int)argf(a, i, def), 0, kMaxOrder); }

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
        shGains(order_, az[n - 1], el[n - 1], g1);
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
// weights. Musil's ambi-decode samples all the channels without the
// normalisation, which blurs the image (energy vector 0.57 instead of 0.71
// at order 1, 0.66 instead of 0.87 at order 2).
class AmbidecBox : public Box {
public:
    explicit AmbidecBox(const Message& a) : order_(std::max(1, orderArg(a, 0, 1.f))) {
        nch_ = channelsOf(order_);
        for (size_t k = 1; k < a.size(); ++k)
            if (a[k].isFloat()) az_.push_back(a[k].f);
        if (az_.empty()) az_ = {45.f, 135.f, -135.f, -45.f};
        float w[36], front[36];
        maxReWeights(order_, w);
        shGains(order_, 0.0, 0.0, front);          // front[n*n+2n] = SN3D factor of cos(n az) at ear height
        gains_.assign(az_.size() * (size_t)nch_, 0.f);
        const double ns = (double)az_.size();
        for (size_t s = 0; s < az_.size(); ++s) {
            double a = rad(az_[s]);
            gains_[s * (size_t)nch_] = (float)(w[0] / ns);
            for (int n = 1; n <= order_; ++n) {
                double c = front[n * n + 2 * n], scale = 2.0 * w[n * n] / (ns * c);
                gains_[s * (size_t)nch_ + (size_t)(n * n + 2 * n)] = (float)(scale * std::cos(n * a));   // m = +n
                gains_[s * (size_t)nch_ + (size_t)(n * n)] = (float)(scale * std::sin(n * a));           // m = -n
            }
        }
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
// binaural~ [order]: a sound field for headphones (after Musil's
// binaural-decode). The field is decoded to 17 virtual speakers spread over
// the sphere (8 at ear height, 4 above, 4 below, 1 overhead) and each is
// heard through the measured head-related impulse responses of its
// direction (MIT KEMAR). The decoder samples the field at each speaker with
// the SN3D normalisation (2n+1) and 3D max-rE weights; Musil's ten speakers
// with neither gave direction errors up to 16 degrees and a blurred image.
// All of it is folded into one filter pair per channel, so the cost does not
// depend on the sources. A source in front plays at the level of the front
// head response.
class BinauralBox : public Box {
public:
    explicit BinauralBox(const Message& a) : order_(std::max(1, orderArg(a, 0, 1.f))) {
        nch_ = channelsOf(order_);
        setPorts(std::vector<Port>((size_t)nch_, P::Signal), {P::Signal, P::Signal});
    }
    void resolve() override { build(patch->root()->sampleRate.load()); }
    void dsp(int n) override {
        double sr = patch->root()->sampleRate.load();
        if (sr > 0 && std::fabs(sr - builtSr_) > 0.5) build(sr);
        float *L = sigOut(0), *R = sigOut(1);
        std::fill(L, L + n, 0.f);
        std::fill(R, R + n, 0.f);
        const int T = taps_;
        if (T == 0) return;
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
        double f = sr / kemar::kRate;
        int T = std::max(1, (int)std::ceil(kemar::kTaps * f));
        std::vector<std::vector<float>> hl(kemar::kDirections), hr(kemar::kDirections);
        for (int d = 0; d < kemar::kDirections; ++d) {
            hl[(size_t)d] = resample(kemar::kSet[d].left, f, T);
            hr[(size_t)d] = resample(kemar::kSet[d].right, f, T);
        }
        float w[36], g[36];
        maxReWeights3D(order_, w);
        for (int n = 0; n <= order_; ++n)
            for (int k = 0; k < 2 * n + 1; ++k) w[n * n + k] *= (float)(2 * n + 1);   // SN3D -> sampling decoder
        std::vector<float> fl((size_t)nch_ * T, 0.f), fr((size_t)nch_ * T, 0.f);
        for (int d = 0; d < kemar::kDirections; ++d) {
            shGains(order_, kemar::kSet[d].az, kemar::kSet[d].el, g);
            for (int k = 0; k < nch_; ++k) {
                float c = g[k] * w[k] / kemar::kDirections;
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
    static std::vector<float> resample(const float* h, double f, int T) {
        std::vector<float> out((size_t)T, 0.f);
        if (std::fabs(f - 1.0) < 1e-6) {
            for (int t = 0; t < T && t < kemar::kTaps; ++t) out[(size_t)t] = h[t];
            return out;
        }
        double fc = std::min(1.0, f);                      // band limit when going down
        const int half = 16;
        for (int t = 0; t < T; ++t) {
            double c = t / f, acc = 0;
            for (int i = (int)c - half; i <= (int)c + half; ++i) {
                if (i < 0 || i >= kemar::kTaps) continue;
                double x = (c - i) * fc;
                double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(dsp::kPi * x) / (dsp::kPi * x);
                double win = 0.5 + 0.5 * std::cos(dsp::kPi * (c - i) / (half + 1));
                acc += h[i] * fc * sinc * win;
            }
            out[(size_t)t] = (float)acc;
        }
        return out;
    }

    int order_, nch_, taps_ = 0;
    double builtSr_ = 0;
    std::vector<float> left_, right_, hist_;
    std::vector<int> pos_;
};

}  // namespace

void registerSpatialObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"ambienc~", "Spatial", "ambisonic encoder: [order] [azimuth] [elevation]", with<AmbiencBox>()},
        {"ambirot~", "Spatial", "turn a sound field: [order], yaw in the last inlet", with<AmbirotBox>()},
        {"ambidec~", "Spatial", "decode to speakers: order azimuth ...", with<AmbidecBox>()},
        {"binaural~", "Spatial", "sound field to headphones (KEMAR): [order]", with<BinauralBox>()},
    });
}

}  // namespace moritz
