// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// core.cpp — atoms, s-expressions, the Box base class and the Patch:
// editing, signal-graph compilation, scheduler, messaging and file format.

#include "objects.h"

#include <algorithm>
#include <sys/stat.h>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace moritz {

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
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

static std::string numberText(const Atom& a) {
    std::string t = formatFloat(a.f);
    if (a.dec && t.find_first_not_of("-0123456789") == std::string::npos) t += '.';
    return t;
}

std::string Atom::str() const { return type == Float ? numberText(*this) : s; }

Atom parseAtom(const std::string& tok) {
    if (tok.empty()) return Atom(tok);
    char c = tok[0];
    bool numeric = std::isdigit((unsigned char)c) ||
                   ((c == '-' || c == '+' || c == '.') && tok.size() > 1);
    if (numeric) {
        char* end = nullptr;
        float v = std::strtof(tok.c_str(), &end);
        if (end && *end == '\0') {
            Atom a(v);
            a.dec = tok.find_first_of(".eE") != std::string::npos;
            return a;
        }
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

std::string homeDir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");    // MSYS2 shells set HOME to /home/...
    if (!h || !*h) h = std::getenv("HOME");
#else
    const char* h = std::getenv("HOME");
#endif
    return portablePath(h ? h : "");
}

bool isAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() > 2 && std::isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

std::string portablePath(std::string p) {
#ifdef _WIN32
    std::replace(p.begin(), p.end(), '\\', '/');
#endif
    return p;
}

std::string dirOf(const std::string& path) {
    size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? "." : k == 0 ? "/" : path.substr(0, k);
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
    return a.isFloat() ? numberText(a) : quoteSymbol(a.s);
}

// a box's script as (script "line" ...), one quoted line each, indented under the box
static std::string scriptForm(const Box* b, const std::string& pad) {
    std::string t = b->script();
    if (t.empty()) return "";
    std::string o = "\n" + pad + "    (script";
    size_t s = 0;
    while (s <= t.size()) {
        size_t e = t.find('\n', s);
        std::string line = t.substr(s, e == std::string::npos ? std::string::npos : e - s);
        std::string q = "\"";
        for (char c : line) {
            if (c == '"' || c == '\\') q += '\\';
            q += c;
        }
        o += "\n" + pad + "      " + q + "\"";
        if (e == std::string::npos) break;
        s = e + 1;
    }
    return o + ")";
}


// ============================================================================
// 2. boxes
// ============================================================================

namespace {
// the audio thread: where the block being run starts on the sample count,
// whether this callback holds the buffer~ lock, and whether we are inside
// Patch::process at all (the allocation guard of the tests looks at it)
thread_local uint64_t tlBlockStart = 0;
thread_local bool tlBuffers = false;
thread_local int tlInProcess = 0;

constexpr int kMaxVector = 16384;              // the largest pfft~ frame's bins, with room
const float kZeros[kMaxVector] = {};
}  // namespace

bool audioBuffersAvailable() { return tlBuffers; }
bool inAudioProcess() { return tlInProcess > 0; }

// the values of a box's inlets: the latest given (GUI), and the one in force
// at the start of the block being run (audio)
struct Box::Scalars {
    int n;
    std::unique_ptr<std::atomic<float>[]> latest;
    std::unique_ptr<float[]> cur;
    explicit Scalars(int count) : n(count), latest(new std::atomic<float>[(size_t)count]), cur(new float[(size_t)count]) {
        for (int i = 0; i < n; ++i) {
            latest[i].store(0.f, std::memory_order_relaxed);
            cur[i] = 0.f;
        }
    }
};

// a box's timed events: posted by the dispatch thread, taken by the audio
// thread when they fall due (the ones of the block being run are in `due`)
struct Box::Inbox {
    static constexpr int kDue = 64;
    dsp::Spsc<Event, 256> q;
    std::atomic<bool> resync{false};     // full, or values given while audio was off: catch up at once
    Event due[kDue];
    int off[kDue] = {};
    int ndue = 0;
};

Box::Box() = default;

Box::~Box() {
    delete scal_.load(std::memory_order_relaxed);
    delete inbox_.load(std::memory_order_relaxed);
}

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

std::string Box::label() const {
    if (!keepRaw) return text();
    std::string raw = messageToString(rawArgs);
    if (kind == BoxKind::Message || kind == BoxKind::Comment) return raw;
    return raw.empty() ? cls : cls + " " + raw;
}

// Ports may change after the box runs (a subpatcher's inlets): the values
// only ever grow, and an old array is kept until the box goes, since the
// audio thread may still be reading it. The signal buffers belong to the plan.
void Box::setPorts(std::vector<Port> in, std::vector<Port> out) {
    ins = std::move(in);
    outs = std::move(out);
    int need = std::max(1, (int)ins.size());
    Scalars* s = scal_.load(std::memory_order_relaxed);
    if (s && s->n >= need) return;
    auto ns = std::make_unique<Scalars>(need);
    if (s)
        for (int i = 0; i < s->n; ++i) {
            float v = s->latest[i].load(std::memory_order_relaxed);
            ns->latest[i].store(v, std::memory_order_relaxed);
            ns->cur[i] = v;
        }
    scal_.store(ns.release(), std::memory_order_release);
    if (s) oldScalars_.emplace_back(s);
}

float Box::scalar(int i) const {
    Scalars* s = scal_.load(std::memory_order_acquire);
    return s && i >= 0 && i < s->n ? s->latest[i].load(std::memory_order_relaxed) : 0.f;
}

void Box::setScalar(int i, float v) {
    Scalars* s = scal_.load(std::memory_order_acquire);
    if (!s || i < 0 || i >= s->n) return;
    s->latest[i].store(v, std::memory_order_relaxed);
    if (!patch) {                    // not in a patch yet, so not running: the value in force too
        s->cur[i] = v;
        return;
    }
    if (i < numIns() && ins[(size_t)i] == Port::Signal) post(kEvScalar, v, 0.f, i);
}

Box::Inbox* Box::inbox() {
    Inbox* q = inbox_.load(std::memory_order_acquire);
    if (!q) {
        q = new Inbox;
        inbox_.store(q, std::memory_order_release);
    }
    return q;
}

void Box::post(uint8_t kind, float a, float b, int inlet) {
    Inbox* q = inbox();
    Patch* r = patch ? patch->root() : nullptr;
    bool running = r && r->dspRunning.load(std::memory_order_relaxed);
    if (!running && kind == kEvScalar) {        // nothing plays: the values are in force at once when it does
        q->resync.store(true, std::memory_order_release);
        return;
    }
    Event e;
    e.kind = kind;
    e.a = a;
    e.b = b;
    e.inlet = (int16_t)inlet;
    if (running) {
        uint64_t at = r->stamp();
        // in the order sent: a message never lands before one sent earlier (within 0.1 s)
        if (at < lastStamp_ && lastStamp_ - at < (uint64_t)(0.1 * r->sampleRate.load(std::memory_order_relaxed)))
            at = lastStamp_;
        e.at = lastStamp_ = at;
    }
    if (!q->q.push(e)) q->resync.store(true, std::memory_order_release);   // full: catch up at once
}

void Box::beginBlock(uint64_t t0, int n, bool untimed) {
    Inbox* q = inbox_.load(std::memory_order_acquire);
    if (!q) return;
    q->ndue = 0;
    Event e;
    if (q->resync.load(std::memory_order_relaxed) && q->resync.exchange(false, std::memory_order_acq_rel)) {
        // all at once, the newest kDue of them (in order): what was sent last is what counts
        int n = 0;
        while (q->q.pop(e))
            if (e.kind != kEvScalar) q->due[n++ % Inbox::kDue] = e;
        if (n > Inbox::kDue) std::rotate(q->due, q->due + n % Inbox::kDue, q->due + Inbox::kDue);
        q->ndue = std::min(n, (int)Inbox::kDue);
        std::fill(q->off, q->off + q->ndue, 0);
        if (Scalars* s = scal_.load(std::memory_order_acquire))
            for (int i = 0; i < s->n; ++i) s->cur[i] = s->latest[i].load(std::memory_order_relaxed);
        return;
    }
    const uint64_t end = t0 + (uint64_t)n;
    while (q->ndue < Inbox::kDue && q->q.front(e) && (untimed || e.at < end)) {
        q->q.pop(e);
        q->due[q->ndue] = e;
        q->off[q->ndue++] = untimed || e.at <= t0 ? 0 : (int)(e.at - t0);
    }
}

void Box::endBlock() {
    Inbox* q = inbox_.load(std::memory_order_relaxed);
    if (!q || q->ndue == 0) return;
    Scalars* s = scal_.load(std::memory_order_acquire);
    for (int k = 0; k < q->ndue; ++k) {
        const Event& e = q->due[k];
        if (e.kind == kEvScalar && s && e.inlet >= 0 && e.inlet < s->n) s->cur[e.inlet] = e.a;
    }
    q->ndue = 0;
}

int Box::events() const {
    Inbox* q = inbox_.load(std::memory_order_relaxed);
    return q ? q->ndue : 0;
}

const Event& Box::event(int k) const { return inbox_.load(std::memory_order_relaxed)->due[k]; }

int Box::eventOffset(int k) const { return inbox_.load(std::memory_order_relaxed)->off[k]; }

const float* Box::sigIn(int i, int n) {
    const BoxIO* io = io_;
    if (!io || i < 0 || i >= (int)io->src.size() || !io->scratch[(size_t)i]) return kZeros;
    const auto& src = io->src[(size_t)i];
    if (src.size() == 1) return src[0];
    float* buf = io->scratch[(size_t)i];
    if (src.empty()) {                               // the inlet's value, changing at its events
        Scalars* s = scal_.load(std::memory_order_acquire);
        dsp::fill(buf, s && i < s->n ? s->cur[i] : 0.f, n);
        if (Inbox* q = inbox_.load(std::memory_order_relaxed))
            for (int k = 0; k < q->ndue; ++k)
                if (q->due[k].kind == kEvScalar && q->due[k].inlet == i && q->off[k] < n)
                    dsp::fill(buf + q->off[k], q->due[k].a, n - q->off[k]);
        return buf;
    }
    std::copy(src[0], src[0] + n, buf);              // fan-in sums, as in MSP
    for (size_t k = 1; k < src.size(); ++k)
        for (int j = 0; j < n; ++j) buf[j] += src[k][j];
    return buf;
}

float* Box::sigOut(int i) {
    const BoxIO* io = io_;
    if (!io) return nullptr;
    if (i < 0 || i >= (int)io->out.size() || !io->out[(size_t)i]) return io->sink;   // ports changed meanwhile
    return io->out[(size_t)i];
}

double* Box::sigOutPrecise(int i) {
    const BoxIO* io = io_;
    return io && i >= 0 && i < (int)io->outD.size() ? io->outD[(size_t)i] : nullptr;
}

const double* Box::sigInPrecise(int i) {
    const BoxIO* io = io_;
    return io && i >= 0 && i < (int)io->srcD.size() ? io->srcD[(size_t)i] : nullptr;
}

int Box::outCount() const { return io_ ? (int)io_->out.size() : 0; }

float* Box::sigOutIf(int i) {
    const BoxIO* io = io_;
    return io && i >= 0 && i < (int)io->out.size() ? io->out[(size_t)i] : nullptr;
}

bool Box::connected(int i) const {
    return io_ && i >= 0 && i < (int)io_->src.size() && !io_->src[(size_t)i].empty();
}

int Box::innerCount() const { return io_ ? (int)io_->inner.size() : 0; }

void Box::runInner(int k, int n) {
    if (io_ && k >= 0 && k < (int)io_->inner.size() && io_->inner[(size_t)k]) io_->inner[(size_t)k]->run(n);
}

void PatchPlan::run(int n) const {
    const uint64_t t0 = tlBlockStart;
    for (const Step& s : steps) {
        Box* b = s.box;
        b->io_ = &s.io;
        b->beginBlock(t0, n, spectral);
        b->dsp(n);
        b->endBlock();
    }
}

void Box::outlet(int i, const Message& m) {
    if (!patch || m.empty()) return;
    Patch* r = patch->root();
    if (r->foreignThread()) {                        // another thread: the patch's own runs it
        r->runOnDispatchThread([this, i, m] { outlet(i, m); });
        return;
    }
    if (r->depth > 256) {
        patch->status("stack overflow: message loop stopped at " + id);
        return;
    }
    if (targetsGen_ != patch->connGen_) {            // the cables changed: this box's outlets again
        targets_.assign(outs.size(), {});
        for (auto& c : patch->conns)
            if (c.from == this && c.outlet >= 0 && c.outlet < (int)targets_.size())
                targets_[(size_t)c.outlet].push_back({c.to, c.inlet});
        targetsGen_ = patch->connGen_;
    }
    if (i < 0 || i >= (int)targets_.size() || targets_[(size_t)i].empty()) return;
    // Like Max: fan-out is delivered right-to-left by destination position.
    // (Positions change as boxes are dragged, so they are sorted each time, on
    // a copy: a message may change the cables.)
    const auto& t = targets_[(size_t)i];
    std::pair<Box*, int> local[16];
    std::vector<std::pair<Box*, int>> many;
    std::pair<Box*, int>* v = local;
    size_t n = t.size();
    if (n > 16) { many = t; v = many.data(); }
    else std::copy(t.begin(), t.end(), local);
    for (size_t a = 1; a < n; ++a)                   // insertion sort: stable, nothing allocated
        for (size_t b = a; b > 0 && v[b].first->x > v[b - 1].first->x; --b) std::swap(v[b], v[b - 1]);
    ++r->depth;
    for (size_t k = 0; k < n; ++k) v[k].first->message(v[k].second, m);
    --r->depth;
}


// ============================================================================
// 3. patch
// ============================================================================

std::function<void(Patch*)> Patch::onDestroyed;

Patch::Patch() = default;

Patch::~Patch() {
    if (!parent) {
        // nothing of it may run any more: no plan, then wait for the audio thread to let go
        plan_.store(nullptr, std::memory_order_seq_cst);
        collectArmed_ = true;              // no timer for it: it is collected right here
        if (current_) retire(std::move(current_));
        timers_.clear();
        collectGarbage(true);
    }
    extras.clear();                        // before the timers they may hold are gone
    if (onDestroyed) onDestroyed(this);
    if (parent) {
        // an inner patch dies with its owner box, which the audio thread no
        // longer runs (it was retired); its timers live in the root
        Patch* r = root();
        for (auto& b : boxes) r->cancel(b.get());
    }
    timers_.clear();
    conns.clear();
    boxes.clear();
    garbage_.clear();
}

void Patch::forEachBox(const std::function<void(Box*)>& fn) {
    for (auto& b : boxes) {
        fn(b.get());
        for (Patch* c : b->inners()) c->forEachBox(fn);
    }
    for (auto& e : extras) e->forEachBox(fn);
}

Patch* Patch::addExtra(std::unique_ptr<Patch> p) {
    Patch* raw = p.get();
    raw->parent = this;
    extras.push_back(std::move(p));
    return raw;
}

void Patch::removeExtra(Patch* p) {
    std::unique_ptr<Patch> gone;
    for (auto it = extras.begin(); it != extras.end(); ++it)
        if (it->get() == p) {
            gone = std::move(*it);
            extras.erase(it);
            break;
        }
    recompile();                // a plan without it
    if (gone) {
        retire(std::shared_ptr<Patch>(gone.release()));
        collectGarbage(true);   // gone now (its window with it), once the audio thread let go
    }
    structureChanged();
}

void Patch::structureChanged() {
    if (onStructure) onStructure();
    if (parent) parent->structureChanged();
}

int Patch::vectorSize() const {
    const Spectral* sp = spectralContext();
    return sp ? std::max(sp->bins, dsp::kBlock) : dsp::kBlock;
}

const Patch::Spectral* Patch::spectralContext() const {
    for (const Patch* p = this; p; p = p->parent)
        if (p->spectral.bins > 0) return &p->spectral;
    return nullptr;
}

std::unique_ptr<Box> Patch::create(const std::string& cls, const Message& written) {
    Message args;
    bool changed = false;
    for (auto& a : written) {
        args.push_back(substitute(a));
        changed = changed || !(args.back().type == a.type &&
                               (a.isFloat() ? args.back().f == a.f : args.back().s == a.s));
    }
    auto b = makeBox(cls, args, this);
    if (changed) {
        b->rawArgs = written;
        b->keepRaw = true;
    }
    return b;
}

// #0 is a number unique to each abstraction instance (for local send,
// receive and buffer~ names); #1..#9 are the abstraction's arguments.
// Outside any abstraction they become 0, so an abstraction file can be
// opened and edited on its own (the written form is what gets saved).
Atom Patch::substitute(const Atom& a) const {
    if (!a.isSymbol() || a.s.find('#') == std::string::npos) return a;
    const Patch* scope = this;
    while (scope && !scope->isAbstraction) scope = scope->parent;
    static const Patch none;             // an abstraction opened on its own: #n is 0
    if (!scope) scope = &none;
    std::string out;
    const std::string& s = a.s;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '#' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])) {
            int k = s[i + 1] - '0';
            if (k == 0) out += std::to_string(scope->instance);
            else if (k <= (int)scope->absArgs.size()) out += scope->absArgs[(size_t)k - 1].str();
            else out += "0";
            ++i;
        } else {
            out += s[i];
        }
    }
    return parseAtom(out);
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
    boxes.push_back(std::move(b));
    recompile();
    raw->attached();
    structureChanged();
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
    if (from->outs[o] == Port::Tap && !to->acceptsTap(i))
        return fail("tapin~ connects only to the left inlet of tapout~");
    for (auto& c : conns)
        if (c.from == from && c.outlet == o && c.to == to && c.inlet == i)
            return fail("already connected");
    return true;
}

