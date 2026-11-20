// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// guitest.cpp — what a test does in the interface: moritz [patch] --window-png out.png act=... act=...
//
// The actions run in order, as the user would do them, through the same code (menus, the canvas's
// events, the code view); tools/test-gui.sh runs them, under the address sanitizer too. Questions are
// answered answer=<n>, a Save As saveas=<path> (none: cancelled) (batch mode, gui.h). What a test checks is
// printed on stdout as "state: ...".
//
//   act=draw                  draw the front document (as the screen would: animated boxes start)
//   act=wait:<seconds>        run the event loop that long (timers fire)
//   act=close                 close the front document window, unsaved changes discarded
//   act=confirmclose          the close box of the front document (questions: answer=<n>)
//   act=closefront            File > Close on the front window (questions: answer=<n>)
//   act=save, act=undo, act=redo   File > Save, Edit > Undo, Redo on the front document
//   act=opensub:<id>          the window of a subpatcher or abstraction box
//   act=help:<class>          open the help patch of a class
//   act=menu:<command>        run a menu command by its id (menus.cpp), if it is enabled now
//   act=new:<class>           Patch > New: a box at the mouse in the front patch window
//   act=text:<file>           the code view of the front document: its text replaced by the file's, applied
//   act=type:<file>           the same, as if typed: applied when typing pauses (wait for it)
//   act=click:<id>            a click on a box of the front document (left part)
//   act=drag:<x>,<y>/<x>,<y>/...[:shift]   on the front patch window's canvas: pressed at the first point,
//                             dragged through the others (one event each, however far apart), released at
//                             the last; in patch coordinates; shift: Shift held while dragging and at release
//   act=drag:...:hold         the same, not released yet
//   act=release:<x>,<y>[:shift]   the button released there
//   act=lit                   print: the cable a box dragged with Shift would go into ("lit none": none)
//   act=zoom:<z>              the front patch window's zoom (1 = 100%)
//   act=pos:<id>              print: where a box of the front patch window is
//   act=cables                print: the cables of the front patch window ("cable <from> <outlet> <to> <inlet>")
//   act=keys:<text>           typed into the front document's canvas (',' is Enter)
//   act=send:<id>:<message>   a message to a box of the front document
//   act=codeedit:<text>       text added at the end of the front code editor
//   act=codesave              the front code editor's Save
//   act=raise:code            the first code editor's window comes to the front, as a click on it does
//   act=raise:doc             the first document's window, the same
//   act=raise:last            the newest document's window, the same
//   act=menuitem:<command>    print: the command's item in the menu bar, active or not
//   act=state                 print: the documents, the front one's state, the code editors
//   act=canvas                print: the front patch window, locked or not, its boxes, the help patch it shows
//   act=buffer:<name>         print: the frames of a buffer~
//   act=boxes                 print: the boxes of the front document

#include "gui.h"
#include "codewindow.h"
#include "fileio.h"
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

DocumentWindow* front() {
    App& app = App::get();
    return app.docs.empty() ? nullptr : app.docs.front();
}

void waitFor(double seconds) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) Fl::wait(0.01);
}

