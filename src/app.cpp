// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// app.cpp — the application: documents, help, code and text windows, the audio switch

#include "gui.h"
#include "ui_util.h"
#include "settings.h"
#include "codewindow.h"
#include "fileio.h"
#include "maximport.h"
#include "objects.h"
#include "sysopen.h"

#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>
#ifdef __APPLE__
#include <FL/platform.H>
#endif

#include <chrono>
#include <cmath>

#ifndef MORITZ_VERSION
#define MORITZ_VERSION "dev"
#endif

namespace moritz {

namespace {
void midiTick(void*) {          // look for MIDI devices plugged in or out
    App::get().midi.refresh();
    Fl::repeat_timeout(1.0, midiTick);
}

void schedulerTimer(void*) {
    Patch& h = App::get().host;
    h.runDue(h.time());           // re-arms through onSchedule
}

bool gInitialized = false;      // App::init done: there are windows to ask in

bool endsWith(const std::string& s, const char* tail) {
    size_t n = std::char_traits<char>::length(tail);
    return s.size() > n && s.compare(s.size() - n, n, tail) == 0;
}
}  // namespace

// ---- the read-only editor of help patches (editor.h)

void ReadOnlyEditor::print(const std::string& s) { App::get().print(s); }
void ReadOnlyEditor::setStatus(const std::string& s) {
    if (!s.empty()) App::get().log(s);
}
void ReadOnlyEditor::openSubpatch(Box* b) { App::get().openHelpSubpatch(b); }

App& App::get() {
    static App* app = new App;      // never destroyed: windows may outlive main's scope
    return *app;
}

void App::init() {
    Fl::keyboard_screen_scaling(0);      // Cmd +/- zoom the patch (View menu), not the whole interface
    openCodeSource = [this](CodeSource* s) { openCodeFor(s); };
    codeSourceGone = [this](CodeSource* s) { codeSourceClosed(s); };
    // a code box marked @trusted: the user decides, once per folder of patches (kept in the preferences,
    // one folder per line); an unsaved patch is trusted for this session only
    askTrust = [](const std::string& folder, const std::string& box) {
        static bool unsavedTrusted = false;
        Fl_Preferences t(appPrefs(), "trust");
        char buf[8192];
        t.get("folders", buf, "", sizeof buf);
        std::string list = buf;
        if (folder.empty() ? unsavedTrusted : ("\n" + list + "\n").find("\n" + folder + "\n") != std::string::npos)
            return true;
        int r = askUser("%s asks to do everything a program can: write files anywhere, run programs, use the "
                        "network.\n\nAllow the patches in\n%s?",
                        "Don't Allow", "Allow", nullptr, box.c_str(),
                        folder.empty() ? "this unsaved patch (until Moritz quits)" : folder.c_str());
        if (r != 1) return false;
        if (folder.empty()) unsavedTrusted = true;
        else {
            t.set("folders", (list.empty() ? folder : list + "\n" + folder).c_str());
            appPrefs().flush();
        }
        return true;
    };
    Fl::add_timeout(1.0 / 15, toolTimer);
    Patch::onDestroyed = [this](Patch* p) {
        for (auto* d : docs) {
            d->closeSubpatchWindows(p);
            d->doc.forget(p);
        }
        closeText(p);
        for (auto* w : std::vector<PatchWindow*>(helpWindows_))
            if (w->patch == p) {
                w->detach();
                helpWindows_.erase(std::remove(helpWindows_.begin(), helpWindows_.end(), w),
                                   helpWindows_.end());
                w->hide();
                Fl::delete_widget(w);
            }
    };
    host.onDsp = [this](bool on) { setAudio(on); };
    host.onPrint = [this](const std::string& s) { print(s); };
    host.onStatus = [this](const std::string& s) {
        if (auto* d = frontDoc()) d->setStatus(s);
    };
    static const auto t0 = std::chrono::steady_clock::now();
    host.clock = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    host.onSchedule = [this] { armTimer(); };
    host.onStructure = [this] {
        int before = audio.channels();
        audio.updateInputNeed();
        if (audio.channels() != before) {            // the patches' channels changed
            if (monitorSet_.on && monitorSet_.speakers == 0) applyMonitor();
            refreshTester();
        }
    };
    // MIDI: RtMidi's thread wakes the GUI thread, which hands the messages to
    // the patches; noteout & co. send through the engine. Devices are checked
    // once a second, so plugging one in just works.
    mainWake = [] { Fl::awake([](void*) { drainMain(); }, nullptr); };   // background jobs report back
    midi.wake = [] { Fl::awake([](void*) { App::get().midi.drain(); }, nullptr); };
    midi.onMessage = [this](const std::vector<unsigned char>& m) { host.midiIn(m); };
    host.onMidiOut = [this](const std::vector<unsigned char>& m) { midi.send(m); };
    midi.refresh();
    Fl::add_timeout(1.0, midiTick);
    Fl::add_handler(eggHandler);
    followFrontWindow();
#ifdef __APPLE__
    // one menu bar for every window, at the top of the screen
    Fl_Group* cur = Fl_Group::current();
    Fl_Group::current(nullptr);
    sysMenu_ = new Fl_Sys_Menu_Bar(0, 0, 0, 0);
    Fl_Group::current(cur);
    buildMenu(sysMenu_);
    static_cast<Fl_Sys_Menu_Bar*>(sysMenu_)->create_window_menu();
    fl_mac_set_about([](Fl_Widget*, void*) { App::get().about(); }, nullptr);
    static const Fl_Menu_Item appItems[] = {
        {"Settings…", FL_COMMAND + ',', [](Fl_Widget*, void*) { showSettings(); }, nullptr, 0, 0, 0, 0, 0},
        {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    Fl_Mac_App_Menu::custom_application_menu_items(appItems);   // Moritz > Settings…, as macOS has it
    sysMenu_->update();
#endif
    log("Moritz " MORITZ_VERSION " - created by Carmine-Emanuele Cella (2026)");
    log("this console shows print objects, errors, audio and file events");
    gInitialized = true;
}

// ---- which window the commands act on

DocumentWindow* App::frontDoc() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (auto mw = dynamic_cast<DocumentWindow*>(w)) return mw;
        if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->doc) return pw->doc;
    }
    return docs.empty() ? nullptr : docs.back();
}

