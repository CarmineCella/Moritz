// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// session.cpp — contexts of the Moritz language inside the application (session.h)

#include "session.h"

#include "gui.h"
#include "lang/lang.h"

#include <FL/Fl.H>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#ifndef _WIN32
#include <pthread.h>
#else
#include <thread>
#endif

namespace moritz {

namespace {

// runs f on the main thread, from any thread
void onMain(std::function<void()> f) {
    auto* p = new std::function<void()>(std::move(f));
    Fl::awake([](void* d) {
        auto* g = static_cast<std::function<void()>*>(d);
        (*g)();
        delete g;
    }, p);
}

// a detached thread with a large stack: the language's recursion is measured against it
void startThread(std::function<void()> f) {
#ifdef _WIN32
    std::thread(std::move(f)).detach();          // the executable's stack size (8 MB)
#else
    auto* fp = new std::function<void()>(std::move(f));
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &a, [](void* p) -> void* {
            auto* g = static_cast<std::function<void()>*>(p);
            (*g)();
            delete g;
            return nullptr;
        }, fp) != 0) {
        (*fp)();
        delete fp;
    }
    pthread_attr_destroy(&a);
#endif
}

// the printed output of a context: whole lines go to the Console, on the main thread
class ConsoleStream : public std::streambuf {
public:
    void finish() {
        if (!line_.empty()) send();
    }

protected:
    int overflow(int c) override {
        if (c == '\n') send();
        else if (c != EOF) line_ += (char)c;
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize k = 0; k < n; ++k) overflow((unsigned char)s[k]);
        return n;
    }

private:
    std::string line_;
    void send() {
        std::string l = std::move(line_);
        line_.clear();
        onMain([l] { App::get().log(l); });
    }
};

// a value printed, cut at max characters without printing all of it first (a database is large)
void preview(const lang::vptr& v, std::string& out, size_t max, int depth = 0) {
    if (out.size() >= max) return;
    if (!v || v->t == lang::Value::NIL) { out += "nil"; return; }
    if (v->t == lang::Value::LIST) {
        if (depth > 20) { out += "(...)"; return; }
        out += "(";
        for (size_t k = 0; k < v->l.size(); ++k) {
            if (out.size() >= max) { out += " ..."; break; }
            if (k) out += " ";
            preview(v->l[k], out, max, depth + 1);
        }
        out += ")";
        return;
    }
    if (v->t == lang::Value::NUM && v->num.size() > 16) {
        lang::vptr head = lang::v_arr(std::valarray<double>(v->num[std::slice(0, 8, 1)]));
        out += lang::str_of(head);
        out += " ... (" + std::to_string(v->num.size()) + " values)";
        return;
    }
    if (v->t == lang::Value::STR) { out += "\"" + v->s.substr(0, max) + "\""; return; }
    out += lang::str_of(v);
}

std::string preview(const lang::vptr& v, size_t max) {
    std::string out;
    preview(v, out, max);
    if (out.size() > max + 20) out = out.substr(0, max) + " ...";
    return out;
}

std::string languageDir() { return resourceDir("language"); }

// the names the libraries define at their top level: their variables (var) and, with functions = true,
// their functions; neither is shown among the user's variables
const std::set<std::string>& languageLibraryNames(bool functions) {
    static std::set<std::string> vars, fns;
    static bool read = [] {
        auto& s = vars;
        std::string dir = languageDir();
        for (const char* lib : {"std", "system", "scientific", "signals", "music", "plot"}) {
            std::ifstream f(dir + "/" + lib + ".moritz");
            std::string line;
            while (std::getline(f, line))
                for (const char* kw : {"var ", "function "}) {
                    size_t n = std::strlen(kw);
                    if (line.compare(0, n, kw) != 0) continue;
                    size_t e = line.find_first_of(" (\t", n);
                    (kw[0] == 'v' ? s : fns).insert(line.substr(n, e == std::string::npos ? std::string::npos : e - n));
                }
        }
        return true;
    }();
    (void)read;
    return functions ? fns : vars;
}

}  // namespace

LangContext::LangContext(std::string l) : label(std::move(l)), interp_(std::make_unique<lang::Interp>()) {
    auto& I = *interp_;
    lang::make_env(I);
    if (std::string d = languageDir(); !d.empty()) I.load_path.push_back(d);
    auto* buf = new ConsoleStream();
    I.out = new std::ostream(buf);
    I.yield_fn = [this] {
        if (stop_) {
            stop_ = false;
            throw lang::Error("", 0, "stopped");
        }
    };
    for (auto& kv : I.global->vars) baseline_.push_back(kv.first);
    try { I.load("std"); } catch (const std::exception& e) { App::get().log(std::string("language: ") + e.what()); }
    snapshot();
}

LangContext::~LangContext() {
    if (interp_) {
        auto* os = interp_->out;
        auto* buf = os ? os->rdbuf() : nullptr;
        interp_.reset();
        delete os;
        delete buf;
    }
}

