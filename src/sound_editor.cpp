// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_editor.cpp — the sound editor's panel (sound_editor.h): toolbar, overview, waveform and spectrogram,
// selection and playhead, the preview, the edits

#include "sound_editor.h"
#include "fileio.h"
#include "objects_maple.h"
#include "preview.h"
#include "sound_process.h"
#include "ui_util.h"

#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace moritz {

namespace {
constexpr int kOverviewH = 40;
constexpr int kBottomH = 176;
const Fl_Color kMarker = fl_rgb_color(245, 200, 40);
// a marker's colour says where it came from: by hand, onsets, beats, novelty (sections)
Fl_Color markerColor(const std::string& source) {
    if (source == "onsets") return kMarker;
    if (source == "beats") return fl_rgb_color(80, 200, 230);
    if (source == "novelty") return fl_rgb_color(235, 110, 200);
    return fl_rgb_color(235, 235, 240);
}
// a region's color by its type (A blue, B orange, C green, D pink ...), grey for none
Fl_Color typeColour(int t) {
    static const unsigned char k[][3] = {{74, 144, 226}, {232, 150, 40}, {46, 190, 140}, {226, 100, 170},
                                         {150, 110, 230}, {200, 200, 70}, {90, 200, 220}, {230, 90, 80}};
    if (t < 0) return fl_rgb_color(96, 100, 112);
    const unsigned char* c = k[t % 8];
    return fl_rgb_color(c[0], c[1], c[2]);
}
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
    int top = y() + kRulerH + kRegionsH + kMarkersH, avail = std::max(0, h() - kRulerH - kRegionsH - kMarkersH);
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
            const bool drawn = stroke_.active && stroke_.ch == c;
            for (long f = f0; f <= f1; ++f) {
                float v = s->data[(size_t)f * ch + c];
                if (drawn && f >= stroke_.lo && f < stroke_.lo + (long)stroke_.v.size()) v = stroke_.v[(size_t)(f - stroke_.lo)];
                int qx = ed_->xOf((double)f), qy = yOf(v);
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
    drawLanes(X, W);
    int top = y() + kRulerH + kRegionsH + kMarkersH, bottom = y() + h();
    fl_push_clip(X, top, W, std::max(0, bottom - top));
    // beats and sections drawn through the sound, faintly, in their colour (onsets are many: only in their lane)
    for (const Marker& mk : d.marks().markers) {
        if (mk.source != "beats" && mk.source != "novelty") continue;
        int px = ed_->xOf((double)mk.at);
        if (px < X || px > X + W) continue;
        fl_color(fl_color_average(markerColor(mk.source), kLaneBg, 0.45f));
        fl_yxline(px, top, bottom);
    }
    if (ed_->selMarker_ >= 0 && ed_->selMarker_ < (int)d.marks().markers.size()) {   // the marker chosen, through
        fl_color(kMarker);
        fl_line_style(FL_DOT);
        fl_yxline(ed_->xOf((double)d.marks().markers[(size_t)ed_->selMarker_].at), top, bottom);
        fl_line_style(0);
    }
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
        fl_draw("empty: paste a sound here", X, top, W, std::max(0, bottom - top), FL_ALIGN_CENTER);
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
    case FL_PUSH: {
        take_focus();
        SoundDoc& d = ed_->doc();
        int ex = Fl::event_x(), ey = Fl::event_y();
        bool inMarkers = ey >= markersY() && ey < markersY() + kMarkersH, inRegions = ey >= regionsY() && ey < markersY();
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
            if (inMarkers && markerAt(ex) >= 0) { markerMenu(markerAt(ex)); return 1; }
            const Fl_Menu_Item* m = ed_->editMenu_->menu();   // the Edit menu, at the mouse
            if (const Fl_Menu_Item* r = m ? m->popup(ex, ey) : nullptr) ed_->editMenu_->picked(r);
            return 1;
        }
        long f = std::clamp((long)std::lround(ed_->frameAt(ex)), 0L, d.frames());
        if (ey < y() + kRulerH) {
            press(ex, true, false);
        } else if (inRegions) {
            int k = regionAt(ex);
            if (k >= 0) {
                ed_->selectRegion(k);
                if (Fl::event_clicks()) ed_->renameRegionDialog(k);
            } else {
                press(ex, true, false);
            }
        } else if (inMarkers) {
            int k = markerAt(ex);
            if (k >= 0) {
                ed_->selMarker_ = k;
                if (Fl::event_clicks()) {
                    markerMenu(k);
                    return 1;
                }
                dragMarker_ = k;
                dragMarkerTo_ = d.marks().markers[(size_t)k].at;
                moved_ = false;
                ed_->selectionChanged();
            } else if (ed_->tool() == SoundEditor::Tool::Marker) {
                ed_->addMarkerAt(f);
            } else {
                ed_->selMarker_ = -1;
                press(ex, true, false);
            }
        } else if (ed_->tool() == SoundEditor::Tool::Draw) {
            double v = 0;
            int c = channelAt(ey, &v);
            if (!ed_->canDraw()) {
                ed_->setStatus("Draw: zoom in until each sample has a pixel (" + shortcutText(FL_COMMAND) + "wheel, or pinch)");
            } else if (c >= 0 && d.frames() > 0) {
                long g = std::clamp((long)std::lround(ed_->frameAt(ex)), 0L, d.frames() - 1);
                stroke_.active = true;
                stroke_.ch = c;
                stroke_.lo = g;
                stroke_.v = {d.sound()->data[(size_t)g * d.channels() + c]};
                stroke_.lastF = g;
                stroke_.lastV = v;
                strokeTo((double)g, v);
            }
        } else if (ed_->tool() == SoundEditor::Tool::Marker) {
            ed_->addMarkerAt(f);
        } else {
            press(ex, false, Fl::event_shift() != 0);
        }
        return 1;
    }
    case FL_DRAG:
        if (dragMarker_ >= 0) {
            dragMarkerTo_ = std::clamp((long)std::lround(ed_->frameAt(Fl::event_x())), 0L, ed_->doc().frames());
            moved_ = true;
            redraw();
        } else if (stroke_.active) {
            double v = 0;
            int wy, wh, sy, sh;
            areas(wy, wh, sy, sh);
            int ch = std::max(1, ed_->doc().channels()), laneH = wh / ch;
            int ly = wy + stroke_.ch * laneH, lh = stroke_.ch == ch - 1 ? wh - stroke_.ch * laneH : laneH;
            double amp = std::max(1, lh / 2 - 3);
            v = (ly + lh / 2 - Fl::event_y()) / amp;
            strokeTo(ed_->frameAt(Fl::event_x()), v);
        } else {
            dragTo(Fl::event_x());
        }
        return 1;
    case FL_RELEASE:
        if (dragMarker_ >= 0) {
            int k = dragMarker_;
            dragMarker_ = -1;
            if (moved_) ed_->moveMarker(k, dragMarkerTo_);
            moved_ = false;
            redraw();
        } else if (stroke_.active) {
            endStroke();
        } else {
            release(Fl::event_x());
        }
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
    loopBtn_ = button(34, ToolButton::Loop, "", true, "Loop the selection (" + shortcutText(FL_COMMAND + 'l') + ")",
                      [](Fl_Widget* w, void* d) { static_cast<E*>(d)->setLoop(static_cast<ToolButton*>(w)->value() != 0); }, 8);
    position_ = new Fl_Box(bx, by, 104, bh);
    position_->box(FL_FLAT_BOX);
    position_->color(fl_rgb_color(14, 15, 17));
    position_->labelfont(FL_COURIER_BOLD);
    position_->labelsize(13);
    position_->labelcolor(fl_rgb_color(110, 220, 140));
    position_->tooltip("Where the playhead is (where the sound plays, while it plays)");
    bx += 104 + 14;
    toolBtns_[0] = button(58, ToolButton::None, "Select", true, "Select: drag to select (Shift extends)",
                          [](Fl_Widget*, void* d) { static_cast<E*>(d)->setTool(Tool::Select); }, 2);
    toolBtns_[1] = button(50, ToolButton::None, "Draw", true, "Draw: zoomed in, drag samples with the mouse (one undo step a stroke)",
                          [](Fl_Widget*, void* d) { static_cast<E*>(d)->setTool(Tool::Draw); }, 2);
    toolBtns_[2] = button(62, ToolButton::None, "Marker", true, "Marker: click to place a marker (M: at the playhead)",
                          [](Fl_Widget*, void* d) { static_cast<E*>(d)->setTool(Tool::Marker); }, 14);
    waveBtn_ = button(56, ToolButton::None, "Wave", true, "The waveform only",
                      [](Fl_Widget*, void* d) { static_cast<E*>(d)->setMode(Mode::Wave); }, 2);
    specBtn_ = button(80, ToolButton::None, "Spectrum", true, "The spectrogram only",
                      [](Fl_Widget*, void* d) { static_cast<E*>(d)->setMode(Mode::Spectrum); }, 2);
    bothBtn_ = button(54, ToolButton::None, "Both", true, "The waveform and the spectrogram",
                      [](Fl_Widget*, void* d) { static_cast<E*>(d)->setMode(Mode::Both); }, 14);
    button(28, ToolButton::Minus, "", false, withShortcut("Zoom out", "view.zoomout"),
           [](Fl_Widget*, void* d) { static_cast<E*>(d)->zoomOut(); }, 2);
    button(46, ToolButton::None, "Fit", false, withShortcut("The whole sound", "view.actual"),
           [](Fl_Widget*, void* d) { static_cast<E*>(d)->fit(); }, 2);
    button(28, ToolButton::Plus, "", false, withShortcut("Zoom in", "view.zoomin"),
           [](Fl_Widget*, void* d) { static_cast<E*>(d)->zoomIn(); }, 14);
    auto menuButton = [&](int bw, const char* label, const char* tip, Fl_Color c) {
        auto* m = new Fl_Menu_Button(bx, by, bw, bh, label);
        m->box(FL_FLAT_BOX);
        m->color(c);
        m->labelcolor(kText);
        m->labelsize(12);
        m->labelfont(FL_HELVETICA_BOLD);
        m->textsize(12);
        m->clear_visible_focus();
        m->tooltip(tip);
        bx += bw + 6;
        return m;
    };
    editMenu_ = menuButton(64, "Edit", "Edit the selection (also the right button on the sound)", kBoxFill);
    processMenu_ = menuButton(84, "Process", "Process the selection (or all of the sound) with an object, a patch or a script",
                              fl_rgb_color(48, 84, 150));
    marksMenu_ = menuButton(84, "Markers", "Markers and regions: by hand, from onsets, beats, novelty or soundtypes", kBoxFill);

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
    bx += 2;
    auto* spacer = new Fl_Box(bx, Y, std::max(1, X + W - bx), kToolH);
    spacer->box(FL_NO_BOX);
    bar_->resizable(spacer);
    bar_->end();
    overview_ = new SoundOverview(X, Y + kToolH, W, kOverviewH, this);
    view_ = new SoundView(X, Y + kToolH + kOverviewH, W, H - kToolH - kOverviewH - kStatusH - kBottomH, this);
    buildBottom(X, Y + H - kStatusH - kBottomH, W);
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
    doc_->onMarks = [this] { marksChanged(); };
    buildProcessMenu();
    buildMarksMenu();
    updateToolButtons();
    audioBtn_->value(App::get().audio.running());
    audioBtn_->copy_label(App::get().audio.running() ? "Audio On" : "Audio Off");
    fit();
    updateModeButtons();
    setStatus(!doc_->message.empty() ? doc_->message
                                     : "Space plays the selection · Shift-click extends it · M adds a marker · " +
                                           shortcutText(FL_COMMAND) + "wheel zooms · right-click: Edit");
    refreshInfo();
    updateSpectrogram();
    Fl::add_timeout(0.1, tick, this);
}

