// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// musil_cli.cpp — Musil, the scripting language of Moritz, from the command line:
//
//   moritz --run file.mu [-- args...]   runs a script, no window
//   moritz --repl                        a prompt in the terminal
//
// As with Musil's own command line, a script loads the libraries it uses (load "signals.mu"); they are
// found in musil/src.

#include "musil_cli.h"

#include "musil_host.h"
#include "gui.h"

#include <fstream>
#include <sstream>

namespace moritz {

std::string musilLibraryDir() {
    std::string d = resourceDir("musil");
    return d.empty() ? d : d + "/src";
}

std::string musilHelpFile() {
    std::string d = resourceDir("help");
    return d.empty() ? d : d + "/musil/help.txt";
}

void musilPaths(musil::Interp& I) {
    // first the folder of Moritz's help.txt (what (help name) reads: Musil as Moritz hosts it), then the libraries
    if (std::string f = musilHelpFile(); !f.empty()) I.load_path.push_back(f.substr(0, f.find_last_of('/')));
    if (std::string dir = musilLibraryDir(); !dir.empty()) I.load_path.push_back(dir);
}

namespace {

void prepare(musil::Interp& I) {
    musilLibraries(I);
    musilPaths(I);
}

}  // namespace

int musilCli(int argc, char** argv) {
    std::string mode = argv[1];
    musil::Interp I;
    prepare(I);
    std::vector<std::string> files, rest;
    for (int k = 2; k < argc; ++k) {
        std::string a = argv[k];
        if (a == "--") { for (++k; k < argc; ++k) rest.push_back(argv[k]); break; }
        files.push_back(a);
    }
    musil::vlist args;
    for (auto& s : rest) args.push_back(musil::v_str(s));
    I.def("args", musil::v_list(std::move(args)));
    if (mode == "--run" && files.empty()) {
        std::fprintf(stderr, "usage: moritz --run file.mu [-- args...]\n");
        return 2;
    }
    try {
        for (auto& f : files) {
            std::ifstream in(f);
            if (!in) { std::fprintf(stderr, "cannot open %s\n", f.c_str()); return 1; }
            std::stringstream ss; ss << in.rdbuf();
            I.run(ss.str(), f);
        }
    } catch (musil::Exit_signal& e) {
        return e.code;
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (mode == "--repl") {
        std::printf("Musil %s (Moritz %s)\n", MUSIL_VERSION, MORITZ_VERSION);
        try { I.repl(); } catch (musil::Exit_signal& e) { return e.code; }
    }
    return 0;
}

}  // namespace moritz
