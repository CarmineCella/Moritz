// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_code.cpp — code: a Musil script inside a patch, as js is in Max. It has an interpreter of its own
// (session.h) and runs off the audio thread when it gets a bang: its inlets set the variables in1, in2...,
// (out k values...) sends out of outlet k, and it reads and writes buffer~s by name. The last outlet bangs
// when the script has run. The script is kept in the patch, or in a .mu file the box names; double-click
// opens it in the code editor.
//
// A code box may do what a patch may do: read and write files (as buffer~ and sfrecord~ do). It may not
// run programs, delete files, use the network, wait or quit (refused with an error, session.cpp).

#include "codebox.h"
#include "objects.h"
#include "session.h"

#include <fstream>
#include <sstream>

namespace moritz {

std::function<void(CodeSource*)> openCodeSource;
std::function<void(CodeSource*)> codeSourceGone;

namespace {

bool endsWithMu(const std::string& s) { return s.size() > 3 && s.compare(s.size() - 3, 3, ".mu") == 0; }

// ---------------------------------------------------------------- code
// code [inlets] [outlets] [file.mu]: a Musil script (1 inlet, 1 outlet, the script in the patch).
//   bang                   runs the script (left inlet)
//   anything else          (left inlet) becomes in1, then the script runs
//   to inlet k             becomes ink (cold: nothing runs)
//   set name values        a variable, without running
//   stop                   stops the script running
//   reset                  a fresh interpreter (the variables of earlier runs are gone)
//   open                   the script in the code editor (or double-click the box)
//   read file.mu           runs that file from now on (written into the box)
// In the script: in1, in2... (a number, a vector for a list of numbers, a string, a list), (out k values...)
// out of outlet k, (buffer name), (buffer-rate name), (set-buffer! name x [rate]), (send name values...).
// Outlets: the script's (out 1 ...) ... (out n ...), then a bang when it has run.
class CodeBox : public Box, public CodeSource {
public:
    explicit CodeBox(const Message& a) {
        int nums = 0;
        for (auto& x : a) {
            if (x.isFloat()) {
                if (nums == 0) nIn_ = std::clamp((int)x.f, 1, 16);
                else if (nums == 1) nOut_ = std::clamp((int)x.f, 0, 16);
                ++nums;
            } else if (endsWithMu(x.s)) {
                file_ = x.s;
            }
        }
        setPorts(std::vector<Port>((size_t)nIn_, Port::Control), std::vector<Port>((size_t)nOut_ + 1, Port::Control));
        values_.resize((size_t)nIn_);
    }
    ~CodeBox() override {
        *alive_ = false;
        if (ctx_) ctx_->detach();
        if (codeSourceGone) codeSourceGone(this);
    }

    void message(int inlet, const Message& m) override {
        if (m.empty()) return;
        if (inlet > 0) { values_[(size_t)inlet] = m; return; }
        const Atom& h = m[0];
        if (h.is("bang")) { run(); return; }
        if (h.is("stop")) { if (ctx_) ctx_->stop(); return; }
        if (h.is("reset")) { if (ctx_ && !ctx_->busy()) { ctx_->detach(); ctx_.reset(); } return; }
        if (h.is("open")) { if (openCodeSource) openCodeSource(this); return; }
        if (h.is("set") && m.size() > 1 && !m[1].isFloat()) {
            Message v(m.begin() + 2, m.end());
            if (ctx_ && ctx_->busy()) { patch->print("code: still running (stop ends it)"); return; }
            context()->define(m[1].s, v);
            return;
        }
        if (h.is("read") && m.size() > 1) {
            file_ = m[1].str();
            Message na;
            for (auto& x : args) if (x.isFloat()) na.push_back(x);
            na.push_back(Atom(file_));
            args = na;
            return;
        }
        values_[0] = m;
        run();
    }
    std::vector<std::string> menuItems() override { return {"Open the Script", "Run"}; }
    void menuAction(int k) override {
        if (k == 0) { if (openCodeSource) openCodeSource(this); }
        else run();
    }

    // CodeSource (and the patch file: (script ...))
    std::string script() const override { return file_.empty() ? script_ : ""; }
    void setScript(const std::string& text) override { script_ = text; }
    std::string file() const override { return file_.empty() ? "" : patch->resolvePath(file_); }
    std::shared_ptr<LangContext> context() override {
        if (!ctx_) {
            ContextHooks h;
            h.patch = patch;
            h.restricted = true;
            h.outlet = [this](int k, const Message& m) {
                if (k >= 1 && k <= nOut_) outlet(k - 1, m);
                else patch->print("code: no outlet " + std::to_string(k) + " (the box has " + std::to_string(nOut_) + ")");
            };
            h.print = [this](const std::string& s) { patch->print(s); };
            ctx_ = std::make_shared<LangContext>("code " + id, h);
            for (int k = 1; k <= nIn_; ++k) ctx_->define("in" + std::to_string(k), {});   // nil until a value comes
        }
        return ctx_;
    }
    std::string title() const override {
        return file_.empty() ? "code " + id : file_.substr(file_.find_last_of("/\\") + 1);
    }
    Patch* patchOf() const override { return patch; }

private:
    void run() {
        auto ctx = context();
        if (ctx->busy()) { patch->print("code: still running (stop ends it)"); return; }
        std::string src, where;
        if (!file_.empty()) {
            where = file();
            std::ifstream f(where, std::ios::binary);
            if (!f) { patch->print("code: can't read " + file_); return; }
            std::stringstream ss;
            ss << f.rdbuf();
            src = ss.str();
        } else {
            src = script_;
            // a name in the patch's folder: errors say "code c1", and relative paths start there
            std::string dir = dirOf(patch->resolvePath("x"));
            where = (dir.empty() ? std::string(".") : dir) + "/code " + id;
        }
        if (src.find_first_not_of(" \t\r\n") == std::string::npos) {
            patch->print("code: no script yet (double-click the box to write one)");
            return;
        }
        for (int k = 0; k < nIn_; ++k)
            if (!values_[(size_t)k].empty()) ctx->define("in" + std::to_string(k + 1), values_[(size_t)k]);
        auto alive = alive_;
        ctx->eval(src, where, 1, [this, alive](const EvalResult& r) {
            if (!*alive) return;
            if (!r.ok && !r.stopped) patch->print("code " + id + ": error: " + r.error);
            if (r.stopped) patch->print("code " + id + ": stopped");
            outletBang(nOut_);
        });
    }

    int nIn_ = 1, nOut_ = 1;
    std::string file_, script_;
    std::vector<Message> values_;
    std::shared_ptr<LangContext> ctx_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

void registerCodeObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"code", "Code", "a Musil script: bang runs it; inlets set in1, in2...; (out k ...) to outlet k: [inlets] [outlets] [file.mu]",
         with<CodeBox>()},
    });
}

}  // namespace moritz
