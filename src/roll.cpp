// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// roll.cpp — the roll (roll.h): RollCore, the drawing and the editing of a score; RollPanel, its editor.

#include "roll.h"
#include "gui.h"
#include "score_doc.h"
#include "score_window.h"
#include "transport.h"
#include "ui_util.h"
#include "zoom.h"

#include "music/score_text.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace moritz {

namespace lay = musil::score::layout;
namespace tr = musil::score::transform;

Fl_Color rollRowColour(int k) {
    static const Fl_Color c[] = {fl_rgb_color(226, 128, 40),  fl_rgb_color(34, 150, 130), fl_rgb_color(206, 60, 120),
                                 fl_rgb_color(60, 110, 220),  fl_rgb_color(110, 170, 60), fl_rgb_color(150, 100, 220),
                                 fl_rgb_color(200, 160, 30),  fl_rgb_color(210, 80, 70)};
    return c[((k % 8) + 8) % 8];
}

namespace {

// the roll's colours: on paper (the window), or on the dark of a box (compact)
struct Palette {
    Fl_Color bg, rowAlt, line, bar, beat, text, dim, sel, ghost, scoreFill, scoreLine, sound, patch, msg, playhead;
};
const Palette& palette(bool compact) {
    static const Palette paper{fl_rgb_color(246, 244, 238), fl_rgb_color(236, 234, 226), fl_rgb_color(150, 150, 150),
                               fl_rgb_color(196, 194, 186), fl_rgb_color(226, 224, 216), fl_rgb_color(40, 40, 44),
                               fl_rgb_color(120, 120, 126), fl_rgb_color(40, 120, 240), fl_rgb_color(90, 150, 250),
                               fl_rgb_color(232, 226, 250), fl_rgb_color(140, 110, 230), fl_rgb_color(60, 170, 110),
                               fl_rgb_color(70, 120, 230), fl_rgb_color(110, 110, 120), fl_rgb_color(230, 40, 40)};
    static const Palette dark{fl_rgb_color(30, 31, 36), fl_rgb_color(36, 37, 43), fl_rgb_color(78, 80, 90),
                              fl_rgb_color(70, 72, 82), fl_rgb_color(44, 46, 52), kText, kDim, kSelect,
                              fl_rgb_color(150, 190, 255), fl_rgb_color(52, 46, 78), fl_rgb_color(140, 110, 230),
                              fl_rgb_color(60, 170, 110), fl_rgb_color(70, 120, 230), fl_rgb_color(150, 150, 160),
                              fl_rgb_color(230, 60, 60)};
    return compact ? dark : paper;
}
const char* const kDynamics[] = {"ppp", "pp", "p", "mp", "mf", "f", "ff", "fff"};
int dynLevel(const std::string& d) {
    for (int k = 0; k < 8; ++k) if (d == kDynamics[k]) return k;
    return -1;
}
bool pitchedEv(const sc::Event& e) { return e.kind == sc::Kind::Note && e.variant != "chord"; }
std::string fmtNum(double v, int prec = 3) {
    char b[32];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}
std::string pitchName(double p) {
    static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int m = (int)std::floor(p + 1e-9);
    std::string s = std::string(n[((m % 12) + 12) % 12]) + std::to_string(m / 12 - 1 - (m < 0 && m % 12 ? 1 : 0));
    double c = (p - m) * 100;
    if (std::fabs(c) > 0.5) s += (c > 0 ? "+" : "") + fmtNum(c, 0);
    return s;
}
std::string placementText(const sc::Event& e) {
    std::string s;
    if (e.transpose != 0) s += (e.transpose > 0 ? " +" : " ") + fmtNum(e.transpose, 2);
    if (e.stretch != 1) s += " x" + fmtNum(e.stretch, 2);
    if (e.gain != 1 && e.gain > 0) {
        double db = 20 * std::log10(e.gain);
        s += (db > 0 ? " +" : " ") + fmtNum(db, 1) + " dB";
    }
    return s;
}
std::string fmtClock(double sec) {
    char b[32];
    int m = (int)(sec / 60);
    std::snprintf(b, sizeof b, "%d'%02d\"", m, (int)std::floor(sec - 60 * m));
    return b;
}

}  // namespace

// ================================================================ RollCore
RollCore::RollCore(std::shared_ptr<RollEditTarget> t) : target_(std::move(t)) {}

void RollCore::setTarget(std::shared_ptr<RollEditTarget> t) {
    target_ = std::move(t);
    path.clear();
    selection.clear();
    synced_.clear();
    syncedPath_.clear();
    focusedPath_.clear();
    everSynced_ = false;
    gen_ = ~0ull;
}

bool RollCore::syncSelection() {
    if (!target_) return false;
    if (path != focusedPath_ || !everSynced_) {
        target_->focus(path);
        focusedPath_ = path;
    }
    Ids theirs;
    if (!target_->sharedSelection(path, theirs)) return false;
    std::set<uint64_t> doc(theirs.begin(), theirs.end());
    if (!everSynced_) {                           // a view opened: the document's selection
        everSynced_ = true;
        syncedPath_ = path;
        bool took = doc != selection && selection.empty();
        if (took) selection = doc;
        else if (doc != selection) target_->shareSelection(path, selectedIds());
        synced_ = selection;
        return took;
    }
    if (path != syncedPath_ || selection != synced_) {     // changed here: the document's now
        if (doc != selection) target_->shareSelection(path, selectedIds());
        Ids kept;
        target_->sharedSelection(path, kept);     // what is still in the score
        selection = synced_ = std::set<uint64_t>(kept.begin(), kept.end());
        syncedPath_ = path;
        return false;
    }
    if (doc != synced_) {                         // changed in another view
        selection = synced_ = doc;
        return true;
    }
    return false;
}

void RollCore::setSize(double w, double h) {
    w_ = std::max(20.0, w);
    h_ = std::max(10.0, h);
}

std::shared_ptr<const sc::Score> RollCore::shown() const {
    if (!target_) return nullptr;
    auto top = std::const_pointer_cast<sc::Score>(target_->score());
    if (!top) return nullptr;
    return tr::descend(top, path);
}

std::vector<RollCore::Placement> RollCore::placements() const {
    std::vector<Placement> out;
    auto s = target_ ? target_->score() : nullptr;
    for (uint64_t id : path) {
        if (!s) break;
        auto e = s->find(id);
        if (!e || !e->sub) break;
        out.push_back({e->at, e->dur, e->stretch, e->transpose, e->gain});
        s = e->sub;
    }
    return out;
}

std::vector<std::string> RollCore::crumbs() const {
    std::vector<std::string> out;
    auto s = target_ ? target_->score() : nullptr;
    if (!s) return out;
    out.push_back(s->name.empty() ? std::string("score") : s->name);
    for (uint64_t id : path) {
        auto e = s->find(id);
        if (!e || !e->sub) break;
        s = e->sub;
        out.push_back((s->name.empty() ? "score " + std::to_string(id) : s->name) + (e->ref.empty() ? "" : " (" + e->ref + ")"));
    }
    return out;
}

void RollCore::enter(uint64_t id) {
    auto s = shown();
    auto e = s ? s->find(id) : nullptr;
    if (!e || e->kind != sc::Kind::Score || !e->sub) return;
    path.push_back(id);
    selection.clear();
    syncSelection();
    fit();
    if (onStatus) onStatus("inside " + (e->sub->name.empty() ? std::string("the score placed") : e->sub->name) + placementText(*e) + ": Esc goes up");
    if (onChange) onChange();
}

void RollCore::up() {
    if (path.empty()) return;
    uint64_t from = path.back();
    path.pop_back();
    selection = {from};
    syncSelection();
    fit();
    if (onChange) onChange();
}

double RollCore::shownTime(double topTime) const {
    if (std::isnan(topTime)) return NAN;
    double t = topTime;
    for (auto& p : placements()) {
        double rel = t - p.at;
        if (rel < 0 || rel > p.dur) return NAN;
        t = rel / (p.stretch > 0 ? p.stretch : 1);
    }
    return t;
}

double RollCore::barSeconds() const {
    auto s = shown();
    double bpm = s && s->bpm > 0 ? s->bpm : 60;
    int num = s ? std::max(1, s->meterNum) : 4;
    return num * 60.0 / bpm;
}
double RollCore::snapSeconds() const {
    if (!(snap > 0)) return 0;
    auto s = shown();
    double bpm = s && s->bpm > 0 ? s->bpm : 60;
    int den = s ? std::max(1, s->meterDen) : 4;
    return snap * den * 60.0 / bpm;
}
double RollCore::snapTime(double t) const {
    double g = snapSeconds();
    if (g <= 0) return std::max(0.0, t);
    return std::max(0.0, std::round(t / g) * g);
}

static double gutterOf(bool compact) { return compact ? 34 : 78; }
static double rulerOf(bool compact) { return compact ? 0 : 20; }

static double padOf(bool compact) { return compact ? 5 : 14; }    // room for the heads of the notes at 0
double RollCore::xOf(double t) const { return gutterOf(compact) + padOf(compact) + (t - t0) * pps; }
double RollCore::tOf(double x) const { return t0 + (x - gutterOf(compact) - padOf(compact)) / std::max(1e-9, pps); }

void RollCore::fit() {
    auto s = shown();
    double d = s ? s->duration() : 0;
    d = std::max(d, std::min(barSeconds(), 4.0));
    if (d <= 0) d = 4;
    t0 = 0;
    pps = std::max(1e-3, (w_ - gutterOf(compact) - padOf(compact) - (compact ? 6 : 16)) / d);
    vscroll = 0;
    gen_ = ~0ull;
}

void RollCore::zoom(double factor, double aroundX) {
    double t = tOf(aroundX);
    pps = std::clamp(pps * factor, 1e-2, 1e5);
    t0 = std::max(0.0, t - (aroundX - gutterOf(compact) - padOf(compact)) / pps);
    gen_ = ~0ull;
}

void RollCore::selectAll() {
    selection.clear();
    if (auto s = shown())
        for (auto& e : s->events()) selection.insert(e->id);
    if (onChange) onChange();
}

std::string RollCore::summary() const {
    auto s = target_ ? target_->score() : nullptr;
    if (!s) return "";
    size_t named = 0;
    for (auto& r : lay::rows(*s)) named += r.byKind ? 0 : 1;
    return std::to_string(named) + " rows · " + std::to_string(s->size()) + " events · " + fmtClock(s->duration());
}

