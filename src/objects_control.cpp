// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_control.cpp — timing, routing, control math, expr and interface objects

#include "objects.h"
#include "sysopen.h"
#include "zoom.h"

#include <map>
#include "expr.h"

#include <FL/fl_draw.H>

#include <cstdio>
#include <random>

namespace moritz {
namespace {

// ---------------------------------------------------------------- + - * / % == != > < >= <=
// Left inlet is hot (outputs), right inlet is cold (stores the operand).
// As in Max, the argument sets the type: [+ 2] or [+] works in integers
// (inputs are truncated, 7 / 2 is 3), [+ 2.] works in decimals (3.5).
using BinFn = double (*)(double, double, bool);   // a, b, integer mode
class CtlOpBox : public Box {
public:
    CtlOpBox(BinFn fn, const Message& a)
        : fn_(fn), int_(a.empty() || !a[0].isDecimal()), b_(in(argf(a, 0, 0.f))) {
        setPorts({P::Control, P::Control}, {P::Control});
    }
    void message(int inl, const Message& m) override {
        if (inl == 1) { if (m[0].isFloat()) b_ = in(m[0].f); return; }
        if (m[0].isFloat()) {
            a_ = in(m[0].f);
            if (m.size() > 1 && m[1].isFloat()) b_ = in(m[1].f);
        } else if (!m[0].is("bang")) {
            return;
        }
        outletFloat(0, (float)fn_(a_, b_, int_));
    }

private:
    double in(float v) const { return int_ ? std::trunc((double)v) : (double)v; }
    BinFn fn_;
    bool int_;
    double a_ = 0.0, b_;
};


// ---------------------------------------------------------------- send / receive
class SendBox : public Box {
public:
    explicit SendBox(const Message& a) {
        setPorts({P::Control}, {});
        if (!a.empty()) name_ = a[0].str();
    }
    void message(int, const Message& m) override {
        if (!name_.empty()) patch->sendTo(name_, m);
    }

private:
    std::string name_;
};

// ---------------------------------------------------------------- loadbang
class LoadbangBox : public Box {
public:
    LoadbangBox() { setPorts({P::Control}, {P::Control}); }
    void loadbang() override { outletBang(0); }
    void message(int, const Message& m) override { if (m[0].is("bang")) outletBang(0); }
};


// ================================================================ timing

// ---------------------------------------------------------------- metro
// metro ms: 1/bang starts, 0/stop stops; right inlet sets the interval.
// Ticks are scheduled on absolute times, so they don't drift.
class MetroBox : public Box {
public:
    explicit MetroBox(const Message& a) : interval_(std::max(1.f, argf(a, 0, 500.f))) {
        setPorts({P::Control, P::Control}, {P::Control});
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) interval_ = std::max(1.f, m[0].f); return; }
        if (m[0].is("bang") || m[0].is("start")) start();
        else if (m[0].is("stop")) stop();
        else if (m[0].isFloat()) m[0].f != 0.f ? start() : stop();
    }

private:
    void start() {
        patch->cancel(this);
        ++gen_;
        running_ = true;
        next_ = patch->time();
        tick();
    }
    void stop() {
        running_ = false;
        patch->cancel(this);
    }
    void tick() {
        int g = gen_;
        outletBang(0);
        if (!running_ || g != gen_) return;     // stopped or restarted downstream
        next_ += interval_ / 1000.0;
        double now = patch->logicalTime();      // a timer very late runs at the clock's time (Patch::runDue)
        if (next_ < now - 0.25) next_ = now + interval_ / 1000.0;   // fell far behind: on from now, no burst
        patch->scheduleAt(this, next_, [this] { tick(); });
    }
    float interval_;
    double next_ = 0.0;
    bool running_ = false;
    int gen_ = 0;
};

// ---------------------------------------------------------------- delay
// delay ms: bang outputs a bang after the delay (re-banging restarts it);
// a float in the left inlet sets the time and starts; stop cancels.
class DelayBox : public Box {
public:
    explicit DelayBox(const Message& a) : ms_(std::max(0.f, argf(a, 0, 0.f))) {
        setPorts({P::Control, P::Control}, {P::Control});
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) ms_ = std::max(0.f, m[0].f); return; }
        if (m[0].is("stop")) { patch->cancel(this); return; }
        if (m[0].isFloat()) ms_ = std::max(0.f, m[0].f);
        else if (!m[0].is("bang")) return;
        patch->cancel(this);
        patch->schedule(this, ms_ / 1000.0, [this] { outletBang(0); });
    }

private:
    float ms_;
};

// ---------------------------------------------------------------- line
// line: control-rate ramp. "target time" ramps, outputting every `grain`
// ms (right inlet, default 20); a float alone jumps (or ramps over the time
// set in the middle inlet, which then resets, as in Max).
class LineBox : public Box {
public:
    explicit LineBox(const Message& a)
        : value_(argf(a, 0, 0.f)), grain_(std::max(1.f, argf(a, 1, 20.f))) {
        setPorts({P::Control, P::Control, P::Control}, {P::Control});
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) time_ = m[0].f; return; }
        if (in == 2) { if (m[0].isFloat()) grain_ = std::max(1.f, m[0].f); return; }
        if (m[0].is("stop")) { patch->cancel(this); return; }
        if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) { patch->cancel(this); value_ = m[1].f; return; }
        if (!m[0].isFloat()) return;
        float target = m[0].f;
        float time = m.size() > 1 && m[1].isFloat() ? m[1].f : time_;
        if (m.size() > 2 && m[2].isFloat()) grain_ = std::max(1.f, m[2].f);
        time_ = 0.f;
        patch->cancel(this);
        if (time <= 0.f) { value_ = target; outletFloat(0, value_); return; }
        from_ = value_;
        to_ = target;
        t0_ = patch->time();
        dur_ = time / 1000.0;
        step();
    }

