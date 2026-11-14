// minlsp.cpp — a minimal Max/MSP-like patcher
//
// Sections
//   1. atoms, messages, s-expressions
//   2. box classes (cycle~, reson~, noise~, click~, +~ ... dac~)
//   3. patch: editing, signal-graph compilation, file format, processing
//   4. audio engine (miniaudio)
//   5. GUI: canvas and main window (FLTK)
//   6. main, including a headless `--render` mode for testing

#include "minlsp.h"

#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/filename.H>
#include <FL/platform.H>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <set>
#include <sstream>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef MINLSP_VERSION
#define MINLSP_VERSION "0.1"
#endif

namespace mlsp {

static double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
// 1. atoms, messages, s-expressions
// ============================================================================

std::string formatFloat(float v) {
    char buf[40];
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e9f)
        std::snprintf(buf, sizeof buf, "%lld", (long long)v);
    else
        std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

std::string Atom::str() const { return type == Float ? formatFloat(f) : s; }

Atom parseAtom(const std::string& tok) {
    if (tok.empty()) return Atom(tok);
    char c = tok[0];
    bool numeric = std::isdigit((unsigned char)c) ||
                   ((c == '-' || c == '+' || c == '.') && tok.size() > 1);
    if (numeric) {
        char* end = nullptr;
        float v = std::strtof(tok.c_str(), &end);
        if (end && *end == '\0') return Atom(v);
    }
    return Atom(tok);
}

std::vector<std::string> splitTokens(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
    for (char c : text) {
        if (std::isspace((unsigned char)c)) flush();
        else if (c == ';' || c == ',') { flush(); out.push_back(std::string(1, c)); }
        else cur += c;
    }
    flush();
    return out;
}

Message parseMessage(const std::string& text) {
    Message m;
    for (auto& t : splitTokens(text)) m.push_back(parseAtom(t));
    return m;
}

std::string messageToString(const Message& m) {
    std::string s;
    for (size_t i = 0; i < m.size(); ++i) {
        if (i && !m[i].is(",")) s += ' ';
        s += m[i].str();
    }
    return s;
}

bool parseSExprs(const std::string& src, std::vector<SExpr>& out, std::string& err) {
    std::vector<SExpr> stack(1);
    stack[0].isList = true;
    int line = 1;
    size_t i = 0;
    auto fail = [&](const std::string& what) {
        err = "line " + std::to_string(line) + ": " + what;
        return false;
    };
    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == ';') {                                   // comment
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (c == '(') {
            SExpr e;
            e.isList = true;
            e.line = line;
            stack.push_back(std::move(e));
            ++i;
            continue;
        }
        if (c == ')') {
            if (stack.size() < 2) return fail("unexpected ')'");
            SExpr e = std::move(stack.back());
            stack.pop_back();
            stack.back().items.push_back(std::move(e));
            ++i;
            continue;
        }
        SExpr a;
        a.line = line;
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < src.size()) {
                char d = src[i++];
                if (d == '\\' && i < src.size()) {
                    char e = src[i++];
                    a.text += (e == 'n') ? '\n' : e;
                } else if (d == '"') {
                    closed = true;
                    break;
                } else {
                    if (d == '\n') ++line;
                    a.text += d;
                }
            }
            if (!closed) return fail("unterminated string");
            a.quoted = true;
        } else {
            size_t j = i;
            while (j < src.size() && !std::isspace((unsigned char)src[j]) && src[j] != '(' &&
                   src[j] != ')' && src[j] != '"' && src[j] != ';')
                ++j;
            a.text = src.substr(i, j - i);
            i = j;
        }
        stack.back().items.push_back(std::move(a));
    }
    if (stack.size() != 1) return fail("missing ')'");
    out = std::move(stack[0].items);
    return true;
}

static Atom sexprAtom(const SExpr& e) { return e.quoted ? Atom(e.text) : parseAtom(e.text); }

static std::string quoteSymbol(const std::string& s) {
    bool need = s.empty();
    for (char c : s)
        if (std::isspace((unsigned char)c) || c == '(' || c == ')' || c == '"' || c == ';' ||
            c == '\\')
            need = true;
    if (!need && parseAtom(s).isFloat()) need = true;   // keep "440" a symbol
    if (!need) return s;
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        if (c == '\n') { r += "\\n"; continue; }
        r += c;
    }
    return r + "\"";
}

static std::string writeAtom(const Atom& a) {
    return a.isFloat() ? formatFloat(a.f) : quoteSymbol(a.s);
}

// ============================================================================
// 2. boxes
// ============================================================================

bool Box::isSignal() const {
    for (auto p : ins) if (p == Port::Signal) return true;
    for (auto p : outs) if (p == Port::Signal) return true;
    return false;
}

std::string Box::text() const {
    std::string t = cls;
    for (auto& a : args) t += (t.empty() ? "" : " ") + a.str();
    return t;
}

void Box::setPorts(std::vector<Port> in, std::vector<Port> out) {
    ins = std::move(in);
    outs = std::move(out);
    outBuf_.assign(outs.size(), std::vector<float>(dsp::kBlock, 0.f));
    inBuf_.assign(ins.size(), std::vector<float>(dsp::kBlock, 0.f));
    scalar_.reset(new std::atomic<float>[ins.size() ? ins.size() : 1]);
    for (size_t i = 0; i < ins.size(); ++i) scalar_[i].store(0.f);
    inSrc.assign(ins.size(), {});
}

const float* Box::sigIn(int i, int n) {
    auto& src = inSrc[i];
    float* buf = inBuf_[i].data();
    if (src.empty()) {
        dsp::fill(buf, scalar(i), n);
        return buf;
    }
    if (src.size() == 1) return src[0];
    std::copy(src[0], src[0] + n, buf);              // fan-in sums, as in MSP
    for (size_t k = 1; k < src.size(); ++k)
        for (int j = 0; j < n; ++j) buf[j] += src[k][j];
    return buf;
}

void Box::outlet(int i, const Message& m) {
    if (!patch || m.empty()) return;
    if (patch->depth > 256) {
        patch->status("stack overflow: message loop stopped at " + id);
        return;
    }
    // Like Max: fan-out is delivered right-to-left by destination position.
    std::vector<std::pair<Box*, int>> targets;
    for (auto& c : patch->conns)
        if (c.from == this && c.outlet == i) targets.push_back({c.to, c.inlet});
    std::stable_sort(targets.begin(), targets.end(),
                     [](auto& a, auto& b) { return a.first->x > b.first->x; });
    ++patch->depth;
    for (auto& t : targets) t.first->message(t.second, m);
    --patch->depth;
}

// ---- look (shared by the canvas and the interface objects)
constexpr int kPortW = 8, kPortH = 3, kBoxH = 22, kFontSize = 12, kRadius = 6;
constexpr int kPortInset = 4;            // keeps ports clear of the rounded corners
constexpr int kSuggRowH = 20, kSuggMax = 8;
constexpr Fl_Font kFont = FL_HELVETICA;

// dark theme
const Fl_Color kBgLocked  = fl_rgb_color(34, 35, 39);
const Fl_Color kBgEdit    = fl_rgb_color(28, 29, 33);
const Fl_Color kGrid      = fl_rgb_color(50, 52, 58);
const Fl_Color kBoxFill   = fl_rgb_color(54, 57, 64);
const Fl_Color kNumFill   = fl_rgb_color(44, 47, 54);
const Fl_Color kMsgFill   = fl_rgb_color(76, 80, 90);
const Fl_Color kFlash     = fl_rgb_color(70, 130, 220);
const Fl_Color kBroken    = fl_rgb_color(78, 40, 44);
const Fl_Color kBorder    = fl_rgb_color(92, 96, 108);
const Fl_Color kText      = fl_rgb_color(226, 228, 234);
const Fl_Color kDim       = fl_rgb_color(140, 144, 156);
const Fl_Color kAccent    = fl_rgb_color(110, 170, 255);
const Fl_Color kSelect    = fl_rgb_color(90, 160, 255);
const Fl_Color kSignal    = fl_rgb_color(236, 196, 70);
const Fl_Color kSignalDim = fl_rgb_color(120, 96, 28);
const Fl_Color kCtlPort   = fl_rgb_color(196, 200, 210);
const Fl_Color kCable     = fl_rgb_color(160, 164, 176);
const Fl_Color kPopup     = fl_rgb_color(44, 46, 53);
const Fl_Color kStatusBg  = fl_rgb_color(22, 23, 26);
const Fl_Color kEditBg    = fl_rgb_color(78, 84, 98);   // box being typed into

namespace {

using P = Port;

float argf(const Message& a, size_t i, float def) {
    return i < a.size() && a[i].isFloat() ? a[i].f : def;
}

// ---------------------------------------------------------------- broken
class BrokenBox : public Box {
public:
    BrokenBox() { kind = BoxKind::Broken; setPorts({}, {}); }
};

// ---------------------------------------------------------------- cycle~
// cycle~ [freq]: inlet 0 frequency (signal or float), inlet 1 phase (0..1)
class CycleBox : public Box {
public:
    explicit CycleBox(const Message& a) {
        setPorts({P::Signal, P::Control}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
        osc_.setPhase(argf(a, 1, 0.f));
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 0) setScalar(0, m[0].f);
        else osc_.setPhase(m[0].f);
    }
    void dsp(int n) override { osc_.process(sigIn(0, n), sigOut(0), n, patch->sampleRate); }

private:
    dsp::Cycle osc_;
};

// ---------------------------------------------------------------- reson~
// reson~ [gain cf Q]: inlets signal in, gain, centre frequency, Q.
// A list "gain cf Q" in the left inlet sets all three.
class ResonBox : public Box {
public:
    explicit ResonBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, 1.f));
        setScalar(2, argf(a, 1, 1000.f));
        setScalar(3, argf(a, 2, 10.f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->dspMutex); f_.clear(); return; }
        if (in == 0 && m.size() >= 2) {
            for (size_t k = 0; k < m.size() && k < 3; ++k)
                if (m[k].isFloat()) setScalar(1 + (int)k, m[k].f);
            return;
        }
        if (in > 0 && m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float g = sigIn(1, n)[0], cf = sigIn(2, n)[0], q = sigIn(3, n)[0];
        f_.process(x, sigOut(0), n, g, cf, q, patch->sampleRate);
    }

private:
    dsp::Reson f_;
};

// ---------------------------------------------------------------- noise~
class NoiseBox : public Box {
public:
    NoiseBox() : noise_(seed()) { setPorts({P::Control}, {P::Signal}); }
    void dsp(int n) override { noise_.process(sigOut(0), n); }

private:
    static uint32_t seed() {
        static uint32_t s = 0x12345678u;
        s = s * 1664525u + 1013904223u;
        return s;
    }
    dsp::Noise noise_;
};

// ---------------------------------------------------------------- click~
// click~: bang outputs an impulse; a list of floats ("set" optional) sets
// the impulse shape.
class ClickBox : public Box {
public:
    explicit ClickBox(const Message& a) {
        setPorts({P::Control}, {P::Signal});
        if (!a.empty()) setShape(a);
    }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) { click_.trigger(); return; }
        setShape(m[0].is("set") ? Message(m.begin() + 1, m.end()) : m);
    }
    void dsp(int n) override { click_.process(sigOut(0), n); }

private:
    void setShape(const Message& m) {
        std::vector<float> s;
        for (auto& a : m) if (a.isFloat()) s.push_back(a.f);
        std::lock_guard<std::mutex> lk(patch ? patch->dspMutex : dummy_);
        click_.setShape(std::move(s));
    }
    std::mutex dummy_;
    dsp::Click click_;
};

// ---------------------------------------------------------------- +~ -~ *~ /~
class SigOpBox : public Box {
public:
    SigOpBox(dsp::Op op, const Message& a) : op_(op) {
        setPorts({P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, 0.f));
    }
    void message(int in, const Message& m) override {
        if (m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override { dsp::binop(op_, sigIn(0, n), sigIn(1, n), sigOut(0), n); }

private:
    dsp::Op op_;
};

// ---------------------------------------------------------------- + - * / % == != > < >= <=
// Left inlet is hot (outputs), right inlet is cold (stores the operand).
using BinFn = float (*)(float, float);
class CtlOpBox : public Box {
public:
    CtlOpBox(BinFn fn, const Message& a) : fn_(fn), b_(argf(a, 0, 0.f)) {
        setPorts({P::Control, P::Control}, {P::Control});
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) b_ = m[0].f; return; }
        if (m[0].isFloat()) {
            a_ = m[0].f;
            if (m.size() > 1 && m[1].isFloat()) b_ = m[1].f;
        } else if (!m[0].is("bang")) {
            return;
        }
        outletFloat(0, fn_(a_, b_));
    }

private:
    BinFn fn_;
    float a_ = 0.f, b_;
};

// ---------------------------------------------------------------- flonum / number
class NumberBox : public Box {
public:
    NumberBox(bool isFloat, const Message& a) : isFloat_(isFloat) {
        kind = BoxKind::Number;
        setPorts({P::Control}, {P::Control});
        value_ = fix(argf(a, 0, 0.f));
    }
    bool isFloat() const { return isFloat_; }
    double value() const { return value_; }
    std::string text() const override {
        if (!isFloat_) return formatFloat((float)value_);
        std::string t = formatFloat((float)value_);
        if (t.find_first_of(".en") == std::string::npos) t += ".";
        return t;
    }
    Message saveArgs() const override { return {Atom((float)value_)}; }
    void set(double v, bool output) {
        value_ = fix(v);
        if (patch) patch->redraw();
        if (output) outletFloat(0, (float)value_);
    }
    void message(int, const Message& m) override {
        if (m[0].isFloat()) set(m[0].f, true);
        else if (m[0].is("bang")) set(value_, true);
        else if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) set(m[1].f, false);
    }

private:
    double fix(double v) const { return isFloat_ ? v : std::trunc(v); }
    bool isFloat_;
    double value_ = 0.0;
};

