// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// patch_window.cpp — the window of a subpatcher, an abstraction, a pfft~ patch or a help patch

#include "gui.h"
#include "ui_util.h"

#include <FL/Fl_Button.H>

namespace moritz {

PatchWindow::PatchWindow(MainWindow* d, Patch* p, const std::string& title, bool isHelp)
    : Fl_Double_Window(560, 420), patch(p), doc(d), help(isHelp) {
    copy_label(title.c_str());
    canvas = new Canvas(0, 0, 560, 420 - kStatusH, d, p);
    auto* bar = new Fl_Group(0, 420 - kStatusH, 560, kStatusH);   // the status line (and Reference)
    auto* status = new Fl_Box(0, 420 - kStatusH, 560, kStatusH);
    status->box(FL_FLAT_BOX);
    status->color(kStatusBg);
    status->labelcolor(kDim);
    status->labelsize(11);
    status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    if (help) {                                       // the reference, next to the example
        auto* ref = new Fl_Button(560 - 96, 420 - kStatusH + 2, 92, kStatusH - 4, "Reference");
        ref->labelsize(11);
        ref->box(FL_FLAT_BOX);
        ref->color(kBoxFill);
        ref->labelcolor(kText);
        ref->tooltip("Every argument, message, inlet and outlet of the object");
        ref->callback([](Fl_Widget* b, void*) {
            auto* pw = static_cast<PatchWindow*>(b->window());
            App::get().showReference(pw->helpClass);
        });
        status->size(560 - 100, kStatusH);
    }
    status->copy_label(help ? "  help · runs with your patches while open · right-click any box for its help"
                       : p->isAbstraction
                           ? "  abstraction · Cmd+S saves its file · Cmd+E toggles edit mode"
                           : "  subpatcher · saved with the main patch · Cmd+E toggles edit mode");
    bar->resizable(status);
    bar->end();
    end();
    resizable(canvas);
    size_range(300, 200);
    canvas->setLocked(true);
    canvas->readOnly = help;
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
        if (pw->help && pw->patch) App::get().closeHelp(pw->patch);   // stops its sound
    });
}

PatchWindow::~PatchWindow() { detach(); }

void PatchWindow::detach() {
    if (!patch) return;
    patch->onRedraw = nullptr;
    patch->onStructure = nullptr;
    patch = nullptr;
}

int PatchWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        canvas->cancelEdit();
        return 1;
    }
    return Fl_Double_Window::handle(e);
}

}  // namespace moritz