SoundEditor::~SoundEditor() {
    Fl::remove_timeout(tick, this);
    job_.cancel();                                        // its done() is dropped
    work_.cancel();                                       // a process: stopped and waited for, then its patch goes
    patchProc_.reset();
    if (script_) script_->stop();                         // its thread ends on its own; its result is dropped
    script_.reset();
    doc_->onMarks = nullptr;
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
    int status = Y + H - kStatusH;
    int bottomH = bottom_->visible() ? kBottomH : 0;
    view_->resize(X, top, W, std::max(40, status - bottomH - top));
    bottom_->resize(X, status - bottomH, W, bottomH);
    layoutBottom();
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
    // replaced by another object (at once), or written by record~ or poke~ (looked at three times a second)
    if (d.boundToBuffer() && !e->view_->stroking() && d.followBuffer(e->followCount_ % 3 == 0)) e->setStatus(d.message);
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
    e->refreshMenus();
    Fl::repeat_timeout(e->playId_ ? 1.0 / 30 : 0.1, tick, data);
}

// ---- edits

void SoundEditor::after(bool done) {
    (void)done;
    if (selMarker_ >= (int)doc_->marks().markers.size()) selMarker_ = -1;
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
    refreshSelectionInfo();
    if (onTitle) onTitle();
}

void SoundEditor::marksChanged() {
    if (selMarker_ >= (int)doc_->marks().markers.size()) selMarker_ = -1;
    view_->redraw();
    table_->redraw();
    refreshSelectionInfo();
    if (onTitle) onTitle();
}

