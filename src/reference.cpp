// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// reference.cpp — the objects' reference (reference.h)

#include "reference.h"

#include "objects.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

#include <FL/filename.H>

namespace moritz {

std::string portHelp(const Box* b, bool outlet, int i);   // ports.cpp

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t k = 0;
    while (k < s.size()) {
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
        size_t e = k;
        while (e < s.size() && s[e] != ' ' && s[e] != '\t') ++e;
        if (e > k) out.push_back(s.substr(k, e - k));
        k = e;
    }
    return out;
}

void parse(const std::string& text, std::vector<RefEntry>& out) {
    RefEntry* e = nullptr;
    bool para = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        std::string t = trim(line);
        if (!t.empty() && t[0] == '#' && (t.size() == 1 || t[1] == ' ')) continue;   // "#0", "#1": text
        if (!t.empty() && t[0] == '@') {
            out.emplace_back();
            e = &out.back();
            e->names = words(t.substr(1));
            para = false;
            continue;
        }
        if (!e) continue;
        auto field = [&](const char* key) { return t.rfind(key, 0) == 0; };
        auto split = [&](size_t from) {
            std::string rest = t.substr(from);
            size_t c = rest.find(": ");
            if (c == std::string::npos && !rest.empty() && rest.back() == ':') c = rest.size() - 1;
            if (c == std::string::npos) return std::make_pair(trim(rest), std::string());
            return std::make_pair(trim(rest.substr(0, c)), trim(rest.substr(c + 1)));
        };
        if (field("syntax:")) e->syntax = trim(t.substr(7));
        else if (field("arg ")) e->args.push_back(split(4));
        else if (field("msg ")) e->msgs.push_back(split(4));
        else if (field("see:")) e->see = words(t.substr(4));
        else if (t.empty()) para = !e->about.empty();
        else {
            if (!e->about.empty()) e->about += para ? "\n\n" : " ";
            e->about += t;
            para = false;
        }
    }
}

struct Reference {
    std::vector<RefEntry> entries;
    std::map<std::string, size_t> byName;
};

const Reference& reference() {
    static const Reference r = [] {
        Reference ref;
        for (auto& dir : helpPath()) {
            std::string d = dir + "/reference";
            dirent** list = nullptr;
            int n = fl_filename_list(d.c_str(), &list, fl_casealphasort);
            for (int k = 0; k < n; ++k) {
                std::string f = list[k]->d_name;
                if (f.size() < 5 || f.compare(f.size() - 4, 4, ".txt") != 0) continue;
                std::string text;
                if (readFile(d + "/" + f, text)) parse(text, ref.entries);
            }
            fl_filename_free_list(&list, n);
        }
        for (size_t i = 0; i < ref.entries.size(); ++i)
            for (auto& nm : ref.entries[i].names)
                if (!ref.byName.count(nm)) ref.byName[nm] = i;
        return ref;
    }();
    return r;
}

// the inlets and outlets of a class: one box made with no arguments
void ports(const std::string& cls, std::vector<std::string>& ins, std::vector<std::string>& outs) {
    if (const char* d = describeClass(cls); !d || !*d) return;   // not a built-in class
    if (cls == "pfft~" || cls == "poly~" || cls == "moritz") return;   // their ports come from a patch file
    std::unique_ptr<Box> b = makeBox(cls, Message{}, nullptr);
    if (!b || b->kind == BoxKind::Broken) return;
    auto body = [](const std::string& h) {
        size_t nl = h.find('\n');
        return nl == std::string::npos ? h : h.substr(nl + 1);
    };
    for (int i = 0; i < b->numIns(); ++i) ins.push_back(body(portHelp(b.get(), false, i)));
    for (int i = 0; i < b->numOuts(); ++i) outs.push_back(body(portHelp(b.get(), true, i)));
}

std::string mdEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '|') out += "\\|";
        else out += c;
    }
    return out;
}

}  // namespace

const RefEntry* findReference(const std::string& cls) {
    const Reference& r = reference();
    auto it = r.byName.find(cls);
    return it == r.byName.end() ? nullptr : &r.entries[it->second];
}

