// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_editor.cpp — the sound editor's panel (sound_editor.h): toolbar, overview, waveform and spectrogram,
// selection and playhead, the preview, the edits

#include "sound_editor.h"
#include "preview.h"
#include "ui_util.h"

#include <FL/fl_ask.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace moritz {

namespace {
constexpr int kOverviewH = 40;
constexpr int kBottomH = 150;
const Fl_Color kLaneBg = fl_rgb_color(24, 25, 29);
const Fl_Color kLaneSel = fl_rgb_color(36, 50, 76);
const Fl_Color kWave = fl_rgb_color(70, 130, 220);
const Fl_Color kWaveSel = fl_rgb_color(140, 190, 255);
const Fl_Color kPlayhead = fl_rgb_color(235, 80, 80);
const Fl_Color kPlayCursor = fl_rgb_color(120, 230, 150);

// inferno-like: dark violet to yellow, as the mockup's spectrogram
void colour(double t, unsigned char* p) {
    static const unsigned char k[][3] = {{0, 0, 4},     {40, 11, 84},   {101, 21, 110}, {159, 42, 99},
                                         {212, 72, 66}, {245, 125, 21}, {250, 193, 39}, {252, 255, 164}};
    t = std::clamp(t, 0.0, 1.0) * 7;
    int i = std::min(6, (int)t);
    double f = t - i;
    for (int c = 0; c < 3; ++c) p[c] = (unsigned char)(k[i][c] + f * (k[i + 1][c] - k[i][c]));
}

std::string rate(double sr) {
    char b[32];
    if (std::fmod(sr, 1000.0) == 0) std::snprintf(b, sizeof b, "%.0f kHz", sr / 1000);
    else std::snprintf(b, sizeof b, "%.1f kHz", sr / 1000);
    return b;
}

double niceTimeStep(double secondsPerPixel, int minPx) {
    static const double steps[] = {0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
                                   1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 1800, 3600};
    for (double s : steps)
        if (s / secondsPerPixel >= minPx) return s;
    return 7200;
}
}  // namespace

std::string formatTime(double s) {
    if (!(s >= 0)) s = 0;
    long ms = std::lround(s * 1000);
    char b[32];
    std::snprintf(b, sizeof b, "%ld:%02ld.%03ld", ms / 60000, ms / 1000 % 60, ms % 1000);
    return b;
}

// ---------------------------------------------------------------- the overview