private:
    void step() {
        double k = std::min(1.0, (patch->time() - t0_) / dur_);
        value_ = (float)(from_ + (to_ - from_) * k);
        outletFloat(0, value_);
        if (k < 1.0) patch->schedule(this, grain_ / 1000.0, [this] { step(); });
    }
    float value_, grain_, time_ = 0.f, from_ = 0.f, to_ = 0.f;
    double t0_ = 0.0, dur_ = 1.0;
};


// ================================================================ interface

// ---------------------------------------------------------------- button
class ButtonBox : public Box {
public:
    ButtonBox() { kind = BoxKind::UI; setPorts({P::Control}, {P::Control}); }
    void uiSize(int& w, int& h) const override { w = 24; h = 24; }
    void message(int, const Message&) override { bang(); }
    void uiMouse(int e, int, int) override { if (e == FL_PUSH) bang(); }
    void drawUI(int X, int Y) override {
        bool on = flashUntil > now();
        fl_color(on ? kAccent : kNumFill);
        zd::pie(X + 5, Y + 5, w - 10, h - 10, 0, 360);
        fl_color(on ? kAccent : kCtlPort);
        zd::arc(X + 5, Y + 5, w - 10, h - 10, 0, 360);
    }

private:
    void bang() {
        flashUntil = now() + 0.15;
        patch->redraw();
        outletBang(0);
    }
};

// ---------------------------------------------------------------- link
// link [text] target: underlined text; a click opens the target with the
// system: a document (a PDF, relative to the patch, or bundled: "docs/..."),
// a folder, or a web address. A bang (any message but set) asks the user
// first (sysopen.h): a patch must not open things by itself.
class LinkBox : public Box {
public:
    explicit LinkBox(const Message& a) {
        kind = BoxKind::UI;
        setPorts({P::Control}, {});
        target_ = argstr(a, a.size() > 1 ? 1 : 0);
        text_ = a.size() > 1 ? argstr(a, 0) : target_;
    }
    void uiSize(int& w, int& h) const override {
        w = std::max(40, (int)(text_.size() * 6.6) + 18);
        h = 22;
    }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { target_ = m[1].str(); return; }
        open(false);
    }
    void uiMouse(int e, int, int) override { if (e == FL_PUSH) open(true); }   // the user clicked
    void drawUI(int X, int Y) override {
        zd::font(FL_HELVETICA, 12);
        fl_color(flashUntil > now() ? kText : fl_rgb_color(110, 165, 245));
        zd::draw(text_.c_str(), X + 9, Y + 15);
        int tw = (int)zd::width(text_.c_str());
        zd::xyline(X + 9, Y + 17, X + 9 + std::min(tw, w - 14));
    }

private:
    void open(bool clicked) {
        flashUntil = now() + 0.2;
        patch->redraw();
        bool url = target_.find("://") != std::string::npos || target_.rfind("mailto:", 0) == 0;
        std::string t = url ? target_ : patch->resolvePath(target_);
        if (!clicked) askToOpenWithSystem(patch, t, "link");
        else if (!openWithSystem(t)) patch->print("link: can't open " + target_);
    }
    std::string text_, target_;
};

// ---------------------------------------------------------------- toggle
class ToggleBox : public Box {
public:
    ToggleBox() { kind = BoxKind::UI; setPorts({P::Control}, {P::Control}); }
    void uiSize(int& w, int& h) const override { w = 24; h = 24; }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) set(!on_, true);
        else if (m[0].isFloat()) set(m[0].f != 0.f, true);
        else if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) set(m[1].f != 0.f, false);
    }
    void uiMouse(int e, int, int) override { if (e == FL_PUSH) set(!on_, true); }
    void drawUI(int X, int Y) override {
        if (!on_) return;
        fl_color(kAccent);
        zd::line_style(FL_SOLID | FL_CAP_ROUND, 2);
        zd::line(X + 7, Y + 7, X + w - 8, Y + h - 8);
        zd::line(X + w - 8, Y + 7, X + 7, Y + h - 8);
        zd::line_style(0);
    }

private:
    void set(bool v, bool out) {
        on_ = v;
        patch->redraw();
        if (out) outletFloat(0, on_ ? 1.f : 0.f);
    }
    bool on_ = false;
};

