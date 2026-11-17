// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// langpatch.cpp — patches in the Moritz language (langpatch.h)

#include "langpatch.h"

#include "moritz.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#ifdef OPAQUE      // Windows (wingdi.h) defines it; the language has a value type of that name
#undef OPAQUE
#endif

namespace moritz {

using namespace lang;

namespace {

struct PatchValue {
    std::string text;
};

// the patch being written: (patch ...) pushes one, obj and connect add to the top one
struct Build {
    std::vector<std::string> objs, conns;
    std::set<std::string> ids;
};
thread_local std::vector<Build*> building;

// an atom as a patch file writes it: quoted when it would not read back as the same symbol
std::string quoteAtom(const std::string& s, bool string) {
    bool need = s.empty();
    for (char c : s)
        if (std::isspace((unsigned char)c) || std::strchr("()\";\\'{}", c)) need = true;
    if (!s.empty() && s[0] == ',') need = true;
    if (!need && string) {                         // a string that reads as a number stays a string
        char* end = nullptr;
        std::strtod(s.c_str(), &end);
        if (end && *end == '\0') need = true;
    }
    if (!need) return s;
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        if (c == '\n') { r += "\\n"; continue; }
        r += c;
    }
    return r + "\"";
}

std::string numberAtom(double d) {
    char b[64];
    if (std::isfinite(d) && std::fabs(d) < 1e15 && d == std::floor(d)) std::snprintf(b, sizeof b, "%lld", (long long)d);
    else {
        std::snprintf(b, sizeof b, "%.10g", d);
        if (!std::strpbrk(b, ".eEn")) std::strcat(b, ".");   // keep it a float
    }
    return b;
}

// a computed value as atoms: numbers, vectors (each value), strings and symbols, lists (spliced)
void valueAtoms(Interp& I, const vptr& v, std::vector<std::string>& out, const char* who) {
    if (!v || v->t == Value::NIL) return;
    switch (v->t) {
    case Value::NUM:
        if (v->num.size() == 1 && !v->s.empty()) out.push_back(v->s);       // as it was written
        else for (double d : v->num) out.push_back(numberAtom(d));
        return;
    case Value::STR: out.push_back(quoteAtom(v->s, true)); return;
    case Value::SYM: out.push_back(quoteAtom(v->s, false)); return;
    case Value::LIST: for (auto& x : v->l) valueAtoms(I, x, out, who); return;
    default: I.err(std::string(who) + ": cannot write a " + type_name(v) + " in a patch");
    }
}

// the arguments of obj, connect and pos, from position k of a form: as written, or computed after a comma
// (,x  ,(...)). Lists other than (pos ...) and (patch ...) are left to the caller.
std::vector<std::string> atomsOf(Interp& I, const vlist& l, size_t from, size_t to, eptr env, const char* who) {
    std::vector<std::string> out;
    for (size_t k = from; k < to; ++k) {
        const vptr& a = l[k];
        if (a->t == Value::SYM && !a->s.empty() && a->s[0] == ',') {
            vptr v;
            if (a->s.size() == 1) {
                if (k + 1 >= to) I.err(std::string(who) + ": nothing after the comma");
                v = I.eval(l[++k], env);
            } else {
                v = env->find(a->s.substr(1));
                if (!v) I.err(std::string(who) + ": undefined: " + a->s.substr(1));
            }
            valueAtoms(I, v, out, who);
            continue;
        }
        switch (a->t) {
        case Value::NUM: out.push_back(!a->s.empty() ? a->s : numberAtom(a->num[0])); break;
        case Value::STR: out.push_back(quoteAtom(a->s, true)); break;
        case Value::SYM: out.push_back(quoteAtom(a->s, false)); break;
        case Value::LIST:
            if (a->l.size() == 2 && a->l[0]->t == Value::SYM && a->l[0]->s == "quote") {   // 'x, written in a file
                valueAtoms(I, a->l[1], out, who);
                break;
            }
            I.err(std::string(who) + ": a list among the arguments: compute it with ,( ... )");
        default: I.err(std::string(who) + ": cannot write a " + type_name(a) + " in a patch");
        }
    }
    return out;
}

bool headIs(const vptr& v, const char* h) {
    return v && v->t == Value::LIST && !v->l.empty() && v->l[0]->t == Value::SYM && v->l[0]->s == h;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

vptr formPatch(vlist& l, eptr env, Interp& I) {
    Build b;
    building.push_back(&b);
    struct Pop { ~Pop() { building.pop_back(); } } pop;
    auto local = I.make_env(env);                  // what the patch's code defines stays inside it
    for (size_t k = 1; k < l.size(); ++k) I.eval(l[k], local);
    std::string t = "(patch";
    for (auto& o : b.objs) t += "\n  " + o;
    for (auto& c : b.conns) t += "\n  " + c;
    t += ")";
    return makePatchValue(t);
}

vptr formObj(vlist& l, eptr env, Interp& I) {
    if (building.empty()) I.err("obj: only inside (patch ...)");
    Build& b = *building.back();
    if (l.size() < 3) I.err("obj: expected (obj id class args... [(pos x y)])");
    // the id and the class: one atom each
    std::vector<std::string> head;
    size_t k = 1;
    while (head.size() < 2 && k < l.size()) {
        size_t start = k;
        if (l[k]->t == Value::SYM && l[k]->s == ",") k += 2;
        else ++k;
        auto a = atomsOf(I, l, start, std::min(k, l.size()), env, "obj");
        if (a.size() != 1) I.err("obj: the id and the class are one word each");
        head.push_back(a[0]);
    }
    if (head.size() < 2) I.err("obj: expected (obj id class args...)");
    std::string id = head[0];
    if (b.ids.count(id)) I.err("obj: two boxes with the id " + id);
    b.ids.insert(id);
    std::vector<std::string> args;
    std::string pos, inner;
    size_t runStart = k;
    auto flush = [&](size_t end) {
        auto a = atomsOf(I, l, runStart, end, env, "obj");
        args.insert(args.end(), a.begin(), a.end());
    };
    for (; k < l.size(); ++k) {
        if (headIs(l[k], "pos")) {
            flush(k);
            auto p = atomsOf(I, l[k]->l, 1, l[k]->l.size(), env, "pos");
            if (p.size() != 2) I.err("pos: expected (pos x y)");
            pos = "(pos " + p[0] + " " + p[1] + ")";
            runStart = k + 1;
        } else if (headIs(l[k], "patch")) {
            flush(k);
            vptr v = I.eval(l[k], env);
            patchValueText(v, inner);
            runStart = k + 1;
        } else if (l[k]->t == Value::SYM && l[k]->s == ",") {
            ++k;                                   // its form belongs to the comma
        }
    }
    flush(l.size());
    std::string o = "(obj " + id + " " + head[1];
    if (!args.empty()) o += " " + join(args);
    if (!pos.empty()) o += " " + pos;
    if (!inner.empty()) {
        std::string ind;
        for (char c : inner) { ind += c; if (c == '\n') ind += "    "; }
        o += "\n    " + ind;
    }
    o += ")";
    b.objs.push_back(o);
    return v_nil();
}

vptr formConnect(vlist& l, eptr env, Interp& I) {
    if (building.empty()) I.err("connect: only inside (patch ...)");
    auto a = atomsOf(I, l, 1, l.size(), env, "connect");
    if (a.size() != 4) I.err("connect: expected (connect from outlet to inlet)");
    building.back()->conns.push_back("(connect " + join(a) + ")");
    return v_nil();
}

// ---- functions on patch values

vptr fnPatchP(vlist& a, Interp&) { std::string t; return v_bool(patchValueText(a[0], t)); }

vptr fnPatchText(vlist& a, Interp& I) {
    std::string t;
    if (!patchValueText(a[0], t)) I.bad("expected a patch, got " + std::string(type_name(a[0])));
    return v_str(t + "\n");
}

vptr fnSavePatch(vlist& a, Interp& I) {
    std::string t;
    if (!patchValueText(a[0], t)) I.bad("expected a patch, got " + std::string(type_name(a[0])));
    std::string path = I.str(a[1]);
    std::ofstream f(path, std::ios::binary);
    if (!f || !(f << t << "\n")) I.bad("cannot write " + path);
    return a[0];
}

vptr fnPatchBoxes(vlist& a, Interp& I) {
    std::string t;
    if (!patchValueText(a[0], t)) I.bad("expected a patch, got " + std::string(type_name(a[0])));
    return patchBoxes(t);
}

// (read-patch path) the patch a file makes: a plain patch file, or one written with code
vptr fnReadPatch(vlist& a, Interp& I) {
    std::string path = I.find_file(I.str(a[0]));
    if (path.empty()) I.bad("not found: " + I.str(a[0]));
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Parser p(ss.str(), std::make_shared<std::string>(path));
    vptr prog = p.program();
    vptr found;
    for (size_t k = 1; k < prog->l.size(); ++k) {
        vptr v = I.eval(prog->l[k], I.global);
        std::string t;
        if (patchValueText(v, t)) found = v;
    }
    if (!found) I.bad(path + " makes no patch");
    return found;
}

}  // namespace

lang::vptr makePatchValue(const std::string& text) {
    auto pv = std::make_shared<PatchValue>();
    pv->text = text;
    return v_opaque("patch", pv);
}

bool patchValueText(const lang::vptr& v, std::string& text) {
    if (!v || v->t != Value::OPAQUE || v->opaque_tag != "patch") return false;
    text = std::static_pointer_cast<PatchValue>(v->opaque)->text;
    return true;
}

lang::vptr patchBoxes(const std::string& text) {
    std::vector<SExpr> top;
    std::string err;
    vlist out;
    if (!parseSExprs(text, top, err) || top.empty()) return v_list(std::move(out));
    for (auto& e : top[0].items) {
        if (!e.isList || e.items.empty() || e.items[0].text != "obj" || e.items.size() < 3) continue;
        vlist args;
        double x = 0, y = 0;
        for (size_t j = 3; j < e.items.size(); ++j) {
            const SExpr& s = e.items[j];
            if (s.isList) {
                if (!s.items.empty() && s.items[0].text == "pos" && s.items.size() >= 3) {
                    x = std::atof(s.items[1].text.c_str());
                    y = std::atof(s.items[2].text.c_str());
                }
                continue;
            }
            char* end = nullptr;
            double d = std::strtod(s.text.c_str(), &end);
            if (!s.quoted && end && *end == '\0' && !s.text.empty()) args.push_back(v_num(d));
            else args.push_back(v_str(s.text));
        }
        out.push_back(v_list({v_list({v_sym("id"), v_str(e.items[1].text)}), v_list({v_sym("class"), v_str(e.items[2].text)}),
                              v_list({v_sym("args"), v_list(std::move(args))}), v_list({v_sym("x"), v_num(x)}),
                              v_list({v_sym("y"), v_num(y)})}));
    }
    return v_list(std::move(out));
}

bool makesPatch(const std::string& text) {
    std::vector<SExpr> top;
    std::string err;
    if (!parseSExprs(text, top, err)) return false;
    for (auto& e : top)
        if (e.isList && !e.items.empty() && !e.items[0].isList && e.items[0].text == "patch") return true;
    return false;
}

bool patchNeedsLanguage(const std::string& text) {
    std::vector<SExpr> top;
    std::string err;
    if (!parseSExprs(text, top, err)) return false;      // the patch loader says what is wrong
    if (top.size() != 1) return true;
    std::function<bool(const SExpr&)> codeIn = [&](const SExpr& patch) {
        for (size_t k = 1; k < patch.items.size(); ++k) {
            const SExpr& e = patch.items[k];
            if (!e.isList || e.items.empty() || e.items[0].isList) return true;
            const std::string& h = e.items[0].text;
            if (h != "obj" && h != "connect") return true;
            for (auto& a : e.items) {
                if (!a.isList && !a.quoted && !a.text.empty() && a.text[0] == ',') return true;
                if (a.isList && !a.items.empty() && !a.items[0].isList) {
                    if (a.items[0].text == "patch" && codeIn(a)) return true;
                    if (a.items[0].text == "pos")
                        for (auto& p : a.items)
                            if (!p.isList && !p.quoted && !p.text.empty() && p.text[0] == ',') return true;
                }
            }
        }
        return false;
    };
    const SExpr& p = top[0];
    if (!p.isList || p.items.empty() || p.items[0].isList || p.items[0].text != "patch") return true;
    return codeIn(p);
}

void addPatchLanguage(lang::Interp& I) {
    // (patch forms...) a patch: its (obj ...) and (connect ...) forms, and any code that writes them
    // (obj id class args... (pos x y)) a box of the patch being written; arguments as written, ,x and ,(...) computed
    // (connect from outlet to inlet) a cable of the patch being written
    I.def_form("patch", formPatch);
    I.def_form("obj", formObj);
    I.def_form("connect", formConnect);
    // (patch? x) 1 when x is a patch
    I.def("patch?", fnPatchP, 1, 1);
    // (patch-text p) the text of a patch, as a file holds it
    I.def("patch-text", fnPatchText, 1, 1);
    // (save-patch p path) writes a patch as a file; => the patch
    I.def("save-patch", fnSavePatch, 2, 2);
    // (patch-boxes p) the boxes of a patch: records with 'id 'class 'args 'x 'y
    I.def("patch-boxes", fnPatchBoxes, 1, 1);
    // (read-patch path) the patch a file makes, a plain one or one written with code
    I.def("read-patch", fnReadPatch, 1, 1);
}

}  // namespace moritz