// ---------------------------------------------------------------- message
// Click or bang to send the contents. "$1".."$9" are replaced by the
// incoming message. "," separates messages, ";" starts a remote message
// whose first word names a receive object:  "; freq 440; amp 0.2"
class MessageBox : public Box {
public:
    explicit MessageBox(const Message& a) {
        kind = BoxKind::Message;
        setPorts({P::Control}, {P::Control});
        content = messageToString(a);
    }
    std::string text() const override { return content; }
    Message saveArgs() const override { return {Atom(content)}; }
    void message(int, const Message& m) override {
        if (m[0].is("set")) {
            content = messageToString(Message(m.begin() + 1, m.end()));
            if (patch) patch->redraw();
            return;
        }
        trigger(m[0].is("bang") ? Message{} : m);
    }
    void trigger(const Message& in) {
        flashUntil = now() + 0.15;
        patch->redraw();
        Message cur;
        std::string target;
        bool wantTarget = false;
        auto flush = [&] {
            if (cur.empty()) return;
            if (target.empty()) outlet(0, cur);
            else patch->sendTo(target, cur);
            cur.clear();
        };
        for (auto& tok : splitTokens(content)) {
            if (tok == ";") { flush(); target.clear(); wantTarget = true; continue; }
            if (tok == ",") { flush(); continue; }
            Atom a = parseAtom(tok);
            if (tok.size() > 1 && tok[0] == '$' && std::isdigit((unsigned char)tok[1])) {
                size_t k = (size_t)std::atoi(tok.c_str() + 1);
                a = (k >= 1 && k <= in.size()) ? in[k - 1] : Atom(0.f);
            }
            if (wantTarget) { target = a.str(); wantTarget = false; continue; }
            cur.push_back(a);
        }
        flush();
    }
    std::string content;
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

class ReceiveBox : public Box {
public:
    explicit ReceiveBox(const Message& a) {
        setPorts({}, {P::Control});
        if (!a.empty()) name = a[0].str();
    }
    void receive(const Message& m) { outlet(0, m); }
    std::string name;
};

// ---------------------------------------------------------------- loadbang
class LoadbangBox : public Box {
public:
    LoadbangBox() { setPorts({P::Control}, {P::Control}); }
    void loadbang() override { outletBang(0); }
    void message(int, const Message& m) override { if (m[0].is("bang")) outletBang(0); }
};

// ---------------------------------------------------------------- adc~ / dac~
std::vector<int> channelArgs(const Message& a) {
    std::vector<int> ch;
    for (auto& x : a)
        if (x.isFloat() && x.f >= 1.f) ch.push_back((int)x.f);
    if (ch.empty()) ch = {1, 2};
    return ch;
}

bool handleDspMessage(Patch* p, const Message& m) {
    if (m[0].is("start") || m[0].is("startwindow")) { if (p->onDsp) p->onDsp(true); return true; }
    if (m[0].is("stop")) { if (p->onDsp) p->onDsp(false); return true; }
    return false;
}

class AdcBox : public Box {
public:
    explicit AdcBox(const Message& a) : ch_(channelArgs(a)) {
        setPorts({P::Control}, std::vector<Port>(ch_.size(), P::Signal));
    }
    bool wantsInput() const override { return true; }
    void message(int, const Message& m) override { handleDspMessage(patch, m); }
    void dsp(int n) override {
        for (size_t k = 0; k < ch_.size(); ++k) {
            float* out = sigOut((int)k);
            int c = ch_[k] - 1;
            if (patch->ioIn && c < patch->ioInCh) {
                const float* in = patch->ioIn + (size_t)patch->ioOffset * patch->ioInCh + c;
                for (int i = 0; i < n; ++i) out[i] = in[(size_t)i * patch->ioInCh];
            } else {
                dsp::fill(out, 0.f, n);
            }
        }
    }

private:
    std::vector<int> ch_;
};

class DacBox : public Box {
public:
    explicit DacBox(const Message& a) : ch_(channelArgs(a)) {
        setPorts(std::vector<Port>(ch_.size(), P::Signal), {});
    }
    void message(int, const Message& m) override { handleDspMessage(patch, m); }
    void dsp(int n) override {
        if (!patch->ioOut) return;
        for (size_t k = 0; k < ch_.size(); ++k) {
            int c = ch_[k] - 1;
            if (c >= patch->ioOutCh) continue;
            const float* in = sigIn((int)k, n);
            float* out = patch->ioOut + (size_t)patch->ioOffset * patch->ioOutCh + c;
            for (int i = 0; i < n; ++i) out[(size_t)i * patch->ioOutCh] += in[i];
        }
    }

private:
    std::vector<int> ch_;
};

// ================================================================ helpers

std::string argstr(const Message& a, size_t i, const std::string& def = "") {
    return i < a.size() ? a[i].str() : def;
}

bool atomEq(const Atom& x, const Atom& y) {
    return x.type == y.type && (x.isFloat() ? x.f == y.f : x.s == y.s);
}

Message tailOf(const Message& m, size_t from = 1) {
    return from < m.size() ? Message(m.begin() + (long)from, m.end()) : Message{};
}

bool isIntegral(float v) { return std::isfinite(v) && v == std::floor(v); }

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
        if (next_ < patch->time() - 0.25) next_ = patch->time();   // fell far behind
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

// ---------------------------------------------------------------- line~
// line~: signal ramp. "target time" pairs make segments ("1 10 0 300");
// a float alone jumps, or ramps over the time given in the right inlet.
class LineSigBox : public Box {
public:
    explicit LineSigBox(const Message& a) {
        setPorts({P::Control, P::Control}, {P::Signal});
        if (!a.empty() && a[0].isFloat()) post({dsp::Ramp::Cmd::Segment, a[0].f, 0.f});
    }
    void message(int in, const Message& m) override {
        if (in == 1) { if (m[0].isFloat()) time_ = m[0].f; return; }
        if (m[0].is("stop")) { post({dsp::Ramp::Cmd::Stop, 0.f, 0.f}); return; }
        if (!m[0].isFloat()) return;
        post({dsp::Ramp::Cmd::Begin, 0.f, 0.f});
        if (m.size() == 1) {
            post({dsp::Ramp::Cmd::Segment, m[0].f, time_});
        } else {
            for (size_t k = 0; k < m.size(); k += 2) {
                float target = m[k].isFloat() ? m[k].f : 0.f;
                float ms = k + 1 < m.size() && m[k + 1].isFloat() ? m[k + 1].f : 0.f;
                post({dsp::Ramp::Cmd::Segment, target, ms});
            }
        }
        time_ = 0.f;
    }
    void dsp(int n) override { ramp_.process(sigOut(0), n, patch->sampleRate); }

private:
    void post(const dsp::Ramp::Cmd& c) { ramp_.post(c); }
    dsp::Ramp ramp_;
    float time_ = 0.f;
};

// ---------------------------------------------------------------- sig~
class SigBox : public Box {
public:
    explicit SigBox(const Message& a) {
        setPorts({P::Signal}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
    }
    void message(int, const Message& m) override { if (m[0].isFloat()) setScalar(0, m[0].f); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        std::copy(x, x + n, sigOut(0));
    }
};

// ================================================================ oscillators

// ---------------------------------------------------------------- phasor~
class PhasorBox : public Box {
public:
    explicit PhasorBox(const Message& a) {
        setPorts({P::Signal, P::Control}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 0) setScalar(0, m[0].f);
        else ph_.setPhase(m[0].f);
    }
    void dsp(int n) override { ph_.process(sigIn(0, n), sigOut(0), n, patch->sampleRate); }

private:
    dsp::Phasor ph_;
};

// ---------------------------------------------------------------- saw~ rect~ tri~
class OscBox : public Box {
public:
    OscBox(dsp::Osc::Shape s, const Message& a) : osc_(s), shape_(s) {
        if (s == dsp::Osc::Saw) setPorts({P::Signal}, {P::Signal});
        else setPorts({P::Signal, P::Signal}, {P::Signal});
        setScalar(0, argf(a, 0, 0.f));
        if (s != dsp::Osc::Saw) setScalar(1, argf(a, 1, 0.5f));
    }
    void message(int in, const Message& m) override { if (m[0].isFloat()) setScalar(in, m[0].f); }
    void dsp(int n) override {
        const float* f = sigIn(0, n);
        const float* d = shape_ == dsp::Osc::Saw ? f : sigIn(1, n);
        osc_.process(f, d, sigOut(0), n, patch->sampleRate);
    }

private:
    dsp::Osc osc_;
    dsp::Osc::Shape shape_;
};

// ================================================================ filters

// ---------------------------------------------------------------- lores~
// lores~ [cf res]: resonant low-pass, res 0..1.
class LoresBox : public Box {
public:
    explicit LoresBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, 1000.f));
        setScalar(2, argf(a, 1, 0.f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->dspMutex); f_ = dsp::Biquad(); return; }
        if (in > 0 && m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float cf = sigIn(1, n)[0], res = std::clamp(sigIn(2, n)[0], 0.f, 1.f);
        f_.lowpass(cf, 0.707 + res * res * 24.0, patch->sampleRate);
        f_.process(x, sigOut(0), n);
    }

private:
    dsp::Biquad f_;
};

// ---------------------------------------------------------------- svf~
// svf~ [freq q]: state-variable filter; outlets low, high, band, notch.
class SvfBox : public Box {
public:
    explicit SvfBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal, P::Signal, P::Signal, P::Signal});
        setScalar(1, argf(a, 0, 1000.f));
        setScalar(2, argf(a, 1, 0.5f));
    }
    void message(int in, const Message& m) override {
        if (in > 0 && m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float f = sigIn(1, n)[0], q = sigIn(2, n)[0];
        f_.process(x, sigOut(0), sigOut(1), sigOut(2), sigOut(3), n, f, q, patch->sampleRate);
    }

private:
    dsp::Svf f_;
};

// ---------------------------------------------------------------- biquad~
// biquad~ [a0 a1 a2 b1 b2]: y = a0 x + a1 x1 + a2 x2 - b1 y1 - b2 y2.
// Coefficients in inlets 1-5, or as a list in the left inlet.
class BiquadBox : public Box {
public:
    explicit BiquadBox(const Message& a) {
        setPorts({P::Signal, P::Control, P::Control, P::Control, P::Control, P::Control},
                 {P::Signal});
        const float def[5] = {1, 0, 0, 0, 0};
        for (int k = 0; k < 5; ++k) setScalar(1 + k, argf(a, (size_t)k, def[k]));
    }
    void message(int in, const Message& m) override {
        if (in == 0 && m.size() >= 5) {
            for (int k = 0; k < 5; ++k)
                if (m[(size_t)k].isFloat()) setScalar(1 + k, m[(size_t)k].f);
        } else if (in > 0 && m[0].isFloat()) {
            setScalar(in, m[0].f);
        }
    }
    void dsp(int n) override {
        f_.set(scalar(1), scalar(2), scalar(3), scalar(4), scalar(5));
        f_.process(sigIn(0, n), sigOut(0), n);
    }

private:
    dsp::Biquad f_;
};

// ================================================================ signal math

// ---------------------------------------------------------------- clip~ abs~ tanh~
class ClipBox : public Box {
public:
    explicit ClipBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        setScalar(1, argf(a, 0, -1.f));
        setScalar(2, argf(a, 1, 1.f));
    }
    void message(int in, const Message& m) override { if (in > 0 && m[0].isFloat()) setScalar(in, m[0].f); }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *lo = sigIn(1, n), *hi = sigIn(2, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) y[i] = std::min(std::max(x[i], lo[i]), hi[i]);
    }
};

class UnarySigBox : public Box {
public:
    explicit UnarySigBox(float (*fn)(float)) : fn_(fn) { setPorts({P::Signal}, {P::Signal}); }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float* y = sigOut(0);
        for (int i = 0; i < n; ++i) y[i] = fn_(x[i]);
    }

private:
    float (*fn_)(float);
};

// ================================================================ delays

// ---------------------------------------------------------------- tapin~ / tapout~
// tapin~ maxms writes a delay line; connect its outlet to tapout~, whose
// inlets set each tap's delay (ms). The tap link doesn't order the two, so
// tapout~ -> ... -> tapin~ feedback is allowed (minimum delay: one block).
class TapinBox : public Box {
public:
    explicit TapinBox(const Message& a) {
        setPorts({P::Signal}, {P::Tap});
        maxMs_ = std::clamp(argf(a, 0, 1000.f), 1.f, 60000.f);
        // sized for sample rates up to 96 kHz
        line.resize((size_t)(maxMs_ / 1000.0 * 96000.0) + 4 * dsp::kBlock);
    }
    void message(int, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->dspMutex); line.clear(); }
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        for (int i = 0; i < n; ++i) line.push(x[i]);
        lastBlock = patch->blockCount;
    }
    float maxMs() const { return maxMs_; }
    dsp::DelayLine line;
    uint64_t lastBlock = ~0ull;

private:
    float maxMs_;
};

class TapoutBox : public Box {
public:
    explicit TapoutBox(const Message& a) {
        size_t taps = std::max<size_t>(1, a.size());
        setPorts(std::vector<Port>(taps, P::Signal), std::vector<Port>(taps, P::Signal));
        for (size_t k = 0; k < taps; ++k) setScalar((int)k, argf(a, k, 0.f));
    }
    void message(int in, const Message& m) override { if (m[0].isFloat()) setScalar(in, m[0].f); }
    void resolve() override {
        tap_ = nullptr;
        for (auto& c : patch->conns)
            if (c.to == this && c.from->outs[c.outlet] == Port::Tap)
                tap_ = static_cast<TapinBox*>(c.from);
    }
    void dsp(int n) override {
        if (!tap_) {
            for (int k = 0; k < numOuts(); ++k) dsp::fill(sigOut(k), 0.f, n);
            return;
        }
        bool written = tap_->lastBlock == patch->blockCount;   // tapin~ ran this block
        double sr = patch->sampleRate, maxD = tap_->line.size() - 2.0 * dsp::kBlock;
        for (int k = 0; k < numOuts(); ++k) {
            const float* ms = sigIn(k, n);
            float* y = sigOut(k);
            for (int i = 0; i < n; ++i) {
                double d = std::clamp(ms[i] * sr / 1000.0, written ? 0.0 : (double)n, maxD);
                y[i] = tap_->line.read(written ? n - i + d : d - i);
            }
        }
    }

private:
    TapinBox* tap_ = nullptr;
};

// ---------------------------------------------------------------- comb~ / allpass~
// comb~ [maxms delay a b c]: y = a x[n] + b x[n-D] + c y[n-D]
class CombBox : public Box {
public:
    explicit CombBox(const Message& a) {
        setPorts(std::vector<Port>(5, P::Signal), {P::Signal});
        float maxMs = std::clamp(argf(a, 0, 50.f), 1.f, 10000.f);
        size_t len = (size_t)(maxMs / 1000.0 * 96000.0) + 8;
        xs_.resize(len);
        ys_.resize(len);
        maxD_ = len - 4.0;
        setScalar(1, argf(a, 1, maxMs * 0.5f));
        setScalar(2, argf(a, 2, 1.f));
        setScalar(3, argf(a, 3, 0.f));
        setScalar(4, argf(a, 4, 0.5f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->dspMutex); xs_.clear(); ys_.clear(); return; }
        if (in > 0 && m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *dm = sigIn(1, n), *ga = sigIn(2, n), *gb = sigIn(3, n),
                    *gc = sigIn(4, n);
        float* y = sigOut(0);
        double sr = patch->sampleRate;
        for (int i = 0; i < n; ++i) {
            double D = std::clamp(dm[i] * sr / 1000.0, 1.0, maxD_);
            float out = ga[i] * x[i] + gb[i] * xs_.read(D) + gc[i] * ys_.read(D);
            if (!std::isfinite(out)) out = 0.f;
            xs_.push(x[i]);
            ys_.push(out);
            y[i] = out;
        }
    }

private:
    dsp::DelayLine xs_, ys_;
    double maxD_ = 1.0;
};

// allpass~ [maxms delay gain]: y = -g x[n] + x[n-D] + g y[n-D]
class AllpassBox : public Box {
public:
    explicit AllpassBox(const Message& a) {
        setPorts({P::Signal, P::Signal, P::Signal}, {P::Signal});
        float maxMs = std::clamp(argf(a, 0, 50.f), 1.f, 10000.f);
        size_t len = (size_t)(maxMs / 1000.0 * 96000.0) + 8;
        xs_.resize(len);
        ys_.resize(len);
        maxD_ = len - 4.0;
        setScalar(1, argf(a, 1, maxMs * 0.5f));
        setScalar(2, argf(a, 2, 0.7f));
    }
    void message(int in, const Message& m) override {
        if (m[0].is("clear")) { std::lock_guard<std::mutex> lk(patch->dspMutex); xs_.clear(); ys_.clear(); return; }
        if (in > 0 && m[0].isFloat()) setScalar(in, m[0].f);
    }
    void dsp(int n) override {
        const float *x = sigIn(0, n), *dm = sigIn(1, n), *g = sigIn(2, n);
        float* y = sigOut(0);
        double sr = patch->sampleRate;
        for (int i = 0; i < n; ++i) {
            double D = std::clamp(dm[i] * sr / 1000.0, 1.0, maxD_);
            float out = -g[i] * x[i] + xs_.read(D) + g[i] * ys_.read(D);
            if (!std::isfinite(out)) out = 0.f;
            xs_.push(x[i]);
            ys_.push(out);
            y[i] = out;
        }
    }

private:
    dsp::DelayLine xs_, ys_;
    double maxD_ = 1.0;
};

// ================================================================ samples

struct SampleBuffer {
    std::vector<float> data;   // interleaved
    int channels = 1;
    long frames = 0;
    double sr = 44100.0;

    float at(long frame, int ch) const {
        if (frame < 0 || frame >= frames) return 0.f;
        return data[(size_t)frame * channels + (ch % channels)];
    }
    float interp(double pos, int ch) const {
        long i = (long)std::floor(pos);
        float f = (float)(pos - i);
        float a = at(i, ch), b = at(i + 1, ch);
        return a + f * (b - a);
    }
};

bool decodeAudioFile(const std::string& path, SampleBuffer& out, std::string& err) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) {
        err = "can't read audio file " + path;
        return false;
    }
    SampleBuffer b;
    b.channels = (int)dec.outputChannels;
    b.sr = dec.outputSampleRate;
    std::vector<float> chunk(4096 * (size_t)b.channels);
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk.data(), 4096, &got);
        b.data.insert(b.data.end(), chunk.begin(), chunk.begin() + (long)(got * b.channels));
        if (r != MA_SUCCESS || got == 0) break;
    }
    ma_decoder_uninit(&dec);
    b.frames = (long)(b.data.size() / b.channels);
    out = std::move(b);
    return true;
}

bool writeWavFile(const std::string& path, const SampleBuffer& b, std::string& err) {
    ma_encoder_config cfg =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, (ma_uint32)b.channels,
                               (ma_uint32)b.sr);
    ma_encoder enc;
    if (ma_encoder_init_file(path.c_str(), &cfg, &enc) != MA_SUCCESS) {
        err = "can't write " + path;
        return false;
    }
    std::vector<int16_t> pcm(b.data.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = (int16_t)std::lrint(std::clamp(b.data[i], -1.f, 1.f) * 32767.f);
    ma_encoder_write_pcm_frames(&enc, pcm.data(), (ma_uint64)b.frames, nullptr);
    ma_encoder_uninit(&enc);
    return true;
}