// ---------------------------------------------------------------- slider
// slider [min max]: horizontal; outputs integers when min and max are.
class SliderBox : public Box {
public:
    explicit SliderBox(const Message& a) {
        kind = BoxKind::UI;
        setPorts({P::Control}, {P::Control});
        lo_ = argf(a, 0, 0.f);
        hi_ = argf(a, 1, 127.f);
        if (hi_ == lo_) hi_ = lo_ + 1.f;
        // integers when both limits are written as integers (slider 0 127);
        // a limit written with a point (slider 0. 1.) gives decimals
        bool dec = (a.size() > 0 && a[0].isDecimal()) || (a.size() > 1 && a[1].isDecimal());
        int_ = !dec && isIntegral(lo_) && isIntegral(hi_);
        value_ = lo_;
    }
    void uiSize(int& w, int& h) const override { w = 140; h = 22; }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) outletFloat(0, value_);
        else if (m[0].isFloat()) set(m[0].f, true, true);      // a number always goes out, as in Max
        else if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) set(m[1].f, false);
    }
    void uiMouse(int e, int mx, int) override {
        if (e == FL_RELEASE) return;
        double k = std::clamp((mx - 8.0) / (w - 16.0), 0.0, 1.0);
        set((float)(lo_ + k * (hi_ - lo_)), true);             // dragging: only changes
    }
    void drawUI(int X, int Y) override {
        double k = std::clamp((value_ - lo_) / (hi_ - lo_), 0.f, 1.f);
        int tx = X + 8, tw = w - 16, ty = Y + h / 2;
        fl_color(fl_rgb_color(30, 31, 36));
        zd::rounded_rectf(tx, ty - 2, tw, 4, 2);
        fl_color(kAccent);
        zd::rounded_rectf(tx, ty - 2, std::max(2, (int)(tw * k)), 4, 2);
        int kx = tx + (int)(tw * k);
        fl_color(kText);
        zd::pie(kx - 6, ty - 6, 12, 12, 0, 360);
    }

private:
    void set(float v, bool out, bool always = false) {
        v = std::clamp(v, std::min(lo_, hi_), std::max(lo_, hi_));
        if (int_) v = std::round(v);
        bool changed = v != value_;
        value_ = v;
        patch->redraw();
        if (out && (changed || always)) outletFloat(0, value_);
    }
    float lo_, hi_, value_;
    bool int_;
};

// ---------------------------------------------------------------- kslider
// kslider [octaves] [lowest] [mode]: a piano keyboard (4 octaves from MIDI
// note 36 by default). Outlets: pitch and velocity, velocity first, as in
// Max. The velocity comes from where the key is clicked: louder lower down.
// Mode 0 (play): the mouse plays a note and releases it (velocity 0) when let
// go; dragging glides from key to key. Mode 1 (toggle): a click holds a key,
// a second click releases it, for chords.
// Inlets: a pitch (or "pitch velocity") lights the key and passes through,
// using the velocity last set in the right inlet (100 at first); velocity 0
// releases it. "set p v" only changes the display; "flush" releases every
// held key, sending its note-off.
class KsliderBox : public Box {
public:
    explicit KsliderBox(const Message& a) {
        kind = BoxKind::UI;
        setPorts({P::Control, P::Control}, {P::Control, P::Control});
        octaves_ = std::clamp((int)argf(a, 0, 4.f), 1, 8);
        lo_ = std::clamp((int)argf(a, 1, 36.f), 0, 127 - 12 * octaves_);
        toggle_ = argf(a, 2, 0.f) != 0.f;
        whites_ = 0;
        for (int n = lo_; n <= hi(); ++n) whites_ += !isBlack(n);
    }
    void uiSize(int& w, int& h) const override { w = whites_ * kKeyW + 2; h = kH; }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) vel_ = std::clamp((int)m[0].f, 0, 127); return; }
        if (m[0].isFloat()) {
            int p = std::clamp((int)m[0].f, 0, 127);
            int v = m.size() > 1 && m[1].isFloat() ? std::clamp((int)m[1].f, 0, 127) : vel_;
            v > 0 ? noteOn(p, v) : noteOff(p);
        } else if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) {
            int p = (int)m[1].f;
            if (argf(m, 2, (float)vel_) > 0.f) held_[p] = (int)argf(m, 2, (float)vel_);
            else held_.erase(p);
            patch->redraw();
        } else if (m[0].is("flush")) {
            auto keys = held_;
            for (auto& [p, v] : keys) noteOff(p);
            current_ = -1;
        } else if (m[0].is("clear")) {
            held_.clear();
            patch->redraw();
        }
    }
    void uiMouse(int e, int mx, int my) override {
        int vel = 0, p = keyAt(mx, my, vel);
        if (e == FL_PUSH) {
            if (p < 0) return;
            if (toggle_) { held_.count(p) ? noteOff(p) : noteOn(p, vel); return; }
            noteOn(p, vel);
            current_ = p;
        } else if (e == FL_DRAG && !toggle_) {
            if (p < 0 || p == current_) return;
            if (current_ >= 0) noteOff(current_);
            noteOn(p, vel);
            current_ = p;
        } else if (e == FL_RELEASE && !toggle_) {
            if (current_ >= 0) noteOff(current_);
            current_ = -1;
        }
    }
    void drawUI(int X, int Y) override {
        int x0 = X + 1, y0 = Y + 1, kh = h - 2;
        int wi = 0;
        for (int n = lo_; n <= hi(); ++n) {                 // white keys
            if (isBlack(n)) continue;
            int kx = x0 + wi++ * kKeyW;
            fl_color(held_.count(n) ? kAccent : fl_rgb_color(228, 229, 233));
            zd::rectf(kx, y0, kKeyW - 1, kh);
            if (n % 12 == 0 && !held_.count(n)) {          // C: a mark and the octave
                fl_color(fl_rgb_color(150, 152, 160));
                zd::font(kFont, 8);
                zd::draw(("C" + std::to_string(n / 12 - 1)).c_str(), kx + 1, y0 + kh - 3);
            }
        }
        for (int n = lo_; n <= hi(); ++n) {                 // black keys on top
            if (!isBlack(n)) continue;
            int bx, bw, bh;
            blackRect(n, bx, bw, bh);
            fl_color(held_.count(n) ? kAccent : fl_rgb_color(34, 35, 40));
            zd::rectf(x0 + bx, y0, bw, bh);
        }
    }