bool LangContext::eval(const std::string& code, const std::string& file, int firstLine,
                       std::function<void(const EvalResult&)> done) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return false;
    stop_ = false;
    // blank lines in front keep the line numbers of the file the code comes from
    std::string src = std::string((size_t)std::max(0, firstLine - 1), '\n') + code;
    auto self = shared_from_this();
    startThread([self, src, file, done] {
        auto& I = *self->interp_;
        EvalResult r;
        try {
            lang::vptr v = I.run(src, file);
            if (v && v->t != lang::Value::NIL) r.value = preview(v, 400);
        } catch (lang::Exit_signal&) {
        } catch (const lang::Error& e) {
            r.ok = false;
            r.error = e.what();
            r.file = e.file;
            r.line = e.line;
            r.stopped = e.msg == "stopped";
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = e.what();
        }
        I.call_stack.clear();
        I.function_depth = 0;
        I.loop_depth = 0;
        I.stack_depth = 0;
        static_cast<ConsoleStream*>(I.out->rdbuf())->finish();
        self->snapshot();
        self->busy_ = false;
        onMain([self, r, done] { if (done) done(r); });
    });
    return true;
}

void LangContext::stop() {
    if (busy_) stop_ = true;
}

void LangContext::snapshot() {
    std::vector<Variable> vars;
    std::vector<std::string> names;
    std::set<std::string> base(baseline_.begin(), baseline_.end());
    std::string lib = languageDir();
    for (auto& kv : interp_->global->vars) {
        names.push_back(kv.first);
        const auto& v0 = kv.second;
        bool fn = v0 && v0->t == lang::Value::FN;
        if (base.count(kv.first) || languageLibraryNames(false).count(kv.first)) continue;
        if (fn && (languageLibraryNames(true).count(kv.first) || languageHelp(kv.first))) continue;   // still the library's
        const auto& v = kv.second;
        if (v && v->t == lang::Value::FN && v->body && v->body->file && !lib.empty() &&
            v->body->file->rfind(lib, 0) == 0)
            continue;                                   // a function of the libraries
        Variable var;
        var.name = kv.first;
        var.kind = lang::type_name(v);
        var.preview = v && v->t == lang::Value::FN ? "(" + [&] {
            std::string p;
            for (auto& s : v->params) p += (p.empty() ? "" : " ") + s;
            return p;
        }() + ")" : preview(v, 60);
        vars.push_back(var);
    }
    std::sort(vars.begin(), vars.end(), [](const Variable& a, const Variable& b) { return a.name < b.name; });
    std::sort(names.begin(), names.end());
    std::lock_guard<std::mutex> g(mu_);
    vars_ = std::move(vars);
    names_ = std::move(names);
}

std::vector<Variable> LangContext::variables() {
    std::lock_guard<std::mutex> g(mu_);
    return vars_;
}

std::vector<std::string> LangContext::names() {
    std::lock_guard<std::mutex> g(mu_);
    return names_;
}

// ---- the contexts

namespace {
std::map<const void*, std::shared_ptr<LangContext>>& contexts() {
    static std::map<const void*, std::shared_ptr<LangContext>> m;
    return m;
}
}  // namespace

std::shared_ptr<LangContext> sessionContext() {
    auto& c = contexts()[nullptr];
    if (!c) c = std::make_shared<LangContext>("session");
    return c;
}

std::shared_ptr<LangContext> patchContext(const void* doc, const std::string& title) {
    auto& c = contexts()[doc];
    if (!c) c = std::make_shared<LangContext>(title);
    c->label = title;
    return c;
}

void dropPatchContext(const void* doc) {
    auto it = contexts().find(doc);
    if (it == contexts().end()) return;
    it->second->stop();                 // a running piece keeps the context alive until it ends
    contexts().erase(it);
}

// ---- the documentation

namespace {
struct HelpIndex {
    std::vector<HelpEntry> entries;
    std::map<std::string, size_t> byName;
    std::vector<std::string> names;
};
const HelpIndex& helpIndex() {
    static HelpIndex h = [] {
        HelpIndex x;
        std::ifstream f(languageDir() + "/help.txt");
        std::string line;
        HelpEntry* e = nullptr;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("# ", 0) == 0 && x.entries.empty() && !e) continue;   // the header
            if (!line.empty() && line[0] == '#') {
                x.entries.emplace_back();
                e = &x.entries.back();
                std::istringstream ss(line.substr(1));
                std::string n;
                while (ss >> n) e->names.push_back(n);
                continue;
            }
            if (!e) continue;
            std::string t = line.size() > 2 ? line.substr(2) : "";
            if (e->signature.empty()) e->signature = t;
            else e->text.push_back(t);
        }
        for (size_t k = 0; k < x.entries.size(); ++k)
            for (auto& n : x.entries[k].names)
                if (!x.byName.count(n)) { x.byName[n] = k; x.names.push_back(n); }
        std::sort(x.names.begin(), x.names.end());
        return x;
    }();
    return h;
}
}  // namespace

const HelpEntry* languageHelp(const std::string& name) {
    auto& h = helpIndex();
    auto it = h.byName.find(name);
    return it == h.byName.end() ? nullptr : &h.entries[it->second];
}

const std::vector<std::string>& languageNames() { return helpIndex().names; }

}  // namespace moritz