// ---------------------------------------------------------------- buffer~
// buffer~ name [file] [ms] [channels]: named sample memory shared with
// play~, groove~ and record~. Messages: read file, write file, size ms, clear.
class BufferBox : public Box {
public:
    explicit BufferBox(const Message& a) {
        setPorts({P::Control}, {});
        name = argstr(a, 0);
        size_t k = 1;
        if (k < a.size() && a[k].isSymbol()) file_ = a[k++].s;
        float ms = argf(a, k, 0.f);
        int ch = std::clamp((int)argf(a, k + 1, 1.f), 1, 8);
        buf.channels = ch;
        buf.frames = (long)(ms / 1000.0 * 44100.0);
        buf.data.assign((size_t)buf.frames * ch, 0.f);
    }
    void attached() override {
        if (!file_.empty()) read(file_);
    }
    void message(int, const Message& m) override {
        std::string err;
        if (m[0].is("read") && m.size() > 1) {
            read(m[1].str());
        } else if (m[0].is("write") && m.size() > 1) {
            SampleBuffer copy;
            { std::lock_guard<std::mutex> lk(patch->dspMutex); copy = buf; }
            if (!writeWavFile(patch->resolvePath(m[1].str()), copy, err)) patch->print(err);
        } else if (m[0].is("size") && m.size() > 1 && m[1].isFloat()) {
            std::lock_guard<std::mutex> lk(patch->dspMutex);
            buf.frames = std::max(0L, (long)(m[1].f / 1000.0 * buf.sr));
            buf.data.resize((size_t)buf.frames * buf.channels, 0.f);
        } else if (m[0].is("clear")) {
            std::lock_guard<std::mutex> lk(patch->dspMutex);
            std::fill(buf.data.begin(), buf.data.end(), 0.f);
        }
    }
    std::string name;
    SampleBuffer buf;        // touched by the audio thread only under the DSP lock

private:
    void read(const std::string& file) {
        SampleBuffer b;
        std::string err;
        if (!decodeAudioFile(patch->resolvePath(file), b, err)) { patch->print("buffer~ " + name + ": " + err); return; }
        std::lock_guard<std::mutex> lk(patch->dspMutex);
        buf = std::move(b);
    }
    std::string file_;
};

BufferBox* findBuffer(Patch* p, const std::string& name) {
    for (auto& b : p->boxes)
        if (auto bb = dynamic_cast<BufferBox*>(b.get()))
            if (bb->name == name) return bb;
    return nullptr;
}

// ---------------------------------------------------------------- play~
// play~ name [channels]: plays a buffer~. With a signal in the inlet it
// reads at that position (ms, e.g. from line~); otherwise "start [from
// [to]]" (ms) or bang plays it, "stop" stops.
class PlayBox : public Box {
public:
    explicit PlayBox(const Message& a) : name_(argstr(a, 0)) {
        setPorts({P::Signal}, std::vector<Port>((size_t)std::clamp((int)argf(a, 1, 1.f), 1, 8), P::Signal));
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int, const Message& m) override {
        if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); return; }
        if (m[0].is("stop")) { cmd_.store(2); return; }
        if (m[0].is("bang") || m[0].is("start")) {
            startMs_.store(m.size() > 1 && m[1].isFloat() ? m[1].f : 0.f);
            endMs_.store(m.size() > 2 && m[2].isFloat() ? m[2].f : -1.f);
            cmd_.store(1);
        }
    }
    void dsp(int n) override {
        int c = cmd_.exchange(0);
        const SampleBuffer* b = b_ ? &b_->buf : nullptr;
        double ms2fr = b ? b->sr / 1000.0 : 0.0;
        if (c == 1 && b) {
            pos_ = startMs_.load() * ms2fr;
            float e = endMs_.load();
            end_ = e > 0 ? e * ms2fr : (double)b->frames;
            playing_ = true;
        } else if (c == 2) {
            playing_ = false;
        }
        bool driven = !inSrc[0].empty();
        const float* posMs = driven ? sigIn(0, n) : nullptr;
        double rate = b ? b->sr / patch->sampleRate : 0.0;
        for (int k = 0; k < numOuts(); ++k) {
            float* y = sigOut(k);
            for (int i = 0; i < n; ++i) {
                double p = driven ? posMs[i] * ms2fr : pos_ + i * rate;
                y[i] = (b && (driven || (playing_ && p < end_))) ? b->interp(p, k) : 0.f;
            }
        }
        if (playing_ && !driven) {
            pos_ += n * rate;
            if (pos_ >= end_) playing_ = false;
        }
    }

private:
    std::string name_;
    BufferBox* b_ = nullptr;
    std::atomic<int> cmd_{0};
    std::atomic<float> startMs_{0.f}, endMs_{-1.f};
    double pos_ = 0.0, end_ = 0.0;
    bool playing_ = false;
};

// ---------------------------------------------------------------- groove~
// groove~ name [channels]: variable-speed looping playback. Left inlet:
// playback rate (signal, e.g. sig~ 1); a float there jumps to that position
// (ms). Inlets 2/3: loop start/end (ms, 0 end = whole buffer). "loop 1"
// turns looping on. Last outlet: sync signal (0..1 through the loop).
class GrooveBox : public Box {
public:
    explicit GrooveBox(const Message& a) : name_(argstr(a, 0)) {
        int ch = std::clamp((int)argf(a, 1, 1.f), 1, 8);
        std::vector<Port> outs((size_t)ch + 1, P::Signal);
        setPorts({P::Signal, P::Control, P::Control}, outs);
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) setScalar(in, std::max(0.f, m[0].f)); return; }
        if (m[0].is("loop") && m.size() > 1) loop_.store(m[1].isFloat() && m[1].f != 0.f);
        else if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
        else if (m[0].is("startloop")) jumpMs_.store(scalar(1));
        else if (m[0].isFloat()) jumpMs_.store(std::max(0.f, m[0].f));
    }
    void dsp(int n) override {
        int ch = numOuts() - 1;
        float* sync = sigOut(ch);
        const SampleBuffer* b = b_ ? &b_->buf : nullptr;
        if (!b || b->frames < 2) {
            for (int k = 0; k <= ch; ++k) dsp::fill(sigOut(k), 0.f, n);
            return;
        }
        double ms2fr = b->sr / 1000.0, ratio = b->sr / patch->sampleRate;
        float j = jumpMs_.exchange(-1.f);
        if (j >= 0.f) pos_ = j * ms2fr;
        double ls = std::clamp(scalar(1) * ms2fr, 0.0, (double)b->frames - 1);
        double le = scalar(2) > 0.f ? std::min(scalar(2) * ms2fr, (double)b->frames) : (double)b->frames;
        if (le <= ls + 1) le = (double)b->frames;
        bool loop = loop_.load();
        const float* rate = sigIn(0, n);
        for (int i = 0; i < n; ++i) {
            if (loop) {
                double L = le - ls;
                if (pos_ >= le) pos_ = ls + std::fmod(pos_ - ls, L);
                if (pos_ < ls) pos_ = le - std::fmod(ls - pos_, L);
            }
            bool inside = pos_ >= 0 && pos_ < b->frames;
            for (int k = 0; k < ch; ++k) sigOut(k)[i] = inside ? b->interp(pos_, k) : 0.f;
            sync[i] = (float)std::clamp((pos_ - ls) / (le - ls), 0.0, 1.0);
            pos_ += rate[i] * ratio;
            if (!loop) pos_ = std::clamp(pos_, -1.0, (double)b->frames);
        }
    }

private:
    std::string name_;
    BufferBox* b_ = nullptr;
    std::atomic<bool> loop_{false};
    std::atomic<float> jumpMs_{-1.f};
    double pos_ = 0.0;
};

// ---------------------------------------------------------------- record~
// record~ name [channels]: records its signal inlets into a buffer~.
// 1 starts (from the beginning), 0 stops; "loop 1" wraps instead of
// stopping at the end. Outlet: sync signal (0..1 through the buffer).
class RecordBox : public Box {
public:
    explicit RecordBox(const Message& a) : name_(argstr(a, 0)) {
        int ch = std::clamp((int)argf(a, 1, 1.f), 1, 8);
        setPorts(std::vector<Port>((size_t)ch, P::Signal), {P::Signal});
    }
    void resolve() override { b_ = findBuffer(patch, name_); }
    void message(int in, const Message& m) override {
        if (in != 0) return;
        if (m[0].is("loop") && m.size() > 1) loop_.store(m[1].isFloat() && m[1].f != 0.f);
        else if (m[0].is("set") && m.size() > 1) { name_ = m[1].str(); patch->rebuild(); }
        else if (m[0].isFloat()) cmd_.store(m[0].f != 0.f ? 1 : 2);
    }
    void dsp(int n) override {
        int c = cmd_.exchange(0);
        if (c == 1) { pos_ = 0; rec_ = true; }
        if (c == 2) rec_ = false;
        SampleBuffer* b = b_ ? &b_->buf : nullptr;
        float* sync = sigOut(0);
        std::vector<const float*> in((size_t)numIns());
        for (int k = 0; k < numIns(); ++k) in[(size_t)k] = sigIn(k, n);
        for (int i = 0; i < n; ++i) {
            if (rec_ && b && b->frames > 0) {
                if (pos_ >= b->frames) {
                    if (loop_.load()) pos_ = 0;
                    else rec_ = false;
                }
                if (rec_) {
                    for (int k = 0; k < b->channels; ++k)
                        b->data[(size_t)pos_ * b->channels + k] = in[(size_t)(k % numIns())][i];
                    ++pos_;
                }
            }
            sync[i] = b && b->frames > 0 ? (float)pos_ / (float)b->frames : 0.f;
        }
    }

private:
    std::string name_;
    BufferBox* b_ = nullptr;
    std::atomic<int> cmd_{0};
    std::atomic<bool> loop_{false};
    long pos_ = 0;
    bool rec_ = false;
};

// ---------------------------------------------------------------- sfplay~
// sfplay~ [channels]: "open file" loads a sound file; 1 plays from the
// start, 0 stops; "loop 1" loops; "speed x" sets the playback speed.
class SfplayBox : public Box {
public:
    explicit SfplayBox(const Message& a) {
        int ch = std::clamp((int)argf(a, 0, 1.f), 1, 8);
        setPorts({P::Control}, std::vector<Port>((size_t)ch, P::Signal));
    }
    void message(int, const Message& m) override {
        if (m[0].is("open") && m.size() > 1) {
            SampleBuffer b;
            std::string err;
            if (!decodeAudioFile(patch->resolvePath(m[1].str()), b, err)) { patch->print("sfplay~: " + err); return; }
            std::lock_guard<std::mutex> lk(patch->dspMutex);
            buf_ = std::move(b);
            playing_ = false;
        } else if (m[0].is("loop") && m.size() > 1) {
            loop_.store(m[1].isFloat() && m[1].f != 0.f);
        } else if (m[0].is("speed") && m.size() > 1 && m[1].isFloat()) {
            speed_.store(m[1].f);
        } else if (m[0].isFloat()) {
            cmd_.store(m[0].f != 0.f ? 1 : 2);
        } else if (m[0].is("bang")) {
            cmd_.store(1);
        }
    }
    void dsp(int n) override {
        int c = cmd_.exchange(0);
        if (c == 1) { pos_ = 0; playing_ = buf_.frames > 0; }
        if (c == 2) playing_ = false;
        double step = buf_.sr / patch->sampleRate * speed_.load();
        for (int k = 0; k < numOuts(); ++k) {
            float* y = sigOut(k);
            double p = pos_;
            for (int i = 0; i < n; ++i) {
                if (playing_ && loop_.load() && buf_.frames > 0) p = std::fmod(p, (double)buf_.frames);
                y[i] = playing_ ? buf_.interp(p, k) : 0.f;
                p += step;
            }
        }
        if (playing_) {
            pos_ += n * step;
            if (loop_.load() && buf_.frames > 0) pos_ = std::fmod(pos_, (double)buf_.frames);
            else if (pos_ >= buf_.frames || pos_ < 0) playing_ = false;
        }
    }

private:
    SampleBuffer buf_;
    std::atomic<int> cmd_{0};
    std::atomic<bool> loop_{false};
    std::atomic<float> speed_{1.f};
    double pos_ = 0.0;
    bool playing_ = false;
};

// ================================================================ analysis

// ---------------------------------------------------------------- snapshot~ / peakamp~
// snapshot~ [ms]: outputs the signal's current value on bang, or every ms.
// peakamp~ [ms]: outputs the peak absolute value since the last report.
class ProbeBox : public Box {
public:
    ProbeBox(bool peak, const Message& a) : peak_(peak), interval_(std::max(0.f, argf(a, 0, 0.f))) {
        setPorts({P::Signal, P::Control}, {P::Control});
    }
    void attached() override { restart(); }
    void message(int in, const Message& m) override {
        if (in == 1 || (m[0].is("interval") && m.size() > 1)) {
            const Atom& v = in == 1 ? m[0] : m[1];
            if (v.isFloat()) { interval_ = std::max(0.f, v.f); restart(); }
            return;
        }
        if (m[0].is("bang")) report();
        else if (m[0].isFloat()) { interval_ = std::max(0.f, m[0].f); restart(); }
    }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        if (peak_) {
            float p = 0.f;
            for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
            if (p > value_.load(std::memory_order_relaxed)) value_.store(p, std::memory_order_relaxed);
        } else {
            value_.store(x[n - 1], std::memory_order_relaxed);
        }
    }

private:
    void report() { outletFloat(0, peak_ ? value_.exchange(0.f) : value_.load()); }
    void restart() {
        patch->cancel(this);
        if (interval_ > 0.f) tick();
    }
    void tick() {
        if (patch->dspRunning) report();
        patch->schedule(this, std::max(5.f, interval_) / 1000.0, [this] { tick(); });
    }
    bool peak_;
    float interval_;
    std::atomic<float> value_{0.f};
};

// ---------------------------------------------------------------- meter~
class MeterBox : public Box {
public:
    MeterBox() { kind = BoxKind::UI; setPorts({P::Signal}, {}); }
    void uiSize(int& w, int& h) const override { w = 18; h = 90; }
    bool animated() const override { return true; }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        float p = 0.f;
        for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
        if (p > peak_.load(std::memory_order_relaxed)) peak_.store(p, std::memory_order_relaxed);
    }
    void drawUI(int X, int Y) override {
        level_ = std::max(peak_.exchange(0.f), level_ * 0.85f);
        double db = 20.0 * std::log10(level_ + 1e-9);
        double norm = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        int H = h - 8, bh = (int)(H * norm);
        fl_color(fl_rgb_color(26, 27, 31));
        fl_rectf(X + 4, Y + 4, w - 8, H);
        Fl_Color c = db > -3 ? fl_rgb_color(235, 80, 70)
                   : db > -12 ? fl_rgb_color(235, 200, 70) : fl_rgb_color(90, 200, 120);
        fl_color(c);
        fl_rectf(X + 4, Y + 4 + H - bh, w - 8, bh);
    }

private:
    std::atomic<float> peak_{0.f};
    float level_ = 0.f;
};

// ---------------------------------------------------------------- scope~
// scope~ [samples]: oscilloscope, triggered on rising zero crossings.
class ScopeBox : public Box {
public:
    explicit ScopeBox(const Message& a) : ring_(new std::atomic<float>[kRing]) {
        kind = BoxKind::UI;
        setPorts({P::Signal}, {});
        window_ = std::clamp((int)argf(a, 0, 512.f), 32, kRing / 4);
        for (int i = 0; i < kRing; ++i) ring_[i].store(0.f);
    }
    void uiSize(int& w, int& h) const override { w = 180; h = 100; }
    bool animated() const override { return true; }
    void dsp(int n) override {
        const float* x = sigIn(0, n);
        size_t w = w_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) ring_[(w + i) & (kRing - 1)].store(x[i], std::memory_order_relaxed);
        w_.store(w + n, std::memory_order_release);
    }
    void drawUI(int X, int Y) override {
        fl_color(fl_rgb_color(20, 24, 22));
        fl_rectf(X + 3, Y + 3, w - 6, h - 6);
        fl_color(fl_rgb_color(45, 60, 52));
        fl_xyline(X + 3, Y + h / 2, X + w - 4);
        size_t end = w_.load(std::memory_order_acquire);
        size_t N = (size_t)window_;
        size_t start = end > 2 * N ? end - 2 * N : 0;
        auto s = [&](size_t i) { return ring_[i & (kRing - 1)].load(std::memory_order_relaxed); };
        size_t trig = end > N ? end - N : 0;
        for (size_t i = start + 1; i + N <= end && i < start + N; ++i)
            if (s(i - 1) < 0.f && s(i) >= 0.f) { trig = i; break; }
        fl_color(kSignal);
        fl_push_clip(X + 3, Y + 3, w - 6, h - 6);
        fl_begin_line();
        double sx = (double)(w - 8) / (double)(N - 1), sy = (h - 10) / 2.0;
        for (size_t i = 0; i < N; ++i)
            fl_vertex(X + 4 + i * sx, Y + h / 2.0 - std::clamp(s(trig + i), -1.f, 1.f) * sy);
        fl_end_line();
        fl_pop_clip();
    }

private:
    static constexpr int kRing = 4096;
    std::unique_ptr<std::atomic<float>[]> ring_;
    std::atomic<size_t> w_{0};
    int window_;
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
        fl_pie(X + 5, Y + 5, w - 10, h - 10, 0, 360);
        fl_color(on ? kAccent : kCtlPort);
        fl_arc(X + 5, Y + 5, w - 10, h - 10, 0, 360);
    }