Fl_Window* App::frontPatchWindow() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w))
        if (dynamic_cast<DocumentWindow*>(w) || dynamic_cast<PatchWindow*>(w)) return w;
    return nullptr;
}

bool App::helpInFront() {
    auto* pw = dynamic_cast<PatchWindow*>(frontPatchWindow());
    return pw && pw->help;
}

DocumentWindow* App::targetDoc() {
    Fl_Window* w = frontPatchWindow();
    if (auto mw = dynamic_cast<DocumentWindow*>(w)) return mw;
    if (auto pw = dynamic_cast<PatchWindow*>(w)) return pw->help ? nullptr : pw->doc;
    return docs.empty() ? nullptr : docs.back();
}

Canvas* App::activeCanvas() {
    Fl_Window* w = frontPatchWindow();
    if (auto mw = dynamic_cast<DocumentWindow*>(w)) return mw->canvas;
    if (auto pw = dynamic_cast<PatchWindow*>(w)) return pw->canvas;
    return docs.empty() ? nullptr : docs.back()->canvas;
}

DocumentWindow* App::docOf(Patch* p) {
    for (auto* d : docs)
        for (Patch* q = p; q; q = q->parent)
            if (q == &d->patch) return d;
    return nullptr;
}

// ---- documents

DocumentWindow* App::newDocument() {
    DocumentWindow* front = frontDoc();
    auto* w = new DocumentWindow(960, 640);
    if (front) {                                     // cascade from the front window
        int X, Y, W, H;
        Fl::screen_work_area(X, Y, W, H, Fl::screen_num(front->x(), front->y()));
        int nx = front->x() + 30, ny = front->y() + 30;
        if (nx + w->w() > X + W || ny + w->h() > Y + H) { nx = X + 20; ny = Y + 20; }
        w->position(nx, ny);
    } else {
        centerOnScreen(w);
    }
    w->show();
    if (docs.size() == 1 && !audio.running() && startAudio) setAudio(true);
    return w;
}

