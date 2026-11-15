// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// maximport.cpp — File > Import from Max: .maxpat (JSON) to Moritz patches
//
// Boxes keep their class, arguments, position and cables. Interface objects
// are mapped (slider range, kslider keys, flonum ...), subpatchers come along
// inline, and abstractions found next to the Max patch (poly~ voices, pfft~
// patches, objects named after a .maxpat file) are converted too, into
// .moritz files beside them. What has no equivalent stays as an unknown box
// with its text, and is listed in the report.

#include "maximport.h"

#include "json.h"
#include "moritz.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace moritz {

namespace {

using json::Value;

// Max writes symbols with spaces in double quotes: "hello world"
std::vector<std::string> maxTokens(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) {
            if (c == '\\' && i + 1 < text.size()) { cur += text[++i]; continue; }
            if (c == '"') { quoted = false; continue; }
            cur += c;
            continue;
        }
        if (c == '"') { quoted = true; any = true; continue; }
        if (std::isspace((unsigned char)c)) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

std::string quote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        if (c == '\n') { r += "\\n"; continue; }
        r += c;
    }
    return r + "\"";
}

// an argument as Moritz writes it: numbers bare, words quoted when needed
std::string arg(const std::string& t) {
    if (t.empty()) return "\"\"";
    Atom a = parseAtom(t);
    if (a.isFloat()) return t;
    for (char c : t)
        if (std::isspace((unsigned char)c) || c == '(' || c == ')' || c == '"' || c == ';' || c == '\\')
            return quote(t);
    return t;
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

bool isSignalType(const Value* outlettype, int k) {
    if (!outlettype || !outlettype->isArray() || k < 0 || k >= (int)outlettype->items.size()) return false;
    const std::string& t = outlettype->items[(size_t)k].str;
    return t == "signal" || t == "multichannelsignal";
}

// Max names Moritz spells differently, or objects it can stand in for
const std::map<std::string, std::string> kRename = {
    {"ezdac~", "dac~"}, {"ezadc~", "adc~"}, {"s~", "send~"}, {"r~", "receive~"},
    {"live.gain~", "gain~"}, {"live.numbox", "flonum"}, {"textbutton", "button"},
    {"live.button", "button"}, {"live.toggle", "toggle"}, {"live.text", "toggle"},
    {"live.scope~", "scope~"}, {"live.meter~", "meter~"}, {"patcher", "p"},
};

// decorations and helpers that have no meaning in Moritz: left out quietly
const std::set<std::string> kSkip = {"panel", "fpic", "jsui", "live.line", "swatch", "markup",
                                     "bpatcher_header", "live.comment"};

struct Line {
    std::string from, to;
    int outlet, inlet;
};

}  // namespace

class MaxImporter {
public:
    MaxImporter(std::string dir, bool writeFiles) : dir_(std::move(dir)), write_(writeFiles) {}

    bool convertFile(const std::string& path, std::string& out, std::string& err) {
        std::string src;
        if (!readFile(path, src)) { err = "can't read " + path; return false; }
        Value root;
        if (!json::Parser(src).parse(root, err)) { err = "not a Max patch (" + err + ")"; return false; }
        const Value* p = root.get("patcher");
        if (!p || !p->isObject()) { err = "not a Max patch: no \"patcher\""; return false; }
        out = "; converted from " + std::string(fl_name(path)) + " by Moritz (File > Import from Max)\n" +
              patcher(*p, 0, nullptr) + "\n";
        return true;
    }

    MaxImportReport report;

private:
    std::string dir_;
    bool write_;
    std::set<std::string> visiting_;

    static const char* fl_name(const std::string& p) {
        size_t k = p.find_last_of("/\\");
        return p.c_str() + (k == std::string::npos ? 0 : k + 1);
    }

    void note(std::map<std::string, int>& m, const std::string& k) { ++m[k]; }