private:
    void bang() {
        flashUntil = now() + 0.15;
        patch->redraw();
        outletBang(0);
    }
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
        fl_line_style(FL_SOLID | FL_CAP_ROUND, 2);
        fl_line(X + 7, Y + 7, X + w - 8, Y + h - 8);
        fl_line(X + w - 8, Y + 7, X + 7, Y + h - 8);
        fl_line_style(0);
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
        int_ = isIntegral(lo_) && isIntegral(hi_);
        value_ = lo_;
    }
    void uiSize(int& w, int& h) const override { w = 140; h = 22; }
    void message(int, const Message& m) override {
        if (m[0].is("bang")) outletFloat(0, value_);
        else if (m[0].isFloat()) set(m[0].f, true);
        else if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) set(m[1].f, false);
    }
    void uiMouse(int e, int mx, int) override {
        if (e == FL_RELEASE) return;
        double k = std::clamp((mx - 8.0) / (w - 16.0), 0.0, 1.0);
        set((float)(lo_ + k * (hi_ - lo_)), true);
    }
    void drawUI(int X, int Y) override {
        double k = std::clamp((value_ - lo_) / (hi_ - lo_), 0.f, 1.f);
        int tx = X + 8, tw = w - 16, ty = Y + h / 2;
        fl_color(fl_rgb_color(30, 31, 36));
        fl_rounded_rectf(tx, ty - 2, tw, 4, 2);
        fl_color(kAccent);
        fl_rounded_rectf(tx, ty - 2, std::max(2, (int)(tw * k)), 4, 2);
        int kx = tx + (int)(tw * k);
        fl_color(kText);
        fl_pie(kx - 6, ty - 6, 12, 12, 0, 360);
    }

private:
    void set(float v, bool out) {
        v = std::clamp(v, std::min(lo_, hi_), std::max(lo_, hi_));
        if (int_) v = std::round(v);
        bool changed = v != value_;
        value_ = v;
        patch->redraw();
        if (out && changed) outletFloat(0, value_);
    }
    float lo_, hi_, value_;
    bool int_;
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
class PackBox : public Box {
public:
    explicit PackBox(const Message& a) {
        Message t = a.empty() ? Message{Atom(0.f), Atom(0.f)} : a;
        for (auto& x : t) {
            if (x.is("f") || x.is("i")) vals_.push_back(Atom(0.f));
            else if (x.is("s")) vals_.push_back(Atom(""));
            else vals_.push_back(x);
        }
        setPorts(std::vector<Port>(vals_.size(), P::Control), {P::Control});
    }
    void message(int in, const Message& m) override {
        if (in == 0) {
            if (m[0].is("bang")) { outlet(0, vals_); return; }
            for (size_t k = 0; k < m.size() && k < vals_.size(); ++k) vals_[k] = m[k];
            outlet(0, vals_);
        } else {
            vals_[(size_t)in] = m[0];
        }
    }

private:
    Message vals_;
};

class UnpackBox : public Box {
public:
    explicit UnpackBox(const Message& a) {
        size_t n = a.empty() ? 2 : a.size();
        setPorts({P::Control}, std::vector<Port>(n, P::Control));
    }
    void message(int, const Message& m) override {
        for (int k = std::min(numOuts(), (int)m.size()) - 1; k >= 0; --k)
            outlet(k, {m[(size_t)k]});
    }
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
    }
    void message(int in, const Message& m) override {
        if (in > 0) { if (m[0].isFloat()) p_[in - 1] = m[0].f; return; }
        if (m[0].isFloat()) x_ = m[0].f;
        else if (!m[0].is("bang")) return;
        double range = p_[1] - p_[0];
        double k = range == 0 ? 0 : (x_ - p_[0]) / range;
        if (p_[4] != 1.f) k = k >= 0 ? std::pow(k, p_[4]) : -std::pow(-k, p_[4]);
        outletFloat(0, (float)(p_[2] + k * (p_[3] - p_[2])));
    }

private:
    float p_[5];
    float x_ = 0.f;
};

// ---------------------------------------------------------------- expr
// expr: C-like expressions over $f1..$f9 (floats) and $i1..$i9 (truncated).
// Operators: + - * / % == != < > <= >= && || ! and parentheses.
// Functions: sin cos tan asin acos atan atan2 sinh cosh tanh exp log log10
// sqrt pow abs floor ceil int round min max fmod random(lo, hi); pi.
class ExprParser {
public:
    using Fn = std::function<double(const double*)>;
    explicit ExprParser(std::string s) : s_(std::move(s)) {}
    bool parse(Fn& out, int& nvars, std::string& err) {
        try {
            pos_ = 0;
            Fn f = parseOr();
            skip();
            if (pos_ != s_.size()) fail("unexpected '" + s_.substr(pos_, 1) + "'");
            out = std::move(f);
            nvars = maxVar_;
            return true;
        } catch (std::exception& e) {
            err = e.what();
            return false;
        }
    }

private:
    [[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }
    void skip() { while (pos_ < s_.size() && std::isspace((unsigned char)s_[pos_])) ++pos_; }
    bool eat(const char* tok) {
        skip();
        size_t L = std::strlen(tok);
        if (s_.compare(pos_, L, tok) == 0) { pos_ += L; return true; }
        return false;
    }
    Fn parseOr() {
        Fn a = parseAnd();
        while (eat("||")) { Fn b = parseAnd(); a = [a, b](const double* v) { return (a(v) != 0 || b(v) != 0) ? 1.0 : 0.0; }; }
        return a;
    }
    Fn parseAnd() {
        Fn a = parseCmp();
        while (eat("&&")) { Fn b = parseCmp(); a = [a, b](const double* v) { return (a(v) != 0 && b(v) != 0) ? 1.0 : 0.0; }; }
        return a;
    }
    Fn parseCmp() {
        Fn a = parseAdd();
        for (;;) {
            int op;
            if (eat("==")) op = 0; else if (eat("!=")) op = 1; else if (eat("<=")) op = 2;
            else if (eat(">=")) op = 3; else if (eat("<")) op = 4; else if (eat(">")) op = 5;
            else return a;
            Fn b = parseAdd();
            a = [a, b, op](const double* v) {
                double x = a(v), y = b(v);
                bool r = op == 0 ? x == y : op == 1 ? x != y : op == 2 ? x <= y
                       : op == 3 ? x >= y : op == 4 ? x < y : x > y;
                return r ? 1.0 : 0.0;
            };
        }
    }
    Fn parseAdd() {
        Fn a = parseMul();
        for (;;) {
            if (eat("+")) { Fn b = parseMul(); a = [a, b](const double* v) { return a(v) + b(v); }; }
            else if (eat("-")) { Fn b = parseMul(); a = [a, b](const double* v) { return a(v) - b(v); }; }
            else return a;
        }
    }
    Fn parseMul() {
        Fn a = parseUnary();
        for (;;) {
            if (eat("*")) { Fn b = parseUnary(); a = [a, b](const double* v) { return a(v) * b(v); }; }
            else if (eat("/")) { Fn b = parseUnary(); a = [a, b](const double* v) { double d = b(v); return d == 0 ? 0.0 : a(v) / d; }; }
            else if (eat("%")) { Fn b = parseUnary(); a = [a, b](const double* v) { double d = b(v); return d == 0 ? 0.0 : std::fmod(a(v), d); }; }
            else return a;
        }
    }
    Fn parseUnary() {
        if (eat("-")) { Fn a = parseUnary(); return [a](const double* v) { return -a(v); }; }
        if (eat("!")) { Fn a = parseUnary(); return [a](const double* v) { return a(v) == 0 ? 1.0 : 0.0; }; }
        if (eat("+")) return parseUnary();
        return parsePrimary();
    }
    Fn parsePrimary() {
        skip();
        if (pos_ >= s_.size()) fail("unexpected end of expression");
        char c = s_[pos_];
        if (c == '(') {
            ++pos_;
            Fn a = parseOr();
            if (!eat(")")) fail("missing ')'");
            return a;
        }
        if (c == '$') {
            ++pos_;
            char t = pos_ < s_.size() ? (char)std::tolower((unsigned char)s_[pos_++]) : 0;
            if (t != 'f' && t != 'i') fail("use $f1..$f9 or $i1..$i9");
            int k = pos_ < s_.size() && std::isdigit((unsigned char)s_[pos_]) ? s_[pos_++] - '0' : 0;
            if (k < 1 || k > 9) fail("use $f1..$f9 or $i1..$i9");
            maxVar_ = std::max(maxVar_, k);
            int idx = k - 1;
            if (t == 'i') return [idx](const double* v) { return std::trunc(v[idx]); };
            return [idx](const double* v) { return v[idx]; };
        }
        if (std::isdigit((unsigned char)c) || c == '.') {
            char* end = nullptr;
            double d = std::strtod(s_.c_str() + pos_, &end);
            pos_ = (size_t)(end - s_.c_str());
            return [d](const double*) { return d; };
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t b = pos_;
            while (pos_ < s_.size() && (std::isalnum((unsigned char)s_[pos_]) || s_[pos_] == '_')) ++pos_;
            std::string name = s_.substr(b, pos_ - b);
            if (!eat("(")) {
                if (name == "pi") return [](const double*) { return dsp::kPi; };
                fail("unknown name '" + name + "'");
            }
            std::vector<Fn> args;
            if (!eat(")")) {
                do args.push_back(parseOr()); while (eat(","));
                if (!eat(")")) fail("missing ')' after arguments of " + name);
            }
            return call(name, args);
        }
        fail(std::string("unexpected '") + c + "'");
    }
    Fn call(const std::string& name, const std::vector<Fn>& a) {
        static const std::map<std::string, double (*)(double)> unary = {
            {"sin", [](double x) { return std::sin(x); }},   {"cos", [](double x) { return std::cos(x); }},
            {"tan", [](double x) { return std::tan(x); }},   {"asin", [](double x) { return std::asin(x); }},
            {"acos", [](double x) { return std::acos(x); }}, {"atan", [](double x) { return std::atan(x); }},
            {"sinh", [](double x) { return std::sinh(x); }}, {"cosh", [](double x) { return std::cosh(x); }},
            {"tanh", [](double x) { return std::tanh(x); }}, {"exp", [](double x) { return std::exp(x); }},
            {"log", [](double x) { return std::log(x); }},   {"log10", [](double x) { return std::log10(x); }},
            {"sqrt", [](double x) { return std::sqrt(x); }}, {"abs", [](double x) { return std::fabs(x); }},
            {"fabs", [](double x) { return std::fabs(x); }}, {"floor", [](double x) { return std::floor(x); }},
            {"ceil", [](double x) { return std::ceil(x); }}, {"int", [](double x) { return std::trunc(x); }},
            {"round", [](double x) { return std::round(x); }},
        };
        static const std::map<std::string, double (*)(double, double)> binary = {
            {"pow", [](double x, double y) { return std::pow(x, y); }},
            {"atan2", [](double x, double y) { return std::atan2(x, y); }},
            {"min", [](double x, double y) { return std::min(x, y); }},
            {"max", [](double x, double y) { return std::max(x, y); }},
            {"fmod", [](double x, double y) { return y == 0 ? 0.0 : std::fmod(x, y); }},
            {"random", [](double lo, double hi) {
                 static std::mt19937 g{std::random_device{}()};
                 return lo + (hi - lo) * std::uniform_real_distribution<double>(0.0, 1.0)(g);
             }},
        };
        if (auto it = unary.find(name); it != unary.end()) {
            if (a.size() != 1) fail(name + "() takes 1 argument");
            auto f = it->second;
            Fn x = a[0];
            return [f, x](const double* v) { return f(x(v)); };
        }
        if (auto it = binary.find(name); it != binary.end()) {
            if (a.size() != 2) fail(name + "() takes 2 arguments");
            auto f = it->second;
            Fn x = a[0], y = a[1];
            return [f, x, y](const double* v) { return f(x(v), y(v)); };
        }
        fail("unknown function '" + name + "'");
    }

    std::string s_;
    size_t pos_ = 0;
    int maxVar_ = 0;
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

// ================================================================ class table

using Factory = std::function<std::unique_ptr<Box>(const Message&)>;

struct ClassDef {
    const char* name;
    const char* group;   // "" = alias (completion only)
    const char* desc;
    Factory make;
};

template <class T>
Factory with() { return [](const Message& a) { return std::make_unique<T>(a); }; }
template <class T>
Factory plain() { return [](const Message&) { return std::make_unique<T>(); }; }
Factory sigop(dsp::Op op) { return [op](const Message& a) { return std::make_unique<SigOpBox>(op, a); }; }
Factory ctlop(BinFn f) { return [f](const Message& a) { return std::make_unique<CtlOpBox>(f, a); }; }
Factory osc(dsp::Osc::Shape s) { return [s](const Message& a) { return std::make_unique<OscBox>(s, a); }; }
Factory unarySig(float (*f)(float)) { return [f](const Message&) { return std::make_unique<UnarySigBox>(f); }; }
Factory unaryCtl(double (*f)(double)) { return [f](const Message&) { return std::make_unique<UnaryCtlBox>(f); }; }

const std::vector<ClassDef>& classDefs() {
    using M = const Message&;
    static const std::vector<ClassDef> t = {
        // oscillators & generators
        {"cycle~", "Oscillators", "cosine oscillator", with<CycleBox>()},
        {"phasor~", "Oscillators", "ramp 0..1", with<PhasorBox>()},
        {"saw~", "Oscillators", "band-limited sawtooth", osc(dsp::Osc::Saw)},
        {"rect~", "Oscillators", "band-limited pulse (freq, duty)", osc(dsp::Osc::Rect)},
        {"tri~", "Oscillators", "triangle (freq, slope)", osc(dsp::Osc::Tri)},
        {"noise~", "Oscillators", "white noise", plain<NoiseBox>()},
        {"click~", "Oscillators", "impulse on bang", with<ClickBox>()},
        {"sig~", "Oscillators", "number to signal", with<SigBox>()},
        {"line~", "Oscillators", "signal ramps: target time ...", with<LineSigBox>()},
        // filters
        {"reson~", "Filters", "resonant bandpass filter", with<ResonBox>()},
        {"lores~", "Filters", "resonant low-pass (cf, res 0-1)", with<LoresBox>()},
        {"svf~", "Filters", "state-variable: low high band notch", with<SvfBox>()},
        {"biquad~", "Filters", "two-pole two-zero filter", with<BiquadBox>()},
        // signal math
        {"+~", "Signal Math", "add signals", sigop(dsp::Op::Add)},
        {"-~", "Signal Math", "subtract signals", sigop(dsp::Op::Sub)},
        {"*~", "Signal Math", "multiply signals", sigop(dsp::Op::Mul)},
        {"/~", "Signal Math", "divide signals", sigop(dsp::Op::Div)},
        {"clip~", "Signal Math", "limit a signal (min, max)", with<ClipBox>()},
        {"abs~", "Signal Math", "absolute value", unarySig([](float x) { return std::fabs(x); })},
        {"tanh~", "Signal Math", "soft saturation", unarySig([](float x) { return std::tanh(x); })},
        // delays
        {"tapin~", "Delay", "delay line input (max ms)", with<TapinBox>()},
        {"tapout~", "Delay", "delay line taps (ms ...)", with<TapoutBox>()},
        {"comb~", "Delay", "comb filter (max, delay, a, b, c)", with<CombBox>()},
        {"allpass~", "Delay", "allpass filter (max, delay, gain)", with<AllpassBox>()},
        // samples
        {"buffer~", "Samples", "named sample memory", with<BufferBox>()},
        {"play~", "Samples", "play a buffer~", with<PlayBox>()},
        {"groove~", "Samples", "variable-speed looping playback", with<GrooveBox>()},
        {"record~", "Samples", "record into a buffer~", with<RecordBox>()},
        {"sfplay~", "Samples", "play a sound file", with<SfplayBox>()},
        // analysis
        {"snapshot~", "Analysis", "signal value as a number", [](M a) { return std::make_unique<ProbeBox>(false, a); }},
        {"peakamp~", "Analysis", "peak amplitude as a number", [](M a) { return std::make_unique<ProbeBox>(true, a); }},
        {"meter~", "Analysis", "level meter", plain<MeterBox>()},
        {"scope~", "Analysis", "oscilloscope", with<ScopeBox>()},
        // audio I/O
        {"adc~", "I/O", "audio input", with<AdcBox>()},
        {"dac~", "I/O", "audio output", with<DacBox>()},
        // timing
        {"metro", "Timing", "repeating bang (ms)", with<MetroBox>()},
        {"delay", "Timing", "delayed bang (ms)", with<DelayBox>()},
        {"line", "Timing", "control ramps: target time [grain]", with<LineBox>()},
        {"loadbang", "Timing", "bang when the patch opens", plain<LoadbangBox>()},
        // control math
        {"+", "Control Math", "add numbers", ctlop([](float a, float b) { return a + b; })},
        {"-", "Control Math", "subtract numbers", ctlop([](float a, float b) { return a - b; })},
        {"*", "Control Math", "multiply numbers", ctlop([](float a, float b) { return a * b; })},
        {"/", "Control Math", "divide numbers", ctlop([](float a, float b) { return b == 0 ? 0.f : a / b; })},
        {"%", "Control Math", "remainder", ctlop([](float a, float b) { return b == 0 ? 0.f : std::fmod(a, b); })},
        {"==", "Control Math", "equal: 1 or 0", ctlop([](float a, float b) { return a == b ? 1.f : 0.f; })},
        {"!=", "Control Math", "not equal: 1 or 0", ctlop([](float a, float b) { return a != b ? 1.f : 0.f; })},
        {">", "Control Math", "greater than: 1 or 0", ctlop([](float a, float b) { return a > b ? 1.f : 0.f; })},
        {"<", "Control Math", "less than: 1 or 0", ctlop([](float a, float b) { return a < b ? 1.f : 0.f; })},
        {">=", "Control Math", "greater or equal: 1 or 0", ctlop([](float a, float b) { return a >= b ? 1.f : 0.f; })},
        {"<=", "Control Math", "less or equal: 1 or 0", ctlop([](float a, float b) { return a <= b ? 1.f : 0.f; })},
        {"mtof", "Control Math", "MIDI note to frequency", unaryCtl(mtof)},
        {"ftom", "Control Math", "frequency to MIDI note", unaryCtl(ftom)},
        {"dbtoa", "Control Math", "decibels to amplitude", unaryCtl(dbtoa)},
        {"atodb", "Control Math", "amplitude to decibels", unaryCtl(atodb)},
        {"scale", "Control Math", "map a range: in lo hi, out lo hi", with<ScaleBox>()},
        {"expr", "Control Math", "expression: $f1 * 2 + sin($f2)", with<ExprBox>()},
        {"random", "Control Math", "random integer below range", with<RandomBox>()},
        {"counter", "Control Math", "count bangs: [min] max", with<CounterBox>()},
        // routing
        {"trigger", "Routing", "ordered outputs, right to left", with<TriggerBox>()},
        {"pack", "Routing", "combine into a list", with<PackBox>()},
        {"unpack", "Routing", "split a list", with<UnpackBox>()},
        {"route", "Routing", "dispatch by first word", with<RouteBox>()},
        {"select", "Routing", "bang on matching values", with<SelectBox>()},
        {"gate", "Routing", "open/close/switch a stream", with<GateBox>()},
        {"int", "Routing", "store an integer", [](M a) { return std::make_unique<StoreBox>(true, a); }},
        {"float", "Routing", "store a float", [](M a) { return std::make_unique<StoreBox>(false, a); }},
        {"send", "Routing", "send to receive objects", with<SendBox>()},
        {"receive", "Routing", "receive sent messages", with<ReceiveBox>()},
        {"print", "Routing", "print to the console", with<PrintBox>()},
        // interface
        {"message", "Interface", "clickable message", with<MessageBox>()},
        {"flonum", "Interface", "float number box", [](M a) { return std::make_unique<NumberBox>(true, a); }},
        {"number", "Interface", "integer number box", [](M a) { return std::make_unique<NumberBox>(false, a); }},
        {"button", "Interface", "click to bang", plain<ButtonBox>()},
        {"toggle", "Interface", "on/off switch", plain<ToggleBox>()},
        {"slider", "Interface", "drag a value: [min max]", with<SliderBox>()},
        {"comment", "Interface", "text note", with<CommentBox>()},
        // Max's short names
        {"t", "", "trigger (short name)", with<TriggerBox>()},
        {"sel", "", "select (short name)", with<SelectBox>()},
        {"del", "", "delay (short name)", with<DelayBox>()},
        {"i", "", "int (short name)", [](M a) { return std::make_unique<StoreBox>(true, a); }},
        {"f", "", "float (short name)", [](M a) { return std::make_unique<StoreBox>(false, a); }},
        {"s", "", "send (short name)", with<SendBox>()},
        {"r", "", "receive (short name)", with<ReceiveBox>()},
    };
    return t;
}

const ClassDef* findClass(const std::string& name) {
    for (auto& c : classDefs())
        if (name == c.name) return &c;
    return nullptr;
}

}  // namespace

std::unique_ptr<Box> makeBox(const std::string& cls, const Message& args) {
    const ClassDef* def = findClass(cls);
    std::unique_ptr<Box> b = def ? def->make(args) : std::make_unique<BrokenBox>();
    b->cls = cls;
    b->args = args;
    if (b->kind == BoxKind::UI) b->uiSize(b->w, b->h);   // sized before it is ever drawn
    return b;
}

const std::vector<ClassInfo>& classInfos() {
    static const std::vector<ClassInfo> v = [] {
        std::vector<ClassInfo> r;
        for (auto& c : classDefs()) r.push_back({c.name, c.group, c.desc});
        return r;
    }();
    return v;
}

std::vector<std::string> allClassNames() {
    std::vector<std::string> v;
    for (auto& c : classDefs()) v.push_back(c.name);
    std::sort(v.begin(), v.end());
    return v;
}

const char* describeClass(const std::string& cls) {
    const ClassDef* c = findClass(cls);
    return c ? c->desc : "";
}

// ============================================================================
// 3. patch
// ============================================================================

Patch::Patch() = default;

Patch::~Patch() {
    timers_.clear();
    std::lock_guard<std::mutex> lk(dspMutex);
    chain_.clear();
    conns.clear();
    boxes.clear();
}

Box* Patch::find(const std::string& id) const {
    for (auto& b : boxes)
        if (b->id == id) return b.get();
    return nullptr;
}

std::string Patch::uniqueId(const std::string& cls) const {
    std::string base;
    for (char c : cls) {
        if (std::isalnum((unsigned char)c)) base += c;
        else if (c == '+') base += "add";
        else if (c == '-') base += "sub";
        else if (c == '*') base += "mul";
        else if (c == '/') base += "div";
    }
    if (base.empty()) base = "obj";
    for (int i = 1;; ++i) {
        std::string id = base + std::to_string(i);
        if (!find(id)) return id;
    }
}

Box* Patch::addBox(std::unique_ptr<Box> b, int x, int y, const std::string& id) {
    b->patch = this;
    b->x = x;
    b->y = y;
    b->id = (id.empty() || find(id)) ? uniqueId(b->cls) : id;
    Box* raw = b.get();
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        boxes.push_back(std::move(b));
        compile();
    }
    raw->attached();
    if (onStructure) onStructure();
    return raw;
}