bool Patch::reaches(Box* a, Box* b) const {
    std::vector<Box*> todo{a};
    std::unordered_set<Box*> seen;
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
    conns.push_back({from, o, to, i});
    cablesChanged();
    recompile();
    structureChanged();
    return true;
}

bool Patch::reconnect(int k, Box* from, int o, Box* to, int i, std::string* err) {
    if (k < 0 || k >= (int)conns.size()) {
        if (err) *err = "no such cable";
        return false;
    }
    std::vector<Connection> next = conns;
    next.erase(next.begin() + k);
    if (!canConnect(next, from, o, to, i, err)) return false;
    next.insert(next.begin() + k, Connection{from, o, to, i});
    std::swap(conns, next);                 // a signal loop, with the cable where it goes
    bool loop = from->outs[o] == Port::Signal && reaches(to, from);
    if (loop) {
        std::swap(conns, next);
        if (err) *err = "that would create a signal feedback loop";
        return false;
    }
    cablesChanged();
    recompile();
    structureChanged();
    return true;
}

bool Patch::insertIntoCable(int k, Box* b, std::string* err) {
    auto fail = [&](const char* what) {
        if (err) *err = what;
        return false;
    };
    if (k < 0 || k >= (int)conns.size()) return fail("no such cable");
    Connection c = conns[k];
    if (!b || b->numIns() < 1 || b->numOuts() < 1) return fail("the box needs an inlet and an outlet");
    if (c.from == b || c.to == b) return fail("the box is already at an end of the cable");
    std::vector<Connection> next = conns;
    next.erase(next.begin() + k);
    if (!canConnect(next, c.from, c.outlet, b, 0, err)) return false;
    next.insert(next.begin() + k, Connection{c.from, c.outlet, b, 0});
    if (!canConnect(next, b, 0, c.to, c.inlet, err)) return false;
    next.insert(next.begin() + k + 1, Connection{b, 0, c.to, c.inlet});
    std::swap(conns, next);                 // a loop through the box (the rest had none)
    bool loop = (c.from->outs[c.outlet] == Port::Signal && reaches(b, c.from)) ||
                (b->outs[0] == Port::Signal && reaches(c.to, b));
    if (loop) {
        std::swap(conns, next);
        return fail("that would create a signal feedback loop");
    }
    cablesChanged();
    recompile();
    structureChanged();
    return true;
}