DocumentWindow* App::openDocument(const std::string& given) {
    std::string path = fileio::absolute(given);
    for (auto* d : docs)                              // already open: bring it forward
        if (!d->path().empty() && d->path() == path) { d->show(); return d; }
    if (endsWith(path, ".mu")) {                      // a Musil script: the editor
        openCode(path);
        return nullptr;
    }
    DocumentWindow* d = frontDoc();
    bool fresh = !(d && d->isEmptyUntitled());
    if (fresh) d = newDocument();
    if (!d->openFile(path)) {
        if (fresh) closeDocument(d);
        return nullptr;
    }
    d->show();
    return d;
}

void App::openLater(const std::string& path) {
    if (ready) { openDocument(path); return; }
    openLater_.push_back(path);         // App::init and the command line first (main.cpp)
}

void App::setReady() {
    ready = true;
    auto later = std::move(openLater_);
    openLater_.clear();
    for (auto& p : later) openDocument(p);
}

void App::openDialog() {
    Fl_Native_File_Chooser fc;
    fc.title("Open");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Moritz patch or Musil script\t*.{moritz,mu,mlsp}\nAll files\t*");
    DocumentWindow* front = frontDoc();
    std::string dir = front && !front->path().empty() ? dirOf(front->path()) : examplesDir();
    if (!dir.empty()) fc.directory(dir.c_str());
    if (fc.show() != 0) return;
    openDocument(portablePath(fc.filename()));
}

void App::importDialog() {
    Fl_Native_File_Chooser fc;
    fc.title("Import from Max");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Max patch\t*.{maxpat,maxhelp}\nAll files\t*");
    if (fc.show() != 0) return;
    importMax(portablePath(fc.filename()));
}

DocumentWindow* App::importMax(const std::string& path) {
    std::string text, err;
    MaxImportReport rep;
    if (!importMaxPatch(path, true, text, rep, err)) {
        alertUser("Can't import %s:\n%s", baseName(path).c_str(), err.c_str());
        return nullptr;
    }
    std::string name = baseName(path);
    name = name.substr(0, name.find_last_of('.'));
    DocumentWindow* d = frontDoc();
    bool fresh = !(d && d->isEmptyUntitled());
    if (fresh) d = newDocument();
    if (!d->loadConverted(text, dirOf(path), name)) {
        if (fresh) closeDocument(d);
        return nullptr;
    }
    log("imported " + path + ": " + rep.summary());
    if (!rep.unknown.empty() || !rep.problems.empty()) showConsole();
    d->setStatus("imported from Max: " + std::to_string(rep.boxes) + " boxes" +
                 (rep.unknown.empty() ? "" : ", some not converted (see the console)"));
    d->show();
    return d;
}

void App::closeDocument(DocumentWindow* w) {
    docs.erase(std::remove(docs.begin(), docs.end(), w), docs.end());
    w->closeAllSubwindows();
    w->hide();
    w->canvas->reset();
    Patch* p = &w->patch;
    p->onRedraw = nullptr;
    p->onStatus = nullptr;
    p->onPrint = nullptr;
    p->onStructure = nullptr;
    w->doc.onChanged = nullptr;
    Fl::delete_widget(w);              // later: the window must not touch the patch then
    host.removeExtra(p);               // stops its sound, timers and jobs
#ifndef __APPLE__
    if (docs.empty()) quit();          // no menu bar is left to do anything else
#endif
    refreshStatus();
    refreshMenus();
}

void App::closeFront() {
    Fl_Window* w = Fl::first_window();
    if (auto pw = dynamic_cast<PatchWindow*>(w)) {
        pw->do_callback();             // hides it (and closes a help patch)
        return;
    }
    if (w && w == console_) { w->hide(); return; }
    if (auto tw = dynamic_cast<PatchTextWindow*>(w)) { tw->do_callback(); return; }
    if (auto cw = dynamic_cast<CodeWindow*>(w)) { cw->do_callback(); return; }
    if (auto d = dynamic_cast<DocumentWindow*>(w)) {
        if (d->confirmDiscard()) closeDocument(d);
    }
    else if (w && w->visible()) w->hide();        // the browser, an about box...
}

