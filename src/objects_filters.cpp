// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_filters.cpp — filter design: filterdesign computes biquad~
// coefficients, filterplot draws a filter's frequency response
//
// The designs are Robert Bristow-Johnson's "Audio EQ Cookbook" biquads, as
// Musil's biquad. The coefficients come in biquad~'s order, as in Max:
// y = a0 x + a1 x[n-1] + a2 x[n-2] - b1 y[n-1] - b2 y[n-2].

#include "objects.h"
#include "zoom.h"
#include "signals/biquad.h"   // Musil's RBJ biquads

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <complex>

namespace moritz {
namespace {

const char* const kTypes[] = {"lowpass", "highpass", "bandpass", "notch", "peak", "lowshelf", "highshelf", "allpass"};

// the RBJ cookbook biquad of Musil's biquad (signals/biquad.h); returns false for an unknown type
bool designBiquad(const std::string& type, double sr, double f0, double q, double gainDb, float c[5]) {
    musil::dsp::BiquadType t;
    if (!musil::dsp::biquad_type(type, t)) return false;
    const musil::dsp::BiquadCoeffs k =
        musil::dsp::rbj(t, sr, std::clamp(f0, 1.0, sr * 0.499), std::max(q, 0.01), gainDb).normalized();
    c[0] = (float)k.b0; c[1] = (float)k.b1; c[2] = (float)k.b2;
    c[3] = (float)k.a1; c[4] = (float)k.a2;
    return true;
}

double sampleRate(Patch* p) {
    double sr = p ? p->root()->sampleRate.load() : 0.0;
    return sr > 0 ? sr : 44100.0;
}

// ---------------------------------------------------------------- filterdesign
// filterdesign [type] [freq] [q] [gain]: the coefficients of a biquad~ for
// a filter type (lowpass highpass bandpass notch peak lowshelf highshelf
// allpass): a type name, a frequency (left inlet), a Q (middle) or a gain in
// dB (right; for peak and the shelves) outputs the list a0 a1 a2 b1 b2,
// ready for biquad~ and filterplot. bang outputs again.
class FilterDesignBox : public Box {
public:
    explicit FilterDesignBox(const Message& a) {
        size_t k = 0;
        if (k < a.size() && a[k].isSymbol()) type_ = a[k++].s;
        f_ = argf(a, k, 1000.f);
        q_ = argf(a, k + 1, 0.707f);
        g_ = argf(a, k + 2, 0.f);
        setPorts({P::Control, P::Control, P::Control}, {P::Control});
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) { q_ = m[0].f; output(); } return; }
        if (in == 2) { if (m[0].isFloat()) { g_ = m[0].f; output(); } return; }
        if (m[0].isFloat()) {
            f_ = m[0].f;
            if (m.size() > 1 && m[1].isFloat()) q_ = m[1].f;
            if (m.size() > 2 && m[2].isFloat()) g_ = m[2].f;
        } else if (m[0].isSymbol() && !m[0].is("bang")) {
            std::string t = m[0].s == "bandstop" ? "notch" : m[0].s;
            if (std::find(std::begin(kTypes), std::end(kTypes), t) == std::end(kTypes)) {
                patch->print("filterdesign: unknown type " + m[0].s);
                return;
            }
            type_ = t;
        }
        output();
    }

private:
    void output() {
        float c[5];
        if (!designBiquad(type_, sampleRate(patch), f_, q_, g_, c)) {
            patch->print("filterdesign: unknown type " + type_);
            return;
        }
        outlet(0, {Atom(c[0]), Atom(c[1]), Atom(c[2]), Atom(c[3]), Atom(c[4])});
    }
    std::string type_ = "lowpass";
    float f_, q_, g_;
};

