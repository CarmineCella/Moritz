// core.h — the Musil language: reader, evaluator, core builtins.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
//   - Top level: each line is a command (auto-listified). No enclosing parens.
//   - (form ...) is a Lisp list. Newlines inside parens are whitespace.
//   - {form ...} is a (do form ...) block of newline-separated sub-lists.
//   - "..." string. 'x is (quote x). ; starts a comment, as in Lisp and in Moritz's patches.
//     # is an ordinary character: 'C#4 and #1 are symbols. \ at line end continues.
//
// Numbers are valarray<double>; size-1 vectors broadcast.
// Values are shared_ptr — sharing by default; copy is explicit. The literals of the program text
// (numbers, strings, quoted lists) are never changed in place: a variable bound to one gets its own
// copy, a quoted list is a fresh copy each time it is evaluated, and the mutators refuse a literal.
// Special forms dispatch by function-pointer comparison.
// User functions curry when called with too few args; with too many, the
// result is applied to the remaining args.
//
// Embedding (the API a host uses besides run/eval/call_fn/def):
//   - Stop. Interp::request_stop() (any thread, also a signal handler: one atomic store) makes the
//     running evaluation throw musil::Stop_signal at its next step (every evaluation step checks it,
//     and so do sleep and udp-receive). Stop_signal derives neither from Error nor from std::exception,
//     so (try ...) and the builtins that catch errors do not stop it: it reaches the host. The request
//     is sticky: it stays set (every later step throws again) until the host calls Interp::clear_stop(),
//     which it does once the evaluation has returned to it. Interp::stopping() reads it.
//   - Capabilities. Interp::caps (struct Capabilities) says what the code may do: read and write files
//     (optionally only under some folders), run programs, use the network, wait (sleep, input, exec,
//     udp-receive), end the program (exit). Every builtin that touches the system checks it inside, with
//     require_read(path), require_write(path), require_exec(what), require_net(what),
//     require_blocking(what), require_exit(); a refusal is an ordinary Musil error
//     "<builtin>: not allowed here: write /abs/path". Capabilities::full() is the default (the standalone
//     musil); Capabilities::sandboxed(write_roots, read_roots) refuses programs, network, waiting and
//     exit, and writes outside write_roots. allow_blocking is caps.blocking.
//   - Per-interpreter state. Interp::state<T>() is a T owned by the interpreter (made on first use):
//     the libraries keep their tables there, so several interpreters run at once on threads.
//   - Names. The builtins and the libraries (files found on the load path) are defined in Interp::library
//     and, shared, in Interp::global, the program's environment; a library function closes over the
//     library environment, so a program that defines a name of its own (var sum 0) does not change what
//     the library's functions mean (see Env). Interp::names() lists every name.
//   - Memory. Interp::collect() frees the cycles a closure stored in its own scope makes; it runs by
//     itself when the environments alive keep growing.
//

#pragma once
#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <valarray>
#include <vector>
#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif
#ifdef _WIN32
// the stack of the calling thread (kernel32, Windows 8 and later), declared here so that core.h does
// not bring in <windows.h> and its macros
extern "C" __declspec(dllimport) void __stdcall GetCurrentThreadStackLimits(unsigned long long* low, unsigned long long* high);
#include <intrin.h>
#else
#include <pthread.h>
#include <sys/select.h>
#include <unistd.h>
#endif

#define MUSIL_VERSION "0.11"   // the one place the version is set: CMake, the deploy scripts and the manual read it here

namespace musil {
namespace fs = std::filesystem;

struct Value; struct Env; struct Interp;
using vptr = std::shared_ptr<Value>;
using eptr = std::shared_ptr<Env>;
using vlist = std::vector<vptr>;
using varr = std::valarray<double>;
using sptr = std::shared_ptr<std::string>;
using op_t = vptr (*)(vlist&, Interp&);

// How many values and environments are alive, in every interpreter of the process: (live-objects) reads
// them, and the memory test checks that what a program makes is freed when it is no longer used.
inline std::atomic<long>& live_values() { static std::atomic<long> n{0}; return n; }
inline std::atomic<long>& live_envs()   { static std::atomic<long> n{0}; return n; }
template <std::atomic<long>& (*N)()> struct live_counter {
    live_counter() { N().fetch_add(1, std::memory_order_relaxed); }
    live_counter(const live_counter&) { N().fetch_add(1, std::memory_order_relaxed); }
    live_counter& operator=(const live_counter&) { return *this; }
    ~live_counter() { N().fetch_sub(1, std::memory_order_relaxed); }
};

// --- the stack of the calling thread ------------------------------------------------------------
// The evaluator, the reader and the structural walks recurse; they stop with an error when the thread's
// stack is nearly used up. The bounds are those of the thread really running (an interpreter on a
// std::thread has 512 KB on macOS, the main thread 8 MB), read once per thread.
struct thread_stack { std::uintptr_t low = 0, high = 0; size_t reserve = 64u << 10; };
#if defined(_MSC_VER)
#define MUSIL_FRAME_ADDRESS() reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress())
#else
#define MUSIL_FRAME_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0))
#endif
inline const thread_stack& this_thread_stack() {
    static thread_local thread_stack s = [] {
        thread_stack t;
#if defined(_WIN32)
        unsigned long long lo = 0, hi = 0; GetCurrentThreadStackLimits(&lo, &hi);
        t.low = (std::uintptr_t)lo; t.high = (std::uintptr_t)hi;
#elif defined(__APPLE__)
        pthread_t self = pthread_self();
        t.high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
        t.low = t.high - pthread_get_stacksize_np(self);
#else
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            void* addr = nullptr; size_t sz = 0;
            if (pthread_attr_getstack(&a, &addr, &sz) == 0) { t.low = reinterpret_cast<std::uintptr_t>(addr); t.high = t.low + sz; }
            pthread_attr_destroy(&a);
        }
#endif
        size_t size = t.high > t.low ? t.high - t.low : 0;
        if (size == 0) { size = 512u << 10; t.high = MUSIL_FRAME_ADDRESS(); t.low = t.high - size; }   // unknown: assume little
        t.reserve = std::min<size_t>(1u << 20, std::max<size_t>(48u << 10, size / 4));
        return t;
    }();
    return s;
}
// is the stack of this thread nearly used up? (an address off the stack, as with a sanitizer's fake frames, is not)
inline bool stack_low() {
    std::uintptr_t here = MUSIL_FRAME_ADDRESS(); const thread_stack& t = this_thread_stack();
    return here >= t.low && here < t.low + t.reserve;
}

// AST
struct Value {
    enum tag { NIL, NUM, STR, SYM, LIST, FN, OPAQUE } t = NIL;
    bool lit = false;                              // read from the program text: never changed in place (see unlit)
    int line = 0;
    int amin = 0, amax = -1;                       // builtin: arity (amax < 0 = unbounded)
    varr num; std::string s; vlist l;
    sptr file;
    op_t op = nullptr;
    std::vector<std::string> params;
    vptr body; eptr closure;                       // a (function ...) form caches its prepared body in body
    std::weak_ptr<Env> wclosure;                  // a function bound in the environment it closes over holds it weakly (Env::bind)
    std::shared_ptr<void> opaque; std::string opaque_tag;
    live_counter<live_values> counted;
    Value() = default;
    Value(const Value&) = default;
    Value& operator=(const Value&) = default;
    ~Value();
};
// Freeing a list never recurses and never allocates: a nested list held only by the list being freed
// hands over its elements and waits in a chain (linked through body, which lists do not use) until
// they are freed. So a list nested a million times deep frees in constant stack, and freeing cannot
// fail for lack of memory.
inline Value::~Value() {
    if (l.empty()) return;
    vlist cur; cur.swap(l);
    vptr chain;
    for (;;) {
        while (!cur.empty()) {
            vptr c = std::move(cur.back()); cur.pop_back();
            if (c && c.use_count() == 1 && !c->l.empty() && !c->body) {
                std::swap(cur, c->l);                   // c keeps what is left of ours; we go on with its elements
                c->body = std::move(chain); chain = std::move(c);
            }
        }
        if (!chain) break;
        vptr n = std::move(chain); chain = std::move(n->body);
        std::swap(cur, n->l);                           // n is empty now: freeing it does not recurse
    }
}
inline const char* type_name(const vptr& v) {
    static const char* names[] = { "nil", "number", "string", "symbol", "list", "function", "opaque" };
    return v ? names[v->t] : "nil";
}

inline vptr v_nil()              { return std::make_shared<Value>(); }
inline vptr v_num(double d)      { auto v = v_nil(); v->t = Value::NUM; v->num = varr(d, 1); return v; }
inline vptr v_arr(varr a)        { auto v = v_nil(); v->t = Value::NUM; v->num = std::move(a); return v; }
inline vptr v_bool(bool b)       { return v_num(b ? 1.0 : 0.0); }
inline vptr v_str(std::string s) { auto v = v_nil(); v->t = Value::STR; v->s = std::move(s); return v; }
inline vptr v_sym(std::string s) { auto v = v_nil(); v->t = Value::SYM; v->s = std::move(s); return v; }
inline vptr v_list(vlist x, int ln=0, sptr f={}) { 
    auto v=v_nil(); v->t=Value::LIST; v->l=std::move(x); v->line=ln; v->file=std::move(f); return v; 
}
inline vptr v_op(op_t f, std::string name, int amin, int amax) { 
    auto v = v_nil(); v->t = Value::FN; v->op = f; v->s = std::move(name); v->amin = amin; v->amax = amax; return v; 
}
inline vptr v_opaque(std::string tag, std::shared_ptr<void> p) { 
    auto v = v_nil(); v->t = Value::OPAQUE; v->opaque_tag = std::move(tag); v->opaque = std::move(p); return v; 
}
inline vptr v_copy(const Value& s) { auto v = std::make_shared<Value>(s); v->lit = false; return v; }
// A literal of the program text, as a value a variable may own: a number or a string is copied (a vector
// literal changed in place would change the program); anything else is itself.
inline vptr unlit(vptr v) {
    if (v && v->lit && (v->t == Value::NUM || v->t == Value::STR)) { auto c = v_nil(); c->t = v->t; c->num = v->num; c->s = v->s; return c; }
    return v;
}
// what a quoted literal evaluates to: a fresh copy of the lists in it (a quoted list changed by push or
// setidx would change the program), the symbols and the other atoms shared
inline vptr copy_literal(const vptr& v, int depth = 0) {
    if (!v || !v->lit) return v;
    if (v->t == Value::NUM || v->t == Value::STR) return unlit(v);
    if (v->t != Value::LIST) return v;
    if (depth > 10000 || stack_low()) return v;                      // the reader refuses deeper nesting
    vlist out; out.reserve(v->l.size());
    for (auto& x : v->l) out.push_back(copy_literal(x, depth + 1));
    return v_list(std::move(out), v->line, v->file);
}