void App::quit() {
    for (auto* d : std::vector<DocumentWindow*>(docs)) {
        d->show();
        if (!d->confirmDiscard()) return;
    }
    for (auto* c : std::vector<CodeWindow*>(codeWindows_)) {
        c->show();
        if (!c->confirmDiscard()) return;
    }
    for (auto* c : codeWindows_) c->hide();
    quitting = true;
    host.onDsp = nullptr;      // a dspstate~ reacting to the stop can't restart audio
    audio.stop();
    for (auto* d : std::vector<DocumentWindow*>(docs)) d->closeAllSubwindows();
    for (auto* w : helpWindows_) w->hide();
    for (auto* w : textWindows_) w->hide();
    hideBrowser();
    if (console_) console_->hide();
    for (auto* d : docs) d->hide();
}

void App::refreshStatus() {
    for (auto* d : docs) d->refreshStatus();
    refreshMenus();
}

void App::armTimer() {
    Fl::remove_timeout(schedulerTimer);
    double next = host.nextDue();
    if (std::isfinite(next))
        Fl::add_timeout(std::max(0.0, next - host.time()), schedulerTimer);
}

// ---- edits that come from elsewhere than a canvas (a code editor, the code view)

void App::markEdited(Patch* p) {
    if (auto* d = docOf(p)) d->markDirty(p);
}

void App::pushUndoFor(Patch* p) {
    if (auto* d = docOf(p)) d->pushUndo(p);
}

void App::abstractionSaved(Patch* a) {
    std::string text = a->serialize();
    for (auto* d : docs) {
        std::vector<Patch*> copies;
        d->patch.forEachBox([&](Box* b) {
            for (Patch* q : b->inners())
                if (q != a && q->isAbstraction && q->file == a->file) copies.push_back(q);
        });
        for (Patch* q : copies) {
            if (d->doc.abstractionDirty(q)) {           // edited there too: it keeps its edits
                print("abstraction " + baseName(a->file) + " was saved; a copy in " +
                      (d->path().empty() ? std::string("Untitled") : baseName(d->path())) +
                      " has edits of its own and was not updated");
                continue;
            }
            std::string err;
            if (q->applyText(text, err)) {              // the boxes that did not change go on
                if (q->owner) q->rebuild();
            } else {
                print("abstraction " + baseName(a->file) + ": a copy could not follow: " + err);
            }
        }
    }
}

// ---- the code editors

CodeWindow* App::newCode() {
    auto* w = new CodeWindow();
    codeWindows_.push_back(w);
    centerOnScreen(w, frontDoc());
    w->show();
    w->editor()->take_focus();
    return w;
}

CodeWindow* App::openCode(const std::string& given) {
    std::string path = fileio::absolute(given);
    for (auto* w : codeWindows_)
        if (w->path() == path) { w->show(); return w; }
    auto* w = new CodeWindow();
    if (!w->load(path)) {
        delete w;
        print("can't open " + path);
        return nullptr;
    }
    codeWindows_.push_back(w);
    centerOnScreen(w, frontDoc());
    w->show();
    w->editor()->take_focus();
    addRecent(path);
    return w;
}

CodeWindow* App::openCodeFor(CodeSource* s) {
    for (auto* w : codeWindows_)
        if (w->source() == s) { w->show(); return w; }
    auto* w = new CodeWindow();
    std::string f = s->file();
    if (!f.empty()) {
        // a file that is not there yet is not made now: the editor starts empty and Save writes it
        if (fileio::isFile(f) ? !w->load(f) : !w->startFile(f)) {
            delete w;
            print("code: can't open " + f);
            return nullptr;
        }
    } else {
        w->setText(s->script());
    }
    w->bind(s);
    codeWindows_.push_back(w);
    centerOnScreen(w, frontDoc());
    w->show();
    w->editor()->take_focus();
    return w;
}

void App::codeSourceClosed(CodeSource* s) {
    // This runs inside the box's destructor, maybe while its patch is being rebuilt (undo, the code
    // view) and under its lock: only note it, and settle from the event loop.
    for (auto* w : codeWindows_) {
        if (w->source() != s) continue;
        Orphan o{w, nullptr, "", ""};
        if (auto* b = dynamic_cast<Box*>(s)) {    // where it was: a box may take its place there
            o.id = b->id;
            o.doc = docOf(b->patch);              // nullptr when its document is closing
            if (o.doc) o.where = o.doc->doc.where(b->patch);
        }
        w->unbind();
        orphans_.push_back(o);
        if (!Fl::has_timeout(settleOrphans)) Fl::add_timeout(0, settleOrphans);
        return;
    }
}