private:
    static constexpr int kKeyW = 12, kH = 56;
    static bool isBlack(int n) { int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }
    int hi() const { return lo_ + 12 * octaves_; }
    int whiteIndex(int n) const {                           // white keys below n
        int k = 0;
        for (int m = lo_; m < n; ++m) k += !isBlack(m);
        return k;
    }
    void blackRect(int n, int& bx, int& bw, int& bh) const {   // relative to the key area
        bw = 8;
        bh = (h - 2) * 3 / 5;
        bx = whiteIndex(n) * kKeyW - bw / 2;
    }
    // the key under (mx, my) in box coordinates, and the velocity there
    int keyAt(int mx, int my, int& vel) const {
        int x = mx - 1, y = my - 1, kh = h - 2;
        if (x < 0 || y < 0 || y >= kh || x >= whites_ * kKeyW) return -1;
        for (int n = lo_; n <= hi(); ++n) {
            if (!isBlack(n)) continue;
            int bx, bw, bh;
            blackRect(n, bx, bw, bh);
            if (x >= bx && x < bx + bw && y < bh) {
                vel = std::clamp(20 + 107 * y / std::max(1, bh - 1), 1, 127);
                return n;
            }
        }
        int wi = x / kKeyW;
        for (int n = lo_; n <= hi(); ++n)
            if (!isBlack(n) && wi-- == 0) {
                vel = std::clamp(20 + 107 * y / std::max(1, kh - 1), 1, 127);
                return n;
            }
        return -1;
    }
    void noteOn(int p, int v) {
        held_[p] = v;
        patch->redraw();
        outletFloat(1, (float)v);
        outletFloat(0, (float)p);
    }
    void noteOff(int p) {
        held_.erase(p);
        patch->redraw();
        outletFloat(1, 0.f);
        outletFloat(0, (float)p);
    }
    int octaves_, lo_, whites_ = 0;
    bool toggle_;
    int vel_ = 100, current_ = -1;
    std::map<int, int> held_;   // pitch -> velocity, lit on the keyboard
};

// ---------------------------------------------------------------- comment
class CommentBox : public Box {
public:
    explicit CommentBox(const Message& a) {
        kind = BoxKind::Comment;
        setPorts({}, {});
        content = messageToString(a);
    }
    std::string text() const override { return content; }
    Message saveArgs() const override { return {Atom(content)}; }
    std::string content;
};

// ---------------------------------------------------------------- print
class PrintBox : public Box {
public:
    explicit PrintBox(const Message& a) : name_(argstr(a, 0, "print")) { setPorts({P::Control}, {}); }
    void message(int, const Message& m) override { patch->print(name_ + ": " + messageToString(m)); }

private:
    std::string name_;
};



// ================================================================ routing

// ---------------------------------------------------------------- trigger / t
// trigger b f i l a s <constant>...: outputs right to left, converting.
class TriggerBox : public Box {
public:
    explicit TriggerBox(const Message& a) : types_(a) {
        if (types_.empty()) types_ = {Atom("b"), Atom("b")};
        setPorts({P::Control}, std::vector<Port>(types_.size(), P::Control));
    }
    void message(int, const Message& m) override {
        for (int k = (int)types_.size() - 1; k >= 0; --k) {
            const Atom& t = types_[(size_t)k];
            float f0 = m[0].isFloat() ? m[0].f : 0.f;
            if (t.isFloat()) outletFloat(k, t.f);
            else if (t.s == "b") outletBang(k);
            else if (t.s == "f") outletFloat(k, f0);
            else if (t.s == "i") outletFloat(k, std::trunc(f0));
            else if (t.s == "l" || t.s == "a") outlet(k, m);
            else if (t.s == "s") outlet(k, {Atom(m[0].str())});
            else outlet(k, {t});
        }
    }

private:
    Message types_;
};

// ---------------------------------------------------------------- pack / unpack
// ---------------------------------------------------------------- prepend, append
// prepend args...: puts its arguments before every message (prepend set:
// shows a list in a message box); append args...: after it. The right inlet
// replaces the arguments.
class PrependBox : public Box {
public:
    PrependBox(const Message& a, bool after) : args_(a), after_(after) { setPorts({P::Control, P::Control}, {P::Control}); }
    void message(int in, const Message& m) override {
        if (in == 1) { args_ = m; return; }
        Message out;
        const Message& body = (m.size() == 1 && m[0].is("bang")) ? Message{} : m;
        if (after_) { out = body; out.insert(out.end(), args_.begin(), args_.end()); }
        else { out = args_; out.insert(out.end(), body.begin(), body.end()); }
        if (!out.empty()) outlet(0, out);
    }

private:
    Message args_;
    bool after_;
};

