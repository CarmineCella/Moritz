// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// core.cpp — atoms, s-expressions, the Box base class and the Patch:
// editing, signal-graph compilation, scheduler, messaging and file format.

#include "objects.h"

#include <algorithm>
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

std::string Box::label() const {
    if (!keepRaw) return text();
    std::string raw = messageToString(rawArgs);
    if (kind == BoxKind::Message || kind == BoxKind::Comment) return raw;
    return raw.empty() ? cls : cls + " " + raw;
}

void Box::setPorts(std::vector<Port> in, std::vector<Port> out) {
    ins = std::move(in);
    outs = std::move(out);
    size_t n = std::max<size_t>(dsp::kBlock, outBuf_.empty() ? (inBuf_.empty() ? 0 : inBuf_[0].size())
                                                             : outBuf_[0].size());
    outBuf_.assign(outs.size(), std::vector<float>(n, 0.f));
    inBuf_.assign(ins.size(), std::vector<float>(n, 0.f));
    scalar_.reset(new std::atomic<float>[ins.size() ? ins.size() : 1]);
    for (size_t i = 0; i < ins.size(); ++i) scalar_[i].store(0.f);
    inSrc.assign(ins.size(), {});
}

void Box::ensureBlock(int n) {
    for (auto& b : outBuf_) if ((int)b.size() < n) b.resize((size_t)n, 0.f);
    for (auto& b : inBuf_) if ((int)b.size() < n) b.resize((size_t)n, 0.f);
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
    if (patch->root()->depth > 256) {
        patch->status("stack overflow: message loop stopped at " + id);
        return;
    }
    // Like Max: fan-out is delivered right-to-left by destination position.
    std::vector<std::pair<Box*, int>> targets;
    for (auto& c : patch->conns)
        if (c.from == this && c.outlet == i) targets.push_back({c.to, c.inlet});
    std::stable_sort(targets.begin(), targets.end(),
                     [](auto& a, auto& b) { return a.first->x > b.first->x; });
    ++patch->root()->depth;
    for (auto& t : targets) t.first->message(t.second, m);
    --patch->root()->depth;
}


// ============================================================================
// 3. patch
// ============================================================================

std::function<void(Patch*)> Patch::onDestroyed;

Patch::Patch() = default;

Patch::~Patch() {
    if (!extras.empty()) {                 // before the timers they may hold are gone
        std::lock_guard<std::mutex> lk(dspMutex);
        extras.clear();
    }
    if (onDestroyed) onDestroyed(this);
    if (parent) {
        // an inner patch dies with its owner box, while the root holds the
        // DSP lock (or is being destroyed); its timers live in the root
        Patch* r = root();
        for (auto& b : boxes) r->cancel(b.get());
        chain_.clear();
        conns.clear();
        boxes.clear();
        return;
    }
    timers_.clear();
    std::lock_guard<std::mutex> lk(dspMutex);
    chain_.clear();
    conns.clear();
    boxes.clear();
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
    std::lock_guard<std::mutex> lk(mutex());
    extras.push_back(std::move(p));
    return raw;
}

void Patch::removeExtra(Patch* p) {
    std::unique_ptr<Patch> gone;
    {
        std::lock_guard<std::mutex> lk(mutex());
        for (auto it = extras.begin(); it != extras.end(); ++it)
            if (it->get() == p) {
                gone = std::move(*it);
                extras.erase(it);
                break;
            }
        recompile();          // nothing may point into it any more
    }
    gone.reset();
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
    {
        std::lock_guard<std::mutex> lk(mutex());
        boxes.push_back(std::move(b));
        recompile();
    }
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
        std::lock_guard<std::mutex> lk(mutex());
        conns.push_back({from, o, to, i});
        recompile();
    }
    structureChanged();
    return true;
}

void Patch::removeConnections(const std::vector<int>& idx) {
    std::vector<int> s = idx;
    std::sort(s.rbegin(), s.rend());
    {
        std::lock_guard<std::mutex> lk(mutex());
        for (int k : s)
            if (k >= 0 && k < (int)conns.size()) conns.erase(conns.begin() + k);
        recompile();
    }
    structureChanged();
}

