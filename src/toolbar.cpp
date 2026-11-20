// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// toolbar.cpp — the toolbar's round buttons and its meters (CPU load, output levels)

#include "gui.h"
#include "ui_util.h"

#include <FL/fl_draw.H>

#include <chrono>
#include <cmath>

namespace moritz {

ToolButton::ToolButton(int x, int y, int w, int h, Icon icon, const char* label, bool toggle)
    : Fl_Button(x, y, w, h), icon_(icon) {
    type(toggle ? FL_TOGGLE_BUTTON : FL_NORMAL_BUTTON);
    copy_label(label);
    box(FL_NO_BOX);
    visible_focus(0);
}

void ToolButton::draw() {
    bool on = value() || lit;
    int X = x(), Y = y(), W = w(), H = h();
    Fl_Color fill = on ? (icon_ == Power ? kOn : icon_ == Record ? fl_rgb_color(170, 40, 40) : fl_rgb_color(52, 92, 160))
                       : kBoxFill;
    fl_color(fill);
    fl_rounded_rectf(X, Y, W, H, H / 2);
    fl_color(on ? fl_color_average(fill, FL_WHITE, 0.6f) : kBorder);
    fl_rounded_rect(X, Y, W, H, H / 2);
    // icon
    int cx = X + H / 2 + 4, cy = Y + H / 2, r = H / 2 - 6;
    fl_color(on ? FL_WHITE : kDim);
    fl_line_style(FL_SOLID | FL_CAP_ROUND, 2);
    if (icon_ == Power) {
        fl_arc(cx - r, cy - r, 2 * r, 2 * r, 120, 420);   // open ring with a gap at the top
        fl_line(cx, cy - r - 1, cx, cy);
    } else if (icon_ == Pencil) {
        fl_line(cx - r, cy + r, cx + r, cy - r);           // a pencil stroke
        fl_line(cx - r, cy + r, cx - r + 3, cy + r);
    } else if (icon_ == Code) {                            // </>
        int h = r - 1;
        fl_line(cx - 2, cy - h, cx - r - 1, cy, cx - 2, cy + h);
        fl_line(cx + 2, cy - h, cx + r + 1, cy, cx + 2, cy + h);
    } else if (icon_ == Minus || icon_ == Plus) {          // centred in a small round button
        int mx = X + W / 2, my = Y + H / 2, a = 5;
        fl_line(mx - a, my, mx + a, my);
        if (icon_ == Plus) fl_line(mx, my - a, mx, my + a);
    } else if (icon_ == Browser) {                         // a grid of four squares
        int q = r - 1;
        fl_line_style(FL_SOLID, 1);
        fl_rect(cx - q - 1, cy - q - 1, q, q);
        fl_rect(cx + 1, cy - q - 1, q, q);
        fl_rect(cx - q - 1, cy + 1, q, q);
        fl_rect(cx + 1, cy + 1, q, q);
    } else if (icon_ == Console) {                         // >_
        fl_line(cx - r, cy - r + 1, cx - 1, cy, cx - r, cy + r - 1);
        fl_line(cx + 1, cy + r - 1, cx + r + 1, cy + r - 1);
    } else if (icon_ == Record) {
        fl_color(on ? FL_WHITE : fl_rgb_color(220, 70, 70));
        fl_pie(cx - r + 1, cy - r + 1, 2 * r - 2, 2 * r - 2, 0, 360);
    } else if (icon_ == Play || icon_ == Stop || icon_ == ToStart || icon_ == Loop) {
        // the transport's icons, centred when the button has no label
        int mx = label() && *label() ? cx : X + W / 2, my = Y + H / 2, a = std::max(3, H / 2 - 6);
        fl_line_style(FL_SOLID, 1);
        if (icon_ == Play) {
            fl_polygon(mx - a + 1, my - a, mx - a + 1, my + a, mx + a + 1, my);
        } else if (icon_ == Stop) {
            fl_rectf(mx - a + 1, my - a + 1, 2 * a - 1, 2 * a - 1);
        } else if (icon_ == ToStart) {
            fl_rectf(mx - a, my - a, 2, 2 * a + 1);
            fl_polygon(mx + a + 1, my - a, mx + a + 1, my + a, mx - a + 2, my);
        } else {                                           // a loop: an open ring with an arrow head
            fl_line_style(FL_SOLID | FL_CAP_ROUND, 2);
            fl_arc(mx - a, my - a, 2 * a, 2 * a, 60, 380);
            fl_line_style(FL_SOLID, 1);
            fl_polygon(mx + a - 4, my - a - 3, mx + a + 3, my - a, mx + a - 3, my - a + 4);
        }
    }
    fl_line_style(0);
    if (alert) {                                           // a red dot at the top right
        fl_color(fl_rgb_color(235, 70, 70));
        fl_pie(X + W - 12, Y + 2, 8, 8, 0, 360);
    }
    fl_font(FL_HELVETICA_BOLD, 12);
    fl_color(on ? FL_WHITE : kText);
    if (icon_ == None) fl_draw(label(), X, Y, W, H, FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    else if (icon_ != Minus && icon_ != Plus) fl_draw(label(), X + H + 4, Y, W - H - 8, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

// ---- toolbar meters: CPU load and output level

ToolMeters::ToolMeters(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    tooltip("CPU: the share of each audio buffer spent computing the patches (red above 80%).\n"
            "Level: the peaks of every output channel; the light stays red for 2 s after a clip.");
}

ToolMeters::~ToolMeters() = default;

// Drawn again only when something it shows changed: with the audio off (or silent) the bars fall to
// rest and the meters stay still.
void ToolMeters::update(float cpu, const std::vector<float>& peaks) {
    bool changed = std::fabs(cpu - cpu_) >= 0.005f;
    if (changed) cpu_ = cpu;
    if (level_.size() != peaks.size()) {
        level_.assign(peaks.size(), 0.f);
        hold_.assign(peaks.size(), 0.f);
        changed = true;
    }
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto rest = [](float v) { return v < 1e-3f ? 0.f : v; };   // below -60 dB the bars show nothing
    for (size_t c = 0; c < peaks.size(); ++c) {
        float l = rest(std::max(peaks[c], level_[c] * 0.82f));        // a falling bar
        float h = rest(std::max(peaks[c], hold_[c] * 0.97f));
        if (l != level_[c] || h != hold_[c]) changed = true;
        level_[c] = l;
        hold_[c] = h;
        if (peaks[c] >= 0.999f) clipUntil_ = now + 2;
    }
    bool clip = now < clipUntil_;
    if (clip != clipShown_) changed = true;
    clipShown_ = clip;
    if (changed) redraw();
}

void ToolMeters::draw() {
    int X = x(), Y = y(), W = w(), H = h();
    fl_color(kStatusBg);
    fl_rectf(X, Y, W, H);
    fl_font(FL_HELVETICA, 11);
    // CPU
    fl_color(kDim);
    fl_draw("CPU", X, Y, 28, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    int bx = X + 30, bw = 46, bh = 8, byy = Y + (H - bh) / 2;
    fl_color(kBoxFill);
    fl_rectf(bx, byy, bw, bh);
    float c = std::clamp(cpu_, 0.f, 1.f);
    fl_color(c > 0.8f ? fl_rgb_color(225, 70, 70) : c > 0.5f ? fl_rgb_color(225, 180, 60) : kAccent);
    fl_rectf(bx, byy, (int)(bw * c), bh);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", (int)std::lround(cpu_ * 100));
    fl_color(kText);
    fl_draw(buf, bx + bw + 4, Y, 34, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    // levels, dB from -60 to 0: up to two channels as thin rows, more as columns
    int lx = X + 118, lw = W - 118 - 12;
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto frac = [](float a) { double db = a > 1e-6f ? 20 * std::log10(a) : -120; return std::clamp((db + 60) / 60, 0.0, 1.0); };
    auto colour = [](double f) { return f > 0.9 ? fl_rgb_color(225, 180, 60) : fl_rgb_color(90, 190, 120); };
    int n = (int)level_.size();
    if (n <= 2) {
        for (int ch = 0; ch < 2; ++ch) {
            int ly = Y + H / 2 - 5 + ch * 6;
            fl_color(kBoxFill);
            fl_rectf(lx, ly, lw, 4);
            float lv = ch < n ? level_[(size_t)ch] : (n == 1 ? level_[0] : 0.f);
            float hv = ch < n ? hold_[(size_t)ch] : (n == 1 ? hold_[0] : 0.f);
            double f = frac(lv);
            fl_color(colour(f));
            fl_rectf(lx, ly, (int)(lw * f), 4);
            fl_color(kText);
            if (int hp = (int)(lw * frac(hv)); hp > 0) fl_yxline(lx + hp - 1, ly, ly + 3);
        }
    } else {
        int top = Y + 2, hh = H - 4, gap = n > 16 ? 1 : 2;
        int cw = std::max(1, (lw - gap * (n - 1)) / n);
        for (int ch = 0; ch < n; ++ch) {
            int cx = lx + ch * (cw + gap);
            fl_color(kBoxFill);
            fl_rectf(cx, top, cw, hh);
            double f = frac(level_[(size_t)ch]);
            int bhh = (int)(hh * f);
            fl_color(colour(f));
            fl_rectf(cx, top + hh - bhh, cw, bhh);
            fl_color(kText);
            if (int hp = (int)(hh * frac(hold_[(size_t)ch])); hp > 0) fl_xyline(cx, top + hh - hp, cx + cw - 1);
        }
    }
    fl_color(now < clipUntil_ ? fl_rgb_color(235, 60, 60) : kBoxFill);
    fl_pie(X + W - 9, Y + H / 2 - 4, 8, 8, 0, 360);
}

}  // namespace moritz
