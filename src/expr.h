// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// expr.h — the expression language of expr and expr~
//
// C-like expressions over $f1..$f9 (floats), $i1..$i9 (truncated) and, for
// expr~, $v1..$v9 (signals; in expr they read like $f).
//
// As in Max, values are typed: $i, integer constants (12) and comparisons
// are integers, $f, $v and decimal constants (12.) are floats. An operation
// between two integers is an integer one: 7 / 2 is 3, 7. / 2 is 3.5.
//
// random(a, b) draws from a mt19937 of each box through Musil's portable random module: the same
// seed gives the same numbers on every system. The seed is made from the box's id, so a patch plays the
// same numbers at each run (and two boxes different ones), unless it sends `seed n`.
//
// An expression compiles to a flat program (postfix instructions run on a
// small stack), not a tree of function objects: expr~ runs it for every
// sample, without calls through std::function and without allocating.
#pragma once

#include "dsp.h"
#include "rng.h"          // Musil's portable random numbers

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace moritz {

class ExprProgram {
public:
    enum Op : uint8_t { Const, Var, VarInt, Neg, Not, Add, Sub, Mul, Div, IDiv, Mod, Or, And,
                        Eq, Ne, Le, Ge, Lt, Gt, Call1, Call2, Random };
    struct Ins {
        Op op;
        int var = 0;
        double k = 0;
        double (*f1)(double) = nullptr;
        double (*f2)(double, double) = nullptr;
    };
    static constexpr int kMaxStack = 64;

    // random()'s generator (by the thread that runs the program)
    void seed(uint32_t s) { rng_.seed(s); }
    // a seed made from a text (a box's id): FNV-1a, the same everywhere
    static uint32_t seedOf(const std::string& t) {
        uint32_t h = 2166136261u;
        for (unsigned char c : t) h = (h ^ c) * 16777619u;
        return h;
    }
    // a seed as a patch gives it (a number, negative too)
    static uint32_t seedOf(double x) { return std::isfinite(x) ? (uint32_t)(int64_t)std::llround(x) : 0u; }

    // v: the variables ($1..$9). Not thread-safe for one program (random() has a state):
    // each box runs its own.
    double eval(const double* v) {
        double st[kMaxStack];
        int sp = 0;
        for (const Ins& in : code_) {
            switch (in.op) {
                case Const: st[sp++] = in.k; break;
                case Var: st[sp++] = v[in.var]; break;
                case VarInt: st[sp++] = std::trunc(v[in.var]); break;
                case Neg: st[sp - 1] = -st[sp - 1]; break;
                case Not: st[sp - 1] = st[sp - 1] == 0 ? 1.0 : 0.0; break;
                case Call1: st[sp - 1] = in.f1(st[sp - 1]); break;
                default: {
                    double b = st[--sp], &a = st[sp - 1];
                    switch (in.op) {
                        case Add: a = a + b; break;
                        case Sub: a = a - b; break;
                        case Mul: a = a * b; break;
                        case Div: a = b == 0 ? 0.0 : a / b; break;
                        case IDiv: a = b == 0 ? 0.0 : std::trunc(a / b); break;
                        case Mod: a = b == 0 ? 0.0 : std::fmod(a, b); break;
                        case Or: a = (a != 0 || b != 0) ? 1.0 : 0.0; break;
                        case And: a = (a != 0 && b != 0) ? 1.0 : 0.0; break;
                        case Eq: a = a == b ? 1.0 : 0.0; break;
                        case Ne: a = a != b ? 1.0 : 0.0; break;
                        case Le: a = a <= b ? 1.0 : 0.0; break;
                        case Ge: a = a >= b ? 1.0 : 0.0; break;
                        case Lt: a = a < b ? 1.0 : 0.0; break;
                        case Gt: a = a > b ? 1.0 : 0.0; break;
                        case Call2: a = in.f2(a, b); break;
                        case Random: a = musil::rnd::uniform(rng_, a, b); break;
                        default: break;
                    }
                }
            }
        }
        return sp > 0 ? st[sp - 1] : 0.0;
    }

    // expr~: the program over n samples, each instruction over a chunk of 64 at once (the
    // dispatch paid once a chunk, the loops simple enough to vectorise). in: the 9 inputs.
    void evalBlock(const float* const* in, int n, float* out) {
        for (int at = 0; at < n; at += kChunk) {
            const int m = std::min(kChunk, n - at);
            int sp = 0;
            for (const Ins& ins : code_) {
                double* a = sp > 0 ? st_[sp - 1] : nullptr;
                switch (ins.op) {
                    case Const: { double* d = st_[sp++]; for (int i = 0; i < m; ++i) d[i] = ins.k; break; }
                    case Var: { double* d = st_[sp++]; const float* x = in[ins.var] + at; for (int i = 0; i < m; ++i) d[i] = x[i]; break; }
                    case VarInt: { double* d = st_[sp++]; const float* x = in[ins.var] + at; for (int i = 0; i < m; ++i) d[i] = std::trunc((double)x[i]); break; }
                    case Neg: for (int i = 0; i < m; ++i) a[i] = -a[i]; break;
                    case Not: for (int i = 0; i < m; ++i) a[i] = a[i] == 0 ? 1.0 : 0.0; break;
                    case Call1: for (int i = 0; i < m; ++i) a[i] = ins.f1(a[i]); break;
                    default: {
                        const double* b = st_[--sp];
                        a = st_[sp - 1];
                        switch (ins.op) {
                            case Add: for (int i = 0; i < m; ++i) a[i] += b[i]; break;
                            case Sub: for (int i = 0; i < m; ++i) a[i] -= b[i]; break;
                            case Mul: for (int i = 0; i < m; ++i) a[i] *= b[i]; break;
                            case Div: for (int i = 0; i < m; ++i) a[i] = b[i] == 0 ? 0.0 : a[i] / b[i]; break;
                            case IDiv: for (int i = 0; i < m; ++i) a[i] = b[i] == 0 ? 0.0 : std::trunc(a[i] / b[i]); break;
                            case Mod: for (int i = 0; i < m; ++i) a[i] = b[i] == 0 ? 0.0 : std::fmod(a[i], b[i]); break;
                            case Or: for (int i = 0; i < m; ++i) a[i] = (a[i] != 0 || b[i] != 0) ? 1.0 : 0.0; break;
                            case And: for (int i = 0; i < m; ++i) a[i] = (a[i] != 0 && b[i] != 0) ? 1.0 : 0.0; break;
                            case Eq: for (int i = 0; i < m; ++i) a[i] = a[i] == b[i] ? 1.0 : 0.0; break;
                            case Ne: for (int i = 0; i < m; ++i) a[i] = a[i] != b[i] ? 1.0 : 0.0; break;
                            case Le: for (int i = 0; i < m; ++i) a[i] = a[i] <= b[i] ? 1.0 : 0.0; break;
                            case Ge: for (int i = 0; i < m; ++i) a[i] = a[i] >= b[i] ? 1.0 : 0.0; break;
                            case Lt: for (int i = 0; i < m; ++i) a[i] = a[i] < b[i] ? 1.0 : 0.0; break;
                            case Gt: for (int i = 0; i < m; ++i) a[i] = a[i] > b[i] ? 1.0 : 0.0; break;
                            case Call2: for (int i = 0; i < m; ++i) a[i] = ins.f2(a[i], b[i]); break;
                            case Random:
                                for (int i = 0; i < m; ++i) a[i] = musil::rnd::uniform(rng_, a[i], b[i]);
                                break;
                            default: break;
                        }
                    }
                }
            }
            for (int i = 0; i < m; ++i) {
                double r = sp > 0 ? st_[sp - 1][i] : 0.0;
                out[at + i] = std::isfinite(r) ? (float)r : 0.f;
            }
        }
    }

private:
    friend class ExprParser;
    static constexpr int kChunk = 64;
    std::vector<Ins> code_;
    double st_[kMaxStack][kChunk];               // evalBlock's stack: in the program, nothing allocated
    std::mt19937 rng_{1u};
};

class ExprParser {
public:
    using Fn = std::function<double(const double*)>;
    explicit ExprParser(std::string s) : s_(std::move(s)) {}
    // the expression as a program; nvars: the highest $n used
    bool parse(ExprProgram& out, int& nvars, std::string& err) {
        try {
            pos_ = 0;
            prog_.code_.clear();
            depth_ = maxDepth_ = 0;
            parseOr();
            skip();
            if (pos_ != s_.size()) fail("unexpected '" + s_.substr(pos_, 1) + "'");
            if (maxDepth_ > ExprProgram::kMaxStack) fail("expression too deep");
            out.code_ = std::move(prog_.code_);
            nvars = maxVar_;
            return true;
        } catch (std::exception& e) {
            err = e.what();
            return false;
        }
    }
    // the same as a function (expr: control rate); program: the program it runs (to seed it)
    bool parse(Fn& out, int& nvars, std::string& err, std::shared_ptr<ExprProgram>* program = nullptr) {
        auto p = std::make_shared<ExprProgram>();
        if (!parse(*p, nvars, err)) return false;
        out = [p](const double* v) { return p->eval(v); };
        if (program) *program = p;
        return true;
    }

private:
    [[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }
    void emit(ExprProgram::Op op, int pushes) {
        ExprProgram::Ins in;
        in.op = op;
        prog_.code_.push_back(in);
        depth_ += pushes;
        maxDepth_ = std::max(maxDepth_, depth_);
    }
    void skip() { while (pos_ < s_.size() && std::isspace((unsigned char)s_[pos_])) ++pos_; }
    bool eat(const char* tok) {
        skip();
        size_t L = std::strlen(tok);
        if (s_.compare(pos_, L, tok) == 0) { pos_ += L; return true; }
        return false;
    }
    void parseOr() {
        parseAnd();
        while (eat("||")) { parseAnd(); int_ = true; emit(ExprProgram::Or, -1); }
    }
    void parseAnd() {
        parseCmp();
        while (eat("&&")) { parseCmp(); int_ = true; emit(ExprProgram::And, -1); }
    }
    void parseCmp() {
        parseAdd();
        for (;;) {
            ExprProgram::Op op;
            if (eat("==")) op = ExprProgram::Eq; else if (eat("!=")) op = ExprProgram::Ne;
            else if (eat("<=")) op = ExprProgram::Le; else if (eat(">=")) op = ExprProgram::Ge;
            else if (eat("<")) op = ExprProgram::Lt; else if (eat(">")) op = ExprProgram::Gt;
            else return;
            parseAdd();
            int_ = true;
            emit(op, -1);
        }
    }
    void parseAdd() {
        parseMul();
        for (;;) {
            bool ia = int_;
            if (eat("+")) { parseMul(); int_ = ia && int_; emit(ExprProgram::Add, -1); }
            else if (eat("-")) { parseMul(); int_ = ia && int_; emit(ExprProgram::Sub, -1); }
            else return;
        }
    }
    void parseMul() {
        parseUnary();
        for (;;) {
            bool ia = int_;
            if (eat("*")) { parseUnary(); int_ = ia && int_; emit(ExprProgram::Mul, -1); }
            else if (eat("/")) { parseUnary(); int_ = ia && int_; emit(int_ ? ExprProgram::IDiv : ExprProgram::Div, -1); }
            else if (eat("%")) { parseUnary(); int_ = ia && int_; emit(ExprProgram::Mod, -1); }
            else return;
        }
    }
    void parseUnary() {
        if (eat("-")) { parseUnary(); emit(ExprProgram::Neg, 0); return; }
        if (eat("!")) { parseUnary(); int_ = true; emit(ExprProgram::Not, 0); return; }
        if (eat("+")) { parseUnary(); return; }
        parsePrimary();
    }
    void parsePrimary() {
        skip();
        if (pos_ >= s_.size()) fail("unexpected end of expression");
        if (++nest_ > 200) fail("expression nested too deeply");
        struct Out { int& n; ~Out() { --n; } } out{nest_};
        char c = s_[pos_];
        if (c == '(') {
            ++pos_;
            parseOr();
            if (!eat(")")) fail("missing ')'");
            return;
        }
        if (c == '$') {
            ++pos_;
            char t = pos_ < s_.size() ? (char)std::tolower((unsigned char)s_[pos_++]) : 0;
            if (t != 'f' && t != 'i' && t != 'v') fail("use $f1..$f9, $i1..$i9 or $v1..$v9");
            int k = pos_ < s_.size() && std::isdigit((unsigned char)s_[pos_]) ? s_[pos_++] - '0' : 0;
            if (k < 1 || k > 9) fail("use $f1..$f9 or $i1..$i9");
            maxVar_ = std::max(maxVar_, k);
            int_ = t == 'i';
            emit(t == 'i' ? ExprProgram::VarInt : ExprProgram::Var, 1);
            prog_.code_.back().var = k - 1;
            return;
        }
        if (std::isdigit((unsigned char)c) || c == '.') {
            char* end = nullptr;
            double d = std::strtod(s_.c_str() + pos_, &end);
            std::string lit = s_.substr(pos_, (size_t)(end - s_.c_str()) - pos_);
            pos_ = (size_t)(end - s_.c_str());
            int_ = lit.find_first_of(".eE") == std::string::npos;
            constant(d);
            return;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t b = pos_;
            while (pos_ < s_.size() && (std::isalnum((unsigned char)s_[pos_]) || s_[pos_] == '_')) ++pos_;
            std::string name = s_.substr(b, pos_ - b);
            if (!eat("(")) {
                if (name == "pi") { int_ = false; constant(dsp::kPi); return; }
                fail("unknown name '" + name + "'");
            }
            int args = 0;
            bool allInt = true;
            if (!eat(")")) {
                do { parseOr(); ++args; allInt = allInt && int_; } while (eat(","));
                if (!eat(")")) fail("missing ')' after arguments of " + name);
            }
            // int(), floor() ... give integers; abs, min, max keep the type
            int_ = name == "int" || name == "floor" || name == "ceil" || name == "round" ||
                   ((name == "abs" || name == "min" || name == "max") && allInt);
            call(name, args);
            return;
        }
        fail(std::string("unexpected '") + c + "'");
    }
    void constant(double d) {
        emit(ExprProgram::Const, 1);
        prog_.code_.back().k = d;
    }
    void call(const std::string& name, int args) {
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
        };
        if (auto it = unary.find(name); it != unary.end()) {
            if (args != 1) fail(name + "() takes 1 argument");
            emit(ExprProgram::Call1, 0);
            prog_.code_.back().f1 = it->second;
            return;
        }
        if (name == "random") {
            if (args != 2) fail("random() takes 2 arguments");
            emit(ExprProgram::Random, -1);
            return;
        }
        if (auto it = binary.find(name); it != binary.end()) {
            if (args != 2) fail(name + "() takes 2 arguments");
            emit(ExprProgram::Call2, -1);
            prog_.code_.back().f2 = it->second;
            return;
        }
        fail("unknown function '" + name + "'");
    }

    std::string s_;
    size_t pos_ = 0;
    int maxVar_ = 0;
    bool int_ = false;   // type of the subexpression just parsed
    ExprProgram prog_;
    int depth_ = 0, maxDepth_ = 0, nest_ = 0;
};

}  // namespace moritz
