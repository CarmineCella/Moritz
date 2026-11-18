// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// guitest.cpp — what a test does in the interface: moritz [patch] --window-png out.png act=... act=...
//
// The actions run in order, as the user would do them, through the same code (menus, the canvas's
// events, the code view); tools/test-gui.sh runs them, under the address sanitizer too. Questions are
// answered answer=<n> (batch mode, gui.h). What a test checks is printed on stdout as "state: ...".
//
//   act=draw                  draw the front document (as the screen would: animated boxes start)
//   act=wait:<seconds>        run the event loop that long (timers fire)
//   act=close                 close the front document window, unsaved changes discarded
//   act=confirmclose          the close box of the front document (questions: answer=<n>)
//   act=save, act=undo, act=redo   File > Save, Edit > Undo, Redo on the front document
//   act=opensub:<id>          the window of a subpatcher or abstraction box
//   act=help:<class>          open the help patch of a class
//   act=new:<class>           Patch > New: a box at the mouse in the front patch window
//   act=text:<file>           the code view of the front document: its text replaced by the file's, applied
//   act=click:<id>            a click on a box of the front document (left part)
//   act=keys:<text>           typed into the front document's canvas (',' is Enter)
//   act=send:<id>:<message>   a message to a box of the front document
//   act=codeedit:<text>       text added at the end of the front code editor
//   act=codesave              the front code editor's Save
//   act=state                 print: the documents, the front one's state, the code editors
//   act=buffer:<name>         print: the frames of a buffer~
//   act=boxes                 print: the boxes of the front document

#include "gui.h"
#include "codewindow.h"
#include "objects.h"

#include <FL/Fl_Image_Surface.H>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace moritz {

namespace {

void state(const char* fmt, const std::string& a = "", const std::string& b = "") {
    std::printf("state: ");
    std::printf(fmt, a.c_str(), b.c_str());
    std::printf("\n");
    std::fflush(stdout);
}

// the front window showing a patch is a help patch
bool helpInFront() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (dynamic_cast<MainWindow*>(w)) return false;
        if (auto* pw = dynamic_cast<PatchWindow*>(w)) return pw->help;
    }
    return false;
}

MainWindow* front() {
    App& app = App::get();
    return app.docs.empty() ? nullptr : app.docs.front();
}

void waitFor(double seconds) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) Fl::wait(0.01);
}

void mouse(Canvas* c, int ex, int ey, int event, int clicks) {
    Fl::e_x = Fl::e_x_root = ex;
    Fl::e_y = Fl::e_y_root = ey;
    Fl::e_state = 0;
    Fl::e_clicks = clicks;
    Fl::e_is_click = 1;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_number = event;
    c->handle(event);
}

void key(Canvas* c, char ch) {
    static char text[2];
    text[0] = ch == ',' ? '\r' : ch;
    text[1] = 0;
    Fl::e_text = text;
    Fl::e_length = 1;
    Fl::e_state = 0;
    Fl::e_keysym = ch == ',' ? FL_Enter : (unsigned char)ch;
    Fl::e_number = FL_KEYBOARD;
    c->handle(FL_KEYBOARD);
}

std::string escape(const std::string& t) {
    std::string r;
    for (char c : t) r += c == '\n' ? std::string("\\n") : std::string(1, c);
    return r;
}

