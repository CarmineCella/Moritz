// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// patch_window.cpp — the window of a subpatcher, an abstraction or a pfft~ patch

#include "gui.h"
#include "ui_util.h"

namespace moritz {

PatchWindow::PatchWindow(DocumentWindow* d, Patch* p, const std::string& title)
    : Fl_Double_Window(560, 420), patch(p), doc(d) {
    copy_label(title.c_str());
    canvas = new Canvas(0, 0, 560, 420 - kStatusH, d, p);   // a document's patches are edited through it
    auto* bar = new Fl_Group(0, 420 - kStatusH, 560, kStatusH);   // the status line
    auto* status = new Fl_Box(0, 420 - kStatusH, 560, kStatusH);
    status->box(FL_FLAT_BOX);
    status->color(kStatusBg);
    status->labelcolor(kDim);
    status->labelsize(11);
    status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    std::string edit = shortcutText(FL_COMMAND + 'e') + " toggles edit mode";
    status->copy_label((p->isAbstraction ? "  abstraction · " + shortcutText(FL_COMMAND + 's') + " saves its file · " + edit
                                         : "  subpatcher · saved with the main patch · " + edit).c_str());
    bar->resizable(status);
    bar->end();
    end();
    resizable(canvas);
    size_range(300, 200);
    canvas->setLocked(true);
    p->onRedraw = [this] {
        canvas->redraw();
        App::get().patchChanged();
    };
    p->onStructure = [this] {
        canvas->redraw();
        App::get().patchChanged();
    };
    callback([](Fl_Widget* w, void*) {
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
        auto* pw = static_cast<PatchWindow*>(w);
        pw->canvas->commitEdit();
        pw->hide();                       // a subpatcher is kept: reopening is instant
    });
}

PatchWindow::~PatchWindow() { detach(); }

void PatchWindow::detach() {
    if (!patch) return;
    patch->onRedraw = nullptr;
    patch->onStructure = nullptr;
    patch = nullptr;
    canvas->detach();     // its timers would walk the freed patch before FLTK deletes the window
}

int PatchWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        canvas->cancelEdit();
        return 1;
    }
    int r = Fl_Double_Window::handle(e);
    if (e == FL_FOCUS || e == FL_SHOW) App::get().refreshMenus();   // what the commands act on changed
    return r;
}

}  // namespace moritz
