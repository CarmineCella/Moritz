// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// session.h — where Musil, the scripting language of Moritz, runs inside the application: a context is
// an interpreter (with std.mu loaded) that code is sent to. The session is the one of the Console's
// prompt and the code editor; every code box has its own. Code runs on a thread of its own, one piece at
// a time, so the windows stay alive and Stop works; what it prints goes to the Console, and the result
// comes back on the main thread (at once, under moritz --render, where jobs are synchronous).
// Besides its libraries, Musil in Moritz reaches the patches: send, buffer, buffer-rate, set-buffer!,
// and in a code box out.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace musil { struct Interp; }
namespace moritz {

class Patch;
struct Atom;
using Message = std::vector<Atom>;

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

// What a context reaches besides its libraries. The session's are empty: the buffers and receives of
// every open patch, output to the Console. A code box gives its patch (buffers and receives are looked
// up from it), its outlets (out), where it prints, and asks for the restrictions of a box: no exec,
// remove, input, exit, sleep, breathe, nor the network (udp-send, udp-receive, serve, serve-stop).
struct ContextHooks {
    Patch* patch = nullptr;
    std::function<void(int, const Message&)> outlet;          // (out k values...): outlet k (from 1)
    std::function<void(const std::string&)> print;
    bool restricted = false;                                   // a code box: Musil's sandboxed profile
    bool trusted = false;                                      // ... unless the user trusted its patch: everything
    std::vector<std::string> preload;                          // libraries loaded after std.mu
};

class LangContext : public std::enable_shared_from_this<LangContext> {
public:
    explicit LangContext(std::string label, ContextHooks hooks = {});
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
    void define(const std::string& name, const Message& value);   // a global, set when not busy
    void detach();                                 // the box is gone: its hooks are dropped (main thread)
    void snapshot();                               // the variables and names, after a run (or before any)
    ContextHooks hooks;

private:
    std::unique_ptr<musil::Interp> interp_;
    std::atomic<bool> busy_{false};
    std::mutex mu_;
    std::vector<Variable> vars_;
    std::vector<std::string> names_;
    std::vector<std::string> baseline_;            // the names before any code ran
};

// the session's context
std::shared_ptr<LangContext> sessionContext();
// the names refused in a code box, and every name a code box can use (sorted: moritz --code-names)
std::vector<std::string> codeBoxNames();

// the documentation of Musil's functions (help/musil/help.txt)
struct HelpEntry {
    std::vector<std::string> names;
    std::string signature;
    std::vector<std::string> text;
};
const HelpEntry* languageHelp(const std::string& name);
const std::vector<std::string>& languageNames();   // every documented name, sorted

}  // namespace moritz
