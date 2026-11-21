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
//   act=soundopen:<file>      File > Open Sound... with that file
//   act=sound                 print: the front sound window (its title, frames, channels, selection, playhead, undo)
//   act=sounddrag:<s>,<s>[:shift]   the mouse pressed on the front sound window's waveform at a time (seconds),
//                             dragged to the other and released (shift: Shift held at the press)
//   act=soundop:<op>          an edit of its Edit menu (cut copy paste delete trim silence fadein fadeout normalize
//                             reverse selectall undo redo), or play stop fit zoomin zoomout
//   act=soundgain:<dB>        the Gain dialog answered
//   act=soundkey:<key>[:cmd][:shift]   a key typed in the front sound window (space, delete, or a letter)
//   act=soundsave             its File > Save (saveas=<path> answers a Save As)
//   act=soundspec             waits for its spectrogram (at most 20 s), then prints how many frames it has
//   act=soundpng:<file>       the front sound window drawn into a PNG file
//   act=datasets...           the Datasets window (datasets_ui.cpp: datasets, datasetsfolder, datasetsregister,
//                             datasetsforget, datasetsanalyse)
//   act=soundsample:<frame>   print: the sample at that frame (channel 0)
//   act=soundtool:<tool>      select, draw or marker
//   act=soundzoom:<f>,<s>     the view: f frames a pixel, from second s
//   act=sounddraw:<s>,<v>/<s>,<v>/...[:<ch>]   with the mouse on channel ch's lane (0): pressed at (second, value),
//                             dragged through the others, released at the last
//   act=soundpinch:<dy>       a trackpad's pinch (FL_ZOOM_GESTURE, dy = magnification x 1000) over the middle
//   act=soundmarks            print: the markers and the regions
//   act=soundmark:<op>        the Markers menu and the regions panel: add onsets beats novelty types regiononsets
//                             addregion clear clearregions delete:<k> deleteregion:<k> rename:<k>:<name>
//                             row:<k> (a click on row k of the table) rowdouble:<k> move:<k>:<s> (marker k dragged
//                             in its lane to second s) lane:<s> (a click in the markers lane) coll (To coll; saveas=)
//                             score (To score; saveas=) toscore (print: the button's state)
//   act=process:<kind>[:k=v,...]   a Process menu item run with these settings (no dialog)
//   act=processdialog:<kind>  its dialog (batch mode: what it would ask is printed)
//   act=processpatch:<file>, act=processscript:<file>   A patch..., A Musil script...
//   act=processresult:<new|replace>   the menu's Result
//   act=processwait           waits (at most 60 s) for the process or analysis, then prints how it ended
//   act=processstop           Process > Stop
//   act=soundmenus            print: the items of the Process and Markers menus
//   act=tl...:..., act=timeline   the timeline (timeline_window.cpp lists them)
//   act=roll:<id>             the roll's window of a score box of the front document (as a double-click opens it)
//   act=rolldrag:<event>:<seconds>,<steps>[:alt]   in the newest roll window, an event dragged by its head that far
//                             in time and that many steps up its staff (alt: Alt held, a copy)
//   act=rollundo, act=rollredo   the roll's Undo and Redo
//   act=rollview:roll|piano|list   the roll's view; act=rollpng:<file>: its window drawn into a PNG file
//   act=rolltext:<file>       the roll's text view given the file's text and applied (an error is printed)
//   act=rollstate             print: the roll's title, its notes (id at dur pitch) and the lines of its text with a note
//   act=boxdrag:<box>:<event>:<dx>,<dy>   on the canvas, an event of a score box's own roll dragged by its head that
//                             many pixels (the patch locked: the box edited in place)

#include "gui.h"
#include "codewindow.h"
#include "datasets.h"
#include "fileio.h"
#include "objects.h"
#include "roll.h"
#include "sound_editor.h"
#include "sound_process.h"
#include "timeline.h"

#include <map>