// A function bound by name in the very environment it closes over (a local function, a var holding a
// lambda made there) would keep that environment alive forever: the environment holds the function and
// the function holds the environment. So such a binding holds the environment weakly, and reading the
// name gives back an ordinary function that holds it (a copy: it can be returned and outlive the scope).
// The global environments are exempt: they live as long as the interpreter, which clears them at the end.
// The two global environments (Interp::global, the program's, and Interp::library) mirror each other: what
// the library defines is defined in both (shared), so the program sees every name in its own global
// environment, while a library function, which closes over the library environment, finds the library's
// definition even after the program defines the same name for itself (a var or a function of the program
// stops the sharing of that name: the program's own from then on). A set changes a shared name on both
// sides. A name the library does not have is looked for among the program's globals (fallback).
struct Env {
    std::unordered_map<std::string, vptr> vars;
    eptr parent;
    bool global = false;
    Env* fallback = nullptr;                      // the library's: the program's global environment
    Env* mirror = nullptr;                        // the other global environment
    bool library_side = false;                    // this is the library's
    std::unordered_set<std::string>* shared = nullptr;   // the names defined in both (the library's, not redefined by the program)
    live_counter<live_envs> counted;
    Env(eptr p = nullptr) : parent(std::move(p)) {}
    static vptr strong(const vptr& v) {
        if (!v || v->t != Value::FN || v->closure || v->op) return v;
        eptr c = v->wclosure.lock();
        if (!c) return v;
        auto s = v_copy(*v); s->closure = std::move(c); s->wclosure.reset();
        return s;
    }
    vptr held(vptr v) const {
        if (global || !v || v->t != Value::FN || v->op || v->closure.get() != this) return v;
        auto w = v_copy(*v); w->wclosure = v->closure; w->closure.reset();
        return w;
    }
    void bind(const std::string& k, vptr v) {
        vptr h = held(std::move(v));
        if (mirror) {
            if (library_side) { mirror->vars[k] = h; shared->insert(k); }
            else shared->erase(k);
        }
        vars[k] = std::move(h);
    }
    vptr find(const std::string& k) {
        Env* last = this;
        for (Env* w = this; w; w = w->parent.get()) {
            auto it = w->vars.find(k);
            if (it != w->vars.end()) return strong(it->second);
            last = w;
        }
        if (last->fallback) { auto it = last->fallback->vars.find(k); if (it != last->fallback->vars.end()) return strong(it->second); }
        return nullptr;
    }
    bool assign(const std::string& k, vptr v) {
        Env* last = this;
        for (Env* w = this; w; w = w->parent.get()) {
            auto it = w->vars.find(k);
            if (it != w->vars.end()) {
                it->second = w->held(std::move(v));
                if (w->mirror && w->shared->count(k)) w->mirror->vars[k] = it->second;
                return true;
            }
            last = w;
        }
        if (last->fallback) { auto it = last->fallback->vars.find(k); if (it != last->fallback->vars.end()) { it->second = std::move(v); return true; } }
        return false;
    }
};

// helpers
struct Error : std::exception {
    std::string file; int line = 0; std::string msg;
    std::vector<std::string> trace;
    mutable std::string cached;
    Error(std::string f, int ln, std::string m) : file(std::move(f)), line(ln), msg(std::move(m)) {}
    const char* what() const noexcept override {
        if (cached.empty()) {
            cached = file + ":" + std::to_string(line) + ": " + msg;
            for (size_t i=0; i<trace.size(); ++i) cached += "\n  " + std::to_string(i+1) + "> " + trace[i];
        }
        return cached.c_str();
    }
};
// return, break and continue are not exceptions (the evaluator passes them up as a state); these types
// stay for hosts that named them
struct Return_signal { vptr val; }; struct break_signal {}; struct continue_signal {};
struct Exit_signal { int code; };
// thrown when the host asked the interpreter to stop (Interp::request_stop): not an Error, not a
// std::exception, so neither (try ...) nor a builtin's error handling catches it
struct Stop_signal {};
inline bool truthy(const vptr& v) {
    if (!v || v->t == Value::NIL) return false;
    if (v->t == Value::NUM) { for (size_t i=0; i<v->num.size(); i++) if (v->num[i]==0.0) return false; return v->num.size() > 0; }
    if (v->t == Value::STR) return !v->s.empty();
    if (v->t == Value::LIST) return !v->l.empty();
    return true;
}
inline void put_double(std::ostream& os, double d) {
    if (std::isfinite(d) && std::fabs(d) < 1e15 && d == std::floor(d)) os << (long long)d;
    else os << std::setprecision(15) << d;
}
inline int& str_of_depth() { static thread_local int d = 0; return d; }
inline std::string str_of(const vptr& v) {
    if (!v) return "nil";
    struct guard { guard() { str_of_depth()++; } ~guard() { str_of_depth()--; } } g;
    if (str_of_depth() > 200 || stack_low()) return "...";   // a list that contains itself (a closure holding its environment) prints as ...
    std::ostringstream os;
    switch (v->t) {
    case Value::NIL: return "nil";
    case Value::NUM:
        if (v->num.size() == 1) { put_double(os, v->num[0]); return os.str(); }
        os << "(";
        for (size_t i=0; i<v->num.size(); i++) { if (i) os << " "; put_double(os, v->num[i]); }
        os << ")";
        return os.str();
    case Value::STR: case Value::SYM: return v->s;
    case Value::FN: return v->op ? "<builtin>" : v->s.empty() ? "<fn>" : "<fn " + v->s + ">";
    case Value::LIST:
        os << "("; for (size_t i=0; i<v->l.size(); i++) { if (i) os << " "; os << str_of(v->l[i]); } os << ")";
        return os.str();
    case Value::OPAQUE: return "<opaque:" + v->opaque_tag + ">";
    }
    return "";
}
// (str_of_limited v limit) what the value prints as, stopped once limit characters are out: for a host's echo of a
// value (a score holds thousands of notes, each its database: printing it whole would take minutes and gigabytes)
inline void str_of_into(const vptr& v, std::string& out, size_t limit, int depth) {
    if (out.size() >= limit) return;
    if (!v) { out += "nil"; return; }
    if (depth > 200 || stack_low()) { out += "..."; return; }
    switch (v->t) {
    case Value::NIL: out += "nil"; return;
    case Value::NUM: {
        std::ostringstream os;
        if (v->num.size() == 1) { put_double(os, v->num[0]); out += os.str(); return; }
        os << "("; for (size_t i = 0; i < v->num.size() && (size_t)os.tellp() + out.size() < limit; i++) { if (i) os << " "; put_double(os, v->num[i]); } os << ")";
        out += os.str(); return; }
    case Value::STR: case Value::SYM: out += v->s.size() + out.size() > limit + 1 ? v->s.substr(0, limit + 1 - std::min(limit + 1, out.size())) : v->s; return;
    case Value::FN: out += v->op ? "<builtin>" : v->s.empty() ? "<fn>" : "<fn " + v->s + ">"; return;
    case Value::LIST:
        out += "(";
        for (size_t i = 0; i < v->l.size() && out.size() < limit; i++) { if (i) out += " "; str_of_into(v->l[i], out, limit, depth + 1); }
        out += ")"; return;
    case Value::OPAQUE: out += "<opaque:" + v->opaque_tag + ">"; return;
    }
}
inline std::string str_of_limited(const vptr& v, size_t limit) { std::string s; str_of_into(v, s, limit, 0); if (s.size() > limit) s = s.substr(0, limit) + " ..."; return s; }
// Structural equality. It recurses into nested lists; a structure too deep for the stack (or one that
// contains itself, which would never end) is an error rather than a crash.
inline bool equal_at(const vptr& a, const vptr& b, int depth) {
    if (a == b) return true;                       // the same object: equal without a walk (a score holds thousands of notes, each its database)
    if (!a || !b) return (!a || a->t == Value::NIL) && (!b || b->t == Value::NIL);
    if (a->t != b->t) return false;
    switch (a->t) {
    case Value::NIL: return true;
    case Value::NUM: if (a->num.size() != b->num.size()) return false;
        for (size_t i=0; i<a->num.size(); i++) { if (a->num[i] != b->num[i]) return false; }
        return true;
    case Value::STR: case Value::SYM: return a->s == b->s;
    case Value::LIST: if (a->l.size() != b->l.size()) return false;
        if (depth > 100000 || ((depth & 31) == 31 && stack_low()))
            throw Error("", 0, "equal?: the lists are nested too deep to compare (or one contains itself)");
        for (size_t i=0; i<a->l.size(); i++) { if (!equal_at(a->l[i], b->l[i], depth + 1)) return false; }
        return true;
    case Value::FN: return a.get() == b.get() || (a->op && a->op == b->op);
    case Value::OPAQUE: return a->opaque.get() == b->opaque.get();
    }
    return false;
}
inline bool equal(const vptr& a, const vptr& b) { return equal_at(a, b, 0); }

// parsing and lexing
struct Parser {
    std::string src; size_t pos = 0; int line = 1; sptr file;
    bool literal = true;                  // mark what is read as literals of the program text (parse makes plain data)
    int depth = 0;                        // nesting of ( { and ' being read
    static constexpr int max_depth = 3000;
    Parser(std::string s, sptr f) : src(std::move(s)), file(std::move(f)) {}
    bool eof() const { return pos >= src.size(); }
    char peek() const { return src[pos]; }
    [[noreturn]] void err(int ln, const std::string& m) { throw Error(file?*file:"<input>", ln, m); }
    vptr mark(vptr v) { v->lit = literal; return v; }
    void skip_h() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') { pos++; continue; }
            if (c == '\\' && pos+1 < src.size() && src[pos+1] == '\n') { pos += 2; line++; continue; }
            if (c == ';') { while (!eof() && peek() != '\n') pos++; continue; }
            break;
        }
    }
    void skip_all() { while (true) { skip_h(); if (eof()) break; if (peek() != '\n') break; pos++; line++; } }
    struct nest { Parser& p; nest(Parser& q, int ln) : p(q) { if (++p.depth > max_depth || stack_low()) { --p.depth; p.err(ln, "nested too deep (more than " + std::to_string(max_depth) + " levels)"); } } ~nest() { --p.depth; } };
    vptr read() {
        skip_h();
        if (eof()) err(line, "unexpected end of input");
        char c = peek(); int ln = line;
        if (c == '(') {
            nest n(*this, ln);
            pos++; vlist out; skip_all();
            while (!eof() && peek() != ')') { out.push_back(read()); skip_all(); }
            if (eof()) err(ln, "missing )");
            pos++;
            return mark(v_list(std::move(out), ln, file));
        }
        if (c == '{') {
            nest n(*this, ln);
            pos++; vlist out = { mark(v_sym("do")) };
            while (true) {
                skip_h();
                if (eof()) err(ln, "missing }");
                if (peek() == '}') { pos++; break; }
                if (peek() == '\n') { pos++; line++; continue; }
                vptr lf = read_line('}');
                if (lf->t != Value::LIST || !lf->l.empty()) out.push_back(lf);
            }
            return mark(v_list(std::move(out), ln, file));
        }
        if (c == ')' || c == '}') err(ln, std::string("unexpected ") + c);
        if (c == '"') {
            pos++; std::string str;
            while (!eof() && peek() != '"') {
                if (peek() == '\\' && pos+1 < src.size()) {
                    char e = src[pos+1]; str += (e=='n'?'\n':e=='t'?'\t':e=='r'?'\r':e=='0'?'\0':e); pos += 2;
                } else { if (peek() == '\n') line++; str += src[pos++]; }
            }
            if (eof()) err(ln, "missing closing \"");
            pos++; return mark(v_str(std::move(str)));
        }
        if (c == '\'') { nest n(*this, ln); pos++; vptr q = read(); return mark(v_list({ mark(v_sym("quote")), q }, ln, file)); }
        std::string w;
        while (!eof()) {
            char k = peek();
            if (k==' '||k=='\t'||k=='\n'||k=='\r'||k=='('||k==')'||k=='{'||k=='}'||k=='"'||k==';'||k=='\'') break;
            w += src[pos++];
        }
        if (w.empty()) err(line, "empty word");
        char* e = nullptr; double d = std::strtod(w.c_str(), &e);
        return mark((e && *e == '\0' && e != w.c_str()) ? v_num(d) : v_sym(std::move(w)));
    }
    // At the top level an indented line continues the command above it (a function's body on
    // the next line, a long call split over several); inside { } lines are separate commands.
    bool continues_here() {
        size_t p = pos;                                   // just after a newline: indentation, then something that is not a blank line or a comment
        while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) p++;
        return p > pos && p < src.size() && src[p] != '\n' && src[p] != '\r' && src[p] != ';' && src[p] != '}' && src[p] != ')';
    }
    vptr read_line(char term = 0) {
        skip_h();
        int ln = line;
        vlist out;
        while (!eof()) {
            char c = peek();
            if (c == '\n') { pos++; line++; if (term == 0 && !out.empty() && continues_here()) { skip_h(); continue; } break; }
            if (c == term) break;
            if (c == ')' || (c == '}' && term != '}')) err(line, std::string("unexpected ") + c);
            out.push_back(read());
            skip_h();
        }
        // A line holding a single list is that list; a single literal is that value.
        // A single symbol is still a command: `ctr` calls ctr.
        if (out.size() == 1 && out[0]->t != Value::SYM) return out[0];
        return mark(v_list(std::move(out), ln, file));
    }
    vptr program() {
        vlist out = { mark(v_sym("do")) };
        while (true) {
            skip_h();
            if (eof()) break;
            if (peek() == '\n') { pos++; line++; continue; }
            vptr lf = read_line(0);
            if (lf->t != Value::LIST || !lf->l.empty()) out.push_back(lf);
        }
        return mark(v_list(std::move(out), 1, file));
    }
};