// ---- the layout: rows and where each event is
void RollCore::layoutRows() const {
    auto s = shown();
    if (!s && !path.empty()) {                                  // the score placed is gone: back to the top
        const_cast<RollCore*>(this)->path.clear();
        s = shown();
    }
    uint64_t g = target_ ? target_->generation() : 0;
    double key[5] = {w_ * 7 + h_, rowScale + (compact ? 1000 : 0) + (mode == Mode::Piano ? 2000 : 0), t0, pps, vscroll + (double)path.size() * 1e6};
    if (g == gen_ && s == cached_ && std::equal(key, key + 5, layoutKey_)) return;
    if (compact && (g != gen_ || s != cached_ || key[0] != layoutKey_[0])) {
        const_cast<RollCore*>(this)->fit();
        key[2] = t0; key[3] = pps; key[4] = vscroll + (double)path.size() * 1e6;
    }
    gen_ = g;
    cached_ = s;
    std::copy(key, key + 5, layoutKey_);
    rows_.clear();
    items_.clear();
    if (!s) return;
    const double ruler = rulerOf(compact);
    auto specs = lay::rows(*s);
    // heights: a staff, or a line
    std::vector<double> natural;
    for (auto& r : specs) natural.push_back(lay::hasStaff(r.clef) ? (compact ? 60 : 96) : (compact ? 22 : 40));
    double total = 0;
    for (double n : natural) total += n;
    double scale = compact ? (total > 0 ? (h_ - ruler) / total : 1) : rowScale;
    double top = ruler - vscroll;
    for (size_t k = 0; k < specs.size(); ++k) {
        RowGeom r;
        r.spec = specs[k];
        r.top = top;
        r.h = natural[k] * scale;
        r.staff = lay::hasStaff(r.spec.clef) && mode == Mode::Staves;
        r.sp = std::max(compact ? 1.2 : 2.5, r.h / 12.0);
        r.yBottom = r.top + r.h * 0.66;
        rows_.push_back(r);
        top += r.h;
    }
    // the piano roll's pitches
    if (mode == Mode::Piano) {
        double lo = 1e9, hi = -1e9;
        for (auto& e : s->events())
            if (pitchedEv(*e)) { lo = std::min(lo, e->sounding()); hi = std::max(hi, e->sounding()); }
        if (lo > hi) { lo = 60; hi = 72; }
        lo = std::floor(lo) - 2;
        hi = std::ceil(hi) + 2;
        if (hi - lo < 24) { double mid = std::round((lo + hi) / 2); lo = mid - 12; hi = mid + 12; }
        pianoLo_ = lo;
        pianoHi_ = hi;
        keyH_ = std::max(compact ? 1.5 : 5.0, (h_ - ruler) / (hi - lo + 1)) * (compact ? 1 : rowScale);
    }
    // the events: blocks first (scores placed, then the others), notes last (drawn on top, hit first)
    std::vector<Item> blocks, others, notes;
    std::map<int, std::vector<double>> laneEnds;               // per row: where each lane is free again
    for (const sc::Event* e : s->byTime()) {
        Item it;
        it.e = e;
        it.row = lay::rowOf(specs, *e);
        if (it.row < 0) continue;
        const RowGeom& r = rows_[(size_t)it.row];
        double x = xOf(e->at), w = std::max(2.0, e->dur * pps);
        if (mode == Mode::Piano) {
            if (!pitchedEv(*e)) continue;
            double y = ruler - vscroll + (pianoHi_ + 0.5 - e->sounding()) * keyH_ - keyH_ / 2;
            it.x = x; it.y = y; it.w = w; it.h = std::max(2.0, keyH_ - 1);
            notes.push_back(it);
            continue;
        }
        if (pitchedEv(*e)) {
            double rad = std::max(compact ? 1.6 : 2.5, r.sp * 0.62);
            double y = r.staff ? r.yBottom - lay::halfSpaces(e->pitch, r.spec.clef) * r.sp / 2 : r.top + r.h * 0.5;
            it.x = x - rad; it.y = y - rad; it.w = w + rad; it.h = 2 * rad;
            notes.push_back(it);
        } else if (e->kind == sc::Kind::Score) {
            it.x = x; it.y = r.top + 2; it.w = w; it.h = std::max(4.0, r.h - 4);
            blocks.push_back(it);
        } else {
            auto& ends = laneEnds[it.row];
            size_t lane = 0;
            while (lane < ends.size() && ends[lane] > e->at + 1e-9) ++lane;
            if (lane == ends.size()) ends.push_back(0);
            ends[lane] = e->kind == sc::Kind::Message ? e->at + 40 / std::max(1e-9, pps) : e->end();
            it.lane = (int)lane;
            it.x = e->kind == sc::Kind::Message ? x - 4 : x;
            it.w = e->kind == sc::Kind::Message ? 8 : w;
            others.push_back(it);
        }
    }
    // the lanes of the blocks on a line
    for (auto& it : others) {
        const RowGeom& r = rows_[(size_t)it.row];
        double lanes = (double)std::max<size_t>(1, laneEnds[it.row].size());
        double lh = (r.h - 6) / lanes;
        it.y = r.top + 3 + it.lane * lh;
        it.h = std::max(2.0, lh - 2);
    }
    items_.reserve(blocks.size() + others.size() + notes.size());
    for (auto* v : {&blocks, &others, &notes}) items_.insert(items_.end(), v->begin(), v->end());
}

int RollCore::rowAt(double y) const {
    layoutRows();
    for (size_t k = 0; k < rows_.size(); ++k)
        if (y >= rows_[k].top && y < rows_[k].top + rows_[k].h) return (int)k;
    return -1;
}

double RollCore::yOfPitch(int row, double pitch) const {
    layoutRows();
    if (mode == Mode::Piano) return rulerOf(compact) - vscroll + (pianoHi_ + 0.5 - pitch) * keyH_;
    if (row < 0 || row >= (int)rows_.size()) return NAN;
    const RowGeom& r = rows_[(size_t)row];
    if (!r.staff) return r.top + r.h * 0.5;
    return r.yBottom - lay::halfSpaces(pitch, r.spec.clef) * r.sp / 2;
}

double RollCore::pitchAtY(int row, double y) const {
    layoutRows();
    if (mode == Mode::Piano) return std::round(pianoHi_ + 0.5 - (y - rulerOf(compact) + vscroll) / keyH_);
    if (row < 0 || row >= (int)rows_.size()) return NAN;
    const RowGeom& r = rows_[(size_t)row];
    if (!r.staff) return NAN;
    int hs = (int)std::lround((r.yBottom - y) / (r.sp / 2));
    return lay::pitchOfStep(lay::bottomStep(r.spec.clef) + hs);
}

RollCore::Hit RollCore::hit(double x, double y) const {
    layoutRows();
    Hit h;
    h.row = rowAt(y);
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (x < it->x - 3 || x > it->x + it->w + 3 || y < it->y - 3 || y > it->y + it->h + 3) continue;
        h.id = it->e->id;
        h.end = it->e->dur > 0 && it->e->kind != sc::Kind::Message && std::fabs(x - (it->x + it->w)) <= 5 && it->w > 10;
        h.row = it->row;
        return h;
    }
    return h;
}

bool RollCore::eventBox(uint64_t id, double& x, double& y, double& w, double& h) const {
    layoutRows();
    for (auto& it : items_)
        if (it.e->id == id) { x = it.x; y = it.y; w = it.w; h = it.h; return true; }
    return false;
}


// ---- the clefs, drawn as shapes (no font needed): each a few strokes whose width varies along them, given as points
// in staff spaces from the line the clef names (x right, y down), joined by a Catmull-Rom curve
namespace {
struct ClefPt {
    double x, y, w;                  // where, and how wide the stroke is there (staff spaces)
};
void clefStroke(const std::vector<ClefPt>& c, double X, double Y, double u) {
    if (c.size() < 2) return;
    std::vector<ClefPt> pts;
    const int kSteps = 10;
    for (size_t i = 0; i + 1 < c.size(); ++i) {
        const ClefPt& p0 = c[i ? i - 1 : 0];
        const ClefPt& p1 = c[i];
        const ClefPt& p2 = c[i + 1];
        const ClefPt& p3 = c[i + 2 < c.size() ? i + 2 : c.size() - 1];
        for (int k = 0; k < kSteps; ++k) {
            double t = (double)k / kSteps, t2 = t * t, t3 = t2 * t;
            auto cr = [&](double a, double b, double cc, double d) {
                return 0.5 * (2 * b + (-a + cc) * t + (2 * a - 5 * b + 4 * cc - d) * t2 + (-a + 3 * b - 3 * cc + d) * t3);
            };
            pts.push_back({cr(p0.x, p1.x, p2.x, p3.x), cr(p0.y, p1.y, p2.y, p3.y), p1.w + (p2.w - p1.w) * t});
        }
    }
    pts.push_back(c.back());
    // the outline, a quadrilateral for each step (a stroke that crosses itself is drawn whole)
    std::vector<double> lx, ly, rx, ry;
    for (size_t i = 0; i < pts.size(); ++i) {
        const ClefPt& a = pts[i ? i - 1 : 0];
        const ClefPt& b = pts[i + 1 < pts.size() ? i + 1 : i];
        double dx = b.x - a.x, dy = b.y - a.y, n = std::hypot(dx, dy);
        if (n < 1e-9) { dx = 1; dy = 0; n = 1; }
        double hw = pts[i].w / 2, nx = -dy / n * hw, ny = dx / n * hw;
        lx.push_back(X + (pts[i].x + nx) * u);
        ly.push_back(Y + (pts[i].y + ny) * u);
        rx.push_back(X + (pts[i].x - nx) * u);
        ry.push_back(Y + (pts[i].y - ny) * u);
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        fl_begin_polygon();
        zd::vertex(lx[i], ly[i]);
        zd::vertex(lx[i + 1], ly[i + 1]);
        zd::vertex(rx[i + 1], ry[i + 1]);
        zd::vertex(rx[i], ry[i]);
        fl_end_polygon();
    }
}
void clefDot(double x, double y, double r, double X, double Y, double u) {
    fl_begin_polygon();
    for (int k = 0; k < 16; ++k) {
        double a = k * M_PI / 8;
        zd::vertex(X + (x + r * std::cos(a)) * u, Y + (y + r * std::sin(a)) * u);
    }
    fl_end_polygon();
}
}  // namespace

// a clef's box in staff spaces: from its left edge, and above and below the line it names
void clefExtent(const std::string& clef, double& w, double& up, double& down) {
    if (clef == "bass") { w = 2.6; up = 1.2; down = 2.7; }
    else if (clef == "alto") { w = 2.4; up = 2.1; down = 2.1; }
    else { w = 2.1; up = 4.75; down = 3.1; }
}

// the clef of a staff: x its left edge, line the y of the line it names (G: the second from the bottom, F: the fourth,
// C: the middle one), u the space between the lines
void drawClef(const std::string& clef, double x, double line, double u) {
    if (clef == "bass") {                                    // F: a hook from the dot on the F line, the dots around it
        double X = x + 0.42 * u;
        clefDot(0, 0, 0.4, X, line, u);
        clefStroke({{-0.05, -0.15, 0.22}, {0.15, -0.7, 0.2}, {0.75, -1.05, 0.16}, {1.4, -0.85, 0.3}, {1.72, -0.2, 0.46},
                    {1.6, 0.75, 0.42}, {1.05, 1.7, 0.26}, {0.0, 2.55, 0.06}},
                   X, line, u);
        clefDot(2.0, -0.5, 0.17, X, line, u);
        clefDot(2.0, 0.5, 0.17, X, line, u);
        return;
    }
    if (clef == "alto") {                                    // C: two bars, two curves meeting on the middle line
        double X = x;
        for (int l = 0; l < 2; ++l) {
            double bx = l ? 0.62 : 0, bw = l ? 0.16 : 0.42;
            fl_begin_polygon();
            zd::vertex(X + bx * u, line - 2.0 * u);
            zd::vertex(X + (bx + bw) * u, line - 2.0 * u);
            zd::vertex(X + (bx + bw) * u, line + 2.0 * u);
            zd::vertex(X + bx * u, line + 2.0 * u);
            fl_end_polygon();
        }
        for (int sgn : {-1, 1}) {
            clefStroke({{0.82, 0.02 * sgn, 0.1}, {1.15, 0.45 * sgn, 0.14}, {1.65, 0.62 * sgn, 0.24}, {2.08, 1.15 * sgn, 0.34},
                        {1.9, 1.78 * sgn, 0.24}, {1.4, 1.98 * sgn, 0.12}, {1.08, 1.7 * sgn, 0.1}},
                       X, line, u);
            clefDot(1.28, 1.6 * sgn, 0.26, X, line, u);
        }
        return;
    }
    // G (treble, and anything else with a staff): the spiral around the G line, the loop above, the stem and its hook
    double X = x + 0.95 * u;
    clefStroke({{0.12, 0.05, 0.14},  {0.42, -0.32, 0.16}, {0.15, -0.72, 0.18}, {-0.45, -0.62, 0.24}, {-0.85, 0.0, 0.38},
                {-0.6, 0.78, 0.4},   {0.15, 1.05, 0.3},   {0.85, 0.62, 0.22},  {1.0, -0.12, 0.24},  {0.68, -0.95, 0.28},
                {0.1, -1.72, 0.3},   {-0.32, -2.55, 0.26}, {-0.36, -3.45, 0.2}, {-0.05, -4.3, 0.15}, {0.32, -4.6, 0.13},
                {0.48, -4.05, 0.14}, {0.28, -3.2, 0.18},  {-0.12, -2.35, 0.2}, {0.02, -0.9, 0.2},   {0.22, 0.6, 0.19},
                {0.36, 2.0, 0.18},   {0.22, 2.62, 0.15},  {-0.28, 2.8, 0.13},  {-0.6, 2.45, 0.12}},
               X, line, u);
    clefDot(-0.5, 2.38, 0.32, X, line, u);
}

