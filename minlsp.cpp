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

// ---------------------------------------------------------------- + - * /
// Left inlet is hot (outputs), right inlet is cold (stores the operand).
class CtlOpBox : public Box {
public:
    CtlOpBox(dsp::Op op, const Message& a) : op_(op), b_(argf(a, 0, 0.f)) {
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
        outletFloat(0, dsp::apply(op_, a_, b_));
    }

private:
    dsp::Op op_;
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

// ---------------------------------------------------------------- factory
using Factory = std::function<std::unique_ptr<Box>(const Message&)>;

const std::map<std::string, Factory>& factories() {
    using M = const Message&;
    static const std::map<std::string, Factory> f = {
        {"cycle~", [](M a) { return std::make_unique<CycleBox>(a); }},
        {"reson~", [](M a) { return std::make_unique<ResonBox>(a); }},
        {"noise~", [](M) { return std::make_unique<NoiseBox>(); }},
        {"click~", [](M a) { return std::make_unique<ClickBox>(a); }},
        {"+~", [](M a) { return std::make_unique<SigOpBox>(dsp::Op::Add, a); }},
        {"-~", [](M a) { return std::make_unique<SigOpBox>(dsp::Op::Sub, a); }},
        {"*~", [](M a) { return std::make_unique<SigOpBox>(dsp::Op::Mul, a); }},
        {"/~", [](M a) { return std::make_unique<SigOpBox>(dsp::Op::Div, a); }},
        {"+", [](M a) { return std::make_unique<CtlOpBox>(dsp::Op::Add, a); }},
        {"-", [](M a) { return std::make_unique<CtlOpBox>(dsp::Op::Sub, a); }},
        {"*", [](M a) { return std::make_unique<CtlOpBox>(dsp::Op::Mul, a); }},
        {"/", [](M a) { return std::make_unique<CtlOpBox>(dsp::Op::Div, a); }},
        {"flonum", [](M a) { return std::make_unique<NumberBox>(true, a); }},
        {"number", [](M a) { return std::make_unique<NumberBox>(false, a); }},
        {"message", [](M a) { return std::make_unique<MessageBox>(a); }},
        {"send", [](M a) { return std::make_unique<SendBox>(a); }},
        {"s", [](M a) { return std::make_unique<SendBox>(a); }},
        {"receive", [](M a) { return std::make_unique<ReceiveBox>(a); }},
        {"r", [](M a) { return std::make_unique<ReceiveBox>(a); }},
        {"loadbang", [](M) { return std::make_unique<LoadbangBox>(); }},
        {"adc~", [](M a) { return std::make_unique<AdcBox>(a); }},
        {"dac~", [](M a) { return std::make_unique<DacBox>(a); }},
    };
    return f;
}

}  // namespace

std::unique_ptr<Box> makeBox(const std::string& cls, const Message& args) {
    auto it = factories().find(cls);
    std::unique_ptr<Box> b = it == factories().end() ? std::make_unique<BrokenBox>()
                                                     : it->second(args);
    b->cls = cls;
    b->args = args;
    return b;
}

const std::vector<std::string>& boxClasses() {
    static const std::vector<std::string> c = {
        "cycle~", "reson~", "noise~", "click~", "+~", "-~", "*~", "/~",
        "+", "-", "*", "/", "flonum", "number", "message", "send", "receive",
        "loadbang", "adc~", "dac~"};
    return c;
}

std::vector<std::string> allClassNames() {
    std::vector<std::string> v;
    for (auto& kv : factories()) v.push_back(kv.first);
    return v;
}

const char* describeClass(const std::string& cls) {
    static const std::map<std::string, const char*> d = {
        {"cycle~", "cosine oscillator"},      {"reson~", "resonant bandpass filter"},
        {"noise~", "white noise"},            {"click~", "impulse on bang"},
        {"+~", "add signals"},                {"-~", "subtract signals"},
        {"*~", "multiply signals"},           {"/~", "divide signals"},
        {"+", "add numbers"},                 {"-", "subtract numbers"},
        {"*", "multiply numbers"},            {"/", "divide numbers"},
        {"flonum", "float number box"},       {"number", "integer number box"},
        {"message", "clickable message"},     {"send", "send to receive objects"},
        {"s", "send (short name)"},           {"receive", "receive sent messages"},
        {"r", "receive (short name)"},        {"loadbang", "bang when the patch opens"},
        {"adc~", "audio input"},              {"dac~", "audio output"},
    };
    auto it = d.find(cls);
    return it == d.end() ? "" : it->second;
}

// ============================================================================
// 3. patch
// ============================================================================

Patch::Patch() = default;

Patch::~Patch() {
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
    if (onStructure) onStructure();
    return raw;
}

