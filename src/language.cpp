// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// language.cpp — the Moritz language from the command line:
//
//   moritz --run file.moritz [-- args...]   evaluates a file, no window
//   moritz --repl                            a prompt in the terminal
//
// The libraries written in the language (std, signals, music...) are found in language/ (load "music").

#include "language.h"

#include "lang/lang.h"
#include "langpatch.h"
#include "codewindow.h"
#include "moritz.h"
#include "gui.h"

#include <fstream>
#include <sstream>

#ifdef OPAQUE      // Windows (wingdi.h) defines it; the language has a value type of that name
#undef OPAQUE
#endif

namespace moritz {

namespace {

void prepare(lang::Interp& I) {
    lang::make_env(I);
    addPatchLanguage(I);
    std::string dir = resourceDir("language");
    if (!dir.empty()) I.load_path.push_back(dir);
    I.load("std");                                   // as in the application's contexts
}

}  // namespace

// --language-check file.moritz: the language reads the patch file as Moritz's loader does (for tests)
int languageCheck(const std::string& path) {
    std::string src, err;
    if (!readFile(path, src)) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    if (isCodeText(src) || patchNeedsLanguage(src)) return 0;   // written with code: only the language reads it
    Patch direct, viaLanguage;
    direct.dir = viaLanguage.dir = path.substr(0, path.find_last_of("/\\") + 1);
    if (!direct.load(src, err)) { std::printf("%s: moritz cannot load it: %s\n", path.c_str(), err.c_str()); return 1; }
    lang::Interp I;
    prepare(I);
    std::string text;
    try {
        lang::vptr v = I.run(src, path);
        if (!patchValueText(v, text)) { std::printf("%s: the language made no patch\n", path.c_str()); return 1; }
    } catch (const std::exception& e) {
        std::printf("%s: the language cannot read it: %s\n", path.c_str(), e.what());
        return 1;
    }
    if (!viaLanguage.load(text, err)) { std::printf("%s: its patch does not load: %s\n", path.c_str(), err.c_str()); return 1; }
    if (direct.serialize() != viaLanguage.serialize()) {
        std::printf("%s: read differently\n--- moritz\n%s--- language\n%s", path.c_str(), direct.serialize().c_str(),
                    viaLanguage.serialize().c_str());
        return 1;
    }
    return 0;
}

bool languagePatch(const std::string& path, std::string& src, std::string& err) {
    lang::Interp I;
    prepare(I);
    std::string last;
    I.toplevel_fn = [&last](const lang::vptr& v) { std::string t; if (patchValueText(v, t)) last = t; };
    try {
        I.run(src, path);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    src = last;
    return true;
}

int languageCli(int argc, char** argv) {
    std::string mode = argv[1];
    if (mode == "--language-check") {
        int bad = 0;
        for (int k = 2; k < argc; ++k) bad += languageCheck(argv[k]);
        return bad ? 1 : 0;
    }
    lang::Interp I;
    prepare(I);
    std::vector<std::string> files, rest;
    for (int k = 2; k < argc; ++k) {
        std::string a = argv[k];
        if (a == "--") { for (++k; k < argc; ++k) rest.push_back(argv[k]); break; }
        files.push_back(a);
    }
    lang::vlist args;
    for (auto& s : rest) args.push_back(lang::v_str(s));
    I.def("args", lang::v_list(std::move(args)));
    if (mode == "--run" && files.empty()) {
        std::fprintf(stderr, "usage: moritz --run file.moritz [-- args...]\n");
        return 2;
    }
    try {
        for (auto& f : files) {
            std::ifstream in(f);
            if (!in) { std::fprintf(stderr, "cannot open %s\n", f.c_str()); return 1; }
            std::stringstream ss; ss << in.rdbuf();
            I.run(ss.str(), f);
        }
    } catch (lang::Exit_signal& e) {
        return e.code;
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (mode == "--repl") {
        std::printf("Moritz %s\n", MORITZ_VERSION);
        try { I.repl(); } catch (lang::Exit_signal& e) { return e.code; }
    }
    return 0;
}

}  // namespace moritz