void mouse(Canvas* c, int ex, int ey, int event, int clicks, int keys = 0) {
    Fl::e_x = Fl::e_x_root = ex;
    Fl::e_y = Fl::e_y_root = ey;
    Fl::e_state = keys | (event == FL_RELEASE ? 0 : FL_BUTTON1);
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
        DocumentWindow* d = front();
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
        DocumentWindow* d = front();
        if (!d) return false;
        app.closeDocument(d);
        return true;
    }
    if (verb == "closefront") { app.closeFront(); return true; }
    if (verb == "confirmclose") {          // the close box: asks, then closes (or not)
        DocumentWindow* d = front();
        if (!d) return false;
        if (d->confirmDiscard()) app.closeDocument(d);
        return true;
    }
    if (verb == "save" || verb == "undo" || verb == "redo") {
        DocumentWindow* d = app.frontDoc();
        if (!d) return false;
        if (verb == "save") d->saveFront();
        else if (verb == "undo") d->undo();
        else d->redo();
        return true;
    }
    if (verb == "opensub") {
        DocumentWindow* d = front();
        Box* b = d ? d->patch.find(arg) : nullptr;
        if (!b || !b->inner()) return false;
        d->openSubpatch(b);
        return true;
    }
    if (verb == "help") { app.openHelp(arg); return true; }
    if (verb == "menu") {
        const Command* c = findCommand(arg);
        if (!c || !c->run) return false;
        if (c->enabled && !c->enabled()) { state("menu %s disabled", arg); return true; }
        c->run();
        app.refreshMenus();
        return true;
    }
    if (verb == "new") {
        Canvas* c = app.activeCanvas();
        if (!c) return false;
        c->newBoxAtMouse(arg);
        c->commitEdit();
        return true;
    }
    if (verb == "text" || verb == "type") {
        DocumentWindow* d = front();
        std::string src;
        if (!d || !fileio::read(arg, src)) return false;
        app.showText(d, &d->patch, "patch", false);
        PatchTextWindow* tw = app.textWindow(&d->patch);
        if (!tw) return false;
        tw->replaceText(src);
        if (verb == "text") tw->applyNow();
        return true;
    }
    if (verb == "click" || verb == "dblclick") {
        DocumentWindow* d = front();
        Box* b = d ? d->patch.find(arg) : nullptr;
        if (!b) return false;
        Canvas* c = d->canvas;
        int ex = c->screenX(b->x + 6), ey = c->screenY(b->y + b->h / 2);
        int clicks = verb == "dblclick" ? 1 : 0;
        mouse(c, ex, ey, FL_PUSH, clicks);
        mouse(c, ex, ey, FL_RELEASE, clicks);
        return true;
    }
    if (verb == "drag") {
        Canvas* c = app.activeCanvas();
        if (!c || !c->patch()) return false;
        std::string path = arg.substr(0, arg.find(':'));
        int keys = arg.find(":shift") != std::string::npos ? FL_SHIFT : 0;
        std::vector<std::pair<int, int>> pts;
        for (size_t k = 0; k < path.size();) {
            size_t end = path.find('/', k);
            std::string p = path.substr(k, end == std::string::npos ? std::string::npos : end - k);
            size_t comma = p.find(',');
            if (comma == std::string::npos) return false;
            pts.push_back({std::atoi(p.c_str()), std::atoi(p.c_str() + comma + 1)});
            k = end == std::string::npos ? path.size() : end + 1;
        }
        if (pts.size() < 2) return false;
        auto at = [&](size_t k, int event, int state) { mouse(c, c->screenX(pts[k].first), c->screenY(pts[k].second), event, 0, state); };
        at(0, FL_PUSH, 0);
        for (size_t k = 1; k < pts.size(); ++k) at(k, FL_DRAG, keys);
        if (arg.find(":hold") == std::string::npos) at(pts.size() - 1, FL_RELEASE, keys);
        return true;
    }
    if (verb == "release") {
        Canvas* c = app.activeCanvas();
        size_t comma = arg.find(',');
        if (!c || comma == std::string::npos) return false;
        int x = std::atoi(arg.c_str()), y = std::atoi(arg.c_str() + comma + 1);
        mouse(c, c->screenX(x), c->screenY(y), FL_RELEASE, 0, arg.find(":shift") != std::string::npos ? FL_SHIFT : 0);
        return true;
    }
    if (verb == "lit") {
        Canvas* c = app.activeCanvas();
        if (!c || !c->patch()) return false;
        int k = c->litCable();
        if (k < 0 || k >= (int)c->patch()->conns.size()) { state("lit none"); return true; }
        auto& x = c->patch()->conns[k];
        state("lit %s %s", x.from->id + " " + std::to_string(x.outlet), x.to->id + " " + std::to_string(x.inlet));
        return true;
    }
    if (verb == "zoom") {
        Canvas* c = app.activeCanvas();
        if (!c) return false;
        c->setZoom(std::atof(arg.c_str()));
        return true;
    }
    if (verb == "pos") {
        Canvas* c = app.activeCanvas();
        Box* b = c && c->patch() ? c->patch()->find(arg) : nullptr;
        if (!b) return false;
        state("pos %s %s", arg, std::to_string(b->x) + " " + std::to_string(b->y));
        return true;
    }
    if (verb == "cables") {
        Canvas* c = app.activeCanvas();
        if (!c || !c->patch()) return false;
        for (auto& k : c->patch()->conns)
            state("cable %s %s", k.from->id + " " + std::to_string(k.outlet), k.to->id + " " + std::to_string(k.inlet));
        state("cables %s", std::to_string(c->patch()->conns.size()));
        return true;
    }
    if (verb == "keys") {
        DocumentWindow* d = front();
        if (!d) return false;
        for (char ch : arg) key(d->canvas, ch);
        return true;
    }
    if (verb == "send") {
        DocumentWindow* d = front();
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
    if (verb == "raise") {
        Fl_Window* w = nullptr;
        if (arg == "code" && !app.codeWindows().empty()) w = app.codeWindows().front();
        if (arg == "doc") w = front();
        if (arg == "last" && !App::get().docs.empty()) w = App::get().docs.back();   // the newest document
        if (!w) return false;
        w->show();
        Fl::first_window(w);
        Fl::handle(FL_FOCUS, w);            // what the system sends the window it brings to the front
        return true;
    }
    if (verb == "menuitem") {
        if (!findCommand(arg)) return false;
        state("menu item %s %s", arg, app.menuItemActive(arg) ? "active" : "inactive");
        return true;
    }
    if (verb == "state") {
        DocumentWindow* d = front();
        state("documents %s", std::to_string(app.docs.size()));
        if (d) {
            state("front %s edited %s", d->path().empty() ? "Untitled" : d->path(), d->doc.anyDirty() ? "yes" : "no");
            state("undo %s redo %s", d->doc.canUndo() ? "yes" : "no", d->doc.canRedo() ? "yes" : "no");
            state("boxes %s", std::to_string(d->patch.boxes.size()));
        }
        state("help in front %s", app.helpInFront() ? "yes" : "no");
        for (auto* cw : app.codeWindows()) {
            char* t = cw->editor()->buffer()->text();
            std::string text = t;
            std::free(t);
            state("code bound %s edited %s", cw->source() ? cw->source()->title() : "none", cw->dirty() ? "yes" : "no");
            state("code text %s", escape(text));
        }
        return true;
    }
    if (verb == "canvas") {
        Canvas* c = app.activeCanvas();
        if (!c || !c->patch()) return false;
        Fl_Window* w = c->top_window();
        state("canvas %s locked %s", w && w->label() ? w->label() : "", c->locked() ? "yes" : "no");
        state("canvas boxes %s", std::to_string(c->patch()->boxes.size()));
        auto* d = dynamic_cast<DocumentWindow*>(w);
        if (d && d->isHelp()) state("canvas help %s", d->doc.helpFile());
        return true;
    }
    if (verb == "buffer") {
        DocumentWindow* d = front();
        BufferBox* b = d ? findBuffer(&d->patch, arg) : nullptr;
        state("buffer %s frames %s", arg, b ? std::to_string(b->buf.frames) : std::string("none"));
        return true;
    }
    if (verb == "boxes") {
        DocumentWindow* d = front();
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
        if (a.rfind("saveas=", 0) == 0) setBatchSavePath(a.substr(7));
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