#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_PNG_Image.H>

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

SoundWindow* frontSound() {
    App& app = App::get();
    if (SoundWindow* s = app.frontSound()) return s;
    return app.soundWindows().empty() ? nullptr : app.soundWindows().back();
}

void soundMouseAt(Fl_Widget* v, int ex, int ey, int event, int keys, int clicks = 0) {
    Fl::e_x = Fl::e_x_root = ex;
    Fl::e_y = Fl::e_y_root = ey;
    Fl::e_state = keys | (event == FL_RELEASE ? 0 : FL_BUTTON1);
    Fl::e_clicks = clicks;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_number = event;
    v->handle(event);
}

void soundMouse(SoundView* v, int ex, int event, int keys) {
    int wy, wh, sy, sh;
    v->areas(wy, wh, sy, sh);
    soundMouseAt(v, ex, wh > 0 ? wy + wh / 2 : sy + sh / 2, event, keys);
}

std::string menuItems(const Fl_Menu_Button* m) {
    std::string t;
    for (int k = 0; m->menu() && k < m->size() - 1; ++k) {
        const Fl_Menu_Item& it = m->menu()[k];
        if (!it.label()) continue;
        if (!t.empty()) t += " | ";
        t += it.label();
        if (!it.active()) t += " (inactive)";
        if ((it.flags & FL_MENU_RADIO) && it.value()) t += " (on)";
    }
    return t;
}

std::string escape(const std::string& t) {
    std::string r;
    for (char c : t) r += c == '\n' ? std::string("\\n") : std::string(1, c);
    return r;
}