// What a program may do (Interp::caps). A flag off refuses the whole kind; the roots, when not empty,
// limit reading or writing to the files under them (resolved: ".." and symbolic links cannot step out).
struct Capabilities {
    bool read = true, write = true, exec = true, net = true, blocking = true, exit = true;
    std::vector<std::string> write_roots;   // empty: anywhere
    std::vector<std::string> read_roots;    // empty: anywhere
    static Capabilities full() { return Capabilities(); }
    // no programs, no network, no waiting, no exit; writing only under write_roots (none: no writing),
    // reading only under read_roots (none: anywhere)
    static Capabilities sandboxed(std::vector<std::string> write_roots = {}, std::vector<std::string> read_roots = {}) {
        Capabilities c; c.exec = c.net = c.blocking = c.exit = false;
        c.write = !write_roots.empty(); c.write_roots = std::move(write_roots); c.read_roots = std::move(read_roots);
        return c;
    }
};
// an absolute path with ".", ".." and the symbolic links of the part that exists resolved
inline fs::path resolved_path(const std::string& path) {
    std::error_code ec; fs::path p = fs::absolute(fs::path(path), ec);
    if (ec) p = fs::path(path);
    fs::path c = fs::weakly_canonical(p, ec);
    return (ec ? p.lexically_normal() : c);
}
// whether p is a root or inside one: compared as normalised text with '/' separators (and without regard to
// case on Windows, whose file names ignore it), so "C:\\x" and "c:/x/" are the same folder
inline std::string path_key(const fs::path& p) {
    std::string s = p.lexically_normal().generic_string();
#ifdef _WIN32
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
#endif
    while (s.size() > 1 && s.back() == '/' && !(s.size() == 3 && s[1] == ':')) s.pop_back();
    return s;
}
inline bool path_under(const fs::path& p, const std::vector<std::string>& roots) {
    std::string k = path_key(p);
    for (auto& r : roots) {
        std::string rk = path_key(resolved_path(r));
        if (k == rk) return true;
        if (k.size() > rk.size() && k.compare(0, rk.size(), rk) == 0 && (rk.back() == '/' || k[rk.size()] == '/')) return true;
    }
    return false;
}

struct Frame { vptr name; sptr file; int line; };   // call-stack entry; formatted lazily on error
struct Interp {
    eptr library;                              // the builtins and the libraries (files found on the load path)
    eptr global;                               // the program's names, and the library's too (shared until the program redefines one)
    std::unordered_set<std::string> shared_names;   // the library's names the program has not redefined
    // names a host leaves out on purpose, and why: still undefined (defined? says no), but the error says why
    std::unordered_map<std::string, std::string> absent;
    std::string undefined(const std::string& name) const {
        auto it = absent.find(name);
        return "undefined: " + name + (it == absent.end() ? "" : " (" + it->second + ")");
    }
    std::vector<Frame> call_stack;
    int stack_depth = 0, max_stack = 100000;   // depth cap (secondary; the stack of the thread is the real guard)
    std::uintptr_t stack_base = 0;             // where the outermost evaluation on this thread started
    size_t stack_budget = 0;                   // bytes of C++ stack eval may use; 0: what the thread has (hosts may set a limit)
    std::uintptr_t stack_floor = 0;            // evaluation stops with an error below this address
    int function_depth = 0, loop_depth = 0;
    enum flow_t { FLOW_NONE = 0, FLOW_RETURN, FLOW_BREAK, FLOW_CONTINUE };
    int flow = FLOW_NONE; vptr flow_value;     // a return, break or continue on its way out (not an exception)
    unsigned long eval_count = 0;
    std::atomic<bool> stop_requested{false};
    std::function<void()> yield_fn;         // called every 1024 evals, for hosts that need to breathe
    std::function<void()> idle_fn;          // background work (schedulers, servers): called with yield_fn, from sleep,
                                            // and by hosts while they wait for input; never re-entered
    bool in_idle = false;
    int waiting = 0;                        // > 0 inside a builtin that waits (sleep): a safe point for background code
    void idle() { if (idle_fn && !in_idle) { in_idle = true; try { idle_fn(); } catch (...) { in_idle = false; throw; } in_idle = false; } }
    std::vector<std::weak_ptr<Env>> tracked_envs;
    size_t compact_at = 1024, gc_threshold = 4096;
    bool gc_enabled = true;
    std::unordered_set<std::string> loaded_files;   // ran to completion; a second load is a no-op unless the file changed
    std::unordered_map<std::string, std::pair<fs::file_time_type, std::uintmax_t>> loaded_stamps;   // when and how large it was then
    std::unordered_set<std::string> loading_files;  // currently running; a load from inside is a cycle and is skipped
    std::unordered_set<std::string> library_files;  // the files that run in the library environment
    std::vector<std::string> load_path;     // extra directories searched by load (after the loading file's own)
    std::string home_path;                  // ~/.musil, searched after load_path
    sptr current_file; int current_line = 0;
    sptr main_file;                         // the program that was started (the first file run, not loaded)
    std::mt19937_64 rng;
    std::ostream* out = &std::cout;
    const char* who = "";                   // name of the builtin being executed (for error messages)
    Capabilities caps;                      // what the program may do (see the top of this file)
    bool& allow_blocking = caps.blocking;   // false: builtins that wait (sleep, input, exec) refuse to run;
                                            // a host whose code must answer at once (a box in a patch) sets it
    std::unordered_map<const void*, std::shared_ptr<void>> extensions;   // state<T>()
    vptr quote_op;                          // the quote builtin itself: apply quotes its arguments with it