class PackBox : public Box {
public:
    // As in Max: each argument sets a slot's type and initial value. An
    // integer (0, i) truncates what it receives, a decimal (0., f) keeps
    // it, a symbol (s, or any word) stores words. No arguments: two integers.
    explicit PackBox(const Message& a) {
        Message t = a.empty() ? Message{Atom(0.f), Atom(0.f)} : a;
        for (auto& x : t) {
            if (x.is("i")) { kind_.push_back('i'); vals_.push_back(Atom(0.f)); }
            else if (x.is("f")) { kind_.push_back('f'); vals_.push_back(Atom(0.f)); }
            else if (x.is("s")) { kind_.push_back('s'); vals_.push_back(Atom("")); }
            else if (x.isSymbol()) { kind_.push_back('s'); vals_.push_back(x); }
            else { kind_.push_back(x.dec ? 'f' : 'i'); vals_.push_back(Atom(x.f)); }
        }
        setPorts(std::vector<Port>(vals_.size(), P::Control), {P::Control});
    }
    void message(int in, const Message& m) override {
        if (in == 0) {
            if (m[0].is("bang")) { outlet(0, vals_); return; }
            if (m[0].is("set")) {
                for (size_t k = 1; k < m.size() && k - 1 < vals_.size(); ++k) vals_[k - 1] = store(k - 1, m[k]);
                return;
            }
            for (size_t k = 0; k < m.size() && k < vals_.size(); ++k) vals_[k] = store(k, m[k]);
            outlet(0, vals_);
        } else {
            for (size_t k = 0; k < m.size() && (size_t)in + k < vals_.size(); ++k)
                vals_[(size_t)in + k] = store((size_t)in + k, m[k]);
        }
    }

private:
    Atom store(size_t k, const Atom& x) const {
        if (kind_[k] == 's') return x;
        if (!x.isFloat()) return Atom(0.f);     // a word in a number slot is 0, as in Max
        return Atom(kind_[k] == 'i' ? std::trunc(x.f) : x.f);
    }
    std::string kind_;
    Message vals_;
};

class UnpackBox : public Box {
public:
    // Typed outlets as in Max: integers (0, i, the default) are truncated,
    // decimals (0., f) pass as they are, symbols (s) pass words.
    explicit UnpackBox(const Message& a) {
        Message t = a.empty() ? Message{Atom(0.f), Atom(0.f)} : a;
        for (auto& x : t)
            kind_ += x.is("f") || x.isDecimal() ? 'f' : x.is("s") || (x.isSymbol() && !x.is("i")) ? 's' : 'i';
        setPorts({P::Control}, std::vector<Port>(kind_.size(), P::Control));
    }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) return;
        for (int k = std::min(numOuts(), (int)m.size()) - 1; k >= 0; --k) {
            const Atom& x = m[(size_t)k];
            char t = kind_[(size_t)k];
            if (t == 's') outlet(k, {x});
            else if (!x.isFloat()) outletFloat(k, 0.f);
            else outletFloat(k, t == 'i' ? std::trunc(x.f) : x.f);
        }
    }

private:
    std::string kind_;
};

// ---------------------------------------------------------------- route
// route a b c: a message starting with "a" goes out outlet 0 without its
// first word (as a bang if nothing is left); no match: last outlet.
class RouteBox : public Box {
public:
    explicit RouteBox(const Message& a) : keys_(a) {
        if (keys_.empty()) keys_ = {Atom(0.f)};
        setPorts({P::Control}, std::vector<Port>(keys_.size() + 1, P::Control));
    }
    void message(int, const Message& m) override {
        for (size_t k = 0; k < keys_.size(); ++k)
            if (atomEq(m[0], keys_[k])) {
                Message rest = tailOf(m);
                if (rest.empty()) outletBang((int)k);
                else outlet((int)k, rest);
                return;
            }
        outlet((int)keys_.size(), m);
    }

private:
    Message keys_;
};

// ---------------------------------------------------------------- select / sel
// select a b: bangs outlet k when the input equals key k; else passes it
// out of the last outlet. With one key, the right inlet changes it.
class SelectBox : public Box {
public:
    explicit SelectBox(const Message& a) : keys_(a) {
        if (keys_.empty()) keys_ = {Atom(0.f)};
        setPorts(keys_.size() == 1 ? std::vector<Port>{P::Control, P::Control}
                                   : std::vector<Port>{P::Control},
                 std::vector<Port>(keys_.size() + 1, P::Control));
    }
    void message(int in, const Message& m) override {
        if (in == 1) { keys_[0] = m[0]; return; }
        for (size_t k = 0; k < keys_.size(); ++k)
            if (atomEq(m[0], keys_[k])) { outletBang((int)k); return; }
        outlet((int)keys_.size(), m);
    }

private:
    Message keys_;
};