bool act(const std::string& a) {
    App& app = App::get();
    std::string verb = a.substr(0, a.find(':')), arg = a.find(':') == std::string::npos ? "" : a.substr(a.find(':') + 1);
    if (verb == "timeline" || verb.rfind("tl", 0) == 0) return timelineTestAction(verb, arg);   // timeline.h
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
    if (verb == "roll") {
        DocumentWindow* d = front();
        auto* src = d ? dynamic_cast<ScoreSource*>(d->patch.find(arg)) : nullptr;
        return src && openRoll(src);
    }
    if (verb.rfind("roll", 0) == 0) {
        RollWindow* w = RollWindow::all().empty() ? nullptr : RollWindow::all().back();
        if (!w) return false;
        RollPanel* panel = w->panel();
        if (verb == "rollpng") {
            Fl_Image_Surface surf(w->w(), w->h());
            Fl_Surface_Device::push_current(&surf);
            surf.draw(w);
            Fl_Surface_Device::pop_current();
            Fl_RGB_Image* img = surf.image();
            int r = fl_write_png(arg.c_str(), img);
            delete img;
            return r == 0;
        }
        if (verb == "rollview") {           // act=rollview:roll|piano|list
            panel->setView(arg == "piano" ? RollPanel::View::Piano : arg == "list" ? RollPanel::View::List : RollPanel::View::Roll);
            return true;
        }
        if (verb == "rollundo") { panel->undo(); return true; }
        if (verb == "rollredo") { panel->redo(); return true; }
        if (verb == "rolltext") {
            std::string src;
            if (!fileio::read(arg, src)) return false;
            panel->setText(src);
            if (!panel->applyText()) state("roll text error %s", panel->textError());
            return true;
        }
        if (verb == "rollstate") {
            RollEditTarget* t = panel->target();
            state("roll %s", w->label() ? w->label() : "");
            auto sc = t ? t->score() : nullptr;
            if (sc)
                for (auto* e : sc->byTime())
                    if (e->kind == musil::score::Kind::Note)
                        state("roll note %s %s", std::to_string(e->id), musil::score::numText(e->at) + " " + musil::score::numText(e->dur) + " " +
                                                                         musil::score::numText(e->pitch));
            std::string text = panel->text();
            for (size_t k = 0; k < text.size();) {
                size_t end = text.find('\n', k);
                std::string line = text.substr(k, end == std::string::npos ? std::string::npos : end - k);
                size_t at = line.find("(note");
                if (at != std::string::npos) state("roll text %s", line.substr(at));
                k = end == std::string::npos ? text.size() : end + 1;
            }
            return true;
        }
        if (verb == "rolldrag") {
            uint64_t id = std::strtoull(arg.c_str(), nullptr, 10);
            size_t c1 = arg.find(':');
            if (c1 == std::string::npos) return false;
            double dt = std::atof(arg.c_str() + c1 + 1);
            size_t comma = arg.find(',', c1);
            double steps = comma == std::string::npos ? 0 : std::atof(arg.c_str() + comma + 1);
            int keys = arg.find(":alt") != std::string::npos ? FL_ALT : 0;
            RollWidget* rw = panel->rollWidget();
            RollCore& c = rw->core();
            double x, y, ww, h;
            if (!c.eventBox(id, x, y, ww, h)) return false;
            int row = c.hit(x + h / 2, y + h / 2).row;
            double sp = row >= 0 && row < (int)c.rows().size() ? c.rows()[(size_t)row].sp : 0;
            int ex = rw->x() + (int)std::lround(x + h / 2), ey = rw->y() + (int)std::lround(y + h / 2);
            int ex2 = ex + (int)std::lround(dt * c.pps), ey2 = ey - (int)std::lround(steps * sp / 2);
            auto send = [&](int event, int px, int py) {
                Fl::e_x = Fl::e_x_root = px;
                Fl::e_y = Fl::e_y_root = py;
                Fl::e_state = keys | (event == FL_RELEASE ? 0 : FL_BUTTON1);
                Fl::e_clicks = 0;
                Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
                Fl::e_number = event;
                rw->handle(event);
            };
            send(FL_PUSH, ex, ey);
            send(FL_DRAG, (ex + ex2) / 2, (ey + ey2) / 2);
            send(FL_DRAG, ex2, ey2);
            send(FL_RELEASE, ex2, ey2);
            panel->changed();
            return true;
        }
        return false;
    }
    if (verb == "boxdrag") {
        DocumentWindow* d = front();
        size_t c1 = arg.find(':'), c2 = c1 == std::string::npos ? c1 : arg.find(':', c1 + 1);
        if (!d || c2 == std::string::npos) return false;
        Box* b = d->patch.find(arg.substr(0, c1));
        auto* src = dynamic_cast<ScoreSource*>(b);
        RollCore* core = src ? src->inlineRoll() : nullptr;
        if (!core) return false;
        uint64_t id = std::strtoull(arg.c_str() + c1 + 1, nullptr, 10);
        int dx = std::atoi(arg.c_str() + c2 + 1);
        size_t comma = arg.find(',', c2);
        int dy = comma == std::string::npos ? 0 : std::atoi(arg.c_str() + comma + 1);
        double x, y, w, h;
        if (!core->eventBox(id, x, y, w, h)) return false;
        Canvas* c = d->canvas;
        int px = b->x + 1 + (int)std::lround(x + h / 2), py = b->y + kBoxH + (int)std::lround(y + h / 2);
        mouse(c, c->screenX(px), c->screenY(py), FL_PUSH, 0);
        mouse(c, c->screenX(px + dx / 2), c->screenY(py + dy / 2), FL_DRAG, 0);
        mouse(c, c->screenX(px + dx), c->screenY(py + dy), FL_DRAG, 0);
        mouse(c, c->screenX(px + dx), c->screenY(py + dy), FL_RELEASE, 0);
        return true;
    }
    if (verb == "soundopen") return app.openSound(arg) != nullptr;
    if (verb.rfind("sound", 0) == 0 || verb.rfind("process", 0) == 0) {
        SoundWindow* w = frontSound();
        if (!w) return false;
        SoundEditor* e = w->editor();
        SoundDoc& d = e->doc();
        if (verb == "sound") {
            state("sound %s", w->label() ? w->label() : "");
            state("sound frames %s channels %s", std::to_string(d.frames()), std::to_string(d.channels()));
            state("sound selection %s playhead %s", std::to_string(d.sel.start) + " " + std::to_string(d.sel.end),
                  std::to_string(d.playhead));
            state("sound undo %s edited %s", d.canUndo() ? "yes" : "no", d.needsSaving() ? "yes" : "no");
            state("sound loop %s view %s", e->loop() ? "yes" : "no", std::to_string(e->framesPerPixel()));
            e->refreshInfo();
            state("sound info%s", e->info());
            state("sound status %s", e->status());
            return true;
        }
        if (verb == "sounddrag") {
            size_t comma = arg.find(',');
            if (comma == std::string::npos) return false;
            double a = std::atof(arg.c_str()), b = std::atof(arg.c_str() + comma + 1);
            int keys = arg.find(":shift") != std::string::npos ? FL_SHIFT : 0;
            SoundView* v = e->view();
            soundMouse(v, e->xOf(a * d.sr()), FL_PUSH, keys);
            soundMouse(v, e->xOf((a + b) / 2 * d.sr()), FL_DRAG, keys);
            soundMouse(v, e->xOf(b * d.sr()), FL_DRAG, keys);
            soundMouse(v, e->xOf(b * d.sr()), FL_RELEASE, keys);
            return true;
        }
        if (verb == "soundop") {
            static const std::pair<const char*, void (SoundEditor::*)()> ops[] = {
                {"cut", &SoundEditor::cut}, {"copy", &SoundEditor::copy}, {"paste", &SoundEditor::paste},
                {"delete", &SoundEditor::erase}, {"trim", &SoundEditor::trim}, {"silence", &SoundEditor::silence},
                {"fadein", &SoundEditor::fadeIn}, {"fadeout", &SoundEditor::fadeOut},
                {"normalize", &SoundEditor::normalize}, {"reverse", &SoundEditor::reverse},
                {"selectall", &SoundEditor::selectAll}, {"undo", &SoundEditor::undo}, {"redo", &SoundEditor::redo},
                {"play", &SoundEditor::play}, {"stop", &SoundEditor::stop}, {"fit", &SoundEditor::fit},
                {"zoomin", &SoundEditor::zoomIn}, {"zoomout", &SoundEditor::zoomOut}};
            for (auto& o : ops)
                if (arg == o.first) {
                    (e->*o.second)();
                    return true;
                }
            return false;
        }
        if (verb == "soundgain") { e->gain(std::atof(arg.c_str())); return true; }
        if (verb == "soundkey") {
            std::string k = arg.substr(0, arg.find(':'));
            static char text[2];
            int key = k == "space" ? ' ' : k == "delete" ? FL_BackSpace : (unsigned char)k[0];
            text[0] = key < 128 ? (char)key : 0;
            text[1] = 0;
            Fl::e_text = text;
            Fl::e_length = text[0] ? 1 : 0;
            Fl::e_state = (arg.find(":cmd") != std::string::npos ? FL_COMMAND : 0) |
                          (arg.find(":shift") != std::string::npos ? FL_SHIFT : 0);
            Fl::e_keysym = key;
            Fl::e_number = FL_KEYBOARD;
            Fl::first_window(w);
            e->handle(FL_KEYBOARD);
            return true;
        }
        if (verb == "soundsave") return w->save() || true;
        if (verb == "soundspec") {
            auto t0 = std::chrono::steady_clock::now();
            while ((!e->spectrogram() || e->spectrogramPending()) &&
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20)
                Fl::wait(0.02);
            Fl::wait(0.05);
            state("spectrogram frames %s", std::to_string(e->spectrogram() ? e->spectrogram()->cols : -1));
            return true;
        }
        if (verb == "soundsample") {             // print: a sample's value (frame, channel 0)
            long f = std::atol(arg.c_str());
            char b[64];
            std::snprintf(b, sizeof b, "%.4f", f >= 0 && f < d.frames() ? d.sound()->data[(size_t)f * d.channels()] : 0.f);
            state("sound sample %s %s", std::to_string(f), b);
            return true;
        }
        if (verb == "soundtool") {
            e->setTool(arg == "draw" ? SoundEditor::Tool::Draw : arg == "marker" ? SoundEditor::Tool::Marker : SoundEditor::Tool::Select);
            return true;
        }
        if (verb == "soundzoom") {
            size_t comma = arg.find(',');
            e->setView(comma == std::string::npos ? 0 : std::atof(arg.c_str() + comma + 1) * d.sr(), std::atof(arg.c_str()));
            return true;
        }
        if (verb == "sounddraw") {
            int ch = 0;
            std::string pts = arg;
            if (size_t c = arg.rfind(':'); c != std::string::npos) {
                ch = std::atoi(arg.c_str() + c + 1);
                pts = arg.substr(0, c);
            }
            std::vector<std::pair<double, double>> p;
            for (size_t a = 0; a < pts.size();) {
                size_t b = pts.find('/', a);
                std::string one = pts.substr(a, b == std::string::npos ? std::string::npos : b - a);
                size_t comma = one.find(',');
                if (comma != std::string::npos) p.push_back({std::atof(one.c_str()), std::atof(one.c_str() + comma + 1)});
                if (b == std::string::npos) break;
                a = b + 1;
            }
            if (p.empty()) return false;
            SoundView* v = e->view();
            for (size_t k = 0; k < p.size(); ++k)
                soundMouseAt(v, e->xOf(p[k].first * d.sr()), v->valueY(ch, p[k].second), k == 0 ? FL_PUSH : FL_DRAG, 0);
            soundMouseAt(v, e->xOf(p.back().first * d.sr()), v->valueY(ch, p.back().second), FL_RELEASE, 0);
            return true;
        }
        if (verb == "soundpinch") {
            SoundView* v = e->view();
            Fl::e_x = Fl::e_x_root = v->laneX() + v->laneW() / 2;
            Fl::e_y = Fl::e_y_root = v->y() + v->h() / 2;
            Fl::e_dy = std::atoi(arg.c_str());
            Fl::e_number = FL_ZOOM_GESTURE;
            return e->handle(FL_ZOOM_GESTURE) != 0;
        }
        if (verb == "soundmarks") {
            const SoundMarks& m = d.marks();
            std::string t;
            for (auto& k : m.markers) t += " " + std::to_string(k.at) + ":" + k.name;
            state("sound markers %s%s", std::to_string(m.markers.size()), t);
            for (size_t k = 0; k < m.regions.size(); ++k) {
                const Region& r = m.regions[k];
                char b[200];
                std::snprintf(b, sizeof b, " %ld %ld type %s energy %.2f pitch %s", r.start, r.end, typeName(r.type).c_str(),
                              r.energy, pitchName(r.pitch).c_str());
                state("sound region %s%s", std::to_string(k + 1) + " " + r.name, b);
            }
            state("sound regions %s selected %s", std::to_string(m.regions.size()), std::to_string(e->selectedRegion() + 1));
            return true;
        }
        if (verb == "soundmark") {
            std::string op = arg.substr(0, arg.find(':')), rest = arg.find(':') == std::string::npos ? "" : arg.substr(arg.find(':') + 1);
            int k = std::atoi(rest.c_str()) - 1;
            if (op == "add") e->addMarkerAtPlayhead();
            else if (op == "onsets" || op == "beats" || op == "novelty") e->markersFrom(op);
            else if (op == "types") e->regionsFrom("types");
            else if (op == "regiononsets") e->regionsFrom("onsets");
            else if (op == "addregion") e->addRegion();
            else if (op == "clear") e->clearMarkers();
            else if (op == "clearregions") e->clearRegions();
            else if (op == "delete") e->deleteMarker(k);
            else if (op == "deleteregion") e->deleteRegion(k);
            else if (op == "rename") e->renameRegion(k, rest.substr(rest.find(':') + 1));
            else if (op == "row" || op == "rowdouble") {
                RegionTable* t = e->regionTable();
                int y = t->y() + RegionTable::kRowH * (k + 1) + 4;
                soundMouseAt(t, t->x() + 40, y, FL_PUSH, 0, op == "rowdouble" ? 1 : 0);
                soundMouseAt(t, t->x() + 40, y, FL_RELEASE, 0);
            } else if (op == "move") {
                const auto& ms = d.marks().markers;
                if (k < 0 || k >= (int)ms.size()) return false;
                SoundView* v = e->view();
                int y = v->markersY() + SoundView::kMarkersH / 2, to = e->xOf(std::atof(rest.substr(rest.find(':') + 1).c_str()) * d.sr());
                soundMouseAt(v, e->xOf((double)ms[(size_t)k].at), y, FL_PUSH, 0);
                soundMouseAt(v, to, y, FL_DRAG, 0);
                soundMouseAt(v, to, y, FL_RELEASE, 0);
            } else if (op == "lane") {
                SoundView* v = e->view();
                int y = v->markersY() + SoundView::kMarkersH / 2, x = e->xOf(std::atof(rest.c_str()) * d.sr());
                soundMouseAt(v, x, y, FL_PUSH, 0);
                soundMouseAt(v, x, y, FL_RELEASE, 0);
            } else if (op == "coll") e->toColl();
            else if (op == "score") e->toScore();
            else if (op == "toscore") state("sound to score %s", e->toScoreButton()->active() ? "active" : "inactive");
            else return false;
            return true;
        }
        if (verb == "soundmenus") {
            e->refreshMenus();
            state("sound process menu: %s", menuItems(e->processMenu()));
            state("sound markers menu: %s", menuItems(e->marksMenu()));
            return true;
        }
        if (verb == "process") {
            std::string kind = arg.substr(0, arg.find(':'));
            std::map<std::string, std::string> params;
            if (size_t c = arg.find(':'); c != std::string::npos) {
                std::string rest = arg.substr(c + 1);
                for (size_t a = 0; a < rest.size();) {
                    size_t b = rest.find(',', a);
                    std::string kv = rest.substr(a, b == std::string::npos ? std::string::npos : b - a);
                    size_t eq = kv.find('=');
                    if (eq != std::string::npos) params[kv.substr(0, eq)] = kv.substr(eq + 1);
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
            }
            e->runProcess(kind, params);
            return true;
        }
        if (verb == "processdialog") { e->processDialog(arg); return true; }
        if (verb == "processpatch") { e->processPatch(arg); return true; }
        if (verb == "processscript") { e->processScript(arg); return true; }
        if (verb == "processresult") { e->setResultNew(arg == "new"); return true; }
        if (verb == "processstop") { e->stopWork(); return true; }
        if (verb == "processwait") {
            auto t0 = std::chrono::steady_clock::now();
            while (e->working() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) Fl::wait(0.02);
            Fl::wait(0.05);
            state("process %s", e->lastWork());
            return true;
        }
        if (verb == "soundpng") {
            Fl_Image_Surface surf(w->w(), w->h());
            Fl_Surface_Device::push_current(&surf);
            surf.draw(w);
            Fl_Surface_Device::pop_current();
            Fl_RGB_Image* img = surf.image();
            int r = fl_write_png(arg.c_str(), img);
            delete img;
            return r == 0;
        }
        return false;
    }
    if (verb == "boxes") {
        DocumentWindow* d = front();
        if (!d) return false;
        for (auto& b : d->patch.boxes) state("box %s %s", b->id, b->label());
        return true;
    }
    if (verb.rfind("datasets", 0) == 0) return datasetsTestAction(verb, arg);   // datasets_ui.cpp
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