void App::settleOrphans(void*) {
    App& app = App::get();
    auto orphans = std::move(app.orphans_);
    app.orphans_.clear();
    for (auto& o : orphans) {
        if (std::find(app.codeWindows_.begin(), app.codeWindows_.end(), o.window) == app.codeWindows_.end()) continue;
        CodeSource* next = nullptr;
        if (o.doc && std::find(app.docs.begin(), app.docs.end(), o.doc) != app.docs.end())
            if (Patch* p = o.doc->doc.resolve(o.where))
                if (Box* b = p->find(o.id)) next = dynamic_cast<CodeSource*>(b);
        if (next) {
            // the box that took its place (undo, the code view): the editor follows it
            bool open = false;
            for (auto* w : app.codeWindows_) open = open || w->source() == next;
            if (!open) {
                o.window->rebind(next);
                continue;
            }
        }
        if (!o.window->dirty()) app.closeCode(o.window);   // nothing unsaved: as before, it closes
        else o.window->keepUnbound();                      // its text stays, as a buffer of its own
    }
}

void App::closeCode(CodeWindow* w) {
    codeWindows_.erase(std::remove(codeWindows_.begin(), codeWindows_.end(), w), codeWindows_.end());
    for (auto& o : orphans_)
        if (o.window == w) o.window = nullptr;
    w->hide();
    Fl::delete_widget(w);
}

CodeWindow* App::frontCode() {
    return dynamic_cast<CodeWindow*>(Fl::first_window());
}

// ---- audio

void App::setAudio(bool on) {
    std::string err;
    if (on) {
        if (!audio.start(err)) { print("audio: " + err); refreshStatus(); return; }
        log("audio on: " + audio.deviceName() + ", " + std::to_string(audio.sampleRate()) +
            " Hz, " + std::to_string(audio.bufferFrames()) + "-sample buffer, " +
            std::to_string(audio.deviceOutputs()) + " outputs" +
            (audio.hasInput() ? ", " + std::to_string(audio.deviceInputs()) + " inputs" : ""));
        applyMonitor();                               // the head responses at this rate, these channels
        refreshTester();
        if (!err.empty()) log("audio: " + err);
        for (auto* d : docs) d->setStatus(err);     // the status bar already says "audio on"
    } else {
        if (audio.running()) log("audio off");
        audio.stop();
        audio.testChannel = -1;
        testerSteps_ = 0;
        refreshTester();
        for (auto* d : docs) d->setStatus("");
    }
    refreshMenus();
}

// ---- a patch asks to open something with the system (link)

void askToOpenWithSystem(Patch* from, const std::string& target, const std::string& who) {
    if (!gInitialized) {                             // no window to ask in (--render): never
        if (from) from->print(who + ": not opened (a message asked, and there is no window to ask in): " + target);
        return;
    }
    struct Ask {
        std::string target, who;
    };
    // later, from the event loop: the message may come in the middle of loading a patch
    Fl::add_timeout(0, [](void* d) {
        std::unique_ptr<Ask> a(static_cast<Ask*>(d));
        int r = askUser("A patch asks to open:\n\n%s\n\n(%s, from a message, not a click)", "Don't Open", "Open",
                        nullptr, a->target.c_str(), a->who.c_str());
        if (r != 1) { App::get().log(a->who + ": not opened: " + a->target); return; }
        if (!openWithSystem(a->target)) App::get().print(a->who + ": can't open " + a->target);
    }, new Ask{target, who});
}

// ---- code views

PatchTextWindow* App::textWindow(Patch* p) {
    for (auto* w : textWindows_)
        if (w->patch == p) return w;
    return nullptr;
}