void Patch::clear() {
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
        if (c.from->outs[c.outlet] != Port::Signal) continue;
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
            if (cls == "message") args = {Atom(messageToString(args))};
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
    {
        std::lock_guard<std::mutex> lk(dspMutex);
        old.swap(boxes);
        conns = std::move(nc);
        boxes = std::move(nb);
        compile();
        old.clear();
    }
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
    editor_->box(FL_BORDER_BOX);
    editor_->color(fl_rgb_color(20, 21, 24));
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
    if (b->kind == BoxKind::Number) b->w = std::max(56, tw + 24);
    else if (b->kind == BoxKind::Message) b->w = std::max(36, tw + 20);
    else b->w = std::max({tw + 16, ports * (kPortW + 8) + 2 * kPortInset, 40});
    b->h = kBoxH;
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

    // message boxes are pills, everything else a softly rounded box
    int r = b->kind == BoxKind::Message ? H / 2 : kRadius;
    Fl_Color fill = kBoxFill;
    if (b->kind == BoxKind::Message) fill = flash ? kFlash : kMsgFill;
    else if (b->kind == BoxKind::Number) fill = kNumFill;
    else if (b->kind == BoxKind::Broken) fill = kBroken;
    fl_color(fill);
    fl_rounded_rectf(X, Y, W, H, r);

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
    if (editing_ != b) {
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
        fl_color(b->ins[k] == Port::Signal ? kSignal : kCtlPort);
        fl_rounded_rectf(x() + px, y() + py, kPortW, kPortH + 1, 1);
    }
    for (int k = 0; k < b->numOuts(); ++k) {
        int px, py;
        portPos(b, true, k, px, py);
        fl_color(b->outs[k] == Port::Signal ? kSignal : kCtlPort);
        fl_rounded_rectf(x() + px, y() + py - kPortH - 1, kPortW, kPortH + 1, 1);
    }
}

// ---- completion popup

void Canvas::updateSuggestions() {
    sugg_.clear();
    suggSel_ = 0;
    if (editing_ && editing_->kind != BoxKind::Message) {
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
        bool sig = c.from->outs[c.outlet] == Port::Signal;
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

    bool flashing = false;
    for (auto& b : p.boxes) {
        drawBox(b.get());
        if (b->flashUntil > now()) flashing = true;
    }
    if (flashing && !Fl::has_timeout(flashTimer, this)) Fl::add_timeout(0.05, flashTimer, this);

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

    if (editor_->visible()) draw_child(*editor_);
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
    if (isNew && b->kind != BoxKind::Message && !t.empty()) t += " ";
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

    if (b->kind == BoxKind::Message) {
        if (t != b->text()) {
            if (!editingNew_) win_->pushUndo();
            p.replaceBox(b, makeBox("message", {Atom(t)}));
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
    if (b->kind == BoxKind::Number) {
        b->selected = true;
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
            }
            redraw();
            return 1;
        }

        case FL_RELEASE: {
            int mx = Fl::event_x() - x(), my = Fl::event_y() - y();
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
    const char* groups[][2] = {
        {"cycle~", "Audio"}, {"reson~", "Audio"}, {"noise~", "Audio"}, {"click~", "Audio"},
        {"+~", "Audio"}, {"-~", "Audio"}, {"*~", "Audio"}, {"/~", "Audio"},
        {"adc~", "Audio"}, {"dac~", "Audio"},
        {"+", "Control"}, {"-", "Control"}, {"*", "Control"}, {"/", "Control"},
        {"send", "Control"}, {"receive", "Control"}, {"loadbang", "Control"},
        {"message", "Interface"}, {"flonum", "Interface"}, {"number", "Interface"}};
    for (auto& g : groups) {
        items.push_back(std::make_unique<NewItem>(NewItem{this, g[0]}));
        std::string path = std::string("Patch/New/") + g[1] + "/" + escapeMenu(g[0]);
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
    menu_->add("Patch/Audio Setup...", 0, MW_CB(audioSetup), this);
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
    if (!patch.load(src, err)) {
        fl_alert("Can't open %s:\n%s", baseName(path).c_str(), err.c_str());
        return false;
    }
    path_ = path;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    canvas->setLocked(true);           // Max opens patches locked
    updateTitle();
    setStatus(err.empty() ? "opened " + baseName(path) : err.substr(0, err.find('\n')));
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
        std::fprintf(stderr, "usage: minlsp --render patch.mlsp seconds [out.wav] [bang=id ...]\n");
        return 2;
    }
    std::string src, err;
    if (!mlsp::readFile(argv[2], src)) { std::fprintf(stderr, "can't read %s\n", argv[2]); return 1; }
    mlsp::Patch p;
    p.onStatus = [](const std::string& s) { std::fprintf(stderr, "status: %s\n", s.c_str()); };
    if (!p.load(src, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (!err.empty()) std::fprintf(stderr, "warning: %s\n", err.c_str());
    if (!p.dspError.empty()) std::fprintf(stderr, "dsp: %s\n", p.dspError.c_str());
    const int sr = 44100, ch = 2, blk = 256;
    p.sampleRate = sr;
    p.loadbangAll();
    const char* wav = nullptr;
    for (int k = 4; k < argc; ++k) {
        std::string a = argv[k];
        if (a.rfind("bang=", 0) == 0) {
            if (auto b = p.find(a.substr(5))) {
                b->message(0, {mlsp::Atom("bang")});
            } else {
                std::fprintf(stderr, "no box '%s'\n", a.c_str() + 5);
            }
        } else {
            wav = argv[k];
        }
    }
    long total = (long)(std::atof(argv[3]) * sr);
    std::vector<float> all, buf(blk * ch);
    for (long done = 0; done < total; done += blk) {
        int n = (int)std::min<long>(blk, total - done);
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
