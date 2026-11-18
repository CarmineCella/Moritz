// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sysopen.h — a patch asking the system to open a file, a folder, a program or an address
//
// A click of the user on a link opens it (openWithSystem, moritz.h). A message (a bang, loadbang) is
// the patch acting by itself: a patch from elsewhere must not open things on the machine without the
// user agreeing, so the user is asked first, later, from the event loop. Without a window to ask in
// (moritz --render) nothing is opened.
#pragma once

#include <string>

namespace moritz {

class Patch;

void askToOpenWithSystem(Patch* from, const std::string& target, const std::string& who);

}  // namespace moritz
