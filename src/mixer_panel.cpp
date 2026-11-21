// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// mixer_panel.cpp — a session's mixer: strips for the tracks, the buses and the master (mixer_panel.h)

#include "mixer_panel.h"
#include "gui.h"
#include "multitrack_ops.h"
#include "timeline_ops.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

namespace moritz {

namespace tl = timeline;

namespace {
const Fl_Color kStripBg = fl_rgb_color(36, 38, 44);
const Fl_Color kBusBg = fl_rgb_color(40, 36, 48);
const Fl_Color kMasterBg = fl_rgb_color(44, 40, 34);
constexpr double kTopDb = 12, kBottomDb = -60;
std::string baseOf(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}
std::string panText(double az) {
    if (std::fabs(az) < 0.5) return "C";
    char b[32];
    std::snprintf(b, sizeof b, "%s%.0f", az > 0 ? "L" : "R", std::fabs(az));
    return b;
}
}  // namespace

MixerPanel::MixerPanel(int x, int y, int w, int h, std::shared_ptr<ScoreDoc> doc) : Fl_Widget(x, y, w, h), doc_(std::move(doc)) {
    Fl::add_timeout(1.0 / 30, tick, this);
}

MixerPanel::~MixerPanel() { Fl::remove_timeout(tick, this); }

void MixerPanel::tick(void* data) {
    auto* m = static_cast<MixerPanel*>(data);
    if (m->visible_r()) m->redraw();
    Fl::repeat_timeout(1.0 / 30, tick, data);
}

Box* MixerPanel::player() const { return doc_->playerBox(); }

void MixerPanel::layoutStrips() {
    const sc::Score& s = doc_->score();
    std::vector<Strip> out;
    std::vector<std::string> buses = mt::buses(s);
    auto add = [&](const std::string& n, bool bus, bool master) {
        Strip st;
        st.name = n;
        st.bus = bus;
        st.master = master;
        out.push_back(st);
    };
    for (auto& t : tl::tracks(s))
        if (std::find(buses.begin(), buses.end(), t) == buses.end()) add(t, false, false);
    for (auto& b : buses) add(b, true, false);
    add(mt::kMaster, false, true);
    int X = x() - scrollX;
    for (auto& st : out) {
        st.x = X;
        st.btnY = y() + 22;
        st.outY = y() + 42;
        st.insY = y() + 56;
        st.sendY = y() + 72;
        int sends = st.master ? 0 : (int)mt::settingsOf(s, st.name).sends.size();
        int after = st.sendY + (st.master ? 0 : 14 * (sends + 1));
        st.panY = after + 8;
        st.fy0 = st.panY + 18;
        st.fy1 = std::max(st.fy0 + 30, y() + h() - 22);
        X += kStripW;
    }
    strips_ = std::move(out);
    if (held_.size() != strips_.size()) held_.assign(strips_.size(), Held{});
}

double MixerPanel::gainOf(const std::string& name) const {
    if (drag_ == Drag::Fader && dragName_ == name) return dragValue_;
    if (name == mt::kMaster) return mt::sessionSettings(doc_->score()).masterGain;
    return tl::stripOf(doc_->score(), name).gain;
}

double MixerPanel::panOf(const std::string& name) const {
    if (drag_ == Drag::Pan && dragName_ == name) return dragValue_;
    if (name == mt::kMaster) return 0;
    return tl::stripOf(doc_->score(), name).az;
}

int MixerPanel::fyOf(const Strip& s, double gain) const {
    double db = std::clamp(mt::gainToDb(gain), kBottomDb, kTopDb);
    return s.fy1 - (int)std::lround((db - kBottomDb) / (kTopDb - kBottomDb) * (s.fy1 - s.fy0));
}

double MixerPanel::gainAt(const Strip& s, int y) const {
    double p = std::clamp((double)(s.fy1 - y) / std::max(1, s.fy1 - s.fy0), 0.0, 1.0);
    double db = kBottomDb + p * (kTopDb - kBottomDb);
    return p <= 0 ? 0.0 : mt::dbToGain(db);
}

void MixerPanel::draw() {
    layoutStrips();
    fl_push_clip(x(), y(), w(), h());
    fl_color(fl_rgb_color(26, 27, 31));
    fl_rectf(x(), y(), w(), h());
    const sc::Score& sco = doc_->score();
    Box* box = player();
    for (size_t k = 0; k < strips_.size(); ++k) {
        const Strip& s = strips_[k];
        if (s.x + kStripW < x() || s.x > x() + w()) continue;
        fl_color(s.master ? kMasterBg : s.bus ? kBusBg : kStripBg);
        fl_rectf(s.x + 1, y() + 1, kStripW - 2, h() - 2);
        int ti = tl::trackIndex(sco, s.name);
        static const Fl_Color colours[] = {fl_rgb_color(66, 186, 128), fl_rgb_color(232, 156, 36), fl_rgb_color(62, 128, 236),
                                           fl_rgb_color(150, 122, 232), fl_rgb_color(40, 190, 200), fl_rgb_color(220, 90, 120)};
        fl_color(s.master ? fl_rgb_color(200, 170, 90) : colours[(ti < 0 ? 0 : ti) % 6]);
        fl_rectf(s.x + 1, y() + 1, kStripW - 2, 3);
        fl_font(FL_HELVETICA_BOLD, 11);
        fl_color(kText);
        std::string name = s.master ? "master" : s.name.empty() ? "(no track)" : s.name;
        fl_push_clip(s.x + 4, y(), kStripW - 8, 20);
        fl_draw(name.c_str(), s.x + 6, y() + 16);
        fl_pop_clip();
        sc::Strip st = s.master ? sc::Strip{} : tl::stripOf(sco, s.name);
        mt::RowSettings rs = s.master ? mt::RowSettings{} : mt::settingsOf(sco, s.name);
        // M S R
        auto button = [&](int bx, const char* t, bool on, Fl_Color c) {
            fl_color(on ? c : fl_rgb_color(60, 63, 72));
            fl_rectf(bx, s.btnY, 24, 16);
            fl_color(on ? FL_BLACK : kText);
            fl_font(FL_HELVETICA_BOLD, 10);
            fl_draw(t, bx, s.btnY, 24, 16, FL_ALIGN_CENTER);
        };
        if (!s.master) {
            button(s.x + 6, "M", st.mute, fl_rgb_color(230, 170, 60));
            button(s.x + 34, "S", st.solo, fl_rgb_color(110, 200, 120));
            if (!s.bus) button(s.x + 62, "R", rs.arm, fl_rgb_color(230, 70, 70));
        }
        fl_font(FL_HELVETICA, 10);
        fl_color(kDim);
        if (!s.master) {
            std::string out = "→ " + (rs.out.empty() ? std::string("master") : rs.out);
            fl_draw(out.c_str(), s.x + 6, s.outY + 10);
        }
        std::string ins = s.master ? mt::sessionSettings(sco).masterInsert : rs.insert;
        fl_color(ins.empty() ? kDim : fl_rgb_color(150, 190, 250));
        std::string it = "fx: " + (ins.empty() ? std::string("none") : baseOf(ins));
        fl_push_clip(s.x + 4, s.insY, kStripW - 8, 14);
        fl_draw(it.c_str(), s.x + 6, s.insY + 10);
        fl_pop_clip();
        if (!s.master) {
            for (size_t j = 0; j < rs.sends.size(); ++j) {
                int sy = s.sendY + 14 * (int)j;
                double g = rs.sends[j].gain;
                if (drag_ == Drag::Send && dragName_ == s.name && dragSend_ == (int)j) g = dragValue_;
                fl_color(fl_rgb_color(50, 52, 60));
                fl_rectf(s.x + 6, sy + 2, kStripW - 12, 10);
                double p = std::clamp((mt::gainToDb(g) - kBottomDb) / (kTopDb - kBottomDb), 0.0, 1.0);
                fl_color(fl_rgb_color(120, 110, 200));
                fl_rectf(s.x + 6, sy + 2, (int)((kStripW - 12) * p), 10);
                fl_color(kText);
                fl_font(FL_HELVETICA, 9);
                std::string t = rs.sends[j].bus + " " + mt::dbText(g);
                fl_draw(t.c_str(), s.x + 8, sy + 11);
            }
            fl_color(kDim);
            fl_font(FL_HELVETICA, 9);
            fl_draw("+ send", s.x + 6, s.sendY + 14 * (int)rs.sends.size() + 11);
        }
        // the pan
        double az = panOf(s.name);
        if (!s.master) {
            int px0 = s.x + 8, px1 = s.x + kStripW - 8;
            fl_color(fl_rgb_color(70, 74, 84));
            fl_xyline(px0, s.panY, px1);
            int px = px0 + (int)std::lround((90 - std::clamp(az, -90.0, 90.0)) / 180 * (px1 - px0));
            fl_color(kAccent);
            fl_rectf(px - 3, s.panY - 5, 7, 11);
            fl_color(kDim);
            fl_font(FL_HELVETICA, 9);
            fl_draw(panText(az).c_str(), s.x + kStripW - 30, s.panY - 6);
        }
        // the fader and the meter
        int fx = s.x + 22;
        fl_color(fl_rgb_color(18, 19, 22));
        fl_rectf(fx - 2, s.fy0, 5, s.fy1 - s.fy0);
        fl_color(fl_rgb_color(60, 63, 72));
        for (double db : {12.0, 0.0, -12.0, -24.0, -48.0}) {
            int ty = s.fy1 - (int)std::lround((db - kBottomDb) / (kTopDb - kBottomDb) * (s.fy1 - s.fy0));
            fl_xyline(fx - 8, ty, fx - 4);
        }
        double g = gainOf(s.name);
        int fy = fyOf(s, g);
        fl_color(fl_rgb_color(210, 212, 220));
        fl_rectf(fx - 10, fy - 5, 21, 10);
        fl_color(fl_rgb_color(60, 60, 70));
        fl_xyline(fx - 8, fy, fx + 8);
        // meter: RMS (green) under the peak's tick
        mt::StripParams* p = box ? mt::stripOf(box, s.name) : nullptr;
        Held& hd = held_[k];
        for (int c = 0; c < 2; ++c) {
            float pk = p ? p->takePeak(c) : 0.f, rms = p ? p->rms[c].load(std::memory_order_relaxed) : 0.f;
            hd.peak[c] = std::max(pk, hd.peak[c] * 0.88f);
            int mx = s.x + 50 + 9 * c;
            fl_color(fl_rgb_color(18, 19, 22));
            fl_rectf(mx, s.fy0, 7, s.fy1 - s.fy0);
            auto yOf = [&](float v) { return fyOf(s, v); };
            int ry = yOf(rms);
            fl_color(fl_rgb_color(70, 190, 110));
            if (rms > 1e-6f) fl_rectf(mx, ry, 7, s.fy1 - ry);
            if (hd.peak[c] > 1e-6f) {
                fl_color(hd.peak[c] >= 1.f ? fl_rgb_color(240, 60, 60) : fl_rgb_color(230, 220, 120));
                fl_xyline(mx, yOf(hd.peak[c]), mx + 6);
            }
        }
        fl_color(kText);
        fl_font(FL_HELVETICA, 10);
        fl_draw((mt::dbText(g) + " dB").c_str(), s.x + 6, s.fy1 + 14);
        fl_color(kGrid);
        fl_yxline(s.x + kStripW - 1, y(), y() + h());
    }
    fl_pop_clip();
}

int MixerPanel::handle(int e) {
    switch (e) {
    case FL_PUSH:
        press(Fl::event_x(), Fl::event_y(), Fl::event_clicks());
        return 1;
    case FL_DRAG:
        dragTo(Fl::event_x(), Fl::event_y());
        return 1;
    case FL_RELEASE:
        release(Fl::event_x(), Fl::event_y());
        return 1;
    case FL_MOUSEWHEEL: {
        int total = (int)strips_.size() * kStripW;
        scrollX = std::clamp(scrollX + 30 * (Fl::event_dx() + Fl::event_dy()), 0, std::max(0, total - w()));
        redraw();
        return 1;
    }
    default:
        return Fl_Widget::handle(e);
    }
}

void MixerPanel::press(int ex, int ey, int clicks) {
    layoutStrips();
    drag_ = Drag::None;
    pressX_ = ex;
    pressY_ = ey;
    const Strip* s = nullptr;
    for (auto& st : strips_)
        if (ex >= st.x && ex < st.x + kStripW) s = &st;
    if (!s) return;
    ScoreDoc& d = *doc_;
    auto status = [&](const std::string& m) { if (onStatus) onStatus(m); };
    // the name
    if (ey < y() + 20) {
        if (!s->master && onTrack) onTrack(s->name);
        return;
    }
    // M S R
    if (!s->master && ey >= s->btnY && ey < s->btnY + 16) {
        sc::Strip st = tl::stripOf(d.score(), s->name);
        if (ex >= s->x + 6 && ex < s->x + 30) { st.mute = !st.mute; tl::setStrip(d, s->name, st); }
        else if (ex >= s->x + 34 && ex < s->x + 58) { st.solo = !st.solo; tl::setStrip(d, s->name, st); }
        else if (!s->bus && ex >= s->x + 62 && ex < s->x + 86) mt::setArm(d, s->name, !mt::settingsOf(d.score(), s->name).arm);
        status(d.message);
        redraw();
        return;
    }
    // where it goes
    if (!s->master && ey >= s->outY && ey < s->outY + 14) {
        std::vector<std::string> opts{""};
        for (auto& b : mt::buses(d.score()))
            if (b != s->name) opts.push_back(b);
        std::vector<Fl_Menu_Item> items(opts.size() + 1);
        for (auto& it : items) it = Fl_Menu_Item{};
        for (size_t k = 0; k < opts.size(); ++k) items[k].label(k == 0 ? "master" : opts[k].c_str());
        if (const Fl_Menu_Item* m = items[0].popup(ex, ey)) {
            size_t k = (size_t)(m - &items[0]);
            if (!mt::setOutput(d, s->name, opts[k])) status(d.message);
        }
        redraw();
        return;
    }
    // its insert: the drawer shows it (a click), or a choice (the menu)
    if (ey >= s->insY && ey < s->insY + 14) {
        Fl_Menu_Item items[] = {{"Show in the Drawer", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                                {"Choose an Insert Patch...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                                {"No Insert", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                                {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
        const Fl_Menu_Item* m = items[0].popup(ex, ey);
        if (m == &items[0]) { if (onInsert) onInsert(s->name); }
        else if (m == &items[1] || m == &items[2]) {
            std::string f = m == &items[1] ? chooseFile(false, "An insert patch (inlet~ to outlet~)", "Patch\t*.moritz") : std::string();
            if (m == &items[1] && f.empty()) return;
            if (s->master) {
                mt::SessionSettings v = mt::sessionSettings(d.score());
                v.masterInsert = f.empty() ? std::string() : tl::sourceName(d, f);
                mt::setSession(d, v, "Master Insert");
            } else {
                mt::setTrackPatch(d, s->name, "insert", f);
            }
            status(d.message);
        }
        redraw();
        return;
    }
    // the sends: a level dragged, + adds one
    if (!s->master && ey >= s->sendY) {
        auto rs = mt::settingsOf(d.score(), s->name);
        int j = (ey - s->sendY) / 14;
        if (j < (int)rs.sends.size()) {
            drag_ = Drag::Send;
            dragName_ = s->name;
            dragSend_ = j;
            dragStart_ = dragValue_ = rs.sends[(size_t)j].gain;
            return;
        }
        if (j == (int)rs.sends.size()) {
            std::vector<std::string> opts;
            for (auto& b : mt::buses(d.score()))
                if (b != s->name) opts.push_back(b);
            if (opts.empty()) { status("no bus to send to: Add > Bus in the timeline"); return; }
            std::vector<Fl_Menu_Item> items(opts.size() + 1);
            for (auto& it : items) it = Fl_Menu_Item{};
            for (size_t k = 0; k < opts.size(); ++k) items[k].label(opts[k].c_str());
            if (const Fl_Menu_Item* m = items[0].popup(ex, ey))
                if (!mt::setSend(d, s->name, opts[(size_t)(m - &items[0])], mt::dbToGain(-6))) status(d.message);
            redraw();
            return;
        }
    }
    // the pan
    if (!s->master && std::abs(ey - s->panY) <= 7) {
        if (clicks) { commitPan(s->name, 0); return; }
        dragStart_ = dragValue_ = panOf(s->name);           // (before the drag is said to be going on)
        drag_ = Drag::Pan;
        dragName_ = s->name;
        dragTo(ex, ey);
        return;
    }
    // the fader
    if (ey >= s->fy0 - 6 && ey <= s->fy1 + 6) {
        if (clicks) { commitGain(s->name, 1.0); return; }
        dragStart_ = dragValue_ = gainOf(s->name);
        drag_ = Drag::Fader;
        dragName_ = s->name;
        return;
    }
}

void MixerPanel::dragTo(int ex, int ey) {
    if (drag_ == Drag::None) return;
    const Strip* s = nullptr;
    for (auto& st : strips_)
        if (st.name == dragName_) s = &st;
    if (!s) return;
    Box* box = player();
    mt::StripParams* p = box ? mt::stripOf(box, dragName_) : nullptr;
    if (drag_ == Drag::Fader) {
        // relative to where it was pressed: the fader does not jump to the mouse
        dragValue_ = gainAt(*s, fyOf(*s, dragStart_) + (ey - pressY_));
        if (p) p->gain.store((float)dragValue_, std::memory_order_relaxed);   // heard at once, no plan made
        if (onStatus) onStatus(std::string(dragName_ == mt::kMaster ? "master" : dragName_) + ": " + mt::dbText(dragValue_) + " dB");
    } else if (drag_ == Drag::Pan) {
        int px0 = s->x + 8, px1 = s->x + kStripW - 8;
        dragValue_ = std::clamp(90 - 180.0 * (ex - px0) / std::max(1, px1 - px0), -90.0, 90.0);
        if (std::fabs(dragValue_) < 3) dragValue_ = 0;
        if (p) p->az.store((float)dragValue_, std::memory_order_relaxed);
        if (onStatus) onStatus(dragName_ + ": pan " + panText(dragValue_));
    } else if (drag_ == Drag::Send) {
        double p0 = std::clamp((mt::gainToDb(dragStart_) - kBottomDb) / (kTopDb - kBottomDb), 0.0, 1.0);
        double q = std::clamp(p0 + (double)(ex - pressX_) / (kStripW - 12), 0.0, 1.0);
        dragValue_ = q <= 0 ? 0.0 : mt::dbToGain(kBottomDb + q * (kTopDb - kBottomDb));
        if (p && dragSend_ >= 0 && dragSend_ < mt::kMaxSends) p->send[dragSend_].store((float)dragValue_, std::memory_order_relaxed);
        if (onStatus) onStatus(dragName_ + ": send " + mt::dbText(dragValue_) + " dB");
    }
    redraw();
}

void MixerPanel::release(int ex, int ey) {
    dragTo(ex, ey);
    Drag was = drag_;
    drag_ = Drag::None;
    if (was == Drag::Fader) commitGain(dragName_, dragValue_);
    else if (was == Drag::Pan) commitPan(dragName_, dragValue_);
    else if (was == Drag::Send) {
        auto rs = mt::settingsOf(doc_->score(), dragName_);
        if (dragSend_ >= 0 && dragSend_ < (int)rs.sends.size() && dragValue_ != dragStart_) {
            if (dragValue_ <= 0) dragValue_ = 1e-6;      // a send dragged to nothing stays (removed from its menu)
            mt::setSend(*doc_, dragName_, rs.sends[(size_t)dragSend_].bus, dragValue_);
        }
    }
    redraw();
}

void MixerPanel::commitGain(const std::string& name, double gain) {
    ScoreDoc& d = *doc_;
    if (name == mt::kMaster) {
        mt::SessionSettings v = mt::sessionSettings(d.score());
        if (v.masterGain == gain) return;
        v.masterGain = gain;
        mt::setSession(d, v, "Master Gain");
        if (Box* b = player()) mt::stripOf(b, name)->gain.store((float)gain);
    } else {
        sc::Strip st = tl::stripOf(d.score(), name);
        if (st.gain == gain) return;
        st.gain = gain;
        tl::setStrip(d, name, st);
    }
    redraw();
}

void MixerPanel::commitPan(const std::string& name, double az) {
    ScoreDoc& d = *doc_;
    sc::Strip st = tl::stripOf(d.score(), name);
    if (st.az == az) return;
    st.az = az;
    tl::setStrip(d, name, st);
    redraw();
}

std::string MixerPanel::describe(const std::string& name) const {
    const sc::Score& s = doc_->score();
    std::string o = "strip " + std::string(name == mt::kMaster ? "master" : name) + " gain " + mt::dbText(gainOf(name));
    if (name == mt::kMaster) return o;
    sc::Strip st = tl::stripOf(s, name);
    mt::RowSettings rs = mt::settingsOf(s, name);
    o += " pan " + panText(panOf(name)) + " mute " + (st.mute ? "yes" : "no") + " solo " + (st.solo ? "yes" : "no") + " arm " +
         (rs.arm ? "yes" : "no") + " out " + (rs.out.empty() ? std::string("master") : rs.out);
    for (auto& x : rs.sends) o += " send " + x.bus + " " + mt::dbText(x.gain);
    if (!rs.insert.empty()) o += " insert " + baseOf(rs.insert);
    if (Box* b = doc_->playerBox())
        if (mt::StripParams* p = mt::stripOf(b, name)) o += " live " + mt::dbText(p->gain.load());
    return o;
}

}  // namespace moritz