// ---------------------------------------------------------------- gate
// gate [n [open]]: left inlet chooses the outlet (0 = closed), right inlet
// is the data.
class GateBox : public Box {
public:
    explicit GateBox(const Message& a) {
        n_ = std::clamp((int)argf(a, 0, 1.f), 1, 64);
        state_ = std::clamp((int)argf(a, 1, 0.f), 0, n_);
        setPorts({P::Control, P::Control}, std::vector<Port>((size_t)n_, P::Control));
    }
    void message(int in, const Message& m) override {
        if (in == 0) { if (m[0].isFloat()) state_ = std::clamp((int)m[0].f, 0, n_); return; }
        if (state_ > 0) outlet(state_ - 1, m);
    }

private:
    int n_, state_;
};

// ---------------------------------------------------------------- int / float
class StoreBox : public Box {
public:
    StoreBox(bool isInt, const Message& a) : int_(isInt) {
        setPorts({P::Control, P::Control}, {P::Control});
        v_ = fix(argf(a, 0, 0.f));
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) v_ = fix(m[0].f); return; }
        if (m[0].isFloat()) v_ = fix(m[0].f);
        else if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) { v_ = fix(m[1].f); return; }
        else if (!m[0].is("bang")) return;
        outletFloat(0, v_);
    }

private:
    float fix(float v) const { return int_ ? std::trunc(v) : v; }
    bool int_;
    float v_;
};

// ---------------------------------------------------------------- counter
// counter [max] | [min max]: each bang outputs the count, then steps it,
// wrapping from max back to min. Right inlet / "set n": next value.
class CounterBox : public Box {
public:
    explicit CounterBox(const Message& a) {
        setPorts({P::Control, P::Control}, {P::Control});
        if (a.size() >= 2) { min_ = (long)argf(a, 0, 0.f); max_ = (long)argf(a, 1, 0.f); }
        else if (a.size() == 1) max_ = (long)argf(a, 0, 0.f);
        if (max_ < min_) std::swap(min_, max_);
        if (max_ == min_ && a.empty()) max_ = 1L << 30;
        count_ = min_;
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) count_ = (long)m[0].f; return; }
        if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) { count_ = (long)m[1].f; return; }
        if (m[0].is("reset")) { count_ = min_; return; }
        if (!m[0].is("bang") && !m[0].isFloat()) return;
        long out = count_;
        count_ = count_ >= max_ ? min_ : count_ + 1;
        outletFloat(0, (float)out);
    }

private:
    long min_ = 0, max_ = 0, count_ = 0;
};

// ---------------------------------------------------------------- random
class RandomBox : public Box {
public:
    explicit RandomBox(const Message& a) : range_(std::max(1, (int)argf(a, 0, 2.f))) {
        setPorts({P::Control, P::Control}, {P::Control});
        rng_.seed(std::random_device{}());
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) range_ = std::max(1, (int)m[0].f); return; }
        if (m[0].is("seed") && m.size() > 1 && m[1].isFloat()) { rng_.seed((unsigned)m[1].f); return; }
        if (!m[0].is("bang")) return;
        outletFloat(0, (float)std::uniform_int_distribution<int>(0, range_ - 1)(rng_));
    }

private:
    int range_;
    std::mt19937 rng_;
};



// ================================================================ conversions

class UnaryCtlBox : public Box {
public:
    explicit UnaryCtlBox(double (*fn)(double)) : fn_(fn) { setPorts({P::Control}, {P::Control}); }
    void message(int, const Message& m) override {
        if (m[0].isFloat()) last_ = m[0].f;
        else if (!m[0].is("bang")) return;
        double r = fn_(last_);
        outletFloat(0, std::isfinite(r) ? (float)r : 0.f);
    }

private:
    double (*fn_)(double);
    float last_ = 0.f;
};

double mtof(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }
double ftom(double f) { return f > 0 ? 69.0 + 12.0 * std::log2(f / 440.0) : 0.0; }
double dbtoa(double db) { return std::pow(10.0, db / 20.0); }
double atodb(double a) { return a > 0 ? 20.0 * std::log10(a) : -999.0; }

// ---------------------------------------------------------------- scale
// scale inlow inhigh outlow outhigh [exp]: maps a number; exp > 1 curves it.
class ScaleBox : public Box {
public:
    explicit ScaleBox(const Message& a) {
        setPorts(std::vector<Port>(6, P::Control), {P::Control});
        const float def[5] = {0, 127, 0, 1, 1};
        for (int k = 0; k < 5; ++k) p_[k] = argf(a, (size_t)k, def[k]);
        // As in Max: four integer arguments give integer output
        int_ = a.size() == 4;
        for (auto& x : a) int_ = int_ && x.isFloat() && !x.dec;
    }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) p_[in - 1] = m[0].f; return; }
        if (m[0].isFloat()) x_ = m[0].f;
        else if (!m[0].is("bang")) return;
        double range = p_[1] - p_[0];
        double k = range == 0 ? 0 : (x_ - p_[0]) / range;
        if (p_[4] != 1.f) k = k >= 0 ? std::pow(k, p_[4]) : -std::pow(-k, p_[4]);
        double y = p_[2] + k * (p_[3] - p_[2]);
        outletFloat(0, (float)(int_ ? std::trunc(y) : y));
    }

private:
    bool int_ = false;
    float p_[5];
    float x_ = 0.f;
};