// ---- drawing
void RollCore::draw(double ox, double oy) {
    syncSelection();
    layoutRows();
    const Palette& P = palette(compact);
    const double gut = gutterOf(compact), ruler = rulerOf(compact);
    zd::push_clip(ox, oy, w_, h_);
    fl_color(P.bg);
    zd::rectf(ox, oy, w_, h_);
    auto s = cached_;
    if (!s) {
        fl_color(P.dim);
        zd::font(FL_HELVETICA, 11);
        zd::draw("(no score)", ox + gut, oy + h_ / 2);
        zd::pop_clip();
        return;
    }
    // alternate rows (the piano roll: its keys)
    if (mode == Mode::Piano) drawPiano(ox, oy);
    else
        for (size_t k = 0; k < rows_.size(); ++k)
            if (k % 2 == 1) { fl_color(P.rowAlt); zd::rectf(ox, oy + rows_[k].top, w_, rows_[k].h); }
    // the grid: bars and beats
    {
        zd::push_clip(ox + gut, oy + ruler, w_ - gut, h_ - ruler);
        double bar = barSeconds(), beat = bar / std::max(1, s->meterNum);
        double t1 = tOf(w_);
        if (beat * pps >= (compact ? 12 : 8) && !compact) {
            fl_color(P.beat);
            for (double t = std::floor(t0 / beat) * beat; t <= t1; t += beat) zd::yxline(ox + xOf(t), oy + ruler, oy + h_);
        }
        if (bar * pps >= 4) {
            fl_color(P.bar);
            for (double t = std::floor(t0 / bar) * bar; t <= t1; t += bar) zd::yxline(ox + xOf(t), oy + ruler, oy + h_);
        }
        zd::pop_clip();
    }
    // the rows: staves or lines, names and clefs
    if (mode == Mode::Staves)
        for (size_t k = 0; k < rows_.size(); ++k) drawRow(ox, oy, rows_[k], (int)k);
    else if (!compact) {                                       // the piano roll: a legend of the rows' colours
        zd::font(FL_HELVETICA_BOLD, 11);
        double lx = w_ - 8;
        for (size_t k = rows_.size(); k-- > 0;) {
            if (!rows_[k].spec.pitched) continue;
            std::string n = lay::rowLabel(rows_[k].spec);
            double tw = zd::width(n.c_str());
            lx -= tw + 22;
            fl_color(rollRowColour((int)k));
            zd::rectf(ox + lx, oy + rulerOf(compact) + 6, 10, 10);
            fl_color(P.text);
            zd::draw(n.c_str(), ox + lx + 14, oy + rulerOf(compact) + 15);
        }
    }
    // the events
    zd::push_clip(ox + gut, oy + ruler, w_ - gut, h_ - ruler);
    bool dragging = drag_ == Drag::Move || drag_ == Drag::Resize;
    drawItems(ox, oy, 0, 0, 0, false);
    if (dragging && moved_) drawItems(ox, oy, dragDt_, dragDp_, dragDd_, true);
    if (drag_ == Drag::Draw) {
        fl_color(P.ghost);
        double y = yOfPitch(anchorRow_, drawPitch_);
        zd::rectf(ox + xOf(drawAt_), oy + y - 1.5, std::max(2.0, (drawEnd_ - drawAt_) * pps), 3);
        zd::pie(ox + xOf(drawAt_) - 4, oy + y - 4, 8, 8, 0, 360);
    }
    if (drag_ == Drag::Rubber && moved_) {
        fl_color(P.sel);
        zd::line_style(FL_DOT, 1);
        zd::rect(ox + std::min(px_, cx_), oy + std::min(py_, cy_), std::fabs(cx_ - px_), std::fabs(cy_ - py_));
        zd::line_style(0);
    }
    // the playhead
    double ph = shownTime(playhead);
    if (!std::isnan(ph)) {
        double x = xOf(ph);
        if (x >= gut && x <= w_) {
            fl_color(P.playhead);
            zd::line_style(FL_SOLID, compact ? 1 : 2);
            zd::yxline(ox + x, oy + ruler, oy + h_);
            zd::line_style(0);
        }
    }
    zd::pop_clip();
    if (!compact) drawRuler(ox, oy);
    zd::pop_clip();
}

void RollCore::drawRuler(double ox, double oy) {
    const Palette& P = palette(compact);
    const double gut = gutterOf(compact), H = rulerOf(compact);
    fl_color(fl_color_average(P.bg, FL_BLACK, 0.92f));
    zd::rectf(ox, oy, w_, H);
    fl_color(P.bar);
    zd::xyline(ox, oy + H - 1, ox + w_);
    auto s = cached_;
    if (!s) return;
    double bar = barSeconds();
    zd::font(FL_HELVETICA, 10);
    fl_color(P.dim);
    zd::push_clip(ox + gut, oy, w_ - gut, H);
    double t1 = tOf(w_);
    int every = std::max(1, (int)std::ceil(36 / std::max(1e-9, bar * pps)));
    long n0 = (long)std::floor(t0 / bar);
    for (long n = n0; n * bar <= t1; ++n) {
        if (n % every) continue;
        double x = xOf(n * bar);
        zd::yxline(ox + x, oy + H - 6, oy + H);
        zd::draw(std::to_string(n + 1).c_str(), ox + x + 3, oy + H - 6);
    }
    zd::pop_clip();
    fl_color(P.dim);
    zd::draw((fmtNum(s->bpm, 1) + " bpm " + std::to_string(s->meterNum) + "/" + std::to_string(s->meterDen)).c_str(), ox + 4, oy + H - 6);
}

RollCore::ClefInk RollCore::clefInk(int row) {
    ClefInk out;
    double x, y, w, h;
    if (!clefBox(row, x, y, w, h)) return out;
    const RowGeom& r = rows_[(size_t)row];
    Fl_Image_Surface surf((int)std::ceil(w_), (int)std::ceil(h_));
    Fl_Surface_Device::push_current(&surf);
    draw(0, 0);
    Fl_Surface_Device::pop_current();
    Fl_RGB_Image* img = surf.image();
    if (!img) return out;
    uchar tr, tg, tb, br, bg, bb;
    Fl::get_color(palette(compact).text, tr, tg, tb);
    Fl::get_color(row % 2 ? palette(compact).rowAlt : palette(compact).bg, br, bg, bb);
    const uchar* d = (const uchar*)img->data()[0];
    int dp = img->d(), ld = img->ld() ? img->ld() : img->w() * dp;
    int x0 = std::max(0, (int)std::floor(x)), x1 = std::min(img->w(), (int)std::ceil(x + w));
    int y0 = std::max(0, (int)std::floor(y)), y1 = std::min(img->h(), (int)std::ceil(y + h));
    int n = 0, ink = 0;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            const uchar* q = d + py * ld + px * dp;
            ++n;
            // nearer the text's colour than the paper's (the edges of a shape are blended)
            if (std::abs(q[0] - tr) + std::abs(q[1] - tg) + std::abs(q[2] - tb) < std::abs(q[0] - br) + std::abs(q[1] - bg) + std::abs(q[2] - bb)) {
                ++ink;
                if (py < r.yBottom - 4 * r.sp - 1) out.above = true;
                if (py > r.yBottom + 1) out.below = true;
            }
        }
    delete img;
    out.drawn = true;
    out.ink = n ? (double)ink / n : 0;
    return out;
}

bool RollCore::clefBox(int row, double& x, double& y, double& w, double& h) const {
    layoutRows();
    if (mode == Mode::Piano || row < 0 || row >= (int)rows_.size()) return false;
    const RowGeom& r = rows_[(size_t)row];
    if (!r.staff) return false;
    double cw, up, down;
    clefExtent(r.spec.clef, cw, up, down);
    double line = r.yBottom - (r.spec.clef == "bass" ? 3 : r.spec.clef == "alto" ? 2 : 1) * r.sp;
    x = compact ? gutterOf(true) - 3 - cw * r.sp : gutterOf(false) - 28;
    y = line - up * r.sp;
    w = cw * r.sp;
    h = (up + down) * r.sp;
    return true;
}

void RollCore::drawRow(double ox, double oy, const RowGeom& r, int k) {
    const Palette& P = palette(compact);
    const double gut = gutterOf(compact);
    if (r.top + r.h < rulerOf(compact) || r.top > h_) return;
    // the clef: a shape scaled with the staff (in the box too; a letter when the staff is too small for one)
    bool clef = r.staff && mode == Mode::Staves;
    bool shape = clef && r.sp >= (compact ? 2.5 : 3.0);
    double cx = 0, cy = 0, cw = 0, ch = 0;
    if (clef) clefBox(k, cx, cy, cw, ch);
    // the name (in the box: left of the clef)
    zd::font(compact ? FL_HELVETICA : FL_HELVETICA_BOLD, compact ? 9 : 12);
    fl_color(mode == Mode::Piano ? rollRowColour(k) : P.text);
    std::string name = lay::rowLabel(r.spec);
    if (compact && clef) zd::push_clip(ox, oy + r.top, std::max(4.0, cx - 1), r.h);
    if (mode == Mode::Staves || compact)
        zd::draw(name.c_str(), ox + (compact ? 2 : 4), oy + (r.staff ? r.yBottom - 2 * r.sp + 4 : r.top + r.h / 2 + 4));
    else
        zd::draw(name.c_str(), ox + 4, oy + r.top + 12);
    if (compact && clef) zd::pop_clip();
    if (mode == Mode::Piano) return;
    fl_color(P.line);
    if (r.staff) {
        double from = compact ? std::min(gut - 2, cx - 2) : gut - 30;
        for (int l = 0; l < 5; ++l) zd::xyline(ox + from, oy + r.yBottom - l * r.sp, ox + w_);
        fl_color(P.text);
        double line = r.yBottom - (r.spec.clef == "bass" ? 3 : r.spec.clef == "alto" ? 2 : 1) * r.sp;
        if (shape) {
            drawClef(r.spec.clef, ox + cx, oy + line, r.sp);
        } else if (clef) {                                      // too small for the shape: its letter
            const char* g = r.spec.clef == "bass" ? "F" : r.spec.clef == "alto" ? "C" : "G";
            zd::font(FL_HELVETICA_BOLD, std::max(6, (int)std::lround(r.sp * 3)));
            zd::draw(g, ox + cx, oy + r.yBottom - r.sp * 0.2);
        }
    } else {
        zd::xyline(ox + gut, oy + r.top + r.h / 2, ox + w_);
    }
}

