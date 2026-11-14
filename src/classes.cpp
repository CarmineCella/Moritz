// classes.cpp — the class table: creating boxes by name, menu groups,
// completion and abstraction lookup.

#include "objects.h"

#include <cstdio>

#include <algorithm>

namespace mlsp {

namespace {

// menu order of the groups; classes keep their registration order inside one
const char* const kGroups[] = {"Oscillators", "Filters", "Signal Math", "Delay", "Samples",
                               "Spectral", "Analysis", "I/O", "Timing", "Control Math",
                               "Routing", "Patching", "Interface"};

int groupRank(const char* g) {
    for (int k = 0; k < (int)(sizeof kGroups / sizeof *kGroups); ++k)
        if (std::string(g) == kGroups[k]) return k;
    return 1000;                      // aliases ("") last
}

const ClassTable& classDefs() {
    static const ClassTable t = [] {
        ClassTable v;
        registerSignalObjects(v);
        registerSampleObjects(v);
        registerControlObjects(v);
        registerPatcherObjects(v);
        std::stable_sort(v.begin(), v.end(), [](const ClassDef& a, const ClassDef& b) {
            return groupRank(a.group) < groupRank(b.group);
        });
        return v;
    }();
    return t;
}

const ClassDef* findClass(const std::string& name) {
    for (auto& c : classDefs())
        if (name == c.name) return &c;
    return nullptr;
}

}  // namespace

std::vector<std::string>& abstractionPath() {
    static std::vector<std::string> v;
    return v;
}

// An abstraction "name" is the file name.mlsp, looked up next to the patch
// (and the patches around it), then in abstractionPath().
std::string findAbstraction(const std::string& name, const Patch* context) {
    if (name.empty() || name.find('/') != std::string::npos) return "";
    auto exists = [](const std::string& p) {
        if (std::FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
        return false;
    };
    std::vector<std::string> dirs;
    for (const Patch* p = context; p; p = p->parent)
        if (!p->dir.empty()) dirs.push_back(p->dir);
    for (auto& d : abstractionPath()) dirs.push_back(d);
    for (auto& d : dirs) {
        std::string f = d + "/" + name + ".mlsp";
        if (exists(f)) return f;
    }
    return "";
}

std::unique_ptr<Box> makeBox(const std::string& cls, const Message& args, Patch* context) {
    std::unique_ptr<Box> b;
    if (const ClassDef* def = findClass(cls)) b = def->make(args, context);
    else if (std::string f = findAbstraction(cls, context); !f.empty())
        b = makeAbstraction(f, args, context);
    if (!b) b = std::make_unique<BrokenBox>();
    b->cls = cls;
    b->args = args;
    if (b->kind == BoxKind::UI) b->uiSize(b->w, b->h);   // sized before it is ever drawn
    return b;
}

const std::vector<ClassInfo>& classInfos() {
    static const std::vector<ClassInfo> v = [] {
        std::vector<ClassInfo> r;
        for (auto& c : classDefs()) r.push_back({c.name, c.group, c.desc});
        return r;
    }();
    return v;
}

std::vector<std::string> allClassNames() {
    std::vector<std::string> v;
    for (auto& c : classDefs()) v.push_back(c.name);
    std::sort(v.begin(), v.end());
    return v;
}

const char* describeClass(const std::string& cls) {
    const ClassDef* c = findClass(cls);
    return c ? c->desc : "";
}

}  // namespace mlsp
