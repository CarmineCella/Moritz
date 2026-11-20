// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_marks.cpp — markers and regions (sound_marks.h): moved by edits, written and read as Musil data, rows for
// a coll

#include "sound_marks.h"
#include "fileio.h"
#include "moritz.h"
#include "music/score.h"     // Musil's score, as the score object reads it

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace moritz {

void SoundMarks::sort() {
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.at < b.at; });
    std::stable_sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.start < b.start; });
}

void SoundMarks::shift(long at, long removed, long inserted) {
    if (removed == inserted) return;                     // processed in place: nothing moves
    const long end = at + removed, d = inserted - removed;
    // a frame of the old sound in the new: after the part replaced, moved by the difference; inside it, scaled
    // to what replaced it (at its start, when it was deleted)
    auto inside = [&](long f) { return inserted > 0 && removed > 0 ? at + (long)((double)(f - at) * inserted / removed) : at; };
    auto start = [&](long f) { return f >= end ? f + d : f <= at ? f : inside(f); };
    auto stop = [&](long f) { return f > end || (f == end && removed > 0) ? f + d : f <= at ? f : inside(f); };
    for (auto& m : markers) m.at = start(m.at);
    std::vector<Region> kept;
    for (auto r : regions) {
        long s = start(r.start), e = stop(r.end);
        if (e <= s) continue;                            // all of it deleted
        r.start = s;
        r.end = e;
        kept.push_back(std::move(r));
    }
    regions = std::move(kept);
    sort();
}

void SoundMarks::clamp(long frames) {
    for (auto& m : markers) m.at = std::clamp(m.at, 0L, frames);
    std::vector<Region> kept;
    for (auto& r : regions) {
        Region q = r;
        q.start = std::clamp(q.start, 0L, frames);
        q.end = std::clamp(q.end, 0L, frames);
        if (q.end > q.start) kept.push_back(std::move(q));
    }
    regions = std::move(kept);
}

bool SoundMarks::operator==(const SoundMarks& o) const {
    if (markers.size() != o.markers.size() || regions.size() != o.regions.size()) return false;
    for (size_t k = 0; k < markers.size(); ++k) {
        const Marker &a = markers[k], &b = o.markers[k];
        if (a.at != b.at || a.name != b.name || a.source != b.source) return false;
    }
    for (size_t k = 0; k < regions.size(); ++k) {
        const Region &a = regions[k], &b = o.regions[k];
        if (a.start != b.start || a.end != b.end || a.name != b.name || a.type != b.type || a.source != b.source ||
            std::fabs(a.energy - b.energy) > 1e-6 || std::fabs(a.pitch - b.pitch) > 1e-6)
            return false;
    }
    return true;
}

std::string typeName(int type) {
    if (type < 0) return "-";
    if (type < 26) return std::string(1, (char)('A' + type));
    return "T" + std::to_string(type);
}

// ---------------------------------------------------------------- the text

namespace {

std::string num(double v, const char* fmt = "%.6f") {
    char b[64];
    std::snprintf(b, sizeof b, fmt, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {               // 1.500000 -> 1.5, 2.000000 -> 2
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        o += c;
    }
    return o + "\"";
}

// a word (a source): letters, digits and - only, else quoted
std::string word(const std::string& s) {
    bool plain = !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum((unsigned char)c) || c == '-' || c == '_';
    }) && !std::isdigit((unsigned char)s[0]);
    return plain ? s : quoted(s);
}

// the deepest nesting of parentheses outside strings and comments (the parser keeps nested forms on the heap,
// but they are freed recursively: a file is refused before it gets that deep)
int nesting(const std::string& t) {
    int depth = 0, most = 0;
    bool str = false, comment = false;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (comment) { if (c == '\n') comment = false; continue; }
        if (str) {
            if (c == '\\') ++i;
            else if (c == '"') str = false;
            continue;
        }
        if (c == ';') comment = true;
        else if (c == '"') str = true;
        else if (c == '(') most = std::max(most, ++depth);
        else if (c == ')') --depth;
    }
    return most;
}

struct Reader {
    std::string err;
    bool fail(const SExpr& at, const std::string& m) {
        if (err.empty()) err = "line " + std::to_string(at.line) + ": " + m;
        return false;
    }
    bool number(const SExpr& e, const char* what, double lo, double hi, double& out) {
        if (e.isList || e.quoted || e.text.empty()) return fail(e, std::string(what) + ": a number is wanted");
        char* end = nullptr;
        double v = std::strtod(e.text.c_str(), &end);
        if (!end || *end) return fail(e, std::string(what) + ": a number is wanted, not " + e.text);
        if (!std::isfinite(v) || v < lo || v > hi) return fail(e, std::string(what) + ": " + e.text + " is out of range");
        out = v;
        return true;
    }
    bool text(const SExpr& e, const char* what, std::string& out) {
        if (e.isList) return fail(e, std::string(what) + ": a word or a string is wanted");
        if (e.text.size() > 1000) return fail(e, std::string(what) + ": too long");
        out = e.text;
        return true;
    }
    static bool head(const SExpr& e, const char* h) {
        return e.isList && !e.items.empty() && !e.items[0].isList && !e.items[0].quoted && e.items[0].text == h;
    }
};

