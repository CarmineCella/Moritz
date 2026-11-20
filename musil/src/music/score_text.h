// score_text.h — .score files: a score written as Musil data, read with Musil's reader and never evaluated.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// The forms are those score::toText writes (score.h). Reading checks everything a file from elsewhere could
// get wrong: the nesting (the reader's own limit, and kMaxDepth scores inside scores), the size of the file,
// numbers that are not finite or out of range, forms of the wrong shape, a score that would contain itself
// through the files it places. Every problem is a score::Error naming the file and the line; nothing is run.
// Times are seconds: a bars.beats.ticks word (1.1.30) is refused with an error that says so.
#pragma once
#include "../core.h"
#include "score.h"
#include <fstream>
#include <map>
#include <sstream>

namespace musil {
namespace score {

constexpr size_t kMaxFileBytes = 64u << 20;     // a .score larger than this is refused
constexpr size_t kMaxEvents = 4000000;           // events in one score

// What a reading needs besides the text: where the file is (references are relative to its folder), and how
// to open a referenced file (a host checks its permissions there; the default reads it). The scores read
// are kept by path: a file placed three times is one score, three instances.
struct ReadContext {
    std::function<std::string(const std::string& path)> readFile;    // the text of a file, or an Error
    std::map<std::string, std::shared_ptr<Score>> loaded;             // resolved path -> score
    std::vector<std::string> open;                                    // the files being read (a cycle check)
    int depth = 0;
};

inline std::string readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("score: cannot open " + path);
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) throw Error("score: cannot read " + path);
    if ((size_t)n > kMaxFileBytes) throw Error("score: " + path + " is larger than " + std::to_string(kMaxFileBytes >> 20) + " MB");
    f.seekg(0);
    std::string s((size_t)n, '\0');
    f.read(&s[0], n);
    if (!f) throw Error("score: cannot read " + path);
    return s;
}
inline std::string folderOf(const std::string& path) {
    size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? std::string() : path.substr(0, k);
}
inline std::string normalPath(const std::string& p) { return std::filesystem::path(p).lexically_normal().generic_string(); }
inline bool absolutePath(const std::string& p) {
    return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 2 && p[1] == ':' && (p[2] == '/' || p[2] == '\\')));
}

namespace detail {

struct Reader {
    ReadContext& ctx;
    std::string file;      // for the errors
    std::string dir;       // references are relative to it