void RollCore::drawPiano(double ox, double oy) {
    const Palette& P = palette(compact);
    const double gut = gutterOf(compact), ruler = rulerOf(compact);
    zd::push_clip(ox, oy + ruler, w_, h_ - ruler);
    for (int p = (int)pianoLo_; p <= (int)pianoHi_; ++p) {
        double y = ruler - vscroll + (pianoHi_ + 0.5 - p) * keyH_ - keyH_ / 2;
        int pc = ((p % 12) + 12) % 12;
        bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        fl_color(black ? fl_color_average(P.bg, FL_BLACK, 0.85f) : P.bg);
        zd::rectf(ox + gut, oy + y, w_ - gut, keyH_);
        fl_color(black ? fl_rgb_color(40, 40, 44) : fl_rgb_color(250, 250, 250));
        zd::rectf(ox + gut - (compact ? 10 : 24), oy + y, compact ? 10 : 24, keyH_);
        fl_color(P.beat);
        zd::xyline(ox + gut - (compact ? 10 : 24), oy + y + keyH_, ox + w_);
        if (pc == 0 && !compact) {
            fl_color(P.dim);
            zd::font(FL_HELVETICA, 9);
            zd::draw(("C" + std::to_string(p / 12 - 1)).c_str(), ox + 4, oy + y + keyH_ - 1);
        }
    }
    zd::pop_clip();
}

void RollCore::drawNote(double ox, double oy, const Item& it, Fl_Color c, bool sel, bool ghost, double dt, double dp, double dd) {
    const Palette& P = palette(compact);
    const sc::Event& e = *it.e;
    double x = xOf(e.at + dt), w = std::max(2.0, (e.dur + dd) * pps);
    if (mode == Mode::Piano) {
        double y = it.y - dp * keyH_;
        fl_color(ghost ? P.ghost : c);
        zd::rectf(ox + x, oy + y, w, it.h);
        if (sel || ghost) { fl_color(ghost ? P.sel : P.text); zd::rect(ox + x, oy + y, w, it.h); }
        if (ghost) return;
        CentsMark m = centsOf(e, e.pitch + dp, it.h / 2);
        if (m.kind == CentsMark::Text) {
            zd::font(FL_HELVETICA, compact ? 7 : 9);
            fl_color(P.text);
            zd::draw(m.text.c_str(), ox + x + w + 2, oy + y + it.h);
        } else if (m.kind == CentsMark::Tick) {
            fl_color(P.sel);
            zd::yxline(ox + x, oy + y - 3, oy + y);
        }
        return;
    }
    const RowGeom& r = rows_[(size_t)it.row];
    double pitch = e.pitch + dp;
    double rad = it.h / 2;
    double y = r.staff ? r.yBottom - lay::halfSpaces(pitch, r.spec.clef) * r.sp / 2 : it.y + rad;
    Fl_Color col = ghost ? P.ghost : sel ? P.sel : c;
    // ledger lines
    if (r.staff && !ghost) {
        int hs = lay::halfSpaces(pitch, r.spec.clef);
        fl_color(P.line);
        for (int k = -2; k >= hs; k -= 2) zd::xyline(ox + x - rad * 2, oy + r.yBottom - k * r.sp / 2, ox + x + rad * 2);
        for (int k = 10; k <= hs; k += 2) zd::xyline(ox + x - rad * 2, oy + r.yBottom - k * r.sp / 2, ox + x + rad * 2);
    }
    fl_color(col);
    double th = std::max(compact ? 1.0 : 2.0, rad * 0.7);
    zd::rectf(ox + x, oy + y - th / 2, w, th);
    zd::pie(ox + x - rad, oy + y - rad, 2 * rad, 2 * rad, 0, 360);
    if (sel && !ghost) {
        fl_color(P.text);
        zd::arc(ox + x - rad - 1, oy + y - rad - 1, 2 * rad + 2, 2 * rad + 2, 0, 360);
    }
    if (ghost) return;
    // the detuning: always shown (orchidea's notes carry the cents of the partial they imitate)
    CentsMark m = centsOf(e, pitch, rad);
    if (m.kind == CentsMark::Text) {
        zd::font(FL_HELVETICA, compact ? 7 : 9);
        fl_color(compact ? P.text : P.dim);
        zd::draw(m.text.c_str(), ox + x - rad, oy + y - rad - (compact ? 1 : 2));
    } else if (m.kind == CentsMark::Tick) {
        fl_color(P.sel);
        zd::yxline(ox + x, oy + y - rad - 3, oy + y - rad);
    }
    if (compact) return;
    bool sharp;
    lay::diatonicStep((int)std::floor(pitch + 1e-9), sharp);
    zd::font(FL_HELVETICA_BOLD, std::max(8, (int)std::lround(r.sp * 2.2)));
    if (sharp) zd::draw("#", ox + x - rad - std::max(6.0, r.sp * 1.6), oy + y + r.sp * 0.8);
}

RollCore::CentsMark RollCore::centsOf(const sc::Event& e, double pitch, double rad) const {
    CentsMark m;
    if (!pitchedEv(e)) return m;
    double c = e.cents + (pitch - std::floor(pitch + 1e-9)) * 100;
    if (!(std::fabs(c) > 0.5)) return m;
    m.cents = c;
    m.text = (c > 0 ? "+" : "") + fmtNum(c, 0);
    m.kind = compact && rad < 2.0 ? CentsMark::Tick : CentsMark::Text;   // no room for the number: a tick
    return m;
}

RollCore::CentsMark RollCore::centsMark(uint64_t id) const {
    layoutRows();
    for (auto& it : items_)
        if (it.e->id == id) return centsOf(*it.e, it.e->pitch, it.h / 2);
    return {};
}

void RollCore::drawItems(double ox, double oy, double dt, double dp, double dd, bool ghost) {
    const Palette& P = palette(compact);
    std::map<int, std::string> lastDyn, lastTech;
    for (auto& it : items_) {
        const sc::Event& e = *it.e;
        bool sel = selection.count(e.id) > 0;
        if (ghost && !sel) continue;
        double gdt = ghost ? dt : 0, gdp = ghost ? dp : 0, gdd = ghost && drag_ == Drag::Resize ? dd : 0;
        if (ghost && drag_ == Drag::Resize) gdt = 0, gdp = 0;
        if (pitchedEv(e)) {
            drawNote(ox, oy, it, rollRowColour(it.row), sel, ghost, gdt, gdp, gdd);
            if (!compact && !ghost && mode == Mode::Staves && rows_[(size_t)it.row].staff) {
                const RowGeom& r = rows_[(size_t)it.row];
                zd::font(FL_HELVETICA_ITALIC, 10);
                fl_color(P.dim);
                if (!e.dyn.empty() && lastDyn[it.row] != e.dyn) zd::draw(e.dyn.c_str(), ox + xOf(e.at) - 3, oy + r.yBottom + r.sp * 3.2);
                lastDyn[it.row] = e.dyn;
                zd::font(FL_HELVETICA, 9);
                if (!e.tech.empty() && lastTech[it.row] != e.tech && e.tech != "ord")
                    zd::draw(e.tech.c_str(), ox + xOf(e.at) - 3, oy + r.yBottom - r.sp * 6);
                lastTech[it.row] = e.tech;
            }
            continue;
        }
        double x = xOf(e.at + gdt), w = std::max(2.0, (e.dur + gdd) * pps);
        if (e.kind == sc::Kind::Score) {
            // a score placed: a faint block with its notes and its placement
            fl_color(ghost ? P.ghost : P.scoreFill);
            if (!ghost) zd::rectf(ox + x, oy + it.y, w, it.h);
            fl_color(sel ? P.sel : P.scoreLine);
            zd::rect(ox + x, oy + it.y, w, it.h);
            if (ghost) continue;
            const RowGeom& r = rows_[(size_t)it.row];
            if (e.sub) {
                std::vector<sc::Placed> flat;
                try { sc::flatten(*e.sub, flat, (int)path.size() + 1); } catch (const std::exception&) { flat.clear(); }
                fl_color(fl_color_average(P.scoreLine, P.scoreFill, 0.55f));
                double lo = 1e9, hi = -1e9;
                for (auto& p : flat) if (pitchedEv(p.ev)) { lo = std::min(lo, p.ev.pitch); hi = std::max(hi, p.ev.pitch); }
                for (auto& p : flat) {
                    double rel = p.ev.at * e.stretch;
                    if (rel >= e.dur || !pitchedEv(p.ev)) continue;
                    double px = xOf(e.at + rel), pw = std::max(1.0, std::min(p.ev.dur * e.stretch, e.dur - rel) * pps);
                    double py;
                    if (r.staff && mode == Mode::Staves) py = r.yBottom - lay::halfSpaces(p.ev.pitch + e.transpose, r.spec.clef) * r.sp / 2;
                    else py = it.y + 14 + (hi > lo ? (hi - p.ev.pitch) / (hi - lo) : 0.5) * std::max(1.0, it.h - 18);
                    double rr = compact ? 1.2 : std::max(1.5, r.sp * 0.45);
                    zd::rectf(ox + px, oy + py - 0.75, pw, 1.5);
                    zd::pie(ox + px - rr, oy + py - rr, 2 * rr, 2 * rr, 0, 360);
                }
            }
            if (!compact || it.h > 14) {
                fl_color(P.scoreLine);
                zd::rectf(ox + x, oy + it.y, w, compact ? 10 : 15);
                fl_color(FL_WHITE);
                zd::font(FL_HELVETICA_BOLD, compact ? 8 : 10);
                std::string t = (e.sub && !e.sub->name.empty() ? e.sub->name : std::string("score")) + placementText(e) +
                                (compact ? "" : "  (double-click to enter)");
                zd::push_clip(ox + x, oy + it.y, w, 15);
                zd::draw(t.c_str(), ox + x + 3, oy + it.y + (compact ? 8 : 11));
                zd::pop_clip();
            }
            continue;
        }
        Fl_Color c = e.kind == sc::Kind::Sound ? P.sound : e.kind == sc::Kind::Patch ? P.patch : P.msg;
        if (ghost) c = P.ghost;
        else if (sel) c = P.sel;
        fl_color(c);
        if (e.kind == sc::Kind::Message) {
            double cy = it.y + it.h / 2, rr = std::min(4.0, it.h / 2);
            zd::polygon(ox + x, oy + cy - rr, ox + x + rr, oy + cy, ox + x, oy + cy + rr);
            zd::polygon(ox + x, oy + cy - rr, ox + x - rr, oy + cy, ox + x, oy + cy + rr);
            if (!compact && !ghost) {
                std::string t = e.source;
                for (auto& v : e.values) t += " " + (v.kind == sc::Datum::Num ? fmtNum(v.num) : v.s);
                fl_color(P.dim);
                zd::font(FL_HELVETICA, 9);
                zd::draw(t.c_str(), ox + x + 6, oy + cy + 3);
            }
            continue;
        }
        zd::rectf(ox + x, oy + it.y, w, it.h);
        if (!compact && !ghost && it.h >= 10) {
            std::string t = !e.label.empty() ? e.label : e.source;
            size_t sl = t.find_last_of("/\\");
            if (sl != std::string::npos) t = t.substr(sl + 1);
            fl_color(FL_WHITE);
            zd::font(FL_HELVETICA, 9);
            zd::push_clip(ox + x, oy + it.y, w, it.h);
            zd::draw(t.c_str(), ox + x + 3, oy + it.y + std::min(it.h - 2, 11.0));
            zd::pop_clip();
        }
    }
}