    Interp();
    ~Interp();
    Interp(const Interp&) = delete;
    Interp& operator=(const Interp&) = delete;
    // a T that belongs to this interpreter, made on first use (the libraries' tables, servers, schedulers)
    template <class T> T& state() {
        static const char key = 0;
        auto& p = extensions[&key];
        if (!p) p = std::make_shared<T>();
        return *static_cast<T*>(p.get());
    }
    // --- stop (see the top of this file)
    void request_stop() noexcept { stop_requested.store(true, std::memory_order_relaxed); }
    void clear_stop() noexcept { stop_requested.store(false, std::memory_order_relaxed); }
    bool stopping() const noexcept { return stop_requested.load(std::memory_order_relaxed); }
    void check_stop() { if (stop_requested.load(std::memory_order_relaxed)) throw Stop_signal{}; }
    // --- capabilities
    [[noreturn]] void refuse(const std::string& what) { err((who && *who ? std::string(who) + ": " : std::string()) + "not allowed here: " + what); }
    void require_read(const std::string& path) {
        if (!caps.read) refuse("read " + resolved_path(path).string());
        if (!caps.read_roots.empty()) { fs::path p = resolved_path(path); if (!path_under(p, caps.read_roots)) refuse("read " + p.string()); }
    }
    void require_write(const std::string& path) {
        if (!caps.write) refuse("write " + resolved_path(path).string());
        if (!caps.write_roots.empty()) { fs::path p = resolved_path(path); if (!path_under(p, caps.write_roots)) refuse("write " + p.string()); }
    }
    void require_exec(const std::string& what) { if (!caps.exec) refuse("run a program (" + what + ")"); }
    void require_net(const std::string& what) { if (!caps.net) refuse("use the network (" + what + ")"); }
    void require_blocking(const std::string& what) { if (!caps.blocking) refuse(what + ": this code must not wait"); }
    void require_exit() { if (!caps.exit) refuse("end the program"); }
    void blocking(const char* name) { if (!caps.blocking) err(std::string(name) + ": not allowed here: this code must not wait"); }
    // the builtin named in errors, for the length of a scope
    struct who_scope { const char*& w; const char* saved; who_scope(Interp& i, const char* n) : w(i.who), saved(i.who) { w = n; } ~who_scope() { w = saved; } };
    // a path to read: resolved like read_path, then checked; a path to write: checked
    std::string readable(const std::string& path) { std::string p = read_path(path); require_read(p); return p; }
    // where a relative path is written: the current directory, unless the host sets write_dir (Moritz: the
    // folder of the code box's patch); the result is the path to open, checked against the capabilities
    std::string write_dir;
    std::string writable(const std::string& path) {
        std::string p = !write_dir.empty() && !path.empty() && !std::filesystem::path(path).is_absolute() ? write_dir + "/" + path : path;
        require_write(p);
        return p;
    }
    // the largest vector or list a builtin makes (elements): beyond it an error rather than gigabytes
    static constexpr double max_elements = 268435456.0;   // 2^28: 2 GB of doubles
    size_t check_size(double n, const char* what) {
        if (!(n >= 0)) err(std::string(what) + ": negative or invalid size");
        if (n > max_elements) err(std::string(what) + ": too large (" + std::to_string((long long)std::min(n, 9e18)) + " elements; the limit is 2^28)");
        return (size_t)n;
    }
    eptr make_env(eptr parent = nullptr) {
        auto e = std::make_shared<Env>(std::move(parent));
        tracked_envs.push_back(e);
        if (tracked_envs.size() >= compact_at) maintain();
        return e;
    }
    void maintain() {            // forget the environments gone; collect cycles when the live ones keep growing
        tracked_envs.erase(std::remove_if(tracked_envs.begin(), tracked_envs.end(),
            [](const std::weak_ptr<Env>& w) { return w.expired(); }), tracked_envs.end());
        if (gc_enabled && tracked_envs.size() > gc_threshold) {
            collect();
            gc_threshold = std::max<size_t>(4096, tracked_envs.size() * 2);
        }
        compact_at = tracked_envs.size() + 1024;
    }
    size_t collect();
    std::vector<std::string> names();       // every name defined: the program's and the libraries', sorted
    [[noreturn]] void err(const std::string& m) { throw Error(current_file?*current_file:"<input>", current_line, m); }
    void yield_check() {
        if (stop_requested.load(std::memory_order_relaxed)) throw Stop_signal{};
        if ((++eval_count & 1023) == 0) { if (yield_fn) yield_fn(); idle(); }
    }
    void def(const std::string& name, op_t f, int amin, int amax) { def(name, v_op(f, name, amin, amax)); }
    void def(const std::string& name, op_t f, int arity = 0) { def(name, f, arity, arity); }
    void def(const std::string& name, vptr v) { library->vars[name] = v; global->vars[name] = std::move(v); shared_names.insert(name); }
    vptr eval(vptr expr, eptr e);           // evaluate: a return reaching it is its value
    vptr ev(vptr expr, eptr e);             // the evaluator itself: a return, break or continue is left in flow
    vptr call_fn(vptr fn, vlist args);
    vptr run(const std::string& src, const std::string& filename = "<input>");
    vptr run_in(const std::string& src, const std::string& filename, eptr env);
    void load(const std::string& path);
    void reload(const std::string& path);   // run a file again even if it did not change
    std::string find_file(const std::string& path, bool* in_library = nullptr);   // the resolved path load would use, or ""
    void set_stack_floor(std::uintptr_t here) {
        const thread_stack& t = this_thread_stack();
        stack_base = here;
        stack_floor = t.low + t.reserve;
        if (stack_budget && here > stack_budget && here - stack_budget > stack_floor) stack_floor = here - stack_budget;
    }
    // A path to read from: as given if it exists (or is absolute); otherwise relative to the
    // directory of the file being run, so a script finds its data wherever it is started from.
    // A relative path for reading: as it is (the current directory), else next to the file whose code is
    // running, else next to the program that was started (so a library function reading "data/x.wav"
    // on behalf of examples/foo.mu finds examples/data/x.wav)
    std::string read_path(const std::string& path) {
        fs::path p(path); std::error_code ec;
        if (p.is_absolute() || fs::exists(p, ec)) return path;
        for (const sptr& f : { current_file, main_file }) {
            if (!f) continue;
            fs::path base = fs::path(*f).parent_path(); if (base.empty()) continue;
            fs::path q = base / p; if (fs::exists(q, ec)) return q.string();
        }
        return path;
    }
    void repl();
    [[noreturn]] void bad(const std::string& m) { err(std::string(who) + ": " + m); }
    const varr& num(const vptr& v) {
        if (!v || v->t != Value::NUM) bad(std::string("expected number, got ") + type_name(v));
        return v->num;
    }
    double scalar(const vptr& v) {
        const varr& n = num(v);
        if (n.size() != 1) bad("expected scalar, got vector of size " + std::to_string(n.size()));
        return n[0];
    }
    long index(const vptr& v) {
        double d = scalar(v);
        if (d != std::floor(d)) bad("expected integer index, got " + str_of(v));
        if (std::fabs(d) > 9e15) bad("index out of range: " + str_of(v));
        return (long)d;
    }
    const std::string& str(const vptr& v) {
        if (!v || v->t != Value::STR) bad(std::string("expected string, got ") + type_name(v));
        return v->s;
    }
    vlist& list(const vptr& v) {
        if (!v || v->t != Value::LIST) bad(std::string("expected list, got ") + type_name(v));
        return v->l;
    }
    const vptr& fn(const vptr& v) {
        if (!v || v->t != Value::FN) bad(std::string("expected function, got ") + type_name(v));
        return v;
    }
    // a literal of the program text may not be changed in place
    void mutable_target(const vptr& v) { if (v && v->lit) bad("cannot change a constant of the program text in place (make a copy with copy)"); }
    vptr call_op(const vptr& f, vlist& args) {
        if ((int)args.size() < f->amin || (f->amax >= 0 && (int)args.size() > f->amax)) {
            std::string want = f->amax < 0 ? "at least " + std::to_string(f->amin)
                             : f->amin == f->amax ? std::to_string(f->amin)
                             : std::to_string(f->amin) + "-" + std::to_string(f->amax);
            err(f->s + ": expected " + want + " argument" + (want == "1" ? "" : "s") + ", got " + std::to_string(args.size()));
        }
        struct guard { const char*& w; const char* saved; ~guard() { w = saved; } } g{who, who};
        who = f->s.c_str();
        try { return f->op(args, *this); }
        catch (const Error&) { throw; }
        catch (const std::bad_alloc&) { err(f->s + ": out of memory"); }
        catch (const std::length_error&) { err(f->s + ": too large"); }
        catch (const std::exception& ex) { err(f->s + ": " + ex.what()); }     // every C++ exception is a Musil error
    }
};
template <class F>
inline varr bcast(const varr& a, const varr& b, F f, Interp& i) {
    if (a.size() == b.size()) { varr r(a.size()); for (size_t k=0; k<a.size(); k++) r[k] = f(a[k], b[k]); return r; }
    if (a.size() == 1) { varr r(b.size()); for (size_t k=0; k<b.size(); k++) r[k] = f(a[0], b[k]); return r; }
    if (b.size() == 1) { varr r(a.size()); for (size_t k=0; k<a.size(); k++) r[k] = f(a[k], b[0]); return r; }
    i.bad("size mismatch (" + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + ")");
}
inline int op_prec(const std::string& o) {
    if (o == "||") return 1;
    if (o == "&&") return 2;
    if (o == "==" || o == "!=") return 3;
    if (o == "<" || o == ">" || o == "<=" || o == ">=") return 4;
    if (o == "+" || o == "-")  return 5;
    if (o == "*" || o == "/" || o == "%")  return 6;
    return 0;
}
// expr: the infix expression is read and evaluated in one pass; with run false it is only read (the side of
// && or || that is not needed: like and and or, it is not evaluated). A return, break or continue inside
// stops the reading and is passed up (the result is then nullptr).
inline vptr expr_parse(vlist& w, size_t& p, int min_p, Interp& i, const eptr& e, bool run);
inline vptr expr_atom(vlist& w, size_t& p, Interp& i, const eptr& e, bool run) {
    if (p >= w.size()) i.err("expr: unexpected end of expression");
    if (stack_low()) i.err("expr: nested too deep");
    vptr a = w[p];
    if (a->t == Value::NUM) { p++; return a; }
    if (a->t == Value::SYM) {
        if (a->s == "-") { p++; vptr v = expr_atom(w, p, i, e, run); if (!run || !v) return v; return v_arr(-i.num(v)); }
        if (op_prec(a->s)) i.err("expr: unexpected operator " + a->s);
        p++; if (!run) return a;
        auto v = e->find(a->s);
        if (!v) i.err("expr: " + i.undefined(a->s));
        return v;
    }
    if (a->t == Value::LIST) {
        p++;
        // (a op b ...) nested in an expr is a sub-expression; anything else is a call.
        vlist& sub = a->l;
        bool infix = sub.size() == 1 || (sub.size() >= 2 && sub[1]->t == Value::SYM && op_prec(sub[1]->s) > 0);
        if (!infix) { if (!run) return a; vptr r = i.ev(a, e); return i.flow ? nullptr : r; }
        size_t q = 0; vptr r = expr_parse(sub, q, 0, i, e, run);
        if (!r) return r;
        if (q < sub.size()) i.err("expr: unexpected " + str_of(sub[q]));
        return r;
    }
    if (a->t == Value::STR) { p++; return a; }
    i.err("expr: bad token " + str_of(a));
}
inline vptr expr_parse(vlist& w, size_t& p, int min_p, Interp& i, const eptr& e, bool run) {
    vptr lhs = expr_atom(w, p, i, e, run);
    if (!lhs) return lhs;
    while (p < w.size()) {
        if (w[p]->t != Value::SYM) i.err("expr: expected operator, got " + str_of(w[p]));
        int prec = op_prec(w[p]->s);
        if (prec == 0) i.err("expr: unknown operator " + w[p]->s);
        if (prec < min_p) break;
        std::string op = w[p++]->s;
        if (op == "&&" || op == "||") {                   // short-circuit, as and / or: the right side runs only when it decides
            bool l = run && truthy(lhs);
            bool need = run && (op == "&&" ? l : !l);
            vptr rhs = expr_parse(w, p, prec + 1, i, e, need);
            if (!rhs) return rhs;
            if (run) lhs = v_bool(need ? truthy(rhs) : l);
            continue;
        }
        vptr rhs = expr_parse(w, p, prec + 1, i, e, run);
        if (!rhs) return rhs;
        if (!run) continue;
        if (op == "==") { if (lhs->t != Value::NUM || rhs->t != Value::NUM) { lhs = v_bool(equal(lhs, rhs)); continue; } }
        if (op == "!=") { if (lhs->t != Value::NUM || rhs->t != Value::NUM) { lhs = v_bool(!equal(lhs, rhs)); continue; } }
        const varr& L = i.num(lhs); const varr& R = i.num(rhs);
        auto bo = [&](auto f) { return v_arr(bcast(L, R, f, i)); };
        if      (op == "+")  lhs = bo([](double a, double b){ return a + b; });
        else if (op == "-")  lhs = bo([](double a, double b){ return a - b; });
        else if (op == "*")  lhs = bo([](double a, double b){ return a * b; });
        else if (op == "/")  lhs = bo([](double a, double b){ return a / b; });
        else if (op == "%")  lhs = bo([](double a, double b){ return std::fmod(a, b); });
        else if (op == "<")  lhs = bo([](double a, double b){ return a <  b ? 1.0 : 0.0; });
        else if (op == ">")  lhs = bo([](double a, double b){ return a >  b ? 1.0 : 0.0; });
        else if (op == "<=") lhs = bo([](double a, double b){ return a <= b ? 1.0 : 0.0; });
        else if (op == ">=") lhs = bo([](double a, double b){ return a >= b ? 1.0 : 0.0; });
        else if (op == "==") lhs = bo([](double a, double b){ return a == b ? 1.0 : 0.0; });
        else if (op == "!=") lhs = bo([](double a, double b){ return a != b ? 1.0 : 0.0; });
    }
    return lhs;
}

// evaluation
// === Special forms (documented here; dispatched in eval) =====
// (quote x) 'x: x itself, unevaluated: code as data (a quoted list is a fresh copy each time)
// (do forms...) evaluate in order, return the last; what { } builds
// (if cond then [else]) cond is true unless it is 0, nil, empty, or a vector with a zero
// (while cond body) loop; (break) and (continue) inside it
// (for init cond step body) C-style loop: (for (var i 0) (< i 10) (var i (+ i 1)) { ... })
// (var name value) define name in the current environment; a function's var is always local
// (set name value) assign to the nearest existing name, however far out; error if none
// (function name (params) body) define a function; (function (params) body) an anonymous one
// (return [value]) leave the function; free in tail position
// (break) (continue) leave or restart the innermost loop
// (try body catch e handler) run body; on an error bind its message to e and run handler
// (expr (a op b ...)) infix arithmetic: + - * / % < > <= >= == != && ||; ( ) groups, (f x) calls; && and || short-circuit
// (parse "text") read Musil source into a form: (do line ...), the same reading a file gets; (eval (parse s)) runs it
// (eval form) evaluate a form (code built as data) in the global environment
// (apply f list) call f with the elements of list as arguments
inline vptr fn_quote   (vlist&, Interp&) { return v_nil(); }
inline vptr fn_do      (vlist&, Interp&) { return v_nil(); }
inline vptr fn_if      (vlist&, Interp&) { return v_nil(); }
inline vptr fn_and     (vlist& a, Interp&) { for (auto& x : a) if (!truthy(x)) return v_bool(false); return v_bool(true); }   // (apply and ...) still works
inline vptr fn_or      (vlist& a, Interp&) { for (auto& x : a) if (truthy(x))  return v_bool(true); return v_bool(false); }
inline vptr fn_while   (vlist&, Interp&) { return v_nil(); }
inline vptr fn_for     (vlist&, Interp&) { return v_nil(); }
inline vptr fn_var     (vlist&, Interp&) { return v_nil(); }
inline vptr fn_set     (vlist&, Interp&) { return v_nil(); }
inline vptr fn_function(vlist&, Interp&) { return v_nil(); }
inline vptr fn_return  (vlist&, Interp&) { return v_nil(); }
inline vptr fn_break   (vlist&, Interp&) { return v_nil(); }
inline vptr fn_continue(vlist&, Interp&) { return v_nil(); }
inline vptr fn_try     (vlist&, Interp&) { return v_nil(); }
inline vptr fn_expr    (vlist&, Interp&) { return v_nil(); }
inline vptr fn_eval    (vlist&, Interp&) { return v_nil(); }
inline vptr fn_apply   (vlist&, Interp&) { return v_nil(); }
inline vptr fn_help    (vlist&, Interp&) { return v_nil(); }
inline void print_help(Interp& i, const std::string& name);
inline vptr make_partial(Interp& i, const vptr& fn, const vlist& args) {
    auto bound = i.make_env(fn->closure);
    for (size_t k=0; k<args.size(); k++) bound->vars[fn->params[k]] = unlit(args[k]);
    auto curr = v_nil(); curr->t = Value::FN; curr->closure = bound; curr->body = fn->body;
    curr->params = std::vector<std::string>(fn->params.begin() + args.size(), fn->params.end());
    return curr;
}
inline bool is_form(const vptr& v, const char* name) { return v && v->t == Value::LIST && !v->l.empty() && v->l[0]->t == Value::SYM && v->l[0]->s == name; }
inline bool ends_in_return(const vptr& v) {
    if (is_form(v, "return")) return true;
    if (is_form(v, "do")) return v->l.size() > 1 && ends_in_return(v->l.back());
    if (is_form(v, "if")) return v->l.size() == 4 && ends_in_return(v->l[2]) && ends_in_return(v->l[3]);
    return false;
}
// A function's body with its early returns moved to tail position where that is possible (the statements
// after an (if c { ... return x }) become its else branch), so they leave without passing up a return.
// Done once per (function ...) form: the form keeps the result.
inline vptr hoist_returns(const vptr& v) {
    if (!v || v->t != Value::LIST || v->l.empty()) return v;
    if (stack_low()) return v;                          // a body nested beyond the stack is left as it is (returns still work)
    if (is_form(v, "if")) {
        vlist n = v->l; for (size_t k=2; k<n.size(); k++) n[k] = hoist_returns(n[k]);
        return v_list(std::move(n), v->line, v->file);
    }
    if (!is_form(v, "do")) return v;
    vlist out = { v->l[0] };
    for (size_t k=1; k<v->l.size(); k++) {
        vptr f = hoist_returns(v->l[k]);
        bool last = k + 1 == v->l.size();
        if (!last && is_form(f, "if") && (f->l.size() == 3 || f->l.size() == 4)) {
            vptr T = f->l[2], E = f->l.size() == 4 ? f->l[3] : nullptr;
            if (ends_in_return(T) || (E && ends_in_return(E))) {
                vlist rest = { v->l[0] }; for (size_t j=k+1; j<v->l.size(); j++) rest.push_back(v->l[j]);
                vptr rest_do = hoist_returns(v_list(std::move(rest), v->line, v->file));
                if (!ends_in_return(T)) T = v_list({ v->l[0], T, rest_do }, T->line, T->file);
                if (!E) E = rest_do; else if (!ends_in_return(E)) E = v_list({ v->l[0], E, rest_do }, E->line, E->file);
                out.push_back(v_list({ f->l[0], f->l[1], T, E }, f->line, f->file));
                return v_list(std::move(out), v->line, v->file);
            }
        }
        out.push_back(f);
    }
    return v_list(std::move(out), v->line, v->file);
}
inline std::string frame_label(const Frame& f) {
    return (f.name && f.name->t == Value::SYM ? f.name->s : std::string("<anon>")) + "() at " +
        (f.file ? *f.file : "?") + ":" + std::to_string(f.line);
}
inline void add_trace(Error& fe, const std::vector<Frame>& call_stack) {
    if (!fe.trace.empty() || call_stack.empty()) return;
    // innermost first. Deep recursions are abbreviated: 12 innermost, a count, 4 outermost.
    size_t n = call_stack.size(), head = 12, tail = 4;
    for (size_t k = 0; k < n; k++) {
        if (n > head + tail + 1 && k == head) { fe.trace.push_back("... " + std::to_string(n - head - tail) + " more frames ..."); k = n - tail - 1; continue; }
        fe.trace.push_back(frame_label(call_stack[n - 1 - k]));
    }
    fe.cached.clear();
}
inline vptr Interp::eval(vptr expr, eptr e) {
    vptr r = ev(std::move(expr), std::move(e));
    if (flow) { if (flow == FLOW_RETURN) r = std::move(flow_value); flow = FLOW_NONE; flow_value.reset(); }
    return r;
}
// A return, break or continue sets flow and leaves; every place that evaluates a part of a form checks flow
// after it and passes it up (MUSIL_FLOW). A return stops at the frame that entered its function; a break or a
// continue at its loop.
#define MUSIL_FLOW(val) do { if (flow) { \
    if (flow == FLOW_RETURN && entered_fn) { flow = FLOW_NONE; vptr rv_ = std::move(flow_value); cleanup(); return rv_ ? rv_ : v_nil(); } \
    cleanup(); return (val); } } while (0)