class ExprBox : public Box {
public:
    explicit ExprBox(const Message& a) : src_(messageToString(a)) {
        int nv = 0;
        ok_ = ExprParser(src_).parse(fn_, nv, err_);
        setPorts(std::vector<Port>((size_t)std::max(1, nv), P::Control), {P::Control});
    }
    Message saveArgs() const override { return {Atom(src_)}; }
    std::string text() const override { return "expr " + src_; }
    void attached() override {
        if (!ok_) patch->print("expr: " + err_ + " in \"" + src_ + "\"");
    }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) in_[in] = m[0].f; return; }
        if (m[0].isFloat())
            for (size_t k = 0; k < m.size() && k < 9; ++k)
                if (m[k].isFloat()) in_[k] = m[k].f;
        if (!m[0].isFloat() && !m[0].is("bang")) return;
        if (!ok_) return;
        double r = fn_(in_);
        outletFloat(0, std::isfinite(r) ? (float)r : 0.f);
    }

private:
    std::string src_, err_;
    bool ok_ = false;
    ExprParser::Fn fn_;
    double in_[9] = {0};
};



// ---------------------------------------------------------------- if
// if <condition> then <message> [else <message>], as in Max. The condition
// is an expr expression over $i1..$i9 and $f1..$f9 (one inlet each, the left
// one hot); the messages are written like message boxes and use the same
// variables, and $s1 for a word arriving at the left inlet. "out2" before a
// message sends it from a second outlet, "send name" to receive name.
class IfBox : public Box {
public:
    explicit IfBox(const Message& a) {
        int part = 0;                                // 0 condition, 1 then, 2 else
        std::string cond;
        for (auto& x : a) {
            if (x.is("then") && part == 0) { part = 1; continue; }
            if (x.is("else") && part == 1) { part = 2; continue; }
            if (part == 0) cond += (cond.empty() ? "" : " ") + x.str();
            else (part == 1 ? then_ : else_).push_back(x);
        }
        int nv = 0;
        if (part == 0) err_ = "if: missing \"then\"";
        else if (!ExprParser(cond).parse(fn_, nv, err_)) err_ = "if: " + err_;
        for (auto* m : {&then_, &else_})
            for (auto& x : *m)
                if (x.isSymbol() && x.s.size() == 3 && x.s[0] == '$' && std::isdigit((unsigned char)x.s[2]))
                    nv = std::max(nv, x.s[2] - '0');
        bool out2 = (!then_.empty() && then_[0].is("out2")) || (!else_.empty() && else_[0].is("out2"));
        setPorts(std::vector<Port>((size_t)std::clamp(nv, 1, 9), P::Control),
                 std::vector<Port>(out2 ? 2 : 1, P::Control));
    }
    void attached() override { if (!err_.empty()) patch->print(err_); }
    void message(int in, const Message& m) override {
        if (m[0].is("set")) {                        // store without evaluating
            for (size_t k = 1; k < m.size() && k - 1 < 9; ++k)
                if (m[k].isFloat()) v_[k - 1] = m[k].f;
            return;
        }
        if (in > 0) { if (m[0].isFloat() && in < 9) v_[in] = m[0].f; return; }
        if (m[0].isSymbol() && !m[0].is("bang")) {
            s1_ = m[0].s;
        } else {
            for (size_t k = 0; k < m.size() && k < 9; ++k)
                if (m[k].isFloat()) v_[k] = m[k].f;
        }
        if (!err_.empty()) return;
        const Message& part = fn_(v_) != 0.0 ? then_ : else_;
        if (part.empty()) return;
        size_t k = 0;
        int outl = 0;
        std::string sendTo;
        if (part[0].is("out2")) { outl = 1; k = 1; }
        if (k + 1 < part.size() && part[k].is("send")) { sendTo = part[k + 1].str(); k += 2; }
        Message out;
        for (; k < part.size(); ++k) {
            const Atom& x = part[k];
            if (x.isSymbol() && x.s.size() == 3 && x.s[0] == '$' && std::isdigit((unsigned char)x.s[2])) {
                int idx = x.s[2] - '1';
                char t = (char)std::tolower((unsigned char)x.s[1]);
                if (idx >= 0 && idx < 9 && t == 'i') { out.push_back(Atom((float)std::trunc(v_[idx]))); continue; }
                if (idx >= 0 && idx < 9 && t == 'f') { out.push_back(Atom((float)v_[idx])); continue; }
                if (idx == 0 && t == 's') { out.push_back(Atom(s1_)); continue; }
            }
            out.push_back(x);
        }
        if (out.empty()) return;
        if (!sendTo.empty()) patch->sendTo(sendTo, out);
        else outlet(outl, out);
    }

private:
    ExprParser::Fn fn_;
    Message then_, else_;
    std::string err_, s1_;
    double v_[9] = {0};
};

}  // namespace

