// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// document_window.cpp — a document's window: toolbar, canvas, status bar; files, undo, subpatchers

#include "gui.h"
#include "ui_util.h"
#include "settings.h"
#include "codewindow.h"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>

#include "fileio.h"

#include <cmath>

namespace moritz {

MainWindow::MainWindow(int w, int h)
    : Fl_Double_Window(w, h, "Moritz"), patch(*App::get().host.addExtra(std::make_unique<Patch>())) {
#ifndef __APPLE__
    menu_ = new Fl_Menu_Bar(0, 0, w, kMenuH);
    App::get().buildMenu(menu_);
#endif
    auto* bar = new Fl_Group(0, kMenuH, w, kToolH);
    bar->box(FL_FLAT_BOX);
    bar->color(kStatusBg);
    audioBtn_ = new ToolButton(8, kMenuH + 5, 104, kToolH - 10, ToolButton::Power, "Audio");
    audioBtn_->tooltip("Start or stop audio (Cmd+/)");
    audioBtn_->callback([](Fl_Widget*, void*) { App::get().toggleAudio(); });
    editBtn_ = new ToolButton(118, kMenuH + 5, 92, kToolH - 10, ToolButton::Pencil, "Edit");
    editBtn_->tooltip("Edit mode on/off (Cmd+E)");
    editBtn_->callback([](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->toggleLock(); }, this);
    textBtn_ = new ToolButton(216, kMenuH + 5, 86, kToolH - 10, ToolButton::Code, "Code");
    textBtn_->tooltip("The patch as code (its text), kept in sync with the drawing (Cmd+T)");
    textBtn_->callback([](Fl_Widget*, void* d) {
        auto* mw = static_cast<MainWindow*>(d);
        App& app = App::get();
        if (auto* tw = app.textWindow(&mw->patch); tw && tw->shown()) { tw->do_callback(); app.refreshStatus(); }
        else app.showText(mw, &mw->patch, mw->label() ? mw->label() : "patch", false);
    }, this);
    const int by = kMenuH + 5, bh = kToolH - 10;
    auto* zoomOut = new ToolButton(316, by, 28, bh, ToolButton::Minus, "", false);
    zoomOut->tooltip("Zoom out (Cmd+-)");
    zoomOut->callback([](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->canvas->zoomBy(-1); }, this);
    zoomLabel_ = new ToolButton(346, by, 54, bh, ToolButton::None, "100%", false);
    zoomLabel_->tooltip("The patch's zoom; click for 100% (Cmd+0)");
    zoomLabel_->callback([](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->canvas->zoomBy(0); }, this);
    auto* zoomIn = new ToolButton(402, by, 28, bh, ToolButton::Plus, "", false);
    zoomIn->tooltip("Zoom in (Cmd++)");
    zoomIn->callback([](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->canvas->zoomBy(1); }, this);
    auto* browser = new ToolButton(442, by, 98, bh, ToolButton::Browser, "Browser", false);
    browser->tooltip("Tutorials, examples, objects and recent files (Shift+Cmd+B)");
    browser->callback([](Fl_Widget*, void*) { App::get().showBrowser(); });
    consoleBtn_ = new ToolButton(546, by, 100, bh, ToolButton::Console, "Console", false);
    consoleBtn_->tooltip("Messages, errors and print (Shift+Cmd+M); a red dot: an error since you last looked");
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
        auto* d = static_cast<MainWindow*>(w);
        if (d->confirmDiscard()) App::get().closeDocument(d);
    });

    App::get().docs.push_back(this);
    updateTitle();
    canvas->setLocked(false);
    setStatus("double-click to create an object · Cmd+E toggles edit mode");
}

MainWindow::~MainWindow() {
    for (auto* w : subwindows_) w->detach();
}

void MainWindow::updateZoomLabel() {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", (int)std::lround(canvas->zoom() * 100));
    zoomLabel_->copy_label(buf);
    zoomLabel_->redraw();
}

void MainWindow::refreshStatus() {
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
    statusBar_->copy_label(("  " + s).c_str());
    Fl_Menu_Bar* m = app.menuFor(this);
    setMenuCheck(m, "Patch/Edit Mode", !canvas->locked());
    setMenuCheck(m, "Patch/Audio On", app.audio.running());
    audioBtn_->value(app.audio.running());
    audioBtn_->copy_label(app.audio.running() ? "Audio On" : "Audio Off");
    editBtn_->value(!canvas->locked());
    editBtn_->copy_label(canvas->locked() ? "Locked" : "Edit");
    auto* tw = app.textWindow(&patch);
    textBtn_->value(tw && tw->shown());
    audioBtn_->redraw();
    editBtn_->redraw();
    textBtn_->redraw();
}

void MainWindow::print(const std::string& s) {
    App::get().log(s);
    setStatus(s);
}

void MainWindow::setStatus(const std::string& s) {
    message_ = s;
    refreshStatus();
}

void MainWindow::updateTitle() {
    std::string t = (path_.empty() ? (suggested_.empty() ? std::string("Untitled") : suggested_ + " (from Max)")
                                   : baseName(path_)) +
                    (edited() ? " — edited" : "") + " — Moritz";
    copy_label(t.c_str());
}

