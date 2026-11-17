// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// musil_cli.h — Musil, the scripting language of Moritz, from the command line (musil_cli.cpp)
#pragma once

#include <string>

namespace moritz {
// moritz --run file.mu [-- args...] or moritz --repl; argv[1] is the option
int musilCli(int argc, char** argv);
// the folder of Musil's libraries (std.mu, signals.mu, ...): musil/src next to the program, or "" when missing
std::string musilLibraryDir();
// Musil's documentation, as Moritz uses it: musil/help.txt
std::string musilHelpFile();
}