constexpr double kMaxSeconds = 1e7;                     // about 4 months

}  // namespace

std::string marksPath(const std::string& soundPath) { return soundPath + ".markers"; }

std::string marksToText(const SoundMarks& m, double sr, double duration, const std::string& soundName) {
    if (!(sr > 0)) sr = 44100;
    std::string o = "; markers and regions of " + soundName +
                    " (Musil data: read by Moritz's sound editor, or by a script with (parse (read path)))\n";
    o += "; (marker seconds name source), (region start end name (type n) (energy rms) (pitch midi) (source s))\n";
    o += "(sound-marks (version 1) (duration " + num(duration) + ")";
    for (auto& k : m.markers) o += "\n  (marker " + num(k.at / sr) + " " + quoted(k.name) + " " + word(k.source) + ")";
    for (auto& r : m.regions) {
        o += "\n  (region " + num(r.start / sr) + " " + num(r.end / sr) + " " + quoted(r.name);
        if (r.type >= 0) o += " (type " + std::to_string(r.type) + ")";
        o += " (energy " + num(r.energy, "%.4f") + ")";
        if (r.pitch > 0) o += " (pitch " + num(r.pitch, "%.2f") + ")";
        o += " (source " + word(r.source) + "))";
    }
    return o + ")\n";
}

bool marksFromText(const std::string& text, double sr, long frames, SoundMarks& out, std::string& err) {
    if (text.size() > kMaxMarksFile) { err = "larger than " + std::to_string(kMaxMarksFile >> 20) + " MB"; return false; }
    if (nesting(text) > 8) { err = "nested too deep to be a markers file"; return false; }
    if (!(sr > 0)) sr = 44100;
    std::vector<SExpr> forms;
    if (!parseSExprs(text, forms, err)) return false;
    const SExpr* top = nullptr;
    for (auto& f : forms)
        if (Reader::head(f, "sound-marks")) { top = &f; break; }
    if (!top) { err = "no (sound-marks ...) in it"; return false; }
    Reader R;
    SoundMarks m;
    auto frame = [&](double s) { return std::clamp((long)std::llround(s * sr), 0L, frames); };
    for (size_t k = 1; k < top->items.size(); ++k) {
        const SExpr& e = top->items[k];
        if (!e.isList || e.items.empty()) { R.fail(e, "a form (marker ...) or (region ...) is wanted"); break; }
        if (m.markers.size() >= kMaxMarks || m.regions.size() >= kMaxMarks) { R.fail(e, "too many markers or regions"); break; }
        if (Reader::head(e, "marker")) {
            if (e.items.size() < 2 || e.items.size() > 4) { R.fail(e, "a marker is (marker seconds name [source])"); break; }
            Marker mk;
            double t;
            if (!R.number(e.items[1], "marker", 0, kMaxSeconds, t)) break;
            mk.at = frame(t);
            if (e.items.size() > 2 && !R.text(e.items[2], "marker name", mk.name)) break;
            if (e.items.size() > 3 && !R.text(e.items[3], "marker source", mk.source)) break;
            m.markers.push_back(std::move(mk));
        } else if (Reader::head(e, "region")) {
            if (e.items.size() < 3) { R.fail(e, "a region is (region start end [name] ...)"); break; }
            Region r;
            double a, b;
            if (!R.number(e.items[1], "region start", 0, kMaxSeconds, a) || !R.number(e.items[2], "region end", 0, kMaxSeconds, b)) break;
            if (b <= a) { R.fail(e, "a region ends after it starts"); break; }
            r.start = frame(a);
            r.end = frame(b);
            size_t j = 3;
            if (j < e.items.size() && !e.items[j].isList) {
                if (!R.text(e.items[j], "region name", r.name)) break;
                ++j;
            }
            bool bad = false;
            for (; j < e.items.size() && !bad; ++j) {
                const SExpr& f = e.items[j];
                double v;
                if (Reader::head(f, "type") && f.items.size() == 2) {
                    bad = !R.number(f.items[1], "type", -1, 4096, v);
                    r.type = (int)v;
                } else if (Reader::head(f, "energy") && f.items.size() == 2) {
                    bad = !R.number(f.items[1], "energy", 0, 1e6, r.energy);
                } else if (Reader::head(f, "pitch") && f.items.size() == 2) {
                    bad = !R.number(f.items[1], "pitch", 0, 200, r.pitch);
                } else if (Reader::head(f, "source") && f.items.size() == 2) {
                    bad = !R.text(f.items[1], "source", r.source);
                } else if (!f.isList) {
                    bad = !R.fail(f, "unexpected " + f.text + " in a region");
                }                                         // another (key value): a later version's, left out
            }
            if (bad) break;
            if (r.end > r.start) m.regions.push_back(std::move(r));
        } else if (Reader::head(e, "version") || Reader::head(e, "duration")) {
            continue;
        }                                                 // another form: a later version's, left out
    }
    if (!R.err.empty()) { err = R.err; return false; }
    m.sort();
    out = std::move(m);
    return true;
}