    [[noreturn]] void fail(const vptr& at, const std::string& m) const {
        int line = at ? at->line : 0;
        throw Error(file + (line > 0 ? ":" + std::to_string(line) : std::string()) + ": " + m);
    }
    static bool isSym(const vptr& v, const char* s = nullptr) { return v && v->t == Value::SYM && (!s || v->s == s); }
    static bool isList(const vptr& v) { return v && v->t == Value::LIST; }
    double number(const vptr& v, const char* what, double lo, double hi) const {
        if (!v || v->t != Value::NUM || v->num.size() != 1) {
            if (isSym(v) && std::count(v->s.begin(), v->s.end(), '.') >= 2)
                fail(v, std::string(what) + ": times are written in seconds (1.5), not bars.beats.ticks (" + v->s + ")");
            fail(v, std::string(what) + ": a number is wanted");
        }
        double d = v->num[0];
        if (!std::isfinite(d)) fail(v, std::string(what) + ": not a finite number");
        if (d < lo || d > hi) fail(v, std::string(what) + ": " + numText(d) + " is out of range (" + numText(lo) + " to " + numText(hi) + ")");
        return d;
    }
    std::string word(const vptr& v, const char* what) const {
        if (v && (v->t == Value::STR || v->t == Value::SYM)) return v->s;
        if (v && v->t == Value::NUM && v->num.size() == 1) return numText(v->num[0]);
        fail(v, std::string(what) + ": a name is wanted");
    }
    Datum datum(const vptr& v, int depth) const {
        if (depth > 64) fail(v, "a value nested more than 64 deep");
        if (!v) return Datum::symbol("nil");
        switch (v->t) {
        case Value::NUM:
            if (v->num.size() == 1) { if (!std::isfinite(v->num[0])) fail(v, "not a finite number"); return Datum::number(v->num[0]); }
            fail(v, "a number is wanted");
        case Value::STR: return Datum::string(v->s);
        case Value::SYM: return Datum::symbol(v->s);
        case Value::LIST: {
            std::vector<Datum> l;
            size_t from = (!v->l.empty() && isSym(v->l[0], "list")) ? 1 : 0;
            l.reserve(v->l.size());
            for (size_t k = from; k < v->l.size(); k++) l.push_back(datum(v->l[k], depth + 1));
            return Datum::of(std::move(l));
        }
        default: fail(v, "not a value a score can hold");
        }
    }
    // (data key value)
    void data(const vptr& f, Fields& into) const {
        if (f->l.size() != 3) fail(f, "(data key value) wanted");
        setField(into, word(f->l[1], "data"), datum(f->l[2], 0));
    }
    // the options after the positional part of an event
    void option(const vptr& f, Event& e) const {
        if (!isList(f) || f->l.empty() || !isSym(f->l[0])) fail(f, std::string(kindName(e.kind)) + ": an option (name value) is wanted");
        const std::string& k = f->l[0]->s;
        if (k == "data") { data(f, e.extra); return; }
        if (f->l.size() != 2) fail(f, "(" + k + " value) wanted");
        const vptr& v = f->l[1];
        if (k == "id") e.id = (uint64_t)number(v, "id", 1, 9007199254740992.0);
        else if (k == "gain") e.gain = number(v, "gain", 0, 1e6);
        else if (k == "db") e.gain = std::pow(10.0, number(v, "db", -400, 120) / 20);
        else if (k == "az") e.az = number(v, "az", -1e6, 1e6);
        else if (k == "el") e.el = number(v, "el", -90, 90);
        else if (k == "cents" && e.kind == Kind::Note) e.cents = number(v, "cents", -1e4, 1e4);
        else if (k == "from" && e.kind == Kind::Sound) e.from = number(v, "from", 0, 1e9);
        else if (k == "to" && e.kind == Kind::Sound) e.to = number(v, "to", -1, 1e9);
        else if (k == "dur" && (e.kind == Kind::Score || e.kind == Kind::Message)) e.dur = number(v, "dur", 0, 1e9);
        else if (k == "stretch" && e.kind == Kind::Score) e.stretch = number(v, "stretch", 1e-6, 1e6);
        else if (k == "transpose" && e.kind == Kind::Score) e.transpose = number(v, "transpose", -1000, 1000);
        else if (k == "row") e.row = word(v, "row");
        else if (k == "variant") e.variant = word(v, "variant");
        else if (k == "label") e.label = word(v, "label");
        else fail(f, std::string(kindName(e.kind)) + ": unknown option " + k);
    }
    std::shared_ptr<Event> event(const vptr& f, const std::string& kind) {
        auto e = std::make_shared<Event>();
        const vlist& l = f->l;
        size_t k = 1;
        auto need = [&](const char* what) -> const vptr& {
            if (k >= l.size() || isList(l[k])) fail(f, kind + ": " + what + " is missing");
            return l[k++];
        };
        if (kind == "note") e->kind = Kind::Note;
        else if (kind == "sound") e->kind = Kind::Sound;
        else if (kind == "patch") e->kind = Kind::Patch;
        else if (kind == "msg") e->kind = Kind::Message;
        else e->kind = Kind::Score;
        e->at = number(need("the onset"), "onset", 0, 1e9);
        bool timed = e->kind != Kind::Message && e->kind != Kind::Score;
        if (timed) e->dur = number(need("the duration"), "duration", 0, 1e9);
        bool durSet = false;
        switch (e->kind) {
        case Kind::Note: {
            e->row = word(need("the row"), "row");
            const vptr& p = need("the pitch");
            if (p->t == Value::NUM) e->pitch = number(p, "pitch", -1000, 1000);
            else {
                double m = pitchOfName(word(p, "pitch"));
                if (m < 0) fail(p, "note: " + word(p, "pitch") + " is not a pitch (a MIDI number or a name: C4, F#3, Bb5)");
                e->pitch = m;
            }
            if (k < l.size() && !isList(l[k])) e->dyn = word(l[k++], "dynamics");
            if (k < l.size() && !isList(l[k])) e->tech = word(l[k++], "technique");
            break;
        }
        case Kind::Sound: {
            if (k < l.size() && isList(l[k]) && l[k]->l.size() == 2 && isSym(l[k]->l[0], "buffer")) { e->buffer = true; e->source = word(l[k]->l[1], "buffer"); k++; }
            else e->source = word(need("the file"), "file");
            break;
        }
        case Kind::Patch:
            e->source = word(need("the patch"), "patch");
            if (k < l.size() && isList(l[k]) && !l[k]->l.empty() && isSym(l[k]->l[0], "args")) {
                for (size_t j = 1; j < l[k]->l.size(); j++) e->values.push_back(datum(l[k]->l[j], 0));
                k++;
            }
            break;
        case Kind::Message:
            e->source = word(need("the name"), "name");
            while (k < l.size() && !(isList(l[k]) && !l[k]->l.empty() && isSym(l[k]->l[0]) && l[k]->l[0]->s != "list")) e->values.push_back(datum(l[k++], 0));
            break;
        case Kind::Score: {
            if (k >= l.size()) fail(f, "score: the score placed is missing (a file or a (score ...) form)");
            const vptr& src = l[k++];
            if (isList(src) && !src->l.empty() && isSym(src->l[0], "score")) {
                e->sub = scoreForm(src);
            } else {
                e->ref = word(src, "score file");
                e->sub = referenced(src, e->ref);
            }
            break;
        }
        }
        for (; k < l.size(); k++) {
            if (e->kind == Kind::Score && isList(l[k]) && !l[k]->l.empty() && isSym(l[k]->l[0], "dur")) durSet = true;
            option(l[k], *e);
        }
        if (e->kind == Kind::Score && !durSet) e->dur = e->sub->duration() * e->stretch;
        return e;
    }
    std::shared_ptr<Score> referenced(const vptr& at, const std::string& ref) {
        std::string path = normalPath(absolutePath(ref) || dir.empty() ? ref : dir + "/" + ref);
        if (ctx.depth >= kMaxDepth) fail(at, "scores nested more than " + std::to_string(kMaxDepth) + " deep");
        for (auto& o : ctx.open) if (o == path) fail(at, "a score cannot contain itself: " + ref + " places a score that places it");
        auto hit = ctx.loaded.find(path);
        if (hit != ctx.loaded.end()) return hit->second;
        std::string text = ctx.readFile ? ctx.readFile(path) : readWholeFile(path);
        ctx.open.push_back(path);
        ctx.depth++;
        std::shared_ptr<Score> s;
        try {
            Reader r{ctx, path, folderOf(path)};
            s = r.text(text);
        } catch (...) { ctx.open.pop_back(); ctx.depth--; throw; }
        ctx.open.pop_back();
        ctx.depth--;
        s->path = path;
        ctx.loaded[path] = s;
        return s;
    }
    std::shared_ptr<Score> scoreForm(const vptr& f) {
        if (!isList(f) || f->l.size() < 2 || !isSym(f->l[0], "score")) fail(f, "not a score: a (score \"name\" ...) form is wanted");
        if (ctx.depth >= kMaxDepth) fail(f, "scores nested more than " + std::to_string(kMaxDepth) + " deep");
        ctx.depth++;
        struct Down { int& d; ~Down() { d--; } } down{ctx.depth};
        auto s = std::make_shared<Score>();
        s->name = word(f->l[1], "score name");
        std::vector<std::shared_ptr<Event>> evs;
        uint64_t nextId = 1;
        for (size_t k = 2; k < f->l.size(); k++) {
            const vptr& x = f->l[k];
            if (!isList(x) || x->l.empty() || !isSym(x->l[0])) fail(x ? x : f, "score: a form (name ...) is wanted");
            const std::string& h = x->l[0]->s;
            auto one = [&](const char* what, double lo, double hi) {
                if (x->l.size() != 2) fail(x, "(" + h + " value) wanted");
                return number(x->l[1], what, lo, hi);
            };
            if (h == "sr") s->sr = one("sr", 1, 1e7);
            else if (h == "bpm") s->bpm = one("bpm", 1e-3, 1e5);
            else if (h == "meter") {
                if (x->l.size() != 3) fail(x, "(meter num den) wanted");
                s->meterNum = (int)number(x->l[1], "meter", 1, 1000);
                s->meterDen = (int)number(x->l[2], "meter", 1, 1024);
            } else if (h == "next-id") nextId = (uint64_t)one("next-id", 1, 9007199254740992.0);
            else if (h == "data") data(x, s->extra);
            else if (h == "row") {
                if (x->l.size() < 2) fail(x, "(row name [clef]) wanted");
                Row r;
                r.name = word(x->l[1], "row");
                size_t j = 2;
                if (j < x->l.size() && !isList(x->l[j])) r.clef = word(x->l[j++], "clef");
                for (; j < x->l.size(); j++) {
                    if (!isList(x->l[j]) || x->l[j]->l.empty() || !isSym(x->l[j]->l[0], "data")) fail(x->l[j], "row: (data key value) wanted");
                    data(x->l[j], r.extra);
                }
                s->rows.push_back(std::move(r));
            } else if (h == "note" || h == "sound" || h == "patch" || h == "msg" || h == "score") {
                if (evs.size() >= kMaxEvents) fail(x, "more than " + std::to_string(kMaxEvents) + " events");
                evs.push_back(event(x, h));
            } else fail(x, "score: unknown form " + h);
        }
        try { s->assign(std::move(evs)); } catch (const Error& e) { fail(f, e.what()); }
        if (nextId > s->nextId()) s->setNextId(nextId);
        return s;
    }
    std::shared_ptr<Score> text(const std::string& src) {
        if (src.size() > kMaxFileBytes) throw Error(file + ": larger than " + std::to_string(kMaxFileBytes >> 20) + " MB");
        vptr prog;
        try {
            Parser p(src, std::make_shared<std::string>(file));
            p.literal = false;
            prog = p.program();
        } catch (const std::exception& e) { throw Error(std::string(e.what())); }
        if (!isList(prog) || prog->l.size() != 2 || !isList(prog->l[1]) || prog->l[1]->l.empty() || isList(prog->l[1]->l[0]))
            throw Error(file + ": a .score file holds one (score ...) form");
        return scoreForm(prog->l[1]);
    }
};

}  // namespace detail