void App::showText() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (auto tw = dynamic_cast<PatchTextWindow*>(w)) { tw->do_callback(); return; }   // toggles
        if (auto mw = dynamic_cast<DocumentWindow*>(w)) {
            showText(mw, &mw->patch, mw->label() ? mw->label() : "patch", false);
            return;
        }
        if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->patch) {
            showText(pw->doc, pw->patch, pw->label() ? pw->label() : "patch", pw->help);
            return;
        }
    }
}

void App::showText(DocumentWindow* doc, Patch* p, const std::string& title, bool readOnly) {
    PatchTextWindow* tw = textWindow(p);
    if (!tw) {
        std::string t = title;
        if (size_t k = t.find(" — Moritz"); k != std::string::npos) t.resize(k);
        tw = new PatchTextWindow(doc, p, t, readOnly);
        textWindows_.push_back(tw);
        Fl_Window* beside = nullptr;                     // beside the patch's window
        for (Fl_Window* w = Fl::first_window(); w && !beside; w = Fl::next_window(w))
            if (auto mw = dynamic_cast<DocumentWindow*>(w); mw && &mw->patch == p) beside = w;
            else if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->patch == p) beside = w;
        if (beside) {
            int X, Y, W, H;
            Fl::screen_work_area(X, Y, W, H, Fl::screen_num(beside->x(), beside->y()));
            int x = beside->x() + beside->w() + 8;
            if (x + tw->w() > X + W) x = std::max(X, X + W - tw->w());
            tw->resize(x, beside->y(), tw->w(), std::min(beside->h(), H));
        }
    }
    tw->refresh(true);
    tw->show();
    refreshStatus();
}

void App::closeText(Patch* p) {
    for (auto* w : std::vector<PatchTextWindow*>(textWindows_))
        if (w->patch == p) {
            w->detach();
            textWindows_.erase(std::remove(textWindows_.begin(), textWindows_.end(), w), textWindows_.end());
            w->hide();
            Fl::delete_widget(w);
        }
}

// ---- help patches

void App::openHelp(const std::string& cls) {
    std::string path = findHelp(cls);
    if (path.empty()) {
        std::string d = describeClass(cls);
        print(cls + ": " + (d.empty() ? "no help patch" : d + " (no help patch yet)"));
        return;
    }
    for (auto* w : helpWindows_)                        // already open: bring it forward
        if (w->helpRoot && w->patch && w->patch->file == path) { w->show(); return; }
    std::string src, err;
    if (!fileio::read(path, src)) { print("can't read " + path); return; }
    auto hp = std::make_unique<Patch>();
    hp->dir = dirOf(path);
    Patch* p = host.addExtra(std::move(hp));
    p->file = path;                       // identifies the window; not an abstraction
    if (!p->load(src, err)) {
        print(baseName(path) + ": " + err);
        host.removeExtra(p);
        return;
    }
    if (!err.empty()) print(baseName(path) + ": " + err);
    p->loadbangAll();
    int W, H;
    fitPatch(p, W, H);
    std::string name = baseName(path);
    name = name.substr(0, name.find(".help."));
    auto* w = new PatchWindow(nullptr, p, name + " help", true);
    w->helpClass = cls;
    w->size(W, H);
    centerOnScreen(w, frontDoc(), 40, 40);
    helpWindows_.push_back(w);
    w->show();
    log("help: " + path);
}

void App::openHelpSubpatch(Box* b) {
    Patch* p = b ? b->inner() : nullptr;
    if (!p) return;
    for (auto* w : helpWindows_)
        if (w->patch == p) { w->show(); return; }
    int W, H;
    fitPatch(p, W, H);
    auto* w = new PatchWindow(nullptr, p, patchTitle(p), true, false);
    w->size(W, H);
    centerOnScreen(w, frontDoc(), 60, 60);
    helpWindows_.push_back(w);
    w->show();
}

void App::closeHelp(Patch* p) {
    host.removeExtra(p);   // its destruction closes and deletes the window
}

// the settings changed (settings.cpp): every window shows them now
void App::settingsChanged() {
    for (auto* w : codeWindows_) w->applyFont();
    for (auto* w : textWindows_) w->applyFont();
    if (consoleView_) {
        consoleView_->textfont(kCodeFontSlot);
        consoleView_->textsize(settings().codeSize - 1);
    }
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) w->redraw();
}

}  // namespace moritz