bool readMarksFor(const std::string& soundPath, double sr, long frames, SoundMarks& out, std::string& err) {
    std::string p = marksPath(soundPath), text;
    out = SoundMarks();
    if (!fileio::isFile(p)) return true;
    if (!fileio::read(p, text)) { err = "can't read " + p; return false; }
    if (!marksFromText(text, sr, frames, out, err)) { err = p + ": " + err; return false; }
    return true;
}

bool writeMarksFor(const std::string& soundPath, const SoundMarks& m, double sr, double duration, std::string& err) {
    std::string p = marksPath(soundPath);
    if (m.empty()) {                                      // nothing to keep: no file (an old one goes)
        if (fileio::isFile(p) && !fileio::remove(p)) { err = "can't remove " + p; return false; }
        return true;
    }
    size_t slash = soundPath.find_last_of("/\\");
    std::string name = slash == std::string::npos ? soundPath : soundPath.substr(slash + 1);
    return fileio::writeAtomic(p, marksToText(m, sr, duration, name), &err);
}

// ---------------------------------------------------------------- score

std::string scoreText(const SoundMarks& m, double sr, const std::string& source, bool buffer, const std::string& name) {
    namespace sc = musil::score;
    if (!(sr > 0)) sr = 44100;
    sc::Score s(name, sr);
    s.rows.push_back({"regions", "", {}});
    for (auto& r : m.regions) {
        auto e = std::make_shared<sc::Event>();
        e->kind = sc::Kind::Sound;
        e->at = r.start / sr;
        e->dur = (r.end - r.start) / sr;
        e->row = "regions";
        e->source = source;
        e->buffer = buffer;
        e->from = r.start / sr;
        e->to = r.end / sr;
        e->label = r.name;
        if (r.type >= 0) e->extra.push_back({"type", sc::Datum::string(typeName(r.type))});
        e->extra.push_back({"energy", sc::Datum::number(std::round(r.energy * 1e4) / 1e4)});
        if (r.pitch > 0) e->extra.push_back({"pitch", sc::Datum::number(std::round(r.pitch * 100) / 100)});
        s.insert(e);
    }
    return sc::toText(s);
}

// ---------------------------------------------------------------- coll

std::string collText(const SoundMarks& m, double sr, const std::string& soundName) {
    if (!(sr > 0)) sr = 44100;
    std::string o = "; the regions of " + soundName + ", a row each (Musil data, a list of lists: Moritz's coll reads it,\n"
                    "; a script with (parse (read path))); seconds, the soundtypes type, RMS energy, MIDI pitch (0: none)\n";
    o += "(columns index start end type energy pitch name)\n";
    int k = 1;
    for (auto& r : m.regions)
        o += "(" + std::to_string(k++) + " " + num(r.start / sr) + " " + num(r.end / sr) + " " + quoted(typeName(r.type)) + " " +
             num(r.energy, "%.4f") + " " + num(r.pitch, "%.2f") + " " + quoted(r.name) + ")\n";
    return o;
}

bool readCollRows(const std::string& text, std::vector<std::string>& columns, std::vector<CollRow>& rows, std::string& err) {
    if (text.size() > kMaxMarksFile * 4) { err = "too large for a coll"; return false; }
    if (nesting(text) > 1) { err = "a coll file is rows of values, (1 0.5 \"a\") ...: nothing nested"; return false; }
    std::vector<SExpr> forms;
    if (!parseSExprs(text, forms, err)) return false;
    columns.clear();
    rows.clear();
    for (auto& f : forms) {
        if (!f.isList) { err = "line " + std::to_string(f.line) + ": a row is a list, (1 0.5 \"a\")"; return false; }
        if (Reader::head(f, "columns")) {
            for (size_t k = 1; k < f.items.size(); ++k) columns.push_back(f.items[k].text);
            continue;
        }
        CollRow r;
        for (auto& c : f.items) r.cells.push_back(c.text);
        rows.push_back(std::move(r));
    }
    return true;
}

}  // namespace moritz