// An edit inside an abstraction (or a subpatcher in one) is unsaved in the abstraction's file, which is
// saved on its own; any other edit is unsaved in the document.
void MainWindow::markDirty(Patch* p) {
    if (Patch* a = abstractionOf(p ? p : &patch)) absDirty_.insert(a);
    else dirty_ = true;
    updateTitle();
    App::get().patchChanged();
}

Patch* MainWindow::abstractionOf(Patch* p) const {
    for (Patch* q = p; q && q != &patch; q = q->parent)
        if (q->isAbstraction && !q->file.empty()) return q;
    return nullptr;
}

void MainWindow::forget(Patch* p) {
    if (absDirty_.erase(p)) updateTitle();
}

int MainWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        activeCanvas()->cancelEdit();
        return 1;
    }
    int r = Fl_Double_Window::handle(e);
    if (e == FL_FOCUS || e == FL_SHOW) refreshStatus();   // macOS: the shared menu follows
    return r;
}

// ---- File

bool MainWindow::confirmDiscard() {
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
    for (Patch* a : std::vector<Patch*>(absDirty_.begin(), absDirty_.end())) {
        int r = askUser("Save the changes to the abstraction \"%s\"?", "Cancel", "Save", "Don't Save",
                        baseName(a->file).c_str());
        if (r == 0) return false;
        if (r == 1 && !saveAbstraction(a)) return false;
    }
    if (!dirty_) return true;
    std::string name = path_.empty() ? "Untitled" : baseName(path_);
    int r = askUser("Save changes to \"%s\"?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return save();
    return true;
}

bool MainWindow::openFile(const std::string& path) {
    std::string src, err;
    if (!fileio::read(path, src)) {
        alertUser("Can't read %s", path.c_str());
        return false;
    }
    canvas->reset();
    closeAllSubwindows();
    std::string oldDir = patch.dir;
    patch.dir = dirOf(path);
    if (!patch.load(src, err)) {
        patch.dir = oldDir;
        alertUser("Can't open %s:\n%s", baseName(path).c_str(), err.c_str());
        return false;
    }
    path_ = path;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    absDirty_.clear();
    canvas->setLocked(true);           // Max opens patches locked
    updateTitle();
    log("opened " + path);
    App::get().addRecent(path);
    if (!err.empty()) print(baseName(path) + ":\n" + err);
    else setStatus("opened " + baseName(path));
    if (!patch.dspError.empty()) log(baseName(path) + ": " + patch.dspError);
    patch.loadbangAll();
    canvas->redraw();
    return true;
}

bool MainWindow::loadConverted(const std::string& src, const std::string& dir, const std::string& name) {
    std::string err;
    canvas->reset();
    closeAllSubwindows();
    patch.dir = dir;                   // abstractions converted next to the original
    if (!patch.load(src, err)) {
        alertUser("Can't load the converted patch:\n%s", err.c_str());
        return false;
    }
    path_.clear();
    suggested_ = name + "." + kExt;
    undo_.clear();
    redo_.clear();
    dirty_ = true;                     // not saved yet
    absDirty_.clear();
    canvas->setLocked(false);
    updateTitle();
    if (!err.empty()) log(name + ":\n" + err);
    patch.loadbangAll();
    canvas->redraw();
    return true;
}

bool MainWindow::saveFront() {
    // inside an abstraction (or a p inside one) in front: Save writes that file
    Canvas* active = activeCanvas();
    if (active != canvas)
        if (Patch* a = abstractionOf(active->patch())) return saveAbstraction(a);
    return save();
}

bool MainWindow::saveAbstraction(Patch* a) {
    for (auto* w : subwindows_) w->canvas->commitEdit();
    std::string err;
    if (!fileio::writeAtomic(a->file, a->serialize(), &err)) {   // the file stays as it was
        alertUser("Can't save the abstraction %s:\n%s\n\nThe file on the disk is as it was.", baseName(a->file).c_str(),
                  err.c_str());
        log("can't save: " + err);
        return false;
    }
    absDirty_.erase(a);
    updateTitle();
    log("saved abstraction " + a->file);
    setStatus("saved abstraction " + baseName(a->file));
    App::get().abstractionSaved(a);           // its other copies follow
    return true;
}

bool MainWindow::save() {
    canvas->commitEdit();
    for (auto* w : subwindows_) w->canvas->commitEdit();
    if (path_.empty()) return saveAs();
    // a temporary file in the same folder, renamed over the old one once written: a full disk or a
    // crash leaves the old file whole
    std::string err;
    if (!fileio::writeAtomic(path_, patch.serialize(), &err)) {
        alertUser("Can't save %s:\n%s\n\nThe file on the disk is as it was.", baseName(path_).c_str(), err.c_str());
        log("can't save: " + err);
        return false;
    }
    dirty_ = false;
    updateTitle();
    log("saved " + path_);
    setStatus("saved " + baseName(path_));
    return true;
}

bool MainWindow::saveAs() {
    canvas->commitEdit();
    Fl_Native_File_Chooser fc;
    fc.title("Save Patch As");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("Moritz patch\t*.moritz");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    if (path_.empty() && !suggested_.empty() && !patch.dir.empty()) fc.directory(patch.dir.c_str());
    fc.preset_file(path_.empty() ? (suggested_.empty() ? "Untitled.moritz" : suggested_.c_str())
                                 : baseName(path_).c_str());
    if (fc.show() != 0) return false;
    std::string p = portablePath(fc.filename());
    const char* ext = fl_filename_ext(p.c_str());
    if (!ext || !*ext) p += std::string(".") + kExt;
    std::string err;
    if (!fileio::writeAtomic(p, patch.serialize(), &err)) {   // the document keeps its file and folder
        alertUser("Can't save %s:\n%s", baseName(p).c_str(), err.c_str());
        log("can't save: " + err);
        return false;
    }
    path_ = p;
    suggested_.clear();
    patch.dir = dirOf(p);
    dirty_ = false;
    updateTitle();
    log("saved " + path_);
    setStatus("saved " + baseName(path_));
    App::get().addRecent(p);
    return true;
}

// ---- Edit

void MainWindow::pushUndo(Patch* p) {
    if (!p) p = &patch;
    undo_.push_back({pathOf(p), p->serialize()});
    if (undo_.size() > 500) undo_.erase(undo_.begin());
    redo_.clear();
}

void MainWindow::dropUndo() {
    if (!undo_.empty()) undo_.pop_back();
}

// Puts back the text of the last step of `from`, keeping what did not change (applyText): a buffer~'s
// sound, a code box's interpreter and editor, the sound of everything else go on.
bool MainWindow::restore(std::vector<UndoStep>& from, std::vector<UndoStep>& to, std::string& msg) {
    canvas->commitEdit();
    activeCanvas()->commitEdit();
    auto open = openSubpatchPaths();
    bool done = false;
    while (!from.empty() && !done) {
        UndoStep s = std::move(from.back());
        from.pop_back();
        Patch* p = resolve(s.where);
        if (!p) { msg = "skipped an edit of " + s.where + ", which is gone"; continue; }
        to.push_back({s.where, p->serialize()});
        std::string err;
        if (!p->applyText(s.text, err)) {     // can't happen with a text the patch wrote; nothing changed
            to.pop_back();
            msg = "can't restore: " + err;
            break;
        }
        if (p->owner) p->rebuild();           // a subpatcher's inlets and outlets follow positions
        markDirty(p);
        done = true;
    }
    reopenSubpatches(open);                   // a subpatcher remade by it: its window again
    canvas->reset();
    for (auto* w : subwindows_) w->canvas->reset();
    canvas->redraw();
    return done;
}

void MainWindow::undo() {
    std::string msg;
    bool ok = restore(undo_, redo_, msg);
    setStatus(ok ? (msg.empty() ? "undo" : msg) : (msg.empty() ? "nothing to undo" : msg));
}

void MainWindow::redo() {
    std::string msg;
    bool ok = restore(redo_, undo_, msg);
    setStatus(ok ? (msg.empty() ? "redo" : msg) : (msg.empty() ? "nothing to redo" : msg));
}

// ---- Patch

void MainWindow::toggleLock() {
    Canvas* c = activeCanvas();
    c->setLocked(!c->locked());
}

// ---- subpatcher windows

Canvas* MainWindow::activeCanvas() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (w == this) return canvas;
        if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->doc == this) return pw->canvas;
    }
    return canvas;
}