void Patch::removeBoxes(const std::vector<Box*>& bs) {
    std::set<Box*> dead(bs.begin(), bs.end());
    for (Box* b : bs) cancel(b);
    {
        std::lock_guard<std::mutex> lk(mutex());
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
        recompile();
    }
    structureChanged();
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
        std::lock_guard<std::mutex> lk(mutex());
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
        recompile();
    }
    raw->attached();
    structureChanged();
    return raw;
}

void Patch::clear() {
    if (parent) for (auto& b : boxes) root()->cancel(b.get());
    else timers_.clear();
    {
        std::lock_guard<std::mutex> lk(mutex());
        chain_.clear();
        conns.clear();
        boxes.clear();
        recompile();
    }
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

// Orders signal boxes so every box runs after the boxes feeding it, and
// points each signal inlet at the output buffers connected to it.
void Patch::compile() {
    // inner patches first: their inlets/outlets decide the owner's ports
    for (auto& b : boxes)
        if (b->inner()) {
            for (Patch* c : b->inners()) c->compile();
            b->syncPorts();
        }
    // drop cables whose ports vanished or changed type
    conns.erase(std::remove_if(conns.begin(), conns.end(),
                               [](const Connection& c) {
                                   return c.outlet >= c.from->numOuts() ||
                                          c.inlet >= c.to->numIns() ||
                                          (c.from->outs[c.outlet] == Port::Signal &&
                                           c.to->ins[c.inlet] == Port::Control);
                               }),
                conns.end());

    std::vector<Box*> sig;
    std::map<Box*, int> indeg;
    int vs = vectorSize();
    for (auto& b : boxes) {
        b->ensureBlock(vs);                  // before inSrc points into the buffers
        for (auto& s : b->inSrc) s.clear();
        if (b->isSignal() || b->inner()) {          // subpatchers run their inner chain
            sig.push_back(b.get());
            indeg[b.get()] = 0;
        }
    }
    std::map<Box*, std::vector<Box*>> next;     // edges of the signal graph
    for (auto& c : conns) {
        if (c.from->outs[c.outlet] != Port::Signal) continue;   // control and tap links
        c.to->inSrc[c.inlet].push_back(c.from->sigOut(c.outlet));
        next[c.from].push_back(c.to);
        indeg[c.to]++;
    }
    addSendReceiveEdges(sig, next, indeg);
    std::vector<Box*> order, ready;
    for (Box* b : sig)
        if (indeg[b] == 0) ready.push_back(b);
    while (!ready.empty()) {
        Box* b = ready.front();
        ready.erase(ready.begin());
        order.push_back(b);
        for (Box* t : next[b])
            if (--indeg[t] == 0) ready.push_back(t);
    }
    if (order.size() != sig.size()) {
        dspError = "signal feedback loop: audio disabled for this patch";
        chain_.clear();
    } else {
        dspError.clear();
        chain_ = std::move(order);
        for (auto& b : boxes)
            if (Patch* c = b->inner(); c && !c->dspError.empty()) {   // first copy speaks for all
                dspError = c->dspError + " (in " + b->text() + ")";
                break;
            }
    }
    for (auto& b : boxes) b->resolve();
    for (auto& e : extras) e->compile();
}

namespace {
// send~ and receive~ names inside a box (itself, or anywhere in its inner patches)
void signalNames(Box* b, std::set<std::string>& sends, std::set<std::string>& recvs) {
    if (auto s = b->signalSendName(); !s.empty()) sends.insert(s);
    if (auto r = b->signalReceiveName(); !r.empty()) recvs.insert(r);
    for (Patch* c : b->inners())
        for (auto& x : c->boxes) signalNames(x.get(), sends, recvs);
}
}  // namespace

// A box holding a send~ runs before a box holding a receive~ of the same
// name, unless that would close a loop: then the receive~ reads the previous
// block (a one-block delay, as in Max).
void Patch::addSendReceiveEdges(const std::vector<Box*>& sig, std::map<Box*, std::vector<Box*>>& next,
                                std::map<Box*, int>& indeg) {
    std::vector<std::pair<Box*, std::set<std::string>>> senders, receivers;
    for (Box* b : sig) {
        std::set<std::string> s, r;
        signalNames(b, s, r);
        if (!s.empty()) senders.push_back({b, s});
        if (!r.empty()) receivers.push_back({b, r});
    }
    auto reachesVia = [&](Box* from, Box* to) {
        std::vector<Box*> stack{from};
        std::set<Box*> seen;
        while (!stack.empty()) {
            Box* x = stack.back();
            stack.pop_back();
            if (x == to) return true;
            if (!seen.insert(x).second) continue;
            for (Box* y : next[x]) stack.push_back(y);
        }
        return false;
    };
    for (auto& [s, sn] : senders)
        for (auto& [r, rn] : receivers) {
            if (s == r) continue;
            bool shared = false;
            for (auto& n : sn) shared = shared || rn.count(n);
            if (!shared || reachesVia(r, s)) continue;
            next[s].push_back(r);
            indeg[r]++;
        }
}

void Patch::rebuild() {
    {
        std::lock_guard<std::mutex> lk(mutex());
        recompile();
    }
    structureChanged();
}

void Patch::print(const std::string& s) {
    if (onPrint) onPrint(s);
    else if (parent) parent->print(s);
    else std::fprintf(stderr, "%s\n", s.c_str());
}

std::string Patch::resolvePath(const std::string& p) const {
    if (p.empty() || isAbsolutePath(p)) return p;
    if (p.rfind("~/", 0) == 0) return homeDir() + p.substr(1);
    for (const Patch* q = this; q; q = q->parent)
        if (!q->dir.empty()) return q->dir + "/" + p;
    return p;
}

// ---- scheduler: a small timer queue, run by the GUI (or by --render)

void Patch::scheduleAt(Box* owner, double t, std::function<void()> fn) {
    if (parent) { root()->scheduleAt(owner, t, std::move(fn)); return; }
    timers_.push_back({t, timerSeq_++, owner, std::move(fn)});
    if (!inRunDue_ && onSchedule) onSchedule();
}

void Patch::cancel(Box* owner) {
    if (parent) { root()->cancel(owner); return; }
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
        for (auto& e : extras) e->runChain(n);
        ++blockCount;
    }
    for (size_t k = 0, N = (size_t)frames * outCh; k < N; ++k)
        out[k] = std::isfinite(out[k]) ? std::clamp(out[k], -1.f, 1.f) : 0.f;
    ioIn = nullptr;
    ioOut = nullptr;
}