void SoundOverview::draw() {
    SoundDoc& d = ed_->doc();
    int X = x(), Y = y(), W = w(), H = h();
    fl_color(kStatusBg);
    fl_rectf(X, Y, W, H);
    int px = X + SoundView::kGutter, pw = std::max(1, W - SoundView::kGutter - 4), py = Y + 4, ph = H - 8;
    fl_color(kLaneBg);
    fl_rectf(px, py, pw, ph);
    const Sound& s = d.sound();
    long frames = d.frames();
    if (frames > 0) {
        double per = (double)frames / pw;
        // the part shown
        int a = px + (int)std::floor(ed_->start() / per), b = px + (int)std::ceil((ed_->start() + ed_->framesPerPixel() * ed_->view()->laneW()) / per);
        b = std::min(b, px + pw);
        fl_color(fl_rgb_color(40, 46, 60));
        fl_rectf(a, py, std::max(2, b - a), ph);
        int mid = py + ph / 2;
        for (int i = 0; i < pw; ++i) {
            float lo = 0, hi = 0;
            for (int c = 0; c < d.channels(); ++c) {
                float l, h;
                d.summary().range(*s, c, (long)(i * per), (long)((i + 1) * per) + 1, l, h);
                lo = std::min(lo, l);
                hi = std::max(hi, h);
            }
            fl_color(px + i >= a && px + i < b ? kWaveSel : fl_rgb_color(90, 100, 120));
            int y0 = mid - (int)std::lround(std::clamp(hi, -1.f, 1.f) * (ph / 2 - 1));
            int y1 = mid - (int)std::lround(std::clamp(lo, -1.f, 1.f) * (ph / 2 - 1));
            fl_yxline(px + i, y0, std::max(y0, y1));
        }
        if (!d.sel.empty()) {                             // the selection, along the bottom
            fl_color(kSelect);
            int sa = px + (int)(d.sel.start / per), sb = px + (int)std::ceil(d.sel.end / per);
            fl_rectf(sa, py + ph - 3, std::max(1, sb - sa), 3);
        }
        fl_color(kAccent);
        fl_rect(a, py, std::max(2, b - a), ph);
        fl_color(kPlayhead);
        fl_yxline(px + (int)(d.playhead / per), py, py + ph - 1);
        double pp = ed_->playPosition();
        if (pp >= 0) {
            fl_color(kPlayCursor);
            fl_yxline(px + (int)(pp / per), py, py + ph - 1);
        }
    }
    fl_font(FL_HELVETICA, 10);
    fl_color(kDim);
    fl_draw("overview", X + 4, Y, SoundView::kGutter - 6, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
}

int SoundOverview::handle(int e) {
    SoundDoc& d = ed_->doc();
    int px = x() + SoundView::kGutter, pw = std::max(1, w() - SoundView::kGutter - 4);
    double per = (double)std::max(1L, d.frames()) / pw;
    double shown = ed_->framesPerPixel() * ed_->view()->laneW();
    double f = (Fl::event_x() - px) * per;
    switch (e) {
    case FL_PUSH:
        if (f >= ed_->start() && f <= ed_->start() + shown) grab_ = f - ed_->start();
        else grab_ = shown / 2;                           // elsewhere: centred there
        ed_->setView(f - grab_, ed_->framesPerPixel());
        return 1;
    case FL_DRAG:
        ed_->setView(f - grab_, ed_->framesPerPixel());
        return 1;
    case FL_RELEASE:
        return 1;
    case FL_MOUSEWHEEL:
        return ed_->view()->handle(e);
    }
    return Fl_Widget::handle(e);
}

// ---------------------------------------------------------------- the view

void SoundView::areas(int& waveY, int& waveH, int& specY, int& specH) const {
    int top = y() + kRulerH, avail = h() - kRulerH;
    waveY = specY = top;
    waveH = specH = 0;
    switch (ed_->mode()) {
    case SoundEditor::Mode::Wave: waveH = avail; break;
    case SoundEditor::Mode::Spectrum: specH = avail; break;
    case SoundEditor::Mode::Both:
        waveH = avail * 11 / 20;
        specY = top + waveH + 4;
        specH = avail - waveH - 4;
        break;
    }
}

void SoundView::drawRuler(int X, int Y, int W) {
    SoundDoc& d = ed_->doc();
    fl_color(kStatusBg);
    fl_rectf(x(), Y, w(), kRulerH);
    double sr = d.sr() > 0 ? d.sr() : 44100.0;
    double spp = ed_->framesPerPixel(), t0 = ed_->start() / sr, t1 = (ed_->start() + spp * W) / sr;
    if (d.frames() == 0) return;                          // nothing to measure
    double step = niceTimeStep(spp / sr, 70);
    fl_font(FL_HELVETICA, 10);
    for (double t = std::ceil(t0 / step) * step; t <= t1; t += step) {
        int px = ed_->xOf(t * sr);
        fl_color(kBorder);
        fl_yxline(px, Y + kRulerH - 7, Y + kRulerH - 1);
        char b[32];
        if (step >= 60) std::snprintf(b, sizeof b, "%d:%02d", (int)(t / 60 + 1e-9), (int)std::lround(std::fmod(t, 60.0)) % 60);
        else if (step >= 1) std::snprintf(b, sizeof b, "%gs", std::round(t));
        else if (step >= 0.01) std::snprintf(b, sizeof b, "%.2f", t);
        else std::snprintf(b, sizeof b, "%.4f", t);
        fl_color(kDim);
        fl_draw(b, px + 3, Y + kRulerH - 6);
        // finer ticks between
        for (int k = 1; k < 5; ++k) {
            int q = ed_->xOf((t + step * k / 5) * sr);
            if (q < X + W) fl_yxline(q, Y + kRulerH - 3, Y + kRulerH - 1);
        }
    }
    fl_color(kGrid);
    fl_xyline(x(), Y + kRulerH - 1, x() + w());
    // the playhead's mark
    int ph = ed_->xOf((double)d.playhead);
    if (ph >= X && ph <= X + W) {
        fl_color(kPlayhead);
        fl_polygon(ph - 5, Y + 2, ph + 5, Y + 2, ph, Y + 9);
    }
}

void SoundView::drawWave(int X, int Y, int W, int H) {
    SoundDoc& d = ed_->doc();
    const Sound& s = d.sound();
    int ch = std::max(1, d.channels());
    int laneH = H / ch;
    double spp = ed_->framesPerPixel(), st = ed_->start();
    int sa = d.sel.empty() ? 0 : ed_->xOf((double)d.sel.start), sb = d.sel.empty() ? 0 : ed_->xOf((double)d.sel.end);
    sa = std::clamp(sa, X, X + W);
    sb = std::clamp(sb, X, X + W);
    fl_font(FL_HELVETICA_BOLD, 11);
    for (int c = 0; c < ch; ++c) {
        int ly = Y + c * laneH, lh = c == ch - 1 ? H - c * laneH : laneH;
        fl_color(kLaneBg);
        fl_rectf(X, ly, W, lh - 1);
        if (sb > sa) {
            fl_color(kLaneSel);
            fl_rectf(sa, ly, sb - sa, lh - 1);
        }
        int mid = ly + lh / 2;
        double amp = std::max(1, lh / 2 - 3);
        fl_color(kGrid);
        fl_xyline(X, mid, X + W - 1);
        fl_color(kDim);
        const char* names2[] = {"L", "R"};
        std::string name = ch == 2 ? names2[c] : std::to_string(c + 1);
        fl_draw(name.c_str(), x() + 8, ly + 16);
        if (d.frames() == 0) continue;
        fl_push_clip(X, ly, W, lh - 1);
        auto yOf = [&](float v) { return mid - (int)std::lround(std::clamp(v, -1.5f, 1.5f) * amp); };
        if (spp >= 1.0) {
            for (int i = 0; i < W; ++i) {
                double a = st + i * spp;
                if (a >= (double)d.frames()) break;
                float lo, hi;
                d.summary().range(*s, c, (long)std::floor(a), (long)std::floor(a + spp) + 1, lo, hi);
                fl_color(X + i >= sa && X + i < sb ? kWaveSel : kWave);
                int y0 = yOf(hi), y1 = yOf(lo);
                fl_yxline(X + i, y0, std::max(y0, y1));
            }
        } else {                                          // closer than a sample a pixel: the samples, joined
            long f0 = std::max(0L, (long)std::floor(st) - 1), f1 = std::min(d.frames() - 1, (long)std::ceil(st + W * spp) + 1);
            bool dots = 1.0 / spp >= 6;
            int px = 0, py = 0;
            for (long f = f0; f <= f1; ++f) {
                int qx = ed_->xOf((double)f), qy = yOf(s->data[(size_t)f * ch + c]);
                fl_color(qx >= sa && qx < sb ? kWaveSel : kWave);
                if (f > f0) fl_line(px, py, qx, qy);
                if (dots) fl_rectf(qx - 1, qy - 1, 3, 3);
                px = qx;
                py = qy;
            }
        }
        fl_pop_clip();
        if (c > 0) {
            fl_color(kGrid);
            fl_xyline(x(), ly - 1, X + W);
        }
    }
}

void SoundView::drawSpectrum(int X, int Y, int W, int H) {
    SoundDoc& d = ed_->doc();
    const Spectrogram* g = ed_->spectrogram();
    bool current = g && ed_->specGen_ == d.generation();
    if (!current || g->cols <= 0 || W <= 0 || H <= 0) {
        fl_color(kLaneBg);
        fl_rectf(X, Y, W, H);
        fl_color(kDim);
        fl_font(FL_HELVETICA, 11);
        const char* t = d.frames() == 0 ? "" : "computing the spectrogram…";
        fl_draw(t, X, Y, W, H, FL_ALIGN_CENTER);
        return;
    }
    // made at twice the size and drawn scaled to the area: every pixel of a hi-DPI screen is used
    const int K = 2, PW = W * K, PH = H * K;
    rgb_.resize((size_t)PW * PH * 3);
    double spp = ed_->framesPerPixel() / K, st = ed_->start();
    std::vector<int> row((size_t)PH);
    for (int j = 0; j < PH; ++j) row[(size_t)j] = std::clamp((int)((double)(PH - 1 - j) / PH * g->rows), 0, g->rows - 1);
    long ss = d.sel.start, se = d.sel.end;
    bool hasSel = !d.sel.empty();
    for (int i = 0; i < PW; ++i) {
        double f = st + (i + 0.5) * spp;
        bool out = f < 0 || f >= (double)d.frames();
        // between two frames when a pixel is narrower than a hop (zoomed in): no blocks
        double cf = std::clamp(f / g->hop, 0.0, (double)(g->cols - 1));
        int col = std::min((int)cf, g->cols - 1), col2 = std::min(col + 1, g->cols - 1);
        double t = spp < g->hop ? cf - col : 0.0;
        if (t == 0.0) col = col2 = std::clamp((int)std::lround(f / g->hop), 0, g->cols - 1);
        const unsigned char* v = &g->v[(size_t)col * g->rows];
        const unsigned char* v2 = &g->v[(size_t)col2 * g->rows];
        bool inSel = hasSel && f >= ss && f < se;
        for (int j = 0; j < PH; ++j) {
            unsigned char* p = &rgb_[((size_t)j * PW + i) * 3];
            if (out) { p[0] = p[1] = p[2] = 18; continue; }
            double val = v[row[(size_t)j]] + t * ((double)v2[row[(size_t)j]] - v[row[(size_t)j]]);
            colour((val - 25.5) / 229.5, p);              // -90 dB to 0 dB
            if (inSel) {                                  // the selection, tinted
                p[0] = (unsigned char)(p[0] * 0.84 + 90 * 0.16);
                p[1] = (unsigned char)(p[1] * 0.84 + 160 * 0.16);
                p[2] = (unsigned char)(p[2] * 0.84 + 255 * 0.16);
            }
        }
    }
    Fl_RGB_Image img(rgb_.data(), PW, PH, 3);
    img.scale(W, H, 0, 1);
    img.draw(X, Y);
    // the frequencies, on the left
    fl_font(FL_HELVETICA, 10);
    static const double marks[] = {50, 100, 200, 500, 1000, 2000, 4000, 8000, 16000};
    for (double f : marks) {
        if (f < g->fmin || f > g->fmax) continue;
        int yy = Y + H - (int)std::lround(std::log(f / g->fmin) / std::log(g->fmax / g->fmin) * H);
        if (yy < Y + 8 || yy > Y + H - 2) continue;
        char b[16];
        if (f >= 1000) std::snprintf(b, sizeof b, "%gk", f / 1000);
        else std::snprintf(b, sizeof b, "%g", f);
        fl_color(kDim);
        fl_draw(b, x() + 4, yy + 4);
        fl_color(kGrid);
        fl_xyline(X - 6, yy, X - 1);
    }
}

void SoundView::draw() {
    SoundDoc& d = ed_->doc();
    fl_push_clip(x(), y(), w(), h());
    fl_color(kBgEdit);
    fl_rectf(x(), y(), w(), h());
    int X = laneX(), W = laneW();
    int wy, wh, sy, sh;
    areas(wy, wh, sy, sh);
    if (wh > 0) drawWave(X, wy, W, wh);
    if (sh > 0) drawSpectrum(X, sy, W, sh);
    drawRuler(X, y(), W);
    int top = y() + kRulerH, bottom = y() + h();
    fl_push_clip(X, top, W, h() - kRulerH);
    if (!d.sel.empty()) {                                 // the selection's edges
        fl_color(kSelect);
        fl_yxline(ed_->xOf((double)d.sel.start), top, bottom);
        fl_yxline(ed_->xOf((double)d.sel.end), top, bottom);
    }
    fl_color(kPlayhead);
    fl_yxline(ed_->xOf((double)d.playhead), top, bottom);
    double pp = ed_->playPosition();
    if (pp >= 0) {
        fl_color(kPlayCursor);
        fl_yxline(ed_->xOf(pp), top, bottom);
    }
    fl_pop_clip();
    if (d.frames() == 0) {
        fl_color(kDim);
        fl_font(FL_HELVETICA, 12);
        fl_draw("empty: paste a sound here", X, top, W, h() - kRulerH, FL_ALIGN_CENTER);
    }
    fl_pop_clip();
}

void SoundView::press(int ex, bool inRuler, bool extend) {
    SoundDoc& d = ed_->doc();
    long f = std::clamp((long)std::lround(ed_->frameAt(ex)), 0L, d.frames());
    dragging_ = true;
    moved_ = false;
    rulerDrag_ = inRuler;
    if (inRuler) {
        d.playhead = f;
    } else if (extend) {                                  // from the far end of the selection (or the playhead)
        anchor_ = d.sel.empty() ? d.playhead : (std::labs(f - d.sel.start) > std::labs(f - d.sel.end) ? d.sel.start : d.sel.end);
        d.select(anchor_, f);
    } else {
        anchor_ = f;
        d.select(f, f);
        d.playhead = f;
    }
    ed_->selectionChanged();
}

void SoundView::dragTo(int ex) {
    if (!dragging_) return;
    SoundDoc& d = ed_->doc();
    double spp = ed_->framesPerPixel();
    if (ex < laneX()) ed_->scrollBy(-(laneX() - ex) * spp);          // past an edge: the view follows
    else if (ex > laneX() + laneW()) ed_->scrollBy((ex - laneX() - laneW()) * spp);
    long f = std::clamp((long)std::lround(ed_->frameAt(ex)), 0L, d.frames());
    moved_ = true;
    if (rulerDrag_) d.playhead = f;
    else d.select(anchor_, f);
    ed_->selectionChanged();
}

void SoundView::release(int ex) {
    if (!dragging_) return;
    if (moved_ || ex != Fl::event_x()) dragTo(ex);
    dragging_ = false;
    rulerDrag_ = false;
    ed_->selectionChanged();
}

int SoundView::handle(int e) {
    switch (e) {
    case FL_FOCUS:
    case FL_UNFOCUS:
        redraw();
        return 1;
    case FL_ENTER:
    case FL_MOVE:
    case FL_LEAVE:
        return 1;
    case FL_PUSH:
        take_focus();
        if (Fl::event_button() == FL_RIGHT_MOUSE) {       // the Edit menu, at the mouse
            const Fl_Menu_Item* m = ed_->editMenu_->menu();
            if (const Fl_Menu_Item* r = m ? m->popup(Fl::event_x(), Fl::event_y()) : nullptr) ed_->editMenu_->picked(r);
            return 1;
        }
        press(Fl::event_x(), Fl::event_y() < y() + kRulerH, Fl::event_shift() != 0);
        return 1;
    case FL_DRAG:
        dragTo(Fl::event_x());
        return 1;
    case FL_RELEASE:
        release(Fl::event_x());
        return 1;
    case FL_MOUSEWHEEL: {
        int dx = Fl::event_dx(), dy = Fl::event_dy();
        if (Fl::event_command() || Fl::event_ctrl() || Fl::event_alt()) {   // zoom around the mouse
            int s = dy ? dy : dx;
            if (s) ed_->zoom(s > 0 ? 1.25 : 0.8, ed_->frameAt(Fl::event_x()));
        } else {
            int px = dx ? dx : dy;
            ed_->scrollBy(px * 24 * ed_->framesPerPixel());
        }
        return 1;
    }
    }
    return Fl_Widget::handle(e);
}

// ---------------------------------------------------------------- the panel

SoundEditor::SoundEditor(int X, int Y, int W, int H, std::shared_ptr<SoundDoc> doc)
    : Fl_Group(X, Y, W, H), doc_(std::move(doc)) {
    box(FL_FLAT_BOX);
    color(kBgLocked);
    bar_ = new Fl_Group(X, Y, W, kToolH);
    bar_->box(FL_FLAT_BOX);
    bar_->color(kStatusBg);
    int bx = X + 8;
    const int by = Y + 5, bh = kToolH - 10;
    auto button = [&](int bw, ToolButton::Icon icon, const char* label, bool toggle, const std::string& tip,
                      Fl_Callback* cb, int gap = 4) {
        auto* b = new ToolButton(bx, by, bw, bh, icon, label, toggle);
        b->copy_tooltip(tip.c_str());
        b->callback(cb, this);
        bx += bw + gap;
        return b;
    };
    using E = SoundEditor;
    audioBtn_ = button(96, ToolButton::Power, "Audio", true, withShortcut("Start or stop audio", "patch.audio"),
                       [](Fl_Widget*, void* d) { App::get().toggleAudio(); static_cast<E*>(d)->refreshInfo(); }, 12);
    button(34, ToolButton::ToStart, "", false, "The playhead to the start (Home)", [](Fl_Widget*, void* d) {
        auto* e = static_cast<E*>(d);
        e->doc().playhead = 0;
        e->setView(0, e->framesPerPixel());
        e->selectionChanged();
    });
    playBtn_ = button(34, ToolButton::Play, "", true, "Play the selection, or from the playhead (Space)",
                      [](Fl_Widget*, void* d) { static_cast<E*>(d)->togglePlay(); });
    button(34, ToolButton::Stop, "", false, "Stop (Space)", [](Fl_Widget*, void* d) { static_cast<E*>(d)->stop(); });
    loopBtn_ = button(34, ToolButton::Loop, "", true, "Loop the selection (L)",
                      [](Fl_Widget* w, void* d) { static_cast<E*>(d)->setLoop(static_cast<ToolButton*>(w)->value() != 0); }, 8);
    position_ = new Fl_Box(bx, by, 104, bh);
    position_->box(FL_FLAT_BOX);
    position_->color(fl_rgb_color(14, 15, 17));
    position_->labelfont(FL_COURIER_BOLD);
    position_->labelsize(13);
    position_->labelcolor(fl_rgb_color(110, 220, 140));
    position_->tooltip("Where the playhead is (where the sound plays, while it plays)");
    bx += 104 + 14;
    waveBtn_ = button(64, ToolButton::None, "Wave", true, "The waveform only",
                      [](Fl_Widget*, void* d) { static_cast<E*>(d)->setMode(Mode::Wave); }, 2);
    specBtn_ = button(86, ToolButton::None, "Spectrum", true, "The spectrogram only",
                      [](Fl_Widget*, void* d) { static_cast<E*>(d)->setMode(Mode::Spectrum); }, 2);
    bothBtn_ = button(62, ToolButton::None, "Both", true, "The waveform and the spectrogram",
                      [](Fl_Widget*, void* d) { static_cast<E*>(d)->setMode(Mode::Both); }, 14);
    button(28, ToolButton::Minus, "", false, withShortcut("Zoom out", "view.zoomout"),
           [](Fl_Widget*, void* d) { static_cast<E*>(d)->zoomOut(); }, 2);
    button(46, ToolButton::None, "Fit", false, withShortcut("The whole sound", "view.actual"),
           [](Fl_Widget*, void* d) { static_cast<E*>(d)->fit(); }, 2);
    button(28, ToolButton::Plus, "", false, withShortcut("Zoom in", "view.zoomin"),
           [](Fl_Widget*, void* d) { static_cast<E*>(d)->zoomIn(); }, 14);
    editMenu_ = new Fl_Menu_Button(bx, by, 80, bh, "Edit");
    editMenu_->box(FL_FLAT_BOX);
    editMenu_->color(kBoxFill);
    editMenu_->labelcolor(kText);
    editMenu_->labelsize(12);
    editMenu_->labelfont(FL_HELVETICA_BOLD);
    editMenu_->textsize(12);
    editMenu_->clear_visible_focus();
    editMenu_->tooltip("Edit the selection (also the right button on the sound)");
    struct Item {
        const char* label;
        void (SoundEditor::*fn)();
        int flags;
    };
    static const Item items[] = {
        {"Cut", &E::cut, 0},           {"Copy", &E::copy, 0},         {"Paste", &E::paste, 0},
        {"Delete", &E::erase, 0},      {"Trim to Selection", &E::trim, FL_MENU_DIVIDER},
        {"Silence", &E::silence, 0},   {"Fade In", &E::fadeIn, 0},    {"Fade Out", &E::fadeOut, 0},
        {"Gain...", &E::gainDialog, 0}, {"Normalize", &E::normalize, 0}, {"Reverse", &E::reverse, FL_MENU_DIVIDER},
        {"Select All", &E::selectAll, 0}, {"Undo", &E::undo, 0},      {"Redo", &E::redo, 0},
    };
    for (const Item& it : items)
        editMenu_->add(it.label, 0, [](Fl_Widget* w, void* d) {
            auto* ed = static_cast<E*>(w->parent()->parent());
            (ed->*(static_cast<const Item*>(d)->fn))();
        }, (void*)&it, it.flags);
    bx += 80 + 8;
    auto* spacer = new Fl_Box(bx, Y, std::max(1, X + W - bx), kToolH);
    spacer->box(FL_NO_BOX);
    bar_->resizable(spacer);
    bar_->end();
    overview_ = new SoundOverview(X, Y + kToolH, W, kOverviewH, this);
    lanes_ = new Fl_Group(X, Y + kToolH + kOverviewH, W, 0);   // marker lanes (from soundtypes, onsets ...) later
    lanes_->end();
    lanes_->hide();
    view_ = new SoundView(X, Y + kToolH + kOverviewH, W, H - kToolH - kOverviewH - kStatusH, this);
    bottom_ = new Fl_Group(X, Y + H - kStatusH, W, 0);         // regions, later
    bottom_->box(FL_FLAT_BOX);
    bottom_->color(kStatusBg);
    bottom_->end();
    bottom_->hide();
    info_ = new Fl_Box(X, Y + H - kStatusH, W * 11 / 20, kStatusH);
    message_ = new Fl_Box(X + W * 11 / 20, Y + H - kStatusH, W - W * 11 / 20, kStatusH);
    for (Fl_Box* b : {info_, message_}) {
        b->box(FL_FLAT_BOX);
        b->color(kStatusBg);
        b->labelcolor(kDim);
        b->labelsize(11);
    }
    info_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    message_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    end();
    resizable(view_);
    doc_->onChanged = [this] { contentsChanged(); };
    audioBtn_->value(App::get().audio.running());
    audioBtn_->copy_label(App::get().audio.running() ? "Audio On" : "Audio Off");
    fit();
    updateModeButtons();
    setStatus("Space plays the selection · Shift-click extends it · " + shortcutText(FL_COMMAND) +
              "wheel zooms · right-click: Edit");
    refreshInfo();
    updateSpectrogram();
    Fl::add_timeout(0.1, tick, this);
}

SoundEditor::~SoundEditor() {
    Fl::remove_timeout(tick, this);
    job_.cancel();                                        // its done() is dropped
    if (playId_ && App::get().audio.preview().active(playId_)) App::get().audio.preview().stop();
    doc_->onChanged = nullptr;
}

void SoundEditor::resize(int X, int Y, int W, int H) {
    bool whole = start_ <= 0 && spp_ * view_->laneW() >= doc_->frames() - 1;
    Fl_Widget::resize(X, Y, W, H);
    layoutChildren();
    if (whole) fit();
    else setView(start_, spp_);
}

void SoundEditor::layoutChildren() {
    int X = x(), Y = y(), W = w(), H = h();
    bar_->resize(X, Y, W, kToolH);
    int top = Y + kToolH;
    overview_->resize(X, top, W, kOverviewH);
    top += kOverviewH;
    int lanesH = lanes_->visible() ? lanes_->h() : 0;
    lanes_->resize(X, top, W, lanesH);
    top += lanesH;
    int status = Y + H - kStatusH;
    int bottomH = bottom_->visible() ? kBottomH : 0;
    view_->resize(X, top, W, std::max(40, status - bottomH - top));
    bottom_->resize(X, status - bottomH, W, bottomH);
    info_->resize(X, status, W * 11 / 20, kStatusH);
    message_->resize(X + W * 11 / 20, status, W - W * 11 / 20, kStatusH);
    redraw();
}

// ---- the view

void SoundEditor::setView(double s, double spp) {
    double frames = (double)doc_->frames();
    int W = view_->laneW();
    double maxSpp = std::max(1.0 / 32, frames / W);
    spp = std::clamp(spp, 1.0 / 32, maxSpp);
    s = std::clamp(s, 0.0, std::max(0.0, frames - W * spp));
    if (s == start_ && spp == spp_) return;
    start_ = s;
    spp_ = spp;
    view_->redraw();
    overview_->redraw();
}

void SoundEditor::zoom(double factor, double around) {
    double px = (around - start_) / spp_;
    double spp = std::clamp(spp_ * factor, 1.0 / 32, std::max(1.0 / 32, (double)doc_->frames() / view_->laneW()));
    setView(around - px * spp, spp);
}

void SoundEditor::fit() {
    start_ = -1;                                          // drawn again even if it was already so
    setView(0, std::max(1.0, (double)doc_->frames()) / view_->laneW());
}

double SoundEditor::centre() const {
    if (!doc_->sel.empty()) {                             // the selection, when it is in view
        double a = (double)doc_->sel.start, b = (double)doc_->sel.end, e = start_ + spp_ * view_->laneW();
        if (b > start_ && a < e) return (std::max(a, start_) + std::min(b, e)) / 2;
    }
    return start_ + spp_ * view_->laneW() / 2;
}

double SoundEditor::frameAt(int ex) const { return start_ + (ex - view_->laneX()) * spp_; }

int SoundEditor::xOf(double frame) const { return view_->laneX() + (int)std::lround((frame - start_) / spp_); }

void SoundEditor::setMode(Mode m) {
    mode_ = m;
    updateModeButtons();
    updateSpectrogram();
    view_->redraw();
}

void SoundEditor::updateModeButtons() {
    waveBtn_->value(mode_ == Mode::Wave);
    specBtn_->value(mode_ == Mode::Spectrum);
    bothBtn_->value(mode_ == Mode::Both);
    for (auto* b : {waveBtn_, specBtn_, bothBtn_}) b->redraw();
}

// ---- playing

bool SoundEditor::playing() const { return playId_ && App::get().audio.preview().active(playId_); }

double SoundEditor::playPosition() const { return playId_ ? App::get().audio.preview().position(playId_) : -1.0; }

void SoundEditor::togglePlay() {
    if (playing()) stop();
    else play();
}

void SoundEditor::play() {
    App& app = App::get();
    SoundDoc& d = *doc_;
    playBtn_->value(0);
    if (!app.audio.running()) {
        setStatus("audio is off: turn it on (the Audio button, " + shortcutText(FL_COMMAND + '/') + ") to listen");
        return;
    }
    if (d.frames() == 0) {
        setStatus("the sound is empty");
        return;
    }
    PreviewPlayer& pp = app.audio.preview();
    if (!d.sel.empty()) {
        playId_ = pp.play(d.sound(), d.sel.start, d.sel.end, loop_);
        playFrom_ = d.sel.start;
    } else {                                              // from the playhead; a loop goes round the whole sound
        long at = d.playhead >= d.frames() ? 0 : d.playhead;
        playId_ = pp.play(d.sound(), 0, d.frames(), loop_, at);
        playFrom_ = at;
    }
    playBtn_->value(1);
    lastPos_ = -1;
    Fl::remove_timeout(tick, this);
    Fl::add_timeout(1.0 / 30, tick, this);
}

void SoundEditor::stop() {
    if (playing()) App::get().audio.preview().stop();   // only its own: another editor may be playing now
    playId_ = 0;
    playBtn_->value(0);
    lastPos_ = -1;
    view_->redraw();
    overview_->redraw();
    refreshInfo();
}

void SoundEditor::setLoop(bool on) {
    loop_ = on;
    loopBtn_->value(on);
    loopBtn_->redraw();
    App::get().audio.preview().setLoop(on);
}

void SoundEditor::tick(void* data) {
    auto* e = static_cast<SoundEditor*>(data);
    App& app = App::get();
    SoundDoc& d = *e->doc_;
    if (d.boundToBuffer() && d.followBuffer()) e->setStatus(d.message);   // replaced by another object
    if (e->audioBtn_->value() != (app.audio.running() ? 1 : 0)) {
        e->audioBtn_->value(app.audio.running());
        e->audioBtn_->copy_label(app.audio.running() ? "Audio On" : "Audio Off");
        e->audioBtn_->redraw();
    }
    if (++e->followCount_ % 10 == 0) {                    // the boxes using the buffer~, the title (gone?)
        e->refreshInfo();
        if (e->onTitle) e->onTitle();
    }
    if (e->playId_) {
        double p = e->playPosition();
        if (!e->playing()) {
            e->playId_ = 0;
            e->playBtn_->value(0);
            e->playBtn_->redraw();
            e->lastPos_ = -1;
            e->view_->redraw();
            e->overview_->redraw();
            e->refreshInfo();
        } else if (p >= 0 && p != e->lastPos_) {
            e->lastPos_ = p;
            double shown = e->spp_ * e->view_->laneW();
            if (p < e->start_ || p > e->start_ + shown) e->setView(p - shown * 0.05, e->spp_);   // the view follows
            e->view_->redraw();
            e->overview_->redraw();
            e->refreshInfo();
        }
    }
    app.audio.preview().collect();
    Fl::repeat_timeout(e->playId_ ? 1.0 / 30 : 0.1, tick, data);
}

// ---- edits

void SoundEditor::after(bool done) {
    (void)done;
    setStatus(doc_->message);
    selectionChanged();
    if (onTitle) onTitle();
}

void SoundEditor::cut() { after(doc_->cut()); }
void SoundEditor::copy() { after(doc_->copy()); }
void SoundEditor::paste() { after(doc_->paste()); }
void SoundEditor::erase() { after(doc_->erase()); }
void SoundEditor::trim() { after(doc_->trim()); }
void SoundEditor::silence() { after(doc_->silence()); }
void SoundEditor::fadeIn() { after(doc_->fadeIn()); }
void SoundEditor::fadeOut() { after(doc_->fadeOut()); }
void SoundEditor::gain(double db) { after(doc_->gain(db)); }
void SoundEditor::normalize() { after(doc_->normalize(0.0)); }
void SoundEditor::reverse() { after(doc_->reverse()); }
void SoundEditor::undo() { after(doc_->undo()); }
void SoundEditor::redo() { after(doc_->redo()); }

void SoundEditor::selectAll() {
    doc_->selectAll();
    selectionChanged();
}

void SoundEditor::gainDialog() {
    if (doc_->sel.empty()) {
        setStatus("Gain: select a part of the sound first");
        return;
    }
    if (batchMode()) {                                    // the tests give it with act=soundgain:<dB>
        std::printf("ask: Gain for the selection, in dB\n");
        std::fflush(stdout);
        return;
    }
    static std::string last = "-6";
    const char* r = fl_input("Gain for the selection, in dB (negative: softer):", last.c_str());
    if (!r) return;
    char* end = nullptr;
    double db = std::strtod(r, &end);
    if (end == r || !std::isfinite(db) || std::fabs(db) > 200) {
        setStatus("Gain: a number of dB, like -6 or 3");
        return;
    }
    last = r;
    gain(db);
}

// ---- showing

void SoundEditor::contentsChanged() {
    setView(start_, spp_);                                // the sound may be shorter now
    updateSpectrogram();
    view_->redraw();
    overview_->redraw();
    refreshInfo();
    if (onTitle) onTitle();
}

void SoundEditor::selectionChanged() {
    view_->redraw();
    overview_->redraw();
    refreshInfo();
}

void SoundEditor::setStatus(const std::string& s) {
    if (s == messageText_) return;
    messageText_ = s;
    message_->copy_label((s + "  ").c_str());
    message_->redraw();
}

void SoundEditor::refreshInfo() {
    SoundDoc& d = *doc_;
    char b[96];
    std::snprintf(b, sizeof b, "%s · %d ch · %s", rate(d.sr()).c_str(), d.channels(), formatTime(d.seconds(d.frames())).c_str());
    std::string s = "  " + std::string(b);
    if (!d.sel.empty()) {
        std::snprintf(b, sizeof b, "selection %.3f – %.3f s (%.3f s)", d.seconds(d.sel.start), d.seconds(d.sel.end),
                      d.seconds(d.sel.length()));
        s += "   ·   " + std::string(b);
    } else {
        std::snprintf(b, sizeof b, "playhead %.3f s", d.seconds(d.playhead));
        s += "   ·   " + std::string(b);
    }
    if (d.boundToBuffer()) {
        int patches = 0;
        auto users = d.users(&patches);
        if (!d.buffer()) s += "   ·   no buffer~ " + d.bufferName() + " now";
        else if (users.empty()) s += "   ·   used by no object";
        else {
            std::vector<std::string> names;
            for (auto& u : users)
                if (std::find(names.begin(), names.end(), u) == names.end()) names.push_back(u);
            s += "   ·   used by";
            for (auto& n : names) s += " " + n;
            s += " (" + std::to_string(users.size()) + " box" + (users.size() == 1 ? "" : "es") + " in " +
                 std::to_string(patches) + " patch" + (patches == 1 ? "" : "es") + ")";
        }
    }
    if (s != infoText_) {
        infoText_ = s;
        info_->copy_label(s.c_str());
        info_->redraw();
    }
    double p = playPosition();
    std::string pos = formatTime(d.seconds(p >= 0 ? (long)p : d.playhead));
    if (pos != positionText_) {
        positionText_ = pos;
        position_->copy_label(pos.c_str());
        position_->redraw();
    }
}

void SoundEditor::updateSpectrogram() {
    if (mode_ == Mode::Wave) return;
    uint64_t gen = doc_->generation();
    if (specGen_ == gen || (jobGen_ == gen && job_.running())) return;
    jobGen_ = gen;
    if (doc_->frames() == 0) {
        spec_ = std::make_shared<Spectrogram>();
        specGen_ = gen;
        return;
    }
    Sound s = doc_->sound();                              // the job reads its own snapshot, nothing else
    auto out = std::make_shared<Spectrogram>();
    auto ok = std::make_shared<bool>(false);
    job_.start([s, out, ok](Job& j) { *ok = computeSpectrogram(*s, *out, [&j](double) { return !j.cancelled(); }); },
               [this, out, ok, gen] {
                   job_.cancel();                // only joins its thread: it is done
                   if (!*ok) return;
                   spec_ = out;
                   specGen_ = gen;
                   view_->redraw();
               });
}

int SoundEditor::handle(int e) {
    if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::first_window() == window()) {
        int k = Fl::event_key();
        bool cmd = Fl::event_command() != 0, mods = cmd || Fl::event_ctrl() || Fl::event_alt();
        if (!mods) {
            switch (k) {
            case ' ': togglePlay(); return 1;
            case FL_BackSpace:
            case FL_Delete: erase(); return 1;
            case FL_Home:
                doc_->playhead = 0;
                setView(0, spp_);
                selectionChanged();
                return 1;
            case FL_End:
                doc_->playhead = doc_->frames();
                setView((double)doc_->frames(), spp_);
                selectionChanged();
                return 1;
            case FL_Left: scrollBy(-spp_ * view_->laneW() / 8); return 1;
            case FL_Right: scrollBy(spp_ * view_->laneW() / 8); return 1;
            case 'l': setLoop(!loop_); return 1;
            }
        } else if (cmd) {
            switch (k) {
            case 'a': selectAll(); return 1;
            case 'z': if (Fl::event_shift()) redo(); else undo(); return 1;
            case 'x': cut(); return 1;
            case 'c': copy(); return 1;
            case 'v': paste(); return 1;
            case '=':
            case '+': zoomIn(); return 1;
            case '-': zoomOut(); return 1;
            case '0': fit(); return 1;
            }
        }
    }
    return Fl_Group::handle(e);
}

}  // namespace moritz