bool canConnect(const std::vector<Connection>& conns, Box* from, int o, Box* to, int i,
                std::string* err) {
    auto fail = [&](const char* what) {
        if (err) *err = what;
        return false;
    };
    if (!from || !to) return fail("no such box");
    if (from == to) return fail("can't connect a box to itself");
    if (o < 0 || o >= from->numOuts() || i < 0 || i >= to->numIns()) return fail("no such port");
    if (from->outs[o] == Port::Signal && to->ins[i] == Port::Control)
        return fail("a signal outlet can't connect to a control inlet");
    if (from->outs[o] == Port::Tap && (to->cls != "tapout~" || i != 0))
        return fail("tapin~ connects only to the left inlet of tapout~");
    for (auto& c : conns)
        if (c.from == from && c.outlet == o && c.to == to && c.inlet == i)
            return fail("already connected");
    return true;
}

bool Patch::reaches(Box* a, Box* b) const {
    std::vector<Box*> todo{a};
    std::set<Box*> seen;
    while (!todo.empty()) {
        Box* x = todo.back();
        todo.pop_back();
        if (x == b) return true;
        if (!seen.insert(x).second) continue;
        for (auto& c : conns)
            if (c.from == x && x->outs[c.outlet] == Port::Signal) todo.push_back(c.to);
    }
    return false;
}

bool Patch::connect(Box* from, int o, Box* to, int i, std::string* err) {
    if (!canConnect(conns, from, o, to, i, err)) return false;
    if (from->outs[o] == Port::Signal && reaches(to, from)) {
        if (err) *err = "that would create a signal feedback loop";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        conns.push_back({from, o, to, i});
        compile();
    }
    if (onStructure) onStructure();
    return true;
}

void Patch::removeConnections(const std::vector<int>& idx) {
    std::vector<int> s = idx;
    std::sort(s.rbegin(), s.rend());
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        for (int k : s)
            if (k >= 0 && k < (int)conns.size()) conns.erase(conns.begin() + k);
        compile();
    }
    if (onStructure) onStructure();
}

void Patch::removeBoxes(const std::vector<Box*>& bs) {
    std::set<Box*> dead(bs.begin(), bs.end());
    for (Box* b : bs) cancel(b);
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        conns.erase(std::remove_if(conns.begin(), conns.end(),
                                   [&](const Connection& c) {
                                       return dead.count(c.from) || dead.count(c.to);
                                   }),
                    conns.end());
        boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                                   [&](const std::unique_ptr<Box>& b) {
                                       return dead.count(b.get()) > 0;
                                   }),
                    boxes.end());
        compile();
    }
    if (onStructure) onStructure();
}

Box* Patch::replaceBox(Box* old, std::unique_ptr<Box> nb) {
    nb->patch = this;
    nb->id = old->id;
    nb->x = old->x;
    nb->y = old->y;
    Box* raw = nb.get();
    std::unique_ptr<Box> doomed;
    cancel(old);
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        for (auto& b : boxes)
            if (b.get() == old) {
                doomed = std::move(b);
                b = std::move(nb);
                break;
            }
        std::vector<Connection> kept;
        for (auto c : conns) {
            if (c.from == old) c.from = raw;
            if (c.to == old) c.to = raw;
            if (canConnect(kept, c.from, c.outlet, c.to, c.inlet, nullptr)) kept.push_back(c);
        }
        conns = std::move(kept);
        compile();
    }
    raw->attached();
    if (onStructure) onStructure();
    return raw;
}

void Patch::clear() {
    timers_.clear();
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        chain_.clear();
        conns.clear();
        boxes.clear();
        compile();
    }
    if (onStructure) onStructure();
}

bool Patch::needsInput() const {
    for (auto& b : boxes)
        if (b->wantsInput()) return true;
    return false;
}

// Orders signal boxes so every box runs after the boxes feeding it, and
// points each signal inlet at the output buffers connected to it.
void Patch::compile() {
    std::vector<Box*> sig;
    std::map<Box*, int> indeg;
    for (auto& b : boxes) {
        for (auto& s : b->inSrc) s.clear();
        if (b->isSignal()) {
            sig.push_back(b.get());
            indeg[b.get()] = 0;
        }
    }
    for (auto& c : conns) {
        if (c.from->outs[c.outlet] != Port::Signal) continue;   // control and tap links
        c.to->inSrc[c.inlet].push_back(c.from->sigOut(c.outlet));
        indeg[c.to]++;
    }
    std::vector<Box*> order, ready;
    for (Box* b : sig)
        if (indeg[b] == 0) ready.push_back(b);
    while (!ready.empty()) {
        Box* b = ready.front();
        ready.erase(ready.begin());
        order.push_back(b);
        for (auto& c : conns)
            if (c.from == b && b->outs[c.outlet] == Port::Signal && --indeg[c.to] == 0)
                ready.push_back(c.to);
    }
    if (order.size() != sig.size()) {
        dspError = "signal feedback loop: audio disabled for this patch";
        chain_.clear();
    } else {
        dspError.clear();
        chain_ = std::move(order);
    }
    for (auto& b : boxes) b->resolve();
}

void Patch::rebuild() {
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        compile();
    }
    if (onStructure) onStructure();
}

void Patch::print(const std::string& s) {
    if (onPrint) onPrint(s);
    else std::fprintf(stderr, "%s\n", s.c_str());
}

std::string Patch::resolvePath(const std::string& p) const {
    if (p.empty() || p[0] == '/') return p;
    if (p.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : "") + p.substr(1);
    }
    return dir.empty() ? p : dir + "/" + p;
}

// ---- scheduler: a small timer queue, run by the GUI (or by --render)

void Patch::scheduleAt(Box* owner, double t, std::function<void()> fn) {
    timers_.push_back({t, timerSeq_++, owner, std::move(fn)});
    if (!inRunDue_ && onSchedule) onSchedule();
}

void Patch::cancel(Box* owner) {
    size_t before = timers_.size();
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [&](const Timer& t) { return t.owner == owner; }),
                  timers_.end());
    if (timers_.size() != before && !inRunDue_ && onSchedule) onSchedule();
}

double Patch::nextDue() const {
    double t = std::numeric_limits<double>::infinity();
    for (auto& x : timers_) t = std::min(t, x.t);
    return t;
}

void Patch::runDue(double now) {
    inRunDue_ = true;
    for (int guard = 0; guard < 100000; ++guard) {
        int best = -1;
        for (int k = 0; k < (int)timers_.size(); ++k)
            if (timers_[k].t <= now &&
                (best < 0 || timers_[k].t < timers_[best].t ||
                 (timers_[k].t == timers_[best].t && timers_[k].seq < timers_[best].seq)))
                best = k;
        if (best < 0) break;
        auto fn = std::move(timers_[best].fn);
        timers_.erase(timers_.begin() + best);
        fn();
    }
    inRunDue_ = false;
    if (onSchedule) onSchedule();
}

void Patch::process(const float* in, int inCh, float* out, int outCh, int frames) {
    std::fill(out, out + (size_t)frames * outCh, 0.f);
    std::unique_lock<std::mutex> lk(dspMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;                     // GUI is editing: one silent block
    ioIn = in;
    ioOut = out;
    ioInCh = in ? inCh : 0;
    ioOutCh = outCh;
    for (int off = 0; off < frames; off += dsp::kBlock) {
        int n = std::min(dsp::kBlock, frames - off);
        ioOffset = off;
        for (Box* b : chain_) b->dsp(n);
        ++blockCount;
    }
    for (size_t k = 0, N = (size_t)frames * outCh; k < N; ++k)
        out[k] = std::isfinite(out[k]) ? std::clamp(out[k], -1.f, 1.f) : 0.f;
    ioIn = nullptr;
    ioOut = nullptr;
}

void Patch::sendTo(const std::string& name, const Message& m) {
    if (m.empty()) return;
    if (name == "dsp") {                       // "; dsp start" / "; dsp stop"
        if (m[0].isFloat()) { if (onDsp) onDsp(m[0].f != 0.f); }
        else handleDspMessage(this, m);
        return;
    }
    std::vector<ReceiveBox*> rs;
    for (auto& b : boxes)
        if (auto r = dynamic_cast<ReceiveBox*>(b.get()))
            if (r->name == name) rs.push_back(r);
    if (depth > 256) { status("stack overflow: message loop via " + name); return; }
    ++depth;
    for (auto r : rs) r->receive(m);
    --depth;
}

void Patch::loadbangAll() {
    for (size_t i = 0; i < boxes.size(); ++i) boxes[i]->loadbang();
}

// ---- file format
//
//   (patch
//     (obj osc cycle~ 440 (pos 40 40))
//     (obj p1 message "; freq 880" (pos 200 40))
//     (connect osc 0 gain 0))
//
// `pos` is optional; boxes without it are stacked down the left side.

std::string Patch::serialize() const {
    std::ostringstream o;
    o << "(patch";
    for (auto& b : boxes) {
        o << "\n  (obj " << quoteSymbol(b->id) << ' ' << quoteSymbol(b->cls);
        for (auto& a : b->saveArgs()) o << ' ' << writeAtom(a);
        o << " (pos " << b->x << ' ' << b->y << "))";
    }
    for (auto& c : conns)
        o << "\n  (connect " << quoteSymbol(c.from->id) << ' ' << c.outlet << ' '
          << quoteSymbol(c.to->id) << ' ' << c.inlet << ')';
    o << ")\n";
    return o.str();
}

bool Patch::load(const std::string& src, std::string& err) {
    std::vector<SExpr> top;
    if (!parseSExprs(src, top, err)) return false;
    if (top.size() != 1 || !top[0].isList || top[0].items.empty() ||
        top[0].items[0].isList || top[0].items[0].text != "patch") {
        err = "not a patch: expected a single (patch ...) form";
        return false;
    }
    std::vector<std::unique_ptr<Box>> nb;
    std::vector<Connection> nc;
    std::map<std::string, Box*> byId;
    std::vector<std::string> warnings;
    int autoY = 20;
    auto headIs = [](const SExpr& e, const char* h) {
        return e.isList && !e.items.empty() && !e.items[0].isList && e.items[0].text == h;
    };

    for (size_t k = 1; k < top[0].items.size(); ++k) {
        const SExpr& e = top[0].items[k];
        std::string where = "line " + std::to_string(e.line) + ": ";
        if (headIs(e, "obj")) {
            if (e.items.size() < 3 || e.items[1].isList || e.items[2].isList) {
                err = where + "expected (obj id class args...)";
                return false;
            }
            std::string id = e.items[1].text, cls = e.items[2].text;
            Message args;
            int x = 20, y = -1;
            for (size_t j = 3; j < e.items.size(); ++j) {
                const SExpr& a = e.items[j];
                if (headIs(a, "pos") && a.items.size() >= 3) {
                    x = (int)std::atof(a.items[1].text.c_str());
                    y = (int)std::atof(a.items[2].text.c_str());
                } else if (!a.isList) {
                    args.push_back(sexprAtom(a));
                }
            }
            if (cls == "message" || cls == "comment" || cls == "expr")
                args = {Atom(messageToString(args))};
            if (y < 0) { y = autoY; autoY += 40; }
            if (byId.count(id)) {
                err = where + "duplicate id '" + id + "'";
                return false;
            }
            auto b = makeBox(cls, args);
            b->patch = this;
            b->id = id;
            b->x = x;
            b->y = y;
            if (b->kind == BoxKind::Broken)
                warnings.push_back(where + "unknown object '" + cls + "'");
            byId[id] = b.get();
            nb.push_back(std::move(b));
        } else if (headIs(e, "connect")) {
            if (e.items.size() != 5) {
                err = where + "expected (connect from outlet to inlet)";
                return false;
            }
            Box* from = byId.count(e.items[1].text) ? byId[e.items[1].text] : nullptr;
            Box* to = byId.count(e.items[3].text) ? byId[e.items[3].text] : nullptr;
            int o = std::atoi(e.items[2].text.c_str()), i = std::atoi(e.items[4].text.c_str());
            std::string why;
            if (canConnect(nc, from, o, to, i, &why)) nc.push_back({from, o, to, i});
            else warnings.push_back(where + "connection skipped (" + why + ")");
        } else {
            warnings.push_back(where + "ignored unknown form");
        }
    }

    std::vector<std::unique_ptr<Box>> old;
    timers_.clear();
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        old.swap(boxes);
        conns = std::move(nc);
        boxes = std::move(nb);
        compile();
        old.clear();
    }
    for (size_t k = 0; k < boxes.size(); ++k) boxes[k]->attached();
    err.clear();
    for (auto& w : warnings) err += (err.empty() ? "" : "\n") + w;
    if (onStructure) onStructure();
    return true;
}