std::string referenceText(const std::string& cls) {
    const RefEntry* e = findReference(cls);
    std::string out;
    if (e) {
        out += (e->syntax.empty() ? cls : e->syntax) + "\n\n";
        if (!e->about.empty()) out += e->about + "\n";
        auto table = [&](const char* title, const std::vector<std::pair<std::string, std::string>>& rows) {
            if (rows.empty()) return;
            out += std::string("\n") + title + "\n";
            for (auto& r : rows) out += "  " + r.first + (r.second.empty() ? "" : "\n      " + r.second) + "\n";
        };
        table("ARGUMENTS", e->args);
        table("MESSAGES", e->msgs);
    } else {
        const char* d = describeClass(cls);
        out += cls + "\n\n" + (d && *d ? std::string(d) : "No reference yet.") + "\n";
    }
    std::vector<std::string> ins, outs;
    ports(cls, ins, outs);
    auto list = [&](const char* title, const std::vector<std::string>& v) {
        if (v.empty()) return;
        out += std::string("\n") + title + "\n";
        for (size_t i = 0; i < v.size(); ++i) {
            std::string t = v[i];
            for (size_t p = 0; (p = t.find('\n', p)) != std::string::npos; p += 7) t.replace(p, 1, "\n      ");
            out += "  " + std::to_string(i + 1) + "   " + t + "\n";
        }
    };
    list("INLETS", ins);
    list("OUTLETS", outs);
    if (e && !e->see.empty()) {
        out += "\nSEE ALSO\n  ";
        for (size_t i = 0; i < e->see.size(); ++i) out += (i ? ", " : "") + e->see[i];
        out += "\n";
    }
    return out;
}

std::vector<std::string> classesWithoutReference() {
    std::vector<std::string> out;
    for (auto& c : classInfos())
        if (!c.group.empty() && !findReference(c.name)) out.push_back(c.name);
    for (auto& n : libraryNames())
        if (!findReference(n)) out.push_back(n);
    return out;
}

std::string referenceMarkdown() {
    // the groups in the menus' order, then the library
    std::vector<std::string> groups;
    std::map<std::string, std::vector<std::string>> members;
    for (auto& c : classInfos()) {
        if (c.group.empty()) continue;
        if (!members.count(c.group)) groups.push_back(c.group);
        members[c.group].push_back(c.name);
    }
    for (auto& n : libraryNames()) {
        if (!members.count("Library")) groups.push_back("Library");
        members["Library"].push_back(n);
    }
    auto anchor = [](std::string g) {
        std::string a;
        for (char c : g) {
            if (std::isalnum((unsigned char)c)) a += (char)std::tolower((unsigned char)c);
            else if (c == ' ' || c == '-') a += '-';
        }
        return a;
    };
    std::string md =
        "# Moritz objects\n\n"
        "Every object, by category, with its arguments, messages, inlets and outlets. The same reference is in\n"
        "Moritz itself: right-click a box and choose Reference, or press Reference in a help window. This file\n"
        "is generated from `help/reference/` by `moritz --reference-md`.\n\n";
    for (auto& g : groups) md += "- [" + g + "](#" + anchor(g) + ")\n";
    md += "\nObjects follow their Max/MSP namesakes. Signal objects end in `~`. An unconnected signal inlet\n"
          "takes a float instead. For control objects, the left inlet is \"hot\" (it makes the object output)\n"
          "and the other inlets are \"cold\" (they only store a value). Numbers follow Max's types: an argument\n"
          "written with a point (`2.`, `0.5`) makes `+ - * / %`, the comparisons, `pack`, `unpack` and `scale`\n"
          "work in decimals; an integer argument, or none, makes them truncate.\n";
    std::set<const RefEntry*> done;
    for (auto& g : groups) {
        md += "\n## " + g + "\n";
        for (auto& name : members[g]) {
            const RefEntry* e = findReference(name);
            if (e && done.count(e)) continue;
            if (e) done.insert(e);
            std::string title;
            if (e) for (size_t i = 0; i < e->names.size(); ++i) title += (i ? " " : "") + std::string("`") + e->names[i] + "`";
            else title = "`" + name + "`";
            md += "\n### " + title + "\n\n";
            if (!e) {
                const char* d = describeClass(name);
                md += std::string(d && *d ? d : "") + "\n";
                continue;
            }
            if (!e->syntax.empty()) md += "`" + e->syntax + "`\n\n";
            if (!e->about.empty()) md += e->about + "\n";
            auto table = [&](const char* head, const std::vector<std::pair<std::string, std::string>>& rows) {
                if (rows.empty()) return;
                md += std::string("\n| ") + head + " | |\n| --- | --- |\n";
                for (auto& r : rows) md += "| `" + mdEscape(r.first) + "` | " + mdEscape(r.second) + " |\n";
            };
            table("Argument", e->args);
            table("Message", e->msgs);
            std::vector<std::string> ins, outs;
            ports(name, ins, outs);
            auto plist = [&](const char* head, const std::vector<std::string>& v) {
                if (v.empty()) return;
                md += std::string("\n| ") + head + " | |\n| --- | --- |\n";
                for (size_t i = 0; i < v.size(); ++i) {
                    std::string t = v[i];
                    std::replace(t.begin(), t.end(), '\n', ' ');
                    md += "| " + std::to_string(i + 1) + " | " + mdEscape(t) + " |\n";
                }
            };
            plist("Inlet", ins);
            plist("Outlet", outs);
            if (!e->see.empty()) {
                md += "\nSee also:";
                for (auto& s : e->see) md += " `" + s + "`";
                md += "\n";
            }
        }
    }
    return md;
}

}  // namespace moritz