// ---- the mouse and the keys
bool RollCore::mouse(int event, double x, double y, int keys, int clicks) {
    if (!target_) return false;
    syncSelection();
    layoutRows();
    bool editable = target_->editable();
    bool shift = keys & FL_SHIFT, alt = keys & FL_ALT;
    switch (event) {
    case FL_PUSH: {
        px_ = cx_ = x;
        py_ = cy_ = y;
        moved_ = false;
        dragDt_ = dragDp_ = dragDd_ = 0;
        if (y < rulerOf(compact) || x < gutterOf(compact)) return false;
        Hit h = hit(x, y);
        if (clicks > 0 && h.id) {                                   // a double-click on a score placed: into it
            auto s = shown();
            auto e = s ? s->find(h.id) : nullptr;
            if (e && e->kind == sc::Kind::Score) { drag_ = Drag::None; if (onEnter) onEnter(h.id); else enter(h.id); return true; }
        }
        if (clicks > 0 && compact) { drag_ = Drag::None; if (onEnter) onEnter(0); return true; }   // the box: its window
        if (tool == Tool::Erase && editable) {
            drag_ = Drag::Erase;
            erasing_.clear();
            if (h.id) erasing_.insert(h.id);
            return true;
        }
        if (tool == Tool::Draw && editable && !h.id) {
            int row = h.row;
            if (mode == Mode::Piano) {
                row = -1;
                auto s = shown();
                if (s && !selection.empty()) if (auto e = s->find(*selection.begin())) row = lay::rowOf(lay::rows(*s), *e);
                if (row < 0) for (size_t k = 0; k < rows_.size(); ++k) if (rows_[k].spec.pitched) { row = (int)k; break; }
            }
            double p = pitchAtY(row, y);
            if (std::isnan(p)) { if (onStatus) onStatus("Draw: on a staff (a row of notes)"); return true; }
            drag_ = Drag::Draw;
            anchorRow_ = row;
            drawPitch_ = p;
            drawAt_ = snapTime(tOf(x));
            double unit = snapSeconds() > 0 ? snapSeconds() : 60.0 / std::max(1.0, shown()->bpm);
            drawEnd_ = drawAt_ + unit;
            if (onChange) onChange();
            return true;
        }
        if (h.id) {
            if (shift) {
                if (selection.count(h.id)) { selection.erase(h.id); drag_ = Drag::None; if (onChange) onChange(); return true; }
                selection.insert(h.id);
            } else if (!selection.count(h.id)) {
                selection = {h.id};
            }
            anchor_ = h.id;
            anchorRow_ = h.row;
            copy_ = alt;
            drag_ = editable ? (h.end ? Drag::Resize : Drag::Move) : Drag::None;
        } else {
            additive_ = shift;
            before_ = selection;
            if (!shift) selection.clear();
            drag_ = Drag::Rubber;
        }
        if (onChange) onChange();
        return true;
    }
    case FL_DRAG: {
        if (drag_ == Drag::None) return false;
        cx_ = x;
        cy_ = y;
        if (std::fabs(x - px_) + std::fabs(y - py_) > 2) moved_ = true;
        auto s = shown();
        auto a = s && anchor_ ? s->find(anchor_) : nullptr;
        if (drag_ == Drag::Move && a) {
            double raw = (x - px_) / std::max(1e-9, pps);
            dragDt_ = snapTime(a->at + raw) - a->at;
            if (a->at + dragDt_ < 0) dragDt_ = -a->at;
            double p0 = pitchAtY(anchorRow_, py_), p1 = pitchAtY(anchorRow_, y);
            dragDp_ = std::isnan(p0) || std::isnan(p1) ? 0 : p1 - p0;
        } else if (drag_ == Drag::Resize && a) {
            double raw = (x - px_) / std::max(1e-9, pps);
            double end = snapTime(a->end() + raw);
            double unit = snapSeconds() > 0 ? snapSeconds() : 0.01;
            dragDd_ = std::max(unit, end - a->at) - a->dur;
        } else if (drag_ == Drag::Draw) {
            double unit = snapSeconds() > 0 ? snapSeconds() : 0.05;
            drawEnd_ = std::max(drawAt_ + unit, snapTime(tOf(x)));
        } else if (drag_ == Drag::Erase) {
            Hit h = hit(x, y);
            if (h.id) erasing_.insert(h.id);
        } else if (drag_ == Drag::Rubber) {
            selection = additive_ ? before_ : std::set<uint64_t>{};
            double x0 = std::min(px_, x), x1 = std::max(px_, x), y0 = std::min(py_, y), y1 = std::max(py_, y);
            for (auto& it : items_)
                if (it.x < x1 && it.x + it.w > x0 && it.y < y1 && it.y + it.h > y0) selection.insert(it.e->id);
        }
        if (onChange) onChange();
        return true;
    }
    case FL_RELEASE: {
        if (drag_ == Drag::None) return false;
        cx_ = x;
        cy_ = y;
        finishDrag(x, y, keys);
        drag_ = Drag::None;
        syncSelection();
        if (onChange) onChange();
        return true;
    }
    }
    return false;
}

void RollCore::finishDrag(double, double, int keys) {
    RollEditTarget* t = target_.get();
    Ids ids = selectedIds();
    switch (drag_) {
    case Drag::Move:
        if (!moved_ || (dragDt_ == 0 && dragDp_ == 0)) return;
        if (copy_ || (keys & FL_ALT)) {
            Ids made = t->copyEvents(path, ids, dragDt_, dragDp_);
            if (!made.empty()) selection = std::set<uint64_t>(made.begin(), made.end());
            if (onStatus) onStatus(std::to_string(made.size()) + " copied");
        } else if (t->moveEvents(path, ids, dragDt_, dragDp_)) {
            if (onStatus) onStatus(std::to_string(ids.size()) + " moved");
        }
        break;
    case Drag::Resize:
        if (moved_ && dragDd_ != 0) t->resizeEvents(path, ids, dragDd_);
        break;
    case Drag::Draw: {
        sc::Event e;
        e.kind = sc::Kind::Note;
        e.at = drawAt_;
        e.dur = std::max(0.01, drawEnd_ - drawAt_);
        e.pitch = drawPitch_;
        e.dyn = "mf";
        if (anchorRow_ >= 0 && anchorRow_ < (int)rows_.size() && !rows_[(size_t)anchorRow_].spec.byKind) e.row = rows_[(size_t)anchorRow_].spec.name;
        uint64_t id = t->insertEvent(path, e);
        if (id) selection = {id};
        if (onStatus) onStatus("a note added: " + pitchName(e.pitch));
        break;
    }
    case Drag::Erase:
        if (!erasing_.empty()) {
            Ids gone(erasing_.begin(), erasing_.end());
            t->removeEvents(path, gone);
            for (uint64_t id : gone) selection.erase(id);
        }
        erasing_.clear();
        break;
    default:
        break;
    }
    if (!t->lastError.empty() && onStatus) onStatus(t->lastError);
}

void RollCore::removeSelection() {
    if (selection.empty() || !target_) return;
    Ids ids = selectedIds();
    if (target_->removeEvents(path, ids)) {
        if (onStatus) onStatus(std::to_string(ids.size()) + " deleted");
        selection.clear();
    }
    if (onChange) onChange();
}

void RollCore::nudge(double dt, double dp) {
    if (selection.empty() || !target_) return;
    target_->moveEvents(path, selectedIds(), dt, dp);
    if (onChange) onChange();
}

void RollCore::detuneSelection(double dc) {
    if (selection.empty() || !target_) return;
    bool ok = target_->detune(path, selectedIds(), dc);
    if (onStatus) onStatus(ok ? (dc > 0 ? "+" : "") + fmtNum(dc, 0) + (std::fabs(dc) == 1 ? " cent" : " cents")
                              : (target_->lastError.empty() ? std::string("no note to detune") : target_->lastError));
    if (onChange) onChange();
}

void RollCore::quantizeSelection() {
    ScoreTransform t;
    t.kind = ScoreTransform::Quantize;
    t.a = snapSeconds() > 0 ? snapSeconds() : 60.0 / std::max(1.0, shown() ? shown()->bpm : 60) / 4;
    t.b = 1;
    transformSelection(t);
}

bool RollCore::transformSelection(ScoreTransform t) {
    if (!target_) return false;
    bool ok = target_->transform(path, selectedIds(), t);
    if (onStatus) onStatus(ok ? (selection.empty() ? std::string("the whole score transformed") : std::to_string(selection.size()) + " transformed")
                              : (target_->lastError.empty() ? std::string("nothing to transform") : target_->lastError));
    // events taken out (thin, density) leave the selection
    if (auto s = shown())
        for (auto it = selection.begin(); it != selection.end();) it = s->find(*it) ? std::next(it) : selection.erase(it);
    if (onChange) onChange();
    return ok;
}

bool RollCore::key(int k, int keys) {
    syncSelection();
    bool shift = keys & FL_SHIFT;
    bool cmd = keys & FL_COMMAND;
    if (cmd && (k == 'a' || k == 'A')) { selectAll(); return true; }
    if (cmd) return false;
    switch (k) {
    case FL_Delete:
    case FL_BackSpace: removeSelection(); return !selection.empty() || true;
    case FL_Left:
    case FL_Right: {
        double unit = shift ? barSeconds() : (snapSeconds() > 0 ? snapSeconds() : 0.1);
        nudge(k == FL_Left ? -unit : unit, 0);
        return true;
    }
    case FL_Up:
    case FL_Down:
        if (keys & FL_ALT) detuneSelection((k == FL_Up ? 1 : -1) * (shift ? 10 : 1));   // the cents: one, or ten
        else nudge(0, (k == FL_Up ? 1 : -1) * (shift ? 12 : 1));
        return true;
    case 'q': quantizeSelection(); return true;
    case FL_Escape:
        if (!selection.empty()) { selection.clear(); if (onChange) onChange(); return true; }
        if (!path.empty()) { up(); return true; }
        return false;
    case FL_Enter:
    case FL_KP_Enter:
        if (selection.size() == 1) {
            auto s = shown();
            auto e = s ? s->find(*selection.begin()) : nullptr;
            if (e && e->kind == sc::Kind::Score) { if (onEnter && !compact) onEnter(e->id); else enter(e->id); return true; }
        }
        return false;
    }
    return false;
}

// ================================================================ the widgets of the panel
RollWidget::RollWidget(int x, int y, int w, int h, RollPanel* panel) : Fl_Widget(x, y, w, h), panel_(panel) {
    panel_->core_.setSize(w, h);
}
RollCore& RollWidget::core() { return panel_->core_; }

void RollWidget::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    core().setSize(W, H);
}

void RollWidget::draw() {
    core().setSize(w(), h());
    core().draw(x(), y());
    if (Fl::focus() == this) {
        fl_color(kAccent);
        fl_rect(x(), y(), w(), h());
    }
}

