// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// language.cpp — the Moritz language from the command line:
//
//   moritz --run file.moritz [-- args...]   evaluates a file, no window
//   moritz --repl                            a prompt in the terminal
//
// The libraries written in the language (std, signals, music...) are found in language/ (load "music").

#include "language.h"

#include "lang/lang.h"
#include "gui.h"

#include <fstream>
#include <sstream>

namespace moritz {

namespace {

void prepare(lang::Interp& I) {
    lang::make_env(I);
    std::string dir = resourceDir("language");
    if (!dir.empty()) I.load_path.push_back(dir);
}

}  // namespace

int languageCli(int argc, char** argv) {
    std::string mode = argv[1];
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