// a score from the text of a .score file (file names it in the errors; references are found next to it)
inline std::shared_ptr<Score> readText(const std::string& text, const std::string& file, ReadContext ctx = {}) {
    detail::Reader r{ctx, file, folderOf(file)};
    if (!file.empty()) ctx.open.push_back(normalPath(file));
    return r.text(text);
}
// a .score file
inline std::shared_ptr<Score> readFile(const std::string& path, ReadContext ctx = {}) {
    std::string text = ctx.readFile ? ctx.readFile(path) : readWholeFile(path);
    auto s = readText(text, path, std::move(ctx));
    s->path = path;
    return s;
}
// a score from its form as Musil data, (score "name" ...) (dir: where its references are)
inline std::shared_ptr<Score> fromData(const vptr& form, const std::string& dir = "", ReadContext ctx = {}) {
    detail::Reader r{ctx, "<data>", dir};
    return r.scoreForm(form);
}

// a score written to a file: a temporary file next to it, checked, then renamed over it
inline void writeFile(const Score& s, const std::string& path) {
    std::string text = toText(s);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw Error("score: cannot write " + path);
        f.write(text.data(), (std::streamsize)text.size());
        f.flush();
        if (!f) { f.close(); std::remove(tmp.c_str()); throw Error("score: cannot write " + path); }
        f.close();
        if (f.fail()) { std::remove(tmp.c_str()); throw Error("score: cannot write " + path); }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::remove(tmp.c_str()); throw Error("score: cannot write " + path + ": " + ec.message()); }
}

}  // namespace score
}  // namespace musil