int RollWidget::handle(int e) {
    RollCore& c = core();
    switch (e) {
    case FL_FOCUS:
    case FL_UNFOCUS:
        redraw();
        return 1;
    case FL_ENTER:
    case FL_LEAVE:
    case FL_MOVE:
        return 1;
    case FL_PUSH: {
        // the event read first: taking the focus from another window can send events that change it
        int ex = Fl::event_x(), ey = Fl::event_y(), keys = Fl::event_state(), clicks = Fl::event_clicks();
        take_focus();
        Fl::e_x = ex;
        Fl::e_y = ey;
        Fl::e_state = keys;
        Fl::e_clicks = clicks;
        if (ey - y() < 20 && ex - x() > 78) {      // the ruler: the transport goes there
            double t = c.tOf(ex - x());
            auto pl = c.placements();
            for (auto it = pl.rbegin(); it != pl.rend(); ++it) t = it->at + t * it->stretch;
            if (ScorePlayback* pb = panel_->playback()) pb->setPlayhead(std::max(0.0, t));
            else if (Patch* p = c.target() ? c.target()->patch() : nullptr) transportOf(p)->locate(p, std::max(0.0, t));
            c.playhead = std::max(0.0, t);
            redraw();
            return 1;
        }
    }
        [[fallthrough]];
    case FL_DRAG:
    case FL_RELEASE:
        if (c.mouse(e, Fl::event_x() - x(), Fl::event_y() - y(), Fl::event_state(), e == FL_PUSH ? Fl::event_clicks() : 0)) {
            if (e == FL_PUSH && Fl::event_clicks()) Fl::event_clicks(0);
            return 1;
        }
        return e == FL_PUSH ? 1 : 0;
    case FL_MOUSEWHEEL:
        if (Fl::event_state(FL_COMMAND | FL_CTRL)) c.zoom(Fl::event_dy() < 0 ? 1.25 : 0.8, Fl::event_x() - x());
        else {
            c.t0 = std::max(0.0, c.t0 + Fl::event_dx() * 16 / std::max(1e-9, c.pps));
            c.vscroll = std::max(0.0, c.vscroll + Fl::event_dy() * 16);
        }
        panel_->changed();
        return 1;
    case FL_KEYBOARD:
        if (Fl::event_key() == ' ' && !Fl::event_state(FL_COMMAND | FL_CTRL | FL_ALT)) {   // whatever is selected
            panel_->togglePlay();
            return 1;
        }
        if (c.key(Fl::event_key(), Fl::event_state())) { panel_->changed(); return 1; }
        return 0;
    }
    return Fl_Widget::handle(e);
}

void DynamicsLane::draw() {
    RollCore& c = panel_->core_;
    const Palette& P = palette(false);
    fl_push_clip(x(), y(), w(), h());
    fl_color(fl_color_average(P.bg, FL_BLACK, 0.9f));
    // around the row chooser, never under it: redrawn alone (a value chosen, a note dragged), the lane would hide it
    Fl_Widget* ch = panel_->dynRow_;
    if (ch && ch->visible() && ch->parent() == parent() && ch->x() >= x() && ch->y() >= y() && ch->x() + ch->w() <= x() + w() &&
        ch->y() + ch->h() <= y() + h()) {
        fl_rectf(x(), y(), w(), ch->y() - y());
        fl_rectf(x(), ch->y() + ch->h(), w(), y() + h() - ch->y() - ch->h());
        fl_rectf(x(), ch->y(), ch->x() - x(), ch->h());
        fl_rectf(ch->x() + ch->w(), ch->y(), x() + w() - ch->x() - ch->w(), ch->h());
    } else {
        fl_rectf(x(), y(), w(), h());
    }
    fl_color(P.dim);
    fl_font(FL_HELVETICA, 10);
    fl_draw("dynamics", x() + 4, y() + 12);
    auto s = c.shown();
    if (s) {
        auto specs = lay::rows(*s);
        int base = y() + h() - 4, top = y() + 8;
        std::map<int, std::string> last;
        fl_push_clip(x() + 78, y(), w() - 78, h());
        for (const sc::Event* e : s->byTime()) {
            if (!pitchedEv(*e)) continue;
            if (!row.empty() && e->row != row) continue;
            int r = lay::rowOf(specs, *e);
            int lvl = dragging_ == e->id && level_ >= 0 ? level_ : dynLevel(e->dyn);
            double v = (lvl >= 0 ? (lvl + 1) / 8.0 : 0.6) * std::min(1.0, e->gain);
            int bx = x() + (int)std::lround(c.xOf(e->at));
            int bh = (int)std::lround(v * (base - top));
            bool sel = c.selection.count(e->id) > 0;
            fl_color(sel ? P.sel : rollRowColour(r));
            fl_rectf(bx - 2, base - bh, 5, bh);
            std::string d = lvl >= 0 ? kDynamics[lvl] : e->dyn;
            if (!d.empty() && last[r] != d) {
                fl_color(P.text);
                fl_font(FL_HELVETICA_ITALIC, 10);
                fl_draw(d.c_str(), bx + 4, base - bh + 8);
            }
            last[r] = d;
        }
        fl_pop_clip();
    }
    fl_pop_clip();
}

int DynamicsLane::handle(int e) {
    RollCore& c = panel_->core_;
    auto s = c.shown();
    if (!s) return Fl_Widget::handle(e);
    int base = y() + h() - 4, top = y() + 8;
    auto levelAt = [&](int ey) { return std::clamp((int)std::lround((double)(base - ey) / (base - top) * 8) - 1, 0, 7); };
    switch (e) {
    case FL_PUSH: {
        dragging_ = 0;
        double best = 6;
        for (auto& ev : s->events()) {
            if (!pitchedEv(*ev) || (!row.empty() && ev->row != row)) continue;
            double d = std::fabs(x() + c.xOf(ev->at) - Fl::event_x());
            if (d < best) { best = d; dragging_ = ev->id; }
        }
        if (!dragging_) return 1;
        if (!Fl::event_state(FL_SHIFT) && !c.selection.count(dragging_)) c.selection = {dragging_};
        else c.selection.insert(dragging_);
        level_ = -1;
        panel_->changed();
        return 1;
    }
    case FL_DRAG:
        if (!dragging_) return 1;
        level_ = levelAt(Fl::event_y());
        redraw();
        return 1;
    case FL_RELEASE:
        if (dragging_ && level_ >= 0 && c.target()) {
            Ids ids = c.selection.count(dragging_) ? c.selectedIds() : Ids{dragging_};
            c.target()->setField(c.path, ids, "dyn", sc::Datum::symbol(kDynamics[level_]));
        }
        dragging_ = 0;
        level_ = -1;
        panel_->changed();
        return 1;
    }
    return Fl_Widget::handle(e);
}

namespace {
struct Column {
    const char* title;
    int w;
    const char* key;          // the field a double-click edits ("" none)
};
const Column kColumns[] = {{"id", 44, ""},     {"kind", 54, ""},  {"row", 64, "row"},  {"at", 74, "at"},    {"dur", 74, "dur"},
                           {"pitch", 74, "pitch"}, {"cents", 54, "cents"}, {"dyn", 48, "dyn"}, {"tech", 84, "tech"},
                           {"gain", 54, "gain"}, {"source / label", 220, "label"}};
constexpr int kListRowH = 18;
std::string cellText(const sc::Event& e, int col) {
    switch (col) {
    case 0: return std::to_string(e.id);
    case 1: return sc::kindName(e.kind);
    case 2: return e.row;
    case 3: return fmtNum(e.at, 4);
    case 4: return fmtNum(e.dur, 4);
    case 5: return pitchedEv(e) ? pitchName(e.pitch) : "";
    case 6: return pitchedEv(e) && e.cents != 0 ? fmtNum(e.cents, 1) : "";
    case 7: return e.dyn;
    case 8: return e.tech;
    case 9: return fmtNum(e.gain, 3);
    case 10: {
        std::string s = !e.label.empty() ? e.label : e.source;
        if (e.kind == sc::Kind::Score && e.sub) s = e.sub->name + placementText(e);
        for (auto& v : e.values) s += " " + (v.kind == sc::Datum::Num ? fmtNum(v.num) : v.s);
        return s;
    }
    }
    return "";
}
}  // namespace

std::vector<const sc::Event*> EventList::rowsShown() const {
    auto s = panel_->core_.shown();
    return s ? s->byTime() : std::vector<const sc::Event*>{};
}

void EventList::draw() {
    RollCore& c = panel_->core_;
    fl_push_clip(x(), y(), w(), h());
    fl_color(fl_rgb_color(26, 27, 31));
    fl_rectf(x(), y(), w(), h());
    fl_font(FL_HELVETICA_BOLD, 11);
    fl_color(kStatusBg);
    fl_rectf(x(), y(), w(), kListRowH);
    int cx = x() + 6;
    fl_color(kDim);
    for (auto& col : kColumns) { fl_draw(col.title, cx, y() + 13); cx += col.w; }
    auto rows = rowsShown();
    fl_font(FL_HELVETICA, 11);
    int n = (h() - kListRowH) / kListRowH;
    scroll = std::clamp(scroll, 0, std::max(0, (int)rows.size() - n));
    for (int k = 0; k < n && scroll + k < (int)rows.size(); ++k) {
        const sc::Event& e = *rows[(size_t)(scroll + k)];
        int ry = y() + kListRowH * (k + 1);
        if (c.selection.count(e.id)) { fl_color(fl_rgb_color(48, 70, 112)); fl_rectf(x(), ry, w(), kListRowH); }
        else if (k % 2) { fl_color(fl_rgb_color(31, 32, 37)); fl_rectf(x(), ry, w(), kListRowH); }
        cx = x() + 6;
        for (int col = 0; col < (int)(sizeof kColumns / sizeof kColumns[0]); ++col) {
            fl_color(col == 0 ? kDim : kText);
            fl_push_clip(cx, ry, kColumns[col].w - 4, kListRowH);
            fl_draw(cellText(e, col).c_str(), cx, ry + 13);
            fl_pop_clip();
            cx += kColumns[col].w;
        }
    }
    if (rows.empty()) { fl_color(kDim); fl_draw("(no events)", x() + 6, y() + kListRowH + 14); }
    fl_pop_clip();
}

void EventList::editCell(int r, int col) {
    auto rows = rowsShown();
    if (r < 0 || r >= (int)rows.size() || col < 0 || col >= (int)(sizeof kColumns / sizeof kColumns[0]) || !*kColumns[col].key) return;
    panel_->editCell(rows[(size_t)r]->id, kColumns[col].key, cellText(*rows[(size_t)r], col),
                     x() + 6 + [&] { int cx = 0; for (int k = 0; k < col; ++k) cx += kColumns[k].w; return cx; }() - 3,
                     y() + kListRowH * (r - scroll + 1), kColumns[col].w + 2);
}

int EventList::handle(int e) {
    RollCore& c = panel_->core_;
    switch (e) {
    case FL_PUSH: {
        int ey0 = Fl::event_y(), ex0 = Fl::event_x(), keys0 = Fl::event_state(), clicks0 = Fl::event_clicks();
        take_focus();                                   // (which can change the event: put back)
        Fl::e_x = ex0;
        Fl::e_y = ey0;
        Fl::e_state = keys0;
        Fl::e_clicks = clicks0;
        int k = (Fl::event_y() - y()) / kListRowH - 1;
        if (k < 0) return 1;
        auto rows = rowsShown();
        int r = scroll + k;
        if (r >= (int)rows.size()) { if (!Fl::event_state(FL_SHIFT)) c.selection.clear(); panel_->changed(); return 1; }
        uint64_t id = rows[(size_t)r]->id;
        if (Fl::event_clicks()) {
            Fl::event_clicks(0);
            int cx = Fl::event_x() - x() - 6, col = 0;
            for (; col < (int)(sizeof kColumns / sizeof kColumns[0]) - 1 && cx >= kColumns[col].w; ++col) cx -= kColumns[col].w;
            c.selection = {id};
            editCell(r, col);
            return 1;
        }
        if (Fl::event_state(FL_SHIFT)) { if (!c.selection.erase(id)) c.selection.insert(id); }
        else c.selection = {id};
        panel_->changed();
        return 1;
    }
    case FL_MOUSEWHEEL:
        scroll = std::max(0, scroll + Fl::event_dy() * 3);
        redraw();
        return 1;
    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1;
    case FL_KEYBOARD:
        if (c.key(Fl::event_key(), Fl::event_state())) { panel_->changed(); return 1; }
        return 0;
    }
    return Fl_Widget::handle(e);
}

