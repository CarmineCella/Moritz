// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// codebox.h — a code box as its editor sees it (objects_code.cpp, code.cpp): a Musil script kept in the
// patch, or in a .mu file the box names, and the interpreter it runs in.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace moritz {

class LangContext;
class Patch;

class CodeSource {
public:
    virtual ~CodeSource() = default;
    virtual std::string script() const = 0;                 // the script kept in the patch ("" if in a file)
    virtual void setScript(const std::string& text) = 0;    // the editor saved it
    virtual std::string file() const = 0;                   // the .mu file it names (resolved), or ""
    virtual std::shared_ptr<LangContext> context() = 0;     // its interpreter
    virtual std::string title() const = 0;                  // for the editor's title bar
    virtual Patch* patchOf() const = 0;
};

// set by the application (mainwindow.cpp): open the editor of a box, and close it when the box goes
extern std::function<void(CodeSource*)> openCodeSource;
extern std::function<void(CodeSource*)> codeSourceGone;

}  // namespace moritz