    // An abstraction used by the patch: convert name.maxpat into name.moritz
    // next to it (an existing name.moritz is kept).
    void abstraction(const std::string& name) {
        if (name.empty() || isClassName(name) || visiting_.count(name)) return;
        std::string maxFile = dir_ + "/" + name + ".maxpat";
        std::string mFile = dir_ + "/" + name + ".moritz";
        std::ifstream probe(maxFile);
        if (!probe) return;
        visiting_.insert(name);
        if (std::ifstream(mFile)) {
            report.kept.push_back(name + ".moritz");
            return;
        }
        std::string text, err;
        if (!convertFile(maxFile, text, err)) {
            report.problems.push_back(name + ".maxpat: " + err);
            return;
        }
        if (!write_) { report.abstractions.push_back(name + ".moritz (not written)"); return; }
        std::ofstream f(mFile, std::ios::binary);
        if (f << text) report.abstractions.push_back(name + ".moritz");
        else report.problems.push_back("can't write " + mFile);
    }

    // parentIns: for a subpatcher, which of its inlets receive signals in the parent
    std::string patcher(const Value& p, int indent, const std::vector<bool>* parentSigIns) {
        std::string pad((size_t)indent, ' ');
        std::ostringstream o;
        o << "(patch";
        const Value* boxes = p.get("boxes");
        const Value* lines = p.get("lines");

        // ---- cables first: they decide inlet/outlet signal types
        std::vector<Line> cables;
        if (lines && lines->isArray())
            for (auto& lv : lines->items) {
                const Value* l = lv.get("patchline");
                if (!l) continue;
                const Value* s = l->get("source");
                const Value* d = l->get("destination");
                if (!s || !d || s->items.size() < 2 || d->items.size() < 2) continue;
                cables.push_back({s->items[0].str, d->items[0].str, (int)s->items[1].num, (int)d->items[1].num});
            }
        std::map<std::string, const Value*> byId;
        if (boxes && boxes->isArray())
            for (auto& bv : boxes->items)
                if (const Value* b = bv.get("box")) byId[b->getStr("id")] = b;
        auto sourceIsSignal = [&](const Line& c) {
            auto it = byId.find(c.from);
            return it != byId.end() && isSignalType(it->second->get("outlettype"), c.outlet);
        };

        // Max orders a subpatcher's inlets left to right, as Moritz does
        std::vector<std::pair<double, std::string>> inletsByX;
        if (boxes)
            for (auto& bv : boxes->items)
                if (const Value* b = bv.get("box"); b && b->getStr("maxclass") == "inlet") {
                    const Value* r = b->get("patching_rect");
                    inletsByX.push_back({r && r->items.size() > 1 ? r->items[0].num : 0.0, b->getStr("id")});
                }
        std::sort(inletsByX.begin(), inletsByX.end());

        std::map<std::string, std::string> idOf, aliasOut;   // Max id -> Moritz id; loadmess -> its message
        std::set<std::string> used;
        auto makeId = [&](const std::string& maxId) {
            std::string id;
            for (char c : maxId) if (std::isalnum((unsigned char)c)) id += c;
            if (id.empty()) id = "obj";
            std::string base = id;
            for (int k = 2; used.count(id); ++k) id = base + "_" + std::to_string(k);
            used.insert(id);
            return id;
        };

        std::vector<std::string> extraConnects;
        if (boxes && boxes->isArray())
            for (auto& bv : boxes->items) {
                const Value* b = bv.get("box");
                if (!b) continue;
                std::string maxId = b->getStr("id"), mc = b->getStr("maxclass");
                const Value* r = b->get("patching_rect");
                int x = r && r->items.size() > 1 ? (int)std::lround(r->items[0].num) : 20;
                int y = r && r->items.size() > 1 ? (int)std::lround(r->items[1].num) : 20;
                std::string pos = " (pos " + std::to_string(x) + " " + std::to_string(y) + ")";
                ++report.boxes;
                if (kSkip.count(mc)) { ++report.skipped; continue; }
                std::string id = makeId(maxId);
                idOf[maxId] = id;
                std::string line = "(obj " + id + " ";

                if (mc == "comment") {
                    line += "comment " + quote(b->getStr("text")) + pos + ")";
                } else if (mc == "message") {
                    line += "message " + quote(b->getStr("text")) + pos + ")";
                } else if (mc == "number" || mc == "flonum" || mc == "toggle" || mc == "button" ||
                           mc == "meter~" || mc == "scope~" || mc == "spectroscope~" || mc == "gain~" ||
                           mc == "waveform~" || mc == "function") {
                    line += mc + pos + ")";
                    if (mc == "function" && b->get("addpoints")) note(report.lost, "function breakpoints");
                } else if (mc == "slider" || mc == "dial") {
                    double size = b->getNum("size", 128.0), mn = b->getNum("min", 0.0);
                    bool fl = b->getNum("floatoutput", 0.0) != 0.0;
                    if (fl) line += "slider " + num(mn) + (std::floor(mn) == mn ? "." : "") + " " +
                                    num(mn + size) + (std::floor(mn + size) == mn + size ? "." : "");
                    else line += "slider " + num(mn) + " " + num(mn + size - 1);
                    line += pos + ")";
                    if (mc == "dial") note(report.renamed, "dial -> slider");
                } else if (mc == "kslider") {
                    int range = (int)b->getNum("range", 61.0), offset = (int)b->getNum("offset", 36.0);
                    int mode = (int)b->getNum("mode", 0.0);
                    line += "kslider " + std::to_string(std::max(1, (int)std::lround(range / 12.0))) + " " +
                            std::to_string(offset) + (mode ? " 1" : "") + pos + ")";
                } else if (mc == "inlet" || mc == "outlet") {
                    bool sig = false;
                    if (mc == "inlet") {
                        for (size_t k = 0; k < inletsByX.size(); ++k)
                            if (inletsByX[k].second == maxId && parentSigIns && k < parentSigIns->size())
                                sig = (*parentSigIns)[k];
                    } else {
                        for (auto& c : cables)
                            if (c.to == maxId && sourceIsSignal(c)) sig = true;
                    }
                    line += mc + (sig ? "~" : "") + pos + ")";
                } else if (mc == "newobj" || kRename.count(mc) || mc == "bpatcher") {
                    std::vector<std::string> t = maxTokens(mc == "newobj" ? b->getStr("text") : mc);
                    if (mc == "bpatcher") {
                        note(report.unknown, "bpatcher");
                        line += "comment " + quote("bpatcher " + b->getStr("name")) + pos + ")";
                        o << "\n" << pad << "  " << line;
                        continue;
                    }
                    if (t.empty()) t = {"comment"};
                    std::string cls = t[0];
                    std::vector<std::string> args(t.begin() + 1, t.end());
                    // attributes (@name value ...) have no Moritz equivalent yet
                    for (size_t k = 0; k < args.size(); ++k)
                        if (!args[k].empty() && args[k][0] == '@') {
                            std::string attr = cls + " " + args[k];
                            note(report.attributes, attr);
                            args.resize(k);
                            break;
                        }
                    if (auto it = kRename.find(cls); it != kRename.end()) {
                        if (cls != "patcher") note(report.renamed, cls + " -> " + it->second);
                        cls = it->second;
                    }
                    if (cls == "bangbang" || cls == "b") {         // b 3 -> t b b b
                        int n = args.empty() ? 2 : std::max(1, std::atoi(args[0].c_str()));
                        cls = "t";
                        args.assign((size_t)n, "b");
                    } else if (cls == "pfft~") {                    // Max's defaults: 512, overlap 2
                        if (args.size() < 2) args.resize(2, "512");
                        if (args.size() < 3) args.push_back("2");
                        if (!args.empty()) abstraction(args[0]);
                    } else if (cls == "poly~") {
                        if (!args.empty()) abstraction(args[0]);
                    } else if (cls == "loadmess") {                 // loadbang -> message
                        std::string mid = makeId(maxId + "msg");
                        std::string text;
                        for (auto& a : args) text += (text.empty() ? "" : " ") + a;
                        o << "\n" << pad << "  (obj " << id << " loadbang" << pos << ")";
                        o << "\n" << pad << "  (obj " << mid << " message " << quote(text)
                          << " (pos " << x + 70 << " " << y << "))";
                        extraConnects.push_back("(connect " + id + " 0 " + mid + " 0)");
                        aliasOut[maxId] = mid;
                        note(report.renamed, "loadmess -> loadbang + message");
                        continue;
                    }
                    line += arg(cls);
                    if (cls == "message" || cls == "comment" || cls == "expr" || cls == "expr~") {
                        std::string text;
                        for (auto& a : args) text += (text.empty() ? "" : " ") + a;
                        line += " " + quote(text);
                    } else {
                        for (auto& a : args) line += " " + arg(a);
                    }
                    line += pos;
                    const Value* inner = b->get("patcher");
                    if (inner && inner->isObject() && (cls == "p")) {
                        // which inlets of this subpatcher get signals here
                        std::vector<bool> sigIns((size_t)std::max(0, (int)b->getNum("numinlets", 0)), false);
                        for (auto& c : cables)
                            if (c.to == maxId && c.inlet >= 0 && c.inlet < (int)sigIns.size() && sourceIsSignal(c))
                                sigIns[(size_t)c.inlet] = true;
                        line += "\n" + pad + "    " + patcher(*inner, indent + 4, &sigIns);
                    } else if (!isClassName(cls)) {
                        abstraction(cls);
                        std::ifstream m(dir_ + "/" + cls + ".moritz");
                        if (!m && !findAbstractionFile(cls)) note(report.unknown, cls);
                    }
                    line += ")";
                } else {
                    note(report.unknown, mc);
                    line += "comment " + quote("[" + mc + "] " + b->getStr("text")) + pos + ")";
                }
                o << "\n" << pad << "  " << line;
            }

        for (auto& c : cables) {
            std::string from = aliasOut.count(c.from) ? aliasOut[c.from] : idOf.count(c.from) ? idOf[c.from] : "";
            std::string to = idOf.count(c.to) ? idOf[c.to] : "";
            if (from.empty() || to.empty()) continue;   // a skipped decoration
            o << "\n" << pad << "  (connect " << from << " " << c.outlet << " " << to << " " << c.inlet << ")";
            ++report.cables;
        }
        for (auto& c : extraConnects) o << "\n" << pad << "  " << c;
        o << ")";
        return o.str();
    }