void Patch::removeConnections(const std::vector<int>& idx) {
    std::vector<int> s = idx;
    std::sort(s.rbegin(), s.rend());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    for (int k : s)
        if (k >= 0 && k < (int)conns.size()) conns.erase(conns.begin() + k);
    cablesChanged();
    recompile();
    structureChanged();
}

// Removed boxes are retired: the audio thread may still be running them in
// the plan before; they are destroyed as soon as it has let go (the GUI
// waits for that, at most a callback, so a removed box is gone on return).
void Patch::retireBoxes(std::vector<std::unique_ptr<Box>> gone) {
    if (gone.empty()) return;
    root()->retire(std::make_shared<std::vector<std::unique_ptr<Box>>>(std::move(gone)));
    root()->collectGarbage(true);
}

void Patch::removeBoxes(const std::vector<Box*>& bs) {
    std::unordered_set<Box*> dead(bs.begin(), bs.end());
    for (Box* b : bs) cancel(b);
    conns.erase(std::remove_if(conns.begin(), conns.end(),
                               [&](const Connection& c) { return dead.count(c.from) || dead.count(c.to); }),
                conns.end());
    cablesChanged();
    std::vector<std::unique_ptr<Box>> gone, kept;
    for (auto& b : boxes) (dead.count(b.get()) ? gone : kept).push_back(std::move(b));
    boxes = std::move(kept);
    recompile();
    retireBoxes(std::move(gone));
    structureChanged();
}