// ================================================================ the panel
namespace {
constexpr int kDynH = 64, kSideW = 250, kTextH = 150, kTextStatusH = 18;

Fl_Box* textBox(int x, int y, int w, int h, const char* text, Fl_Color c = kDim, int size = 11, Fl_Font f = FL_HELVETICA) {
    auto* b = new Fl_Box(x, y, w, h);
    b->copy_label(text);
    b->labelcolor(c);
    b->labelsize(size);
    b->labelfont(f);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    return b;
}
Fl_Input* field(int x, int y, int w, int h, const char* value, const char* tip) {
    auto* in = new Fl_Input(x, y, w, h);
    in->value(value);
    in->textsize(11);
    in->color(fl_rgb_color(24, 25, 28));
    in->textcolor(kText);
    in->box(FL_FLAT_BOX);
    in->cursor_color(kText);
    in->tooltip(tip);
    return in;
}
ToolButton* pill(int x, int y, int w, int h, const char* text, const char* tip, Fl_Callback* cb, void* data, bool toggle = false) {
    auto* b = new ToolButton(x, y, w, h, ToolButton::None, text, toggle);
    b->copy_tooltip(tip);
    b->callback(cb, data);
    return b;
}
}  // namespace

RollPanel::RollPanel(int X, int Y, int W, int H, std::shared_ptr<RollEditTarget> target, std::shared_ptr<ScorePlayback> play)
    : Fl_Group(X, Y, W, H), core_(std::move(target)), play_(std::move(play)) {
    if (!play_)
        if (auto* dt = dynamic_cast<ScoreDocTarget*>(core_.target())) play_ = std::make_shared<ScorePlayback>(dt->document());
    box(FL_FLAT_BOX);
    color(kBgLocked);
    // ---- the toolbar
    bar_ = new Fl_Group(X, Y, W, kToolH);
    bar_->box(FL_FLAT_BOX);
    bar_->color(kStatusBg);
    int bx = X + 8;
    const int by = Y + 5, bh = kToolH - 10;
    using P = RollPanel;
    const char* tools[] = {"Select", "Draw", "Erase"};
    const int tw[] = {58, 50, 52};
    for (int k = 0; k < 3; ++k) {
        toolBtn_[k] = pill(bx, by, tw[k], bh, tools[k], k == 0 ? "Select, move (drag), resize (drag the end), copy (Alt-drag)" : k == 1 ? "Add notes on the grid" : "Take events out",
                           [](Fl_Widget* w, void* d) {
                               auto* p = static_cast<P*>(d);
                               for (int j = 0; j < 3; ++j) if (p->toolBtn_[j] == w) p->setTool((RollCore::Tool)j);
                           }, this, true);
        bx += tw[k] + 2;
    }
    bx += 12;
    snap_ = new Fl_Choice(bx + 38, by, 70, bh, "Snap");
    snap_->labelcolor(kDim);
    snap_->labelsize(11);
    snap_->textsize(11);
    snap_->add("off|1\\/4|1\\/8|1\\/16|1\\/32|1\\/8T|1\\/16T");   // (a / makes a submenu)
    snap_->value(3);
    snap_->tooltip("The grid moves and new notes snap to (a fraction of a whole note)");
    snap_->callback([](Fl_Widget* w, void* d) {
        static const double v[] = {0, 1.0 / 4, 1.0 / 8, 1.0 / 16, 1.0 / 32, 1.0 / 12, 1.0 / 24};
        static_cast<P*>(d)->setSnap(v[static_cast<Fl_Choice*>(w)->value()]);
    }, this);
    bx += 38 + 70 + 6;
    pill(bx, by, 76, bh, "Quantize", "The selection (or everything) onto the grid (Q)",
         [](Fl_Widget*, void* d) { auto* p = static_cast<P*>(d); p->core_.quantizeSelection(); p->changed(); }, this);
    bx += 76 + 12;
    auto* zout = new ToolButton(bx, by, 28, bh, ToolButton::Minus, "", false);
    zout->tooltip("Zoom out (Cmd+wheel)");
    zout->callback([](Fl_Widget*, void* d) { auto* p = static_cast<P*>(d); p->core_.zoom(0.8, p->core_.width() / 2); p->changed(); }, this);
    bx += 30;
    pill(bx, by, 40, bh, "Fit", "The whole score", [](Fl_Widget*, void* d) { auto* p = static_cast<P*>(d); p->core_.fit(); p->changed(); }, this);
    bx += 42;
    auto* zin = new ToolButton(bx, by, 28, bh, ToolButton::Plus, "", false);
    zin->tooltip("Zoom in (Cmd+wheel)");
    zin->callback([](Fl_Widget*, void* d) { auto* p = static_cast<P*>(d); p->core_.zoom(1.25, p->core_.width() / 2); p->changed(); }, this);
    bx += 30;
    auto* spacer = new Fl_Box(bx, Y, std::max(1, X + W - bx), kToolH);
    bar_->resizable(spacer);
    bar_->end();
    // ---- the view, the lane, the list
    int mainH = H - kToolH - kTextH;
    roll_ = new RollWidget(X, Y + kToolH, W - kSideW, mainH - kDynH, this);
    dyn_ = new DynamicsLane(X, Y + kToolH + mainH - kDynH, W - kSideW, kDynH, this);
    dynRow_ = new Fl_Choice(X + 4, Y + kToolH + mainH - kDynH + 18, 70, 20);
    dynRow_->textsize(11);
    dynRow_->labelsize(11);
    dynRow_->box(FL_FLAT_BOX);
    dynRow_->down_box(FL_FLAT_BOX);
    dynRow_->color(kBoxFill);
    dynRow_->textcolor(kText);
    dynRow_->clear_visible_focus();
    dynRow_->add("all");
    dynRow_->value(0);
    dynRow_->tooltip("The row whose dynamics the lane shows");
    dynRow_->callback([](Fl_Widget* w, void* d) {
        auto* p = static_cast<P*>(d);
        int v = static_cast<Fl_Choice*>(w)->value();
        p->dyn_->row = v <= 0 || v > (int)p->dynRows_.size() ? "" : p->dynRows_[(size_t)v - 1];
        p->dyn_->redraw();
    }, this);
    list_ = new EventList(X, Y + kToolH, W - kSideW, mainH, this);
    list_->hide();
    cellInput_ = new Fl_Input(X, Y, 80, 18);
    cellInput_->textsize(11);
    cellInput_->when(FL_WHEN_ENTER_KEY_ALWAYS);
    cellInput_->callback([](Fl_Widget*, void* d) { static_cast<P*>(d)->commitCell(); }, this);
    cellInput_->hide();
    // ---- the side panel: Generate (next) and Transform
    side_ = new Fl_Group(X + W - kSideW, Y + kToolH, kSideW, mainH);
    side_->box(FL_FLAT_BOX);
    side_->color(fl_rgb_color(30, 31, 36));
    buildTransform(X + W - kSideW, Y + kToolH, kSideW, mainH);
    side_->end();
    // ---- the score as Musil data
    int ty = Y + kToolH + mainH;
    text_ = new Fl_Text_Buffer();
    textEd_ = new Fl_Text_Editor(X, ty, W, kTextH - kTextStatusH);
    textEd_->buffer(text_);
    textEd_->box(FL_FLAT_BOX);
    textEd_->color(fl_rgb_color(24, 25, 28));
    textEd_->textcolor(fl_rgb_color(214, 216, 222));
    textEd_->textfont(kCodeFontSlot);
    textEd_->textsize(12);
    textEd_->cursor_color(kText);
    textEd_->selection_color(fl_rgb_color(52, 72, 110));
    textEd_->linenumber_width(36);
    textEd_->linenumber_bgcolor(fl_rgb_color(32, 33, 37));
    textEd_->linenumber_fgcolor(fl_rgb_color(110, 112, 120));
    textEd_->linenumber_size(10);
    textEd_->tooltip("The score as Musil data (what a .score file holds): edit it; it is applied when it reads");
    textStatus_ = textBox(X, ty + kTextH - kTextStatusH, W, kTextStatusH, "", kDim, 11);
    textStatus_->box(FL_FLAT_BOX);
    textStatus_->color(fl_rgb_color(30, 31, 35));
    end();
    resizable(roll_);
    text_->add_modify_callback(textChanged, this);
    core_.onChange = [this] { changed(); };
    core_.onStatus = [this](const std::string& s) { setStatus(s); };
    core_.onEnter = [this](uint64_t id) { if (id) core_.enter(id); };
    setView(View::Roll);
    setTool(RollCore::Tool::Select);
    core_.fit();
    changed();
    refreshText(true);
    setStatus("drag to move, drag the end to resize, Alt-drag copies; Alt+Up/Down: a cent; a double-click on a score placed enters it, Esc goes up");
    Fl::add_timeout(1.0 / 30, tick, this);
}

RollPanel::~RollPanel() {
    Fl::remove_timeout(tick, this);
    Fl::remove_timeout(applyLater, this);
    text_->remove_modify_callback(textChanged, this);
    textEd_->buffer(nullptr);
    delete text_;
}

void RollPanel::buildTransform(int X, int Y, int W, int) {
    int y = Y + 10;
    textBox(X + 10, y, W - 20, 18, "GENERATE", kSignal, 12, FL_HELVETICA_BOLD);
    y += 20;
    auto* gen = textBox(X + 10, y, W - 20, 34, "Musil's algorithms into the selection,\nrun offline: in the next version", kDim, 11);
    gen->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    y += 46;
    textBox(X + 10, y, W - 20, 18, "TRANSFORM THE SELECTION", kSignal, 12, FL_HELVETICA_BOLD);
    y += 20;
    textBox(X + 10, y, W - 20, 16, "(nothing selected: the whole score)", kDim, 10);
    y += 22;
    const int bh = 22, gap = 6;
    struct Btn {
        const char* label;
        const char* tip;
        int kind;
    };
    auto row = [&](std::initializer_list<Btn> bs, Fl_Input** in, const char* value, const char* tip) {
        int x = X + 10;
        for (auto& b : bs) {
            int w = (int)std::max(64.0, 7.0 * std::strlen(b.label) + 18);
            transformButtons_.push_back(std::make_unique<TransformButton>(TransformButton{this, b.kind}));
            pill(x, y, w, bh, b.label, b.tip, [](Fl_Widget*, void* d) {
                auto* t = static_cast<TransformButton*>(d);
                runTransform(t->panel, t->kind);
            }, transformButtons_.back().get());
            x += w + gap;
        }
        if (in) *in = field(x, y, X + W - 10 - x, bh, value, tip);
        y += bh + gap;
    };
    row({{"transpose", "The notes by semitones (the number at the right)", ScoreTransform::Transpose}}, &inTranspose_, "7", "semitones");
    row({{"invert", "Mirrored around the middle of their pitches", ScoreTransform::Invert},
         {"retrograde", "Backwards within their span", ScoreTransform::Retrograde}}, nullptr, "", "");
    row({{"augment", "Longer by the factor at the right", ScoreTransform::Augment}}, &inAugment_, "2", "factor (augment: times, diminish: divided by)");
    row({{"diminish", "Shorter by the factor of augment", (ScoreTransform::Kind)100}}, nullptr, "", "");
    row({{"quantize", "Onto the grid (Snap), by the strength at the right", ScoreTransform::Quantize}}, &inStrength_, "1", "strength: 1 onto the grid, 0.5 half way");
    row({{"humanize", "Onsets moved at random by up to the ms at the right, gains by 1.5 dB", ScoreTransform::Humanize}}, &inHumanize_, "20", "ms");
    row({{"thin", "Each event kept with the probability at the right", ScoreTransform::Thin}}, &inThin_, "0.5", "keep (0 to 1)");
    row({{"density", "Kept with a probability read along the span from the curve at the right", ScoreTransform::Density}}, &inDensity_, "1 0.3 1", "a curve: values 0 to 1");
}