inline vptr Interp::ev(vptr expr, eptr e) {
    if (++stack_depth > max_stack) { --stack_depth; err("stack overflow (depth " + std::to_string(max_stack) + ")"); }
    struct guard { int& d; ~guard() { --d; } } g{stack_depth};
    {   // the thread's stack, measured: frame sizes differ between compilers and thread stacks between hosts
        std::uintptr_t here = MUSIL_FRAME_ADDRESS();
        if (stack_depth == 1) set_stack_floor(here);
        if (here < stack_floor) err("stack overflow (recursion too deep)");
    }
    yield_check();
    bool entered_fn = false;        // did we enter a user fn in this frame?
    int saved_loop = loop_depth;
    sptr saved_file; int saved_line = 0;    // where the function was called from: restored when it returns
    auto cleanup = [&]() {
        if (entered_fn) { call_stack.pop_back(); --function_depth; loop_depth = saved_loop; current_file = saved_file; current_line = saved_line; entered_fn = false; }
    };
try {
    while (true) {
        if (!expr) { cleanup(); return v_nil(); }
        if (expr->t != Value::SYM && expr->t != Value::LIST) { cleanup(); return expr; }
        if (expr->t == Value::SYM) {
            auto v = e->find(expr->s);
            if (!v) err(undefined(expr->s));
            cleanup(); return v;
        }
        if (expr->line) current_line = expr->line;
        if (expr->file) current_file = expr->file;
        auto& l = expr->l;
        if (l.empty()) { cleanup(); return v_nil(); }
        if (l.size() == 1 && l[0]->t == Value::LIST) { expr = l[0]; continue; }
        vptr head;
        if (l[0]->t == Value::SYM) {
            head = e->find(l[0]->s);
            if (!head) err(undefined(l[0]->s));
        } else if (l[0]->t == Value::FN) head = l[0];
        else { head = ev(l[0], e); MUSIL_FLOW(head); }
        op_t op = head->op;

        if (op == fn_quote) { cleanup(); return l.size() > 1 ? copy_literal(l[1]) : v_nil(); }
        if (op == fn_do) {
            if (l.size() == 1) { cleanup(); return v_nil(); }
            for (size_t i=1; i+1<l.size(); i++) { vptr r = ev(l[i], e); MUSIL_FLOW(r); }
            expr = l.back(); continue;
        }
        if (op == fn_and || op == fn_or) {                    // short-circuit: arguments are evaluated left to right, only as far as needed
            bool is_and = op == fn_and;
            if (l.size() == 1) { cleanup(); return v_bool(is_and); }
            for (size_t i = 1; i + 1 < l.size(); i++) {
                vptr x = ev(l[i], e); MUSIL_FLOW(x);
                bool t = truthy(x);
                if (is_and && !t) { cleanup(); return v_bool(false); }
                if (!is_and && t) { cleanup(); return v_bool(true); }
            }
            expr = l.back(); continue;                         // the last argument decides, in tail position
        }
        if (op == fn_if) {
            if (l.size() < 3 || l.size() > 4) err("if: expected (if cond then [else])");
            vptr c = ev(l[1], e); MUSIL_FLOW(c);
            if (truthy(c)) { expr = l[2]; continue; }
            if (l.size() == 4) { expr = l[3]; continue; }
            cleanup(); return v_nil();
        }
        if (op == fn_while || op == fn_for) {
            bool is_for = op == fn_for;
            if (!is_for && l.size() != 3) err("while: expected (while cond body)");
            if (is_for && l.size() != 5) err("for: expected (for init cond step body)");
            if (is_for) { vptr r = ev(l[1], e); MUSIL_FLOW(r); }
            const vptr& cond = l[is_for ? 2 : 1]; const vptr& body = l[is_for ? 4 : 2];
            vptr last = v_nil(); ++loop_depth;
            // a break ends this loop, a continue goes on with the step; a return goes up
            auto loop_flow = [&]() -> int { if (flow == FLOW_BREAK) { flow = FLOW_NONE; return 1; } if (flow == FLOW_CONTINUE) { flow = FLOW_NONE; return 2; } return 3; };
            while (true) {
                vptr c = ev(cond, e);
                if (flow) { int k = loop_flow(); if (k == 1) break; if (k == 2) continue; --loop_depth; MUSIL_FLOW(c); }
                if (!truthy(c)) break;
                vptr b = ev(body, e);
                if (flow) { int k = loop_flow(); if (k == 1) break; if (k == 3) { --loop_depth; MUSIL_FLOW(b); } }
                else last = std::move(b);
                if (is_for) {
                    vptr s = ev(l[3], e);
                    if (flow) { int k = loop_flow(); if (k == 1) break; if (k == 3) { --loop_depth; MUSIL_FLOW(s); } }
                }
            }
            --loop_depth; cleanup(); return last;
        }
        if (op == fn_var) {   // define (or redefine) in the current environment: a function's var never touches an outer one
            if (l.size() != 3) err("var: expected (var name value)");
            if (l[1]->t != Value::SYM) err("var: name must be a symbol, got " + str_of(l[1]));
            vptr v = ev(l[2], e); MUSIL_FLOW(v);
            v = unlit(std::move(v));
            e->bind(l[1]->s, v);
            cleanup(); return v;
        }
        if (op == fn_set) {   // assign to the nearest existing binding, however far out; error if there is none
            if (l.size() != 3) err("set: expected (set name value)");
            if (l[1]->t != Value::SYM) err("set: name must be a symbol, got " + str_of(l[1]));
            vptr v = ev(l[2], e); MUSIL_FLOW(v);
            v = unlit(std::move(v));
            if (expr->line) current_line = expr->line;
            if (!e->assign(l[1]->s, v)) err("set: undefined: " + l[1]->s);
            cleanup(); return v;
        }
        if (op == fn_function) {
            bool named; size_t pi, bi;
            if (l.size() == 4 && l[1]->t == Value::SYM && l[2]->t == Value::LIST) { named = true;  pi = 2; bi = 3; }
            else if (l.size() == 3 && l[1]->t == Value::LIST)                     { named = false; pi = 1; bi = 2; }
            else err("function: expected (function [name] (params) body)");
            // the form's prepared body and parameters: kept the first time for program text (which cannot change in
            // place), made again each time for a form built as data (which a program may have changed since)
            std::vector<std::string> ps; vptr body;
            if (!expr->lit || !expr->body) {
                for (auto& p : l[pi]->l) {
                    if (p->t != Value::SYM) err("function: parameter must be a symbol, got " + str_of(p));
                    ps.push_back(p->s);
                }
                body = hoist_returns(l[bi]);
                if (expr->lit) { expr->params = ps; expr->body = body; }
            } else { ps = expr->params; body = expr->body; }
            auto fn = v_nil(); fn->t = Value::FN; fn->closure = e; fn->body = body; fn->params = std::move(ps);
            if (named) { fn->s = l[1]->s; e->bind(l[1]->s, fn); }   // a named function remembers its name (str prints it)
            cleanup(); return fn;
        }
        if (op == fn_return) {
            if (function_depth == 0) err("return outside function");
            if (l.size() > 2) err("return: expected (return [value])");
            if (entered_fn) { // tail position — convert (return X) to evaluating X in tail position
                if (l.size() <= 1) { cleanup(); return v_nil(); }
                expr = l[1]; continue;
            }
            vptr v = l.size() > 1 ? ev(l[1], e) : v_nil(); MUSIL_FLOW(v);
            flow = FLOW_RETURN; flow_value = v; cleanup(); return v;
        }
        if (op == fn_break)    { if (loop_depth == 0) err("break outside loop"); flow = FLOW_BREAK; cleanup(); return v_nil(); }
        if (op == fn_continue) { if (loop_depth == 0) err("continue outside loop"); flow = FLOW_CONTINUE; cleanup(); return v_nil(); }
        if (op == fn_try) {
            if (l.size() != 5 || l[2]->t != Value::SYM || l[2]->s != "catch" || l[3]->t != Value::SYM)
                err("try: expected (try body catch name handler)");
            size_t depth = call_stack.size(); int fd = function_depth, ld = loop_depth;
            vptr r;
            try { r = ev(l[1], e); }
            catch (Error& ex) {
                call_stack.resize(depth); function_depth = fd; loop_depth = ld; flow = FLOW_NONE; flow_value.reset();
                auto ne = make_env(e); ne->vars[l[3]->s] = v_str(ex.what());
                vptr h = ev(l[4], ne); MUSIL_FLOW(h);
                cleanup(); return h;
            }
            MUSIL_FLOW(r);
            cleanup(); return r;
        }
        if (op == fn_expr) {
            if (l.size() != 2 || l[1]->t != Value::LIST) err("expr: expected (expr (a op b ...))");
            struct wg { const char*& w; const char* s; ~wg() { w = s; } } wguard{who, who}; who = "expr";
            size_t p = 0; vptr r = expr_parse(l[1]->l, p, 0, *this, e, true);
            if (!r && !flow) r = v_nil();
            MUSIL_FLOW(r);
            cleanup(); return r;
        }
        if (op == fn_eval) {
            if (l.size() != 2) err("eval: expected (eval form)");
            vptr f = ev(l[1], e); MUSIL_FLOW(f);
            expr = std::move(f); continue;
        }
        if (op == fn_help) {
            if (l.size() != 2 || (l[1]->t != Value::SYM && l[1]->t != Value::STR)) err("help: expected (help name)");
            print_help(*this, l[1]->s); cleanup(); return v_nil();
        }
        if (op == fn_apply) {
            if (l.size() != 3) err("apply: expected (apply fn list)");
            vptr fv = ev(l[1], e); MUSIL_FLOW(fv);
            vptr lv = ev(l[2], e); MUSIL_FLOW(lv);
            if (fv->t != Value::FN) err("apply: expected function, got " + std::string(type_name(fv)));
            if (lv->t != Value::LIST) err("apply: expected list, got " + std::string(type_name(lv)));
            vlist nl = { fv };
            for (auto& x : lv->l) nl.push_back(v_list({ quote_op, x }));
            expr = v_list(std::move(nl), expr->line, expr->file);
            continue;
        }

        // function call
        if (head->t != Value::FN) err("not callable: " + str_of(l[0]) + " (" + type_name(head) + ")");
        vlist args; args.reserve(l.size() - 1);
        for (size_t i=1; i<l.size(); i++) { vptr a = ev(l[i], e); MUSIL_FLOW(a); args.push_back(std::move(a)); }
        if (expr->line) current_line = expr->line;            // an argument may have run code elsewhere: errors are this call's
        if (expr->file) current_file = expr->file;
        if (head->op) { vptr r = call_op(head, args); cleanup(); return r; }   // op may throw: cleanup after
        // currying: too few args => return partial application
        if (args.size() < head->params.size()) { vptr r = make_partial(*this, head, args); cleanup(); return r; }
        // over-application: call with the params it takes, apply the result to the rest
        if (args.size() > head->params.size()) {
            vlist first(args.begin(), args.begin() + head->params.size());
            vptr r = call_fn(head, std::move(first));
            if (r->t != Value::FN)
                err((l[0]->t == Value::SYM ? l[0]->s : std::string("<anon>")) + ": too many arguments (expected " +
                    std::to_string(head->params.size()) + ", got " + std::to_string(args.size()) + ")");
            vlist nl = { r };
            for (size_t i=head->params.size(); i<args.size(); i++) nl.push_back(v_list({ quote_op, args[i] }));
            expr = v_list(std::move(nl), expr->line, expr->file);
            continue;
        }
        // user function call — in-place TCO; replace top of call_stack on each
        // tail-merged call so the trace reflects current location, not history.
        Frame fr{ l[0], current_file, current_line };
        if (entered_fn) call_stack.back() = std::move(fr);
        else { call_stack.push_back(std::move(fr)); ++function_depth; loop_depth = 0; entered_fn = true; saved_file = current_file; saved_line = current_line; }
        auto ne = make_env(head->closure);
        ne->vars.reserve(head->params.size());
        for (size_t i=0; i<head->params.size(); i++) ne->vars[head->params[i]] = unlit(std::move(args[i]));
        expr = head->body; e = std::move(ne);
        // continue eval loop with new expr/env — no new C++ frame.
    }
} catch (Error& fe) {
    if (fe.file.empty() && fe.line == 0) { fe.file = current_file ? *current_file : "<input>"; fe.line = current_line; fe.cached.clear(); }
    add_trace(fe, call_stack);
    cleanup(); throw;
  }
  catch (const std::exception& ex) {                    // a C++ exception from the evaluator itself (out of memory): a Musil error
    Error fe(current_file ? *current_file : "<input>", current_line, dynamic_cast<const std::bad_alloc*>(&ex) ? "out of memory" : ex.what());
    add_trace(fe, call_stack);
    cleanup(); throw fe;
  }
  catch (...) { cleanup(); throw; }
}
#undef MUSIL_FLOW
inline vptr Interp::call_fn(vptr f, vlist args) {
    if (f->t != Value::FN) err("call: expected function, got " + std::string(type_name(f)));
    if (f->op == fn_eval) { if (args.size() != 1) err("eval: expected 1 argument"); return eval(args[0], global); }
    if (f->op == fn_apply) {
        if (args.size() != 2 || args[0]->t != Value::FN || args[1]->t != Value::LIST) err("apply: expected (apply fn list)");
        return call_fn(args[0], args[1]->l);
    }
    if (f->op) return call_op(f, args);
    if (args.size() < f->params.size()) return make_partial(*this, f, args);
    if (args.size() > f->params.size()) {
        vlist first(args.begin(), args.begin() + f->params.size());
        vptr r = call_fn(f, std::move(first));
        if (r->t != Value::FN) err("too many arguments (expected " + std::to_string(f->params.size()) + ", got " + std::to_string(args.size()) + ")");
        return call_fn(r, vlist(args.begin() + f->params.size(), args.end()));
    }
    auto ne = make_env(f->closure);
    ne->vars.reserve(f->params.size());
    for (size_t i=0; i<f->params.size(); i++) ne->vars[f->params[i]] = unlit(std::move(args[i]));
    call_stack.push_back(Frame{ nullptr, current_file, current_line });
    struct restore {
        Interp& I; int loop; sptr file; int line;
        ~restore() { I.call_stack.pop_back(); --I.function_depth; I.loop_depth = loop; I.current_file = std::move(file); I.current_line = line; }
    } rs{ *this, loop_depth, current_file, current_line };
    ++function_depth; loop_depth = 0;
    return eval(f->body, ne);           // a return in the body is its value
}

