// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// session.h — where code of the Moritz language runs inside the application: a context is an
// interpreter (with std loaded) that the Console's prompt and the code editors send code to. There
// is one called "session", and one for each open patch. Code runs on a thread of its own, one piece
// at a time per context, so the windows stay alive and Stop works; what it prints goes to the
// Console, and the result comes back on the main thread.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace moritz {

namespace lang { struct Interp; }

struct EvalResult {
    bool ok = true;
    bool stopped = false;
    std::string value;          // what the last form gave, printed (empty for nil)
    std::string error;          // the message, with the call stack
    std::string file;           // where the error is
    int line = 0;
};

struct Variable {
    std::string name, kind, preview;
};

class LangContext : public std::enable_shared_from_this<LangContext> {
public:
    explicit LangContext(std::string label);
    ~LangContext();
    std::string label;
    bool busy() const { return busy_; }
    // Runs code on the context's thread. file names it (errors and relative loads use it); the code's
    // first line is line firstLine of that file. done runs on the main thread. False if busy.
    bool eval(const std::string& code, const std::string& file, int firstLine,
              std::function<void(const EvalResult&)> done);
    void stop();                                   // the running code stops at its next step
    std::vector<Variable> variables();             // what the code defined (as of the last run)
    std::vector<std::string> names();              // every name it knows (as of the last run)

private:
    std::unique_ptr<lang::Interp> interp_;
    std::atomic<bool> busy_{false}, stop_{false};
    std::mutex mu_;
    std::vector<Variable> vars_;
    std::vector<std::string> names_;
    std::vector<std::string> baseline_;            // the names before any code ran
    void snapshot();                               // on the context's thread, after a run
};

// the contexts: "session", and one per open patch (keyed by the patch's document)
std::shared_ptr<LangContext> sessionContext();
std::shared_ptr<LangContext> patchContext(const void* doc, const std::string& title);
void dropPatchContext(const void* doc);

// the documentation of the language's functions (language/help.txt)
struct HelpEntry {
    std::vector<std::string> names;
    std::string signature;
    std::vector<std::string> text;
};
const HelpEntry* languageHelp(const std::string& name);
const std::vector<std::string>& languageNames();   // every documented name, sorted

}  // namespace moritz
