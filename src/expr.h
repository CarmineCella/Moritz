// expr.h — the expression language of expr and expr~
//
// C-like expressions over $f1..$f9 (floats), $i1..$i9 (truncated) and, for
// expr~, $v1..$v9 (signals; in expr they read like $f).
#pragma once

#include "dsp.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace mlsp {

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
            if (t != 'f' && t != 'i' && t != 'v') fail("use $f1..$f9, $i1..$i9 or $v1..$v9");
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

}  // namespace mlsp