// builtins
// (+ a b ...) (- a [b ...]) (* a b ...) (/ a [b ...]) arithmetic, elementwise with size-1 broadcasting;
//   (- x) negates, (/ x) is the reciprocal, (+) is 0 and (*) is 1
#define BINOP(name, sym, init, unary) \
    inline vptr name(vlist& a, Interp& i) { \
        if (a.empty()) return v_num(init); \
        varr r = i.num(a[0]); \
        auto f = [](double x, double y){ return x sym y; }; \
        if (a.size() == 1 && unary) r = bcast(varr(init,1), r, f, i); \
        else for (size_t k=1; k<a.size(); k++) r = bcast(r, i.num(a[k]), f, i); \
        return v_arr(std::move(r)); }
BINOP(fn_add, +, 0.0, false) BINOP(fn_sub, -, 0.0, true)
BINOP(fn_mul, *, 1.0, false) BINOP(fn_div, /, 1.0, true)
// Comparisons are elementwise on numbers and lexicographic on two strings.
// (< a b) (> a b) (<= a b) (>= a b) comparisons: elementwise 1/0 on numbers, lexicographic on two strings
#define CMP(name, sym) inline vptr name(vlist& a, Interp& i) { \
    if (a[0]->t == Value::STR && a[1]->t == Value::STR) return v_bool(a[0]->s sym a[1]->s); \
    return v_arr(bcast(i.num(a[0]), i.num(a[1]), [](double x, double y){return x sym y ? 1.0 : 0.0;}, i)); }
CMP(fn_lt, <) CMP(fn_gt, >) CMP(fn_le, <=) CMP(fn_ge, >=)
// (== a b) (!= a b) equality: elementwise on numbers, structural on anything else
inline vptr fn_eq(vlist& a, Interp& i) {
    if (a[0]->t == Value::NUM && a[1]->t == Value::NUM) return v_arr(bcast(a[0]->num, a[1]->num, [](double x, double y){return x == y ? 1.0 : 0.0;}, i));
    return v_bool(equal(a[0], a[1])); }
inline vptr fn_ne(vlist& a, Interp& i) {
    if (a[0]->t == Value::NUM && a[1]->t == Value::NUM) return v_arr(bcast(a[0]->num, a[1]->num, [](double x, double y){return x != y ? 1.0 : 0.0;}, i));
    return v_bool(!equal(a[0], a[1])); }
// (equal? a b) structural equality as a single 1/0, also for vectors and nested lists
inline vptr fn_equalp(vlist& a, Interp&) { return v_bool(equal(a[0], a[1])); }
// (same? a b) are a and b the same object (not merely equal in value)? For records and lists: the very one, however
//   large; a number or a string is never the same object as another copy of it
inline vptr fn_samep(vlist& a, Interp&) { return v_bool(a[0] == a[1]); }
// (sin x) (cos x) (tan x) (asin x) (acos x) (atan x) (sqrt x) (exp x) (log x) (log2 x) (log10 x) trigonometry and logs, elementwise
// (abs x) (floor x) (ceil x) (round x) rounding and magnitude, elementwise
// (tanh x) hyperbolic tangent, elementwise (a soft clip for signals)
// (mod a b) (pow a b) (atan2 y x) remainder (sign follows fmod), power, two-argument arctangent; elementwise
#define UN(name, f)  inline vptr name(vlist& a, Interp& i) { varr r=i.num(a[0]); for (size_t k=0; k<r.size(); k++) r[k]=f(r[k]); return v_arr(std::move(r)); }
#define BIN(name, f) inline vptr name(vlist& a, Interp& i) { return v_arr(bcast(i.num(a[0]), i.num(a[1]), [](double x, double y){ return f(x, y); }, i)); }
UN(fn_sin, std::sin) UN(fn_cos, std::cos) UN(fn_tan, std::tan) UN(fn_asin, std::asin) UN(fn_acos, std::acos) UN(fn_atan, std::atan)
UN(fn_sqrt, std::sqrt) UN(fn_exp, std::exp) UN(fn_log, std::log) UN(fn_log2, std::log2) UN(fn_log10, std::log10)
UN(fn_abs, std::fabs) UN(fn_flr, std::floor) UN(fn_cei, std::ceil) UN(fn_rnd, std::round) UN(fn_tanh, std::tanh)
BIN(fn_mod, std::fmod) BIN(fn_pow, std::pow) BIN(fn_atan2, std::atan2)
// (not x) the negation of a truth value
inline vptr fn_not(vlist& a, Interp& i) { return v_bool(!truthy(a[0])); }
// (and a b ...) (or a b ...) short-circuit, left to right: (and) stops at the first false value, (or) at the first true one;
//   the last argument is returned as it is, so (or x default) is x when x is true. Special forms: later arguments are
//   not evaluated when the answer is known, so (and (> k 0) (getidx v k)) is safe.
// min/max: one vector argument => reduction; several arguments => elementwise.
// (min v) (max v) smallest or largest element of one vector; with several arguments, elementwise over them
inline vptr fn_min(vlist& a, Interp& i) {
    if (a.size() == 1) { const varr& v = i.num(a[0]); if (v.size() == 0) i.err("min: empty vector"); return v_num(v.min()); }
    varr r = i.num(a[0]);
    for (size_t k=1; k<a.size(); k++) r = bcast(r, i.num(a[k]), [](double x, double y){return x < y ? x : y;}, i);
    return v_arr(std::move(r));
}
inline vptr fn_max(vlist& a, Interp& i) {
    if (a.size() == 1) { const varr& v = i.num(a[0]); if (v.size() == 0) i.err("max: empty vector"); return v_num(v.max()); }
    varr r = i.num(a[0]);
    for (size_t k=1; k<a.size(); k++) r = bcast(r, i.num(a[k]), [](double x, double y){return x > y ? x : y;}, i);
    return v_arr(std::move(r));
}
// (list x ...) a list of anything
// (append L x ...) a new list with x ... at the end
// (push L x) append in place, returning the list; (pop L) remove and return the last element
inline vptr fn_list(vlist& a, Interp&) { return v_list(a); }
// (append L x ...) a new list with x ... added at the end
inline vptr fn_append(vlist& a, Interp& i) { vlist r = i.list(a[0]); for (size_t k=1; k<a.size(); k++) r.push_back(a[k]); return v_list(std::move(r)); }
// A list does not contain itself: putting a list into one of its own elements makes a loop that is
// never freed (until the interpreter ends) and that printing must cut short. push, setidx and put!
// refuse the loops they can see cheaply: the inserted value itself, and its first nodes down to a
// fixed budget, without allocating. A loop hidden deeper in a large structure is not looked for:
// checking it would cost as much as the structure at every insertion.
inline bool reaches(const vptr& from, const Value* target) {
    if (!from || from->t != Value::LIST) return false;
    constexpr int budget = 256;
    const Value* todo[budget]; int n = 0, seen = 0;
    todo[n++] = from.get();
    while (n > 0 && seen < budget) {
        const Value* v = todo[--n]; ++seen;
        if (v == target) return true;
        for (auto& c : v->l) {
            if (!c || c->t != Value::LIST) continue;
            if (c.get() == target) return true;
            if (n < budget) todo[n++] = c.get();
        }
    }
    return false;
}
inline void check_insert(Interp& i, const char* who, const vptr& into, const vptr& x) {
    i.mutable_target(into);
    if (reaches(x, into.get())) i.err(std::string(who) + ": a list cannot contain itself");
}
inline vptr fn_push(vlist& a, Interp& i) { i.list(a[0]); check_insert(i, "push", a[0], a[1]); a[0]->l.push_back(a[1]); return a[0]; }
// (pop L) remove and return the last element of a list, in place
inline vptr fn_pop(vlist& a, Interp& i) { vlist& l = i.list(a[0]); i.mutable_target(a[0]); if (l.empty()) i.err("pop: empty list"); vptr r = l.back(); l.pop_back(); return r; }
// (length x) (head x) (tail x) (empty? x) on a list, a vector or a string
// (getidx x k) element k of a list, vector or string; negative k counts from the end
// (setidx x k v) set element k of a list or vector in place
inline vptr fn_length(vlist& a, Interp& i) { auto& v=a[0];
    if (v->t==Value::LIST) return v_num((double)v->l.size());
    if (v->t==Value::NUM)  return v_num((double)v->num.size());
    if (v->t==Value::STR)  return v_num((double)v->s.size());
    i.err(std::string("length: expected list, number or string, got ") + type_name(v)); }