// ============================================================================
// 4. audio engine
// ============================================================================

AudioEngine::AudioEngine(Patch& p) : patch_(p) {
    contextOk_ = ma_context_init(nullptr, 0, nullptr, &context_) == MA_SUCCESS;
    refreshDevices();
}

AudioEngine::~AudioEngine() {
    stop();
    if (contextOk_) ma_context_uninit(&context_);
}

void AudioEngine::refreshDevices() {
    outputs.clear();
    inputs.clear();
    if (!contextOk_) return;
    ma_device_info *pb = nullptr, *cap = nullptr;
    ma_uint32 npb = 0, ncap = 0;
    if (ma_context_get_devices(&context_, &pb, &npb, &cap, &ncap) != MA_SUCCESS) return;
    for (ma_uint32 i = 0; i < npb; ++i) outputs.push_back({pb[i].name, pb[i].id});
    for (ma_uint32 i = 0; i < ncap; ++i) inputs.push_back({cap[i].name, cap[i].id});
}

void AudioEngine::callback(ma_device* d, void* out, const void* in, ma_uint32 frames) {
    static thread_local bool ftz = (dsp::flushDenormals(), true);
    (void)ftz;
    auto* self = static_cast<AudioEngine*>(d->pUserData);
    self->patch_.process(static_cast<const float*>(in), (int)d->capture.channels,
                         static_cast<float*>(out), (int)d->playback.channels, (int)frames);
}

bool AudioEngine::start(std::string& err) {
    stop();
    if (!contextOk_) {
        err = "could not initialise the audio system";
        return false;
    }
    bool wantIn = patch_.needsInput() && settings.input != -2;
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool duplex = wantIn && attempt == 0;
        ma_device_config cfg =
            ma_device_config_init(duplex ? ma_device_type_duplex : ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        if (settings.output >= 0 && settings.output < (int)outputs.size())
            cfg.playback.pDeviceID = &outputs[settings.output].id;
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 2;
        if (settings.input >= 0 && settings.input < (int)inputs.size())
            cfg.capture.pDeviceID = &inputs[settings.input].id;
        cfg.sampleRate = (ma_uint32)std::max(0, settings.sampleRate);
        cfg.periodSizeInFrames = (ma_uint32)std::max(16, settings.bufferFrames);
        cfg.dataCallback = callback;
        cfg.pUserData = this;
        if (ma_device_init(&context_, &cfg, &device_) != MA_SUCCESS) {
            err = duplex ? "could not open audio input; running output only"
                         : "could not open the audio output device";
            if (duplex) continue;
            return false;
        }
        patch_.sampleRate = (double)device_.sampleRate;
        if (ma_device_start(&device_) != MA_SUCCESS) {
            ma_device_uninit(&device_);
            err = "could not start the audio device";
            return false;
        }
        running_ = true;
        duplex_ = duplex;
        patch_.dspRunning = true;
        if (attempt == 0) err.clear();
        return true;
    }
    return false;
}

void AudioEngine::stop() {
    if (!running_) return;
    ma_device_uninit(&device_);
    running_ = false;
    duplex_ = false;
    patch_.dspRunning = false;
}

void AudioEngine::updateInputNeed() {
    if (!running_) return;
    bool want = patch_.needsInput() && settings.input != -2;
    if (want != duplex_) {
        std::string err;
        start(err);
    }
}

// ============================================================================
// 5. GUI
// ============================================================================

namespace {

double segDist(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy;
    double t = L > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / L, 0.0, 1.0) : 0.0;
    double cx = ax + t * dx - px, cy = ay + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

void flashTimer(void* data) { static_cast<Canvas*>(data)->redraw(); }

// Text field for box editing. While the completion popup is open it takes
// Up/Down (choose), Tab (complete, keep typing) and Enter (complete and
// create); every other key edits the text and refreshes the suggestions.
class CompletingInput : public Fl_Input {
public:
    CompletingInput(int x, int y, int w, int h, Canvas* c) : Fl_Input(x, y, w, h), canvas_(c) {}
    int handle(int e) override {
        if (e == FL_KEYBOARD && canvas_->hasSuggestions()) {
            switch (Fl::event_key()) {
                case FL_Down: canvas_->moveSuggestion(1); return 1;
                case FL_Up: canvas_->moveSuggestion(-1); return 1;
                case FL_Tab: canvas_->acceptSuggestion(false); return 1;
                case FL_Escape: canvas_->clearSuggestions(); return 1;
                case FL_Enter:
                case FL_KP_Enter:
                    if (canvas_->acceptSuggestion(true)) return 1;
                    break;
            }
        }
        int r = Fl_Input::handle(e);
        if (e == FL_KEYBOARD || e == FL_PASTE) canvas_->updateSuggestions();
        return r;
    }

private:
    Canvas* canvas_;
};
}  // namespace

Canvas::Canvas(int x, int y, int w, int h, MainWindow* win)
    : Fl_Group(x, y, w, h), win_(win) {
    box(FL_FLAT_BOX);
    editor_ = new CompletingInput(x, y, 120, kBoxH, this);
    editor_->textfont(kFont);
    editor_->textsize(kFontSize);
    editor_->box(FL_FLAT_BOX);
    editor_->color(kEditBg);
    editor_->textcolor(kText);
    editor_->cursor_color(kAccent);
    editor_->selection_color(fl_rgb_color(60, 100, 170));
    editor_->when(FL_WHEN_ENTER_KEY_ALWAYS | FL_WHEN_RELEASE_ALWAYS);
    editor_->callback(editCb, this);
    editor_->hide();
    end();
}

void Canvas::layoutBox(Box* b) const {
    fl_font(kFont, kFontSize);
    std::string t = b->text();
    int tw = (int)fl_width(t.c_str());
    int ports = std::max(b->numIns(), b->numOuts());
    b->h = kBoxH;
    if (b->kind == BoxKind::UI) b->uiSize(b->w, b->h);
    else if (b->kind == BoxKind::Number) b->w = std::max(56, tw + 24);
    else if (b->kind == BoxKind::Message) b->w = std::max(36, tw + 20);
    else if (b->kind == BoxKind::Comment) b->w = std::max(40, tw + 12);
    else b->w = std::max({tw + 16, ports * (kPortW + 8) + 2 * kPortInset, 40});
}

void Canvas::portPos(Box* b, bool out, int i, int& px, int& py) const {
    int n = out ? b->numOuts() : b->numIns();
    int span = b->w - kPortW - 2 * kPortInset;
    px = b->x + kPortInset + (n <= 1 ? 0 : i * span / (n - 1));
    py = out ? b->y + b->h : b->y;
}

void Canvas::drawBox(Box* b) {
    int X = x() + b->x, Y = y() + b->y, W = b->w, H = b->h;
    bool flash = b->flashUntil > now();

    if (b->kind == BoxKind::Comment) {               // comments: just text
        if (b->selected || !locked_) {
            fl_color(b->selected ? kSelect : kGrid);
            fl_line_style(FL_DOT, 1);
            fl_rounded_rect(X, Y, W, H, 4);
            fl_line_style(0);
        }
        if (editing_ != b) {
            fl_font(kFont, kFontSize);
            fl_color(kDim);
            fl_draw(b->text().c_str(), X + 6, Y + H / 2 + fl_height() / 2 - fl_descent());
        }
        return;
    }

    // message boxes are pills, everything else a softly rounded box
    int r = b->kind == BoxKind::Message ? H / 2 : kRadius;
    Fl_Color fill = kBoxFill;
    if (b->kind == BoxKind::Message) fill = flash ? kFlash : kMsgFill;
    else if (b->kind == BoxKind::Number || b->kind == BoxKind::UI) fill = kNumFill;
    else if (b->kind == BoxKind::Broken) fill = kBroken;
    fl_color(fill);
    fl_rounded_rectf(X, Y, W, H, r);

    if (b->kind == BoxKind::UI) {
        fl_push_clip(X, Y, W, H);
        b->drawUI(X, Y);
        fl_pop_clip();
    }

    // border
    if (b->selected) {
        fl_color(kSelect);
        fl_line_style(FL_SOLID, 2);
    } else if (b->kind == BoxKind::Broken) {
        fl_color(fl_rgb_color(230, 80, 80));
        fl_line_style(FL_DASH, 1);
    } else {
        fl_color(b->isSignal() ? fl_color_average(kSignal, kBorder, 0.25f) : kBorder);
        fl_line_style(FL_SOLID, 1);
    }
    fl_rounded_rect(X, Y, W, H, r);
    fl_line_style(0);

    // text
    if (editing_ != b && b->kind != BoxKind::UI) {
        fl_font(kFont, kFontSize);
        int tx = X + (b->kind == BoxKind::Message ? 10 : 8);
        std::string t = b->text();
        if (b->kind == BoxKind::Number) {
            fl_color(kAccent);
            fl_polygon(X + 6, Y + 7, X + 11, Y + H / 2, X + 6, Y + H - 7);
            tx = X + 16;
            if (typingBox_ == b && !typed_.empty()) t = typed_ + "_";
        }
        fl_color(kText);
        fl_push_clip(X + 2, Y + 1, W - 4, H - 2);
        fl_draw(t.c_str(), tx, Y + H / 2 + fl_height() / 2 - fl_descent());
        fl_pop_clip();
    }

    // ports: small tabs on the top (inlets) and bottom (outlets) edges
    for (int k = 0; k < b->numIns(); ++k) {
        int px, py;
        portPos(b, false, k, px, py);
        fl_color(b->ins[k] == Port::Control ? kCtlPort : kSignal);
        fl_rounded_rectf(x() + px, y() + py, kPortW, kPortH + 1, 1);
    }
    for (int k = 0; k < b->numOuts(); ++k) {
        int px, py;
        portPos(b, true, k, px, py);
        fl_color(b->outs[k] == Port::Control ? kCtlPort : kSignal);
        fl_rounded_rectf(x() + px, y() + py - kPortH - 1, kPortW, kPortH + 1, 1);
    }
}

// ---- completion popup

void Canvas::updateSuggestions() {
    sugg_.clear();
    suggSel_ = 0;
    if (editing_ && editing_->kind != BoxKind::Message && editing_->kind != BoxKind::Comment) {
        std::string t = editor_->value();
        if (!t.empty() && t.find(' ') == std::string::npos) {
            std::vector<std::string> exact, prefix, inner;
            for (auto& c : allClassNames()) {
                if (c == t) exact.push_back(c);
                else if (c.compare(0, t.size(), t) == 0) prefix.push_back(c);
                else if (c.find(t) != std::string::npos) inner.push_back(c);
            }
            for (auto* v : {&exact, &prefix, &inner})
                for (auto& c : *v)
                    if ((int)sugg_.size() < kSuggMax) sugg_.push_back(c);
            // nothing left to complete
            if (sugg_.size() == 1 && sugg_[0] == t) sugg_.clear();
        }
    }
    redraw();
}

void Canvas::moveSuggestion(int d) {
    if (sugg_.empty()) return;
    int n = (int)sugg_.size();
    suggSel_ = ((suggSel_ + d) % n + n) % n;
    redraw();
}

void Canvas::clearSuggestions() {
    sugg_.clear();
    redraw();
}

bool Canvas::acceptSuggestion(bool commit) {
    if (sugg_.empty() || !editing_) return false;
    std::string t = editor_->value(), s = sugg_[suggSel_];
    sugg_.clear();
    if (commit && t == s) return false;          // already complete: plain Enter
    std::string v = commit ? s : s + " ";
    editor_->value(v.c_str());
    editor_->insert_position((int)v.size());
    if (commit) commitEdit();
    else redraw();
    return true;
}

void Canvas::drawSuggestions() {
    if (!editing_ || sugg_.empty() || !editor_->visible()) return;
    fl_font(kFont, kFontSize);
    int nameW = 0;
    for (auto& s : sugg_) nameW = std::max(nameW, (int)fl_width(s.c_str()));
    int descW = 0;
    for (auto& s : sugg_) descW = std::max(descW, (int)fl_width(describeClass(s)));
    suggW_ = std::max(editor_->w(), nameW + descW + 40);
    int H = (int)sugg_.size() * kSuggRowH + 6;
    suggX_ = editor_->x();
    suggY_ = editor_->y() + editor_->h() + 3;
    if (suggY_ + H > y() + h()) suggY_ = editor_->y() - H - 3;   // flip above near the bottom

    fl_color(fl_rgb_color(10, 10, 12));                              // soft shadow
    fl_rounded_rectf(suggX_ + 2, suggY_ + 3, suggW_, H, 6);
    fl_color(kPopup);
    fl_rounded_rectf(suggX_, suggY_, suggW_, H, 6);
    fl_color(kBorder);
    fl_rounded_rect(suggX_, suggY_, suggW_, H, 6);
    for (int k = 0; k < (int)sugg_.size(); ++k) {
        int ry = suggY_ + 3 + k * kSuggRowH;
        if (k == suggSel_) {
            fl_color(fl_rgb_color(52, 92, 160));
            fl_rounded_rectf(suggX_ + 3, ry, suggW_ - 6, kSuggRowH, 4);
        }
        int base = ry + kSuggRowH / 2 + fl_height() / 2 - fl_descent();
        fl_color(kText);
        fl_draw(sugg_[k].c_str(), suggX_ + 10, base);
        fl_color(k == suggSel_ ? fl_rgb_color(200, 215, 240) : kDim);
        fl_draw(describeClass(sugg_[k]), suggX_ + 26 + nameW, base);
    }
}

int Canvas::suggestionAt(int ex, int ey) const {
    if (sugg_.empty()) return -1;
    int H = (int)sugg_.size() * kSuggRowH + 6;
    if (ex < suggX_ || ex > suggX_ + suggW_ || ey < suggY_ + 3 || ey >= suggY_ + H - 3) return -1;
    return std::min((int)sugg_.size() - 1, (ey - suggY_ - 3) / kSuggRowH);
}

// ---- option-drag duplication

void Canvas::duplicateSelection() {
    auto sel = selectedBoxes();
    if (sel.empty()) return;
    win_->pushUndo();
    Patch& p = win_->patch;
    std::map<Box*, Box*> copy;
    for (Box* b : sel) copy[b] = p.addBox(makeBox(b->cls, b->saveArgs()), b->x, b->y);
    auto conns = p.conns;               // connect() appends to p.conns
    for (auto& c : conns)
        if (copy.count(c.from) && copy.count(c.to))
            p.connect(copy[c.from], c.outlet, copy[c.to], c.inlet);
    clearSelection();
    for (auto& kv : copy) kv.second->selected = true;
    win_->markDirty();
}

void Canvas::draw() {
    Patch& p = win_->patch;
    fl_push_clip(x(), y(), w(), h());
    fl_color(locked_ ? kBgLocked : kBgEdit);
    fl_rectf(x(), y(), w(), h());
    if (!locked_) {
        fl_color(kGrid);
        for (int gx = 15; gx < w(); gx += 15)
            for (int gy = 15; gy < h(); gy += 15) fl_point(x() + gx, y() + gy);
    }

    for (auto& b : p.boxes) layoutBox(b.get());

    // cables
    for (auto& c : p.conns) {
        int ax, ay, bx, by;
        portPos(c.from, true, c.outlet, ax, ay);
        portPos(c.to, false, c.inlet, bx, by);
        ax += x() + kPortW / 2; bx += x() + kPortW / 2;
        ay += y(); by += y();
        bool sig = c.from->outs[c.outlet] != Port::Control;   // signal and tap links
        if (sig) {
            fl_color(c.selected ? kSelect : kSignal);
            fl_line_style(FL_SOLID | FL_CAP_ROUND, 3);
            fl_line(ax, ay, bx, by);
            fl_color(c.selected ? fl_rgb_color(30, 70, 140) : kSignalDim);
            fl_line_style(FL_DASH, 1);
            fl_line(ax, ay, bx, by);
        } else {
            fl_color(c.selected ? kSelect : kCable);
            fl_line_style(FL_SOLID | FL_CAP_ROUND, c.selected ? 2 : 1);
            fl_line(ax, ay, bx, by);
        }
        fl_line_style(0);
    }

    bool flashing = false, animating = false;
    for (auto& b : p.boxes) {
        drawBox(b.get());
        if (b->flashUntil > now()) flashing = true;
        if (b->animated()) animating = true;
    }
    if ((flashing || animating) && !Fl::has_timeout(flashTimer, this))
        Fl::add_timeout(animating ? 1.0 / 30.0 : 0.05, flashTimer, this);

    if (drag_ == Drag::Cable && dragBox_) {
        int ax, ay;
        portPos(dragBox_, true, dragPort_, ax, ay);
        fl_color(kSelect);
        fl_line_style(FL_SOLID, 2);
        fl_line(x() + ax + kPortW / 2, y() + ay, x() + curX_, y() + curY_);
        fl_line_style(0);
    } else if (drag_ == Drag::Rubber) {
        fl_color(kSelect);
        fl_line_style(FL_DOT, 1);
        int rx = std::min(pressX_, curX_), ry = std::min(pressY_, curY_);
        fl_rect(x() + rx, y() + ry, std::abs(curX_ - pressX_), std::abs(curY_ - pressY_));
        fl_line_style(0);
    }

    if (editor_->visible()) {
        draw_child(*editor_);
        fl_color(kAccent);                       // outline: the box being typed into
        fl_rounded_rect(editor_->x() - 1, editor_->y() - 1, editor_->w() + 2, editor_->h() + 2, 3);
    }
    drawSuggestions();
    fl_pop_clip();
}

