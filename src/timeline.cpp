// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// timeline.cpp — the timeline's panel (timeline.h): the transport bar, the ruler, tracks and clips, automation
// lanes, the inspector, the mouse and the keys; every edit goes through timeline_ops.h, one undo step each.

#include "timeline.h"
#include "roll.h"
#include "transport.h"
#include "ui_util.h"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace moritz {

namespace tl = timeline;

namespace {

constexpr int kInspectorW = 252;
constexpr int kCrumbsH = 24;
constexpr int kScrollW = 14;
constexpr int kFields = 10;
constexpr int kButtons = 6;

const Fl_Color kLaneBg = fl_rgb_color(30, 31, 36);
const Fl_Color kLaneAlt = fl_rgb_color(26, 27, 31);
const Fl_Color kHeadBg = fl_rgb_color(38, 40, 46);
const Fl_Color kHeadSel = fl_rgb_color(48, 56, 74);
const Fl_Color kPlayhead = fl_rgb_color(235, 80, 80);
const Fl_Color kSound = fl_rgb_color(66, 186, 128);
const Fl_Color kNote = fl_rgb_color(232, 156, 36);
const Fl_Color kPatchC = fl_rgb_color(62, 128, 236);
const Fl_Color kScoreC = fl_rgb_color(150, 122, 232);
const Fl_Color kMsgC = fl_rgb_color(150, 156, 170);
const Fl_Color kTrackColours[] = {fl_rgb_color(66, 186, 128), fl_rgb_color(232, 156, 36), fl_rgb_color(62, 128, 236),
                                  fl_rgb_color(150, 122, 232), fl_rgb_color(48, 190, 190), fl_rgb_color(226, 98, 140)};

Fl_Color kindColour(sc::Kind k) {
    switch (k) {
    case sc::Kind::Sound: return kSound;
    case sc::Kind::Note: return kNote;
    case sc::Kind::Patch: return kPatchC;
    case sc::Kind::Score: return kScoreC;
    case sc::Kind::Message: return kMsgC;
    }
    return kMsgC;
}

std::string fmt(double v, const char* f = "%g") {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}
std::string dbText(double g) { return g <= 0 ? std::string("-inf") : fmt(20 * std::log10(g), "%.1f"); }
std::string azText(double az) {
    if (std::fabs(az) < 0.05) return "C";
    return (az > 0 ? "L" : "R") + fmt(std::fabs(az), "%.0f");
}
std::string clock(double s) {
    if (!(s >= 0)) s = 0;
    long ms = std::lround(s * 1000);
    char b[32];
    std::snprintf(b, sizeof b, "%02ld:%02ld.%03ld", ms / 60000, ms / 1000 % 60, ms % 1000);
    return b;
}
std::string baseOf(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool endsWith(const std::string& s, const char* e) {
    size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}
bool isSoundFile(const std::string& p) {
    std::string l = lower(p);
    for (const char* e : {".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3"})
        if (endsWith(l, e)) return true;
    return false;
}
// a line of text asked for (nothing in batch mode: the question is printed, the tests act directly)
std::string askLine(const char* prompt, const std::string& def) {
    if (batchMode()) {
        std::printf("ask: %s\n", prompt);
        std::fflush(stdout);
        return "";
    }
    const char* r = fl_input("%s", def.c_str(), prompt);
    return r ? r : "";
}
std::string chooseOpen(const char* title, const char* filter) {
    if (batchMode()) {
        std::printf("open: %s -> cancelled\n", title);
        std::fflush(stdout);
        return "";
    }
    Fl_Native_File_Chooser fc;
    fc.title(title);
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter(filter);
    if (fc.show() != 0) return "";
    return portablePath(fc.filename());
}
std::string clipLabel(const sc::Event& e) {
    if (!e.label.empty()) return e.label;
    switch (e.kind) {
    case sc::Kind::Sound: {
        std::string s = e.buffer ? "buffer~ " + e.source : baseOf(e.source);
        if (e.from > 0 || e.to >= 0) s += " " + fmt(e.from, "%.2f") + "–" + fmt(e.to >= 0 ? e.to : e.from + e.dur, "%.2f");
        return s;
    }
    case sc::Kind::Score: {
        std::string s = e.sub && !e.sub->name.empty() ? e.sub->name : baseOf(e.ref);
        if (e.transpose != 0) s += (e.transpose > 0 ? " +" : " ") + fmt(e.transpose);
        if (e.stretch != 1) s += " ×" + fmt(e.stretch);
        return s;
    }
    case sc::Kind::Patch: return "patch " + baseOf(e.source);
    case sc::Kind::Note: return fmt(e.sounding());
    case sc::Kind::Message: return e.source;
    }
    return "";
}

}  // namespace

// ---------------------------------------------------------------- the waveforms

WaveCache::~WaveCache() {
    if (job_) job_->cancel();
}

const WaveCache::Entry* WaveCache::get(const std::string& path, bool buffer, Patch* root) {
    std::string key = (buffer ? "buffer:" : "file:") + path;
    auto it = entries_.find(key);
    if (buffer) {
        BufferBox* b = root ? findBuffer(root, path) : nullptr;
        if (!b) return nullptr;
        if (it == entries_.end() || it->second->version != b->version) {
            auto e = std::make_shared<Entry>();
            auto s = std::make_shared<SampleBuffer>(b->copy());
            e->summary.build(*s);
            e->sound = s;
            e->version = b->version;
            entries_[key] = e;
            return e.get();
        }
        return it->second.get();
    }
    if (it != entries_.end()) return it->second.get();
    if (std::find(queue_.begin(), queue_.end(), path) == queue_.end()) {
        queue_.push_back(path);
        next();
    }
    return nullptr;
}

void WaveCache::next() {
    if ((job_ && job_->running()) || queue_.empty()) return;
    std::string path = queue_.front();
    auto e = std::make_shared<Entry>();
    if (!job_) job_ = std::make_unique<Job>();
    job_->start(
        [path, e](Job&) {
            auto s = std::make_shared<SampleBuffer>();
            std::string err;
            if (!decodeAudioFile(path, *s, err)) { e->failed = true; return; }
            e->summary.build(*s);
            e->sound = s;
        },
        [this, path, e] {
            entries_["file:" + path] = e;
            queue_.erase(std::remove(queue_.begin(), queue_.end(), path), queue_.end());
            job_->cancel();                       // only joins its thread: it is done
            if (onReady) onReady();
            next();
        });
}

// ---------------------------------------------------------------- the view: rows

void TimelineView::layoutRows() {
    const sc::Score& s = tp_->doc().score();
    rows_.clear();
    int y = 0;
    for (auto& t : tl::tracks(s)) {
        rows_.push_back(Row{t, -1, "", 0, 1, y, kTrackH});
        y += kTrackH;
        int k = 0;
        for (auto& l : tl::lanes(s, t)) {
            rows_.push_back(Row{t, k++, l.name, l.lo, l.hi, y, kLaneH});
            y += kLaneH;
        }
    }
    contentH_ = y;
}

const TimelineView::Row* TimelineView::rowAt(int ey) const {
    int cy = ey - (y() + kRulerH) + tp_->scrollY();
    if (ey < y() + kRulerH) return nullptr;
    for (auto& r : rows_)
        if (cy >= r.y && cy < r.y + r.h) return &r;
    return nullptr;
}

bool TimelineView::clipRect(const sc::Event& e, int& X, int& Y, int& W, int& H) const {
    for (auto& r : rows_) {
        if (r.lane >= 0 || r.track != e.row) continue;
        X = tp_->xOf(e.at);
        W = std::max(4, tp_->xOf(e.end()) - X);
        Y = y() + kRulerH + r.y - tp_->scrollY() + 3;
        H = r.h - 6;
        if (e.kind == sc::Kind::Note) {              // a note: a bar at its pitch
            double p = std::clamp((e.sounding() - 36) / 60.0, 0.0, 1.0);
            Y = Y + 14 + (int)std::lround((1 - p) * (H - 20));
            H = 5;
        }
        return true;
    }
    return false;
}

uint64_t TimelineView::clipAt(int ex, int ey, int* zone) const {
    const auto& evs = tp_->doc().score().events();
    for (auto it = evs.rbegin(); it != evs.rend(); ++it) {
        const sc::Event& e = **it;
        if (!tl::isClip(e)) continue;
        int X, Y, W, H;
        if (!clipRect(e, X, Y, W, H)) continue;
        int pad = e.kind == sc::Kind::Note ? 3 : 0;
        if (ex >= X && ex < X + W && ey >= Y - pad && ey < Y + H + pad) {
            if (zone) {
                int edge = std::min(7, W / 4);
                *zone = e.kind == sc::Kind::Note ? 0 : ex < X + edge ? -1 : ex >= X + W - edge ? 1 : 0;
            }
            return e.id;
        }
    }
    return 0;
}

uint64_t TimelineView::pointAt(int ex, int ey) const {
    const Row* r = rowAt(ey);
    if (!r || r->lane < 0) return 0;
    int Y = y() + kRulerH + r->y - tp_->scrollY();
    for (auto* p : tl::points(tp_->doc().score(), r->track, r->laneName)) {
        int px = tp_->xOf(p->at);
        double v = (tl::pointValue(*p) - r->lo) / (r->hi - r->lo);
        int py = Y + 4 + (int)std::lround((1 - v) * (r->h - 8));
        if (std::abs(ex - px) <= 5 && std::abs(ey - py) <= 6) return p->id;
    }
    return 0;
}

int TimelineView::headerButton(const Row& r, int ex, int ey) const {
    if (r.lane >= 0) return 0;
    int Y = y() + kRulerH + r.y - tp_->scrollY();
    int bx = x() + kHeadW - 50, by = Y + 8;
    if (ey >= by && ey < by + 16) {
        if (ex >= bx && ex < bx + 20) return 1;
        if (ex >= bx + 22 && ex < bx + 42) return 2;
    }
    return 0;
}

// ---------------------------------------------------------------- the view: drawing

void TimelineView::drawRuler() {
    const sc::Score& s = tp_->doc().score();
    int Y = y();
    fl_color(kStatusBg);
    fl_rectf(x(), Y, w(), kRulerH);
    fl_font(FL_HELVETICA_BOLD, 10);
    fl_color(kDim);
    fl_draw("TRACKS", x() + 10, Y + kRulerH - 9);
    int X = laneX(), W = laneW();
    fl_push_clip(X, Y, W, kRulerH);
    double beat = tl::beatSeconds(s), bar = beat * std::max(1, s.meterNum);
    double pps = tp_->pixelsPerSecond();
    int every = 1;                                   // bars between numbers
    while (bar * every * pps < 46) every *= 2;
    double t0 = tp_->start(), t1 = t0 + W / pps;
    // the loop
    if (tp_->loopTo() > tp_->loopFrom()) {
        int a = tp_->xOf(tp_->loopFrom()), b = tp_->xOf(tp_->loopTo());
        fl_color(tp_->loop() ? fl_rgb_color(50, 90, 170) : fl_rgb_color(48, 52, 64));
        fl_rectf(a, Y + kRulerH - 6, std::max(2, b - a), 5);
    }
    fl_font(FL_HELVETICA, 10);
    long firstBar = (long)std::floor(t0 / bar);
    for (long k = std::max(0L, firstBar); k * bar <= t1; ++k) {
        int px = tp_->xOf(k * bar);
        bool numbered = k % every == 0;
        fl_color(kBorder);
        fl_yxline(px, Y + (numbered ? 4 : 12), Y + kRulerH - 1);
        if (numbered) {
            fl_color(kDim);
            fl_draw(std::to_string(k + 1).c_str(), px + 3, Y + 13);
        }
        if (beat * pps >= 8)
            for (int b = 1; b < s.meterNum; ++b) {
                int q = tp_->xOf(k * bar + b * beat);
                fl_color(kGrid);
                fl_yxline(q, Y + kRulerH - 6, Y + kRulerH - 1);
            }
    }
    // the playhead's mark
    int ph = tp_->xOf(tp_->playhead());
    fl_color(kPlayhead);
    fl_polygon(ph - 5, Y + 2, ph + 5, Y + 2, ph, Y + 10);
    fl_pop_clip();
    fl_color(kGrid);
    fl_xyline(x(), Y + kRulerH - 1, x() + w());
}

void TimelineView::drawHeader(const Row& r, int Y) {
    ScoreDoc& d = tp_->doc();
    bool cur = r.track == tp_->currentTrack();
    fl_color(cur && r.lane < 0 ? kHeadSel : r.lane < 0 ? kHeadBg : fl_rgb_color(33, 34, 40));
    fl_rectf(x(), Y, kHeadW, r.h - 1);
    int ti = tl::trackIndex(d.score(), r.track);
    fl_color(kTrackColours[(ti < 0 ? 0 : ti) % 6]);
    fl_rectf(x(), Y, 4, r.h - 1);
    if (r.lane >= 0) {
        fl_font(FL_HELVETICA, 11);
        fl_color(kText);
        fl_draw(r.laneName.c_str(), x() + 16, Y + 15);
        fl_font(FL_HELVETICA, 9);
        fl_color(kDim);
        std::string sub = "receive " + r.laneName + " · " + fmt(r.lo) + "…" + fmt(r.hi);
        fl_draw(sub.c_str(), x() + 16, Y + 28);
        return;
    }
    sc::Strip st = tl::stripOf(d.score(), r.track);
    fl_font(FL_HELVETICA_BOLD, 12);
    fl_color(kText);
    std::string name = r.track.empty() ? "(no track)" : r.track;
    fl_push_clip(x() + 8, Y, kHeadW - 64, r.h);
    fl_draw(name.c_str(), x() + 12, Y + 20);
    fl_pop_clip();
    fl_font(FL_HELVETICA, 10);
    fl_color(kDim);
    std::string sub = "gain " + dbText(st.gain) + " dB · pan " + azText(st.az);
    fl_draw(sub.c_str(), x() + 12, Y + 38);
    int n = 0;
    for (auto& e : d.score().events()) n += e->row == r.track && tl::isClip(*e);
    fl_draw((std::to_string(n) + (n == 1 ? " clip" : " clips")).c_str(), x() + 12, Y + 53);
    // M and S
    int bx = x() + kHeadW - 50, by = Y + 8;
    auto button = [&](int bxx, const char* t, bool on, Fl_Color onC) {
        fl_color(on ? onC : fl_rgb_color(60, 63, 72));
        fl_rectf(bxx, by, 20, 16);
        fl_color(on ? FL_BLACK : kText);
        fl_font(FL_HELVETICA_BOLD, 10);
        fl_draw(t, bxx, by, 20, 16, FL_ALIGN_CENTER);
    };
    button(bx, "M", st.mute, fl_rgb_color(230, 170, 60));
    button(bx + 22, "S", st.solo, fl_rgb_color(110, 200, 120));
}

void TimelineView::drawClip(const sc::Event& e, int X, int Y, int W, int H, bool sel) {
    Fl_Color c = kindColour(e.kind);
    if (e.kind == sc::Kind::Note) {
        fl_color(sel ? kText : c);
        fl_rectf(X, Y, std::max(2, W), H);
        return;
    }
    int head = 15;
    fl_color(fl_color_average(c, kLaneBg, 0.35f));
    fl_rectf(X, Y, W, H);
    fl_color(c);
    fl_rectf(X, Y, W, head);
    fl_color(sel ? kText : fl_color_average(c, FL_BLACK, 0.6f));
    fl_rect(X, Y, W, H);
    if (sel) fl_rect(X + 1, Y + 1, W - 2, H - 2);
    fl_push_clip(X + 1, Y, W - 2, H);
    fl_font(FL_HELVETICA_BOLD, 10);
    fl_color(FL_BLACK);
    std::string t = clipLabel(e);
    if (e.kind == sc::Kind::Score) {
        int which = 0, n = tl::instances(tp_->doc().score(), e.sub.get(), &which, e.id);
        if (n > 1) t = "[" + std::to_string(which) + "/" + std::to_string(n) + "] " + t;
    }
    if (e.gain != 1) t += "  " + dbText(e.gain) + " dB";
    fl_draw(t.c_str(), X + 4, Y + 11);
    int by = Y + head, bh = H - head;
    switch (e.kind) {
    case sc::Kind::Sound: {
        Patch* root = tp_->doc().box() ? tp_->doc().box()->patch->root() : &App::get().host;
        const WaveCache::Entry* wave = tp_->waves().get(e.buffer ? e.source : tl::sourcePath(tp_->doc(), e.source), e.buffer, root);
        int mid = by + bh / 2, amp = std::max(1, bh / 2 - 2);
        fl_color(fl_color_average(c, FL_WHITE, 0.75f));
        if (!wave) {
            fl_font(FL_HELVETICA, 9);
            fl_draw(e.buffer ? "no buffer~ of that name" : "reading…", X + 4, by + 12);
        } else if (wave->failed || !wave->sound) {
            fl_font(FL_HELVETICA, 9);
            fl_draw("can't read the file", X + 4, by + 12);
        } else {
            const SampleBuffer& s = *wave->sound;
            double pps = tp_->pixelsPerSecond();
            int x0 = std::max(X + 1, x() + kHeadW), x1 = std::min(X + W - 1, x() + w());
            for (int px = x0; px < x1; ++px) {
                double src = e.from + (px - X) / pps;
                long a = (long)std::floor(src * s.sr), b = (long)std::floor((src + 1 / pps) * s.sr) + 1;
                if (e.to >= 0 && src >= e.to) break;
                if (a >= s.frames) break;
                float lo = 0, hi = 0;
                for (int ch = 0; ch < s.channels; ++ch) {
                    float l, h;
                    wave->summary.range(s, ch, a, b, l, h);
                    lo = std::min(lo, l);
                    hi = std::max(hi, h);
                }
                float g = (float)e.gain;
                int y0 = mid - (int)std::lround(std::clamp(hi * g, -1.f, 1.f) * amp);
                int y1 = mid - (int)std::lround(std::clamp(lo * g, -1.f, 1.f) * amp);
                fl_yxline(px, y0, std::max(y0, y1));
            }
        }
        // the fades
        double fi = sc::numField(e.extra, "fade-in", 0), fo = sc::numField(e.extra, "fade-out", 0);
        fl_color(fl_rgb_color(20, 20, 24));
        if (fi > 0) fl_line(X, by + bh, X + (int)(fi * tp_->pixelsPerSecond()), by);
        if (fo > 0) fl_line(X + W - (int)(fo * tp_->pixelsPerSecond()), by, X + W, by + bh);
        break;
    }
    case sc::Kind::Score: {
        if (!e.sub) break;
        std::vector<sc::Placed> flat;
        try { sc::flatten(*e.sub, flat); } catch (const std::exception&) {}
        double lo = 1e9, hi = -1e9;
        for (auto& p : flat)
            if (p.ev.kind == sc::Kind::Note) { lo = std::min(lo, p.ev.sounding()); hi = std::max(hi, p.ev.sounding()); }
        if (hi < lo) {
            fl_font(FL_HELVETICA, 9);
            fl_color(fl_color_average(c, FL_WHITE, 0.6f));
            fl_draw((std::to_string(e.sub->size()) + " events").c_str(), X + 4, by + 12);
            break;
        }
        if (hi - lo < 12) { lo -= 6; hi += 6; }
        fl_color(fl_color_average(c, FL_WHITE, 0.55f));
        int n = 0;
        for (auto& p : flat) {
            if (p.ev.kind != sc::Kind::Note || ++n > 4000) continue;
            double rel = p.ev.at * e.stretch;
            if (rel >= e.dur) continue;
            int nx = X + (int)(rel * tp_->pixelsPerSecond());
            int nw = std::max(2, (int)(std::min(p.ev.dur * e.stretch, e.dur - rel) * tp_->pixelsPerSecond()));
            int ny = by + 2 + (int)std::lround((hi - p.ev.sounding()) / (hi - lo) * (bh - 6));
            fl_rectf(nx, ny, nw, 2);
        }
        break;
    }
    case sc::Kind::Patch:
        fl_font(FL_HELVETICA, 9);
        fl_color(fl_color_average(c, FL_WHITE, 0.4f));
        fl_draw("runs while the clip plays (not yet)", X + 4, by + 12);
        break;
    default: break;
    }
    fl_pop_clip();
}

void TimelineView::drawTrack(const Row& r, int Y) {
    ScoreDoc& d = tp_->doc();
    fl_color(tl::trackIndex(d.score(), r.track) % 2 ? kLaneAlt : kLaneBg);
    fl_rectf(laneX(), Y, laneW(), r.h - 1);
    fl_push_clip(laneX(), Y, laneW(), r.h - 1);
    double vis0 = tp_->start(), vis1 = vis0 + laneW() / tp_->pixelsPerSecond();
    for (auto& ep : d.score().events()) {
        const sc::Event& e = *ep;
        if (!tl::isClip(e) || e.row != r.track) continue;
        bool sel = d.isSelected(e.id);
        double at = e.at, end = e.end();
        int dy = 0;
        if (sel && tp_->view()->movingSelection()) {             // drawn where the drag takes it
            at += dragDt;
            end += dragDt;
            if (dragDTrack) continue;                            // drawn on the other track
        }
        if (trimming(e.id)) {
            if (trimmingLeft()) at = std::min(dragEdge, end - 0.001);
            else end = std::max(dragEdge, at + 0.001);
        }
        if (end < vis0 || at > vis1) continue;
        sc::Event shown = e;
        if (trimming(e.id) && trimmingLeft() && e.kind == sc::Kind::Sound) shown.from = std::max(0.0, e.from + (at - e.at));
        shown.at = at;
        shown.dur = end - at;
        int X, YY, W, H;
        if (!clipRect(shown, X, YY, W, H)) continue;
        drawClip(shown, X, YY + dy, W, H, sel);
    }
    // clips moved here from another track by the drag going on
    if (tp_->view()->movingSelection() && dragDTrack) {
        auto all = tl::tracks(d.score());
        for (uint64_t id : d.selection()) {
            auto e = d.score().find(id);
            if (!e || !tl::isClip(*e)) continue;
            int k = tl::trackIndex(d.score(), e->row);
            if (k < 0 || all[(size_t)std::clamp(k + dragDTrack, 0, (int)all.size() - 1)] != r.track) continue;
            sc::Event shown = *e;
            shown.at += dragDt;
            shown.row = r.track;
            int X, YY, W, H;
            if (clipRect(shown, X, YY, W, H)) drawClip(shown, X, YY, W, H, true);
        }
    }
    fl_pop_clip();
}

void TimelineView::drawLane(const Row& r, int Y) {
    ScoreDoc& d = tp_->doc();
    fl_color(fl_rgb_color(24, 25, 30));
    fl_rectf(laneX(), Y, laneW(), r.h - 1);
    fl_push_clip(laneX(), Y, laneW(), r.h - 1);
    auto pts = tl::points(d.score(), r.track, r.laneName);
    auto yOf = [&](double v) { return Y + 4 + (int)std::lround((1 - (v - r.lo) / (r.hi - r.lo)) * (r.h - 8)); };
    struct P { double t, v; uint64_t id; bool num; };
    std::vector<P> ps;
    for (auto* p : pts) {
        P q{p->at, tl::pointValue(*p), p->id, !p->values.empty() && p->values[0].kind == sc::Datum::Num};
        if (movingPoint(p->id)) { q.t = dragPointT; q.v = dragPointV; }
        ps.push_back(q);
    }
    std::stable_sort(ps.begin(), ps.end(), [](const P& a, const P& b) { return a.t < b.t; });
    fl_color(fl_rgb_color(100, 160, 240));
    // the values hold from one point to the next (each point is the message sent there)
    for (size_t k = 0; k < ps.size(); ++k) {
        int x0 = tp_->xOf(ps[k].t), y0 = yOf(ps[k].v);
        int x1 = k + 1 < ps.size() ? tp_->xOf(ps[k + 1].t) : laneX() + laneW();
        fl_xyline(x0, y0, x1);
        if (k + 1 < ps.size()) fl_yxline(x1, y0, yOf(ps[k + 1].v));
    }
    for (auto& p : ps) {
        int px = tp_->xOf(p.t), py = yOf(p.v);
        bool sel = d.isSelected(p.id);
        fl_color(sel ? kText : fl_rgb_color(120, 180, 255));
        fl_rectf(px - 3, py - 3, 7, 7);
        if (!p.num) {                                    // a word: its text
            auto e = d.score().find(p.id);
            if (e && !e->values.empty()) {
                fl_font(FL_HELVETICA, 9);
                fl_draw(e->values[0].s.c_str(), px + 5, py + 3);
            }
        }
    }
    fl_pop_clip();
}

void TimelineView::draw() {
    fl_push_clip(x(), y(), w(), h());
    fl_color(kBgEdit);
    fl_rectf(x(), y(), w(), h());
    int top = y() + kRulerH;
    fl_push_clip(x(), top, w(), h() - kRulerH);
    for (auto& r : rows_) {
        int Y = top + r.y - tp_->scrollY();
        if (Y + r.h < top || Y > y() + h()) continue;
        drawHeader(r, Y);
        if (r.lane < 0) drawTrack(r, Y);
        else drawLane(r, Y);
        fl_color(kGrid);
        fl_xyline(x(), Y + r.h - 1, x() + w());
    }
    if (rows_.empty()) {
        fl_color(kDim);
        fl_font(FL_HELVETICA, 12);
        fl_draw("an empty score: Add > Track, or drop sound files here", laneX(), top, laneW(), 80, FL_ALIGN_CENTER);
    }
    // the bars, faint, over the lanes; the playhead
    fl_push_clip(laneX(), top, laneW(), h() - kRulerH);
    {
        const sc::Score& s = tp_->doc().score();
        double bar = tl::beatSeconds(s) * std::max(1, s.meterNum);
        double t1 = tp_->start() + laneW() / tp_->pixelsPerSecond();
        if (bar * tp_->pixelsPerSecond() >= 6)
            for (long k = std::max(0L, (long)std::floor(tp_->start() / bar)); k * bar <= t1; ++k) {
                fl_color(fl_rgb_color(44, 46, 54));
                fl_yxline(tp_->xOf(k * bar), top, top + std::min(h() - kRulerH, contentH_ - tp_->scrollY()));
            }
        fl_color(kPlayhead);
        fl_yxline(tp_->xOf(tp_->playhead()), top, y() + h());
    }
    fl_pop_clip();
    fl_pop_clip();
    drawRuler();
    fl_color(kGrid);
    fl_yxline(laneX() - 1, y(), y() + h());
    fl_pop_clip();
}

// ---------------------------------------------------------------- the view: the mouse

void TimelineView::press(int ex, int ey, int clicks, bool shift, bool right) {
    ScoreDoc& d = tp_->doc();
    pressX_ = ex;
    pressY_ = ey;
    moved_ = false;
    drag_ = Drag::None;
    dragDt = 0;
    dragDTrack = 0;
    if (ey < y() + kRulerH) {                            // the ruler: the playhead there; dragged: the loop
        if (ex < laneX()) return;
        tp_->setPlayhead(std::max(0.0, tp_->snapped(tp_->timeAt(ex))));
        drag_ = Drag::Ruler;
        return;
    }
    const Row* r = rowAt(ey);
    if (!r) {
        if (!shift) d.clearSelection();
        return;
    }
    std::string track = r->track;
    if (ex < laneX()) {                                  // a header
        tp_->setCurrentTrack(track);
        d.clearSelection();
        int b = headerButton(*r, ex, ey);
        if (b) {
            sc::Strip st = tl::stripOf(d.score(), track);
            if (b == 1) st.mute = !st.mute;
            else st.solo = !st.solo;
            tp_->after(tl::setStrip(d, track, st));
            return;
        }
        if (right) {
            if (r->lane >= 0) {
                Fl_Menu_Item items[] = {{"Remove Lane", 0, nullptr, nullptr, 0, 0, 0, 0, 0}, {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
                std::string lane = r->laneName;
                if (const Fl_Menu_Item* m = items->popup(ex, ey); m == &items[0]) tp_->after(tl::removeLane(d, track, lane));
                return;
            }
            Fl_Menu_Item items[] = {{"Add Automation Lane...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                                    {"Rename...", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
                                    {"Move Up", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                                    {"Move Down", 0, nullptr, nullptr, FL_MENU_DIVIDER, 0, 0, 0, 0},
                                    {"Delete Track", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                                    {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
            const Fl_Menu_Item* m = items->popup(ex, ey);
            if (!m) return;
            long k = m - items;
            if (k == 0) tp_->addLaneDialog();
            else if (k == 1) tp_->renameTrackDialog(track);
            else if (k == 2) tp_->after(tl::moveTrack(d, track, -1));
            else if (k == 3) tp_->after(tl::moveTrack(d, track, 1));
            else if (k == 4) tp_->after(tl::deleteTrack(d, track));
            return;
        }
        if (clicks && r->lane < 0) tp_->renameTrackDialog(track);
        return;
    }
    int rowIndex = (int)(r - rows_.data());             // (an edit lays the rows out again: r is not kept)
    tp_->setCurrentTrack(track);
    if (r->lane >= 0) {                                  // a lane: its points
        uint64_t p = pointAt(ex, ey);
        if (right) {
            if (p) tp_->after(tl::removeClips(d, {p}, "Delete Point"));
            return;
        }
        int Y = y() + kRulerH + r->y - tp_->scrollY();
        double v = r->lo + (1 - std::clamp((double)(ey - Y - 4) / std::max(1, r->h - 8), 0.0, 1.0)) * (r->hi - r->lo);
        std::string lane = r->laneName;
        if (!p) {
            p = tl::addPoint(d, track, lane, std::max(0.0, tp_->snapped(tp_->timeAt(ex))), v);
            tp_->after(p != 0);
            if (!p) return;
        }
        d.select({p});
        auto e = d.score().find(p);
        drag_ = Drag::Point;
        dragId_ = p;
        dragTrack_ = track;
        dragRow_ = rowIndex;
        dragPointT = e->at;
        dragPointV = tl::pointValue(*e);
        dragAt_ = e->at;
        dragV_ = dragPointV;
        return;
    }
    int zone = 0;
    uint64_t c = clipAt(ex, ey, &zone);
    if (right) {
        if (c && !d.isSelected(c)) d.select({c});
        const Fl_Menu_Item* m = tp_->editMenu_->menu();
        if (const Fl_Menu_Item* it = m ? m->popup(ex, ey) : nullptr) tp_->editMenu_->picked(it);
        return;
    }
    if (!c) {
        if (!shift) d.clearSelection();
        tp_->setPlayhead(std::max(0.0, tp_->snapped(tp_->timeAt(ex))));
        return;
    }
    if (clicks) {
        tp_->openClip(c);
        return;
    }
    if (shift) {
        d.toggle(c);
        return;
    }
    if (!d.isSelected(c)) d.select({c});
    auto e = d.score().find(c);
    dragId_ = c;
    dragAt_ = e->at;
    dragEnd_ = e->end();
    dragRow_ = tl::trackIndex(d.score(), e->row);
    dragEdge = zone < 0 ? dragAt_ : dragEnd_;
    drag_ = zone < 0 ? Drag::TrimL : zone > 0 ? Drag::TrimR : Drag::Move;
}

void TimelineView::dragTo(int ex, int ey) {
    if (drag_ == Drag::None) return;
    if (!moved_ && std::abs(ex - pressX_) < 3 && std::abs(ey - pressY_) < 3) return;
    moved_ = true;
    ScoreDoc& d = tp_->doc();
    double pps = tp_->pixelsPerSecond();
    double t = tp_->timeAt(ex);
    switch (drag_) {
    case Drag::Move: {
        double at = tp_->snapped(dragAt_ + (ex - pressX_) / pps);
        double first = 1e300;
        for (uint64_t id : d.selection())
            if (auto e = d.score().find(id)) first = std::min(first, e->at);
        dragDt = std::max(at - dragAt_, first == 1e300 ? 0.0 : -first);
        if (const Row* r = rowAt(ey)) dragDTrack = tl::trackIndex(d.score(), r->track) - dragRow_;
        break;
    }
    case Drag::TrimL:
    case Drag::TrimR: dragEdge = std::max(0.0, tp_->snapped(t)); break;
    case Drag::Point: {
        if (dragRow_ < 0 || dragRow_ >= (int)rows_.size()) break;
        const Row& r = rows_[(size_t)dragRow_];
        int Y = y() + kRulerH + r.y - tp_->scrollY();
        dragPointT = std::max(0.0, tp_->snapped(t));
        dragPointV = r.lo + (1 - std::clamp((double)(ey - Y - 4) / std::max(1, r.h - 8), 0.0, 1.0)) * (r.hi - r.lo);
        break;
    }
    case Drag::Ruler:
        drag_ = Drag::Loop;
        [[fallthrough]];
    case Drag::Loop: {
        double a = std::max(0.0, tp_->snapped(tp_->timeAt(pressX_))), b = std::max(0.0, tp_->snapped(t));
        tp_->setLoopRange(std::min(a, b), std::max(a, b));
        break;
    }
    default: break;
    }
    redraw();
}

void TimelineView::release(int ex, int ey) {
    if (drag_ == Drag::None) return;
    if (ex != pressX_ || ey != pressY_) dragTo(ex, ey);
    ScoreDoc& d = tp_->doc();
    Drag was = drag_;
    drag_ = Drag::None;
    if (!moved_) {
        redraw();
        return;
    }
    moved_ = false;
    switch (was) {
    case Drag::Move: {
        auto ids = d.selection();
        std::vector<uint64_t> clips;
        for (uint64_t id : ids)
            if (auto e = d.score().find(id); e && tl::isClip(*e)) clips.push_back(id);
        if (dragDt != 0 || dragDTrack != 0) tp_->after(tl::moveClips(d, clips, dragDt, dragDTrack));
        break;
    }
    case Drag::TrimL: tp_->after(tl::trimStart(d, dragId_, dragEdge)); break;
    case Drag::TrimR: tp_->after(tl::trimEnd(d, dragId_, dragEdge)); break;
    case Drag::Point:
        if (dragPointT != dragAt_ || dragPointV != dragV_) tp_->after(tl::movePoint(d, dragId_, dragPointT, dragPointV));
        break;
    case Drag::Loop: tp_->setLoop(true); break;
    default: break;
    }
    dragDt = 0;
    dragDTrack = 0;
    redraw();
}

void TimelineView::dropFiles(const std::string& text, int ex, int ey) {
    std::vector<std::string> files;
    size_t k = 0;
    while (k < text.size()) {
        size_t e = text.find_first_of("\r\n", k);
        std::string f = text.substr(k, e == std::string::npos ? std::string::npos : e - k);
        k = e == std::string::npos ? text.size() : e + 1;
        if (f.rfind("file://", 0) == 0) {                // a URL (Linux): decoded
            f = f.substr(7);
            std::string out;
            for (size_t i = 0; i < f.size(); ++i) {
                if (f[i] == '%' && i + 2 < f.size()) {
                    out += (char)std::strtol(f.substr(i + 1, 2).c_str(), nullptr, 16);
                    i += 2;
                } else out += f[i];
            }
            f = out;
        }
        if (!f.empty()) files.push_back(portablePath(f));
    }
    const Row* r = rowAt(ey);
    std::string track = r ? r->track : tp_->currentTrack();
    double at = std::max(0.0, tp_->snapped(tp_->timeAt(std::max(ex, laneX()))));
    ScoreDoc& d = tp_->doc();
    for (auto& f : files) {
        if (isSoundFile(f)) {
            if (tp_->addSoundFile(f, track, at))
                if (auto e = d.score().find(d.selection().empty() ? 0 : d.selection().back())) at = e->end();
        } else if (endsWith(lower(f), ".score")) {
            uint64_t id = tl::addSubScore(d, f, track, at);
            tp_->after(id != 0);
            if (auto e = d.score().find(id)) at = e->end();
        } else if (endsWith(lower(f), ".moritz")) {
            uint64_t id = tl::addPatchClip(d, f, track, at, 0);
            tp_->after(id != 0);
            if (auto e = d.score().find(id)) at = e->end();
        } else {
            tp_->setStatus("not a sound, a score or a patch: " + baseOf(f));
        }
    }
}

int TimelineView::handle(int e) {
    switch (e) {
    case FL_FOCUS:
    case FL_UNFOCUS:
        return 1;
    case FL_ENTER:
    case FL_LEAVE:
        return 1;
    case FL_MOVE: {
        int zone = 0;
        bool edge = Fl::event_y() >= y() + kRulerH && Fl::event_x() >= laneX() && clipAt(Fl::event_x(), Fl::event_y(), &zone) && zone != 0;
        window()->cursor(edge ? FL_CURSOR_WE : FL_CURSOR_DEFAULT);
        return 1;
    }
    case FL_PUSH:
        take_focus();
        press(Fl::event_x(), Fl::event_y(), Fl::event_clicks(), Fl::event_shift() != 0,
              Fl::event_button() == FL_RIGHT_MOUSE || (Fl::event_button() == FL_LEFT_MOUSE && Fl::event_ctrl()));
        if (Fl::event_clicks()) Fl::event_clicks(0);
        return 1;
    case FL_DRAG:
        dragTo(Fl::event_x(), Fl::event_y());
        return 1;
    case FL_RELEASE:
        release(Fl::event_x(), Fl::event_y());
        return 1;
    case FL_MOUSEWHEEL: {
        int dx = Fl::event_dx(), dy = Fl::event_dy();
        if (Fl::event_command() || Fl::event_ctrl() || Fl::event_alt()) {     // zoom around the mouse
            int s = dy ? dy : dx;
            if (s) tp_->zoom(s > 0 ? 0.8 : 1.25, tp_->timeAt(Fl::event_x()));
        } else if (dx || Fl::event_shift()) {
            int px = dx ? dx : dy;
            tp_->setView(tp_->start() + px * 24 / tp_->pixelsPerSecond(), tp_->pixelsPerSecond());
        } else {
            tp_->scrollYTo(tp_->scrollY() + dy * 24);
        }
        return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
    case FL_DND_RELEASE:
        return 1;
    case FL_PASTE:
        dropFiles(std::string(Fl::event_text(), (size_t)Fl::event_length()), Fl::event_x(), Fl::event_y());
        return 1;
    }
    return Fl_Widget::handle(e);
}

// ---------------------------------------------------------------- the panel

TimelinePanel::TimelinePanel(int X, int Y, int W, int H, std::shared_ptr<ScoreDoc> doc)
    : Fl_Group(X, Y, W, H), doc_(std::move(doc)) {
    box(FL_FLAT_BOX);
    color(kBgLocked);
    bar_ = new Fl_Group(X, Y, W, kToolH);
    bar_->box(FL_FLAT_BOX);
    bar_->color(kStatusBg);
    int bx = X + 8;
    const int by = Y + 5, bh = kToolH - 10;
    using P = TimelinePanel;
    auto button = [&](int bw, ToolButton::Icon icon, const char* lab, bool toggle, const std::string& tip, Fl_Callback* cb,
                      int gap = 4) {
        auto* b = new ToolButton(bx, by, bw, bh, icon, lab, toggle);
        b->copy_tooltip(tip.c_str());
        b->callback(cb, this);
        bx += bw + gap;
        return b;
    };
    auto menu = [&](int bw, const char* lab, const char* tip) {
        auto* m = new Fl_Menu_Button(bx, by, bw, bh, lab);
        m->box(FL_FLAT_BOX);
        m->color(kBoxFill);
        m->labelcolor(kText);
        m->labelsize(12);
        m->labelfont(FL_HELVETICA_BOLD);
        m->textsize(12);
        m->clear_visible_focus();
        m->tooltip(tip);
        bx += bw + 4;
        return m;
    };
    audioBtn_ = button(96, ToolButton::Power, "Audio", true, withShortcut("Start or stop audio", "patch.audio"),
                       [](Fl_Widget*, void* d) { App::get().toggleAudio(); static_cast<P*>(d)->refreshInfo(); }, 12);
    button(34, ToolButton::ToStart, "", false, "The playhead to the start (Home)",
           [](Fl_Widget*, void* d) { static_cast<P*>(d)->setPlayhead(0); });
    playBtn_ = button(34, ToolButton::Play, "", true, "Play from the playhead (Space)",
                      [](Fl_Widget*, void* d) { static_cast<P*>(d)->togglePlay(); });
    button(34, ToolButton::Stop, "", false, "Stop (Space)", [](Fl_Widget*, void* d) { static_cast<P*>(d)->stop(); });
    loopBtn_ = button(34, ToolButton::Loop, "", true, "Loop (L): drag in the ruler to set where",
                      [](Fl_Widget* w, void* d) { static_cast<P*>(d)->setLoop(static_cast<ToolButton*>(w)->value() != 0); }, 8);
    position_ = new Fl_Box(bx, by, 190, bh);
    position_->box(FL_FLAT_BOX);
    position_->color(fl_rgb_color(14, 15, 17));
    position_->labelfont(FL_COURIER_BOLD);
    position_->labelsize(13);
    position_->labelcolor(fl_rgb_color(110, 220, 140));
    position_->tooltip("Where the playhead is: bar.beat.tick and minutes:seconds");
    bx += 190 + 8;
    tempo_ = new Fl_Button(bx, by, 96, bh);
    tempo_->box(FL_FLAT_BOX);
    tempo_->color(kBoxFill);
    tempo_->labelcolor(kText);
    tempo_->labelsize(12);
    tempo_->clear_visible_focus();
    tempo_->tooltip("The score's tempo and meter (click to change them)");
    tempo_->callback([](Fl_Widget*, void* d) { static_cast<P*>(d)->tempoDialog(); }, this);
    bx += 96 + 8;
    snapMenu_ = menu(96, "Snap: beat", "Snap clips and points to beats or bars");
    snapMenu_->add("Off", 0, [](Fl_Widget* w, void*) { static_cast<P*>(w->parent()->parent())->setSnap(tl::Snap::Off); });
    snapMenu_->add("Beat", 0, [](Fl_Widget* w, void*) { static_cast<P*>(w->parent()->parent())->setSnap(tl::Snap::Beat); });
    snapMenu_->add("Bar", 0, [](Fl_Widget* w, void*) { static_cast<P*>(w->parent()->parent())->setSnap(tl::Snap::Bar); });
    bx += 6;
    button(28, ToolButton::Minus, "", false, withShortcut("Zoom out", "view.zoomout"),
           [](Fl_Widget*, void* d) { static_cast<P*>(d)->zoomOut(); }, 2);
    button(46, ToolButton::None, "Fit", false, withShortcut("The whole score", "view.actual"),
           [](Fl_Widget*, void* d) { static_cast<P*>(d)->fit(); }, 2);
    button(28, ToolButton::Plus, "", false, withShortcut("Zoom in", "view.zoomin"),
           [](Fl_Widget*, void* d) { static_cast<P*>(d)->zoomIn(); }, 12);
    addMenu_ = menu(64, "Add", "Add a track, a sound, a score, a patch clip or an automation lane");
    struct Item {
        const char* label;
        void (TimelinePanel::*fn)();
        int flags;
    };
    static const Item adds[] = {{"Track", &P::addTrack, FL_MENU_DIVIDER},
                                {"Sound...", &P::addSoundDialog, 0},
                                {"Sub-score...", &P::addScoreDialog, 0},
                                {"Patch Clip...", &P::addPatchDialog, FL_MENU_DIVIDER},
                                {"Automation Lane...", &P::addLaneDialog, 0}};
    for (const Item& it : adds)
        addMenu_->add(it.label, 0, [](Fl_Widget* w, void* d) {
            auto* p = static_cast<P*>(w->parent()->parent());
            (p->*(static_cast<const Item*>(d)->fn))();
        }, (void*)&it, it.flags);
    editMenu_ = menu(64, "Edit", "Edit the clips (also the right button on a clip)");
    static const Item edits[] = {{"Undo", &P::undo, 0},
                                 {"Redo", &P::redo, FL_MENU_DIVIDER},
                                 {"Cut", &P::cut, 0},
                                 {"Copy", &P::copy, 0},
                                 {"Paste", &P::paste, 0},
                                 {"Duplicate", &P::duplicate, 0},
                                 {"Delete", &P::erase, FL_MENU_DIVIDER},
                                 {"Split at Playhead", &P::splitAtPlayhead, 0},
                                 {"Make Unique", &P::makeUnique, FL_MENU_DIVIDER},
                                 {"Select All", &P::selectAll, 0}};
    for (const Item& it : edits)
        editMenu_->add(it.label, 0, [](Fl_Widget* w, void* d) {
            auto* p = static_cast<P*>(w->parent()->parent());
            (p->*(static_cast<const Item*>(d)->fn))();
        }, (void*)&it, it.flags);
    bx += 6;
    renderBtn_ = new Fl_Button(bx, by, 84, bh, "Render...");
    renderBtn_->box(FL_FLAT_BOX);
    renderBtn_->color(fl_rgb_color(40, 90, 190));
    renderBtn_->labelcolor(FL_WHITE);
    renderBtn_->labelsize(12);
    renderBtn_->labelfont(FL_HELVETICA_BOLD);
    renderBtn_->clear_visible_focus();
    renderBtn_->tooltip("Render the score offline into a stereo WAV file");
    renderBtn_->callback([](Fl_Widget*, void* d) { static_cast<P*>(d)->renderDialog(); }, this);
    bx += 84 + 8;
    // the views of the score (mockup 4): this one, and the roll's (a window of its own on the same document)
    auto* here = button(74, ToolButton::None, "Timeline", true, "The score as tracks (this view)", [](Fl_Widget* w, void*) {
        static_cast<ToolButton*>(w)->value(1);
    }, 2);
    here->value(1);
    button(48, ToolButton::None, "Roll", false, "The same score as a roll: staves, edited note by note (the same edits, undo and selection)",
           [](Fl_Widget*, void* d) { static_cast<P*>(d)->showRoll(0); }, 2);
    button(44, ToolButton::None, "List", false, "The same score as a list of its events, each cell edited",
           [](Fl_Widget*, void* d) { static_cast<P*>(d)->showRoll(2); }, 8);
    auto* spacer = new Fl_Box(bx, Y, std::max(1, X + W - bx), kToolH);
    spacer->box(FL_NO_BOX);
    bar_->resizable(spacer);
    bar_->end();
    crumbs_ = new Fl_Group(X, Y + kToolH, W, kCrumbsH);
    crumbs_->box(FL_FLAT_BOX);
    crumbs_->color(fl_rgb_color(30, 32, 40));
    crumbs_->end();
    int midY = Y + kToolH + kCrumbsH, midH = H - kToolH - kCrumbsH - kStatusH;
    int viewW = W - kInspectorW - kScrollW;
    view_ = new TimelineView(X, midY, viewW, midH - kScrollW, this);
    vbar_ = new Fl_Scrollbar(X + viewW, midY, kScrollW, midH - kScrollW);
    vbar_->type(FL_VERTICAL);
    vbar_->callback([](Fl_Widget* w, void* d) { static_cast<P*>(d)->scrollYTo(static_cast<Fl_Scrollbar*>(w)->value()); }, this);
    hbar_ = new Fl_Scrollbar(X + TimelineView::kHeadW, midY + midH - kScrollW, viewW - TimelineView::kHeadW, kScrollW);
    hbar_->type(FL_HORIZONTAL);
    hbar_->callback([](Fl_Widget* w, void* d) {
        auto* p = static_cast<P*>(d);
        p->setView(static_cast<Fl_Scrollbar*>(w)->value() / 20.0, p->pps_);
    }, this);
    for (Fl_Scrollbar* s : {vbar_, hbar_}) {
        s->box(FL_FLAT_BOX);
        s->slider(FL_FLAT_BOX);
        s->color(kStatusBg);
        s->selection_color(kBorder);
        s->clear_visible_focus();
    }
    inspector_ = new Fl_Group(X + W - kInspectorW, midY, kInspectorW, midH);
    inspector_->box(FL_FLAT_BOX);
    inspector_->color(fl_rgb_color(32, 33, 38));
    buildInspector();
    inspector_->end();
    info_ = new Fl_Box(X, Y + H - kStatusH, W / 2, kStatusH);
    message_ = new Fl_Box(X + W / 2, Y + H - kStatusH, W - W / 2, kStatusH);
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
    token_ = doc_->listen([this](unsigned what) {
        if (what & ScoreDoc::Content) contentsChanged();
        if (what & ScoreDoc::Selection) selectionChanged();
        if (what & ScoreDoc::State) stateChanged();
    });
    waves_.onReady = [this] { view_->redraw(); };
    render_.onProgress = [this](float f) { setStatus("rendering… " + std::to_string((int)(f * 100)) + "%"); };
    auto all = tl::tracks(doc_->score());
    if (!all.empty()) track_ = all.front();
    audioBtn_->value(App::get().audio.running());
    audioBtn_->copy_label(App::get().audio.running() ? "Audio On" : "Audio Off");
    view_->layoutRows();
    layoutChildren();
    fit();
    updateCrumbs();
    showInspector();
    refreshInfo();
    setStatus("Space plays · drag clips, their edges to trim · click a lane to add a point · " + shortcutText(FL_COMMAND) +
              "wheel zooms · right-click: Edit");
    Fl::add_timeout(0.1, tick, this);
}

void TimelinePanel::showRoll(int view) { openRoll(doc_, view); }

TimelinePanel::~TimelinePanel() {
    Fl::remove_timeout(tick, this);
    doc_->unlisten(token_);
    render_.cancel();
}

void TimelinePanel::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    layoutChildren();
    setView(t0_, pps_);
}

void TimelinePanel::layoutChildren() {
    int X = x(), Y = y(), W = w(), H = h();
    bar_->resize(X, Y, W, kToolH);
    int top = Y + kToolH;
    int ch = crumbs_->visible() ? kCrumbsH : 0;
    crumbs_->resize(X, top, W, ch);
    top += ch;
    int status = Y + H - kStatusH;
    int midH = std::max(60, status - top);
    int viewW = std::max(300, W - kInspectorW - kScrollW);
    view_->resize(X, top, viewW, midH - kScrollW);
    vbar_->resize(X + viewW, top, kScrollW, midH - kScrollW);
    hbar_->resize(X + TimelineView::kHeadW, top + midH - kScrollW, viewW - TimelineView::kHeadW + kScrollW, kScrollW);
    inspector_->resize(X + W - kInspectorW, top, kInspectorW, midH);
    showInspector();
    info_->resize(X, status, W / 2, kStatusH);
    message_->resize(X + W / 2, status, W - W / 2, kStatusH);
    updateScrollbars();
    redraw();
}

// ---- the view

void TimelinePanel::setView(double t0, double pps) {
    pps = std::clamp(pps, 0.5, 20000.0);
    double len = std::max(doc_->score().duration(), playhead()) + 30;
    t0 = std::clamp(t0, 0.0, std::max(0.0, len - view_->laneW() / pps));
    t0_ = t0;
    pps_ = pps;
    updateScrollbars();
    view_->redraw();
}

void TimelinePanel::zoom(double factor, double around) {
    double px = (around - t0_) * pps_;
    double pps = std::clamp(pps_ * factor, 0.5, 20000.0);
    setView(around - px / pps, pps);
}

void TimelinePanel::fit() {
    double len = std::max(doc_->score().duration(), 8 * tl::beatSeconds(doc_->score()));
    setView(0, std::max(0.5, (view_->laneW() - 20) / (len * 1.05)));
}

double TimelinePanel::centre() const { return t0_ + view_->laneW() / pps_ / 2; }
double TimelinePanel::timeAt(int ex) const { return t0_ + (ex - view_->laneX()) / pps_; }
int TimelinePanel::xOf(double t) const { return view_->laneX() + (int)std::lround((t - t0_) * pps_); }

void TimelinePanel::scrollYTo(int y) {
    int most = std::max(0, view_->contentH() - (view_->h() - TimelineView::kRulerH));
    sy_ = std::clamp(y, 0, most);
    updateScrollbars();
    view_->redraw();
}

void TimelinePanel::updateScrollbars() {
    int visH = view_->h() - TimelineView::kRulerH;
    vbar_->value(sy_, std::max(1, visH), 0, std::max(visH, view_->contentH()));
    double len = std::max(doc_->score().duration(), playhead()) + 30;
    int total = (int)(len * 20), shown = (int)(view_->laneW() / pps_ * 20);
    hbar_->value((int)(t0_ * 20), std::max(1, shown), 0, std::max(shown, total));
}

void TimelinePanel::setSnap(tl::Snap s) {
    snap_ = s;
    snapMenu_->copy_label(s == tl::Snap::Off ? "Snap: off" : s == tl::Snap::Beat ? "Snap: beat" : "Snap: bar");
    snapMenu_->redraw();
}

std::string TimelinePanel::currentTrack() const {
    auto all = tl::tracks(doc_->score());
    if (std::find(all.begin(), all.end(), track_) != all.end()) return track_;
    return all.empty() ? std::string("track 1") : all.front();
}

void TimelinePanel::setCurrentTrack(const std::string& t) {
    if (t == track_) return;
    track_ = t;
    view_->redraw();
    showInspector();
}

// ---- playing

Box* TimelinePanel::player() { return doc_->player(&App::get().host); }

// the transport the score plays on: its box's patch's, or the application's (every open patch's)
static Patch* clockPatch(const ScoreDoc& d) {
    Box* b = d.box();
    return b ? b->patch : &App::get().host;
}

bool TimelinePanel::playing() const { return transportOf(clockPatch(*doc_))->playing(); }

double TimelinePanel::playhead() const {
    if (!playing()) return playhead_;
    Patch* p = clockPatch(*doc_);
    auto t = transportOf(p);
    int lap;
    double pos;
    t->where(p, lap, pos);
    return t->seconds(pos);
}

void TimelinePanel::setPlayhead(double t) {
    playhead_ = std::max(0.0, t);
    if (playing())
        if (Box* b = player()) transportOf(b->patch)->locate(b->patch, playhead_);
    view_->redraw();
    refreshInfo();
}

void TimelinePanel::setLoop(bool on) {
    loop_ = on && loopB_ > loopA_;
    loopBtn_->value(loop_);
    loopBtn_->redraw();
    if (playing())
        if (Box* b = player()) transportOf(b->patch)->setLoop(b->patch, loop_, loopA_, loopB_);
    view_->redraw();
}

void TimelinePanel::setLoopRange(double a, double b) {
    loopA_ = a;
    loopB_ = b;
    view_->redraw();
}

void TimelinePanel::togglePlay() {
    if (playing()) stop();
    else play();
}

void TimelinePanel::play() {
    App& app = App::get();
    playBtn_->value(0);
    if (!app.audio.running()) {
        setStatus("audio is off: turn it on (the Audio button, " + shortcutText(FL_COMMAND + '/') + ") to listen");
        return;
    }
    Box* b = player();
    if (!b) {
        setStatus("nothing can play this score now");
        return;
    }
    auto t = transportOf(b->patch);
    const sc::Score& s = doc_->score();
    t->setTempo(b->patch, s.bpm);                       // bars and beats as the score counts them
    t->setMeter(b->patch, s.meterNum, s.meterDen);
    t->setLoop(b->patch, loop_, loopA_, loopB_);
    t->locate(b->patch, playhead_);
    t->play(b->patch);
    wasPlaying_ = true;
    playBtn_->value(1);
    Fl::remove_timeout(tick, this);
    Fl::add_timeout(1.0 / 30, tick, this);
}

void TimelinePanel::stop() {
    Patch* p = clockPatch(*doc_);
    auto t = transportOf(p);
    if (t->playing()) {
        playhead_ = playhead();
        t->stop(p);
    }
    wasPlaying_ = false;
    playBtn_->value(0);
    view_->redraw();
    refreshInfo();
}

void TimelinePanel::tick(void* data) {
    auto* p = static_cast<TimelinePanel*>(data);
    App& app = App::get();
    if (++p->followCount_ % 3 == 0) p->doc_->follow();     // a message to the box, the box replaced or gone
    if (p->audioBtn_->value() != (app.audio.running() ? 1 : 0)) {
        p->audioBtn_->value(app.audio.running());
        p->audioBtn_->copy_label(app.audio.running() ? "Audio On" : "Audio Off");
        p->audioBtn_->redraw();
    }
    bool now = p->playing();
    if (now != (p->playBtn_->value() != 0)) {
        p->playBtn_->value(now);
        p->playBtn_->redraw();
        if (!now) p->wasPlaying_ = false;
    }
    if (now) {
        double ph = p->playhead();
        double shown = p->view_->laneW() / p->pps_;
        if (ph < p->t0_ || ph > p->t0_ + shown) p->setView(ph - shown * 0.05, p->pps_);   // the view follows
        p->view_->redraw();
        p->refreshInfo();
    }
    Fl::repeat_timeout(now ? 1.0 / 30 : 0.1, tick, data);
}

// ---- commands

void TimelinePanel::after(bool done) {
    (void)done;
    setStatus(doc_->message);
}

static std::vector<uint64_t> clipsOf(ScoreDoc& d) {
    std::vector<uint64_t> out;
    for (uint64_t id : d.selection())
        if (d.score().find(id)) out.push_back(id);
    return out;
}

void TimelinePanel::copy() {
    auto ids = clipsOf(*doc_);
    if (ids.empty()) { setStatus("Copy: select clips first"); return; }
    tl::clipboard() = tl::copy(doc_->score(), ids);
    setStatus("copied " + std::to_string(ids.size()) + (ids.size() == 1 ? " clip" : " clips"));
}

void TimelinePanel::cut() {
    auto ids = clipsOf(*doc_);
    if (ids.empty()) { setStatus("Cut: select clips first"); return; }
    tl::clipboard() = tl::copy(doc_->score(), ids);
    after(tl::removeClips(*doc_, ids, "Cut"));
}

void TimelinePanel::paste() {
    const auto& c = tl::clipboard();
    auto made = tl::paste(*doc_, c, playhead(), currentTrack());
    after(!made.empty());
}

void TimelinePanel::duplicate() { after(!tl::duplicate(*doc_, clipsOf(*doc_)).empty()); }
void TimelinePanel::erase() { after(tl::removeClips(*doc_, clipsOf(*doc_))); }

void TimelinePanel::splitAtPlayhead() {
    auto ids = clipsOf(*doc_);
    double t = playhead();
    if (ids.empty())                                     // nothing selected: the sounds of the track under the playhead
        for (auto& e : doc_->score().events())
            if (e->kind == sc::Kind::Sound && e->row == currentTrack() && e->at < t && e->end() > t) ids.push_back(e->id);
    after(!tl::split(*doc_, ids, t).empty());
}

void TimelinePanel::makeUnique() {
    auto ids = clipsOf(*doc_);
    if (ids.size() != 1) { setStatus("Make Unique: select one placed score"); return; }
    after(tl::makeUnique(*doc_, ids[0]));
}

void TimelinePanel::selectAll() {
    std::vector<uint64_t> ids;
    for (auto& e : doc_->score().events())
        if (tl::isClip(*e)) ids.push_back(e->id);
    doc_->select(ids);
}

void TimelinePanel::undo() { after(doc_->undo()); }
void TimelinePanel::redo() { after(doc_->redo()); }

void TimelinePanel::addTrack() {
    std::string t = tl::addTrack(*doc_);
    if (!t.empty()) setCurrentTrack(t);
    after(!t.empty());
}

bool TimelinePanel::addSoundFile(const std::string& path, const std::string& track, double at) {
    auto s = std::make_shared<SampleBuffer>();
    std::string err;
    if (!decodeAudioFile(path, *s, err)) {
        setStatus("can't read " + baseOf(path) + ": " + err);
        return false;
    }
    double len = s->sr > 0 ? s->frames / s->sr : 0;
    uint64_t id = tl::addSound(*doc_, path, false, track, at, len);
    after(id != 0);
    return id != 0;
}

void TimelinePanel::addSoundDialog() {
    std::string f = chooseOpen("Add Sound", "Sound\t*.{wav,wave,aif,aiff,flac,mp3}\nAll files\t*");
    if (!f.empty()) addSoundFile(f, currentTrack(), playhead());
}

void TimelinePanel::addScoreDialog() {
    std::string f = chooseOpen("Add Sub-score", "Score\t*.score\nAll files\t*");
    if (f.empty()) return;
    after(tl::addSubScore(*doc_, f, currentTrack(), playhead()) != 0);
}

void TimelinePanel::addPatchDialog() {
    std::string f = chooseOpen("Add Patch Clip", "Moritz patch\t*.moritz\nAll files\t*");
    if (f.empty()) return;
    after(tl::addPatchClip(*doc_, f, currentTrack(), playhead(), 0) != 0);
}

void TimelinePanel::addLaneDialog() {
    std::string t = currentTrack();
    if (tl::trackIndex(doc_->score(), t) < 0) {         // no track yet: one first
        t = tl::addTrack(*doc_);
        if (t.empty()) { after(false); return; }
        setCurrentTrack(t);
    }
    std::string r = askLine("An automation lane: the receive name it drives, and its range (density 0 1)", "density 0 1");
    if (r.empty()) return;
    Message m = parseMessage(r);
    if (m.empty() || !m[0].isSymbol()) { setStatus("a lane is named after a receive: density 0 1"); return; }
    double lo = m.size() > 1 && m[1].isFloat() ? m[1].f : 0, hi = m.size() > 2 && m[2].isFloat() ? m[2].f : 1;
    after(tl::addLane(*doc_, t, m[0].s, lo, hi));
}

void TimelinePanel::tempoDialog() {
    const sc::Score& s = doc_->score();
    std::string r = askLine("Tempo (beats a minute) and meter, as 120 4/4", fmt(s.bpm) + " " + std::to_string(s.meterNum) + "/" +
                                                                       std::to_string(s.meterDen));
    if (r.empty()) return;
    double bpm = 0;
    int num = s.meterNum, den = s.meterDen;
    if (std::sscanf(r.c_str(), "%lf %d/%d", &bpm, &num, &den) < 1) { setStatus("a tempo, then a meter: 120 4/4"); return; }
    auto e = doc_->edit("Tempo and Meter");
    e.setHeader(s.name, bpm, num, den);
    after(e.commit());
}

void TimelinePanel::renameTrackDialog(const std::string& track) {
    std::string r = askLine("The track's name", track);
    if (r.empty() || r == track) return;
    if (tl::renameTrack(*doc_, track, r)) setCurrentTrack(r);
    after(true);
}

void TimelinePanel::renderDialog() {
    if (render_.running()) {
        render_.cancel();
        setStatus("render stopped");
        renderBtn_->copy_label("Render...");
        return;
    }
    Fl_Native_File_Chooser fc;
    fc.title("Render the Score to a WAV File");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("WAV\t*.wav");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    std::string name = doc_->score().name.empty() ? std::string("score") : doc_->score().name;
    std::string dir = doc_->folder();
    if (!dir.empty()) fc.directory(dir.c_str());
    fc.preset_file((name + ".wav").c_str());
    std::string p = showSaveChooser(fc);
    if (p.empty()) return;
    if (!endsWith(lower(p), ".wav")) p += ".wav";
    std::string err;
    double sr = App::get().audio.running() ? App::get().audio.sampleRate() : 44100;
    bool ok = render_.start(doc_->score(), doc_->folder(), p, 2, sr, [this](bool done, const std::string& m) {
        renderBtn_->copy_label("Render...");
        setStatus(m);
        App::get().log(m);
        (void)done;
    }, err);
    if (!ok) { setStatus("can't render: " + err); return; }
    if (render_.running()) renderBtn_->copy_label("Stop");
}

void TimelinePanel::openClip(uint64_t id) {
    auto e = doc_->score().find(id);
    if (!e) return;
    switch (e->kind) {
    case sc::Kind::Score: {
        auto sub = ScoreDoc::ofPlacement(doc_, id);
        if (sub) openTimeline(sub);
        break;
    }
    case sc::Kind::Patch: {
        std::string p = tl::sourcePath(*doc_, e->source);
        if (!endsWith(lower(p), ".moritz")) p += ".moritz";
        App::get().openDocument(p);
        break;
    }
    case sc::Kind::Sound:
        if (e->buffer) {
            Patch* root = doc_->box() ? doc_->box()->patch->root() : &App::get().host;
            openBufferEditor(root, e->source);
        } else {
            App::get().openSound(tl::sourcePath(*doc_, e->source));
        }
        break;
    default:
        setStatus("notes are edited in the roll");
        break;
    }
}

// ---- showing

void TimelinePanel::contentsChanged() {
    view_->layoutRows();
    scrollYTo(sy_);
    updateScrollbars();
    view_->redraw();
    showInspector();
    refreshInfo();
    updateCrumbs();
}

void TimelinePanel::selectionChanged() {
    view_->redraw();
    showInspector();
    refreshInfo();
}

void TimelinePanel::stateChanged() {
    updateCrumbs();
    refreshInfo();
    if (!doc_->message.empty()) setStatus(doc_->message);
    if (onTitle) onTitle();
}

void TimelinePanel::setStatus(const std::string& s) {
    if (s == messageText_) return;
    messageText_ = s;
    message_->copy_label((s + "  ").c_str());
    message_->redraw();
}

void TimelinePanel::refreshInfo() {
    const sc::Score& s = doc_->score();
    auto all = tl::tracks(s);
    std::string inf = "  " + std::to_string(all.size()) + (all.size() == 1 ? " track · " : " tracks · ") +
                      std::to_string(s.size()) + " events · " + clock(s.duration()) + " · " + fmt(s.bpm) + " bpm " +
                      std::to_string(s.meterNum) + "/" + std::to_string(s.meterDen);
    if (!doc_->selection().empty()) inf += " · " + std::to_string(doc_->selection().size()) + " selected";
    if (inf != infoText_) {
        infoText_ = inf;
        info_->copy_label(inf.c_str());
        info_->redraw();
    }
    double ph = playhead();
    std::string pos = tl::bbt(s, ph) + "  " + clock(ph);
    if (pos != positionText_) {
        positionText_ = pos;
        position_->copy_label(pos.c_str());
        position_->redraw();
    }
    std::string tempo = "♩ " + fmt(s.bpm) + "  " + std::to_string(s.meterNum) + "/" + std::to_string(s.meterDen);
    if (tempo != tempoText_) {
        tempoText_ = tempo;
        tempo_->copy_label(tempo.c_str());
        tempo_->redraw();
    }
}

void TimelinePanel::updateCrumbs() {
    auto chain = doc_->chain();
    bool show = chain.size() > 1;
    crumbs_->clear();
    crumbDocs_.clear();
    for (auto& c : chain) crumbDocs_.push_back(c);
    if (show) {
        crumbs_->begin();
        int bx = crumbs_->x() + 8;
        for (size_t k = 0; k < chain.size(); ++k) {
            std::string t = chain[k]->title();
            if (k + 1 == chain.size()) {
                auto e = doc_->parent()->score().find(doc_->placement());
                int which = 0, n = tl::instances(doc_->parent()->score(), &doc_->score(), &which, doc_->placement());
                if (e && n > 0) t += "   (instance " + std::to_string(which) + " of " + std::to_string(n) + ")";
            }
            fl_font(FL_HELVETICA, 12);
            int bw = (int)fl_width(t.c_str()) + 16;
            auto* b = new Fl_Button(bx, crumbs_->y() + 2, bw, kCrumbsH - 4);
            b->copy_label(t.c_str());
            b->box(FL_FLAT_BOX);
            b->color(fl_rgb_color(30, 32, 40));
            b->labelsize(12);
            b->labelcolor(k + 1 == chain.size() ? kText : kAccent);
            b->clear_visible_focus();
            if (k + 1 < chain.size()) {
                b->tooltip("Open this score's timeline");
                b->callback([](Fl_Widget* w, void* d) {
                    auto* p = static_cast<TimelinePanel*>(w->parent()->parent());
                    size_t i = (size_t)(intptr_t)d;
                    if (i < p->crumbDocs_.size())
                        if (auto s = p->crumbDocs_[i].lock()) openTimeline(s);
                }, (void*)(intptr_t)k);
            }
            bx += bw;
            if (k + 1 < chain.size()) {
                auto* sep = new Fl_Box(bx, crumbs_->y() + 2, 14, kCrumbsH - 4, "›");
                sep->labelcolor(kDim);
                sep->labelsize(12);
                bx += 14;
            }
        }
        crumbs_->end();
    }
    if (show != (crumbs_->visible() != 0)) {
        if (show) crumbs_->show();
        else crumbs_->hide();
        layoutChildren();
    }
    crumbs_->redraw();
}

// ---- the inspector

void TimelinePanel::buildInspector() {
    int X = inspector_->x(), Y = inspector_->y();
    insTitle_ = new Fl_Box(X + 12, Y + 10, kInspectorW - 24, 22);
    insTitle_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    insTitle_->labelfont(FL_HELVETICA_BOLD);
    insTitle_->labelsize(14);
    insTitle_->labelcolor(kText);
    insSub_ = new Fl_Box(X + 12, Y + 32, kInspectorW - 24, 18);
    insSub_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    insSub_->labelsize(11);
    insSub_->labelcolor(kDim);
    for (int k = 0; k < kFields; ++k) {
        Field f;
        f.label = new Fl_Box(X + 12, Y, 86, 22);
        f.label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        f.label->labelsize(12);
        f.label->labelcolor(kDim);
        f.input = new Fl_Input(X + 100, Y, kInspectorW - 112, 22);
        f.input->box(FL_FLAT_BOX);
        f.input->color(fl_rgb_color(22, 23, 27));
        f.input->textcolor(kText);
        f.input->textsize(12);
        f.input->cursor_color(kText);
        f.input->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
        f.input->callback([](Fl_Widget* w, void* d) {
            auto* p = static_cast<TimelinePanel*>(d);
            for (int i = 0; i < (int)p->fields_.size(); ++i)
                if (p->fields_[(size_t)i].input == w) p->applyField(i);
        }, this);
        fields_.push_back(f);
    }
    for (int k = 0; k < kButtons; ++k) {
        auto* b = new Fl_Button(X + 12, Y, kInspectorW - 24, 24);
        b->box(FL_FLAT_BOX);
        b->color(kBoxFill);
        b->labelcolor(kText);
        b->labelsize(12);
        b->clear_visible_focus();
        b->callback([](Fl_Widget* w, void* d) {
            auto* p = static_cast<TimelinePanel*>(d);
            std::string what = w->label() ? w->label() : "";
            ScoreDoc& doc = *p->doc_;
            std::string t = p->currentTrack();
            if (what == "Open") { if (!doc.selection().empty()) p->openClip(doc.selection()[0]); }
            else if (what == "Make Unique") p->makeUnique();
            else if (what == "Split at Playhead") p->splitAtPlayhead();
            else if (what == "Delete") p->erase();
            else if (what == "Add Automation Lane...") p->addLaneDialog();
            else if (what == "Move Track Up") p->after(tl::moveTrack(doc, t, -1));
            else if (what == "Move Track Down") p->after(tl::moveTrack(doc, t, 1));
            else if (what == "Delete Track") p->after(tl::deleteTrack(doc, t));
            else if (what == "Mute" || what == "Unmute" || what == "Solo" || what == "Unsolo") {
                sc::Strip st = tl::stripOf(doc.score(), t);
                if (what == "Mute" || what == "Unmute") st.mute = !st.mute;
                else st.solo = !st.solo;
                p->after(tl::setStrip(doc, t, st));
            }
        }, this);
        insButtons_.push_back(b);
    }
}

// The fields shown for the selection: a clip (by its kind), a point, several clips, or the current track
void TimelinePanel::showInspector() {
    if (fields_.empty()) return;
    ScoreDoc& d = *doc_;
    const sc::Score& s = d.score();
    struct F { const char* key; const char* label; std::string value; };
    std::vector<F> fs;
    std::vector<const char*> buttons;
    std::string title, sub;
    auto sel = d.selection();
    std::shared_ptr<sc::Event> e = sel.size() == 1 ? s.find(sel[0]) : nullptr;
    if (e) {
        insKey_ = "event";
        switch (e->kind) {
        case sc::Kind::Sound:
            title = "Sound";
            sub = e->buffer ? "buffer~ " + e->source : e->source;
            fs = {{"at", "start", fmt(e->at, "%.4g")}, {"dur", "length", fmt(e->dur, "%.4g")},
                  {"from", "from", fmt(e->from, "%.4g")}, {"to", "to", e->to >= 0 ? fmt(e->to, "%.4g") : std::string("end")},
                  {"gain", "gain dB", dbText(e->gain)}, {"az", "azimuth", fmt(e->az)},
                  {"fade-in", "fade in", fmt(sc::numField(e->extra, "fade-in", 0), "%.4g")},
                  {"fade-out", "fade out", fmt(sc::numField(e->extra, "fade-out", 0), "%.4g")}};
            buttons = {"Open", "Split at Playhead", "Delete"};
            break;
        case sc::Kind::Score: {
            int which = 0, n = tl::instances(s, e->sub.get(), &which, e->id);
            title = "Score " + (e->sub ? e->sub->name : std::string());
            sub = (n > 1 ? "instance " + std::to_string(which) + " of " + std::to_string(n) + " · " : std::string()) +
                  (e->ref.empty() ? std::string("embedded") : e->ref);
            fs = {{"at", "start", fmt(e->at, "%.4g")}, {"dur", "length", fmt(e->dur, "%.4g")},
                  {"stretch", "stretch", fmt(e->stretch)}, {"transpose", "transpose", fmt(e->transpose)},
                  {"gain", "gain dB", dbText(e->gain)}, {"az", "azimuth", fmt(e->az)}, {"el", "elevation", fmt(e->el)}};
            buttons = {"Open", "Make Unique", "Delete"};
            break;
        }
        case sc::Kind::Patch:
            title = "Patch " + baseOf(e->source);
            sub = "runs while the clip plays (in a later version)";
            fs = {{"at", "start", fmt(e->at, "%.4g")}, {"dur", "length", fmt(e->dur, "%.4g")}, {"source", "patch", e->source}};
            buttons = {"Open", "Delete"};
            break;
        case sc::Kind::Note:
            title = "Note " + fmt(e->sounding());
            sub = "edited in the roll";
            fs = {{"at", "start", fmt(e->at, "%.4g")}, {"dur", "length", fmt(e->dur, "%.4g")}, {"pitch", "pitch", fmt(e->pitch)},
                  {"dyn", "dynamics", e->dyn}, {"gain", "gain dB", dbText(e->gain)}};
            buttons = {"Delete"};
            break;
        case sc::Kind::Message: {
            title = "Point " + e->source;
            sub = "sent to receive " + e->source;
            std::string v;
            for (auto& x : e->values) v += (v.empty() ? "" : " ") + (x.kind == sc::Datum::Num ? fmt(x.num) : x.s);
            fs = {{"at", "time", fmt(e->at, "%.4g")}, {"values", "value", v}};
            buttons = {"Delete"};
            break;
        }
        }
        sub = tl::bbt(s, e->at) + " · " + sub;
    } else if (sel.size() > 1) {
        insKey_ = "many";
        title = std::to_string(sel.size()) + " clips";
        sub = "drag one to move them all";
        buttons = {"Split at Playhead", "Delete"};
    } else {
        insKey_ = "track";
        std::string t = currentTrack();
        bool exists = tl::trackIndex(s, t) >= 0;
        title = exists ? "Track " + (t.empty() ? std::string("(no track)") : t) : "No track";
        if (exists) {
            sc::Strip st = tl::stripOf(s, t);
            sub = st.mute ? "muted" : st.solo ? "soloed" : "its mixer strip";
            fs = {{"name", "name", t}, {"gain", "gain dB", dbText(st.gain)}, {"az", "pan (az)", fmt(st.az)}};
            buttons = {st.mute ? "Unmute" : "Mute", st.solo ? "Unsolo" : "Solo", "Add Automation Lane...", "Move Track Up",
                       "Move Track Down", "Delete Track"};
        } else {
            sub = "Add > Track, or drop a sound file";
        }
    }
    insTitle_->copy_label(title.c_str());
    insSub_->copy_label(sub.c_str());
    int X = inspector_->x(), Y = inspector_->y() + 60;
    for (int k = 0; k < kFields; ++k) {
        Field& f = fields_[(size_t)k];
        if (k < (int)fs.size()) {
            f.key = fs[(size_t)k].key;
            f.label->copy_label(fs[(size_t)k].label);
            if (Fl::focus() != f.input || f.input->value() == nullptr) f.input->value(fs[(size_t)k].value.c_str());
            f.label->resize(X + 12, Y, 86, 22);
            f.input->resize(X + 100, Y, kInspectorW - 112, 22);
            f.label->show();
            f.input->show();
            Y += 28;
        } else {
            f.key.clear();
            f.label->hide();
            f.input->hide();
        }
    }
    Y += 10;
    for (int k = 0; k < kButtons; ++k) {
        Fl_Button* b = insButtons_[(size_t)k];
        if (k < (int)buttons.size()) {
            b->copy_label(buttons[(size_t)k]);
            b->resize(X + 12, Y, kInspectorW - 24, 24);
            b->show();
            Y += 30;
        } else {
            b->hide();
        }
    }
    inspector_->redraw();
}

void TimelinePanel::applyField(int k) {
    if (k < 0 || k >= (int)fields_.size()) return;
    Field& f = fields_[(size_t)k];
    std::string v = f.input->value() ? f.input->value() : "";
    ScoreDoc& d = *doc_;
    char* end = nullptr;
    double num = std::strtod(v.c_str(), &end);
    bool isNum = end && end != v.c_str() && std::isfinite(num);
    auto bad = [&](const char* why) { setStatus(why); showInspector(); };
    if (insKey_ == "track") {
        std::string t = currentTrack();
        sc::Strip st = tl::stripOf(d.score(), t);
        if (f.key == "name") {
            if (v != t && tl::renameTrack(d, t, v)) setCurrentTrack(v);
            after(true);
            return;
        }
        if (f.key == "gain") {
            if (v == "-inf") st.gain = 0;
            else if (isNum) st.gain = std::pow(10.0, num / 20);
            else return bad("gain: a number of dB, like -6");
        } else if (f.key == "az") {
            if (!isNum) return bad("pan: an azimuth in degrees (90 left, -90 right)");
            st.az = num;
        }
        sc::Strip old = tl::stripOf(d.score(), t);
        if (old.gain != st.gain || old.az != st.az) after(tl::setStrip(d, t, st));
        return;
    }
    if (insKey_ != "event" || d.selection().size() != 1) return;
    auto e = d.score().find(d.selection()[0]);
    if (!e) return;
    sc::Event now = *e;
    const std::string& key = f.key;
    if (key == "source") now.source = v;
    else if (key == "dyn") now.dyn = v;
    else if (key == "values") {
        Message m = parseMessage(v);
        now.values.clear();
        for (auto& a : m) now.values.push_back(a.isFloat() ? sc::Datum::number(a.f) : sc::Datum::symbol(a.s));
    } else if (key == "to" && (v == "end" || v.empty())) now.to = -1;
    else if (key == "gain") {
        if (v == "-inf") now.gain = 0;
        else if (isNum) now.gain = std::pow(10.0, num / 20);
        else return bad("gain: a number of dB");
    } else {
        if (!isNum) return bad("a number, please");
        if (key == "at") now.at = std::max(0.0, num);
        else if (key == "dur") now.dur = std::max(0.0, num);
        else if (key == "from") now.from = std::max(0.0, num);
        else if (key == "to") now.to = num;
        else if (key == "az") now.az = num;
        else if (key == "el") now.el = std::clamp(num, -90.0, 90.0);
        else if (key == "stretch") {
            if (!(num > 0)) return bad("stretch: above 0");
            now.stretch = num;
        } else if (key == "transpose") now.transpose = num;
        else if (key == "pitch") now.pitch = num;
        else if (key == "fade-in" || key == "fade-out") {
            if (num <= 0) sc::eraseField(now.extra, key);
            else sc::setField(now.extra, key, sc::Datum::number(std::min(num, now.dur)));
        }
    }
    // nothing changed: no step
    if (now.at == e->at && now.dur == e->dur && now.from == e->from && now.to == e->to && now.gain == e->gain &&
        now.az == e->az && now.el == e->el && now.stretch == e->stretch && now.transpose == e->transpose &&
        now.pitch == e->pitch && now.source == e->source && now.dyn == e->dyn && now.values == e->values &&
        now.extra == e->extra) {
        showInspector();
        return;
    }
    std::string label = f.label->label() ? f.label->label() : key;
    after(tl::change(d, now, "Set " + label));
}

// ---- the keys

int TimelinePanel::handle(int e) {
    if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::first_window() == window()) {
        if (dynamic_cast<Fl_Input*>(Fl::focus())) return Fl_Group::handle(e);   // typing in the inspector
        int k = Fl::event_key();
        bool cmd = Fl::event_command() != 0, mods = cmd || Fl::event_ctrl() || Fl::event_alt();
        if (!mods) {
            switch (k) {
            case ' ': togglePlay(); return 1;
            case FL_BackSpace:
            case FL_Delete: erase(); return 1;
            case FL_Home: setPlayhead(0); setView(0, pps_); return 1;
            case FL_Left: setView(t0_ - view_->laneW() / pps_ / 8, pps_); return 1;
            case FL_Right: setView(t0_ + view_->laneW() / pps_ / 8, pps_); return 1;
            case 'l': setLoop(!loop_); return 1;
            case 's': splitAtPlayhead(); return 1;
            }
        } else if (cmd) {
            switch (k) {
            case 'a': selectAll(); return 1;
            case 'z': if (Fl::event_shift()) redo(); else undo(); return 1;
            case 'x': cut(); return 1;
            case 'c': copy(); return 1;
            case 'v': paste(); return 1;
            case 'd': duplicate(); return 1;
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