    static bool isClassName(const std::string& cls) {
        static std::set<std::string> names;
        if (names.empty())
            for (auto& n : allClassNames()) names.insert(n);
        return names.count(cls) > 0;
    }
    static bool findAbstractionFile(const std::string& cls) {
        return !findAbstraction(cls, nullptr).empty();
    }
};

bool importMaxPatch(const std::string& path, bool writeAbstractions, std::string& out,
                    MaxImportReport& report, std::string& err) {
    MaxImporter imp(dirOf(path), writeAbstractions);
    bool ok = imp.convertFile(path, out, err);
    report = imp.report;
    return ok;
}

std::string MaxImportReport::summary() const {
    std::ostringstream o;
    o << boxes << " boxes, " << cables << " cables";
    if (skipped) o << " (" << skipped << " decorations left out)";
    auto list = [&](const char* what, const std::map<std::string, int>& m) {
        if (m.empty()) return;
        o << "\n" << what;
        bool first = true;
        for (auto& kv : m) {
            o << (first ? " " : ", ") << kv.first;
            if (kv.second > 1) o << " (" << kv.second << ")";
            first = false;
        }
    };
    list("not converted, kept as unknown boxes:", unknown);
    list("changed:", renamed);
    list("attributes left out:", attributes);
    list("lost:", lost);
    auto files = [&](const char* what, const std::vector<std::string>& v) {
        if (v.empty()) return;
        o << "\n" << what;
        for (size_t k = 0; k < v.size(); ++k) o << (k ? ", " : " ") << v[k];
    };
    files("abstractions converted:", abstractions);
    files("abstractions already converted (kept):", kept);
    files("problems:", problems);
    return o.str();
}

}  // namespace moritz