Canvas::Hit Canvas::hitTest(int mx, int my) const {
    Hit h;
    auto& boxes = win_->patch.boxes;
    for (auto it = boxes.rbegin(); it != boxes.rend(); ++it) {
        Box* b = it->get();
        if (mx < b->x - 2 || mx > b->x + b->w + 2 || my < b->y - 2 || my > b->y + b->h + 2)
            continue;
        h.box = b;
        h.what = Hit::Body;
        if (my >= b->y + b->h - 5) {
            for (int k = 0; k < b->numOuts(); ++k) {
                int px, py;
                portPos(b, true, k, px, py);
                if (mx >= px - 3 && mx <= px + kPortW + 3) {
                    h.what = Hit::Outlet;
                    h.port = k;
                    break;
                }
            }
        }
        return h;
    }
    auto& conns = win_->patch.conns;
    for (int k = (int)conns.size() - 1; k >= 0; --k) {
        auto& c = conns[k];
        int ax, ay, bx, by;
        portPos(c.from, true, c.outlet, ax, ay);
        portPos(c.to, false, c.inlet, bx, by);
        if (segDist(mx, my, ax + kPortW / 2, ay, bx + kPortW / 2, by) < 4.0) {
            h.what = Hit::Cable;
            h.cable = k;
            return h;
        }
    }
    return h;
}

int Canvas::inletAt(Box* b, int mx) const {
    int best = -1, bestD = 1 << 30;
    for (int k = 0; k < b->numIns(); ++k) {
        int px, py;
        portPos(b, false, k, px, py);
        int d = std::abs(mx - (px + kPortW / 2));
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

void Canvas::clearSelection() {
    for (auto& b : win_->patch.boxes) b->selected = false;
    for (auto& c : win_->patch.conns) c.selected = false;
}

std::vector<Box*> Canvas::selectedBoxes() const {
    std::vector<Box*> s;
    for (auto& b : win_->patch.boxes)
        if (b->selected) s.push_back(b.get());
    return s;
}

void Canvas::selectAll() {
    if (locked_) return;
    for (auto& b : win_->patch.boxes) b->selected = true;
    redraw();
}

void Canvas::reset() {
    if (editor_->visible()) editor_->hide();
    editing_ = nullptr;
    sugg_.clear();
    dragBox_ = nullptr;
    typingBox_ = nullptr;
    typed_.clear();
    drag_ = Drag::None;
    redraw();
}

void Canvas::setLocked(bool l) {
    commitEdit();
    locked_ = l;
    clearSelection();
    typingBox_ = nullptr;
    typed_.clear();
    redraw();
    win_->refreshStatus();
}

void Canvas::beginEdit(Box* b, bool isNew) {
    editing_ = b;
    editingNew_ = isNew;
    sugg_.clear();
    std::string t = b->text();
    bool content = b->kind == BoxKind::Message || b->kind == BoxKind::Comment;
    if (isNew && !content && !t.empty()) t += " ";
    editor_->resize(x() + b->x, y() + b->y, std::max(b->w, 160), kBoxH);
    editor_->value(t.c_str());
    editor_->show();
    editor_->take_focus();
    editor_->insert_position((int)t.size());
    redraw();
}

void Canvas::editCb(Fl_Widget*, void* data) { static_cast<Canvas*>(data)->commitEdit(); }

void Canvas::commitEdit() {
    if (!editing_) return;
    Box* b = editing_;
    editing_ = nullptr;
    sugg_.clear();
    std::string t = editor_->value();
    editor_->hide();
    // trim
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    Patch& p = win_->patch;

    if (b->kind == BoxKind::Comment && t.empty() && editingNew_) {
        p.removeBoxes({b});
        win_->dropUndo();
    } else if (b->kind == BoxKind::Message || b->kind == BoxKind::Comment) {
        // message and comment boxes keep their text verbatim
        if (t != b->text()) {
            if (!editingNew_) win_->pushUndo();
            p.replaceBox(b, makeBox(b->cls, {Atom(t)}));
            win_->markDirty();
        }
    } else if (t.empty()) {
        if (editingNew_) {
            p.removeBoxes({b});
            win_->dropUndo();
        }
    } else if (editingNew_ || t != b->text()) {
        if (!editingNew_) win_->pushUndo();
        Message m = parseMessage(t);
        std::string cls = m[0].str();
        Box* nb = p.replaceBox(b, makeBox(cls, Message(m.begin() + 1, m.end())));
        if (nb->kind == BoxKind::Broken) win_->setStatus("unknown object: " + cls);
        win_->markDirty();
    }
    Fl::focus(this);
    redraw();
}

void Canvas::cancelEdit() {
    if (!editing_) return;
    Box* b = editing_;
    editing_ = nullptr;
    sugg_.clear();
    editor_->hide();
    if (editingNew_ && b->kind != BoxKind::Message) {
        win_->patch.removeBoxes({b});
        win_->dropUndo();
    }
    Fl::focus(this);
    redraw();
}

void Canvas::newBoxAt(const std::string& cls, int bx, int by) {
    commitEdit();
    if (locked_) setLocked(false);
    clearSelection();
    win_->pushUndo();
    Patch& p = win_->patch;
    Box* b = p.addBox(makeBox(cls, {}), std::max(0, bx), std::max(0, by));
    layoutBox(b);
    win_->markDirty();
    if (b->kind == BoxKind::Number || b->kind == BoxKind::UI) {
        b->selected = true;                 // ready to use, nothing to type
        redraw();
    } else {
        beginEdit(b, true);
    }
}

void Canvas::newBoxAtMouse(const std::string& cls) { newBoxAt(cls, mouseX_, mouseY_); }

void Canvas::deleteSelection() {
    Patch& p = win_->patch;
    std::vector<int> cables;
    for (int k = 0; k < (int)p.conns.size(); ++k)
        if (p.conns[k].selected) cables.push_back(k);
    auto bs = selectedBoxes();
    if (cables.empty() && bs.empty()) return;
    win_->pushUndo();
    if (!cables.empty()) p.removeConnections(cables);
    if (!bs.empty()) p.removeBoxes(bs);
    dragBox_ = typingBox_ = nullptr;
    win_->markDirty();
    redraw();
}

void Canvas::numberDrag(int dy) {
    auto nb = dynamic_cast<NumberBox*>(dragBox_);
    if (!nb) return;
    double v = dragStartValue_ + dy * dragStep_;
    if (dragStep_ < 1.0) v = std::round(v / dragStep_) * dragStep_;
    nb->set(v, true);
}

int Canvas::handle(int e) {
    switch (e) {
        case FL_ENTER:
        case FL_LEAVE:
        case FL_FOCUS:
        case FL_UNFOCUS:
            return 1;
        case FL_MOVE:
            mouseX_ = Fl::event_x() - x();
            mouseY_ = Fl::event_y() - y();
            return 1;

        case FL_PUSH: {
            if (editor_->visible() && Fl::event_inside(editor_)) return Fl_Group::handle(e);
            int pick = editing_ ? suggestionAt(Fl::event_x(), Fl::event_y()) : -1;
            if (pick >= 0) {                                // click on a completion
                suggSel_ = pick;
                acceptSuggestion(false);
                editor_->take_focus();
                return 1;
            }
            commitEdit();
            Fl::focus(this);
            int mx = Fl::event_x() - x(), my = Fl::event_y() - y();
            pressX_ = curX_ = mouseX_ = mx;
            pressY_ = curY_ = mouseY_ = my;
            moved_ = false;
            Hit h = hitTest(mx, my);
            bool shift = Fl::event_state(FL_SHIFT);

            if (locked_) {
                typingBox_ = nullptr;
                typed_.clear();
                if (h.box && h.box->kind == BoxKind::Message) {
                    static_cast<MessageBox*>(h.box)->trigger({});
                } else if (h.box && h.box->kind == BoxKind::Number) {
                    auto nb = static_cast<NumberBox*>(h.box);
                    drag_ = Drag::Number;
                    dragBox_ = nb;
                    typingBox_ = nb;
                    dragStartValue_ = nb->value();
                    dragStep_ = 1.0;
                    if (nb->isFloat()) {
                        std::string t = nb->text();
                        size_t dot = t.find('.');
                        fl_font(kFont, kFontSize);
                        int dotX = nb->x + 16 + (int)fl_width(t.substr(0, dot).c_str());
                        if (dot != std::string::npos && mx > dotX) dragStep_ = 0.01;
                    }
                } else if (h.box && h.box->kind == BoxKind::UI) {
                    drag_ = Drag::UI;
                    dragBox_ = h.box;
                    h.box->uiMouse(FL_PUSH, mx - h.box->x, my - h.box->y);
                } else if (h.box && h.box->cls == "loadbang" && Fl::event_clicks()) {
                    h.box->loadbang();
                }
                redraw();
                return 1;
            }

            if (Fl::event_clicks()) {                       // double-click
                if (h.box && h.what == Hit::Body && h.box->kind != BoxKind::Number) {
                    Fl::event_clicks(0);
                    beginEdit(h.box, false);
                    return 1;
                }
                if (h.what == Hit::None) {
                    Fl::event_clicks(0);
                    newBoxAt("", mx, my - kBoxH / 2);
                    return 1;
                }
            }
            switch (h.what) {
                case Hit::Outlet:
                    drag_ = Drag::Cable;
                    dragBox_ = h.box;
                    dragPort_ = h.port;
                    break;
                case Hit::Body:
                    if (Fl::event_state(FL_ALT)) {          // option-drag copies
                        if (!h.box->selected) { clearSelection(); h.box->selected = true; }
                        duplicateSelection();
                        drag_ = Drag::Move;
                        moved_ = true;                      // undo already recorded
                        break;
                    }
                    if (shift) h.box->selected = !h.box->selected;
                    else if (!h.box->selected) { clearSelection(); h.box->selected = true; }
                    if (h.box->selected) drag_ = Drag::Move;
                    break;
                case Hit::Cable:
                    if (!shift) clearSelection();
                    win_->patch.conns[h.cable].selected = true;
                    break;
                default:
                    if (!shift) clearSelection();
                    drag_ = Drag::Rubber;
            }
            redraw();
            return 1;
        }

        case FL_DRAG: {
            int mx = Fl::event_x() - x(), my = Fl::event_y() - y();
            int dx = mx - curX_, dy = my - curY_;
            curX_ = mouseX_ = mx;
            curY_ = mouseY_ = my;
            if (drag_ == Drag::Move && (dx || dy)) {
                if (!moved_) { win_->pushUndo(); moved_ = true; }
                for (Box* b : selectedBoxes()) {
                    b->x = std::max(0, b->x + dx);
                    b->y = std::max(0, b->y + dy);
                }
                win_->markDirty();
            } else if (drag_ == Drag::Number) {
                if (my != pressY_) { moved_ = true; typed_.clear(); }
                numberDrag(pressY_ - my);
            } else if (drag_ == Drag::UI && dragBox_) {
                dragBox_->uiMouse(FL_DRAG, mx - dragBox_->x, my - dragBox_->y);
            }
            redraw();
            return 1;
        }

        case FL_RELEASE: {
            int mx = Fl::event_x() - x(), my = Fl::event_y() - y();
            if (drag_ == Drag::UI && dragBox_)
                dragBox_->uiMouse(FL_RELEASE, mx - dragBox_->x, my - dragBox_->y);
            if (drag_ == Drag::Cable && dragBox_) {
                Hit h = hitTest(mx, my);
                if (h.box && h.box != dragBox_) {
                    int in = inletAt(h.box, mx);
                    std::string err;
                    Patch& p = win_->patch;
                    if (canConnect(p.conns, dragBox_, dragPort_, h.box, in, &err)) {
                        win_->pushUndo();
                        if (p.connect(dragBox_, dragPort_, h.box, in, &err)) win_->markDirty();
                        else { win_->dropUndo(); win_->setStatus(err); }
                    } else if (err != "already connected") {
                        win_->setStatus(err);
                    }
                }
            } else if (drag_ == Drag::Rubber) {
                int x0 = std::min(pressX_, mx), x1 = std::max(pressX_, mx);
                int y0 = std::min(pressY_, my), y1 = std::max(pressY_, my);
                for (auto& b : win_->patch.boxes)
                    if (b->x < x1 && b->x + b->w > x0 && b->y < y1 && b->y + b->h > y0)
                        b->selected = true;
            }
            drag_ = Drag::None;
            if (moved_) typingBox_ = nullptr;
            dragBox_ = nullptr;
            redraw();
            return 1;
        }

        case FL_KEYBOARD:
        case FL_SHORTCUT: {
            if (editing_) return Fl_Group::handle(e);
            if (Fl::event_state(FL_COMMAND | FL_CTRL | FL_ALT)) return 0;
            int key = Fl::event_key();
            const char* txt = Fl::event_text();
            char ch = (txt && txt[0] && !txt[1]) ? txt[0] : 0;

            if (locked_) {
                if (!typingBox_) return 0;
                auto nb = static_cast<NumberBox*>(typingBox_);
                if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
                    typed_ += ch;
                } else if (key == FL_BackSpace && !typed_.empty()) {
                    typed_.pop_back();
                } else if ((key == FL_Enter || key == FL_KP_Enter) && !typed_.empty()) {
                    nb->set(std::atof(typed_.c_str()), true);
                    typed_.clear();
                } else if (key == FL_Escape) {
                    typed_.clear();
                } else {
                    return 0;
                }
                redraw();
                return 1;
            }

            if (key == FL_BackSpace || key == FL_Delete) { deleteSelection(); return 1; }
            if (e == FL_SHORTCUT && Fl::focus() != this) return 0;
            switch (ch) {                                   // Max's patcher shortcuts
                case 'n': newBoxAtMouse(""); return 1;
                case 'm': newBoxAtMouse("message"); return 1;
                case 'f': newBoxAtMouse("flonum"); return 1;
                case 'i': newBoxAtMouse("number"); return 1;
                case 'b': newBoxAtMouse("button"); return 1;
                case 't': newBoxAtMouse("toggle"); return 1;
                case 'c': newBoxAtMouse("comment"); return 1;
            }
            return 0;
        }
    }
    return Fl_Group::handle(e);
}

// ---------------------------------------------------------------- main window

namespace {
#ifdef __APPLE__
constexpr int kMenuH = 0;        // the macOS menu bar lives at the top of the screen
#else
constexpr int kMenuH = 25;
#endif
constexpr int kStatusH = 22;
constexpr const char* kExt = "mlsp";

std::string escapeMenu(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '_') r += '\\';
        if (c == '&') r += '&';
        r += c;
    }
    return r;
}

std::string baseName(const std::string& path) {
    const char* n = fl_filename_name(path.c_str());
    return n ? n : path;
}

std::string exeDir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[4096];
        if (realpath(buf, real)) {
            std::string s = real;
            return s.substr(0, s.find_last_of('/'));
        }
    }
#endif
    return ".";
}

std::string examplesDir() {
    std::string d = exeDir();
    for (std::string c : {d + "/../Resources/examples", d + "/examples"})
        if (fl_filename_isdir(c.c_str())) return c;
    return "";
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

struct NewItem {
    MainWindow* win;
    std::string cls;
};
}  // namespace

