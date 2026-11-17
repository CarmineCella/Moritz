// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// language.h — the Moritz language from the command line (language.cpp)
#pragma once

#include <string>

namespace moritz {
// moritz --run file.moritz [-- args...] or moritz --repl; argv[1] is the option
int languageCli(int argc, char** argv);
// runs a file of the language (without a window): src becomes the text of the last patch its top-level
// forms make, or "" when it makes none; false and err on an error
bool languagePatch(const std::string& path, std::string& src, std::string& err);
}
