// objects_control.cpp — timing, routing, control math, expr and interface objects

#include "objects.h"
#include "expr.h"

#include <FL/fl_draw.H>

#include <cstdio>
#include <random>

namespace mlsp {
namespace {

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



}  // namespace

void registerControlObjects(ClassTable& t) {
    using M = const Message&;
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
        {"mtof", "Control Math", "MIDI note to frequency", unary(mtof)},
        {"ftom", "Control Math", "frequency to MIDI note", unary(ftom)},
        {"dbtoa", "Control Math", "decibels to amplitude", unary(dbtoa)},
        {"atodb", "Control Math", "amplitude to decibels", unary(atodb)},
        {"scale", "Control Math", "map a range: in lo hi, out lo hi", with<ScaleBox>()},
        {"expr", "Control Math", "expression: $f1 * 2 + sin($f2)", with<ExprBox>()},
        {"random", "Control Math", "random integer below range", with<RandomBox>()},
        {"counter", "Control Math", "count bangs: [min] max", with<CounterBox>()},
        {"trigger", "Routing", "ordered outputs, right to left", with<TriggerBox>()},
        {"pack", "Routing", "combine into a list", with<PackBox>()},
        {"unpack", "Routing", "split a list", with<UnpackBox>()},
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
        {"slider", "Interface", "drag a value: [min max]", with<SliderBox>()},
        {"comment", "Interface", "text note", with<CommentBox>()},
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

}  // namespace mlsp