MainWindow::MainWindow(int w, int h)
    : Fl_Double_Window(w, h, "minlsp"), audio(patch) {
    menu_ = new Fl_Sys_Menu_Bar(0, 0, w, kMenuH);
    canvas = new Canvas(0, kMenuH, w, h - kMenuH - kStatusH, this);
    statusBar_ = new Fl_Box(0, h - kStatusH, w, kStatusH);
    statusBar_->box(FL_FLAT_BOX);
    statusBar_->color(kStatusBg);
    statusBar_->labelcolor(kDim);
    statusBar_->labelsize(11);
    statusBar_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    end();
    resizable(canvas);
    size_range(400, 300);

    patch.onRedraw = [this] { canvas->redraw(); };
    patch.onStatus = [this](const std::string& s) { setStatus(s); };
    patch.onDsp = [this](bool on) { setAudio(on); };
    patch.onPrint = [this](const std::string& s) { print(s); };
    static const auto t0 = std::chrono::steady_clock::now();
    patch.clock = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    patch.onSchedule = [this] { armTimer(); };
    patch.onStructure = [this] {
        audio.updateInputNeed();
        refreshStatus();
    };

    callback([](Fl_Widget*, void* d) {
        // Escape must not close the window; everything else (close box, Quit) does.
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
        static_cast<MainWindow*>(d)->quit();
    }, this);

    buildMenu();
    updateTitle();
    setAudio(true);
    setStatus("double-click to create an object · Cmd+E toggles edit mode");
}

MainWindow::~MainWindow() { audio.stop(); }

void MainWindow::buildMenu() {
#define MW_CB(method) [](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->method(); }
    menu_->add("File/New", FL_COMMAND + 'n', MW_CB(newPatch), this);
    menu_->add("File/Open...", FL_COMMAND + 'o', MW_CB(openPatch), this);
    menu_->add("File/Save", FL_COMMAND + 's', MW_CB(save), this);
    menu_->add("File/Save as...", FL_COMMAND + FL_SHIFT + 's', MW_CB(saveAs), this,
               FL_MENU_DIVIDER);
    menu_->add("File/Quit", FL_COMMAND + 'q', MW_CB(quit), this);
    menu_->add("Edit/Undo", FL_COMMAND + 'z', MW_CB(undo), this);
    menu_->add("Edit/Redo", FL_COMMAND + FL_SHIFT + 'z', MW_CB(redo), this, FL_MENU_DIVIDER);
    menu_->add("Edit/Select All", FL_COMMAND + 'a',
               [](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->canvas->selectAll(); },
               this);

    static std::vector<std::unique_ptr<NewItem>> items;
    for (auto& c : classInfos()) {
        if (c.group.empty()) continue;                 // short aliases
        items.push_back(std::make_unique<NewItem>(NewItem{this, c.name}));
        std::string path = "Patch/New/" + c.group + "/" + escapeMenu(c.name);
        menu_->add(path.c_str(), 0,
                   [](Fl_Widget*, void* d) {
                       auto it = static_cast<NewItem*>(d);
                       it->win->canvas->newBoxAtMouse(it->cls);
                   },
                   items.back().get());
    }
    menu_->add("Patch/Edit Mode", FL_COMMAND + 'e', MW_CB(toggleLock), this, FL_MENU_TOGGLE);
    menu_->add("Patch/Audio On", FL_COMMAND + '/', MW_CB(toggleAudio), this,
               FL_MENU_TOGGLE | FL_MENU_DIVIDER);
    menu_->add("Patch/Audio Setup...", 0, MW_CB(audioSetup), this, FL_MENU_DIVIDER);
    menu_->add("Patch/Console", FL_COMMAND + FL_SHIFT + 'm', MW_CB(showConsole), this);
#undef MW_CB
}

static void setMenuCheck(Fl_Sys_Menu_Bar* m, const char* path, bool on) {
    auto it = const_cast<Fl_Menu_Item*>(m->find_item(path));
    if (!it) return;
    if (on) it->set();
    else it->clear();
    m->update();
}

void MainWindow::refreshStatus() {
    std::string s = canvas->locked() ? "LOCKED" : "EDIT";
    if (audio.running())
        s += "   ·   audio " + std::to_string(audio.sampleRate()) + " Hz" +
             (audio.hasInput() ? " in/out" : " out");
    else
        s += "   ·   audio off";
    if (!patch.dspError.empty()) s += "   ·   " + patch.dspError;
    if (!message_.empty()) s += "   ·   " + message_;
    statusBar_->copy_label(("  " + s).c_str());
    setMenuCheck(menu_, "Patch/Edit Mode", !canvas->locked());
    setMenuCheck(menu_, "Patch/Audio On", audio.running());
}

namespace {
void schedulerTimer(void* d) {
    auto* w = static_cast<MainWindow*>(d);
    w->patch.runDue(w->patch.time());           // re-arms through onSchedule
}
}  // namespace

void MainWindow::armTimer() {
    Fl::remove_timeout(schedulerTimer, this);
    double next = patch.nextDue();
    if (std::isfinite(next))
        Fl::add_timeout(std::max(0.0, next - patch.time()), schedulerTimer, this);
}

void MainWindow::showConsole() {
    if (!console_) {
        console_ = new Fl_Double_Window(520, 300, "minlsp Console");
        consoleBuf_ = new Fl_Text_Buffer();
        consoleView_ = new Fl_Text_Display(0, 0, 520, 300);
        consoleView_->buffer(consoleBuf_);
        consoleView_->box(FL_FLAT_BOX);
        consoleView_->color(fl_rgb_color(20, 21, 24));
        consoleView_->textcolor(kText);
        consoleView_->textfont(FL_COURIER);
        consoleView_->textsize(12);
        console_->resizable(consoleView_);
        console_->end();
        for (auto& line : consoleLines_) consoleBuf_->append((line + "\n").c_str());
    }
    console_->show();
    consoleView_->scroll(consoleBuf_->count_lines(0, consoleBuf_->length()), 0);
}

void MainWindow::print(const std::string& s) {
    consoleLines_.push_back(s);
    if (consoleLines_.size() > 2000) consoleLines_.erase(consoleLines_.begin());
    if (consoleBuf_) {
        consoleBuf_->append((s + "\n").c_str());
        if (consoleBuf_->length() > 200000) consoleBuf_->remove(0, consoleBuf_->length() - 150000);
        consoleView_->scroll(consoleBuf_->count_lines(0, consoleBuf_->length()), 0);
    }
    setStatus(s);
}

void MainWindow::setStatus(const std::string& s) {
    message_ = s;
    refreshStatus();
}

void MainWindow::updateTitle() {
    std::string t = (path_.empty() ? std::string("Untitled") : baseName(path_)) +
                    (dirty_ ? " — edited" : "") + " — minlsp";
    copy_label(t.c_str());
}

void MainWindow::markDirty() {
    if (!dirty_) {
        dirty_ = true;
        updateTitle();
    }
}

int MainWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        canvas->cancelEdit();
        return 1;
    }
    return Fl_Double_Window::handle(e);
}

// ---- File

bool MainWindow::confirmDiscard() {
    canvas->commitEdit();
    if (!dirty_) return true;
    std::string name = path_.empty() ? "Untitled" : baseName(path_);
    int r = fl_choice("Save changes to \"%s\"?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return save();
    return true;
}

void MainWindow::newPatch() {
    if (!confirmDiscard()) return;
    canvas->reset();
    patch.clear();
    path_.clear();
    patch.dir.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    canvas->setLocked(false);
    updateTitle();
    setStatus("new patch");
}

void MainWindow::openPatch() {
    if (!confirmDiscard()) return;
    Fl_Native_File_Chooser fc;
    fc.title("Open Patch");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("minlsp patch\t*.mlsp\nAll files\t*");
    std::string dir = path_.empty() ? examplesDir() : path_.substr(0, path_.find_last_of('/'));
    if (!dir.empty()) fc.directory(dir.c_str());
    if (fc.show() != 0) return;
    dirty_ = false;          // already confirmed
    openFile(fc.filename());
}

bool MainWindow::openFile(const std::string& path) {
    std::string src, err;
    if (!readFile(path, src)) {
        fl_alert("Can't read %s", path.c_str());
        return false;
    }
    canvas->reset();
    std::string oldDir = patch.dir;
    patch.dir = path.substr(0, path.find_last_of('/'));
    if (!patch.load(src, err)) {
        patch.dir = oldDir;
        fl_alert("Can't open %s:\n%s", baseName(path).c_str(), err.c_str());
        return false;
    }
    path_ = path;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    canvas->setLocked(true);           // Max opens patches locked
    updateTitle();
    if (!err.empty()) print(baseName(path) + ":\n" + err);
    else setStatus("opened " + baseName(path));
    patch.loadbangAll();
    canvas->redraw();
    return true;
}

bool MainWindow::save() {
    canvas->commitEdit();
    if (path_.empty()) return saveAs();
    std::ofstream f(path_, std::ios::binary);
    if (!(f << patch.serialize())) {
        fl_alert("Can't write %s", path_.c_str());
        return false;
    }
    dirty_ = false;
    updateTitle();
    setStatus("saved " + baseName(path_));
    return true;
}

bool MainWindow::saveAs() {
    canvas->commitEdit();
    Fl_Native_File_Chooser fc;
    fc.title("Save Patch As");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("minlsp patch\t*.mlsp");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    fc.preset_file(path_.empty() ? "Untitled.mlsp" : baseName(path_).c_str());
    if (fc.show() != 0) return false;
    std::string p = fc.filename();
    const char* ext = fl_filename_ext(p.c_str());
    if (!ext || !*ext) p += std::string(".") + kExt;
    path_ = p;
    patch.dir = p.substr(0, p.find_last_of('/'));
    return save();
}

void MainWindow::quit() {
    if (!confirmDiscard()) return;
    audio.stop();
    hide();
}

// ---- Edit

void MainWindow::pushUndo() {
    undo_.push_back(patch.serialize());
    if (undo_.size() > 500) undo_.erase(undo_.begin());
    redo_.clear();
}

void MainWindow::dropUndo() {
    if (!undo_.empty()) undo_.pop_back();
}

void MainWindow::restore(const std::string& snapshot) {
    std::string err;
    canvas->reset();
    patch.load(snapshot, err);
    markDirty();
    canvas->redraw();
}

void MainWindow::undo() {
    canvas->commitEdit();
    if (undo_.empty()) { setStatus("nothing to undo"); return; }
    redo_.push_back(patch.serialize());
    std::string s = std::move(undo_.back());
    undo_.pop_back();
    restore(s);
    setStatus("undo");
}

void MainWindow::redo() {
    canvas->commitEdit();
    if (redo_.empty()) { setStatus("nothing to redo"); return; }
    undo_.push_back(patch.serialize());
    std::string s = std::move(redo_.back());
    redo_.pop_back();
    restore(s);
    setStatus("redo");
}

// ---- Patch

void MainWindow::toggleLock() { canvas->setLocked(!canvas->locked()); }

void MainWindow::toggleAudio() { setAudio(!audio.running()); }

void MainWindow::setAudio(bool on) {
    std::string err;
    if (on) {
        if (!audio.start(err)) { setStatus(err); return; }
        setStatus(err.empty() ? "audio on" : err);
    } else {
        audio.stop();
        setStatus("audio off");
    }
}

void MainWindow::audioSetup() {
    audio.refreshDevices();
    Fl_Double_Window dlg(440, 230, "Audio Setup");
    Fl_Choice outC(130, 15, 290, 25, "Output device:");
    Fl_Choice inC(130, 50, 290, 25, "Input device:");
    Fl_Choice srC(130, 85, 120, 25, "Sample rate:");
    Fl_Choice bufC(130, 120, 120, 25, "Buffer size:");
    Fl_Box note(20, 150, 400, 30,
                "Input opens only when the patch contains adc~.");
    note.labelsize(11);
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button cancel(240, 190, 85, 28, "Cancel");
    Fl_Return_Button ok(335, 190, 85, 28, "OK");
    dlg.end();
    dlg.set_modal();

    outC.add("Default", 0, nullptr);
    for (auto& d : audio.outputs) outC.add(escapeMenu(d.name).c_str(), 0, nullptr);
    outC.value(audio.settings.output + 1 < outC.size() ? audio.settings.output + 1 : 0);

    inC.add("None", 0, nullptr);
    inC.add("Default", 0, nullptr);
    for (auto& d : audio.inputs) inC.add(escapeMenu(d.name).c_str(), 0, nullptr);
    inC.value(audio.settings.input + 2 < inC.size() ? audio.settings.input + 2 : 1);

    const int rates[] = {0, 44100, 48000, 88200, 96000};
    for (int r : rates) srC.add(r ? std::to_string(r).c_str() : "Default", 0, nullptr);
    srC.value(0);
    for (int k = 0; k < 5; ++k)
        if (rates[k] == audio.settings.sampleRate) srC.value(k);

    const int bufs[] = {64, 128, 256, 512, 1024, 2048};
    for (int b : bufs) bufC.add(std::to_string(b).c_str(), 0, nullptr);
    bufC.value(2);
    for (int k = 0; k < 6; ++k)
        if (bufs[k] == audio.settings.bufferFrames) bufC.value(k);

    int result = 0;
    ok.callback([](Fl_Widget* w, void* r) { *(int*)r = 1; w->window()->hide(); }, &result);
    cancel.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.show();
    while (dlg.shown()) Fl::wait();
    if (result != 1) return;

    audio.settings.output = outC.value() - 1;
    audio.settings.input = inC.value() - 2;
    audio.settings.sampleRate = rates[srC.value()];
    audio.settings.bufferFrames = bufs[bufC.value()];
    setAudio(true);
}

}  // namespace mlsp

// ============================================================================
// 6. main
// ============================================================================

namespace {

mlsp::MainWindow* gWin = nullptr;

void openFromFinder(const char* path) {
    if (gWin && gWin->confirmDiscard()) gWin->openFile(path);
}

void writeWav(const char* path, const std::vector<float>& data, int ch, int sr) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = (uint32_t)data.size() * 2;
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16((uint16_t)ch); u32(sr); u32(sr * ch * 2); u16((uint16_t)(ch * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float s : data) {
        int16_t v = (int16_t)std::lrint(std::clamp(s, -1.f, 1.f) * 32767.f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// minlsp --render patch.mlsp seconds [out.wav] [bang=<id> ...]
// Runs a patch offline (no GUI, no audio device) and prints levels.
int renderCli(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: minlsp --render patch.mlsp seconds [out.wav] [bang=id] [send=id:message] ...\n");
        return 2;
    }
    std::string src, err, path = argv[2];
    if (!mlsp::readFile(path, src)) { std::fprintf(stderr, "can't read %s\n", argv[2]); return 1; }
    const int sr = 44100, ch = 2, blk = dsp::kBlock;
    long frame = 0;
    mlsp::Patch p;
    p.onStatus = [](const std::string& s) { std::fprintf(stderr, "status: %s\n", s.c_str()); };
    p.onPrint = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    p.clock = [&frame] { return (double)frame / sr; };     // the scheduler follows sample time
    p.sampleRate = sr;
    p.dspRunning = true;
    size_t slash = path.find_last_of('/');
    p.dir = slash == std::string::npos ? "." : path.substr(0, slash);
    if (!p.load(src, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (!err.empty()) std::fprintf(stderr, "warning: %s\n", err.c_str());
    if (!p.dspError.empty()) std::fprintf(stderr, "dsp: %s\n", p.dspError.c_str());
    p.loadbangAll();
    const char* wav = nullptr;
    for (int k = 4; k < argc; ++k) {
        std::string a = argv[k];
        bool isBang = a.rfind("bang=", 0) == 0, isSend = a.rfind("send=", 0) == 0;
        if (isBang || isSend) {
            std::string rest = a.substr(5), id = rest.substr(0, rest.find(':'));
            mlsp::Message m = isBang ? mlsp::Message{mlsp::Atom("bang")}
                                     : mlsp::parseMessage(rest.substr(rest.find(':') + 1));
            if (auto b = p.find(id)) {
                if (!m.empty()) b->message(0, m);
            } else {
                std::fprintf(stderr, "no box '%s'\n", id.c_str());
            }
        } else {
            wav = argv[k];
        }
    }
    long total = (long)(std::atof(argv[3]) * sr);
    std::vector<float> all, buf(blk * ch);
    for (; frame < total; frame += blk) {
        p.runDue((double)frame / sr);
        int n = (int)std::min<long>(blk, total - frame);
        p.process(nullptr, 0, buf.data(), ch, n);
        all.insert(all.end(), buf.begin(), buf.begin() + n * ch);
    }
    for (int c = 0; c < ch; ++c) {
        double peak = 0, sum = 0;
        long n = 0;
        for (size_t i = c; i < all.size(); i += ch, ++n) {
            peak = std::max(peak, (double)std::fabs(all[i]));
            sum += all[i] * all[i];
        }
        std::printf("ch%d peak %.4f rms %.4f\n", c + 1, peak, n ? std::sqrt(sum / n) : 0.0);
    }
    if (wav) writeWav(wav, all, ch, sr);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--render") return renderCli(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::printf("minlsp %s\n", MINLSP_VERSION);
        return 0;
    }

    Fl::scheme("gtk+");
    Fl::visual(FL_DOUBLE | FL_RGB);
    Fl::background(42, 44, 50);          // dark widgets for dialogs and choices
    Fl::background2(24, 25, 28);
    Fl::foreground(226, 228, 234);
    Fl::set_color(FL_SELECTION_COLOR, 52, 92, 160);
    auto* win = new mlsp::MainWindow(960, 640);
    gWin = win;
    fl_open_callback(openFromFinder);
    win->show();
    for (int k = 1; k < argc; ++k)
        if (argv[k][0] != '-') { win->openFile(argv[k]); break; }
    int r = Fl::run();
    delete win;
    return r;
}