bool act(const std::string& a) {
    App& app = App::get();
    std::string verb = a.substr(0, a.find(':')), arg = a.find(':') == std::string::npos ? "" : a.substr(a.find(':') + 1);
    if (verb == "draw") {
        MainWindow* d = front();
        if (!d) return false;
        Fl_Image_Surface surf(d->w(), d->h());
        Fl_Surface_Device::push_current(&surf);
        surf.draw(d);
        Fl_Surface_Device::pop_current();
        delete surf.image();
        return true;
    }
    if (verb == "wait") { waitFor(std::atof(arg.c_str())); return true; }
    if (verb == "close") {
        MainWindow* d = front();
        if (!d) return false;
        app.closeDocument(d);
        return true;
    }
    if (verb == "confirmclose") {          // the close box: asks, then closes (or not)
        MainWindow* d = front();
        if (!d) return false;
        if (d->confirmDiscard()) app.closeDocument(d);
        return true;
    }
    if (verb == "save" || verb == "undo" || verb == "redo") {
        MainWindow* d = app.frontDoc();
        if (!d) return false;
        if (verb == "save") d->save();
        else if (verb == "undo") d->undo();
        else d->redo();
        return true;
    }
    if (verb == "opensub") {
        MainWindow* d = front();
        Box* b = d ? d->patch.find(arg) : nullptr;
        if (!b || !b->inner()) return false;
        d->openSubpatch(b);
        return true;
    }
    if (verb == "help") { app.openHelp(arg); return true; }
    if (verb == "new") {
        Canvas* c = app.activeCanvas();
        if (!c) return false;
        c->newBoxAtMouse(arg);
        c->commitEdit();
        return true;
    }
    if (verb == "text") {
        MainWindow* d = front();
        std::string src;
        if (!d || !readFile(arg, src)) return false;
        app.showText(d, &d->patch, "patch", false);
        PatchTextWindow* tw = app.textWindow(&d->patch);
        if (!tw) return false;
        tw->replaceText(src);
        tw->applyNow();
        return true;
    }
    if (verb == "click" || verb == "dblclick") {
        MainWindow* d = front();
        Box* b = d ? d->patch.find(arg) : nullptr;
        if (!b) return false;
        Canvas* c = d->canvas;
        int ex = c->screenX(b->x + 6), ey = c->screenY(b->y + b->h / 2);
        int clicks = verb == "dblclick" ? 1 : 0;
        mouse(c, ex, ey, FL_PUSH, clicks);
        mouse(c, ex, ey, FL_RELEASE, clicks);
        return true;
    }
    if (verb == "keys") {
        MainWindow* d = front();
        if (!d) return false;
        for (char ch : arg) key(d->canvas, ch);
        return true;
    }
    if (verb == "send") {
        MainWindow* d = front();
        size_t k = arg.find(':');
        Box* b = d ? d->patch.find(arg.substr(0, k)) : nullptr;
        if (!b || k == std::string::npos) return false;
        b->message(0, parseMessage(arg.substr(k + 1)));
        return true;
    }
    if (verb == "codeedit" || verb == "codesave") {
        if (app.codeWindows().empty()) return false;
        CodeWindow* cw = app.codeWindows().front();
        if (verb == "codesave") return cw->save();
        Fl_Text_Buffer* buf = cw->editor()->buffer();
        buf->append(arg.c_str());
        return true;
    }
    if (verb == "state") {
        MainWindow* d = front();
        state("documents %s", std::to_string(app.docs.size()));
        if (d) {
            state("front %s edited %s", d->path().empty() ? "Untitled" : d->path(), d->edited() ? "yes" : "no");
            state("undo %s redo %s", d->canUndo() ? "yes" : "no", d->canRedo() ? "yes" : "no");
            state("boxes %s", std::to_string(d->patch.boxes.size()));
        }
        state("help in front %s", helpInFront() ? "yes" : "no");
        for (auto* cw : app.codeWindows()) {
            char* t = cw->editor()->buffer()->text();
            std::string text = t;
            std::free(t);
            state("code bound %s edited %s", cw->source() ? cw->source()->title() : "none", cw->dirty() ? "yes" : "no");
            state("code text %s", escape(text));
        }
        return true;
    }
    if (verb == "buffer") {
        MainWindow* d = front();
        BufferBox* b = d ? findBuffer(&d->patch, arg) : nullptr;
        state("buffer %s frames %s", arg, b ? std::to_string(b->buf.frames) : std::string("none"));
        return true;
    }
    if (verb == "boxes") {
        MainWindow* d = front();
        if (!d) return false;
        for (auto& b : d->patch.boxes) state("box %s %s", b->id, b->label());
        return true;
    }
    return false;
}

}  // namespace

int runTestActions(int argc, char** argv) {
    int failed = 0;
    for (int k = 1; k < argc; ++k) {
        std::string a = argv[k];
        if (a.rfind("answer=", 0) == 0) setBatchAnswer(std::atoi(a.c_str() + 7));
        if (a.rfind("act=", 0) != 0) continue;
        if (!act(a.substr(4))) {
            std::printf("failed: %s\n", a.c_str());
            std::fflush(stdout);
            ++failed;
        }
        Fl::wait(0);              // what the action scheduled for the event loop (code editors settle)
        Fl::wait(0);
    }
    return failed;
}

}  // namespace moritz