inline vptr fn_head(vlist& a, Interp& i) { auto& v=a[0];
    if (v->t==Value::LIST) { if (v->l.empty()) i.err("head: empty list"); return v->l[0]; }
    if (v->t==Value::NUM)  { if (v->num.size()==0) i.err("head: empty vector"); return v_num(v->num[0]); }
    i.err(std::string("head: expected list or number, got ") + type_name(v)); }
inline vptr fn_tail(vlist& a, Interp& i) { auto& v=a[0];
    if (v->t==Value::LIST) { if (v->l.empty()) i.err("tail: empty list"); return v_list(vlist(v->l.begin()+1, v->l.end())); }
    if (v->t==Value::NUM)  { if (v->num.size()==0) i.err("tail: empty vector"); varr r=v->num[std::slice(1, v->num.size()-1, 1)]; return v_arr(std::move(r)); }
    i.err(std::string("tail: expected list or number, got ") + type_name(v)); }
inline vptr fn_emptyp(vlist& a, Interp& i) { auto& v=a[0];
    return v_bool(v->t==Value::LIST?v->l.empty():v->t==Value::NUM?v->num.size()==0:v->t==Value::STR?v->s.empty():v->t==Value::NIL); }
inline size_t checked_index(Interp& i, const vptr& idx, size_t size) {
    long k = i.index(idx);
    if (k < 0) k += (long)size;                       // negative indices count from the end
    if (k < 0 || (size_t)k >= size) i.bad("index " + str_of(idx) + " out of range (size " + std::to_string(size) + ")");
    return (size_t)k;
}
inline vptr fn_getidx(vlist& a, Interp& i) {
    auto& v=a[0];
    if (v->t==Value::NUM)  return v_num(v->num[checked_index(i, a[1], v->num.size())]);
    if (v->t==Value::LIST) return v->l[checked_index(i, a[1], v->l.size())];
    if (v->t==Value::STR)  return v_str(std::string(1, v->s[checked_index(i, a[1], v->s.size())]));
    i.err(std::string("getidx: expected list, number or string, got ") + type_name(v)); }
inline vptr fn_setidx(vlist& a, Interp& i) {
    auto& v=a[0];
    if (v->t==Value::NUM)  { i.mutable_target(v); v->num[checked_index(i, a[1], v->num.size())] = i.scalar(a[2]); return v; }
    if (v->t==Value::LIST) { size_t k = checked_index(i, a[1], v->l.size()); check_insert(i, "setidx", v, a[2]); v->l[k] = a[2]; return v; }
    i.err(std::string("setidx: expected list or number, got ") + type_name(v)); }
// (vec x ...) a vector from numbers, vectors and lists of numbers, concatenated; a scalar is a vector of size 1
// (sum v) sum of the elements (0 for an empty vector)
inline vptr fn_vec(vlist& a, Interp& i) {
    if (a.size()==1 && a[0]->t==Value::LIST) {
        varr r(a[0]->l.size());
        for (size_t k=0; k<a[0]->l.size(); k++) r[k]=i.scalar(a[0]->l[k]);
        return v_arr(std::move(r));
    }
    size_t n = 0; for (auto& x : a) n += i.num(x).size();
    varr r(n); size_t p = 0;
    for (auto& x : a) for (size_t k=0; k<x->num.size(); k++) r[p++] = x->num[k];
    return v_arr(std::move(r));
}
inline vptr fn_sum(vlist& a, Interp& i) {
    if (a[0]->t==Value::NUM)  { double s=0; for (size_t k=0; k<a[0]->num.size(); k++) s+=a[0]->num[k]; return v_num(s); }
    if (a[0]->t==Value::LIST) { double s=0; for (auto& x : a[0]->l) s+=i.scalar(x); return v_num(s); }
    i.err(std::string("sum: expected number or list, got ") + type_name(a[0]));
}
// (print x ...) print the arguments separated by spaces, then a newline
// (exit [code]) end the program
inline vptr fn_print(vlist& a, Interp& i) { for (size_t k=0; k<a.size(); k++) { if (k) *i.out<<" "; *i.out<<str_of(a[k]); } *i.out<<"\n"<<std::flush; return v_nil(); }
inline vptr fn_exit(vlist& a, Interp& i) { i.require_exit(); throw Exit_signal{ a.empty() ? 0 : (int)i.scalar(a[0]) }; }
// (type x) "scalar" "vec" "string" "symbol" "list" "function" "nil" or "opaque:tag"
// (str x) x printed as a string
// (sym s) a symbol from a string; (num s) a number from a string
// (copy x) a shallow copy: a new list or vector with the same elements
// (error x ...) raise an error with the arguments as message
// (clock) seconds from a monotonic clock, for timing
// (load "file.mu") run a file, searched next to the loading file, then in MUSIL_PATH and ~/.musil (the
//   libraries), then in the current directory; once: a second load of the same file does nothing, unless the
//   file changed since (then it runs again: edit a module, load it again)
// (defined? 'name) is name bound?
// (vars) the global names, sorted
// (help name) print the documentation of a builtin or library function (from help.txt)
inline vptr fn_type(vlist& a, Interp& i) {
    if (a[0]->t==Value::NUM) return v_str(a[0]->num.size()==1 ? "scalar" : "vec");
    if (a[0]->t==Value::OPAQUE) return v_str("opaque:" + a[0]->opaque_tag);
    return v_str(type_name(a[0]));
}
inline vptr fn_str(vlist& a, Interp& i) { return v_str(str_of(a[0])); }
inline vptr fn_sym(vlist& a, Interp& i) { return v_sym(i.str(a[0])); }
// (num s) the number written in a string (or a number unchanged)
inline vptr fn_num(vlist& a, Interp& i) {
    if (a[0]->t==Value::NUM) return a[0];
    const std::string& s=i.str(a[0]);
    char* e=nullptr; double d=std::strtod(s.c_str(), &e);
    if (!e || *e!='\0' || e==s.c_str()) i.err("num: not a number: " + s);
    return v_num(d);
}
// copy is shallow
inline vptr fn_copy(vlist& a, Interp& i) {
    auto& s=a[0]; auto v=v_nil();
    v->t=s->t; v->num=s->num; v->s=s->s; v->l=s->l;
    v->op=s->op; v->params=s->params; v->closure=s->closure; v->wclosure=s->wclosure;
    if (s->t != Value::LIST) v->body=s->body;          // a list's body is a cache of the form, not data
    v->opaque=s->opaque; v->opaque_tag=s->opaque_tag;
    return v;
}
// (live-objects) => (values environments): how many are alive in the whole process (tests use it to
//   check that what a program makes is freed)
inline vptr fn_live_objects(vlist&, Interp&) { return v_list({ v_num((double)live_values().load()), v_num((double)live_envs().load()) }); }
inline vptr fn_error(vlist& a, Interp& i) { std::string m; for (auto& x : a) m+=str_of(x); i.err(m.empty() ? "error" : m); }
inline vptr fn_clock(vlist&, Interp&) { return v_num(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
inline vptr fn_load(vlist& a, Interp& i) { i.load(i.str(a[0])); return v_nil(); }
inline vptr fn_parse(vlist& a, Interp& i) { Parser p(i.str(a[0]), std::make_shared<std::string>("<parse>")); p.literal = false; return p.program(); }
// help is a special form (its argument is not evaluated): (help range) works, and so does (help "range").
inline void print_help(Interp& i, const std::string& name) {
    std::string path = i.find_file("help.txt");
    if (path.empty()) i.err("help: help.txt not found (run tools/gendoc.py, or install)");
    std::ifstream f(path); std::string line; bool found = false, in = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] == '#') {            // "#name name ..." starts an entry
            in = false; std::istringstream ss(line.substr(1)); std::string n;
            while (ss >> n) if (n == name) in = true;
            if (in) found = true;
        } else if (in) *i.out << line << "\n";
    }
    if (!found) *i.out << "no help for " << name << "\n";
    *i.out << std::flush;
}
inline vptr fn_definedp(vlist& a, Interp& i) { const std::string& n = a[0]->t==Value::SYM ? a[0]->s : i.str(a[0]); return v_bool(i.global->find(n) != nullptr); }
inline vptr fn_vars(vlist&, Interp& i) { vlist out; for (auto& n : i.names()) out.push_back(v_sym(n)); return v_list(std::move(out)); }

