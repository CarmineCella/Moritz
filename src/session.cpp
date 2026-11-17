// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// session.cpp — contexts of the Moritz language inside the application (session.h)

#include "session.h"

#include "gui.h"
#include "lang/lang.h"
#include "langpatch.h"
#include "objects.h"

#include <FL/Fl.H>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#ifndef _WIN32
#include <pthread.h>
#else
#include <thread>
#endif

#ifdef OPAQUE      // Windows (wingdi.h) defines it; the language has a value type of that name
#undef OPAQUE
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

// ---- the live patches: code on its thread reaches them through the main thread

std::thread::id& mainThread() {
    static std::thread::id id = std::this_thread::get_id();   // first used on the main thread (a context is made there)
    return id;
}

// runs f on the main thread and waits for it (at once when already there)
void onMainWait(const std::function<void()>& f) {
    if (std::this_thread::get_id() == mainThread()) { f(); return; }
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

struct LiveRef { const void* doc; };

lang::vptr liveValue(const void* doc) { return lang::v_opaque("live-patch", std::make_shared<LiveRef>(LiveRef{doc})); }

// the open patch a live value names (main thread), or nullptr when it was closed
MainWindow* liveDoc(const lang::vptr& v) {
    if (!v || v->t != lang::Value::OPAQUE || v->opaque_tag != "live-patch") return nullptr;
    const void* d = std::static_pointer_cast<LiveRef>(v->opaque)->doc;
    for (auto* w : App::get().docs) if (w == d) return w;
    return nullptr;
}

bool isLive(const lang::vptr& v) { return v && v->t == lang::Value::OPAQUE && v->opaque_tag == "live-patch"; }

// the text of a patch value or of an open patch
std::string anyPatchText(lang::Interp& I, const lang::vptr& v) {
    std::string t;
    if (patchValueText(v, t)) return t;
    if (!isLive(v)) I.bad("expected a patch, got " + std::string(lang::type_name(v)));
    bool open = false;
    onMainWait([&] { if (auto* d = liveDoc(v)) { t = d->patch.serialize(); open = true; } });
    if (!open) I.bad("that patch is closed");
    return t;
}

// language values as a message: numbers, strings and symbols, lists spliced
void toMessage(const lang::vptr& v, Message& m) {
    if (!v || v->t == lang::Value::NIL) return;
    if (v->t == lang::Value::NUM) {
        for (double d : v->num) {
            if (v->num.size() == 1 && !v->s.empty()) m.push_back(parseAtom(v->s));
            else m.push_back(Atom(d));
        }
    } else if (v->t == lang::Value::LIST) {
        for (auto& x : v->l) toMessage(x, m);
    } else {
        m.push_back(Atom(v->s));
    }
}

// (this-patch) the patch of this context
lang::vptr fnThisPatch(lang::vlist&, lang::Interp& I) {
    auto v = I.global->find("*patch*");
    if (!v || !isLive(v)) I.bad("the session has no patch: choose a patch's context");
    return v;
}

// (patch-text p) (patch-boxes p): of a patch value or an open patch
lang::vptr fnAnyPatchText(lang::vlist& a, lang::Interp& I) { return lang::v_str(anyPatchText(I, a[0]) + "\n"); }
lang::vptr fnAnyPatchBoxes(lang::vlist& a, lang::Interp& I) { return patchBoxes(anyPatchText(I, a[0])); }

// (send name values...) to every receive name (as a send box, or a ; message)
lang::vptr fnSend(lang::vlist& a, lang::Interp& I) {
    std::string name = I.str(a[0]);
    Message m;
    for (size_t k = 1; k < a.size(); ++k) toMessage(a[k], m);
    if (m.empty()) m.push_back(Atom("bang"));
    onMainWait([&] { App::get().host.sendTo(name, m); });
    return lang::v_nil();
}

// (box-send p id values...) a message into the left inlet of a box of an open patch
lang::vptr fnBoxSend(lang::vlist& a, lang::Interp& I) {
    std::string id = I.str(a[1]);
    Message m;
    for (size_t k = 2; k < a.size(); ++k) toMessage(a[k], m);
    if (m.empty()) m.push_back(Atom("bang"));
    bool found = false, open = false;
    onMainWait([&] {
        auto* d = liveDoc(a[0]);
        if (!d) return;
        open = true;
        for (auto& b : d->patch.boxes)
            if (b->id == id) { found = true; b->message(0, m); break; }
    });
    if (!open) I.bad("that patch is closed");
    if (!found) I.bad("no box " + id);
    return lang::v_nil();
}

// (buffer name) the channels of a buffer~ as vectors; (buffer-rate name) its sample rate
lang::vptr fnBuffer(lang::vlist& a, lang::Interp& I) {
    std::string name = I.str(a[0]);
    SampleBuffer copy;
    bool found = false;
    onMainWait([&] {
        if (auto* b = findBuffer(&App::get().host, name)) {
            std::lock_guard<std::mutex> lk(b->patch->mutex());
            copy = b->buf;
            found = true;
        }
    });
    if (!found) I.bad("no buffer~ " + name);
    lang::vlist chans;
    for (int c = 0; c < copy.channels; ++c) {
        std::valarray<double> v((size_t)copy.frames);
        for (long f = 0; f < copy.frames; ++f) v[(size_t)f] = copy.data[(size_t)f * copy.channels + c];
        chans.push_back(lang::v_arr(std::move(v)));
    }
    return lang::v_list(std::move(chans));
}

lang::vptr fnBufferRate(lang::vlist& a, lang::Interp& I) {
    std::string name = I.str(a[0]);
    double sr = 0;
    onMainWait([&] { if (auto* b = findBuffer(&App::get().host, name)) sr = b->buf.sr; });
    if (sr <= 0) I.bad("no buffer~ " + name);
    return lang::v_num(sr);
}

// (set-buffer! name channels [rate]) fills a buffer~: a vector, or a list of channels
lang::vptr fnSetBuffer(lang::vlist& a, lang::Interp& I) {
    std::string name = I.str(a[0]);
    std::vector<std::valarray<double>> ch;
    if (a[1]->t == lang::Value::NUM) ch.push_back(a[1]->num);
    else for (auto& c : I.list(a[1])) ch.push_back(I.num(c));
    if (ch.empty() || ch.size() > 8) I.bad("1 to 8 channels");
    size_t frames = 0;
    for (auto& c : ch) frames = std::max(frames, c.size());
    double sr = a.size() > 2 ? I.scalar(a[2]) : 0;
    bool found = false;
    onMainWait([&] {
        auto* b = findBuffer(&App::get().host, name);
        if (!b) return;
        found = true;
        {
            std::lock_guard<std::mutex> lk(b->patch->mutex());
            b->buf.channels = (int)ch.size();
            b->buf.frames = (long)frames;
            if (sr > 0) b->buf.sr = sr;
            b->buf.data.assign(frames * ch.size(), 0.f);
            for (size_t c = 0; c < ch.size(); ++c)
                for (size_t f = 0; f < ch[c].size(); ++f) b->buf.data[f * ch.size() + c] = (float)ch[c][f];
            ++b->version;
        }
        b->patch->rebuild();
    });
    if (!found) I.bad("no buffer~ " + name);
    return a[1];
}

// (insert! p patch [dx dy]) adds the boxes of a patch value to an open patch (ids already used are renamed)
lang::vptr fnInsert(lang::vlist& a, lang::Interp& I) {
    std::string text;
    if (!patchValueText(a[1], text)) I.bad("expected a patch to insert, got " + std::string(lang::type_name(a[1])));
    int dx = a.size() > 2 ? (int)I.scalar(a[2]) : 0, dy = a.size() > 3 ? (int)I.scalar(a[3]) : 0;
    std::string err;
    bool open = false, ok = false;
    int n = 0;
    onMainWait([&] {
        auto* d = liveDoc(a[0]);
        if (!d) return;
        open = true;
        d->pushUndo();
        std::vector<Box*> added;
        ok = d->patch.merge(text, dx, dy, added, err);
        if (!ok) { d->dropUndo(); return; }
        n = (int)added.size();
        d->markDirty();
        d->redraw();
    });
    if (!open) I.bad("that patch is closed");
    if (!ok) I.bad(err);
    return lang::v_num(n);
}

// (replace! p patch) an open patch becomes the patch value; boxes that did not change go on running
lang::vptr fnReplace(lang::vlist& a, lang::Interp& I) {
    std::string text;
    if (!patchValueText(a[1], text)) I.bad("expected a patch, got " + std::string(lang::type_name(a[1])));
    std::string err;
    bool open = false, ok = false;
    onMainWait([&] {
        auto* d = liveDoc(a[0]);
        if (!d) return;
        open = true;
        d->pushUndo();
        ok = d->patch.applyText(text, err);
        if (!ok) { d->dropUndo(); return; }
        d->markDirty();
        d->redraw();
    });
    if (!open) I.bad("that patch is closed");
    if (!ok) I.bad(err);
    return a[0];
}

// (open-patch p [title]) a patch value in a window of its own; => the open patch
lang::vptr fnOpenPatch(lang::vlist& a, lang::Interp& I) {
    std::string text;
    if (!patchValueText(a[0], text)) I.bad("expected a patch, got " + std::string(lang::type_name(a[0])));
    std::string title = a.size() > 1 ? I.str(a[1]) : "made by code";
    const void* doc = nullptr;
    onMainWait([&] { doc = App::get().openPatchText(text, title, ""); });
    if (!doc) I.bad("the patch could not be opened");
    return liveValue(doc);
}

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

LangContext::LangContext(std::string l, const void* doc)
    : label(std::move(l)), interp_(std::make_unique<lang::Interp>()) {
    mainThread();
    auto& I = *interp_;
    lang::make_env(I);
    addPatchLanguage(I);
    // the live patches
    I.def("this-patch", fnThisPatch, 0, 0);
    I.def("patch-text", fnAnyPatchText, 1, 1);
    I.def("patch-boxes", fnAnyPatchBoxes, 1, 1);
    I.def("send", fnSend, 1, -1);
    I.def("box-send", fnBoxSend, 2, -1);
    I.def("buffer", fnBuffer, 1, 1);
    I.def("buffer-rate", fnBufferRate, 1, 1);
    I.def("set-buffer!", fnSetBuffer, 2, 3);
    I.def("insert!", fnInsert, 2, 4);
    I.def("replace!", fnReplace, 2, 2);
    I.def("open-patch", fnOpenPatch, 1, 2);
    if (doc) I.def("*patch*", liveValue(doc));
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
        I.toplevel_fn = [&r](const lang::vptr& v) {
            std::string t;
            if (patchValueText(v, t)) r.patches.push_back(t);
        };
        try {
            lang::vptr v = I.run(src, file);
            if (std::string t; patchValueText(v, t)) v = nullptr;   // shown as a window, not as text
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
        I.toplevel_fn = nullptr;
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
    if (!c) c = std::make_shared<LangContext>("session", nullptr);
    return c;
}

std::shared_ptr<LangContext> patchContext(const void* doc, const std::string& t) {
    std::string title = t;
    if (size_t k = title.find(" — Moritz"); k != std::string::npos) title.resize(k);
    auto& c = contexts()[doc];
    if (!c) c = std::make_shared<LangContext>(title, doc);
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
