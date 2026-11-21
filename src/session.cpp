// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// session.cpp — where Musil runs inside the application (session.h)

#include "session.h"

#include "musil_cli.h"
#include "musil_host.h"
#include "gui.h"
#include "objects.h"
#include "transport.h"

#include <FL/Fl.H>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "jobs.h"

#include <thread>
#ifndef _WIN32
#include <pthread.h>
#endif

#ifdef OPAQUE      // Windows (wingdi.h) defines it; Musil has a value type of that name
#undef OPAQUE
#endif

namespace moritz {

namespace {

// the thread of a synchronous run (moritz --render): the main thread waits for it, so what it would send
// to the main thread runs at once
thread_local bool inlineMain = false;

// runs f on the main thread, from any thread
void onMain(std::function<void()> f) {
    if (inlineMain) { f(); return; }
    auto* p = new std::function<void()>(std::move(f));
    Fl::awake([](void* d) {
        auto* g = static_cast<std::function<void()>*>(d);
        (*g)();
        delete g;
    }, p);
}

// a detached thread with a large stack: Musil's recursion is measured against it
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

// the printed output of a context: whole lines go to the Console (or where the context's hooks say), on
// the main thread. The sink is read and changed on the main thread only (the interpreter thread copies the
// pointer, set once): a line queued before the context's box was deleted finds it emptied by detach() and
// goes to the Console instead of to the freed box.
class ConsoleStream : public std::streambuf {
public:
    using Sink = std::function<void(const std::string&)>;
    const std::shared_ptr<Sink> sink = std::make_shared<Sink>();     // main thread; empty: the Console
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
        std::shared_ptr<Sink> s = sink;
        onMain([l, s] { if (*s) (*s)(l); else App::get().log(l); });
    }
};

// a value printed, cut at max characters without printing all of it first (a database is large)
void preview(const musil::vptr& v, std::string& out, size_t max, int depth = 0) {
    if (out.size() >= max) return;
    if (!v || v->t == musil::Value::NIL) { out += "nil"; return; }
    if (v->t == musil::Value::LIST) {
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
    if (v->t == musil::Value::NUM && v->num.size() > 16) {
        musil::vptr head = musil::v_arr(std::valarray<double>(v->num[std::slice(0, 8, 1)]));
        out += musil::str_of(head);
        out += " ... (" + std::to_string(v->num.size()) + " values)";
        return;
    }
    if (v->t == musil::Value::STR) { out += "\"" + v->s.substr(0, max) + "\""; return; }
    out += musil::str_of(v);
}

std::string preview(const musil::vptr& v, size_t max) {
    std::string out;
    preview(v, out, max);
    if (out.size() > max + 20) out = out.substr(0, max) + " ...";
    return out;
}


// ---- Moritz, from Musil: code on its thread reaches the patches through the main thread

std::thread::id& mainThread() {
    static std::thread::id id = std::this_thread::get_id();   // first used on the main thread (a context is made there)
    return id;
}

// runs f on the main thread and waits for it (at once when already there)
void onMainWait(const std::function<void()>& f) {
    if (inlineMain || std::this_thread::get_id() == mainThread()) { f(); return; }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr ex;
    onMain([&] {
        try { f(); } catch (...) { ex = std::current_exception(); }
        std::lock_guard<std::mutex> g(m);
        done = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> g(m);
    cv.wait(g, [&] { return done; });
    if (ex) std::rethrow_exception(ex);
}

// Musil values as a message: numbers, strings and symbols, lists spliced; a score as `score <token>` (a copy without
// what only Musil holds, made here on the code's thread, that a score object takes)
void toMessage(const musil::vptr& v, Message& m) {
    if (!v || v->t == musil::Value::NIL) return;
    if (musil::score_musil::isScore(v)) {
        m.push_back(Atom("score"));
        m.push_back(Atom(shareScore(musil::score_musil::detached(*std::static_pointer_cast<musil::score::Score>(v->opaque)))));
        return;
    }
    if (v->t == musil::Value::NUM) {
        for (double d : v->num) m.push_back(Atom(d));
    } else if (v->t == musil::Value::LIST) {
        for (auto& x : v->l) toMessage(x, m);
    } else {
        m.push_back(Atom(v->s));
    }
}

// the context of an interpreter, for the functions that reach Moritz
// (made once and never destroyed, as the help's index: a script's thread may still use them while the program exits)
std::mutex& contextsMutex() { static std::mutex& m = *new std::mutex; return m; }
std::map<musil::Interp*, LangContext*>& contexts() { static auto& m = *new std::map<musil::Interp*, LangContext*>; return m; }
LangContext* contextOf(musil::Interp& I) {
    std::lock_guard<std::mutex> g(contextsMutex());
    auto it = contexts().find(&I);
    return it == contexts().end() ? nullptr : it->second;
}
// where a context's buffers and receives are looked up (main thread): its box's patch, or every open one
Patch* patchOf(musil::Interp& I) {
    LangContext* c = contextOf(I);
    if (c && c->hooks.patch) return c->hooks.patch;
    if (c && c->hooks.restricted) return nullptr;        // a box that is gone
    return &App::get().host;
}

// (send name values...) to every receive name (as a send box, or a ; message)
musil::vptr fnSend(musil::vlist& a, musil::Interp& I) {
    std::string name = I.str(a[0]);
    Message m;
    for (size_t k = 1; k < a.size(); ++k) toMessage(a[k], m);
    if (m.empty()) m.push_back(Atom("bang"));
    onMainWait([&] { if (Patch* p = patchOf(I)) p->sendTo(name, m); });
    return musil::v_nil();
}

// (out k values...) out of outlet k of the code box (from 1); a list is sent as one message, a score (score-make,
//   score-read...) as score <token>, which a score object takes
musil::vptr fnOut(musil::vlist& a, musil::Interp& I) {
    int k = (int)I.scalar(a[0]);
    Message m;
    for (size_t j = 1; j < a.size(); ++j) toMessage(a[j], m);
    if (m.empty()) m.push_back(Atom("bang"));
    bool ok = true;
    onMainWait([&] {
        LangContext* c = contextOf(I);
        if (c && c->hooks.outlet) c->hooks.outlet(k, m); else ok = false;
    });
    if (!ok) I.bad("not in a code box");
    return musil::v_nil();
}


// (buffer name) the channels of a buffer~ as vectors; (buffer-rate name) its sample rate
musil::vptr fnBuffer(musil::vlist& a, musil::Interp& I) {
    std::string name = I.str(a[0]);
    SampleBuffer copy;
    bool found = false;
    onMainWait([&] {
        Patch* p = patchOf(I);
        if (auto* b = p ? findBuffer(p, name) : nullptr) {
            std::lock_guard<std::mutex> lk(b->patch->mutex());
            copy = b->buf;
            found = true;
        }
    });
    if (!found) I.bad("no buffer~ " + name);
    musil::vlist chans;
    for (int c = 0; c < copy.channels; ++c) {
        std::valarray<double> v((size_t)copy.frames);
        for (long f = 0; f < copy.frames; ++f) v[(size_t)f] = copy.data[(size_t)f * copy.channels + c];
        chans.push_back(musil::v_arr(std::move(v)));
    }
    return musil::v_list(std::move(chans));
}

musil::vptr fnBufferRate(musil::vlist& a, musil::Interp& I) {
    std::string name = I.str(a[0]);
    double sr = 0;
    onMainWait([&] { Patch* p = patchOf(I); if (auto* b = p ? findBuffer(p, name) : nullptr) sr = b->buf.sr; });
    if (sr <= 0) I.bad("no buffer~ " + name);
    return musil::v_num(sr);
}

// (set-buffer! name channels [rate]) fills a buffer~: a vector, or a list of channels
musil::vptr fnSetBuffer(musil::vlist& a, musil::Interp& I) {
    std::string name = I.str(a[0]);
    std::vector<std::valarray<double>> ch;
    if (a[1]->t == musil::Value::NUM) ch.push_back(a[1]->num);
    else for (auto& c : I.list(a[1])) ch.push_back(I.num(c));
    if (ch.empty() || ch.size() > 8) I.bad("1 to 8 channels");
    size_t frames = 0;
    for (auto& c : ch) frames = std::max(frames, c.size());
    double sr = a.size() > 2 ? I.scalar(a[2]) : 0;
    // the samples are interleaved here, on the interpreter's thread; under the audio lock they are only
    // swapped in, and the old ones are freed after it is released
    std::vector<float> data(frames * ch.size(), 0.f);
    for (size_t c = 0; c < ch.size(); ++c)
        for (size_t f = 0; f < ch[c].size(); ++f) data[f * ch.size() + c] = (float)ch[c][f];
    bool found = false;
    onMainWait([&] {
        Patch* p = patchOf(I);
        auto* b = p ? findBuffer(p, name) : nullptr;
        if (!b) return;
        found = true;
        {
            std::lock_guard<std::mutex> lk(b->patch->mutex());
            b->buf.data.swap(data);
            b->buf.channels = (int)ch.size();
            b->buf.frames = (long)frames;
            if (sr > 0) b->buf.sr = sr;
            ++b->version;
        }
        b->patch->rebuild();
    });
    if (!found) I.bad("no buffer~ " + name);
    return a[1];
}

// the names the libraries define at their top level: their variables (var) and, with functions = true,
// their functions; neither is shown among the user's variables
const std::set<std::string>& languageLibraryNames(bool functions) {
    static std::set<std::string> vars, fns;
    static bool read = [] {
        auto& s = vars;
        std::string dir = musilLibraryDir();
        for (const char* lib : {"std", "system", "scientific", "signals", "music", "live", "plot"}) {
            std::ifstream f(dir + "/" + lib + ".mu");
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

// What a code box may do: Musil's sandboxed profile, checked inside every builtin that touches the system.
// It reads anywhere; it writes only in its patch's folder and in ~/Documents/Moritz; it does not run
// programs, use the network, wait or quit. A box whose patch the user trusted (code @trusted) may do all.
musil::Capabilities codeBoxCapabilities(const Patch* p) { return musil::Capabilities::sandboxed(patchWriteRoots(p)); }

std::vector<std::string> patchWriteRoots(const Patch* p) {
    std::vector<std::string> roots;
    if (p) {
        const Patch* q = p;
        while (q && q->dir.empty() && q->parent) q = q->parent;   // a subpatch: its file's folder
        if (q && !q->dir.empty()) roots.push_back(q->dir);
    }
    if (std::string home = homeDir(); !home.empty()) roots.push_back(home + "/Documents/Moritz");
    return roots;
}

bool patchMayWrite(const Patch* p, const std::string& path, std::string& why) {
    std::vector<std::string> roots = patchWriteRoots(p);
    std::filesystem::path r = musil::resolved_path(path);
    if (path.empty() || !musil::path_under(r, roots)) {
        why = "not written: " + path + " is outside the patch's folder and ~/Documents/Moritz";
        return false;
    }
    if (std::string home = homeDir(); !home.empty() && musil::path_under(r, {home + "/Documents/Moritz"})) {
        std::error_code ec;                                     // a fresh machine: ~/Documents/Moritz made
        std::filesystem::create_directories(r.parent_path(), ec);
    }
    return true;
}

LangContext::LangContext(std::string l, ContextHooks h)
    : label(std::move(l)), hooks(std::move(h)), interp_(std::make_unique<musil::Interp>()) {
    mainThread();
    {
        std::lock_guard<std::mutex> g(contextsMutex());
        contexts()[interp_.get()] = this;
    }
    auto& I = *interp_;
    musilLibraries(I);
    // Moritz, from Musil
    I.def("send", fnSend, 1, -1);
    I.def("buffer", fnBuffer, 1, 1);
    I.def("buffer-rate", fnBufferRate, 1, 1);
    I.def("set-buffer!", fnSetBuffer, 2, 3);
    if (hooks.restricted) {
        I.def("out", fnOut, 1, -1);
        if (!hooks.trusted) I.caps = codeBoxCapabilities(hooks.patch);
        // a relative path is written in the patch's folder (or in ~/Documents/Moritz for an unsaved patch)
        auto roots = codeBoxCapabilities(hooks.patch).write_roots;
        if (!roots.empty()) I.write_dir = roots.front();
    }
    musilPaths(I);
    // the command line's arguments: none here (moritz --run sets them), so that a script written for the
    // command line, which reads args for its options, takes its defaults in the Console, the editor or a code box
    I.def("args", musil::v_list(musil::vlist{}));
#ifndef _WIN32
    I.stack_budget = (64u << 20) / 4 * 3;          // code runs on a thread of 64 MB (startThread)
#endif
    auto* buf = new ConsoleStream();
    *buf->sink = hooks.print;
    I.out = new std::ostream(buf);
    for (auto& kv : I.global->vars) baseline_.push_back(kv.first);
    try {
        I.load("std.mu");
        for (auto& f : hooks.preload) I.load(f);
    } catch (const std::exception& e) {
        if (hooks.print) hooks.print(std::string("musil: ") + e.what());
        else App::get().log(std::string("musil: ") + e.what());
    }
    snapshot();
}

LangContext::~LangContext() {
    {
        std::lock_guard<std::mutex> g(contextsMutex());
        contexts().erase(interp_.get());
    }
    if (interp_) {
        auto* os = interp_->out;
        auto* buf = os ? os->rdbuf() : nullptr;
        interp_.reset();
        delete os;
        delete buf;
    }
}

bool LangContext::eval(const std::string& code, const std::string& file, int firstLine,
                       std::function<void(const EvalResult&)> done, std::function<void(musil::Interp&)> then) {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return false;
    interp_->clear_stop();
    // blank lines in front keep the line numbers of the file the code comes from
    std::string src = std::string((size_t)std::max(0, firstLine - 1), '\n') + code;
    auto self = shared_from_this();
    bool sync = jobsSynchronous;                       // moritz --render: run now, and wait
    auto work = [self, src, file, done, sync, then] {
        if (sync) inlineMain = true;
        auto& I = *self->interp_;
        EvalResult r;
        try {
            musil::vptr v = I.run(src, file);
            if (v && v->t != musil::Value::NIL) r.value = preview(v, 400);
            if (then) then(I);
        } catch (musil::Exit_signal&) {
        } catch (musil::Stop_signal&) {             // Stop: try in the script could not catch it
            r.ok = false;
            r.error = "stopped";
            r.stopped = true;
        } catch (const musil::Error& e) {
            r.ok = false;
            r.error = e.what();
            r.file = e.file;
            r.line = e.line;
            r.stopped = e.msg == "stopped";
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = e.what();
        }
        I.clear_stop();
        I.call_stack.clear();
        I.function_depth = 0;
        I.loop_depth = 0;
        I.stack_depth = 0;
        static_cast<ConsoleStream*>(I.out->rdbuf())->finish();
        self->snapshot();
        self->busy_ = false;
        if (sync) { inlineMain = false; if (done) done(r); return; }
        onMain([self, r, done] { if (done) done(r); });
    };
    if (!sync) { startThread(work); return true; }
    // on a thread with the large stack, the main thread waiting for it
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    startThread([&] {
        work();
        std::lock_guard<std::mutex> g(m);
        finished = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> g(m);
    cv.wait(g, [&] { return finished; });
    return true;
}

void LangContext::define(const std::string& name, const Message& value) {
    if (busy_) return;
    musil::vptr v;
    bool numbers = !value.empty();
    for (auto& a : value) numbers = numbers && a.isFloat();
    if (value.empty()) v = musil::v_nil();
    else if (numbers && value.size() == 1) v = musil::v_num(value[0].f);
    else if (numbers) {
        std::valarray<double> x(value.size());
        for (size_t k = 0; k < value.size(); ++k) x[k] = value[k].f;
        v = musil::v_arr(std::move(x));
    } else if (value.size() == 1) v = musil::v_str(value[0].s);
    else {
        musil::vlist l;
        for (auto& a : value) l.push_back(a.isFloat() ? musil::v_num(a.f) : musil::v_str(a.s));
        v = musil::v_list(std::move(l));
    }
    interp_->def(name, v);
}

void LangContext::detach() {
    stop();
    hooks.patch = nullptr;
    hooks.outlet = nullptr;
    hooks.print = nullptr;
    if (interp_ && interp_->out) *static_cast<ConsoleStream*>(interp_->out->rdbuf())->sink = nullptr;   // main thread
}

std::vector<std::string> codeBoxNames() {
    ContextHooks h;
    h.restricted = true;
    h.preload = {"signals.mu"};
    auto c = std::make_shared<LangContext>("code", h);
    c->snapshot();
    std::vector<std::string> out;
    for (auto& n : c->names()) out.push_back(n);   // what may run is decided inside the builtins
    return out;
}

void LangContext::stop() {
    if (busy_) interp_->request_stop();             // any thread; sticky until the run returns
}

void LangContext::snapshot() {
    std::vector<Variable> vars;
    std::vector<std::string> names;
    std::set<std::string> base(baseline_.begin(), baseline_.end());
    std::string lib = musilLibraryDir();
    for (auto& kv : interp_->global->vars) {
        names.push_back(kv.first);
        const auto& v0 = kv.second;
        bool fn = v0 && v0->t == musil::Value::FN;
        if (base.count(kv.first) || languageLibraryNames(false).count(kv.first)) continue;
        if (fn && (languageLibraryNames(true).count(kv.first) || languageHelp(kv.first))) continue;   // still the library's
        const auto& v = kv.second;
        if (v && v->t == musil::Value::FN && v->body && v->body->file && !lib.empty() &&
            v->body->file->rfind(lib, 0) == 0)
            continue;                                   // a function of the libraries
        Variable var;
        var.name = kv.first;
        var.kind = musil::type_name(v);
        var.preview = v && v->t == musil::Value::FN ? "(" + [&] {
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
std::shared_ptr<LangContext>& theSession() {
    static std::shared_ptr<LangContext> c;
    return c;
}
}  // namespace

std::shared_ptr<LangContext> sessionContext() {
    auto& c = theSession();
    if (!c) c = std::make_shared<LangContext>("session");
    return c;
}


// ---- the documentation

namespace {
struct HelpIndex {
    std::vector<HelpEntry> entries;
    std::map<std::string, size_t> byName;
    std::vector<std::string> names;
};
// Made once and never destroyed: a script's thread that Stop left running may still look a name up while the program
// exits, and an index destroyed at exit would be read freed (ThreadSanitizer, process_script_stopped_from_another_thread)
const HelpIndex& helpIndex() {
    static const HelpIndex& h = *new HelpIndex([] {
        HelpIndex x;
        std::ifstream f(musilHelpFile());
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
    }());
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