void MainWindow::openSubpatch(Box* b) {
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

void MainWindow::closeSubpatchWindows(Patch* p) {
    for (auto* w : std::vector<PatchWindow*>(subwindows_))
        if (w->patch == p) {
            w->detach();
            forgetWindow(w);
            w->hide();
            Fl::delete_widget(w);
        }
}

void MainWindow::closeAllSubwindows() {
    for (auto* w : subwindows_) {
        w->detach();
        w->hide();
        Fl::delete_widget(w);
    }
    subwindows_.clear();
}

void MainWindow::forgetWindow(PatchWindow* w) {
    subwindows_.erase(std::remove(subwindows_.begin(), subwindows_.end(), w), subwindows_.end());
}

std::vector<std::string> MainWindow::openSubpatchPaths() const {
    std::vector<std::string> v;
    for (auto* w : subwindows_)
        if (w->patch && w->shown()) v.push_back(pathOf(w->patch));
    return v;
}

Patch* MainWindow::resolve(const std::string& where) const {
    Patch* p = &patch;
    size_t start = 0;
    while (p && start < where.size()) {
        size_t end = where.find('/', start);
        std::string id = where.substr(start, end == std::string::npos ? std::string::npos : end - start);
        Box* b = p->find(id);
        p = b ? b->inner() : nullptr;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return p;
}

void MainWindow::reopenSubpatches(const std::vector<std::string>& paths) {
    for (auto& path : paths) {
        Patch* p = resolve(path);
        if (!p || !p->owner) continue;
        bool open = false;                    // still open (its box was kept): left where it is
        for (auto* w : subwindows_) open = open || (w->patch == p && w->shown());
        if (!open) openSubpatch(p->owner);
    }
}

}  // namespace moritz