void SoundEditor::selectionChanged() {
    view_->redraw();
    overview_->redraw();
    table_->redraw();
    refreshInfo();
    refreshSelectionInfo();
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
    if (e == FL_ZOOM_GESTURE) {                           // a trackpad's pinch (macOS): zoom around the mouse
        int ex = Fl::event_x();
        double around = ex >= view_->laneX() && ex < view_->laneX() + view_->laneW() ? frameAt(ex) : centre();
        double m = Fl::event_dy() / 1000.0;               // the magnification: > 0 spreads the fingers
        if (m != 0) zoom(std::clamp(1.0 / (1.0 + m), 0.25, 4.0), around);
        return 1;
    }
    if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::first_window() == window()) {
        int k = Fl::event_key();
        bool cmd = Fl::event_command() != 0, mods = cmd || Fl::event_ctrl() || Fl::event_alt();
        if (!mods) {
            switch (k) {
            case ' ': togglePlay(); return 1;
            case FL_BackSpace:
            case FL_Delete:
                if (selMarker_ >= 0) deleteMarker(selMarker_);   // the marker chosen, else the samples
                else erase();
                return 1;
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
            case 'm': addMarkerAtPlayhead(); return 1;
            }
        } else if (cmd) {
            switch (k) {
            case 'l': setLoop(!loop_); return 1;
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

// ---------------------------------------------------------------- the lanes, the Draw tool

int SoundView::valueY(int c, double v) const {
    int wy, wh, sy, sh;
    areas(wy, wh, sy, sh);
    int ch = std::max(1, ed_->doc().channels()), laneH = wh / ch;
    c = std::clamp(c, 0, ch - 1);
    int ly = wy + c * laneH, lh = c == ch - 1 ? wh - c * laneH : laneH;
    double amp = std::max(1, lh / 2 - 3);
    return ly + lh / 2 - (int)std::lround(v * amp);
}

int SoundView::channelAt(int ey, double* value) const {
    int wy, wh, sy, sh;
    areas(wy, wh, sy, sh);
    if (wh <= 0 || ey < wy || ey >= wy + wh) return -1;
    int ch = std::max(1, ed_->doc().channels()), laneH = std::max(1, wh / ch);
    int c = std::clamp((ey - wy) / laneH, 0, ch - 1);
    int ly = wy + c * laneH, lh = c == ch - 1 ? wh - c * laneH : laneH;
    double amp = std::max(1, lh / 2 - 3);
    if (value) *value = (ly + lh / 2 - ey) / amp;
    return c;
}

int SoundView::markerAt(int ex) const {
    const auto& m = ed_->doc().marks().markers;
    int best = -1, dist = 6;
    for (size_t k = 0; k < m.size(); ++k) {
        int d = std::abs(ed_->xOf((double)m[k].at) - ex);
        if (d < dist) { dist = d; best = (int)k; }
    }
    return best;
}

int SoundView::regionAt(int ex) const {
    const auto& r = ed_->doc().marks().regions;
    double f = ed_->frameAt(ex);
    int found = -1;
    for (size_t k = 0; k < r.size(); ++k)
        if (f >= (double)r[k].start && f < (double)r[k].end) found = (int)k;   // the last one that starts before
    return found;
}

void SoundView::drawLanes(int X, int W) {
    SoundDoc& d = ed_->doc();
    int ry = regionsY(), my = markersY();
    fl_color(kStatusBg);
    fl_rectf(x(), ry, w(), kRegionsH + kMarkersH);
    fl_font(FL_HELVETICA, 10);
    fl_color(kDim);
    fl_draw("regions", x() + 4, ry, kGutter - 6, kRegionsH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    fl_draw("markers", x() + 4, my, kGutter - 6, kMarkersH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    fl_push_clip(X, ry, W, kRegionsH + kMarkersH);
    const SoundMarks& m = d.marks();
    int selR = ed_->selectedRegion();
    for (size_t k = 0; k < m.regions.size(); ++k) {
        const Region& r = m.regions[k];
        int a = ed_->xOf((double)r.start), b = ed_->xOf((double)r.end);
        if (b < X || a > X + W) continue;
        fl_color(typeColour(r.type));
        fl_rectf(a + 1, ry + 2, std::max(1, b - a - 1), kRegionsH - 4);
        if ((int)k == selR) {
            fl_color(FL_WHITE);
            fl_rect(a, ry + 1, std::max(2, b - a + 1), kRegionsH - 2);
        }
        if (b - a > 14) {
            fl_color(fl_rgb_color(16, 18, 24));
            fl_font(FL_HELVETICA_BOLD, 10);
            std::string t = r.type >= 0 ? typeName(r.type) : r.name;
            fl_draw(t.c_str(), a + 4, ry + 2, b - a - 6, kRegionsH - 4, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        }
    }
    for (size_t k = 0; k < m.markers.size(); ++k) {
        long at = (int)k == dragMarker_ ? dragMarkerTo_ : m.markers[k].at;
        int px = ed_->xOf((double)at);
        if (px < X - 6 || px > X + W + 6) continue;
        fl_color((int)k == ed_->selMarker_ ? FL_WHITE : markerColor(m.markers[k].source));
        fl_polygon(px - 4, my + 3, px + 4, my + 3, px, my + 11);
    }
    fl_pop_clip();
    fl_color(kGrid);
    fl_xyline(x(), my + kMarkersH - 1, x() + w());
}

void SoundView::strokeTo(double frame, double value) {
    SoundDoc& d = ed_->doc();
    if (!stroke_.active || d.frames() <= 0) return;
    const int ch = d.channels();
    const float* x = d.sound()->data.data();
    long to = std::clamp((long)std::lround(frame), 0L, d.frames() - 1);
    value = std::clamp(value, -1.0, 1.0);
    auto set = [&](long f, double v) {
        if (f < stroke_.lo) {                             // grown to the left: the samples there as they are
            std::vector<float> before((size_t)(stroke_.lo - f));
            for (long g = f; g < stroke_.lo; ++g) before[(size_t)(g - f)] = x[(size_t)g * ch + stroke_.ch];
            stroke_.v.insert(stroke_.v.begin(), before.begin(), before.end());
            stroke_.lo = f;
        }
        while (f >= stroke_.lo + (long)stroke_.v.size()) {
            long g = stroke_.lo + (long)stroke_.v.size();
            stroke_.v.push_back(x[(size_t)g * ch + stroke_.ch]);
        }
        stroke_.v[(size_t)(f - stroke_.lo)] = (float)v;
    };
    // every sample between the last point and this one, on the line joining them: none is skipped
    long from = stroke_.lastF;
    if (to == from) {
        set(to, value);
    } else {
        long step = to > from ? 1 : -1;
        for (long f = from + step;; f += step) {
            double t = (double)(f - from) / (double)(to - from);
            set(f, stroke_.lastV + t * (value - stroke_.lastV));
            if (f == to) break;
        }
    }
    stroke_.lastF = to;
    stroke_.lastV = value;
    redraw();
}

void SoundView::endStroke() {
    SoundDoc& d = ed_->doc();
    Stroke s = std::move(stroke_);
    stroke_ = Stroke();
    if (s.v.empty() || d.frames() <= 0) return;
    const int ch = d.channels();
    long n = std::min((long)s.v.size(), d.frames() - s.lo);
    if (n <= 0) return;
    std::vector<float> frames(d.sound()->data.begin() + (long)((size_t)s.lo * ch),
                              d.sound()->data.begin() + (long)((size_t)(s.lo + n) * ch));
    for (long f = 0; f < n; ++f) frames[(size_t)f * ch + s.ch] = s.v[(size_t)f];
    ed_->after(d.replaceFrames("Draw", s.lo, frames));
}

void SoundView::markerMenu(int k) {
    SoundDoc& d = ed_->doc();
    if (k < 0 || k >= (int)d.marks().markers.size()) return;
    ed_->selMarker_ = k;
    redraw();
    if (batchMode()) {
        std::printf("ask: marker %s: Rename..., Delete\n", d.marks().markers[(size_t)k].name.c_str());
        std::fflush(stdout);
        return;
    }
    Fl_Menu_Item items[3];
    std::memset(items, 0, sizeof items);
    items[0].text = "Rename...";
    items[1].text = "Delete";
    const Fl_Menu_Item* r = items->popup(Fl::event_x(), Fl::event_y());
    if (r == &items[0]) {
        const char* v = fl_input("Name of the marker:", d.marks().markers[(size_t)k].name.c_str());
        if (v) ed_->renameMarker(k, v);
    } else if (r == &items[1]) {
        ed_->deleteMarker(k);
    }
}

// ---------------------------------------------------------------- the regions table

namespace {
const int kCols[] = {28, 132, 66, 66, 44, 60, 54};    // # name start end type energy pitch
const char* const kColNames[] = {"#", "name", "start", "end", "type", "energy", "pitch"};
}  // namespace

void RegionTable::draw() {
    fl_push_clip(x(), y(), w(), h());
    fl_color(kStatusBg);
    fl_rectf(x(), y(), w(), h());
    fl_font(FL_HELVETICA, 11);
    int cx = x() + 6;
    fl_color(kDim);
    for (int c = 0; c < 7; ++c) {
        fl_draw(kColNames[c], cx, y(), kCols[c], kRowH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        cx += kCols[c];
    }
    const auto& rs = ed_->doc().marks().regions;
    double sr = ed_->doc().sr() > 0 ? ed_->doc().sr() : 44100.0;
    int sel = ed_->selectedRegion();
    int rows = std::max(0, (h() - kRowH) / kRowH);
    top_ = std::clamp(top_, 0, std::max(0, (int)rs.size() - rows));
    if (rs.empty()) {
        fl_color(kDim);
        fl_draw("no regions yet: select a part and Add, or make them From types or From onsets", x() + 6, y() + kRowH,
                w() - 12, kRowH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    }
    for (int r = 0; r < rows && top_ + r < (int)rs.size(); ++r) {
        int k = top_ + r, ry = y() + kRowH * (r + 1);
        const Region& g = rs[(size_t)k];
        if (k == sel) {
            fl_color(fl_rgb_color(40, 70, 140));
            fl_rectf(x() + 2, ry, w() - 4, kRowH);
        }
        char b[7][64];
        std::snprintf(b[0], 64, "%d", k + 1);
        std::snprintf(b[1], 64, "%s", g.name.c_str());
        std::snprintf(b[2], 64, "%.3f", g.start / sr);
        std::snprintf(b[3], 64, "%.3f", g.end / sr);
        std::snprintf(b[4], 64, "%s", typeName(g.type).c_str());
        std::snprintf(b[5], 64, "%.2f", g.energy);
        std::snprintf(b[6], 64, "%s", pitchName(g.pitch).c_str());
        cx = x() + 6;
        for (int c = 0; c < 7; ++c) {
            if (c == 4 && g.type >= 0) {                      // the type, in its color
                fl_color(typeColour(g.type));
                fl_rectf(cx, ry + 3, 14, kRowH - 6);
                fl_color(fl_rgb_color(16, 18, 24));
                fl_font(FL_HELVETICA_BOLD, 10);
                fl_draw(b[c], cx, ry + 3, 14, kRowH - 6, FL_ALIGN_CENTER | FL_ALIGN_CLIP);
                fl_font(FL_HELVETICA, 11);
            } else {
                fl_color(c == 0 ? kDim : kText);
                fl_draw(b[c], cx, ry, kCols[c] - 4, kRowH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
            }
            cx += kCols[c];
        }
    }
    fl_pop_clip();
}

int RegionTable::rowAt(int ey) const {
    int r = (ey - y()) / kRowH - 1;
    if (ey < y() + kRowH || r < 0) return -1;
    int k = top_ + r;
    return k < (int)ed_->doc().marks().regions.size() ? k : -1;
}

void RegionTable::showRow(int k) {
    int rows = std::max(1, (h() - kRowH) / kRowH);
    if (k < top_) top_ = k;
    else if (k >= top_ + rows) top_ = k - rows + 1;
    redraw();
}

int RegionTable::handle(int e) {
    switch (e) {
    case FL_ENTER:
    case FL_MOVE:
    case FL_LEAVE:
        return 1;
    case FL_PUSH: {
        int k = rowAt(Fl::event_y());
        if (k >= 0) {
            ed_->selectRegion(k);
            if (Fl::event_clicks()) ed_->renameRegionDialog(k);
        }
        return 1;
    }
    case FL_MOUSEWHEEL:
        top_ += Fl::event_dy() > 0 ? 2 : Fl::event_dy() < 0 ? -2 : 0;
        redraw();
        return 1;
    }
    return Fl_Widget::handle(e);
}

// ---------------------------------------------------------------- the regions panel


void SoundEditor::buildBottom(int X, int Y, int W) {
    bottom_ = new Fl_Group(X, Y, W, kBottomH);
    bottom_->box(FL_FLAT_BOX);
    bottom_->color(kStatusBg);
    regionsTitle_ = new Fl_Box(X, Y, 80, 24, "REGIONS");
    regionsTitle_->labelfont(FL_HELVETICA_BOLD);
    regionsTitle_->labelsize(12);
    regionsTitle_->labelcolor(kDim);
    regionsTitle_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    using E = SoundEditor;
    struct B {
        const char* label;
        const char* tip;
        Fl_Callback* cb;
    };
    static const B buttons[] = {
        {"From types", "Regions from soundtypes' units, colored by type (a background job)",
         [](Fl_Widget*, void* d) { static_cast<E*>(d)->regionsFrom("types"); }},
        {"From onsets", "Regions between consecutive onsets (a background job)",
         [](Fl_Widget*, void* d) { static_cast<E*>(d)->regionsFrom("onsets"); }},
        {"Add", "The selection made a region", [](Fl_Widget*, void* d) { static_cast<E*>(d)->addRegion(); }},
        {"Delete", "Delete the region selected", [](Fl_Widget*, void* d) {
             auto* e = static_cast<E*>(d);
             e->deleteRegion(e->selectedRegion());
         }},
        {"To score \xe2\x96\xb8", "A .score with a sound event for each region (the score object reads it)", [](Fl_Widget*, void* d) {
             static_cast<E*>(d)->toScore();
         }},
        {"To coll...", "The regions written as rows (start end type energy pitch name), for a coll",
         [](Fl_Widget*, void* d) { static_cast<E*>(d)->toColl(); }},
    };
    for (const B& b : buttons) {
        auto* w = new ToolButton(X, Y, 60, 22, ToolButton::None, b.label, false);
        w->labelsize(11);
        w->tooltip(b.tip);
        w->callback(b.cb, this);
        regionButtons_.push_back(w);
    }
    toScore_ = regionButtons_[4];
    if (!scoreAvailable()) {
        toScore_->deactivate();
        toScore_->labelcolor(kDim);
    }
    table_ = new RegionTable(X, Y, W / 2, kBottomH, this);
    selInfo_ = new Fl_Box(X + W / 2, Y, W / 2, kBottomH);
    selInfo_->box(FL_FLAT_BOX);
    selInfo_->color(kStatusBg);
    selInfo_->labelcolor(kText);
    selInfo_->labelsize(11);
    selInfo_->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_CLIP | FL_ALIGN_WRAP);
    bottom_->end();
    bottom_->resizable(nullptr);
    layoutBottom();
}

void SoundEditor::layoutBottom() {
    if (!bottom_ || !table_) return;
    int X = bottom_->x(), Y = bottom_->y(), W = bottom_->w(), H = bottom_->h();
    int left = std::max(420, W * 56 / 100);
    regionsTitle_->resize(X + 12, Y + 6, 72, 22);
    static const int widths[] = {78, 82, 46, 56, 78, 72};
    int bx = X + 90;
    for (size_t k = 0; k < regionButtons_.size(); ++k) {
        regionButtons_[k]->resize(bx, Y + 6, widths[k], 22);
        bx += widths[k] + 4;
    }
    table_->resize(X + 6, Y + 34, std::max(10, left - 12), std::max(10, H - 38));
    selInfo_->resize(X + left + 8, Y + 8, std::max(10, W - left - 16), std::max(10, H - 16));
}

bool SoundEditor::scoreAvailable() const { return describeClass("score")[0] != 0; }   // a score object exists

void SoundEditor::refreshSelectionInfo() {
    if (!selInfo_) return;
    SoundDoc& d = *doc_;
    char b[200];
    std::string s = "SELECTION\n\n";
    if (!d.sel.empty()) {
        std::snprintf(b, sizeof b, "%.3f \xe2\x80\x93 %.3f s (%.3f s)", d.seconds(d.sel.start), d.seconds(d.sel.end),
                      d.seconds(d.sel.length()));
        s += b;
        int k = selectedRegion();
        if (k >= 0) {
            const Region& r = d.marks().regions[(size_t)k];
            s += " \xc2\xb7 region " + std::to_string(k + 1) + (r.type >= 0 ? ", type " + typeName(r.type) : "");
        }
    } else {
        std::snprintf(b, sizeof b, "nothing selected; the playhead at %.3f s", d.seconds(d.playhead));
        s += b;
    }
    s += "\n" + std::to_string(d.marks().markers.size()) + " markers \xc2\xb7 " + std::to_string(d.marks().regions.size()) +
         " regions";
    if (d.boundToBuffer()) {
        int patches = 0;
        auto users = d.users(&patches);
        std::vector<std::string> names;
        for (auto& u : users)
            if (std::find(names.begin(), names.end(), u) == names.end()) names.push_back(u);
        s += "\nbuffer~ " + d.bufferName() + (names.empty() ? ", used by no object" : ", used by:");
        for (auto& n : names) s += " " + n;
    }
    std::string name = d.boundToBuffer() ? "buffer~ " + d.bufferName() : d.path().empty() ? "Untitled sound" : baseName(d.path());
    std::snprintf(b, sizeof b, "%s \xc2\xb7 %s \xc2\xb7 %d ch \xc2\xb7 %.2f s%s", name.c_str(), rate(d.sr()).c_str(), d.channels(),
                  d.seconds(d.frames()), d.needsSaving() ? " \xc2\xb7 edited" : "");
    s += "\n";
    s += b;
    if (s != selInfoText_) {
        selInfoText_ = s;
        selInfo_->copy_label(s.c_str());
        selInfo_->redraw();
    }
}

// ---------------------------------------------------------------- tools

void SoundEditor::setTool(Tool t) {
    tool_ = t;
    updateToolButtons();
    switch (t) {
    case Tool::Select: setStatus("Select: drag to select, Shift extends"); break;
    case Tool::Draw:
        setStatus(canDraw() ? "Draw: drag the samples (a stroke is one undo step)"
                            : "Draw: zoom in until each sample has a pixel, then drag the samples");
        break;
    case Tool::Marker: setStatus("Marker: click to place a marker (M: at the playhead)"); break;
    }
}

void SoundEditor::updateToolButtons() {
    for (int k = 0; k < 3; ++k) {
        toolBtns_[k]->value((int)tool_ == k);
        toolBtns_[k]->redraw();
    }
}

// ---------------------------------------------------------------- markers and regions

void SoundEditor::addMarkerAt(long frame, const std::string& source) {
    SoundMarks m = doc_->marks();
    Marker k;
    k.at = std::clamp(frame, 0L, doc_->frames());
    k.name = "marker " + std::to_string(m.markers.size() + 1);
    k.source = source;
    m.markers.push_back(k);
    bool ok = doc_->setMarks("Add Marker", std::move(m));
    if (ok) {
        for (size_t j = 0; j < doc_->marks().markers.size(); ++j)
            if (doc_->marks().markers[j].at == k.at && doc_->marks().markers[j].name == k.name) selMarker_ = (int)j;
        doc_->message = "a marker at " + formatTime(doc_->seconds(k.at));
    }
    after(ok);
}

void SoundEditor::deleteMarker(int k) {
    SoundMarks m = doc_->marks();
    if (k < 0 || k >= (int)m.markers.size()) { setStatus("Delete Marker: click a marker first"); return; }
    m.markers.erase(m.markers.begin() + k);
    selMarker_ = -1;
    after(doc_->setMarks("Delete Marker", std::move(m)));
}

void SoundEditor::moveMarker(int k, long frame) {
    SoundMarks m = doc_->marks();
    if (k < 0 || k >= (int)m.markers.size()) return;
    m.markers[(size_t)k].at = std::clamp(frame, 0L, doc_->frames());
    std::string name = m.markers[(size_t)k].name;
    bool ok = doc_->setMarks("Move Marker", std::move(m));
    for (size_t j = 0; j < doc_->marks().markers.size(); ++j)
        if (doc_->marks().markers[j].name == name && doc_->marks().markers[j].at == std::clamp(frame, 0L, doc_->frames()))
            selMarker_ = (int)j;
    after(ok);
}

void SoundEditor::renameMarker(int k, const std::string& name) {
    SoundMarks m = doc_->marks();
    if (k < 0 || k >= (int)m.markers.size()) return;
    m.markers[(size_t)k].name = name.substr(0, 200);
    after(doc_->setMarks("Rename Marker", std::move(m)));
}

void SoundEditor::clearMarkers() {
    SoundMarks m = doc_->marks();
    m.markers.clear();
    selMarker_ = -1;
    after(doc_->setMarks("Clear Markers", std::move(m)));
}

void SoundEditor::addRegion() {
    if (doc_->sel.empty()) { setStatus("Add Region: select a part of the sound first"); return; }
    SoundMarks m = doc_->marks();
    Region r;
    r.start = doc_->sel.start;
    r.end = doc_->sel.end;
    r.name = "region " + std::to_string(m.regions.size() + 1);
    measureRegion(*doc_->sound(), r);                 // its energy and pitch (bounded: a few dozen frames)
    m.regions.push_back(r);
    bool ok = doc_->setMarks("Add Region", std::move(m));
    if (ok) doc_->message = r.name + ": " + formatTime(doc_->seconds(r.start)) + " to " + formatTime(doc_->seconds(r.end));
    after(ok);
    int k = selectedRegion();
    if (k >= 0) table_->showRow(k);
}

void SoundEditor::deleteRegion(int k) {
    SoundMarks m = doc_->marks();
    if (k < 0 || k >= (int)m.regions.size()) { setStatus("Delete Region: click a region first"); return; }
    m.regions.erase(m.regions.begin() + k);
    after(doc_->setMarks("Delete Region", std::move(m)));
}

void SoundEditor::renameRegion(int k, const std::string& name) {
    SoundMarks m = doc_->marks();
    if (k < 0 || k >= (int)m.regions.size()) return;
    m.regions[(size_t)k].name = name.substr(0, 200);
    after(doc_->setMarks("Rename Region", std::move(m)));
}

void SoundEditor::renameRegionDialog(int k) {
    if (k < 0 || k >= (int)doc_->marks().regions.size()) return;
    if (batchMode()) {                                    // the tests rename with act=soundmark:rename:k:name
        std::printf("ask: Name of region %d (%s)\n", k + 1, doc_->marks().regions[(size_t)k].name.c_str());
        std::fflush(stdout);
        return;
    }
    const char* v = fl_input("Name of the region:", doc_->marks().regions[(size_t)k].name.c_str());
    if (v) renameRegion(k, v);
}

void SoundEditor::clearRegions() {
    SoundMarks m = doc_->marks();
    m.regions.clear();
    after(doc_->setMarks("Clear Regions", std::move(m)));
}

void SoundEditor::selectRegion(int k) {
    const auto& rs = doc_->marks().regions;
    if (k < 0 || k >= (int)rs.size()) return;
    const Region& r = rs[(size_t)k];
    doc_->select(r.start, r.end);
    doc_->playhead = r.start;
    double shown = spp_ * view_->laneW();
    if ((double)r.end < start_ || (double)r.start > start_ + shown) setView((double)r.start - shown * 0.05, spp_);
    table_->showRow(k);
    selectionChanged();
}

int SoundEditor::selectedRegion() const {
    const auto& rs = doc_->marks().regions;
    if (doc_->sel.empty()) return -1;
    for (size_t k = 0; k < rs.size(); ++k)
        if (rs[k].start == doc_->sel.start && rs[k].end == doc_->sel.end) return (int)k;
    return -1;
}

void SoundEditor::markersFrom(const std::string& what) {
    if (doc_->frames() == 0) { setStatus("the sound is empty"); return; }
    Sound s = doc_->sound();                              // the job reads its own snapshot
    uint64_t gen = doc_->generation();
    auto out = std::make_shared<std::vector<long>>();
    auto bpm = std::make_shared<double>(0);
    std::string name = "Markers from " + std::string(what == "onsets" ? "Onsets" : what == "beats" ? "Beats" : "Novelty");
    startWork(name, [s, what, out, bpm](Job& j) {
        if (what == "onsets") *out = onsetFrames(*s, MarkOptions(), j);
        else if (what == "beats") *out = beatFrames(*s, j, bpm.get());
        else *out = noveltyFrames(*s, j);
    }, [this, what, out, gen, name, bpm] {
        if (doc_->generation() != gen) { setStatus(name + ": the sound changed meanwhile: run it again"); return; }
        SoundMarks m = doc_->marks();                     // those of an earlier run replaced
        m.markers.erase(std::remove_if(m.markers.begin(), m.markers.end(), [&](const Marker& k) { return k.source == what; }),
                        m.markers.end());
        std::string word = what == "onsets" ? "onset " : what == "beats" ? "beat " : "section ";
        int n = 0;
        for (long f : *out) m.markers.push_back({f, word + std::to_string(++n), what});
        bool ok = doc_->setMarks(name, std::move(m));
        char b[160];
        std::snprintf(b, sizeof b, "%s: %d found%s", name.c_str(), n,
                      what == "beats" && *bpm > 0 ? (" (" + std::to_string((int)std::lround(*bpm)) + " bpm)").c_str() : "");
        if (ok || n == 0) doc_->message = b;
        lastWork_ = b;
        after(ok);
    });
}

void SoundEditor::regionsFrom(const std::string& what) {
    if (doc_->frames() == 0) { setStatus("the sound is empty"); return; }
    Sound s = doc_->sound();
    uint64_t gen = doc_->generation();
    auto out = std::make_shared<std::vector<Region>>();
    std::string name = what == "types" ? "Regions from Types" : "Regions from Onsets";
    startWork(name, [s, what, out](Job& j) {
        MarkOptions o;
        *out = what == "types" ? typeRegions(*s, o, j) : onsetRegions(*s, o, j);
    }, [this, what, out, gen, name] {
        if (doc_->generation() != gen) { setStatus(name + ": the sound changed meanwhile: run it again"); return; }
        SoundMarks m = doc_->marks();
        // the regions an analysis made before (types or onsets) give way; those made by hand stay
        m.regions.erase(std::remove_if(m.regions.begin(), m.regions.end(), [&](const Region& r) { return r.source != "hand"; }),
                        m.regions.end());
        for (auto& r : *out) m.regions.push_back(r);
        bool ok = doc_->setMarks(name, std::move(m));
        std::string msg = name + ": " + std::to_string(out->size()) + (out->empty() ? " (none found)" : "");
        if (ok || out->empty()) doc_->message = msg;
        lastWork_ = msg;
        after(ok);
    });
}

bool SoundEditor::writeColl(const std::string& path, std::string& err) {
    std::string name = doc_->boundToBuffer() ? "buffer~ " + doc_->bufferName() : doc_->path().empty() ? "Untitled sound" : baseName(doc_->path());
    return fileio::writeAtomic(path, collText(doc_->marks(), doc_->sr(), name), &err);
}

void SoundEditor::toColl() {
    if (doc_->marks().regions.empty()) { setStatus("To coll: no regions yet"); return; }
    Fl_Native_File_Chooser fc;
    fc.title("The regions as rows, for a coll");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("Coll (Musil data)\t*.coll");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    std::string base = doc_->boundToBuffer() ? doc_->bufferName() : doc_->path().empty() ? "regions" : baseName(doc_->path());
    if (size_t dot = base.find_last_of('.'); dot != std::string::npos && !doc_->boundToBuffer()) base.resize(dot);
    if (!doc_->boundToBuffer() && !doc_->path().empty()) fc.directory(dirOf(doc_->path()).c_str());
    fc.preset_file((base + "-regions.coll").c_str());
    std::string p = showSaveChooser(fc);
    if (p.empty()) return;
    std::string err;
    if (!writeColl(p, err)) {
        alertUser("Can't write %s:\n%s", baseName(p).c_str(), err.c_str());
        return;
    }
    setStatus("To coll: " + std::to_string(doc_->marks().regions.size()) + " rows in " + baseName(p));
    App::get().log("regions written to " + p);
}

bool SoundEditor::writeScore(const std::string& path, std::string& err) {
    SoundDoc& d = *doc_;
    if (!d.boundToBuffer() && d.path().empty()) {
        err = "the sound has no file yet: save it first (the score plays the regions of its file)";
        return false;
    }
    std::string name = d.boundToBuffer() ? d.bufferName() : baseName(d.path());
    if (size_t dot = name.find_last_of('.'); dot != std::string::npos && !d.boundToBuffer()) name.resize(dot);
    // the file named relative to the score's folder (in it, below, or a folder or two up): the folder moves with it
    std::string source = d.boundToBuffer() ? d.bufferName() : soundPathForScore(d.path(), path);
    return fileio::writeAtomic(path, scoreText(d.marks(), d.sr(), source, d.boundToBuffer(), name + " regions"), &err);
}

void SoundEditor::toScore() {
    if (!scoreAvailable()) { setStatus("To score: with the score object"); return; }
    if (doc_->marks().regions.empty()) { setStatus("To score: no regions yet"); return; }
    Fl_Native_File_Chooser fc;
    fc.title("The regions as a score (a sound event each)");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("Score\t*.score");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    std::string base = doc_->boundToBuffer() ? doc_->bufferName() : doc_->path().empty() ? "regions" : baseName(doc_->path());
    if (size_t dot = base.find_last_of('.'); dot != std::string::npos && !doc_->boundToBuffer()) base.resize(dot);
    if (!doc_->boundToBuffer() && !doc_->path().empty()) fc.directory(dirOf(doc_->path()).c_str());
    fc.preset_file((base + "-regions.score").c_str());
    std::string p = showSaveChooser(fc);
    if (p.empty()) return;
    std::string err;
    if (!writeScore(p, err)) {
        alertUser("Can't write %s:\n%s", baseName(p).c_str(), err.c_str());
        return;
    }
    std::string msg = "To score: " + std::to_string(doc_->marks().regions.size()) + " sound events in " + baseName(p) +
                      " (a score object reads it: score name " + baseName(p) + ")";
    if (doc_->needsSaving()) msg += "; they play the file as saved";
    setStatus(msg);
    App::get().log("regions written to " + p);
}

// ---------------------------------------------------------------- the Process menu

namespace {
std::string menuText(const std::string& s) {             // a label FLTK's menus show as it is
    std::string o;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '&' || c == '_') o += '\\';
        o += c;
    }
    return o;
}
std::vector<std::string> atomsNames() {
    std::vector<std::string> out;
    App::get().host.forEachBox([&](Box* b) {
        if (b->cls == "atoms" && !b->args.empty() && b->args[0].isSymbol() &&
            std::find(out.begin(), out.end(), b->args[0].s) == out.end())
            out.push_back(b->args[0].s);
    });
    return out;
}
std::map<std::string, std::string>& lastAnswers() {      // a dialog shows what was answered last time
    static std::map<std::string, std::string> m;
    return m;
}
}  // namespace

std::vector<std::string> SoundEditor::otherSounds() const {
    std::vector<std::string> out;
    App::get().host.forEachBox([&](Box* b) {
        if (auto* bb = dynamic_cast<BufferBox*>(b)) {
            std::string s = "buffer~ " + bb->name;
            if (!bb->name.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
    });
    for (auto* w : App::get().soundWindows()) {
        SoundDoc& d = w->doc();
        if (!d.boundToBuffer() && !d.path().empty()) out.push_back("sound " + d.path());
    }
    return out;
}

std::vector<std::string> SoundEditor::recentProcesses() {
    std::vector<std::string> out;
    Fl_Preferences r(appPrefs(), "sound-process-recent");
    for (int k = 0; k < 6; ++k) {
        char* v = nullptr;
        if (!r.get(std::to_string(k).c_str(), v, "") || !v) break;
        std::string s = v;
        free(v);
        if (s.empty()) break;
        out.push_back(s);
    }
    return out;
}

void SoundEditor::addRecent(const std::string& given) {
    std::string path = fileio::absolute(given);
    std::vector<std::string> list = recentProcesses();
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > 6) list.resize(6);
    Fl_Preferences r(appPrefs(), "sound-process-recent");
    for (size_t k = 0; k < 6; ++k) r.set(std::to_string(k).c_str(), k < list.size() ? list[k].c_str() : "");
    appPrefs().flush();
    for (auto* w : App::get().soundWindows()) w->editor()->menuDirty_ = true;   // every editor lists it (made again by its timer)
}

void SoundEditor::buildProcessMenu() {
    using E = SoundEditor;
    processMenu_->clear();
    auto ed = [](Fl_Widget* w) { return static_cast<E*>(w->parent()->parent()); };
    (void)ed;
    for (const ProcessInfo& i : processKinds())
        processMenu_->add((menuText(i.menu) + "...").c_str(), 0, [](Fl_Widget* w, void* d) {
            static_cast<E*>(w->parent()->parent())->processDialog(static_cast<const ProcessInfo*>(d)->kind);
        }, (void*)&i, &i == &processKinds().back() ? FL_MENU_DIVIDER : 0);
    processMenu_->add("A patch (inlet~ to outlet~)...", 0, [](Fl_Widget* w, void*) {
        static_cast<E*>(w->parent()->parent())->choosePatch();
    }, nullptr, 0);
    auto recent = recentProcesses();
    processMenu_->add("A Musil script (process x sr)...", 0, [](Fl_Widget* w, void*) {
        static_cast<E*>(w->parent()->parent())->chooseScript();
    }, nullptr, recent.empty() ? FL_MENU_DIVIDER : 0);
    recentShown_ = recent;
    for (size_t k = 0; k < recent.size(); ++k)
        processMenu_->add(("Recent: " + menuText(baseName(recent[k]))).c_str(), 0, [](Fl_Widget* w, void* d) {
            auto* e = static_cast<E*>(w->parent()->parent());
            size_t k = (size_t)(uintptr_t)d;
            if (k >= e->recentShown_.size()) return;
            std::string p = e->recentShown_[k];
            if (p.size() > 3 && p.compare(p.size() - 3, 3, ".mu") == 0) e->processScript(p);
            else e->processPatch(p);
        }, (void*)(uintptr_t)k, k + 1 == recent.size() ? FL_MENU_DIVIDER : 0);
    processMenu_->add("Result: replace the selection", 0, [](Fl_Widget* w, void*) {
        static_cast<E*>(w->parent()->parent())->setResultNew(false);
    }, nullptr, FL_MENU_RADIO | (resultNew_ ? 0 : FL_MENU_VALUE));
    processMenu_->add("Result: a new sound", 0, [](Fl_Widget* w, void*) {
        static_cast<E*>(w->parent()->parent())->setResultNew(true);
    }, nullptr, FL_MENU_RADIO | FL_MENU_DIVIDER | (resultNew_ ? FL_MENU_VALUE : 0));
    processMenu_->add("Stop (Esc)", 0, [](Fl_Widget* w, void*) { static_cast<E*>(w->parent()->parent())->stopWork(); }, nullptr, 0);
}

void SoundEditor::buildMarksMenu() {
    using E = SoundEditor;
    marksMenu_->clear();
    struct Item {
        const char* label;
        void (*fn)(E*);
        int flags;
    };
    static const Item items[] = {
        {"Add a Marker at the Playhead (M)", [](E* e) { e->addMarkerAtPlayhead(); }, 0},
        {"Markers from Onsets", [](E* e) { e->markersFrom("onsets"); }, 0},
        {"Markers from Beats", [](E* e) { e->markersFrom("beats"); }, 0},
        {"Markers from Novelty", [](E* e) { e->markersFrom("novelty"); }, 0},
        {"Delete the Marker (Backspace)", [](E* e) { e->deleteMarker(e->selectedMarker()); }, 0},
        {"Clear Markers", [](E* e) { e->clearMarkers(); }, FL_MENU_DIVIDER},
        {"Add a Region from the Selection", [](E* e) { e->addRegion(); }, 0},
        {"Regions from Types (soundtypes)", [](E* e) { e->regionsFrom("types"); }, 0},
        {"Regions from Onsets", [](E* e) { e->regionsFrom("onsets"); }, 0},
        {"Delete the Region", [](E* e) { e->deleteRegion(e->selectedRegion()); }, 0},
        {"Clear Regions", [](E* e) { e->clearRegions(); }, FL_MENU_DIVIDER},
        {"To Coll...", [](E* e) { e->toColl(); }, 0},
        {"To Score...", [](E* e) { e->toScore(); }, FL_MENU_DIVIDER},
        {"Show the Regions Panel", [](E* e) {
             if (e->bottom_->visible()) e->bottom_->hide();
             else e->bottom_->show();
             e->layoutChildren();
         }, FL_MENU_TOGGLE},
    };
    for (const Item& it : items) {
        int flags = it.flags;
        if (std::string(it.label).rfind("To Score", 0) == 0 && !scoreAvailable()) flags |= FL_MENU_INACTIVE;
        if (flags & FL_MENU_TOGGLE) flags |= FL_MENU_VALUE;
        marksMenu_->add(it.label, 0, [](Fl_Widget* w, void* d) {
            static_cast<const Item*>(d)->fn(static_cast<E*>(w->parent()->parent()));
        }, (void*)&it, flags);
    }
}

void SoundEditor::setResultNew(bool on) {
    resultNew_ = on;
    menuDirty_ = true;                                    // made again by the timer: this may be its own callback
    setStatus(on ? "Process: the result opens as a new sound" : "Process: the result replaces the selection");
}

bool SoundEditor::working() const { return work_.running() || (script_ && script_->running()); }

bool SoundEditor::startWork(const std::string& name, std::function<void(Job&)> work, std::function<void()> done) {
    if (working()) {
        setStatus(workName_ + " is still running (Esc stops it)");
        return false;
    }
    workName_ = name;
    lastWork_.clear();
    work_.onProgress = [this](float p) {
        char b[160];
        std::snprintf(b, sizeof b, "%s\xe2\x80\xa6 %d%%  (Esc stops)", workName_.c_str(), (int)std::lround(p * 100));
        setStatus(b);
    };
    setStatus(name + "\xe2\x80\xa6  (Esc stops)");
    work_.start(std::move(work), [this, done] {
        work_.cancel();                                   // only joins its thread: it is done
        done();
    });
    return true;
}

void SoundEditor::stopWork() {
    if (!working()) return;
    work_.cancel();                                       // waits for it; its done() is dropped
    patchProc_.reset();
    if (script_) script_->stop();
    lastWork_ = workName_ + ": stopped";
    setStatus(lastWork_);
}

void SoundEditor::deliver(const std::string& name, SampleBuffer&& result, uint64_t gen, const std::string& note, bool mixTail) {
    if (result.frames <= 0) {
        lastWork_ = name + ": the result is empty";
        setStatus(lastWork_);
        return;
    }
    bool changed = doc_->generation() != gen;
    if (!resultNew_ && !changed && doc_->replaceSelection(name, result, mixTail)) {
        after(true);
        lastWork_ = doc_->message;
    } else {
        double secs = result.sr > 0 ? result.frames / result.sr : 0;
        App::get().newSound(std::move(result));
        lastWork_ = name + ": a new sound of " + formatTime(secs) +
                    (changed && !resultNew_ ? " (the sound was edited meanwhile: not replaced)" : "");
    }
    if (!note.empty()) {
        lastWork_ += " \xc2\xb7 " + note;
        App::get().print(note);
    }
    setStatus(lastWork_);
}

bool SoundEditor::runProcess(const std::string& kind, const std::map<std::string, std::string>& params) {
    const ProcessInfo* info = processInfo(kind);
    if (!info) { setStatus("no process " + kind); return false; }
    if (doc_->frames() == 0) { setStatus("the sound is empty"); return false; }
    if (working()) { setStatus(workName_ + " is still running (Esc stops it)"); return false; }
    ProcessSpec spec;
    spec.kind = kind;
    spec.p = params;
    std::string otherFile;
    std::string with = spec.word("with");
    if (!with.empty()) {
        if (with.rfind("buffer~ ", 0) == 0) {
            BufferBox* b = findBuffer(&App::get().host, with.substr(8));
            if (!b) { setStatus(kind + ": no " + with); return false; }
            spec.other = std::make_shared<const SampleBuffer>(b->copy());
        } else if (with.rfind("sound ", 0) == 0) {
            for (auto* w : App::get().soundWindows())
                if (!w->doc().boundToBuffer() && w->doc().path() == with.substr(6)) spec.other = w->doc().sound();
            if (!spec.other) otherFile = with.substr(6);
        } else if (with.rfind("file ", 0) == 0) {
            otherFile = with.substr(5);
        } else {
            otherFile = with;
        }
    }
    if (info->other && !spec.other && otherFile.empty()) {
        setStatus(info->menu + ": choose the other sound");
        return false;
    }
    if (kind == "pursuit") {
        std::string dn = spec.word("dictionary");
        if (dn != "gabor" && dn != "gammatone") {
            spec.dict = findDictionary(&App::get().host, dn);
            if (!spec.dict) { setStatus("pursuit: no atoms " + dn + " in the open patches"); return false; }
        }
    }
    Selection s = doc_->sel.empty() ? Selection{0, doc_->frames()} : doc_->sel;
    auto in = std::make_shared<SampleBuffer>();
    const size_t ch = (size_t)doc_->channels();
    in->channels = doc_->channels();
    in->sr = doc_->sr();
    in->frames = s.length();
    in->data.assign(doc_->sound()->data.begin() + (long)((size_t)s.start * ch), doc_->sound()->data.begin() + (long)((size_t)s.end * ch));
    uint64_t gen = doc_->generation();
    auto out = std::make_shared<SampleBuffer>();
    auto msg = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    std::string name = kind == "cross" ? "sparkle~ cross" : kind;
    return startWork(name, [spec, otherFile, in, out, msg, ok](Job& j) mutable {
        if (!otherFile.empty()) {                          // a file: read here, off the GUI thread
            SampleBuffer b;
            if (!decodeAudioFile(otherFile, b, *msg)) return;
            spec.other = std::make_shared<const SampleBuffer>(std::move(b));
        }
        *ok = moritz::runProcess(spec, *in, *out, *msg, j);
    }, [this, name, out, msg, ok, gen] {
        if (!*ok) {
            lastWork_ = msg->empty() ? name + ": stopped" : *msg;
            setStatus(lastWork_);
            if (!msg->empty()) App::get().print(*msg);
            return;
        }
        deliver(name, std::move(*out), gen, *msg);
    });
}

void SoundEditor::processDialog(const std::string& kind) {
    const ProcessInfo* info = processInfo(kind);
    if (!info) return;
    std::vector<std::string> others = otherSounds(), atoms = atomsNames();
    if (batchMode()) {                                    // the tests run it with act=process:kind:key=value,...
        std::string t = "ask: " + info->title + ":";
        for (auto& f : info->fields) {
            t += " " + f.key + "=" + f.def;
            if (f.key == "with") {
                t += "[";
                for (size_t k = 0; k < others.size(); ++k) t += (k ? "|" : "") + others[k];
                t += "|a file...]";
            } else if (f.key == "dictionary") {
                t += "[gabor|gammatone";
                for (auto& a : atoms) t += "|" + a;
                t += "]";
            }
        }
        t += std::string(" result=") + (resultNew_ ? "new" : "replace");
        std::printf("%s\n", t.c_str());
        std::fflush(stdout);
        return;
    }
    const int W = 460, rowH = 32, n = (int)info->fields.size();
    Fl_Double_Window dlg(W, 70 + rowH * (n + 1) + 50, info->title.c_str());
    centerOnScreen(&dlg, window());
    Fl_Box head(16, 10, W - 32, 40);
    std::string what = doc_->sel.empty() ? "All of the sound (nothing is selected)"
                                         : "The selection, " + formatTime(doc_->seconds(doc_->sel.start)) + " to " +
                                               formatTime(doc_->seconds(doc_->sel.end));
    head.copy_label(what.c_str());
    head.labelsize(12);
    head.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    struct Row {
        const ProcessField* f;
        Fl_Choice* choice = nullptr;
        Fl_Input* input = nullptr;
        std::vector<std::string> values;
    };
    std::vector<Row> rows;
    int y = 60;
    for (auto& f : info->fields) {
        Row r;
        r.f = &f;
        std::string key = kind + "." + f.key;
        std::string last = lastAnswers().count(key) ? lastAnswers()[key] : f.def;
        bool text = f.choices.size() == 1 && f.choices[0] == "text";
        if (f.key == "with" || f.key == "dictionary" || (!f.choices.empty() && !text)) {
            r.choice = new Fl_Choice(190, y, W - 210, 24);
            if (f.key == "with") {
                r.values = others;
                r.values.push_back("a file...");
            } else {
                r.values = f.choices;
                if (f.key == "dictionary")
                    for (auto& a : atoms) r.values.push_back(a);
            }
            int pick = 0;
            for (size_t k = 0; k < r.values.size(); ++k) {
                r.choice->add(menuText(r.values[k]).c_str());
                if (r.values[k] == last) pick = (int)k;
            }
            r.choice->value(pick);
            r.choice->copy_tooltip(f.tip.c_str());
        } else {
            r.input = new Fl_Input(190, y, W - 210, 24);
            r.input->value(last.c_str());
            r.input->copy_tooltip(f.tip.c_str());
        }
        Fl_Widget* w = r.choice ? (Fl_Widget*)r.choice : (Fl_Widget*)r.input;
        w->copy_label((f.label + ":").c_str());
        w->labelsize(12);
        rows.push_back(std::move(r));
        y += rowH;
    }
    Fl_Choice result(190, y, W - 210, 24, "Result:");
    result.add("replace the selection");
    result.add("a new sound");
    result.value(resultNew_ ? 1 : 0);
    result.labelsize(12);
    Fl_Button cancel(W - 200, y + rowH + 12, 85, 28, "Cancel");
    Fl_Return_Button ok(W - 105, y + rowH + 12, 85, 28, "Process");
    dlg.end();
    dlg.set_modal();
    int answer = 0;
    ok.callback([](Fl_Widget* w, void* r) { *(int*)r = 1; w->window()->hide(); }, &answer);
    cancel.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.show();
    while (dlg.shown()) Fl::wait();
    if (answer != 1) return;
    std::map<std::string, std::string> params;
    for (auto& r : rows) {
        std::string v = r.choice ? (r.choice->value() >= 0 && r.choice->value() < (int)r.values.size() ? r.values[(size_t)r.choice->value()] : "")
                                 : std::string(r.input->value());
        if (r.f->key == "with" && v == "a file...") {
            std::string p = chooseFile(false, info->menu + ": the other sound", "Sound\t*.{wav,wave,aif,aiff,flac,mp3}");
            if (p.empty()) return;
            v = "file " + p;
        } else {
            lastAnswers()[kind + "." + r.f->key] = v;
        }
        params[r.f->key] = v;
    }
    resultNew_ = result.value() == 1;
    menuDirty_ = true;
    runProcess(kind, params);
}

double SoundEditor::patchTail() {
    auto it = lastAnswers().find("patch.tail");
    double t = it == lastAnswers().end() ? 2.0 : std::atof(it->second.c_str());
    return std::isfinite(t) ? std::clamp(t, 0.0, 60.0) : 2.0;
}

void SoundEditor::choosePatch() {
    std::string p = chooseFile(false, "Process the selection with a patch (inlet~ in, outlet~ out)", "Moritz patch\t*.moritz");
    if (p.empty()) return;
    // the tail: how long the patch rings on after the selection (a reverb, an echo); and where the result goes
    const int W = 460, H = 170;
    Fl_Double_Window dlg(W, H, "Process with a patch");
    centerOnScreen(&dlg, window());
    Fl_Box head(16, 10, W - 32, 40);
    head.copy_label(("The selection through " + baseName(p) + "; it rings on for the tail, mixed over what follows the selection when it replaces it").c_str());
    head.labelsize(12);
    head.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Input tail(190, 60, W - 210, 24, "Tail (seconds):");
    char t[32];
    std::snprintf(t, sizeof t, "%g", patchTail());
    tail.value(t);
    tail.labelsize(12);
    tail.copy_tooltip("How long the patch is rendered after the selection ends (0 to 60 s): a reverb's or an echo's tail. Silence at its end is left out");
    Fl_Choice result(190, 92, W - 210, 24, "Result:");
    result.add("replace the selection");
    result.add("a new sound");
    result.value(resultNew_ ? 1 : 0);
    result.labelsize(12);
    Fl_Button cancel(W - 200, H - 40, 85, 28, "Cancel");
    Fl_Return_Button ok(W - 105, H - 40, 85, 28, "Process");
    dlg.end();
    dlg.set_modal();
    int answer = 0;
    ok.callback([](Fl_Widget* w, void* r) { *(int*)r = 1; w->window()->hide(); }, &answer);
    cancel.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.show();
    while (dlg.shown()) Fl::wait();
    if (answer != 1) return;
    lastAnswers()["patch.tail"] = tail.value();
    resultNew_ = result.value() == 1;
    menuDirty_ = true;
    processPatch(p, patchTail());
}

void SoundEditor::chooseScript() {
    std::string p = chooseFile(false, "Process the selection with a Musil script that defines (process x sr)", "Musil script\t*.mu");
    if (!p.empty()) processScript(p);
}

bool SoundEditor::processPatch(const std::string& path, double tail) {
    if (doc_->frames() == 0) { setStatus("the sound is empty"); return false; }
    if (!(tail >= 0)) tail = patchTail();
    if (working()) { setStatus(workName_ + " is still running (Esc stops it)"); return false; }
    Selection s = doc_->sel.empty() ? Selection{0, doc_->frames()} : doc_->sel;
    SampleBuffer in;
    const size_t ch = (size_t)doc_->channels();
    in.channels = doc_->channels();
    in.sr = doc_->sr();
    in.frames = s.length();
    in.data.assign(doc_->sound()->data.begin() + (long)((size_t)s.start * ch), doc_->sound()->data.begin() + (long)((size_t)s.end * ch));
    auto pp = std::make_unique<PatchProcess>();
    std::string err;
    if (!pp->prepare(path, in, tail, err)) {               // made and loaded here, on the GUI thread
        lastWork_ = baseName(path) + ": " + err;
        setStatus(lastWork_);
        App::get().print("process: " + lastWork_);
        return false;
    }
    addRecent(path);
    patchProc_ = std::move(pp);
    PatchProcess* raw = patchProc_.get();                 // the editor owns it; the job is stopped before it goes
    uint64_t gen = doc_->generation();
    std::string name = baseName(path);
    return startWork(name, [raw](Job& j) { raw->run(j); }, [this, gen, name] {
        std::vector<std::string> prints;
        SampleBuffer out = patchProc_ ? patchProc_->take(prints) : SampleBuffer();
        patchProc_.reset();                                // the patch destroyed here, on the GUI thread
        for (auto& p : prints) App::get().print(name + ": " + p);
        deliver(name, std::move(out), gen, "", true);       // the tail mixed over what follows the selection
    });
}

bool SoundEditor::processScript(const std::string& path) {
    if (doc_->frames() == 0) { setStatus("the sound is empty"); return false; }
    if (working()) { setStatus(workName_ + " is still running (Esc stops it)"); return false; }
    Selection s = doc_->sel.empty() ? Selection{0, doc_->frames()} : doc_->sel;
    SampleBuffer in;
    const size_t ch = (size_t)doc_->channels();
    in.channels = doc_->channels();
    in.sr = doc_->sr();
    in.frames = s.length();
    in.data.assign(doc_->sound()->data.begin() + (long)((size_t)s.start * ch), doc_->sound()->data.begin() + (long)((size_t)s.end * ch));
    addRecent(path);
    uint64_t gen = doc_->generation();
    std::string name = baseName(path);
    workName_ = name;
    lastWork_.clear();
    setStatus(name + ": running\xe2\x80\xa6  (Esc stops)");
    script_ = std::make_unique<ScriptProcess>();
    // done never runs after stop() or once the editor is gone (the script's alive flag)
    return script_->start(path, in, [this, gen, name](bool ok, SampleBuffer out, std::string err) {
        if (!ok) {
            lastWork_ = name + ": " + err;
            setStatus(lastWork_);
            App::get().print("process: " + lastWork_);
            return;
        }
        deliver(name, std::move(out), gen, "");
    });
}

}  // namespace moritz