Box* Patch::replaceBox(Box* old, std::unique_ptr<Box> nb) {
    nb->patch = this;
    nb->id = old->id;
    nb->x = old->x;
    nb->y = old->y;
    Box* raw = nb.get();
    std::vector<std::unique_ptr<Box>> doomed;
    cancel(old);
    for (auto& b : boxes)
        if (b.get() == old) {
            doomed.push_back(std::move(b));
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
    cablesChanged();
    recompile();
    retireBoxes(std::move(doomed));
    raw->attached();
    structureChanged();
    return raw;
}

void Patch::clear() {
    if (parent) for (auto& b : boxes) root()->cancel(b.get());
    else { timers_.clear(); collectArmed_ = false; }
    conns.clear();
    cablesChanged();
    std::vector<std::unique_ptr<Box>> gone = std::move(boxes);
    boxes.clear();
    recompile();
    retireBoxes(std::move(gone));
    structureChanged();
}

bool Patch::needsInput() const {
    for (auto& b : boxes) {
        if (b->wantsInput()) return true;
        for (Patch* c : b->inners())
            if (c->needsInput()) return true;
    }
    for (auto& e : extras)
        if (e->needsInput()) return true;
    return false;
}

int Patch::maxOutputChannel() const {
    int m = 0;
    for (auto& b : boxes) {
        m = std::max(m, b->outputChannels());
        for (Patch* c : b->inners()) m = std::max(m, c->maxOutputChannel());
    }
    for (auto& e : extras) m = std::max(m, e->maxOutputChannel());
    return m;
}

// ---- compiling: the plan

// Compiles every patch under the root (its own boxes, the inner patches and
// the extras) into a new plan, on the GUI thread, and publishes it with one
// pointer swap. The audio thread keeps running the old plan until its next
// callback; the old plan is retired.
void Patch::compile() {
    indexDirty_ = true;              // boxes came or went: the name index follows
    planRate_ = sampleRate.load();
    ++compiles;
    auto plan = std::make_shared<PatchPlan>();
    buildPlan(*plan);
    for (auto& e : extras) {
        auto ep = std::make_unique<PatchPlan>();
        e->buildPlan(*ep);
        plan->extras.push_back(ep.get());
        plan->owned.push_back(std::move(ep));
    }
    std::shared_ptr<PatchPlan> old = std::move(current_);
    current_ = plan;
    plan_.store(plan.get(), std::memory_order_seq_cst);
    if (old) retire(std::move(old));
    collectGarbage();
}

void Patch::rateChanged() {
    if (parent) { root()->rateChanged(); return; }
    if (sampleRate.load() != planRate_) compile();
}

namespace {
// send~ and receive~ names inside a box (itself, or anywhere in its inner patches)
void signalNames(Box* b, std::set<std::string>& sends, std::set<std::string>& recvs) {
    if (auto s = b->signalSendName(); !s.empty()) sends.insert(s);
    if (auto r = b->signalReceiveName(); !r.empty()) recvs.insert(r);
    for (Patch* c : b->inners())
        for (auto& x : c->boxes) signalNames(x.get(), sends, recvs);
}

// A box holding a send~ runs before a box holding a receive~ of the same
// name, unless that would close a loop: then the receive~ reads the previous
// block (a one-block delay, as in Max).
void addSendReceiveEdges(const std::vector<Box*>& sig, std::vector<std::vector<int>>& next, std::vector<int>& indeg) {
    std::vector<std::pair<int, std::set<std::string>>> senders, receivers;
    for (int k = 0; k < (int)sig.size(); ++k) {
        std::set<std::string> s, r;
        signalNames(sig[(size_t)k], s, r);
        if (!s.empty()) senders.push_back({k, std::move(s)});
        if (!r.empty()) receivers.push_back({k, std::move(r)});
    }
    if (senders.empty() || receivers.empty()) return;
    std::vector<char> seen(sig.size());
    auto reachesVia = [&](int from, int to) {
        std::fill(seen.begin(), seen.end(), 0);
        std::vector<int> stack{from};
        while (!stack.empty()) {
            int x = stack.back();
            stack.pop_back();
            if (x == to) return true;
            if (seen[(size_t)x]) continue;
            seen[(size_t)x] = 1;
            for (int y : next[(size_t)x]) stack.push_back(y);
        }
        return false;
    };
    for (auto& [s, sn] : senders)
        for (auto& [r, rn] : receivers) {
            if (s == r) continue;
            bool shared = false;
            for (auto& n : sn) shared = shared || rn.count(n);
            if (!shared || reachesVia(r, s)) continue;
            next[(size_t)s].push_back(r);
            indeg[(size_t)r]++;
        }
}
}  // namespace

// Orders this patch's signal boxes so every box runs after the boxes feeding
// it, and gives each signal port its buffer in the plan.
void Patch::buildPlan(PatchPlan& pp) {
    // inner patches first: their inlets/outlets decide the owner's ports
    std::unordered_map<Box*, std::vector<const PatchPlan*>> innerOf;
    for (auto& b : boxes) {
        if (!b->inner()) continue;
        auto& v = innerOf[b.get()];
        for (Patch* c : b->inners()) {
            auto cp = std::make_unique<PatchPlan>();
            c->buildPlan(*cp);
            v.push_back(cp.get());
            pp.owned.push_back(std::move(cp));
        }
        b->syncPorts();
    }
    // drop cables whose ports vanished or changed type
    size_t before = conns.size();
    conns.erase(std::remove_if(conns.begin(), conns.end(),
                               [](const Connection& c) {
                                   return c.outlet >= c.from->numOuts() || c.inlet >= c.to->numIns() ||
                                          (c.from->outs[(size_t)c.outlet] == Port::Signal &&
                                           c.to->ins[(size_t)c.inlet] == Port::Control);
                               }),
                conns.end());
    if (conns.size() != before) cablesChanged();

    const int vs = vectorSize();
    pp.vs = vs;
    pp.spectral = spectralContext() != nullptr;
    std::vector<Box*> sig;
    std::unordered_map<Box*, int> index;
    for (auto& b : boxes)
        if (b->isSignal() || b->inner()) {          // subpatchers run their inner chain
            index[b.get()] = (int)sig.size();
            sig.push_back(b.get());
        }
    const int N = (int)sig.size();
    std::vector<std::vector<int>> next((size_t)N);  // edges of the signal graph
    std::vector<int> indeg((size_t)N, 0);
    for (auto& c : conns) {
        if (c.from->outs[(size_t)c.outlet] != Port::Signal) continue;   // control and tap links
        auto f = index.find(c.from), t = index.find(c.to);
        if (f == index.end() || t == index.end()) continue;
        next[(size_t)f->second].push_back(t->second);
        indeg[(size_t)t->second]++;
    }
    addSendReceiveEdges(sig, next, indeg);
    std::vector<int> order;                         // Kahn, first come first served
    order.reserve((size_t)N);
    for (int k = 0; k < N; ++k)
        if (indeg[(size_t)k] == 0) order.push_back(k);
    for (size_t head = 0; head < order.size(); ++head)
        for (int t : next[(size_t)order[head]])
            if (--indeg[(size_t)t] == 0) order.push_back(t);

    if ((int)order.size() != N) {
        dspError = "signal feedback loop: audio disabled for this patch";
        order.clear();
    } else {
        dspError.clear();
        for (auto& b : boxes)
            if (Patch* c = b->inner(); c && !c->dspError.empty()) {   // first copy speaks for all
                dspError = c->dspError + " (in " + b->text() + ")";
                break;
            }
    }
    // the memory of the signal ports: a buffer per signal outlet and per
    // signal inlet (for sums and values), and a sink
    size_t total = (size_t)vs, totalD = 0;
    for (int k : order) {
        Box* b = sig[(size_t)k];
        for (size_t o = 0; o < b->outs.size(); ++o)
            if (b->outs[o] == Port::Signal) {
                total += (size_t)vs;
                if (b->preciseOut((int)o)) totalD += (size_t)vs;
            }
        for (auto p : b->ins) total += p == Port::Signal ? (size_t)vs : 0;
    }
    pp.arena.assign(total, 0.f);
    pp.arenaD.assign(totalD, 0.0);
    float* mem = pp.arena.data();
    double* memD = pp.arenaD.data();
    float* sink = mem;
    mem += vs;
    std::vector<int> stepOf((size_t)N, -1);
    pp.steps.reserve(order.size());
    for (int k : order) {
        Box* b = sig[(size_t)k];
        PatchPlan::Step s{b, {}};
        s.io.sink = sink;
        s.io.out.assign(b->outs.size(), nullptr);
        s.io.outD.assign(b->outs.size(), nullptr);
        for (size_t o = 0; o < b->outs.size(); ++o)
            if (b->outs[o] == Port::Signal) {
                s.io.out[o] = mem;
                mem += vs;
                if (b->preciseOut((int)o)) { s.io.outD[o] = memD; memD += vs; }
            }
        s.io.src.assign(b->ins.size(), {});
        s.io.srcD.assign(b->ins.size(), nullptr);
        s.io.scratch.assign(b->ins.size(), nullptr);
        for (size_t i = 0; i < b->ins.size(); ++i)
            if (b->ins[i] == Port::Signal) { s.io.scratch[i] = mem; mem += vs; }
        if (auto it = innerOf.find(b); it != innerOf.end()) s.io.inner = it->second;
        stepOf[(size_t)k] = (int)pp.steps.size();
        pp.steps.push_back(std::move(s));
    }
    if (!pp.steps.empty())
        for (auto& c : conns) {
            if (c.from->outs[(size_t)c.outlet] != Port::Signal) continue;
            auto f = index.find(c.from), t = index.find(c.to);
            if (f == index.end() || t == index.end()) continue;
            int sf = stepOf[(size_t)f->second], st = stepOf[(size_t)t->second];
            BoxIO& to = pp.steps[(size_t)st].io;
            const BoxIO& from = pp.steps[(size_t)sf].io;
            to.src[(size_t)c.inlet].push_back(from.out[(size_t)c.outlet]);
            // one precise source: its doubles too; more (a sum): floats only
            to.srcD[(size_t)c.inlet] = to.src[(size_t)c.inlet].size() == 1 ? from.outD[(size_t)c.outlet] : nullptr;
        }
    for (auto& b : boxes) b->resolve();
}

void Patch::rebuild() {
    if (foreignThread()) { runOnDispatchThread([this] { rebuild(); }); return; }
    recompile();
    structureChanged();
}

// ---- retiring what the audio thread may still be using

void Patch::retire(std::shared_ptr<void> x) {
    if (parent) { root()->retire(std::move(x)); return; }
    if (!x) return;
    // the epoch after the swap that made x unreachable: freed once the audio
    // thread has started a callback in it (or is not in one)
    uint64_t tag = epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
    garbage_.push_back({tag, std::move(x)});
    if (!collectArmed_ && !offline) {         // free it soon even if nothing else happens
        collectArmed_ = true;
        scheduleAt(nullptr, time() + 0.05, [this] {
            collectArmed_ = false;
            collectGarbage();
            if (!garbage_.empty() && !collectArmed_) {
                collectArmed_ = true;
                scheduleAt(nullptr, time() + 0.05, [this] { collectArmed_ = false; collectGarbage(); });
            }
        });
    }
}

void Patch::collectGarbage(bool wait) {
    if (parent) { root()->collectGarbage(wait); return; }
    for (int tries = 0;; ++tries) {
        bool in = inside_.load(std::memory_order_seq_cst);
        uint64_t seen = seen_.load(std::memory_order_seq_cst);
        std::vector<std::shared_ptr<void>> dead;
        size_t keep = 0;
        for (size_t k = 0; k < garbage_.size(); ++k) {
            if (!in || seen >= garbage_[k].first) dead.push_back(std::move(garbage_[k].second));
            else garbage_[keep++] = std::move(garbage_[k]);
        }
        garbage_.resize(keep);
        dead.clear();                       // destructors may retire more: garbage_ is consistent again
        if (garbage_.empty() || !wait || tries >= 2000) return;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// ---- messages from other threads

void Patch::setDispatchThread(std::thread::id t) { root()->dispatch_.store(t); }

bool Patch::foreignThread() const {
    std::thread::id d = root()->dispatch_.load(std::memory_order_relaxed);
    return d != std::thread::id() && d != std::this_thread::get_id();
}

void Patch::runOnDispatchThread(std::function<void()> fn) {
    Patch* r = root();
    std::lock_guard<std::mutex> lk(r->foreignMutex_);
    r->foreign_.push_back(std::move(fn));
    r->hasForeign_.store(true, std::memory_order_release);
}

void Patch::drainForeign() {
    Patch* r = root();
    if (!r->hasForeign_.load(std::memory_order_acquire)) return;
    std::vector<std::function<void()>> q;
    {
        std::lock_guard<std::mutex> lk(r->foreignMutex_);
        q.swap(r->foreign_);
        r->hasForeign_.store(false, std::memory_order_relaxed);
    }
    for (auto& fn : q) fn();
}

void Patch::print(const std::string& s) {
    if (onPrint) onPrint(s);
    else if (parent) parent->print(s);
    else std::fprintf(stderr, "%s\n", s.c_str());
}

namespace {
bool pathExists(const std::string& p) {      // a file or a folder
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

std::string Patch::resolvePath(const std::string& p) const {
    if (p.empty() || isAbsolutePath(p)) return p;
    if (p.rfind("~/", 0) == 0) return homeDir() + p.substr(1);
    std::string here = p;
    for (const Patch* q = this; q; q = q->parent)
        if (!q->dir.empty()) { here = q->dir + "/" + p; break; }
    if (!pathExists(here))                       // not next to the patch: the bundled sounds
        for (auto& d : soundPath())
            if (std::string f = d + "/" + p; pathExists(f)) return f;
    return here;
}

// ---- scheduler: a timer heap, run by the GUI (or by a render)

namespace {
// the heap's order: the earliest on top, then the first scheduled
bool laterTimer(double ta, uint64_t sa, double tb, uint64_t sb) { return ta > tb || (ta == tb && sa > sb); }
}  // namespace

void Patch::scheduleAt(Box* owner, double t, std::function<void()> fn) {
    if (parent) { root()->scheduleAt(owner, t, std::move(fn)); return; }
    timers_.push_back({t, timerSeq_++, owner, std::move(fn)});
    std::push_heap(timers_.begin(), timers_.end(),
                   [](const Timer& a, const Timer& b) { return laterTimer(a.t, a.seq, b.t, b.seq); });
    if (!inRunDue_ && onSchedule) onSchedule();
}

void Patch::cancel(Box* owner) {
    if (parent) { root()->cancel(owner); return; }
    size_t before = timers_.size();
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.owner == owner; }),
                  timers_.end());
    if (timers_.size() == before) return;
    std::make_heap(timers_.begin(), timers_.end(),
                   [](const Timer& a, const Timer& b) { return laterTimer(a.t, a.seq, b.t, b.seq); });
    if (!inRunDue_ && onSchedule) onSchedule();
}

double Patch::nextDue() const {
    const Patch* r = root();
    return r->timers_.empty() ? std::numeric_limits<double>::infinity() : r->timers_.front().t;
}

double Patch::logicalTime() const {
    const Patch* r = root();
    return r->inRunDue_ ? r->dueNow_ : r->time();
}

void Patch::runDue(double now) {
    if (parent) { root()->runDue(now); return; }
    auto later = [](const Timer& a, const Timer& b) { return laterTimer(a.t, a.seq, b.t, b.seq); };
    bool outer = !inRunDue_;
    double prevNow = dueNow_;
    inRunDue_ = true;
    for (int guard = 0; guard < 100000 && !timers_.empty() && timers_.front().t <= now; ++guard) {
        std::pop_heap(timers_.begin(), timers_.end(), later);
        Timer tm = std::move(timers_.back());
        timers_.pop_back();
        // the timer's own time: what it schedules next counts from there, and
        // the messages it sends are stamped with it. A timer more than a quarter
        // second late (the GUI was stuck) fires once, now, and what it schedules
        // counts from now: no burst to catch up. Offline the time is always exact.
        dueNow_ = offline || tm.t >= now - 0.25 ? tm.t : now;
        tm.fn();
    }
    dueNow_ = prevNow;
    if (outer) inRunDue_ = false;
    if (onSchedule) onSchedule();
}

// ---- timed events: where a message sent now lands on the sample count

uint64_t Patch::stamp() {
    if (parent) return root()->stamp();
    double sr = sampleRate.load(std::memory_order_relaxed);
    double t = logicalTime();
    if (offline) return t <= 0 ? 0 : (uint64_t)std::llround(t * sr);
    uint64_t now = sampleTime_.load(std::memory_order_acquire);
    if (!anchored_.load(std::memory_order_acquire)) return now;
    // two device buffers: the callback may have computed one buffer ahead of the clock already,
    // and a GUI timer may fire up to one buffer late; later than that, it lands late
    double latency = 2.0 * std::max(ioBufferFrames.load(std::memory_order_relaxed), dsp::kBlock);
    double target = clockOffset_.load(std::memory_order_relaxed) + t * sr + latency;
    return (uint64_t)std::llround(std::clamp(target, (double)now, (double)now + sr));
}

// At the start of a callback: the sample count against the clock (seconds).
// The offset is smoothed, so the callbacks' own jitter does not move events;
// a jump (the device restarted) resets it.
void Patch::anchorClock() {
    if (!clock) return;
    double sr = sampleRate.load(std::memory_order_relaxed);
    double off = (double)sampleTime_.load(std::memory_order_relaxed) - clock() * sr;
    if (!anchored_.load(std::memory_order_relaxed) || std::fabs(off - offsetEst_) > 0.1 * sr) offsetEst_ = off;
    else offsetEst_ += 0.02 * (off - offsetEst_);
    clockOffset_.store(offsetEst_, std::memory_order_relaxed);
    anchored_.store(true, std::memory_order_release);
}

void Patch::process(const float* in, int inCh, float* out, int outCh, int frames) {
    ++tlInProcess;
    std::fill(out, out + (size_t)frames * outCh, 0.f);
    // say which epoch we are in before taking the plan (see collectGarbage)
    inside_.store(true, std::memory_order_seq_cst);
    seen_.store(epoch_.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
    const PatchPlan* plan = plan_.load(std::memory_order_seq_cst);
    std::unique_lock<std::mutex> lk(dspMutex, std::try_to_lock);   // buffer~ storage: never waits
    bool prevBuffers = tlBuffers;
    tlBuffers = lk.owns_lock();
    if (!offline) anchorClock();
    ioIn = in;
    ioOut = out;
    ioInCh = in ? inCh : 0;
    ioOutCh = outCh;
    uint64_t t = sampleTime_.load(std::memory_order_relaxed);
    for (int off = 0; off < frames; off += dsp::kBlock) {
        int n = std::min(dsp::kBlock, frames - off);
        ioOffset = off;
        tlBlockStart = t;
        if (plan) {
            plan->run(n);
            for (const PatchPlan* e : plan->extras) e->run(n);
        }
        t += (uint64_t)n;
        sampleTime_.store(t, std::memory_order_release);
        ++blockCount;
    }
    for (size_t k = 0, N = (size_t)frames * outCh; k < N; ++k)
        out[k] = std::isfinite(out[k]) ? std::clamp(out[k], -1.f, 1.f) : 0.f;
    ioIn = nullptr;
    ioOut = nullptr;
    tlBuffers = prevBuffers;
    if (lk.owns_lock()) lk.unlock();
    inside_.store(false, std::memory_order_release);
    --tlInProcess;
}

// ---- names: receive and buffer~, indexed

void Patch::rebuildIndex() {
    receivers_.clear();
    buffers_.clear();
    forEachBox([&](Box* b) {
        if (auto r = dynamic_cast<ReceiveBox*>(b)) receivers_[r->name].push_back(r);
        else if (auto bb = dynamic_cast<BufferBox*>(b)) buffers_.emplace(bb->name, bb);   // the first one
    });
    indexDirty_ = false;
}

const std::vector<ReceiveBox*>& Patch::receivers(const std::string& name) {
    Patch* r = root();
    if (r->indexDirty_) r->rebuildIndex();
    static const std::vector<ReceiveBox*> none;
    auto it = r->receivers_.find(name);
    return it == r->receivers_.end() ? none : it->second;
}

BufferBox* Patch::bufferNamed(const std::string& name) {
    Patch* r = root();
    if (r->indexDirty_) r->rebuildIndex();
    auto it = r->buffers_.find(name);
    return it == r->buffers_.end() ? nullptr : it->second;
}

void Patch::sendTo(const std::string& name, const Message& m) {
    if (m.empty()) return;
    if (parent) { root()->sendTo(name, m); return; }   // names are global, as in Max
    if (foreignThread()) { runOnDispatchThread([this, name, m] { sendTo(name, m); }); return; }
    if (name == "dsp") {                       // "; dsp start" / "; dsp stop"
        if (m[0].isFloat()) { if (onDsp) onDsp(m[0].f != 0.f); }
        else handleDspMessage(this, m);
        return;
    }
    if (depth > 256) { status("stack overflow: message loop via " + name); return; }
    std::vector<ReceiveBox*> rs = receivers(name);   // a copy: receiving may change the patches
    ++depth;
    for (auto r : rs) r->receive(m);
    --depth;
}

void Patch::loadbangAll() {
    for (size_t i = 0; i < boxes.size(); ++i) boxes[i]->loadbang();   // p boxes recurse
}

bool handleDspMessage(Patch* p, const Message& m) {
    Patch* r = p->root();
    if (m[0].is("start") || m[0].is("startwindow")) { if (r->onDsp) r->onDsp(true); return true; }
    if (m[0].is("stop")) { if (r->onDsp) r->onDsp(false); return true; }
    return false;
}

// Classes whose box text is one string (message, comment, expr, expr~): the
// object files register them; build() joins their arguments before making them.
std::set<std::string>& wholeTextClasses() {
    static std::set<std::string> s;
    return s;
}

bool takesWholeText(const std::string& cls) {
    (void)classInfos();                        // the object files have registered theirs
    return wholeTextClasses().count(cls) > 0;
}

// ---- file format
//
//   (patch
//     (obj osc cycle~ 440 (pos 40 40))
//     (obj p1 message "; freq 880" (pos 200 40))
//     (connect osc 0 gain 0))
//
// `pos` is optional; boxes without it are stacked down the left side.

std::string Patch::serialize(int indent) const {
    std::ostringstream o;
    std::string pad((size_t)indent, ' ');
    o << "(patch";
    for (auto& b : boxes) {
        o << "\n" << pad << "  (obj " << quoteSymbol(b->id) << ' ' << quoteSymbol(b->cls);
        for (auto& a : b->keepRaw ? b->rawArgs : b->saveArgs()) o << ' ' << writeAtom(a);
        o << " (pos " << b->x << ' ' << b->y << ")";
        if (b->savesInner() && b->inner())
            o << "\n" << pad << "    " << b->inner()->serialize(indent + 4);
        o << scriptForm(b.get(), pad);
        o << ")";
    }
    for (auto& c : conns)
        o << "\n" << pad << "  (connect " << quoteSymbol(c.from->id) << ' ' << c.outlet << ' '
          << quoteSymbol(c.to->id) << ' ' << c.inlet << ')';
    o << ")";
    if (indent == 0) o << "\n";
    return o.str();
}

bool Patch::load(const std::string& src, std::string& err) {
    std::vector<SExpr> top;
    if (!parseSExprs(src, top, err)) return false;
    if (top.size() != 1) {
        err = "not a patch: expected a single (patch ...) form";
        return false;
    }
    return load(top[0], err);
}

// Makes the boxes and cables of a (patch ...) form for this patch, without
// adding them yet.
bool Patch::build(const SExpr& form, std::vector<std::unique_ptr<Box>>& nb,
                  std::vector<Connection>& nc, std::vector<std::string>& warnings,
                  std::string& err) {
    if (!form.isList || form.items.empty() || form.items[0].isList ||
        form.items[0].text != "patch") {
        err = "not a patch: expected a (patch ...) form";
        return false;
    }
    std::map<std::string, Box*> byId;
    int autoY = 20;
    auto headIs = [](const SExpr& e, const char* h) {
        return e.isList && !e.items.empty() && !e.items[0].isList && e.items[0].text == h;
    };

    for (size_t k = 1; k < form.items.size(); ++k) {
        const SExpr& e = form.items[k];
        std::string where = "line " + std::to_string(e.line) + ": ";
        if (headIs(e, "obj")) {
            if (e.items.size() < 3 || e.items[1].isList || e.items[2].isList) {
                err = where + "expected (obj id class args...)";
                return false;
            }
            std::string id = e.items[1].text, cls = e.items[2].text;
            Message raw;                    // as written; create() substitutes #1..
            const SExpr* innerForm = nullptr;
            const SExpr* scriptF = nullptr;
            int x = 20, y = -1;
            for (size_t j = 3; j < e.items.size(); ++j) {
                const SExpr& a = e.items[j];
                if (headIs(a, "pos") && a.items.size() >= 3) {
                    x = (int)std::atof(a.items[1].text.c_str());
                    y = (int)std::atof(a.items[2].text.c_str());
                } else if (headIs(a, "patch")) {
                    innerForm = &a;
                } else if (headIs(a, "script")) {
                    scriptF = &a;
                } else if (!a.isList) {
                    raw.push_back(sexprAtom(a));
                }
            }
            if (takesWholeText(cls))
                raw = {Atom(messageToString(raw))};
            if (y < 0) { y = autoY; autoY += 40; }
            if (byId.count(id)) {
                err = where + "duplicate id '" + id + "'";
                return false;
            }
            auto b = create(cls, raw);
            b->patch = this;
            b->id = id;
            b->x = x;
            b->y = y;
            if (innerForm) {
                std::vector<std::string> w;
                b->loadInner(*innerForm, w);
                for (auto& s : w) warnings.push_back("in " + cls + " " + id + ": " + s);
            }
            if (scriptF) {
                std::string t;
                for (size_t j = 1; j < scriptF->items.size(); ++j) t += (j > 1 ? "\n" : "") + scriptF->items[j].text;
                b->setScript(t);
            }
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
    return true;
}

bool Patch::load(const SExpr& form, std::string& err) {
    std::vector<std::unique_ptr<Box>> nb;
    std::vector<Connection> nc;
    std::vector<std::string> warnings;
    if (!build(form, nb, nc, warnings, err)) return false;

    std::vector<std::unique_ptr<Box>> old;
    if (parent) for (auto& b : boxes) root()->cancel(b.get());
    else { timers_.clear(); collectArmed_ = false; }
    old.swap(boxes);
    conns = std::move(nc);
    cablesChanged();
    boxes = std::move(nb);
    recompile();
    retireBoxes(std::move(old));
    for (size_t k = 0; k < boxes.size(); ++k) boxes[k]->attached();
    err.clear();
    for (auto& w : warnings) err += (err.empty() ? "" : "\n") + w;
    structureChanged();
    return true;
}

// Paste: adds the boxes and cables of `src`, a (patch ...) text, moved by
// dx, dy. Ids already used here are renamed. The rest of the patch keeps
// running untouched.
bool Patch::merge(const std::string& src, int dx, int dy, std::vector<Box*>& added,
                  std::string& err) {
    std::vector<SExpr> top;
    if (!parseSExprs(src, top, err)) return false;
    if (top.size() != 1) { err = "not a patch"; return false; }
    std::vector<std::unique_ptr<Box>> nb;
    std::vector<Connection> nc;
    std::vector<std::string> warnings;
    if (!build(top[0], nb, nc, warnings, err)) return false;
    std::set<std::string> taken;
    for (auto& b : boxes) taken.insert(b->id);
    for (auto& b : nb) {
        b->x += dx;
        b->y += dy;
        if (taken.count(b->id)) {
            std::string base = uniqueId(b->cls);          // e.g. cycle3: then step past taken ones
            std::string stem = base;
            while (!stem.empty() && std::isdigit((unsigned char)stem.back())) stem.pop_back();
            for (int k = 1;; ++k)
                if (!taken.count(stem + std::to_string(k))) { b->id = stem + std::to_string(k); break; }
        }
        taken.insert(b->id);
    }
    added.clear();
    for (auto& b : nb) {
        added.push_back(b.get());
        boxes.push_back(std::move(b));
    }
    for (auto& c : nc) conns.push_back(c);
    cablesChanged();
    recompile();
    for (Box* b : added) b->attached();
    err.clear();
    for (auto& w : warnings) err += (err.empty() ? "" : "\n") + w;
    structureChanged();
    return true;
}

// Copy: the boxes in `sel` and the cables between them, as a (patch ...) text.
std::string Patch::serializeBoxes(const std::vector<Box*>& sel) const {
    std::set<const Box*> in(sel.begin(), sel.end());
    std::ostringstream o;
    o << "(patch";
    for (auto& b : boxes) {
        if (!in.count(b.get())) continue;
        o << "\n  (obj " << quoteSymbol(b->id) << ' ' << quoteSymbol(b->cls);
        for (auto& a : b->keepRaw ? b->rawArgs : b->saveArgs()) o << ' ' << writeAtom(a);
        o << " (pos " << b->x << ' ' << b->y << ")";
        if (b->savesInner() && b->inner()) o << "\n    " << b->inner()->serialize(4);
        o << scriptForm(b.get(), "");
        o << ")";
    }
    for (auto& c : conns)
        if (in.count(c.from) && in.count(c.to))
            o << "\n  (connect " << quoteSymbol(c.from->id) << ' ' << c.outlet << ' '
              << quoteSymbol(c.to->id) << ' ' << c.inlet << ')';
    o << ")\n";
    return o.str();
}

// The text view: applies an edited (patch ...) text to this patch. Boxes whose
// line is unchanged (class, arguments, contents) stay the same objects, with
// their state and sound; only new or changed boxes are made, and the cables
// are replaced. Nothing changes if the text has an error.
bool Patch::applyText(const std::string& src, std::string& err) {
    std::vector<SExpr> top;
    if (!parseSExprs(src, top, err)) return false;
    if (top.size() != 1) { err = "expected a single (patch ...) form"; return false; }
    std::vector<std::unique_ptr<Box>> nb;
    std::vector<Connection> nc;
    std::vector<std::string> warnings;
    if (!build(top[0], nb, nc, warnings, err)) return false;

    auto signature = [](const Box* b) {
        std::string t = b->cls;
        for (auto& a : b->keepRaw ? b->rawArgs : b->saveArgs()) t += ' ' + writeAtom(a);
        if (b->savesInner() && b->inner()) t += b->inner()->serialize(0);
        t += b->script();
        return t;
    };
    std::map<std::string, Box*> oldById;
    for (auto& b : boxes) oldById[b->id] = b.get();
    std::map<Box*, Box*> replaceWith;            // new box -> the old one kept instead
    for (auto& b : nb) {
        auto it = oldById.find(b->id);
        if (it != oldById.end() && signature(it->second) == signature(b.get())) replaceWith[b.get()] = it->second;
    }
    std::vector<Connection> conns2;
    for (auto c : nc) {
        if (replaceWith.count(c.from)) c.from = replaceWith[c.from];
        if (replaceWith.count(c.to)) c.to = replaceWith[c.to];
        conns2.push_back(c);
    }
    std::vector<std::unique_ptr<Box>> next, gone, unused;
    std::vector<Box*> added;
    std::set<Box*> kept;
    for (auto& b : nb) {
        if (Box* old = replaceWith.count(b.get()) ? replaceWith[b.get()] : nullptr) {
            old->x = b->x;
            old->y = b->y;
            kept.insert(old);
            unused.push_back(std::move(b));
        } else {
            added.push_back(b.get());
            next.push_back(std::move(b));
        }
    }
    {
        // keep the text's order: kept boxes go where the text puts them
        std::vector<std::unique_ptr<Box>> ordered;
        std::map<Box*, std::unique_ptr<Box>*> oldSlot;
        for (auto& b : boxes) oldSlot[b.get()] = &b;
        size_t ni = 0;
        for (auto& f : top[0].items) {
            if (!f.isList || f.items.size() < 2 || f.items[0].isList || f.items[0].text != "obj") continue;
            const std::string& id = f.items[1].text;
            Box* o = oldById.count(id) ? oldById[id] : nullptr;
            if (o && kept.count(o)) ordered.push_back(std::move(*oldSlot[o]));
            else if (ni < next.size()) ordered.push_back(std::move(next[ni++]));
        }
        for (; ni < next.size(); ++ni) ordered.push_back(std::move(next[ni]));
        for (auto& b : boxes)
            if (b) {
                root()->cancel(b.get());
                gone.push_back(std::move(b));
            }
        boxes = std::move(ordered);
        conns = std::move(conns2);
        cablesChanged();
        recompile();
        retireBoxes(std::move(gone));
    }
    unused.clear();                              // never ran
    for (Box* b : added) b->attached();
    err.clear();
    for (auto& w : warnings) err += (err.empty() ? "" : "\n") + w;
    structureChanged();
    return true;
}

}  // namespace moritz
