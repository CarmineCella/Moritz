// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// language.h — the Moritz language from the command line (language.cpp)
#pragma once

namespace moritz {
// moritz --run file.moritz [-- args...] or moritz --repl; argv[1] is the option
int languageCli(int argc, char** argv);
}
