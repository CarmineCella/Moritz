// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// document_window.cpp — a document's window: toolbar, canvas, status bar; and what the document does
// through it (files, undo, subpatcher windows). The document itself, without the interface, is
// document.h.

#include "gui.h"
#include "ui_util.h"
#include "settings.h"
#include "codewindow.h"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>

#include <cmath>

namespace moritz {

DocumentWindow::DocumentWindow(int w, int h)
    : Fl_Double_Window(w, h, "Moritz"), doc(*App::get().host.addExtra(std::make_unique<Patch>())), patch(doc.patch) {
#ifndef __APPLE__
    menu_ = new Fl_Menu_Bar(0, 0, w, kMenuH);
    App::get().buildMenu(menu_);
#endif
    auto* bar = new Fl_Group(0, kMenuH, w, kToolH);
    bar->box(FL_FLAT_BOX);
    bar->color(kStatusBg);
    auto tip = [](Fl_Widget* wd, const std::string& text, const char* cmd) { wd->copy_tooltip(withShortcut(text, cmd).c_str()); };
    audioBtn_ = new ToolButton(8, kMenuH + 5, 104, kToolH - 10, ToolButton::Power, "Audio");
    tip(audioBtn_, "Start or stop audio", "patch.audio");
    audioBtn_->callback([](Fl_Widget*, void*) { App::get().toggleAudio(); });
    editBtn_ = new ToolButton(118, kMenuH + 5, 92, kToolH - 10, ToolButton::Pencil, "Edit");
    tip(editBtn_, "Edit mode on/off", "patch.edit");
    editBtn_->callback([](Fl_Widget*, void* d) { static_cast<DocumentWindow*>(d)->toggleLock(); }, this);
    textBtn_ = new ToolButton(216, kMenuH + 5, 86, kToolH - 10, ToolButton::Code, "Code");
    tip(textBtn_, "The patch as code (its text), kept in sync with the drawing", "patch.code");
    textBtn_->callback([](Fl_Widget*, void* d) {
        auto* mw = static_cast<DocumentWindow*>(d);
        App& app = App::get();
        if (auto* tw = app.textWindow(&mw->patch); tw && tw->shown()) { tw->do_callback(); app.refreshStatus(); }
        else app.showText(mw, &mw->patch, mw->label() ? mw->label() : "patch", false);
    }, this);
    const int by = kMenuH + 5, bh = kToolH - 10;
    auto* zoomOut = new ToolButton(316, by, 28, bh, ToolButton::Minus, "", false);
    tip(zoomOut, "Zoom out", "view.zoomout");
    zoomOut->callback([](Fl_Widget*, void* d) { static_cast<DocumentWindow*>(d)->canvas->zoomBy(-1); }, this);
    zoomLabel_ = new ToolButton(346, by, 54, bh, ToolButton::None, "100%", false);
    tip(zoomLabel_, "The patch's zoom; click for 100%", "view.actual");
    zoomLabel_->callback([](Fl_Widget*, void* d) { static_cast<DocumentWindow*>(d)->canvas->zoomBy(0); }, this);
    auto* zoomIn = new ToolButton(402, by, 28, bh, ToolButton::Plus, "", false);
    tip(zoomIn, "Zoom in", "view.zoomin");
    zoomIn->callback([](Fl_Widget*, void* d) { static_cast<DocumentWindow*>(d)->canvas->zoomBy(1); }, this);
    auto* browser = new ToolButton(442, by, 98, bh, ToolButton::Browser, "Browser", false);
    tip(browser, "Tutorials, examples, objects and recent files", "help.browser");
    browser->callback([](Fl_Widget*, void*) { App::get().showBrowser(); });
    consoleBtn_ = new ToolButton(546, by, 100, bh, ToolButton::Console, "Console", false);
    consoleBtn_->copy_tooltip((withShortcut("Messages, errors and print", "patch.console") +
                               "; a red dot: an error since you last looked").c_str());
    consoleBtn_->callback([](Fl_Widget*, void*) { App::get().showConsole(); });
    recordBtn_ = new ToolButton(652, by, 96, bh, ToolButton::Record, "Record", false);
    recordBtn_->tooltip("Quick recorder: what goes to the dac, into a WAV file (Extras > Quick Recorder)");
    recordBtn_->callback([](Fl_Widget*, void*) { App::get().toggleRecording(); });
    auto* spacer = new Fl_Box(756, kMenuH, w - 756 - 200, kToolH);
    spacer->box(FL_NO_BOX);
    meters_ = new ToolMeters(w - 196, by, 188, bh);
    bar->resizable(spacer);
    bar->end();
    canvas = new Canvas(0, kMenuH + kToolH, w, h - kMenuH - kToolH - kStatusH, this, &patch);
    canvas->onZoom = [this] { updateZoomLabel(); };
    statusBar_ = new Fl_Box(0, h - kStatusH, w, kStatusH);
    statusBar_->box(FL_FLAT_BOX);
    statusBar_->color(kStatusBg);
    statusBar_->labelcolor(kDim);
    statusBar_->labelsize(11);
    statusBar_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    end();
    resizable(canvas);
    size_range(400, 300);

    doc.onChanged = [this] {
        updateTitle();
        App::get().patchChanged();
        App::get().refreshMenus();
    };
    patch.onRedraw = [this] {
        canvas->redraw();
        App::get().patchChanged();
    };
    patch.onStatus = [this](const std::string& s) { setStatus(s); };
    patch.onPrint = [this](const std::string& s) { print(s); };
    patch.onStructure = [this] {
        refreshStatus();
        canvas->redraw();                 // an inner edit can change a box's ports
        App::get().patchChanged();
    };

    callback([](Fl_Widget* w, void*) {
        // Escape must not close the window; everything else (close box) does.
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
        auto* d = static_cast<DocumentWindow*>(w);
        if (d->confirmDiscard()) App::get().closeDocument(d);
    });

    App::get().docs.push_back(this);
    updateTitle();
    canvas->setLocked(false);
    setStatus(std::string("double-click to create an object · ") + shortcutText(FL_COMMAND + 'e') +
              " toggles edit mode");
}

DocumentWindow::~DocumentWindow() {
    for (auto* w : subwindows_) w->detach();
    if (menu_) App::get().forgetMenu(menu_);
}

void DocumentWindow::updateZoomLabel() {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", (int)std::lround(canvas->zoom() * 100));
    zoomLabel_->copy_label(buf);
    zoomLabel_->redraw();
}

void DocumentWindow::refreshStatus() {
    App& app = App::get();
    std::string s = canvas->locked() ? "LOCKED" : "EDIT";
    if (app.audio.running())
        s += "   ·   audio " + std::to_string(app.audio.sampleRate()) + " Hz, " +
             std::to_string(app.audio.deviceOutputs()) + " out" +
             (app.audio.hasInput() ? ", " + std::to_string(app.audio.deviceInputs()) + " in" : "") +
             (app.audio.channels() > app.audio.deviceOutputs()
                  ? " (patches: " + std::to_string(app.audio.channels()) + " out)" : "");
    else
        s += "   ·   audio off";
    if (!patch.dspError.empty()) s += "   ·   " + patch.dspError;
    if (!message_.empty()) s += "   ·   " + message_;
    if (s != shownStatus_) {
        shownStatus_ = s;
        statusBar_->copy_label(("  " + s).c_str());
    }
    auto set = [](ToolButton* b, bool on, const char* label) {
        if (b->value() == (on ? 1 : 0) && b->label() && std::string(b->label()) == label) return;
        b->value(on);
        b->copy_label(label);
        b->redraw();
    };
    set(audioBtn_, app.audio.running(), app.audio.running() ? "Audio On" : "Audio Off");
    set(editBtn_, !canvas->locked(), canvas->locked() ? "Locked" : "Edit");
    auto* tw = app.textWindow(&patch);
    set(textBtn_, tw && tw->shown(), "Code");
    app.refreshMenus();
}

void DocumentWindow::print(const std::string& s) {
    App::get().log(s);
    setStatus(s);
}

void DocumentWindow::setStatus(const std::string& s) {
    message_ = s;
    refreshStatus();
}

void DocumentWindow::updateTitle() {
    const std::string& path = doc.path();
    std::string t = (path.empty() ? (doc.suggested.empty() ? std::string("Untitled") : doc.suggested + " (from Max)")
                                  : baseName(path)) +
                    (doc.anyDirty() ? " — edited" : "") + " — Moritz";
    if (!label() || t != label()) copy_label(t.c_str());
}

void DocumentWindow::showRecording(bool on, const char* label) {
    if (recordBtn_->lit == on && recordBtn_->label() && std::string(recordBtn_->label()) == label) return;
    recordBtn_->lit = on;
    recordBtn_->copy_label(label);
    recordBtn_->redraw();
}

void DocumentWindow::showConsoleAlert(bool on) {
    if (consoleBtn_->alert == on) return;
    consoleBtn_->alert = on;
    consoleBtn_->redraw();
}

void DocumentWindow::updateMeters(float cpu, const std::vector<float>& peaks) { meters_->update(cpu, peaks); }

int DocumentWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        activeCanvas()->cancelEdit();
        return 1;
    }
    int r = Fl_Double_Window::handle(e);
    if (e == FL_FOCUS || e == FL_SHOW) refreshStatus();   // macOS: the shared menu follows
    return r;
}

