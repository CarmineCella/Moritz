// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// classes.cpp — the class table: creating boxes by name, menu groups,
// completion and abstraction lookup.

#include "objects.h"

#include <cstdio>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace moritz {

namespace {

// menu order of the groups; classes keep their registration order inside one
const char* const kGroups[] = {"Oscillators", "Filters", "Signal Math", "Signal Routing", "Delay", "Samples",
                               "Spectral", "Spatial", "Analysis", "Atoms and types", "Orchestration", "Learning", "I/O", "MIDI", "Timing", "Control Math",
                               "Routing", "Patching", "Interface", "Code"};

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
        registerMidiObjects(v);
        registerSoundObjects(v);
        registerOfflineObjects(v);
        registerLearnObjects(v);
        registerSpectralObjects(v);
        registerFilterObjects(v);
        registerSpatialObjects(v);
        registerMirObjects(v);
        registerPlotObjects(v);
        registerMapleObjects(v);
        registerSoundtypesObjects(v);
        registerOrchideaObjects(v);
        registerCodeObjects(v);
        registerScoreObjects(v);
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

std::vector<std::string>& libraryPath() {
    static std::vector<std::string> v;
    return v;
}

std::vector<std::string>& soundPath() {
    static std::vector<std::string> v;
    return v;
}

std::vector<std::string>& helpPath() {
    static std::vector<std::string> v;
    return v;
}

namespace {
bool fileExists(const std::string& p) {
    if (std::FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}
bool endsWith(const std::string& s, const std::string& end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}
}  // namespace

std::vector<std::string> libraryNames() {
    std::set<std::string> names;
    for (auto& d : libraryPath()) {
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(d, ec)) {
            std::string f = e.path().filename().string();
            if (f[0] == '.' || endsWith(f, ".help.moritz") || endsWith(f, ".help.mlsp")) continue;
            if (endsWith(f, ".moritz")) names.insert(f.substr(0, f.size() - 7));
            else if (endsWith(f, ".mlsp")) names.insert(f.substr(0, f.size() - 5));   // before the rename
        }
    }
    return {names.begin(), names.end()};
}

// Short names, and objects documented together in one help patch.
std::string helpName(const std::string& cls) {
    static const std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> r;
        auto group = [&](const char* file, std::initializer_list<const char*> members) {
            for (auto c : members) r[c] = file;
        };
        group("arith~", {"+~", "-~", "*~", "/~"});
        group("arith", {"+", "-", "*", "/", "%"});
        group("compare", {"==", "!=", ">", "<", ">=", "<="});
        group("convert", {"mtof", "ftom", "dbtoa", "atodb"});
        group("saw~", {"saw~", "rect~", "tri~"});
        group("clip~", {"clip~", "abs~", "tanh~"});
        group("tapin~", {"tapin~", "tapout~"});
        group("snapshot~", {"snapshot~", "peakamp~"});
        group("dac~", {"dac~", "adc~"});
        group("pfft~", {"pfft~", "fftin~", "fftout~"});
        group("cartopol~", {"cartopol~", "poltocar~"});
        group("framedelta~", {"framedelta~", "frameaccum~"});
        group("poly~", {"poly~", "thispoly~"});
        group("inlet", {"inlet", "inlet~", "outlet", "outlet~", "in", "in~", "out", "out~"});
        group("send", {"send", "receive", "s", "r"});
        group("send~", {"send~", "receive~"});
        group("selector~", {"selector~", "gate~"});
        group("pack", {"pack", "unpack"});
        group("prepend", {"prepend", "append"});
        group("int", {"int", "float", "i", "f"});
        group("number", {"number", "flonum"});
        group("button", {"button", "toggle"});
        group("meter~", {"meter~", "scope~"});
        group("ctlin", {"ctlin", "bendin", "pgmin"});
        group("noteout", {"noteout", "ctlout"});
        group("conv~", {"conv~", "condec"});
        group("filterdesign", {"filterdesign", "filterplot"});
        group("ambienc~", {"ambienc~", "ambirot~", "ambidec~"});
        group("spectrogram", {"spectrogram", "melgram", "mfccgram", "chromagram"});
        group("melbands~", {"melbands~", "mfcc~", "chroma~"});
        group("similarity", {"similarity", "novelty"});
        r["t"] = "trigger";
        r["sel"] = "select";
        r["del"] = "delay";
        return r;
    }();
    auto it = m.find(cls);
    return it == m.end() ? cls : it->second;
}

std::string findHelp(const std::string& cls) {
    if (cls.empty() || cls.find('/') != std::string::npos) return "";
    for (auto* dirs : {&helpPath(), &libraryPath()})
        for (auto& d : *dirs)
            for (const char* ext : {".help.moritz", ".help.mlsp"})
                if (std::string f = d + "/" + helpName(cls) + ext; fileExists(f)) return f;
    return "";
}

// An abstraction "name" is the file name.moritz (or name.mlsp, from before
// the rename to Moritz), looked up next to the patch
// (and the patches around it), then in abstractionPath().
std::string findAbstraction(const std::string& name, const Patch* context) {
    if (name.empty() || name.find('/') != std::string::npos) return "";
    std::vector<std::string> dirs;
    for (const Patch* p = context; p; p = p->parent)
        if (!p->dir.empty()) dirs.push_back(p->dir);
    for (auto& d : libraryPath()) dirs.push_back(d);
    for (auto& d : abstractionPath()) dirs.push_back(d);
    for (auto& d : dirs) {
        for (const char* ext : {".moritz", ".mlsp"})
            if (std::string f = d + "/" + name + ext; fileExists(f)) return f;
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

}  // namespace moritz