// ---------------------------------------------------------------- filterplot
// filterplot [width] [height]: draws the frequency response (gain in dB, on
// a log frequency axis) of the biquad~ coefficients it receives, a0 a1 a2 b1
// b2; a list of 10, 15 ... numbers is a cascade of biquads. Dragging in it
// outputs "frequency gain" (Hz, dB) at the mouse, to drive a filterdesign.
class FilterPlotBox : public Box {
public:
    explicit FilterPlotBox(const Message& a) {
        kind = BoxKind::UI;
        w_ = std::clamp((int)argf(a, 0, 300.f), 120, 2000);
        h_ = std::clamp((int)argf(a, 1, 140.f), 60, 1000);
        setPorts({P::Control}, {P::Control});
        coeffs_ = {1.f, 0.f, 0.f, 0.f, 0.f};
    }
    void uiSize(int& w, int& h) const override { w = w_; h = h_; }
    void message(int, const Message& m) override {
        std::vector<float> c;
        for (auto& x : m)
            if (x.isFloat()) c.push_back(x.f);
        if (c.size() < 5) return;
        c.resize(c.size() / 5 * 5);
        coeffs_ = c;
        patch->redraw();
    }
    void uiMouse(int e, int mx, int my) override {
        if (e != FL_PUSH && e != FL_DRAG) return;
        double nyq = sampleRate(patch) / 2;
        double fx = std::clamp((mx - 3.0) / (w - 6), 0.0, 1.0), fy = std::clamp((my - 3.0) / (h - 6), 0.0, 1.0);
        double f = 20.0 * std::pow(nyq / 20.0, fx), db = kTop - fy * (kTop - kBottom);
        outlet(0, {Atom((float)std::round(f)), Atom((float)(std::round(db * 10) / 10))});
    }
    void drawUI(int X, int Y) override {
        int x0 = X + 3, y0 = Y + 3, W = w - 6, H = h - 6;
        fl_color(fl_rgb_color(20, 22, 26));
        zd::rectf(x0, y0, W, H);
        double sr = sampleRate(patch), nyq = sr / 2;
        auto xOf = [&](double f) { return x0 + (int)(std::log(f / 20.0) / std::log(nyq / 20.0) * (W - 1)); };
        auto yOf = [&](double db) { return y0 + (int)(std::clamp((kTop - db) / (kTop - kBottom), 0.0, 1.0) * (H - 1)); };
        zd::font(FL_HELVETICA, 9);
        for (double f : {100.0, 1000.0, 10000.0}) {
            if (f >= nyq) continue;
            fl_color(fl_rgb_color(42, 46, 54));
            zd::yxline(xOf(f), y0, y0 + H - 1);
            fl_color(fl_rgb_color(90, 94, 104));
            zd::draw(f >= 1000 ? (std::to_string((int)(f / 1000)) + "k").c_str() : "100", xOf(f) + 2, y0 + H - 3);
        }
        for (double db : {12.0, 0.0, -12.0, -24.0, -36.0}) {
            fl_color(db == 0 ? fl_rgb_color(70, 76, 88) : fl_rgb_color(42, 46, 54));
            zd::xyline(x0, yOf(db), x0 + W - 1);
            fl_color(fl_rgb_color(90, 94, 104));
            zd::draw((std::to_string((int)db) + " dB").c_str(), x0 + 2, yOf(db) - 2);
        }
        // |H(e^jw)| of the cascade
        fl_color(kAccent);
        zd::line_style(FL_SOLID, 2);
        int px = -1, py = 0;
        for (int i = 0; i < W; i += 2) {
            double f = 20.0 * std::pow(nyq / 20.0, (double)i / (W - 1));
            std::complex<double> z1 = std::polar(1.0, -2 * dsp::kPi * f / sr), z2 = z1 * z1;
            double mag = 1.0;
            for (size_t k = 0; k + 4 < coeffs_.size(); k += 5) {
                const float* c = &coeffs_[k];
                std::complex<double> num = (double)c[0] + (double)c[1] * z1 + (double)c[2] * z2;
                std::complex<double> den = 1.0 + (double)c[3] * z1 + (double)c[4] * z2;
                mag *= std::abs(num) / std::max(std::abs(den), 1e-12);
            }
            int y = yOf(20 * std::log10(std::max(mag, 1e-9)));
            if (px >= 0) zd::line(px, py, x0 + i, y);
            px = x0 + i;
            py = y;
        }
        zd::line_style(0);
    }
    Message saveArgs() const override { return {Atom((float)w_), Atom((float)h_)}; }

private:
    static constexpr double kTop = 24.0, kBottom = -48.0;
    int w_, h_;
    std::vector<float> coeffs_;
};

}  // namespace

void registerFilterObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"filterdesign", "Filters", "biquad~ coefficients: type freq q gain", with<FilterDesignBox>()},
        {"filterplot", "Filters", "frequency response of biquad coefficients [w h]", with<FilterPlotBox>()},
    });
}

}  // namespace moritz