void RollPanel::runTransform(RollPanel* p, int kind) {
    ScoreTransform t;
    t.seed = p->seed_++;
    if (kind == 100) {                                  // diminish: augment by the inverse
        t.kind = ScoreTransform::Augment;
        double k = p->param(p->inAugment_, 2);
        if (!(k > 0)) { p->setStatus("the factor must be > 0"); return; }
        t.a = 1 / k;
    } else {
        t.kind = (ScoreTransform::Kind)kind;
        switch (t.kind) {
        case ScoreTransform::Transpose: t.a = p->param(p->inTranspose_, 7); break;
        case ScoreTransform::Invert: t.a = NAN; break;
        case ScoreTransform::Augment: t.a = p->param(p->inAugment_, 2); break;
        case ScoreTransform::Quantize:
            t.a = p->core_.snapSeconds() > 0 ? p->core_.snapSeconds() : 0.25;
            t.b = p->param(p->inStrength_, 1);
            break;
        case ScoreTransform::Humanize: t.a = p->param(p->inHumanize_, 20) / 1000; t.b = 1.5; break;
        case ScoreTransform::Thin: t.a = p->param(p->inThin_, 0.5); break;
        case ScoreTransform::Density: {
            const char* s = p->inDensity_->value();
            char* end = nullptr;
            while (s && *s) {
                double v = std::strtod(s, &end);
                if (end == s) break;
                t.curve.push_back(std::clamp(v, 0.0, 1.0));
                s = end;
            }
            if (t.curve.empty()) t.curve = {1, 0.3, 1};
            break;
        }
        default: break;
        }
    }
    p->core_.transformSelection(t);
    p->changed();
}

double RollPanel::param(Fl_Input* in, double def) const {
    if (!in) return def;
    char* end = nullptr;
    double v = std::strtod(in->value(), &end);
    return end == in->value() || !std::isfinite(v) ? def : v;
}

void RollPanel::layoutChildren() {
    int X = x(), Y = y(), W = w(), H = h();
    int mainH = std::max(80, H - kToolH - kTextH);
    bar_->resize(X, Y, W, kToolH);
    int vy = Y + kToolH;
    roll_->resize(X, vy, W - kSideW, mainH - kDynH);
    dyn_->resize(X, vy + mainH - kDynH, W - kSideW, kDynH);
    dynRow_->resize(X + 4, vy + mainH - kDynH + 18, 70, 20);
    list_->resize(X, vy, W - kSideW, mainH);
    side_->resize(X + W - kSideW, vy, kSideW, mainH);
    textEd_->resize(X, vy + mainH, W, kTextH - kTextStatusH);
    textStatus_->resize(X, vy + mainH + kTextH - kTextStatusH, W, kTextStatusH);
}

void RollPanel::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    // the side panel keeps its children where they were relative to it
    int dx = (X + W - kSideW) - side_->x(), dy = (Y + kToolH) - side_->y();
    for (int k = 0; k < side_->children(); ++k) {
        Fl_Widget* c = side_->child(k);
        c->resize(c->x() + dx, c->y() + dy, c->w(), c->h());
    }
    side_->Fl_Widget::resize(side_->x() + dx, side_->y() + dy, side_->w(), side_->h());
    layoutChildren();
    changed();
}

void RollPanel::setView(View v) {
    view_ = v;
    bool list = v == View::List;
    if (list) { roll_->hide(); dyn_->hide(); dynRow_->hide(); list_->show(); }
    else { list_->hide(); roll_->show(); dyn_->show(); dynRow_->show(); }
    core_.mode = v == View::Piano ? RollCore::Mode::Piano : RollCore::Mode::Staves;
    changed();
}

void RollPanel::setTool(RollCore::Tool t) {
    core_.tool = t;
    for (int k = 0; k < 3; ++k) toolBtn_[k]->value(k == (int)t);
    setStatus(t == RollCore::Tool::Draw ? "Draw: press on a staff where the note goes, drag for its length"
              : t == RollCore::Tool::Erase ? "Erase: click (or drag over) events to take them out"
                                           : "Select: click, drag a rectangle, Shift adds; drag to move, the end to resize, Alt-drag copies");
}

void RollPanel::setSnap(double s) {
    core_.snap = s;
    static const double v[] = {0, 1.0 / 4, 1.0 / 8, 1.0 / 16, 1.0 / 32, 1.0 / 12, 1.0 / 24};
    for (int k = 0; k < 7; ++k) if (std::fabs(v[k] - s) < 1e-12) snap_->value(k);
}

void RollPanel::setStatus(const std::string& s) {
    status_ = s;
    if (onStatus) onStatus(s);
}

void RollPanel::play() {
    applyText();
    if (!play_) { setStatus("nothing can play this score now"); return; }
    play_->play();
    setStatus(play_->message);
}

void RollPanel::stop() {
    if (play_) play_->stop();
}

void RollPanel::togglePlay() {
    if (play_ && play_->playing()) stop();
    else play();
}

void RollPanel::undo() {
    applyText();
    core_.syncSelection();                        // the score the view is in: its history
    if (!target() || !target()->undo()) setStatus("nothing to undo");
    else setStatus("undo");
    changed();
}

void RollPanel::redo() {
    applyText();
    core_.syncSelection();
    if (!target() || !target()->redo()) setStatus("nothing to redo");
    else setStatus("redo");
    changed();
}

void RollPanel::changed() {
    core_.syncSelection();
    RollEditTarget* t = target();
    uint64_t g = t ? t->generation() : 0;
    bool scoreChanged = g != seenGen_;
    seenGen_ = g;
    if (scoreChanged) {
        // the selection keeps what is still there
        if (auto s = core_.shown())
            for (auto it = core_.selection.begin(); it != core_.selection.end();) it = s->find(*it) ? std::next(it) : core_.selection.erase(it);
        refreshText(false);
        // the rows of the dynamics lane
        std::vector<std::string> rows;
        if (auto s = core_.shown())
            for (auto& r : lay::rows(*s)) if (!r.byKind && r.pitched) rows.push_back(r.name);
        if (rows != dynRows_) {
            dynRows_ = rows;
            dynRow_->clear();
            dynRow_->add("all");
            for (auto& r : rows) dynRow_->add(escapeMenu(r).c_str());
            dynRow_->value(0);
            dyn_->row.clear();
        }
        if (onTitle) onTitle();
    }
    if (onChanged) onChanged();
    roll_->redraw();
    dyn_->redraw();
    dynRow_->redraw();
    list_->redraw();
}

std::string RollPanel::crumbInfo() const {
    std::string c;
    auto pl = core_.placements();
    if (!pl.empty()) {
        const auto& p = pl.back();
        sc::Event e;
        e.transpose = p.transpose; e.stretch = p.stretch; e.gain = p.gain;
        c += "placed at " + fmtNum(p.at, 3) + " s" + placementText(e) + "; Esc goes up     ";
    }
    if (docTarget() && docTarget()->document()->boxGone()) c += "its box is gone: the score is kept here     ";
    return c;
}

void RollPanel::tick(void* data) {
    auto* p = static_cast<RollPanel*>(data);
    Fl::repeat_timeout(1.0 / 30, tick, data);
    RollEditTarget* t = p->target();
    if (p->core_.syncSelection() || (t && t->generation() != p->seenGen_)) p->changed();
    double ph = NAN;
    if (p->play_) {
        p->play_->tick();
        ph = p->play_->position();
    } else if (Patch* pt = t ? t->patch() : nullptr) {
        auto tp = transportOf(pt);
        int lap;
        double pos;
        tp->where(pt, lap, pos);
        if (tp->playing() || pos > 0) ph = tp->seconds(pos);
    }
    if (!(std::isnan(ph) && std::isnan(p->core_.playhead)) && ph != p->core_.playhead) {
        p->core_.playhead = ph;
        p->roll_->redraw();
    }
}

// ---- the text view
std::string RollPanel::text() const {
    char* t = text_->text();
    std::string s = t ? t : "";
    free(t);
    return s;
}

void RollPanel::refreshText(bool force) {
    if (textPending_ && !force) return;
    auto s = target() ? target()->score() : nullptr;
    std::string t = s ? sc::toText(*s) : std::string();
    if (t == shownText_ && !force) return;
    shownText_ = t;
    if (t != text()) {
        settingText_ = true;
        int pos = textEd_->insert_position();
        text_->text(t.c_str());
        textEd_->insert_position(std::min(pos, text_->length()));
        settingText_ = false;
    }
    textPending_ = false;
    textError_.clear();
    textStatus_->labelcolor(kDim);
    textStatus_->copy_label("  the score as Musil data (a .score file holds the same): edit it, it is applied when it reads");
    textStatus_->redraw();
}

void RollPanel::textChanged(int, int nInserted, int nDeleted, int, const char*, void* data) {
    auto* p = static_cast<RollPanel*>(data);
    if (p->settingText_ || (nInserted == 0 && nDeleted == 0)) return;
    p->textPending_ = true;
    Fl::remove_timeout(applyLater, p);
    Fl::add_timeout(0.7, applyLater, p);
}

void RollPanel::applyLater(void* data) { static_cast<RollPanel*>(data)->applyText(); }

void RollPanel::setText(const std::string& t) {
    text_->text(t.c_str());          // as typed: the modify callback marks it pending
}

bool RollPanel::applyText() {
    Fl::remove_timeout(applyLater, this);
    if (!textPending_) return true;
    RollEditTarget* t = target();
    std::string src = text();
    std::string where = "roll";
    if (Patch* p = t ? t->patch() : nullptr) where = dirOf(p->resolvePath("x")) + "/roll";
    try {
        auto s = sc::readText(src, where);
        auto cur = t ? t->score() : nullptr;
        if (cur) { s->path = cur->path; if (!cur->name.empty() && s->name != cur->name) s->name = cur->name; }
        if (!cur || sc::toText(*cur) != sc::toText(*s)) t->replaceScore(s, "text");
        textPending_ = false;
        refreshText(true);
        changed();
        setStatus("the text applied");
        return true;
    } catch (const std::exception& e) {
        textError_ = e.what();
        textStatus_->labelcolor(fl_rgb_color(230, 120, 110));
        textStatus_->copy_label(("  " + textError_ + "  — not applied").c_str());
        textStatus_->redraw();
        return false;
    }
}

// ---- the list's cells
void RollPanel::editCell(uint64_t id, const std::string& key, const std::string& value, int x, int y, int w) {
    cellId_ = id;
    cellKey_ = key;
    cellInput_->resize(x, y, w, 18);
    cellInput_->value(value.c_str());
    cellInput_->show();
    cellInput_->take_focus();
    cellInput_->insert_position(0, cellInput_->size());
    redraw();
}

void RollPanel::commitCell() {
    if (!cellInput_->visible()) return;
    std::string v = cellInput_->value();
    cellInput_->hide();
    if (!target() || !cellId_) return;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    sc::Datum datum = end && *end == '\0' && !v.empty() ? sc::Datum::number(d) : sc::Datum::symbol(v);
    if (!target()->setField(core_.path, {cellId_}, cellKey_, datum)) setStatus(target()->lastError.empty() ? "nothing changed" : target()->lastError);
    list_->take_focus();
    changed();
}

int RollPanel::handle(int e) {
    if (e == FL_KEYBOARD && cellInput_->visible() && Fl::event_key() == FL_Escape) { cellInput_->hide(); list_->take_focus(); redraw(); return 1; }
    return Fl_Group::handle(e);
}

}  // namespace moritz
