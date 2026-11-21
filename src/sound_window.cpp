// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_window.cpp — the sound editor in a window of its own, and how the application opens it: a buffer~
// double-clicked (or its Edit... item), a sound file (File > Open Sound..., or any sound opened as a document).
//
// A sound file is a document of its own, held by its window (App::soundWindows): not tied to any patch, its edits
// are unsaved until Save writes the file (a WAV file; another kind is saved as a WAV file next to it), and closing
// asks. A buffer~ is patch memory: its window edits it in place, Save writes a copy (Save as WAV...), and closing
// asks nothing, as nothing of it is on the disk.

#include "sound_editor.h"
#include "fileio.h"
#include "preview.h"
#include "ui_util.h"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/filename.H>
void fl_open_display();   // FL/platform.H (not included: it drags in X11's macros on Linux, None among them)

namespace moritz {

SoundWindow::SoundWindow(std::shared_ptr<SoundDoc> doc) : Fl_Double_Window(kW, kH) {
    color(kStatusBg);
#ifndef __APPLE__
    menu_ = new Fl_Menu_Bar(0, 0, w(), kMenuH);
    App::get().buildMenu(menu_);
#endif
    editor_ = new SoundEditor(0, kMenuH, w(), h() - kMenuH, std::move(doc));
    end();
    resizable(editor_);
    size_range(640, 380);
    editor_->onTitle = [this] { updateTitle(); };
    callback([](Fl_Widget* w, void*) {
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;   // Escape doesn't close it
        auto* sw = static_cast<SoundWindow*>(w);
        if (sw->confirmDiscard()) App::get().closeSound(sw);
    });
    updateTitle();
}

SoundWindow::~SoundWindow() {
    if (menu_) App::get().forgetMenu(menu_);
}

void SoundWindow::updateTitle() {
    SoundDoc& d = doc();
    std::string t;
    if (d.boundToBuffer()) t = "buffer~ " + d.bufferName() + (d.buffer() ? "" : " (not in any open patch now)");
    else t = (d.path().empty() ? std::string("Untitled sound") : baseName(d.path())) + (d.dirty() ? " — edited" : "");
    t += " — Moritz";
    if (!label() || t != label()) copy_label(t.c_str());
}

int SoundWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        editor_->stop();
        editor_->stopWork();                     // a process or an analysis running
        return 1;
    }
    int r = Fl_Double_Window::handle(e);
    if (e == FL_FOCUS || e == FL_SHOW) App::get().refreshMenus();
    return r;
}

bool SoundWindow::save() {
    SoundDoc& d = doc();
    if (d.boundToBuffer() || !d.savesInPlace()) return saveAs();
    std::string err;
    if (!d.save(err)) {
        alertUser("Can't save %s:\n%s\n\nThe file on the disk is as it was.", baseName(d.path()).c_str(), err.c_str());
        App::get().print("can't save: " + err);
        return false;
    }
    App::get().log("saved " + d.path());
    editor_->setStatus("saved " + baseName(d.path()));
    updateTitle();
    return true;
}

bool SoundWindow::saveAs() {
    SoundDoc& d = doc();
    Fl_Native_File_Chooser fc;
    fc.title(d.boundToBuffer() ? "Save buffer~ as WAV" : "Save Sound As");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("WAV\t*.wav");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    std::string name = d.boundToBuffer() ? d.bufferName() : d.path().empty() ? std::string("Untitled") : baseName(d.path());
    if (size_t dot = name.find_last_of('.'); dot != std::string::npos && !d.boundToBuffer()) name.resize(dot);
    if (!d.boundToBuffer() && !d.path().empty()) fc.directory(dirOf(d.path()).c_str());
    fc.preset_file((name + ".wav").c_str());
    std::string p = showSaveChooser(fc);
    if (p.empty()) return false;
    const char* ext = fl_filename_ext(p.c_str());
    if (!ext || !*ext) p += ".wav";
    std::string err;
    bool ok = d.boundToBuffer() ? d.exportTo(p, err) : d.saveTo(p, err);
    if (!ok) {
        alertUser("Can't save %s:\n%s", baseName(p).c_str(), err.c_str());
        App::get().print("can't save: " + err);
        return false;
    }
    App::get().log("saved " + p);
    editor_->setStatus("saved " + baseName(p));
    if (!d.boundToBuffer()) App::get().addRecent(d.path());
    updateTitle();
    return true;
}