// constructor/destructor, methods
inline Interp::Interp() {
    rng.seed((uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    library = make_env(); library->global = true;
    global = make_env(); global->global = true;
    library->fallback = global.get();
    library->mirror = global.get(); global->mirror = library.get(); library->library_side = true;
    library->shared = global->shared = &shared_names;
#ifdef _WIN32
    const char sep = ';'; const char* home = std::getenv("USERPROFILE");
#else
    const char sep = ':'; const char* home = std::getenv("HOME");
#endif
    if (const char* mp = std::getenv("MUSIL_PATH")) {
        std::string s = mp; size_t st = 0, p;
        while ((p = s.find(sep, st)) != std::string::npos) { if (p > st) load_path.push_back(s.substr(st, p-st)); st = p+1; }
        if (st < s.size()) load_path.push_back(s.substr(st));
    }
    if (home) home_path = (fs::path(home) / ".musil").string();
    auto a = [&](const char* n, op_t f, int lo = 0, int hi = 0) { def(n, f, lo, hi); };
    const int N = -1;   // unbounded
    // Constants: nil, true (1), false (0), pi, inf, version
    def("nil", v_nil()); def("true", v_num(1)); def("false", v_num(0));
    def("pi", v_num(3.14159265358979323846)); def("inf", v_num(INFINITY));
    def("version", v_str(MUSIL_VERSION));
    // Special forms (arity checked in eval)
    a("quote", fn_quote); a("do", fn_do); a("if", fn_if); a("while", fn_while); a("for", fn_for); a("var", fn_var); a("set", fn_set);
    a("function", fn_function); a("return", fn_return); a("break", fn_break); a("continue", fn_continue);
    a("try", fn_try); a("expr", fn_expr); a("eval", fn_eval); a("apply", fn_apply);
    // Arithmetic                                   name        fn        min max
    a("+", fn_add, 0, N); a("-", fn_sub, 0, N); a("*", fn_mul, 0, N); a("/", fn_div, 0, N);
    a("<", fn_lt, 2, 2); a(">", fn_gt, 2, 2); a("<=", fn_le, 2, 2); a(">=", fn_ge, 2, 2);
    a("==", fn_eq, 2, 2); a("!=", fn_ne, 2, 2); a("equal?", fn_equalp, 2, 2); a("same?", fn_samep, 2, 2);
    for (auto& u : { std::pair<const char*, op_t>{"sin", fn_sin}, {"cos", fn_cos}, {"tan", fn_tan}, {"asin", fn_asin}, {"acos", fn_acos}, {"atan", fn_atan},
                     {"sqrt", fn_sqrt}, {"exp", fn_exp}, {"log", fn_log}, {"log2", fn_log2}, {"log10", fn_log10}, {"tanh", fn_tanh},
                     {"abs", fn_abs}, {"floor", fn_flr}, {"ceil", fn_cei}, {"round", fn_rnd}, {"not", fn_not} })
        a(u.first, u.second, 1, 1);
    a("atan2", fn_atan2, 2, 2); a("pow", fn_pow, 2, 2); a("mod", fn_mod, 2, 2);
    a("and", fn_and, 0, N); a("or", fn_or, 0, N); a("min", fn_min, 1, N); a("max", fn_max, 1, N);
    // Lists and vectors
    a("list", fn_list, 0, N); a("append", fn_append, 1, N); a("push", fn_push, 2, 2); a("pop", fn_pop, 1, 1);
    a("length", fn_length, 1, 1); a("head", fn_head, 1, 1); a("tail", fn_tail, 1, 1); a("empty?", fn_emptyp, 1, 1);
    a("getidx", fn_getidx, 2, 2); a("setidx", fn_setidx, 3, 3); a("vec", fn_vec, 0, N); a("sum", fn_sum, 1, 1);
    // Meta and I/O
    a("print", fn_print, 0, N); a("type", fn_type, 1, 1); a("str", fn_str, 1, 1); a("sym", fn_sym, 1, 1); a("num", fn_num, 1, 1);
    a("copy", fn_copy, 1, 1); a("live-objects", fn_live_objects, 0, 0); a("error", fn_error, 0, N); a("clock", fn_clock, 0, 0); a("load", fn_load, 1, 1);
    a("defined?", fn_definedp, 1, 1); a("vars", fn_vars, 0, 0); a("exit", fn_exit, 0, 1);
    a("help", fn_help, 1, 1); a("parse", fn_parse, 1, 1);
    quote_op = library->vars["quote"];
}
inline Interp::~Interp() {
    for (auto& w : tracked_envs) if (auto e = w.lock()) { e->vars.clear(); e->parent.reset(); }
    if (global) global->vars.clear();
    if (library) { library->vars.clear(); library->fallback = library->mirror = nullptr; }
    if (global) global->mirror = nullptr;
    flow_value.reset(); quote_op.reset();
}
inline std::vector<std::string> Interp::names() {
    std::vector<std::string> n;
    for (auto& kv : global->vars) n.push_back(kv.first);
    for (auto& kv : library->vars) if (!global->vars.count(kv.first)) n.push_back(kv.first);
    std::sort(n.begin(), n.end());
    return n;
}
// The cycle collector. Reference counting frees everything except a cycle: a closure stored in a structure
// of its own scope (the object pattern: (put! self 'get (function () self))), an inner closure assigned to a
// variable of an outer scope. Trial deletion finds them without knowing the roots: every environment and
// every list or function reachable from an environment gets its reference count; the references among them
// are subtracted; what is left is held from outside (a variable of the host, a C++ frame of the evaluator,
// the interpreter's globals) and is alive, with all it reaches. The rest is garbage: its environments and
// lists are emptied, which breaks the cycles, and reference counting frees them. Safe at any moment of an
// evaluation, since everything the evaluator holds it holds by shared pointer.
inline size_t Interp::collect() {
    struct node { long refs; bool env; bool live; };
    std::unordered_map<const void*, node> nodes;
    std::vector<eptr> envs; envs.reserve(tracked_envs.size());
    for (auto& w : tracked_envs) if (auto p = w.lock()) envs.push_back(std::move(p));
    for (auto& p : envs) nodes.emplace(p.get(), node{ (long)p.use_count() - 1, true, false });
    std::vector<const Value*> vals, todo;
    auto container = [](const vptr& v) { return v && ((v->t == Value::LIST && !v->l.empty()) || (v->t == Value::FN && v->closure)); };
    auto visit = [&](const vptr& v) {
        if (!container(v)) return;
        if (nodes.emplace(v.get(), node{ (long)v.use_count(), false, false }).second) { vals.push_back(v.get()); todo.push_back(v.get()); }
    };
    for (auto& p : envs) for (auto& kv : p->vars) visit(kv.second);
    while (!todo.empty()) { const Value* v = todo.back(); todo.pop_back(); if (v->t == Value::LIST) for (auto& c : v->l) visit(c); }
    auto internal = [&](const void* k) { auto it = nodes.find(k); if (it != nodes.end()) it->second.refs--; };
    for (auto& p : envs) { if (p->parent) internal(p->parent.get()); for (auto& kv : p->vars) if (container(kv.second)) internal(kv.second.get()); }
    for (const Value* v : vals) {
        if (v->t == Value::LIST) { for (auto& c : v->l) if (container(c)) internal(c.get()); }
        else if (v->closure) internal(v->closure.get());
    }
    // what is held from outside is alive, and so is everything it reaches
    std::vector<const void*> live;
    for (auto& kv : nodes) if (kv.second.refs > 0) { kv.second.live = true; live.push_back(kv.first); }
    auto reach = [&](const void* k) { auto it = nodes.find(k); if (it != nodes.end() && !it->second.live) { it->second.live = true; live.push_back(k); } };
    while (!live.empty()) {
        const void* k = live.back(); live.pop_back();
        if (nodes[k].env) {
            const Env* p = static_cast<const Env*>(k);
            if (p->parent) reach(p->parent.get());
            for (auto& kv : p->vars) if (container(kv.second)) reach(kv.second.get());
        } else {
            const Value* v = static_cast<const Value*>(k);
            if (v->t == Value::LIST) { for (auto& c : v->l) if (container(c)) reach(c.get()); }
            else if (v->closure) reach(v->closure.get());
        }
    }
    // the rest only holds itself: empty it (what it held is freed when the graveyard goes)
    std::vector<std::unordered_map<std::string, vptr>> graveyard; std::vector<eptr> parents; std::vector<vlist> lists; std::vector<eptr> closures;
    size_t freed = 0;
    for (auto& p : envs) if (!nodes[p.get()].live) { graveyard.emplace_back(); graveyard.back().swap(p->vars); parents.push_back(std::move(p->parent)); ++freed; }
    for (const Value* v : vals) if (!nodes[v].live) {
        Value* m = const_cast<Value*>(v);
        if (m->t == Value::LIST) { lists.emplace_back(); lists.back().swap(m->l); } else closures.push_back(std::move(m->closure));
    }
    nodes.clear();
    graveyard.clear(); parents.clear(); lists.clear(); closures.clear();
    envs.clear();
    return freed;
}
// load: search order for a relative path:
//   1. the directory of the file doing the loading
//   2. each entry of load_path (MUSIL_PATH, plus whatever the host adds), then ~/.musil: the libraries
//   3. the current directory
// A file found on the load path (or next to a library that loads it) is a library: it runs in the library
// environment, so a program's own names never change what the library's functions mean.
inline std::string Interp::find_file(const std::string& path, bool* in_library) {
    fs::path p(path), resolved; bool lib = false;
    auto try_path = [&](const fs::path& q, bool is_lib) { std::error_code ec; if (resolved.empty() && !q.empty() && fs::is_regular_file(q, ec)) { resolved = q; lib = is_lib; } };
    if (p.is_absolute()) try_path(p, false);
    else {
        if (current_file) {
            fs::path base = fs::path(*current_file).parent_path();
            bool from_lib = library_files.count(*current_file) > 0;
            std::error_code fe;                        // a file run by its bare name: its folder is "." (not typed input)
            if (base.empty() && fs::is_regular_file(*current_file, fe)) base = ".";
            if (!base.empty()) try_path(base / p, from_lib);
        }
        for (auto& d : load_path) try_path(fs::path(d) / p, true);
        if (!home_path.empty()) try_path(fs::path(home_path) / p, true);
        try_path(p, false);
    }
    if (in_library) *in_library = lib;
    if (resolved.empty()) return "";
    std::error_code ec;
    fs::path canon_p = fs::weakly_canonical(resolved, ec);
    return (ec ? fs::absolute(resolved) : canon_p).generic_string();
}
inline void Interp::load(const std::string& path) {
    bool lib = false;
    std::string canon = find_file(path, &lib);
    if (canon.empty()) err("load: not found: " + path);
    std::error_code sec; auto stamp = std::make_pair(fs::last_write_time(canon, sec), fs::file_size(canon, sec));
    if (loading_files.count(canon)) return;
    if (loaded_files.count(canon)) { auto it = loaded_stamps.find(canon); if (it == loaded_stamps.end() || it->second == stamp) return; }
    if (!lib) { who_scope ws(*this, "load"); require_read(canon); }
    std::ifstream f(canon); if (!f) err("load: cannot open " + canon);
    std::stringstream ss; ss << f.rdbuf();
    auto sf = current_file; int sl = current_line;
    loading_files.insert(canon);
    if (lib) library_files.insert(canon);
    struct guard { Interp& i; std::string c; sptr f; int l; bool ok = false;
        std::pair<fs::file_time_type, std::uintmax_t> st;
        ~guard() { i.loading_files.erase(c); if (ok) { i.loaded_files.insert(c); i.loaded_stamps[c] = st; } i.current_file = f; i.current_line = l; } } g{*this, canon, sf, sl, false, stamp};
    run_in(ss.str(), canon, lib ? library : global);
    g.ok = true;   // a module that fails is not cached: the next load runs it again and reports the error again
}
inline void Interp::reload(const std::string& path) {
    std::string canon = find_file(path);
    if (canon.empty()) err("reload: not found: " + path);
    loaded_files.erase(canon);
    load(path);
}
inline vptr Interp::run(const std::string& src, const std::string& filename) { return run_in(src, filename, global); }
inline vptr Interp::run_in(const std::string& src, const std::string& filename, eptr env) {
    auto file = std::make_shared<std::string>(filename);
    Parser p(src, file);
    auto prog = p.program();
    current_file = file;
    if (!main_file && !filename.empty() && filename[0] != '<' && loading_files.empty()) main_file = file;
    vptr r = eval(prog, std::move(env));
    if (stack_depth == 0) { flow = FLOW_NONE; flow_value.reset(); if (tracked_envs.size() >= compact_at / 2) maintain(); }
    return r;
}
inline Interp*& repl_interp() { static thread_local Interp* p = nullptr; return p; }
inline void Interp::repl() {
    std::string buffer; int depth = 0;
    repl_interp() = this;
#ifdef HAVE_READLINE
    // background work (live loops, servers) while readline waits for a key: a getc that polls stdin in
    // slices and idles in between. rl_getc_function exists in GNU readline and in libedit (macOS);
    // rl_event_hook, the usual way, does not exist in libedit.
    rl_getc_function = [](FILE* f) -> int {
        int fd = f ? fileno(f) : 0;                       // libedit (macOS) calls this with a null FILE*: read stdin then
        while (true) {
            fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds); timeval tv{ 0, 20000 };
            int r = select(fd + 1, &fds, nullptr, nullptr, &tv);
            if (r > 0) break;
            if (repl_interp()) { try { repl_interp()->idle(); } catch (...) {} repl_interp()->clear_stop(); }
        }
        unsigned char c; long n = read(fd, &c, 1);
        return n == 1 ? (int)c : EOF;
    };
#endif
    while (true) {
        const char* prompt = buffer.empty() ? "> " : "  ";
        std::string line;
#ifdef HAVE_READLINE
        char* rp = readline(prompt);
        if (!rp) break;
        if (*rp) add_history(rp);
        line = rp; free(rp);
#else
        *out << prompt << std::flush;
#ifndef _WIN32
        // without readline: wait for input in slices, so background work (live loops, servers) runs meanwhile
        while (true) {
            fd_set fds; FD_ZERO(&fds); FD_SET(0, &fds); timeval tv{ 0, 20000 };
            int r = select(1, &fds, nullptr, nullptr, &tv);
            if (r > 0) break;
            try { idle(); } catch (...) {}
            clear_stop();
        }
#endif
        if (!std::getline(std::cin, line)) break;
#endif
        if (line == ":q" && buffer.empty()) break;
        bool in_str = false;
        for (size_t i=0; i<line.size(); i++) {
            char c = line[i];
            if (in_str) { if (c == '\\' && i+1 < line.size()) { i++; continue; } if (c == '"') in_str = false; continue; }
            if (c == '"') in_str = true;
            else if (c == ';') break;
            else if (c == '(' || c == '{') depth++;
            else if (c == ')' || c == '}') depth--;
        }
        buffer += line; buffer += '\n';
        if (depth <= 0) {
            try { auto r = run(buffer, "<repl>"); if (r && r->t != Value::NIL) *out << str_of(r) << "\n"; }
            catch (Exit_signal&) { break; }
            catch (Stop_signal&) { *out << "stopped\n"; }
            catch (const std::exception& ex) { *out << "error: " << ex.what() << "\n"; }
            clear_stop();
            call_stack.clear(); function_depth = 0; loop_depth = 0; stack_depth = 0; flow = FLOW_NONE; flow_value.reset();
            buffer.clear(); depth = 0;
        }
    }
}

} // namespace musil

// eof

