// db_query.h — choosing the samples of a database by a query: one small language, the same for Musil's db-select and
// the orchestrators' 'select, and for Moritz's orchestra (select).
//
// A query is terms joined by and, or, not and parentheses; two terms side by side are joined by and. A term is a field
// and a value:
//   instrument Vn|Va          (instr) the instrument's code as the file names say it (Vn, ClBb, Vn+S...)
//   family strings            the instrument's family: winds brass percussion keyboard plucked strings other
//   technique pizz*           (tech, style) the playing technique (ord, pizz-lv, sul_pont...)
//   dynamics p..f             (dyn) the dynamics; a range in the order ppp pp p mp mf f ff fff
//   pitch C4..G5              (note) the pitch, by name or MIDI number: pitch 60..72, pitch C#4; an unpitched sound
//                             has none (pitch N matches it by name)
//   midi 60..72               the MIDI pitch as a number
//   string 1..2               the string a sound is played on (the tag 1c, 2c... of the file name)
//   mute S                    the mute: what follows + in the instrument's code (Vn+S: S); none without one
//   tag 4c                    (other) any of the other tags of the file name (after the dynamics)
//   file Vn-ord-*             the file's name, without its folders
//   centroid 500..2000        spread, skewness, kurtosis: the features of a database of moments (db-gen ... moments)
// and, in Musil, any numeric field of the entries (put! e 'loudness ...). A value is a pattern with * and ? (any text,
// any one character; letters' case ignored), a regular expression between slashes (/sul_?pont/, searched in the text,
// case ignored), a range lo..hi for the pitch, MIDI, dynamics, string and the numeric fields (an end may be left out:
// C4.. or ..G5), or several of them separated by | (any of them). Examples:
//   technique ord and dynamics p..f
//   instrument Vn|Va and not technique pizz*
//   (family strings and pitch ..C4) or (family brass and dynamics ff|fff)
// Parsing never trusts its input: the text, the tokens, the nesting and the patterns are bounded; a mistake throws
// dbq::query_error with a message that says what and where. Matching never throws.
//
// Uses music.h's pitch_to_midi (it is included from music.h, after it) and score_layout.h's orchestral order.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause

#pragma once
#include "score_layout.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace musil {
namespace dbq {

// the dynamics in their order
inline const std::vector<std::string>& dynamics_scale() { static const std::vector<std::string> s = {"ppp", "pp", "p", "mp", "mf", "f", "ff", "fff"}; return s; }
// (instrument-family instr): winds brass percussion keyboard plucked strings, other
inline std::string instrument_family(const std::string& instr) {
    const auto& o = score::layout::orchestralOrder();
    auto it = std::find(o.begin(), o.end(), instr);
    if (it == o.end()) return "other";
    long k = it - o.begin();
    if (k < 17) return "winds";
    if (k < 23) return "brass";
    if (k < 25) return "percussion";
    if (instr == "Hp" || instr == "Gtr") return "plucked";
    if (k < 29) return "keyboard";
    return "strings";
}

struct query_error : std::runtime_error { using std::runtime_error::runtime_error; };

// what a query sees of a sample: the fields of its file's name, its MIDI pitch (-1: none), the moments when the
// database has them, and other numeric fields (a Musil entry's) through `number` (may be empty)
struct sample {
    std::string instr, tech, pitch, dyn, other, file;
    int midi = -1;
    bool has_moments = false; double moments[4] = {0, 0, 0, 0};
    std::function<bool(const std::string&, double&)> number;
};

// what the parser knows of the database: its features' type (moments gives centroid, spread, skewness, kurtosis) and,
// for Musil's entries, which other names are numeric fields
struct options {
    std::string type;
    std::function<bool(const std::string&)> numeric_field;
};

enum class field { instr, family, tech, dyn, pitch, midi, string, mute, tag, file, moment, extra };

// one alternative of a value
struct alt {
    enum kind { glob, regex, range, number } k = glob;
    std::string text;                                     // the pattern (glob, lowercased)
    std::shared_ptr<const std::regex> re;
    double lo = -HUGE_VAL, hi = HUGE_VAL;                 // range, number (lo)
};
struct node {
    enum kind { term, all_of, any_of, negation } k = term;
    field f = field::instr; int moment = 0; std::string name;   // the field (name: an extra's)
    std::vector<alt> alts;
    std::vector<std::shared_ptr<const node>> kids;
};
using query = std::shared_ptr<const node>;               // nullptr: every sample

// --- the parts ----------------------------------------------------------------------------------------------------
inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// a pattern with * and ? against a text, case ignored (the iterative matcher: linear backtracking on the last *)
inline bool glob_match(const std::string& pat, const std::string& txt) {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < txt.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::tolower((unsigned char)pat[p]) == std::tolower((unsigned char)txt[t]))) { p++; t++; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}
// the tags of a sample's other field (dash separated; N: none)
inline std::vector<std::string> tags_of(const std::string& other) {
    std::vector<std::string> out; if (other.empty() || other == "N") return out;
    std::string cur; for (char c : other) { if (c == '-') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
// the string a sample is played on: a tag of digits followed by c (4c); -1 when none
inline int string_of(const std::string& other) {
    for (auto& t : tags_of(other)) {
        if (t.size() < 2 || t.size() > 3 || (t.back() != 'c' && t.back() != 'C')) continue;
        bool digits = true; for (size_t k = 0; k + 1 < t.size(); k++) digits = digits && std::isdigit((unsigned char)t[k]);
        if (digits) return std::atoi(t.c_str());
    }
    return -1;
}
inline std::string mute_of(const std::string& instr) { size_t p = instr.find('+'); return p == std::string::npos ? "none" : instr.substr(p + 1); }
inline std::string base_name(const std::string& file) { size_t p = file.find_last_of("/\\"); return p == std::string::npos ? file : file.substr(p + 1); }
inline int dynamics_rank(const std::string& d) {
    const auto& s = dynamics_scale(); std::string l = lower(d);
    for (size_t k = 0; k < s.size(); k++) if (s[k] == l) return (int)k;
    return -1;
}
inline bool number_of(const std::string& s, double& v) {
    if (s.empty() || s.size() > 64) return false;
    char* end = nullptr; v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(v);
}
inline std::string number_text(double v) { char b[64]; std::snprintf(b, sizeof b, "%.10g", v); return b; }

// the fields a query knows by name (the numeric fields of Musil's entries aside)
inline std::string fields_text(bool moments) {
    std::string s = "instrument, family, technique, dynamics, pitch, midi, string, mute, tag, file";
    return moments ? s + ", centroid, spread, skewness, kurtosis" : s;
}
// does a query begin with this word: a field's name, not, or a parenthesis (else it may be an instrument's name, as
// Moritz's select Vn technique pizz* has it)
inline bool starts_query(const std::string& word) {
    if (word.empty()) return false;
    if (word[0] == '(') return true;
    std::string w = lower(word);
    static const char* const names[] = {"instrument", "instr", "inst", "family", "technique", "tech", "style", "dynamics", "dyn", "dynamic", "pitch", "note",
                                        "midi", "string", "mute", "tag", "other", "file", "centroid", "spread", "skewness", "kurtosis", "not"};
    for (auto n : names) if (w == n) return true;
    return false;
}

// --- the parser -----------------------------------------------------------------------------------------------------
namespace detail {
struct token { enum kind { word, open, close } k = word; std::vector<std::pair<bool, std::string>> parts; std::string raw; size_t at = 0; };

constexpr size_t kMaxText = 8192, kMaxTokens = 2048, kMaxAlts = 256, kMaxPattern = 256;
constexpr int kMaxDepth = 64;

inline std::vector<token> tokenize(const std::string& s) {
    if (s.size() > kMaxText) throw query_error("the query is too long (" + std::to_string(s.size()) + " characters; at most " + std::to_string(kMaxText) + ")");
    std::vector<token> out; size_t p = 0;
    while (p < s.size()) {
        unsigned char c = (unsigned char)s[p];
        if (std::isspace(c) || c == ',') { p++; continue; }
        if (out.size() >= kMaxTokens) throw query_error("the query has too many words");
        token t; t.at = p;
        if (c == '(') { t.k = token::open; t.raw = "("; out.push_back(t); p++; continue; }
        if (c == ')') { t.k = token::close; t.raw = ")"; out.push_back(t); p++; continue; }
        std::string cur; bool cur_re = false, fresh = true;          // fresh: at the start of an alternative
        auto close_alt = [&]() {
            if (cur.empty() && !cur_re) throw query_error("an empty alternative in '" + s.substr(t.at, p - t.at + 1) + "'");
            if (t.parts.size() >= kMaxAlts) throw query_error("too many alternatives (at most " + std::to_string(kMaxAlts) + ")");
            t.parts.push_back({cur_re, cur}); cur.clear(); cur_re = false; fresh = true;
        };
        while (p < s.size()) {
            unsigned char d = (unsigned char)s[p];
            if (std::isspace(d) || d == '(' || d == ')' || d == ',') break;
            if (d == '|') { close_alt(); p++; continue; }
            if (d == '/' && fresh) {                                  // a regular expression, to its closing slash
                size_t q = p + 1; std::string re;
                while (q < s.size() && s[q] != '/') { if (s[q] == '\\' && q + 1 < s.size()) { if (s[q + 1] != '/') re += '\\'; re += s[q + 1]; q += 2; continue; } re += s[q++]; }
                if (q >= s.size()) throw query_error("a regular expression without its closing slash: " + s.substr(p, std::min<size_t>(40, s.size() - p)));
                if (re.empty()) throw query_error("an empty regular expression //");
                cur = re; cur_re = true; fresh = false; p = q + 1;
                if (p < s.size() && !std::isspace((unsigned char)s[p]) && s[p] != '|' && s[p] != ')' && s[p] != '(' && s[p] != ',')
                    throw query_error("text after the regular expression /" + re + "/");
                continue;
            }
            if (cur_re) throw query_error("text after a regular expression");
            cur += (char)d; fresh = false; p++;
        }
        if (!t.parts.empty() || !cur.empty() || cur_re) {
            if (fresh && !t.parts.empty()) throw query_error("an empty alternative at the end of '" + s.substr(t.at, p - t.at) + "'");
            if (!fresh) close_alt();
        }
        t.raw = s.substr(t.at, p - t.at);
        out.push_back(std::move(t));
    }
    return out;
}

inline bool keyword(const token& t, const char* k) { return t.k == token::word && t.parts.size() == 1 && !t.parts[0].first && lower(t.parts[0].second) == k; }

struct parser {
    const std::vector<token>& toks; const options& opt; size_t pos = 0; int depth = 0;
    parser(const std::vector<token>& t, const options& o) : toks(t), opt(o) {}

    bool at_end() const { return pos >= toks.size(); }
    std::string near() const { return at_end() ? "at the end" : "at '" + toks[pos].raw + "'"; }

    query expr() {
        if (++depth > kMaxDepth) throw query_error("the query is nested too deeply (at most " + std::to_string(kMaxDepth) + " levels)");
        auto n = std::make_shared<node>(); n->k = node::any_of;
        n->kids.push_back(conj());
        while (!at_end() && keyword(toks[pos], "or")) { pos++; n->kids.push_back(conj()); }
        depth--;
        if (n->kids.size() == 1) return n->kids[0];
        return n;
    }
    query conj() {
        auto n = std::make_shared<node>(); n->k = node::all_of;
        n->kids.push_back(unary());
        while (!at_end() && toks[pos].k != token::close && !keyword(toks[pos], "or")) {
            if (keyword(toks[pos], "and")) pos++;
            n->kids.push_back(unary());
        }
        if (n->kids.size() == 1) return n->kids[0];
        return n;
    }
    query unary() {
        if (at_end()) throw query_error("a term is missing at the end (a field and a value: technique ord)");
        const token& t = toks[pos];
        if (keyword(t, "not")) {
            pos++;
            if (++depth > kMaxDepth) throw query_error("the query is nested too deeply (at most " + std::to_string(kMaxDepth) + " levels)");
            auto n = std::make_shared<node>(); n->k = node::negation; n->kids.push_back(unary());
            depth--;
            return n;
        }
        if (t.k == token::open) {
            pos++;
            query q = expr();
            if (at_end() || toks[pos].k != token::close) throw query_error("a ( without its ) " + near());
            pos++;
            return q;
        }
        if (t.k == token::close) throw query_error("a ) without its (");
        if (keyword(t, "and") || keyword(t, "or")) throw query_error("'" + t.raw + "' needs a term before it");
        return term();
    }
    query term() {
        const token& t = toks[pos++];
        if (t.parts.size() != 1 || t.parts[0].first) throw query_error("'" + t.raw + "' is not a field (" + fields_known() + ")");
        auto n = std::make_shared<node>(); n->k = node::term;
        std::string f = lower(t.parts[0].second);
        if (f == "instrument" || f == "instr" || f == "inst") n->f = field::instr;
        else if (f == "family") n->f = field::family;
        else if (f == "technique" || f == "tech" || f == "style") n->f = field::tech;
        else if (f == "dynamics" || f == "dyn" || f == "dynamic") n->f = field::dyn;
        else if (f == "pitch" || f == "note") n->f = field::pitch;
        else if (f == "midi") n->f = field::midi;
        else if (f == "string") n->f = field::string;
        else if (f == "mute") n->f = field::mute;
        else if (f == "tag" || f == "other") n->f = field::tag;
        else if (f == "file") n->f = field::file;
        else if (f == "centroid" || f == "spread" || f == "skewness" || f == "kurtosis") {
            if (opt.type != "moments") throw query_error(f + ": the database's features are " + (opt.type.empty() ? "not known" : opt.type) + ", not moments");
            n->f = field::moment; n->moment = f == "centroid" ? 0 : f == "spread" ? 1 : f == "skewness" ? 2 : 3;
        } else if (opt.numeric_field && opt.numeric_field(t.parts[0].second)) { n->f = field::extra; n->name = t.parts[0].second; }
        else throw query_error("'" + t.raw + "' is not a field (" + fields_known() + ")");
        const std::string fname = t.parts[0].second;
        if (at_end() || toks[pos].k != token::word || keyword(toks[pos], "and") || keyword(toks[pos], "or") || keyword(toks[pos], "not"))
            throw query_error(fname + ": a value is missing " + near());
        const token& v = toks[pos++];
        for (auto& part : v.parts) n->alts.push_back(value(n->f, fname, part.first, part.second));
        return n;
    }
    std::string fields_known() const { return "the fields are " + fields_text(opt.type == "moments") + (opt.numeric_field ? " and the entries' numeric fields" : ""); }
    // a group that is repeated (followed by * + or {) and holds a repetition or an alternative: the patterns whose
    // backtracking grows exponentially with the text ((a+)+$, (a|aa)*), refused so that a query never hangs
    static bool nested_repetition(const std::string& re) {
        std::vector<bool> inner;                                     // per open group: holds a quantifier or a |
        for (size_t k = 0; k < re.size(); k++) {
            char c = re[k];
            if (c == '\\') { k++; continue; }
            if (c == '[') { k++; if (k < re.size() && re[k] == '^') k++; if (k < re.size() && re[k] == ']') k++; while (k < re.size() && re[k] != ']') { if (re[k] == '\\') k++; k++; } continue; }
            if (c == '(') { inner.push_back(false); if (k + 2 < re.size() && re[k + 1] == '?') k += 2; continue; }   // (?: (?= (?!
            if (c == '*' || c == '+' || c == '?' || c == '{' || c == '|') { if (!inner.empty()) inner.back() = true; continue; }
            if (c == ')') {
                bool held = !inner.empty() && inner.back();
                if (!inner.empty()) inner.pop_back();
                char next = k + 1 < re.size() ? re[k + 1] : '\0';
                bool repeated = next == '*' || next == '+' || next == '{';
                if (held && repeated) return true;
                if (held && !inner.empty()) inner.back() = true;       // the outer group holds what the inner held
            }
        }
        return false;
    }
    static bool numeric(field f) { return f == field::pitch || f == field::midi || f == field::string || f == field::moment || f == field::extra; }
    // an end of a range or a number: a pitch's name for the pitch and MIDI, a dynamics' rank, else a number
    double bound(field f, const std::string& fname, const std::string& s) {
        double v = 0;
        if (f == field::dyn) { int r = dynamics_rank(s); if (r < 0) throw query_error("dynamics: '" + s + "' is not one of ppp pp p mp mf f ff fff"); return r; }
        if ((f == field::pitch || f == field::midi) && !number_of(s, v)) { int m = pitch_to_midi(s); if (m < 0) throw query_error(fname + ": '" + s + "' is neither a pitch (C4, F#5) nor a MIDI number"); return m; }
        if (!number_of(s, v)) throw query_error(fname + ": '" + s + "' is not a number");
        return v;
    }
    alt value(field f, const std::string& fname, bool is_re, const std::string& s) {
        alt a;
        if (is_re) {
            if (s.size() > kMaxPattern) throw query_error("a regular expression of " + std::to_string(s.size()) + " characters (at most " + std::to_string(kMaxPattern) + ")");
            a.k = alt::regex; a.text = s;
            size_t repeats = 0;
            for (size_t k = 0; k < s.size(); k++) { if (s[k] == '\\') { k++; continue; } if (s[k] == '*' || s[k] == '+' || s[k] == '{') repeats++; }
            if (repeats > 8) throw query_error("the regular expression /" + s + "/ repeats more than 8 times: its matching could take forever");
            if (nested_repetition(s)) throw query_error("the regular expression /" + s + "/ repeats a group that repeats or has alternatives ((a+)+, (a|b)*): its matching could take forever");
            try { a.re = std::make_shared<const std::regex>(s, std::regex::ECMAScript | std::regex::icase); }
            catch (const std::exception& e) { throw query_error("a bad regular expression /" + s + "/: " + e.what()); }
            return a;
        }
        if (s.size() > kMaxPattern) throw query_error(fname + ": a value of " + std::to_string(s.size()) + " characters (at most " + std::to_string(kMaxPattern) + ")");
        size_t dots = s.find("..");
        if (dots != std::string::npos) {
            if (!(numeric(f) || f == field::dyn)) throw query_error(fname + ": a range (" + s + ") is for pitch, midi, dynamics, string and the numeric features");
            std::string l = s.substr(0, dots), h = s.substr(dots + 2);
            if (l.empty() && h.empty()) throw query_error(fname + ": a range needs an end: " + s);
            a.k = alt::range;
            if (!l.empty()) a.lo = bound(f, fname, l);
            if (!h.empty()) a.hi = bound(f, fname, h);
            if (a.lo > a.hi) std::swap(a.lo, a.hi);
            return a;
        }
        bool wild = s.find_first_of("*?") != std::string::npos;
        if (!wild && numeric(f)) {
            double v = 0;
            if (number_of(s, v)) { a.k = alt::number; a.lo = v; return a; }
            if (f == field::pitch || f == field::midi) { int m = pitch_to_midi(s); if (m >= 0) { a.k = alt::number; a.lo = m; return a; } }
            if (f != field::pitch) throw query_error(fname + ": '" + s + "' is not a number");
        }
        a.k = alt::glob; a.text = s;
        return a;
    }
};
}  // namespace detail

// the query compiled; nullptr for a blank text (every sample). Throws query_error
inline query compile(const std::string& text, const options& o = {}) {
    auto toks = detail::tokenize(text);
    if (toks.empty()) return nullptr;
    detail::parser p(toks, o);
    query q = p.expr();
    if (!p.at_end()) throw query_error(p.toks[p.pos].k == detail::token::close ? "a ) without its (" : "unexpected '" + p.toks[p.pos].raw + "'");
    return q;
}

// --- matching -------------------------------------------------------------------------------------------------------
namespace detail {
inline bool text_match(const alt& a, const std::string& s) {
    if (a.k == alt::glob) return glob_match(a.text, s);
    if (a.k == alt::regex) { if (s.size() > 1024) return false; try { return std::regex_search(s, *a.re); } catch (...) { return false; } }
    return false;
}
inline bool term_match(const node& n, const sample& s) {
    double v = 0; bool has_num = false; std::string text; std::vector<std::string> many;
    switch (n.f) {
    case field::instr: text = s.instr; break;
    case field::family: { std::string i = s.instr; size_t p = i.find('+'); if (p != std::string::npos) i = i.substr(0, p); text = instrument_family(i); break; }
    case field::tech: text = s.tech; break;
    case field::dyn: { text = s.dyn; int r = dynamics_rank(s.dyn); if (r >= 0) { v = r; has_num = true; } break; }
    case field::pitch: text = s.pitch; if (s.midi >= 0) { v = s.midi; has_num = true; } break;
    case field::midi: if (s.midi >= 0) { v = s.midi; has_num = true; text = std::to_string(s.midi); } break;
    case field::string: { int k = string_of(s.other); if (k >= 0) { v = k; has_num = true; text = std::to_string(k); } break; }
    case field::mute: text = mute_of(s.instr); break;
    case field::tag: many = tags_of(s.other); break;
    case field::file: text = base_name(s.file); break;
    case field::moment: if (s.has_moments && std::isfinite(s.moments[n.moment])) { v = s.moments[n.moment]; has_num = true; text = number_text(v); } break;
    case field::extra: if (s.number && s.number(n.name, v) && std::isfinite(v)) { has_num = true; text = number_text(v); } break;
    }
    for (const alt& a : n.alts) {
        if (a.k == alt::range) { if (has_num && v >= a.lo && v <= a.hi) return true; continue; }
        if (a.k == alt::number) { if (has_num && v == a.lo) return true; continue; }
        if (n.f == field::tag) { for (auto& t : many) if (text_match(a, t)) return true; continue; }
        if (n.f == field::dyn && a.k == alt::glob) { if (glob_match(a.text, text)) return true; continue; }
        if ((n.f == field::midi || n.f == field::string || n.f == field::moment || n.f == field::extra) && !has_num) continue;
        if (text_match(a, text)) return true;
    }
    return false;
}
}  // namespace detail
inline bool matches(const node& n, const sample& s) {
    switch (n.k) {
    case node::term: return detail::term_match(n, s);
    case node::all_of: for (auto& k : n.kids) if (!matches(*k, s)) return false; return true;
    case node::any_of: for (auto& k : n.kids) if (matches(*k, s)) return true; return false;
    case node::negation: return !matches(*n.kids[0], s);
    }
    return false;
}
inline bool matches(const query& q, const sample& s) { return !q || matches(*q, s); }

// --- a selection: a query for every sample and one per instrument (applied to that instrument's samples only) ------
struct selection {
    query all; std::string all_text;
    std::map<std::string, std::pair<query, std::string>> per;      // instrument -> its query and its text
    bool empty() const { return !all && per.empty(); }
    bool matches(const sample& s) const {
        if (all && !dbq::matches(*all, s)) return false;
        auto it = per.find(s.instr);
        return it == per.end() || !it->second.first || dbq::matches(*it->second.first, s);
    }
    // the selection as it would be typed: "dynamics p..f; Vn: technique pizz*"
    std::string text() const {
        std::string t = all ? all_text : "";
        for (auto& kv : per) t += (t.empty() ? "" : "; ") + kv.first + ": " + kv.second.second;
        return t.empty() ? "every sound" : t;
    }
};

// how many samples a selection keeps, in all and per instrument
struct counts {
    struct row { std::string name; size_t kept = 0, total = 0; };
    size_t kept = 0, total = 0;
    std::map<std::string, std::pair<size_t, size_t>> by;            // instrument -> kept, of
    void add(const std::string& instr, bool keep) { total++; auto& c = by[instr]; c.second++; if (keep) { kept++; c.first++; } }
    // the instruments in orchestral order, each once: every one counted, or those of `only` when it is not empty
    std::vector<row> rows(const std::vector<std::string>& only = {}) const {
        std::vector<std::string> names;
        if (only.empty()) for (auto& kv : by) names.push_back(kv.first);
        else for (auto& i : only) if (std::find(names.begin(), names.end(), i) == names.end()) names.push_back(i);
        const auto& order = score::layout::orchestralOrder();
        auto rank = [&](const std::string& i) { auto it = std::find(order.begin(), order.end(), i); return it == order.end() ? (long)order.size() : (long)(it - order.begin()); };
        std::stable_sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) { long ra = rank(a), rb = rank(b); return ra != rb ? ra < rb : a < b; });
        std::vector<row> out;
        for (auto& i : names) { auto it = by.find(i); row r; r.name = i; if (it != by.end()) { r.kept = it->second.first; r.total = it->second.second; } out.push_back(r); }
        return out;
    }
    // "24 of 32 sounds: Ob 0 of 8, Hn 8, Vn 8, Vc 8": an instrument whose sounds all remain says only how many
    std::string text(const std::vector<std::string>& only = {}) const {
        std::string t = std::to_string(kept) + " of " + std::to_string(total) + (total == 1 ? " sound" : " sounds"), list;
        for (auto& r : rows(only)) list += (list.empty() ? "" : ", ") + r.name + " " + std::to_string(r.kept) + (r.kept == r.total ? "" : " of " + std::to_string(r.total));
        return list.empty() ? t : t + ": " + list;
    }
    // the instruments (of `only`, or every one) that keep no sample
    std::vector<std::string> silent(const std::vector<std::string>& only = {}) const {
        std::vector<std::string> out;
        for (auto& r : rows(only)) if (r.kept == 0) out.push_back(r.name);
        return out;
    }
};

}  // namespace dbq
}  // namespace musil