// ---- File

bool DocumentWindow::confirmDiscard() {
    canvas->commitEdit();
    for (auto* w : subwindows_) w->canvas->commitEdit();
    App& app = App::get();
    // the scripts of its code boxes, edited in their editors and not put back yet
    for (auto* cw : std::vector<CodeWindow*>(app.codeWindows())) {
        CodeSource* s = cw->source();
        if (!s || !cw->dirty() || app.docOf(s->patchOf()) != this) continue;
        cw->show();
        if (!cw->confirmDiscard()) return false;
    }
    // abstractions edited in their windows: each has its own file
    for (Patch* a : doc.dirtyAbstractions()) {
        int r = askUser("Save the changes to the abstraction \"%s\"?", "Cancel", "Save", "Don't Save",
                          baseName(a->file).c_str());
        if (r == 0) return false;
        if (r == 1 && !saveAbstraction(a)) return false;
    }
    if (!doc.dirty()) return true;
    std::string name = path().empty() ? "Untitled" : baseName(path());
    int r = askUser("Save changes to \"%s\"?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return save();
    return true;
}

bool DocumentWindow::openFile(const std::string& path) {
    std::string err;
    canvas->reset();
    closeAllSubwindows();
    if (!doc.load(path, err)) {
        if (err.rfind("can't read", 0) == 0) alertUser("Can't read %s", path.c_str());
        else alertUser("Can't open %s:\n%s", baseName(path).c_str(), err.c_str());
        return false;
    }
    canvas->setLocked(true);           // Max opens patches locked
    updateTitle();
    log("opened " + doc.path());
    App::get().addRecent(doc.path());
    if (!err.empty()) print(baseName(path) + ":\n" + err);
    else setStatus("opened " + baseName(path));
    if (!patch.dspError.empty()) log(baseName(path) + ": " + patch.dspError);
    patch.loadbangAll();
    canvas->redraw();
    return true;
}

bool DocumentWindow::loadConverted(const std::string& src, const std::string& dir, const std::string& name) {
    std::string err;
    canvas->reset();
    closeAllSubwindows();
    if (!doc.loadText(src, dir, err)) {    // abstractions converted next to the original
        alertUser("Can't load the converted patch:\n%s", err.c_str());
        return false;
    }
    doc.suggested = name + "." + kExt;
    canvas->setLocked(false);
    updateTitle();
    if (!err.empty()) log(name + ":\n" + err);
    patch.loadbangAll();
    canvas->redraw();
    return true;
}

bool DocumentWindow::save() {
    canvas->commitEdit();
    for (auto* w : subwindows_) w->canvas->commitEdit();
    if (path().empty()) return saveAs();
    std::string err;
    if (!doc.save(err)) {
        alertUser("Can't save %s:\n%s\n\nThe file on the disk is as it was.", baseName(path()).c_str(), err.c_str());
        print("can't save: " + err);
        return false;
    }
    log("saved " + path());
    setStatus("saved " + baseName(path()));
    return true;
}

bool DocumentWindow::saveAs() {
    canvas->commitEdit();
    Fl_Native_File_Chooser fc;
    fc.title("Save Patch As");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("Moritz patch\t*.moritz");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    if (path().empty() && !doc.suggested.empty() && !patch.dir.empty()) fc.directory(patch.dir.c_str());
    fc.preset_file(path().empty() ? (doc.suggested.empty() ? "Untitled.moritz" : doc.suggested.c_str())
                                  : baseName(path()).c_str());
    if (fc.show() != 0) return false;
    std::string p = portablePath(fc.filename());
    const char* ext = fl_filename_ext(p.c_str());
    if (!ext || !*ext) p += std::string(".") + kExt;
    std::string err;
    if (!doc.saveTo(p, err)) {          // the document keeps its file and folder
        alertUser("Can't save %s:\n%s", baseName(p).c_str(), err.c_str());
        print("can't save: " + err);
        return false;
    }
    log("saved " + path());
    setStatus("saved " + baseName(path()));
    App::get().addRecent(path());
    return true;
}

bool DocumentWindow::saveAbstraction(Patch* a) {
    for (auto* w : subwindows_) w->canvas->commitEdit();
    std::string err;
    if (!doc.saveAbstraction(a, err)) {
        alertUser("Can't save the abstraction %s:\n%s\n\nThe file on the disk is as it was.", baseName(a->file).c_str(),
                 err.c_str());
        print("can't save: " + err);
        return false;
    }
    log("saved abstraction " + a->file);
    setStatus("saved abstraction " + baseName(a->file));
    App::get().abstractionSaved(a);
    return true;
}

bool DocumentWindow::saveFront() {
    // inside an abstraction (or a p inside one) in front: Save writes that file
    if (PatchWindow* w = frontSubwindow(); w && w->patch)
        if (Patch* a = doc.abstractionOf(w->patch)) return saveAbstraction(a);
    return save();
}

void DocumentWindow::revert() {
    if (path().empty()) return;
    if (doc.anyDirty() &&
        askUser("Revert to the saved \"%s\"?\nThe changes since it was saved will be lost.", "Cancel", "Revert",
                  nullptr, baseName(path()).c_str()) != 1)
        return;
    std::string p = path();
    openFile(p);
}

// ---- Edit

void DocumentWindow::pushUndo(Patch* p) { doc.pushUndo(p ? p : &patch); }

void DocumentWindow::dropUndo() { doc.dropUndo(); }

void DocumentWindow::markDirty(Patch* p) { doc.markDirty(p ? p : &patch); }

void DocumentWindow::afterRestore(Patch* restored) {
    (void)restored;
    canvas->reset();
    for (auto* w : subwindows_) w->canvas->reset();
    canvas->redraw();
}

void DocumentWindow::undo() {
    canvas->commitEdit();
    activeCanvas()->commitEdit();
    auto open = openSubpatchPaths();
    std::string msg;
    Patch* restored = nullptr;
    doc.undo(msg, &restored);
    reopenSubpatches(open);             // a subpatcher remade by the undo: its window again
    afterRestore(restored);
    setStatus(msg);
}

void DocumentWindow::redo() {
    canvas->commitEdit();
    activeCanvas()->commitEdit();
    auto open = openSubpatchPaths();
    std::string msg;
    Patch* restored = nullptr;
    doc.redo(msg, &restored);
    reopenSubpatches(open);
    afterRestore(restored);
    setStatus(msg);
}

// ---- Patch

void DocumentWindow::toggleLock() {
    Canvas* c = activeCanvas();
    c->setLocked(!c->locked());
}

// ---- subpatcher windows

Canvas* DocumentWindow::activeCanvas() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (w == this) return canvas;
        if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->doc == this) return pw->canvas;
    }
    return canvas;
}