void registerControlObjects(ClassTable& t) {
    using M = const Message&;
    wholeTextClasses().insert({"message", "comment", "expr"});   // their box text is one string
    auto ctlop = [](BinFn f) -> Factory {
        return [f](M a, Patch*) { return std::make_unique<CtlOpBox>(f, a); };
    };
    auto unary = [](double (*f)(double)) -> Factory {
        return [f](M, Patch*) { return std::make_unique<UnaryCtlBox>(f); };
    };
    Factory intBox = [](M a, Patch*) { return std::make_unique<StoreBox>(true, a); };
    Factory floatBox = [](M a, Patch*) { return std::make_unique<StoreBox>(false, a); };
    t.insert(t.end(), {
        {"metro", "Timing", "repeating bang (ms)", with<MetroBox>()},
        {"delay", "Timing", "delayed bang (ms)", with<DelayBox>()},
        {"line", "Timing", "control ramps: target time [grain]", with<LineBox>()},
        {"loadbang", "Timing", "bang when the patch opens", plain<LoadbangBox>()},
        {"+", "Control Math", "add (integers; [+ 0.] for decimals)", ctlop([](double a, double b, bool) { return a + b; })},
        {"-", "Control Math", "subtract (integers; [- 0.] for decimals)", ctlop([](double a, double b, bool) { return a - b; })},
        {"*", "Control Math", "multiply (integers; [* 0.] for decimals)", ctlop([](double a, double b, bool) { return a * b; })},
        {"/", "Control Math", "divide (integers; [/ 1.] for decimals)", ctlop([](double a, double b, bool i) {
             return b == 0 ? 0.0 : i ? std::trunc(a / b) : a / b; })},
        {"%", "Control Math", "remainder (integers; [% 1.] for decimals)", ctlop([](double a, double b, bool) {
             return b == 0 ? 0.0 : std::fmod(a, b); })},
        {"==", "Control Math", "equal: 1 or 0", ctlop([](double a, double b, bool) { return a == b ? 1.0 : 0.0; })},
        {"!=", "Control Math", "not equal: 1 or 0", ctlop([](double a, double b, bool) { return a != b ? 1.0 : 0.0; })},
        {">", "Control Math", "greater than: 1 or 0", ctlop([](double a, double b, bool) { return a > b ? 1.0 : 0.0; })},
        {"<", "Control Math", "less than: 1 or 0", ctlop([](double a, double b, bool) { return a < b ? 1.0 : 0.0; })},
        {">=", "Control Math", "greater or equal: 1 or 0", ctlop([](double a, double b, bool) { return a >= b ? 1.0 : 0.0; })},
        {"<=", "Control Math", "less or equal: 1 or 0", ctlop([](double a, double b, bool) { return a <= b ? 1.0 : 0.0; })},
        {"mtof", "Control Math", "MIDI note to frequency", unary(mtof)},
        {"ftom", "Control Math", "frequency to MIDI note", unary(ftom)},
        {"dbtoa", "Control Math", "decibels to amplitude", unary(dbtoa)},
        {"atodb", "Control Math", "amplitude to decibels", unary(atodb)},
        {"scale", "Control Math", "map a range: in lo hi, out lo hi", with<ScaleBox>()},
        {"expr", "Control Math", "expression: $f1 * 2 + sin($f2)", with<ExprBox>()},
        {"if", "Control Math", "if $i1 > 5 then ... else ...", with<IfBox>()},
        {"random", "Control Math", "random integer below range", with<RandomBox>()},
        {"counter", "Control Math", "count bangs: [min] max", with<CounterBox>()},
        {"trigger", "Routing", "ordered outputs, right to left", with<TriggerBox>()},
        {"pack", "Routing", "combine into a list", with<PackBox>()},
        {"unpack", "Routing", "split a list", with<UnpackBox>()},
        {"prepend", "Routing", "put words before a message: prepend set", [](const Message& a, Patch*) { return std::make_unique<PrependBox>(a, false); }},
        {"append", "Routing", "put words after a message", [](const Message& a, Patch*) { return std::make_unique<PrependBox>(a, true); }},
        {"route", "Routing", "dispatch by first word", with<RouteBox>()},
        {"select", "Routing", "bang on matching values", with<SelectBox>()},
        {"gate", "Routing", "open/close/switch a stream", with<GateBox>()},
        {"int", "Routing", "store an integer", intBox},
        {"float", "Routing", "store a float", floatBox},
        {"send", "Routing", "send to receive objects", with<SendBox>()},
        {"receive", "Routing", "receive sent messages", with<ReceiveBox>()},
        {"print", "Routing", "print to the console", with<PrintBox>()},
        {"message", "Interface", "clickable message", with<MessageBox>()},
        {"flonum", "Interface", "float number box", [](M a, Patch*) { return std::make_unique<NumberBox>(true, a); }},
        {"number", "Interface", "integer number box", [](M a, Patch*) { return std::make_unique<NumberBox>(false, a); }},
        {"button", "Interface", "click to bang", plain<ButtonBox>()},
        {"toggle", "Interface", "on/off switch", plain<ToggleBox>()},
        {"link", "Interface", "click to open a document, a folder or a web page: [text] target", with<LinkBox>()},
        {"slider", "Interface", "drag a value: [min max]", with<SliderBox>()},
        {"comment", "Interface", "text note", with<CommentBox>()},
        {"kslider", "Interface", "keyboard: [octaves] [lowest note] [mode 0 play, 1 toggle]", with<KsliderBox>()},
        // Max's short names
        {"t", "", "trigger (short name)", with<TriggerBox>()},
        {"sel", "", "select (short name)", with<SelectBox>()},
        {"del", "", "delay (short name)", with<DelayBox>()},
        {"i", "", "int (short name)", intBox},
        {"f", "", "float (short name)", floatBox},
        {"s", "", "send (short name)", with<SendBox>()},
        {"r", "", "receive (short name)", with<ReceiveBox>()},
    });
}

}  // namespace moritz