void Patch::sendTo(const std::string& name, const Message& m) {
    if (m.empty()) return;
    if (parent) { root()->sendTo(name, m); return; }   // names are global, as in Max
    if (name == "dsp") {                       // "; dsp start" / "; dsp stop"
        if (m[0].isFloat()) { if (onDsp) onDsp(m[0].f != 0.f); }
        else handleDspMessage(this, m);
        return;
    }
    std::vector<ReceiveBox*> rs;
    forEachBox([&](Box* b) {
        if (auto r = dynamic_cast<ReceiveBox*>(b))
            if (r->name == name) rs.push_back(r);
    });
    if (depth > 256) { status("stack overflow: message loop via " + name); return; }
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
            int x = 20, y = -1;
            for (size_t j = 3; j < e.items.size(); ++j) {
                const SExpr& a = e.items[j];
                if (headIs(a, "pos") && a.items.size() >= 3) {
                    x = (int)std::atof(a.items[1].text.c_str());
                    y = (int)std::atof(a.items[2].text.c_str());
                } else if (headIs(a, "patch")) {
                    innerForm = &a;
                } else if (!a.isList) {
                    raw.push_back(sexprAtom(a));
                }
            }
            if (cls == "message" || cls == "comment" || cls == "expr" || cls == "expr~")
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
    else timers_.clear();
    {
        std::lock_guard<std::mutex> lk(mutex());
        old.swap(boxes);
        conns = std::move(nc);
        boxes = std::move(nb);
        recompile();
        old.clear();
    }
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
    {
        std::lock_guard<std::mutex> lk(mutex());
        for (auto& b : nb) {
            added.push_back(b.get());
            boxes.push_back(std::move(b));
        }
        for (auto& c : nc) conns.push_back(c);
        recompile();
    }
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
        std::lock_guard<std::mutex> lk(mutex());
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
        recompile();
        gone.clear();
    }
    unused.clear();
    for (Box* b : added) b->attached();
    err.clear();
    for (auto& w : warnings) err += (err.empty() ? "" : "\n") + w;
    structureChanged();
    return true;
}

}  // namespace moritz