bool SoundWindow::confirmDiscard() {
    if (!doc().needsSaving()) return true;
    std::string name = doc().path().empty() ? "Untitled sound" : baseName(doc().path());
    int r = askUser("Save changes to the sound \"%s\"?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return save();
    return true;
}

// ---------------------------------------------------------------- the application's side

namespace {
void place(SoundWindow* w, int index) {
    centerOnScreen(w, App::get().frontDoc(), 24 * (index % 6), 24 * (index % 6));
}
}  // namespace

SoundWindow* App::openSound(const std::string& given) {
    std::string path = fileio::absolute(given);
    for (auto* w : soundWindows_)
        if (!w->doc().boundToBuffer() && w->doc().path() == path) { w->show(); return w; }
    auto doc = std::make_shared<SoundDoc>();
    std::string err;
    if (!doc->load(path, err)) {
        alertUser("Can't open %s:\n%s", baseName(path).c_str(), err.c_str());
        return nullptr;
    }
    auto* w = new SoundWindow(doc);
    place(w, (int)soundWindows_.size());
    soundWindows_.push_back(w);
    w->show();
    addRecent(path);
    log("opened sound " + path);
    return w;
}

SoundWindow* App::newSound(SampleBuffer&& samples) {
    fl_open_display();
    auto doc = std::make_shared<SoundDoc>();
    doc->setSound(std::move(samples));
    doc->markUnsaved();                          // nothing of it is on the disk: closing asks
    auto* w = new SoundWindow(doc);
    place(w, (int)soundWindows_.size());
    soundWindows_.push_back(w);
    w->show();
    return w;
}

void App::openSoundDialog() {
    Fl_Native_File_Chooser fc;
    fc.title("Open Sound");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Sound\t*.{wav,wave,aif,aiff,flac,mp3}\nAll files\t*");
    if (fc.show() != 0) return;
    openSound(portablePath(fc.filename()));
}

SoundWindow* App::editBuffer(Patch* root, const std::string& name) {
    if (!root || !findBuffer(root, name)) {
        print("no buffer~ " + name);
        return nullptr;
    }
    for (auto* w : soundWindows_)
        if (w->doc().boundToBuffer() && w->doc().bufferName() == name) { w->show(); return w; }
    fl_open_display();
    auto doc = std::make_shared<SoundDoc>();
    doc->bindBuffer(root, name);
    auto* w = new SoundWindow(doc);
    place(w, (int)soundWindows_.size());
    soundWindows_.push_back(w);
    w->show();
    return w;
}

void App::closeSound(SoundWindow* w) {
    soundWindows_.erase(std::remove(soundWindows_.begin(), soundWindows_.end(), w), soundWindows_.end());
    w->editor()->stop();
    w->hide();
    Fl::delete_widget(w);
    refreshMenus();
}

void App::closeAllSounds() {
    auto all = std::move(soundWindows_);
    soundWindows_.clear();
    for (auto* w : all) {
        w->editor()->stop();
        w->hide();
        delete w;                        // its background work is stopped and waited for
    }
}

SoundWindow* App::frontSound() { return dynamic_cast<SoundWindow*>(Fl::first_window()); }

// the buffer~ a box names, in the sound editor (double-click, or its menu)
void openBufferEditor(Patch* p, const std::string& name) {
    if (!p) return;
    App::get().editBuffer(p->root(), name);
}

}  // namespace moritz