PatchWindow* DocumentWindow::frontSubwindow() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (w == this) return nullptr;
        if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->doc == this) return pw;
    }
    return nullptr;
}

void DocumentWindow::openSubpatch(Box* b) {
    Patch* p = b ? b->inner() : nullptr;
    if (!p) return;
    for (auto* w : subwindows_)
        if (w->patch == p) { w->show(); return; }
    int W, H;
    fitPatch(p, W, H);
    auto* w = new PatchWindow(this, p, patchTitle(p));
    w->size(W, H);
    int shown = 0;
    for (auto* o : subwindows_) shown += o->shown();
    centerOnScreen(w, this, 28 * shown, 28 * shown);   // cascade over the centre
    subwindows_.push_back(w);
    w->show();
}

void DocumentWindow::closeSubpatchWindows(Patch* p) {
    for (auto* w : std::vector<PatchWindow*>(subwindows_))
        if (w->patch == p) {
            w->detach();
            forgetWindow(w);
            w->hide();
            Fl::delete_widget(w);
        }
}

void DocumentWindow::closeAllSubwindows() {
    for (auto* w : subwindows_) {
        w->detach();
        w->hide();
        Fl::delete_widget(w);
    }
    subwindows_.clear();
}

void DocumentWindow::forgetWindow(PatchWindow* w) {
    subwindows_.erase(std::remove(subwindows_.begin(), subwindows_.end(), w), subwindows_.end());
}

std::vector<std::string> DocumentWindow::openSubpatchPaths() const {
    std::vector<std::string> v;
    for (auto* w : subwindows_)
        if (w->patch && w->shown()) v.push_back(doc.where(w->patch));
    return v;
}

void DocumentWindow::reopenSubpatches(const std::vector<std::string>& paths) {
    for (auto& path : paths) {
        Patch* p = doc.resolve(path);
        if (!p || !p->owner) continue;
        bool open = false;
        for (auto* w : subwindows_) open = open || w->patch == p;
        if (!open) openSubpatch(p->owner);
    }
}

}  // namespace moritz
