// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// mainwindow.cpp — the main window (menus, toolbar, files, undo, audio
// setup, console) and the windows of subpatchers.

#include "gui.h"
#include "settings.h"
#include "codewindow.h"
#include "reference.h"
#include "session.h"
#include "maximport.h"
#include "fileio.h"
#include "objects.h"

#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/filename.H>

#include <chrono>
#include <functional>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <fstream>

#ifdef __APPLE__
#include <FL/platform.H>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#elif defined(_WIN32)
#include <FL/fl_utf8.h>
#include <windows.h>
#endif

#ifndef MORITZ_VERSION
#define MORITZ_VERSION "dev"
#endif

namespace moritz {

namespace {
#ifdef __APPLE__
constexpr int kMenuH = 0;        // the macOS menu bar lives at the top of the screen
#else
constexpr int kMenuH = 25;
#endif
constexpr int kStatusH = 22;
constexpr int kToolH = 34;
constexpr const char* kExt = "moritz";

std::string escapeMenu(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '_') r += '\\';
        if (c == '&') r += '&';
        r += c;
    }
    return r;
}

std::string baseName(const std::string& path) {
    const char* n = fl_filename_name(path.c_str());
    return n ? n : path;
}

std::string exeDir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[4096];
        if (realpath(buf, real)) {
            std::string s = real;
            return dirOf(s);
        }
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        std::string s(buf, (size_t)n);
        return dirOf(s);
    }
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    if (n > 0) {
        char utf8[MAX_PATH * 8];
        fl_utf8fromwc(utf8, sizeof utf8, buf, n);
        return dirOf(portablePath(utf8));
    }
#endif
    return ".";
}

}  // namespace

std::string resourceDir(const std::string& name) {
    std::string d = exeDir();
    for (std::string c : {d + "/../Resources/" + name, d + "/" + name})
        if (fl_filename_isdir(c.c_str())) return c;
    return "";
}

void setupSearchPaths() {
    libraryPath().clear();
    abstractionPath().clear();
    helpPath().clear();
    if (std::string home = homeDir(); !home.empty()) {
        std::string user = home + "/Documents/Moritz/library";
        if (fl_filename_isdir(user.c_str())) libraryPath().push_back(user);
    }
    if (std::string d = resourceDir("library"); !d.empty()) libraryPath().push_back(d);
    soundPath().clear();
    if (std::string d = resourceDir("examples"); !d.empty()) {
        // the examples are in category folders: each is searched for abstractions
        abstractionPath().push_back(d);
        dirent** list = nullptr;
        int n = fl_filename_list(d.c_str(), &list, fl_casealphasort);
        for (int k = 0; k < n; ++k) {
            std::string f = list[k]->d_name;
            if (!f.empty() && f.back() == '/') f.pop_back();
            if (f.empty() || f[0] == '.') continue;
            if (std::string sub = d + "/" + f; fl_filename_isdir(sub.c_str())) abstractionPath().push_back(sub);
        }
        fl_filename_free_list(&list, n);
        soundPath().push_back(d);
    }
    // the sounds and data shared by Moritz's patches and Musil's examples, then Musil's own data (the hall)
    if (std::string d = resourceDir("data"); !d.empty()) soundPath().push_back(d);
    if (std::string d = resourceDir("musil"); !d.empty()) soundPath().push_back(d + "/src");
    if (std::string d = resourceDir("docs"); !d.empty()) {          // "docs/...": the bundled documents
        std::string root = d.substr(0, d.find_last_of('/'));
        soundPath().push_back(root);
    }
    if (std::string d = resourceDir("help"); !d.empty()) helpPath().push_back(d);
}

std::string chooseFile(bool save, const std::string& title, const std::string& filter, const std::string& preset) {
    Fl_Native_File_Chooser fc;
    fc.title(title.c_str());
    fc.type(save ? Fl_Native_File_Chooser::BROWSE_SAVE_FILE : Fl_Native_File_Chooser::BROWSE_FILE);
    if (!filter.empty()) fc.filter(filter.c_str());
    if (!preset.empty()) fc.preset_file(preset.c_str());
    if (save) fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    if (fc.show() != 0 || !fc.filename()) return "";
    return portablePath(fc.filename());
}

bool openWithSystem(const std::string& target) {
    std::string uri = target;
    if (target.find("://") == std::string::npos && target.rfind("mailto:", 0) != 0) {
        if (std::FILE* f = std::fopen(target.c_str(), "rb")) std::fclose(f);
        else if (!fl_filename_isdir(target.c_str())) return false;
        // a file URI: forward slashes, and the characters a URI can't hold escaped
        std::string p = target;
        for (auto& c : p) if (c == '\\') c = '/';
        std::string enc;
        for (unsigned char c : p) {
            if (std::isalnum(c) || std::strchr("/-_.~:", c)) enc += (char)c;
            else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); enc += b; }
        }
        uri = std::string("file://") + (enc.empty() || enc[0] != '/' ? "/" : "") + enc;
    }
    char err[512] = "";
    return fl_open_uri(uri.c_str(), err, sizeof err) != 0;
}

namespace {

// Centres a window on the usable area of the screen showing `ref` (or the
// main screen), shifted by dx, dy.
void centerOnScreen(Fl_Window* w, Fl_Window* ref = nullptr, int dx = 0, int dy = 0) {
    int X, Y, W, H;
    int n = ref ? Fl::screen_num(ref->x() + ref->w() / 2, ref->y() + ref->h() / 2) : 0;
    Fl::screen_work_area(X, Y, W, H, n);
    w->position(X + std::max(0, (W - w->w()) / 2) + dx, Y + std::max(0, (H - w->h()) / 2) + dy);
}

int eggHandler(int e);          // Shift+Cmd+J (defined with the banner below)

void midiTick(void*) {          // look for MIDI devices plugged in or out
    App::get().midi.refresh();
    Fl::repeat_timeout(1.0, midiTick);
}

void schedulerTimer(void*) {
    Patch& h = App::get().host;
    h.runDue(h.time());           // re-arms through onSchedule
}

// Size of a window that shows all of a patch's boxes.
void fitPatch(Patch* p, int& W, int& H) {
    W = 420;
    H = 300;
    fl_font(kFont, kFontSize);
    for (auto& x : p->boxes) {
        int bw = std::max(x->w, (int)fl_width(x->label().c_str()) + 24);
        W = std::max(W, x->x + bw + 40);
        H = std::max(H, x->y + x->h + 50);
    }
    W = std::min(W, 1400);
    H = std::min(H + 22, 900);
}

std::string patchTitle(Patch* p) {
    std::string t = p->owner ? p->owner->label() : "patch";
    if (p->isAbstraction) t += "  (" + baseName(p->file) + ")";
    return t;
}

// path of box ids from the top patch, e.g. "p1/fm2", to reopen after undo
std::string pathOf(Patch* p) {
    std::string r;
    for (; p && p->owner; p = p->parent) r = p->owner->id + (r.empty() ? "" : "/" + r);
    return r;
}

void setMenuCheck(Fl_Menu_Bar* m, const char* path, bool on) {
    if (!m) return;
    auto it = const_cast<Fl_Menu_Item*>(m->find_item(path));
    if (!it) return;
    if (on) it->set();
    else it->clear();
    m->update();
}
}  // namespace

// ============================================================================
// App: audio, MIDI, console, help windows, documents
// ============================================================================

App& App::get() {
    static App* app = new App;      // never destroyed: windows may outlive main's scope
    return *app;
}

void App::init() {
    Fl::keyboard_screen_scaling(0);      // Cmd +/- zoom the patch (View menu), not the whole interface
    openCodeSource = [this](CodeSource* s) { openCodeFor(s); };
    codeSourceGone = [this](CodeSource* s) { codeSourceClosed(s); };
    Fl::add_timeout(1.0 / 15, toolTimer);
    Patch::onDestroyed = [this](Patch* p) {
        for (auto* d : docs) d->closeSubpatchWindows(p);
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
#endif
    log("Moritz " MORITZ_VERSION " - created by Carmine-Emanuele Cella (2026)");
    log("this console shows print objects, errors, audio and file events");
}

Fl_Menu_Bar* App::menuFor(MainWindow* w) {
    if (sysMenu_) return w == frontDoc() ? sysMenu_ : nullptr;
    return w ? w->menu_ : nullptr;
}

namespace {
// the text editor the Edit menu should act on: the focused one, or the
// editor of a text view that is the front window
Fl_Text_Editor* frontTextEditor() {
    if (auto* ed = dynamic_cast<Fl_Text_Editor*>(Fl::focus())) return ed;
    if (auto* tw = dynamic_cast<PatchTextWindow*>(Fl::first_window())) return tw->editor();
    return nullptr;
}

struct NewItem {
    std::string cls;
};
}  // namespace

void App::buildMenu(Fl_Menu_Bar* m) {
#define APP_CB(method) [](Fl_Widget*, void*) { App::get().method(); }
#define DOC_CB(method) [](Fl_Widget*, void*) { if (auto* d = App::get().frontDoc()) d->method(); }
    m->add("File/New", FL_COMMAND + 'n', APP_CB(newDocument), nullptr);
    m->add("File/New Code", FL_COMMAND + FL_SHIFT + 'n', [](Fl_Widget*, void*) { App::get().newCode(); }, nullptr);
    m->add("File/Open...", FL_COMMAND + 'o', APP_CB(openDialog), nullptr);
    m->add("File/Import from Max...", FL_COMMAND + FL_SHIFT + 'i', APP_CB(importDialog), nullptr,
           FL_MENU_DIVIDER);
    m->add("File/Save", FL_COMMAND + 's', [](Fl_Widget*, void*) {
        if (auto* c = App::get().frontCode()) { c->save(); return; }
        if (auto* d = App::get().frontDoc()) d->save();
    }, nullptr);
    m->add("File/Close", FL_COMMAND + 'w', APP_CB(closeFront), nullptr);
    m->add("File/Save as...", FL_COMMAND + FL_SHIFT + 's', [](Fl_Widget*, void*) {
        if (auto* c = App::get().frontCode()) { c->saveAs(); return; }
        if (auto* d = App::get().frontDoc()) d->saveAs();
    }, nullptr, FL_MENU_DIVIDER);
    m->add("File/Quit", FL_COMMAND + 'q', APP_CB(quit), nullptr);
    // with a text view in front, the Edit commands work on its text
#define TEXT_OR(kf, call) [](Fl_Widget*, void*) {                                  \
        if (auto* ed = frontTextEditor()) { Fl_Text_Editor::kf(0, ed); return; } \
        call; }
    m->add("Edit/Undo", FL_COMMAND + 'z',
           TEXT_OR(kf_undo, if (auto* d = App::get().frontDoc()) d->undo()), nullptr);
    m->add("Edit/Redo", FL_COMMAND + FL_SHIFT + 'z',
           TEXT_OR(kf_redo, if (auto* d = App::get().frontDoc()) d->redo()), nullptr, FL_MENU_DIVIDER);
    m->add("Edit/Cut", FL_COMMAND + 'x',
           TEXT_OR(kf_cut, if (auto* c = App::get().activeCanvas()) c->cutSelection()), nullptr);
    m->add("Edit/Copy", FL_COMMAND + 'c',
           TEXT_OR(kf_copy, if (auto* c = App::get().activeCanvas()) c->copySelection()), nullptr);
    m->add("Edit/Paste", FL_COMMAND + 'v',
           TEXT_OR(kf_paste, if (auto* c = App::get().activeCanvas()) c->paste()), nullptr);
    m->add("Edit/Duplicate", FL_COMMAND + 'd',
           [](Fl_Widget*, void*) { if (auto* c = App::get().activeCanvas()) c->duplicateSelection(15, 15); },
           nullptr, FL_MENU_DIVIDER);
    m->add("Edit/Select All", FL_COMMAND + 'a',
           TEXT_OR(kf_select_all, if (auto* c = App::get().activeCanvas()) c->selectAll()), nullptr);
#ifndef __APPLE__
    m->add("Edit/Settings...", FL_COMMAND + ',', [](Fl_Widget*, void*) { showSettings(); }, nullptr,
           FL_MENU_DIVIDER);       // (on macOS: Moritz > Settings…)
#endif
#undef TEXT_OR

    static std::vector<std::unique_ptr<NewItem>> items;
    auto addNew = [&](const std::string& path, const std::string& cls) {
        items.push_back(std::make_unique<NewItem>(NewItem{cls}));
        m->add(path.c_str(), 0,
               [](Fl_Widget*, void* d) {
                   if (auto* c = App::get().activeCanvas()) c->newBoxAtMouse(static_cast<NewItem*>(d)->cls);
               },
               items.back().get());
    };
    for (auto& c : classInfos())
        if (!c.group.empty())                            // not the short aliases
            addNew("Patch/New/" + c.group + "/" + escapeMenu(c.name), c.name);
    for (auto& name : libraryNames())                    // abstractions in the libraries
        addNew("Patch/New/Library/" + escapeMenu(name), name);
    m->add("Patch/Edit Mode", FL_COMMAND + 'e', DOC_CB(toggleLock), nullptr, FL_MENU_TOGGLE);
    m->add("Patch/Code View", FL_COMMAND + 't', [](Fl_Widget*, void*) { App::get().showText(); }, nullptr);
    m->add("Patch/Audio On", FL_COMMAND + '/', APP_CB(toggleAudio), nullptr,
           FL_MENU_TOGGLE | FL_MENU_DIVIDER);
    m->add("Patch/Audio Setup...", 0, APP_CB(audioSetup), nullptr);
    m->add("Patch/MIDI Setup...", 0, APP_CB(midiSetup), nullptr, FL_MENU_DIVIDER);
    m->add("Patch/Console", FL_COMMAND + FL_SHIFT + 'm', APP_CB(showConsole), nullptr);
    m->add("View/Zoom In", FL_COMMAND + '=', [](Fl_Widget*, void*) { App::get().zoomFront(1); }, nullptr);
    m->add("View/Zoom Out", FL_COMMAND + '-', [](Fl_Widget*, void*) { App::get().zoomFront(-1); }, nullptr);
    m->add("View/Actual Size", FL_COMMAND + '0', [](Fl_Widget*, void*) { App::get().zoomFront(0); }, nullptr,
           FL_MENU_DIVIDER);
    m->add("View/Interface Larger", 0, [](Fl_Widget*, void*) { App::get().scaleInterface(1.1f); }, nullptr);
    m->add("View/Interface Smaller", 0, [](Fl_Widget*, void*) { App::get().scaleInterface(1 / 1.1f); }, nullptr);
    m->add("View/Interface Normal Size", 0, [](Fl_Widget*, void*) { App::get().scaleInterface(0); }, nullptr);
    m->add("Extras/Quick Recorder...", FL_COMMAND + FL_SHIFT + 'r', APP_CB(showRecorder), nullptr);
    m->add("Extras/Start or Stop Recording", FL_COMMAND + FL_ALT + 'r', APP_CB(toggleRecording), nullptr,
           FL_MENU_DIVIDER);
    m->add("Extras/Binaural Monitor...", 0, APP_CB(showMonitor), nullptr);
    m->add("Extras/Channel Tester...", 0, APP_CB(showTester), nullptr);
    // Help > Tutorials: the numbered patches in the tutorials folder
    static std::vector<std::string> tutorials;
    if (tutorials.empty())
        if (std::string dir = resourceDir("tutorials"); !dir.empty()) {
            dirent** list = nullptr;
            int n = fl_filename_list(dir.c_str(), &list, fl_casealphasort);
            for (int k = 0; k < n; ++k) {
                std::string f = list[k]->d_name;
                if (f.size() > 7 && f.compare(f.size() - 7, 7, ".moritz") == 0) tutorials.push_back(dir + "/" + f);
            }
            fl_filename_free_list(&list, n);
        }
    m->add("Help/Browser...", FL_COMMAND + FL_SHIFT + 'b', APP_CB(showBrowser), nullptr, FL_MENU_DIVIDER);
    // Help > Documents: the PDFs in the docs folder (and its folders), and Musil's manual
    static std::vector<std::string> documents;
    if (documents.empty())
        if (std::string dir = resourceDir("docs"); !dir.empty()) {
            std::function<void(const std::string&, int)> scan = [&](const std::string& d, int depth) {
                dirent** list = nullptr;
                int n = fl_filename_list(d.c_str(), &list, fl_casealphasort);
                for (int k = 0; k < n; ++k) {
                    std::string f = list[k]->d_name;
                    if (!f.empty() && f.back() == '/') f.pop_back();
                    if (f.empty() || f[0] == '.') continue;
                    std::string full = d + "/" + f;
                    if (fl_filename_isdir(full.c_str())) { if (depth < 2) scan(full, depth + 1); }
                    else if (f.size() > 4 && (f.compare(f.size() - 4, 4, ".pdf") == 0 || f.compare(f.size() - 4, 4, ".PDF") == 0))
                        documents.push_back(full);
                }
                fl_filename_free_list(&list, n);
            };
            scan(dir, 0);
        }
    if (static bool musil = false; !musil) {            // and Musil's manual, from Musil's own folder
        musil = true;
        if (std::string d = resourceDir("musil"); !d.empty())
            if (std::string f = d + "/docs/musil_manual.pdf"; fl_access(f.c_str(), 0) == 0) documents.push_back(f);
    }
    for (auto& path : documents) {
        std::string name = baseName(path);
        name = name.substr(0, name.size() - 4);
        for (auto& c : name) if (c == '-' || c == '_') c = ' ';
        if (!name.empty()) name[0] = (char)std::toupper((unsigned char)name[0]);
        m->add(("Help/Documents/" + escapeMenu(name)).c_str(), 0,
               [](Fl_Widget*, void* d) { openWithSystem(*static_cast<std::string*>(d)); }, &path);
    }
    if (documents.empty()) m->add("Help/Documents", 0, nullptr, nullptr, FL_MENU_INACTIVE);
    for (auto& path : tutorials) {
        std::string name = baseName(path);
        name = name.substr(0, name.size() - 7);             // "01-first-patch" -> "01 first patch"
        for (auto& c : name) if (c == '-') c = ' ';
        m->add(("Help/Tutorials/" + escapeMenu(name)).c_str(), 0,
               [](Fl_Widget*, void* d) { App::get().openDocument(*static_cast<std::string*>(d)); },
               &path);
    }
    if (tutorials.empty()) m->add("Help/Tutorials", 0, nullptr, nullptr, FL_MENU_INACTIVE);
#ifndef __APPLE__
    m->add("Help/About Moritz", 0, APP_CB(about), nullptr);
#endif
#undef APP_CB
#undef DOC_CB
}

MainWindow* App::frontDoc() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (auto mw = dynamic_cast<MainWindow*>(w)) return mw;
        if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->doc) return pw->doc;
    }
    return docs.empty() ? nullptr : docs.back();
}

Canvas* App::activeCanvas() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (auto mw = dynamic_cast<MainWindow*>(w)) return mw->canvas;
        if (auto pw = dynamic_cast<PatchWindow*>(w)) return pw->canvas;
    }
    return docs.empty() ? nullptr : docs.back()->canvas;
}

MainWindow* App::newDocument() {
    MainWindow* front = frontDoc();
    auto* w = new MainWindow(960, 640);
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

MainWindow* App::openDocument(const std::string& path) {
    for (auto* d : docs)                              // already open: bring it forward
        if (!d->path().empty() && d->path() == path) { d->show(); return d; }
    if (path.size() > 3 && path.compare(path.size() - 3, 3, ".mu") == 0) {   // a Musil script: the editor
        openCode(path);
        return nullptr;
    }
    MainWindow* d = frontDoc();
    bool fresh = !(d && d->isEmptyUntitled());
    if (fresh) d = newDocument();
    if (!d->openFile(path)) {
        if (fresh) closeDocument(d);
        return nullptr;
    }
    d->show();
    return d;
}

void App::openDialog() {
    Fl_Native_File_Chooser fc;
    fc.title("Open");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Moritz patch or Musil script\t*.{moritz,mu,mlsp}\nAll files\t*");
    MainWindow* front = frontDoc();
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

MainWindow* App::importMax(const std::string& path) {
    std::string text, err;
    MaxImportReport rep;
    if (!importMaxPatch(path, true, text, rep, err)) {
        alertUser("Can't import %s:\n%s", baseName(path).c_str(), err.c_str());
        return nullptr;
    }
    std::string name = baseName(path);
    name = name.substr(0, name.find_last_of('.'));
    MainWindow* d = frontDoc();
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

void App::closeDocument(MainWindow* w) {
    docs.erase(std::remove(docs.begin(), docs.end(), w), docs.end());
    w->closeAllSubwindows();
    w->hide();
    w->canvas->reset();
    Patch* p = &w->patch;
    p->onRedraw = nullptr;
    p->onStatus = nullptr;
    p->onPrint = nullptr;
    p->onStructure = nullptr;
    Fl::delete_widget(w);              // later: the window must not touch the patch then
    host.removeExtra(p);               // stops its sound, timers and jobs
#ifndef __APPLE__
    if (docs.empty()) quit();          // no menu bar is left to do anything else
#endif
    refreshStatus();
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
    if (auto d = dynamic_cast<MainWindow*>(w)) {
        if (d->confirmDiscard()) closeDocument(d);
    }
    else if (w && w->visible()) w->hide();        // the browser, an about box...
}

void App::quit() {
    for (auto* d : std::vector<MainWindow*>(docs)) {
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
    for (auto* d : std::vector<MainWindow*>(docs)) d->closeAllSubwindows();
    for (auto* w : helpWindows_) w->hide();
    for (auto* w : textWindows_) w->hide();
    hideBrowser();
    if (console_) console_->hide();
    for (auto* d : docs) d->hide();
}

void App::refreshStatus() {
    for (auto* d : docs) d->refreshStatus();
}

void App::armTimer() {
    Fl::remove_timeout(schedulerTimer);
    double next = host.nextDue();
    if (std::isfinite(next))
        Fl::add_timeout(std::max(0.0, next - host.time()), schedulerTimer);
}

void App::showReference(const std::string& cls) {
    if (!referenceWin_) {
        referenceWin_ = new Fl_Double_Window(620, 560, "Reference");
        centerOnScreen(referenceWin_, frontDoc(), 60, 30);
        referenceWin_->color(kStatusBg);
        referenceBuf_ = new Fl_Text_Buffer();
        referenceStyle_ = new Fl_Text_Buffer();
        referenceView_ = new Fl_Text_Display(0, 0, 620, 560);
        referenceView_->buffer(referenceBuf_);
        referenceView_->box(FL_FLAT_BOX);
        referenceView_->color(fl_rgb_color(24, 25, 28));
        referenceView_->textcolor(kText);
        referenceView_->textfont(FL_HELVETICA);
        referenceView_->textsize(13);
        referenceView_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        static Fl_Text_Display::Style_Table_Entry styles[] = {
            {fl_rgb_color(226, 228, 234), FL_HELVETICA, 13, 0, 0},               // A text
            {fl_rgb_color(150, 190, 255), FL_HELVETICA_BOLD, 15, 0, 0},          // B the syntax
            {fl_rgb_color(150, 155, 165), FL_HELVETICA_BOLD, 11, 0, 0},          // C headings
            {fl_rgb_color(235, 200, 120), FL_COURIER_BOLD, 13, 0, 0},            // D a message or argument
        };
        referenceView_->highlight_data(referenceStyle_, styles, 4, 'A', nullptr, nullptr);
        referenceWin_->resizable(referenceView_);
        referenceWin_->end();
    }
    std::string text = referenceText(cls);
    referenceBuf_->text(text.c_str());
    // styles: the first line, the headings (capitals), the names in the tables
    std::string st(text.size(), 'A');
    size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        char c = 'A';
        if (first && !line.empty()) { c = 'B'; first = false; }
        else if (!line.empty() && line.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ ") == std::string::npos) c = 'C';
        else if (line.rfind("  ", 0) == 0 && line.rfind("      ", 0) != 0) c = 'D';
        std::fill(st.begin() + (long)pos, st.begin() + (long)nl, c);
        pos = nl + 1;
    }
    referenceStyle_->text(st.c_str());
    referenceWin_->copy_label((cls + " reference").c_str());
    referenceView_->scroll(0, 0);
    referenceWin_->show();
}

void App::showConsole() {
    if (!console_) {
        console_ = new Fl_Double_Window(640, 380, "Moritz Console");
        centerOnScreen(console_, frontDoc());
        console_->color(kStatusBg);
        consoleBuf_ = new Fl_Text_Buffer();
        consoleView_ = new Fl_Text_Display(0, 0, 640, 320);
        auto* hint = new Fl_Box(8, 322, 540, 22,
                                "print objects, errors, audio and file events; below, Musil, the scripting language");
        hint->labelsize(11);
        hint->labelcolor(kDim);
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        auto* clear = new Fl_Button(574, 322, 60, 20, "Clear");
        clear->labelsize(11);
        clear->callback([](Fl_Widget*, void*) { App::get().clearConsole(); });
        clear->clear_visible_focus();
        auto* row = new Fl_Group(0, 348, 640, 28);
        buildConsolePrompt(console_, 6, 350, 628, 24);
        row->end();
        row->resizable(nullptr);
        consoleView_->buffer(consoleBuf_);
        consoleView_->box(FL_FLAT_BOX);
        consoleView_->color(fl_rgb_color(20, 21, 24));
        consoleView_->textcolor(kText);
        consoleView_->textfont(kCodeFontSlot);
        consoleView_->textsize(settings().codeSize - 1);
        console_->resizable(consoleView_);
        console_->end();
        for (auto& line : consoleLines_) consoleBuf_->append((line + "\n").c_str());
    }
    console_->show();
    consoleView_->scroll(consoleBuf_->count_lines(0, consoleBuf_->length()), 0);
    consoleAlert = false;
    refreshTools();
}

void App::ensureConsole() {
    if (!console_ || !console_->visible()) showConsole();
}

CodeWindow* App::newCode() {
    auto* w = new CodeWindow();
    codeWindows_.push_back(w);
    centerOnScreen(w, frontDoc());
    w->show();
    w->editor()->take_focus();
    return w;
}

CodeWindow* App::openCode(const std::string& path) {
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
        if (!w->load(f)) {                              // a file not written yet: it is made on Save
            std::ofstream make(f, std::ios::app);
            if (!make || !w->load(f)) { delete w; print("code: can't open " + f); return nullptr; }
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
    for (auto* w : codeWindows_)
        if (w->source() == s) { closeCode(w); return; }
}

void App::markEdited(Patch* p) {
    for (auto* d : docs)
        for (Patch* q = p; q; q = q->parent)
            if (q == &d->patch) { d->markDirty(); return; }
}

void App::closeCode(CodeWindow* w) {
    codeWindows_.erase(std::remove(codeWindows_.begin(), codeWindows_.end(), w), codeWindows_.end());
    w->hide();
    Fl::delete_widget(w);
}

CodeWindow* App::frontCode() {
    return dynamic_cast<CodeWindow*>(Fl::first_window());
}

// ---- tools: zoom, interface size, quick recorder, the toolbar's meters

void App::zoomFront(int steps) {
    if (Canvas* c = activeCanvas()) c->zoomBy(steps);
}

void App::scaleInterface(float factor) {
    for (int n = 0; n < Fl::screen_count(); ++n) {
        float s = factor == 0 ? 1.f : std::clamp(Fl::screen_scale(n) * factor, 0.5f, 3.f);
        Fl::screen_scale(n, s);
    }
}

std::string App::recordingsFolder() {
    if (recordingsDir_.empty()) {
        char buf[4096] = "";
        appPrefs().get("recordings_folder", buf, "", sizeof buf);
        recordingsDir_ = buf;
    }
    if (recordingsDir_.empty()) {
        const char* home = std::getenv("HOME");
#ifdef _WIN32
        if (!home) home = std::getenv("USERPROFILE");
#endif
        std::string base = home ? home : ".";
        std::string music = base + "/Music";
        recordingsDir_ = (fl_filename_isdir(music.c_str()) ? music : base) + "/Moritz Recordings";
    }
    return recordingsDir_;
}

void App::chooseRecordingsFolder() {
    Fl_Native_File_Chooser fc;
    fc.title("Folder for recordings");
    fc.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    fc.directory(recordingsFolder().c_str());
    if (fc.show() != 0) return;
    recordingsDir_ = fc.filename();
    appPrefs().set("recordings_folder", recordingsDir_.c_str());
    appPrefs().flush();
    refreshTools();
}

void App::toggleRecording() {
    Recorder& r = audio.recorder;
    if (r.recording()) {
        r.stop();
        print("recorded " + r.path());
        refreshTools();
        return;
    }
    if (!audio.running()) setAudio(true);
    if (!audio.running()) { print("quick recorder: the audio is off"); refreshTools(); return; }
    std::string dir = recordingsFolder();
    fl_make_path(dir.c_str());
    std::time_t t = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", std::localtime(&t));
    std::string path = dir + "/Moritz " + stamp + ".wav";
    if (!r.start(path, audio.channels(), audio.sampleRate())) print("quick recorder: can't write " + path);
    refreshTools();
}

void App::showRecorder() {
    if (!recorderWin_) {
        recorderWin_ = new Fl_Double_Window(420, 132, "Quick Recorder");
        centerOnScreen(recorderWin_, frontDoc());
        recorderWin_->color(kStatusBg);
        recorderBtn_ = new ToolButton(14, 14, 110, 30, ToolButton::Record, "Record", false);
        recorderBtn_->callback([](Fl_Widget*, void*) { App::get().toggleRecording(); });
        recorderTime_ = new Fl_Box(134, 14, 120, 30, "0:00.0");
        recorderTime_->labelfont(FL_COURIER_BOLD);
        recorderTime_->labelsize(18);
        recorderTime_->labelcolor(kText);
        recorderTime_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        auto* hint = new Fl_Box(14, 50, 392, 20, "Records what goes to the dac (24-bit WAV, the audio's sample rate).");
        hint->labelsize(11);
        hint->labelcolor(kDim);
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        recorderFile_ = new Fl_Box(14, 70, 392, 20);
        recorderFile_->labelsize(11);
        recorderFile_->labelcolor(kText);
        recorderFile_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        auto* choose = new Fl_Button(14, 98, 120, 22, "Choose folder...");
        choose->labelsize(11);
        choose->callback([](Fl_Widget*, void*) { App::get().chooseRecordingsFolder(); });
        auto* open = new Fl_Button(142, 98, 120, 22, "Open folder");
        open->labelsize(11);
        open->callback([](Fl_Widget*, void*) {
            std::string dir = App::get().recordingsFolder();
            fl_make_path(dir.c_str());
            openWithSystem(dir);
        });
        recorderWin_->end();
    }
    refreshTools();
    recorderWin_->show();
}

void App::refreshTools() {
    const Recorder& r = audio.recorder;
    bool rec = r.recording();
    char t[32];
    double sec = rec ? r.seconds() : 0;
    std::snprintf(t, sizeof t, "%d:%02d.%d", (int)sec / 60, (int)sec % 60, (int)(sec * 10) % 10);
    for (auto* d : docs) {
        d->recordBtn_->lit = rec;
        d->recordBtn_->copy_label(rec ? t : "Record");
        d->recordBtn_->redraw();
        if (d->consoleBtn_->alert != consoleAlert) {
            d->consoleBtn_->alert = consoleAlert;
            d->consoleBtn_->redraw();
        }
    }
    if (recorderWin_) {
        recorderBtn_->lit = rec;
        recorderBtn_->copy_label(rec ? "Stop" : "Record");
        recorderBtn_->redraw();
        recorderTime_->copy_label(t);
        std::string f = rec ? "→ " + baseName(r.path()) : "into " + recordingsFolder();
        if (rec && r.overflowed()) f += "   (the disk was too slow: some audio was lost)";
        recorderFile_->copy_label(f.c_str());
        recorderWin_->redraw();
    }
}

// ---- the binaural monitor

namespace {
// the speakers of a ring of n: 0, 1, 2 the layouts of the choice
std::vector<std::pair<float, float>> ringLayout(int layout, int n) {
    std::vector<std::pair<float, float>> v;
    for (int k = 0; k < n; ++k) {
        double a = layout == 0 ? 180.0 / n + 360.0 * k / n          // 1 front-left, counterclockwise (as ambidec~)
                 : layout == 1 ? 180.0 / n - 360.0 * k / n          // 1 front-left, clockwise
                               : 360.0 * k / n;                     // 1 front, counterclockwise
        v.push_back({(float)std::remainder(a, 360.0), 0.f});
    }
    return v;
}
std::string layoutText(const std::vector<std::pair<float, float>>& v) {
    std::string t;
    char b[32];
    for (auto& s : v) {
        if (s.second != 0) std::snprintf(b, sizeof b, "%g/%g", s.first, s.second);
        else std::snprintf(b, sizeof b, "%g", s.first);
        t += (t.empty() ? "" : " ") + std::string(b);
    }
    return t;
}
std::vector<std::pair<float, float>> parseLayout(const std::string& t) {
    std::vector<std::pair<float, float>> v;
    size_t k = 0;
    while (k < t.size()) {
        while (k < t.size() && (t[k] == ' ' || t[k] == ',' || t[k] == '\t')) ++k;
        size_t e = k;
        while (e < t.size() && t[e] != ' ' && t[e] != ',' && t[e] != '\t') ++e;
        if (e > k) {
            std::string w = t.substr(k, e - k);
            size_t sl = w.find('/');
            float az = (float)std::atof(w.substr(0, sl).c_str()), el = sl == std::string::npos ? 0.f : (float)std::atof(w.substr(sl + 1).c_str());
            v.push_back({az, el});
        }
        k = e;
    }
    return v;
}
struct MonitorUi {
    Fl_Check_Button* on;
    Fl_Choice* layout;
    Fl_Choice* count;
    Fl_Input* dirs;
    Fl_Box* info;
};
MonitorUi gMon;
}  // namespace

void App::applyMonitor() {
    MonitorSettings& m = monitorSet_;
    int n = m.speakers > 0 ? m.speakers : std::max(2, audio.channels());
    std::vector<std::pair<float, float>> sp = m.layout == 3 ? parseLayout(m.custom) : ringLayout(m.layout, n);
    if (!m.on || sp.empty() || !audio.running()) audio.setMonitor({});
    else audio.setMonitor(sp);
    if (monitorWin_) {
        if (m.layout != 3) gMon.dirs->value(layoutText(sp).c_str());
        std::string i = !m.on ? "Off: the device plays the channels it has."
                       : !audio.running() ? "On when the audio is on."
                                          : std::to_string(sp.size()) + " speakers rendered to the device's channels 1 and 2 (headphones). "
                                            "The quick recorder still records the speaker channels.";
        gMon.info->copy_label(i.c_str());
        monitorWin_->redraw();
    }
}

void App::showMonitor() {
    if (!monitorWin_) {
        monitorWin_ = new Fl_Double_Window(480, 240, "Binaural Monitor");
        centerOnScreen(monitorWin_, frontDoc());
        monitorWin_->color(kStatusBg);
        gMon.on = new Fl_Check_Button(14, 12, 440, 24, "Listen to the speakers on headphones (binaural)");
        gMon.on->labelcolor(kText);
        gMon.layout = new Fl_Choice(110, 46, 356, 24, "Speakers:");
        gMon.layout->add("Ring: 1 front-left, counterclockwise (as ambidec~)");
        gMon.layout->add("Ring: 1 front-left, clockwise");
        gMon.layout->add("Ring: 1 front, counterclockwise");
        gMon.layout->add("Custom directions");
        gMon.count = new Fl_Choice(110, 78, 90, 24, "How many:");
        gMon.count->add("Patches");
        for (int c : {2, 4, 5, 6, 8, 10, 12, 16, 24, 32}) gMon.count->add(std::to_string(c).c_str());
        gMon.dirs = new Fl_Input(110, 110, 356, 24, "Directions:");
        gMon.dirs->tooltip("One per channel, in degrees: the azimuth (positive to the left, 0 in front), "
                           "or azimuth/elevation (90/30). Edit them to make a custom layout.");
        auto* hint = new Fl_Box(14, 140, 452, 34,
                                "Each channel is placed as a speaker around you with the MIT KEMAR head responses; "
                                "channels 1 and 2 of the device carry the mix.");
        hint->labelsize(11);
        hint->labelcolor(kDim);
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        gMon.info = new Fl_Box(14, 180, 452, 46);
        gMon.info->labelsize(11);
        gMon.info->labelcolor(kText);
        gMon.info->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        for (Fl_Widget* w : {(Fl_Widget*)gMon.layout, (Fl_Widget*)gMon.count, (Fl_Widget*)gMon.dirs}) w->labelcolor(kText);
        auto changed = [](Fl_Widget* w, void*) {
            App& app = App::get();
            MonitorSettings& m = app.monitorSet_;
            m.on = gMon.on->value();
            if (w == gMon.dirs) { m.layout = 3; m.custom = gMon.dirs->value(); gMon.layout->value(3); }
            else {
                m.layout = gMon.layout->value();
                if (m.layout == 3) m.custom = gMon.dirs->value();
                const int counts[] = {0, 2, 4, 5, 6, 8, 10, 12, 16, 24, 32};
                m.speakers = counts[std::clamp(gMon.count->value(), 0, 10)];
            }
            if (m.on && !app.audio.running()) app.setAudio(true);
            app.applyMonitor();
        };
        gMon.on->callback(changed);
        gMon.layout->callback(changed);
        gMon.count->callback(changed);
        gMon.dirs->callback(changed);
        gMon.dirs->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
        monitorWin_->end();
        gMon.layout->value(0);
        gMon.count->value(0);
    }
    gMon.on->value(monitorSet_.on);
    applyMonitor();
    monitorWin_->show();
}

// ---- the channel tester

void App::refreshTester() {
    if (!testerWin_) return;
    int n = std::clamp(audio.running() ? audio.channels() : 2, 1, AudioEngine::kMaxBus);
    int cur = audio.testChannel.load();
    if (testerGrid_->children() != n) {
        testerGrid_->clear();
        testerGrid_->begin();
        int cols = 8, bw = 52, bh = 30;
        for (int c = 0; c < n; ++c) {
            auto* b = new ToolButton(testerGrid_->x() + (c % cols) * (bw + 6), testerGrid_->y() + (c / cols) * (bh + 6), bw, bh,
                                     ToolButton::None, std::to_string(c + 1).c_str(), false);
            b->callback([](Fl_Widget* w, void* d) {
                App& app = App::get();
                int ch = (int)(intptr_t)d;
                app.testerSteps_ = 0;
                if (!app.audio.running()) app.setAudio(true);
                app.audio.testChannel = app.audio.testChannel.load() == ch ? -1 : ch;
                app.refreshTester();
                (void)w;
            }, (void*)(intptr_t)c);
        }
        testerGrid_->end();
        int rows = (n + 7) / 8;
        testerWin_->size(testerWin_->w(), 130 + rows * 36);
    }
    for (int c = 0; c < testerGrid_->children(); ++c) {
        auto* b = static_cast<ToolButton*>(testerGrid_->child(c));
        b->lit = c == cur;
        b->redraw();
    }
    std::string i;
    int dev = audio.deviceOutputs();
    if (!audio.running()) i = "The audio is off: click a channel to turn it on.";
    else if (cur < 0) i = "Click a channel: bursts of noise come out of its speaker. Click it again to stop.";
    else if (audio.monitoring()) i = "Channel " + std::to_string(cur + 1) + ": heard through the binaural monitor, where its speaker is.";
    else if (cur >= dev) i = "Channel " + std::to_string(cur + 1) + ": the device has " + std::to_string(dev) + " outputs, so it is not heard (the meter shows it).";
    else i = "Channel " + std::to_string(cur + 1) + " of " + std::to_string(dev) + ": the speaker it comes from is number " + std::to_string(cur + 1) + ".";
    testerInfo_->copy_label(i.c_str());
    testerWin_->redraw();
}

void App::testerTimer(void*) {
    App& app = App::get();
    if (!app.testerWin_ || !app.testerWin_->shown() || app.testerSteps_ == 0) return;
    int n = std::max(1, app.audio.channels());
    app.audio.testChannel = (app.audio.testChannel.load() + 1) % n;
    app.refreshTester();
    Fl::repeat_timeout(1.5, testerTimer);
}

void App::showTester() {
    if (!testerWin_) {
        testerWin_ = new Fl_Double_Window(470, 166, "Channel Tester");
        centerOnScreen(testerWin_, frontDoc());
        testerWin_->color(kStatusBg);
        auto* step = new Fl_Button(14, 12, 130, 26, "Step through all");
        step->labelsize(12);
        step->callback([](Fl_Widget*, void*) {
            App& app = App::get();
            if (!app.audio.running()) app.setAudio(true);
            app.testerSteps_ = 1;
            app.audio.testChannel = 0;
            app.refreshTester();
            Fl::remove_timeout(testerTimer);
            Fl::add_timeout(1.5, testerTimer);
        });
        auto* stop = new Fl_Button(150, 12, 70, 26, "Stop");
        stop->labelsize(12);
        stop->callback([](Fl_Widget*, void*) {
            App& app = App::get();
            app.testerSteps_ = 0;
            app.audio.testChannel = -1;
            app.refreshTester();
        });
        auto* level = new Fl_Value_Slider(290, 12, 166, 26, "Level (dB)");
        level->type(FL_HOR_NICE_SLIDER);
        level->labelsize(11);
        level->labelcolor(kText);
        level->align(FL_ALIGN_LEFT);
        level->range(-40, 0);
        level->step(1);
        level->value(-14);
        level->callback([](Fl_Widget* w, void*) {
            App::get().audio.testLevel = (float)std::pow(10.0, static_cast<Fl_Value_Slider*>(w)->value() / 20.0);
        });
        testerInfo_ = new Fl_Box(14, 44, 442, 36);
        testerInfo_->labelsize(11);
        testerInfo_->labelcolor(kText);
        testerInfo_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        testerGrid_ = new Fl_Group(14, 88, 442, 40);
        testerGrid_->end();
        testerWin_->end();
        testerWin_->callback([](Fl_Widget* w, void*) {           // closing it stops the noise
            App& app = App::get();
            app.testerSteps_ = 0;
            app.audio.testChannel = -1;
            w->hide();
        });
        audio.testLevel = (float)std::pow(10.0, -14 / 20.0);
    }
    refreshTester();
    testerWin_->show();
}

void App::toolTimer(void*) {
    App& app = App::get();
    float cpu = app.audio.running() ? app.host.cpuLoad.load(std::memory_order_relaxed) : 0.f;
    std::vector<float> peaks((size_t)std::clamp(app.audio.channels(), 1, AudioEngine::kMeterChannels));
    for (size_t c = 0; c < peaks.size(); ++c) peaks[c] = app.audio.takePeak((int)c);
    for (auto* d : app.docs) d->meters_->update(cpu, peaks);
    if (app.audio.recorder.recording()) app.refreshTools();
    Fl::repeat_timeout(1.0 / 15, toolTimer);
}

void App::clearConsole() {
    consoleLines_.clear();
    if (consoleBuf_) consoleBuf_->text("");
}

void App::print(const std::string& s) {
    log(s);
    if (auto* d = frontDoc()) d->setStatus(s);
}

void App::log(const std::string& s) {
    std::string low = s;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (!consoleAlert && (low.find("error") != std::string::npos || low.find("can't") != std::string::npos ||
                          low.find("unknown") != std::string::npos || low.find("fail") != std::string::npos)) {
        consoleAlert = true;
        refreshTools();
    }
    consoleLines_.push_back(s);
    if (consoleLines_.size() > 2000) consoleLines_.erase(consoleLines_.begin());
    if (consoleBuf_) {
        consoleBuf_->append((s + "\n").c_str());
        if (consoleBuf_->length() > 200000) consoleBuf_->remove(0, consoleBuf_->length() - 150000);
        consoleView_->scroll(consoleBuf_->count_lines(0, consoleBuf_->length()), 0);
    }
}

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
}

PatchTextWindow* App::textWindow(Patch* p) {
    for (auto* w : textWindows_)
        if (w->patch == p) return w;
    return nullptr;
}

void App::showText() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (auto tw = dynamic_cast<PatchTextWindow*>(w)) { tw->do_callback(); return; }   // toggles
        if (auto mw = dynamic_cast<MainWindow*>(w)) {
            showText(mw, &mw->patch, mw->label() ? mw->label() : "patch", false);
            return;
        }
        if (auto pw = dynamic_cast<PatchWindow*>(w); pw && pw->patch) {
            showText(pw->doc, pw->patch, pw->label() ? pw->label() : "patch", pw->help);
            return;
        }
    }
}

void App::showText(MainWindow* doc, Patch* p, const std::string& title, bool readOnly) {
    PatchTextWindow* tw = textWindow(p);
    if (!tw) {
        std::string t = title;
        if (size_t k = t.find(" — Moritz"); k != std::string::npos) t.resize(k);
        tw = new PatchTextWindow(doc, p, t, readOnly);
        textWindows_.push_back(tw);
        Fl_Window* beside = nullptr;                     // beside the patch's window
        for (Fl_Window* w = Fl::first_window(); w && !beside; w = Fl::next_window(w))
            if (auto mw = dynamic_cast<MainWindow*>(w); mw && &mw->patch == p) beside = w;
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

void App::openHelp(const std::string& cls) {
    std::string path = findHelp(cls);
    if (path.empty()) {
        std::string d = describeClass(cls);
        print(cls + ": " + (d.empty() ? "no help patch" : d + " (no help patch yet)"));
        return;
    }
    for (auto* w : helpWindows_)                        // already open: bring it forward
        if (w->patch && w->patch->file == path) { w->show(); return; }
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

void App::closeHelp(Patch* p) {
    host.removeExtra(p);   // its destruction closes and deletes the window
}

// ============================================================================
// MainWindow: one document
// ============================================================================

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

    patch.onRedraw = [this] { canvas->redraw(); };
    patch.onStatus = [this](const std::string& s) { setStatus(s); };
    patch.onPrint = [this](const std::string& s) { print(s); };
    patch.onStructure = [this] {
        refreshStatus();
        canvas->redraw();                 // an inner edit can change a box's ports
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
                    (dirty_ ? " — edited" : "") + " — Moritz";
    copy_label(t.c_str());
}

void MainWindow::markDirty(Patch*) {
    if (!dirty_) {
        dirty_ = true;
        updateTitle();
    }
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
    canvas->setLocked(false);
    updateTitle();
    if (!err.empty()) log(name + ":\n" + err);
    patch.loadbangAll();
    canvas->redraw();
    return true;
}

bool MainWindow::save() {
    canvas->commitEdit();
    Canvas* active = activeCanvas();
    if (active != canvas) {
        active->commitEdit();
        // inside an abstraction (or a p inside one): Save writes that file
        for (Patch* p = active->patch(); p; p = p->parent)
            if (p->isAbstraction && !p->file.empty()) {
                std::string err;
                if (!fileio::writeAtomic(p->file, p->serialize(), &err)) {   // the file stays as it was
                    alertUser("Can't save the abstraction %s:\n%s\n\nThe file on the disk is as it was.",
                              baseName(p->file).c_str(), err.c_str());
                    return false;
                }
                log("saved abstraction " + p->file);
                setStatus("saved abstraction " + baseName(p->file) +
                          " (other copies update when the patch is reopened)");
                return true;
            }
    }
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

void MainWindow::pushUndo(Patch*) {
    undo_.push_back(patch.serialize());
    if (undo_.size() > 500) undo_.erase(undo_.begin());
    redo_.clear();
}

void MainWindow::dropUndo() {
    if (!undo_.empty()) undo_.pop_back();
}

void MainWindow::restore(const std::string& snapshot) {
    std::string err;
    canvas->reset();
    auto open = openSubpatchPaths();
    patch.load(snapshot, err);
    reopenSubpatches(open);
    markDirty();
    canvas->redraw();
}

void MainWindow::undo() {
    canvas->commitEdit();
    activeCanvas()->commitEdit();
    if (undo_.empty()) { setStatus("nothing to undo"); return; }
    redo_.push_back(patch.serialize());
    std::string s = std::move(undo_.back());
    undo_.pop_back();
    restore(s);
    setStatus("undo");
}

void MainWindow::redo() {
    canvas->commitEdit();
    activeCanvas()->commitEdit();
    if (redo_.empty()) { setStatus("nothing to redo"); return; }
    undo_.push_back(patch.serialize());
    std::string s = std::move(redo_.back());
    redo_.pop_back();
    restore(s);
    setStatus("redo");
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

void MainWindow::reopenSubpatches(const std::vector<std::string>& paths) {
    for (auto& path : paths) {
        Patch* p = &patch;
        Box* b = nullptr;
        size_t start = 0;
        while (p && start <= path.size()) {
            size_t end = path.find('/', start);
            std::string id = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            b = p->find(id);
            p = b ? b->inner() : nullptr;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (p && b) openSubpatch(b);
    }
}

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
    p->onRedraw = [this] { canvas->redraw(); };
    p->onStructure = [this] { canvas->redraw(); };
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

// ---- dialogs

namespace {
bool gBatch = false;
int gBatchAnswer = 0;
}  // namespace

void setBatchMode(bool on) { gBatch = on; }
bool batchMode() { return gBatch; }
void setBatchAnswer(int a) { gBatchAnswer = a; }

int askUser(const char* fmt, const char* b0, const char* b1, const char* b2, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, b2);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (gBatch) {
        const char* b[3] = {b0, b1, b2};
        int a = std::clamp(gBatchAnswer, 0, 2);
        std::printf("ask: %s -> %s\n", text, b[a] ? b[a] : "?");
        std::fflush(stdout);
        return a;
    }
    return fl_choice("%s", b0, b1, b2, text);
}

void alertUser(const char* fmt, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (gBatch) {
        std::printf("alert: %s\n", text);
        std::fflush(stdout);
        return;
    }
    fl_alert("%s", text);
}

namespace {
// Shift+Cmd+J, from any window: a little homage to Wilhelm Busch
int eggHandler(int e) {
    if (e != FL_SHORTCUT) return 0;
    int s = Fl::event_state();
    if ((s & FL_COMMAND) && (s & FL_SHIFT) && Fl::event_key() == 'j') {
        App::get().showBanner();
        return 1;
    }
    return 0;
}

class Banner : public Fl_Double_Window {
public:
    Banner(Fl_Image* img) : Fl_Double_Window(480, 445), img_(img) {
        border(0);
        end();
        Fl::add_timeout(8.0, timeUp, this);
    }
    ~Banner() override {
        Fl::remove_timeout(timeUp, this);
        delete img_;
    }
    static void timeUp(void* d) { static_cast<Banner*>(d)->close(); }
    void draw() override {
        Fl_Color paper = fl_rgb_color(247, 241, 227), ink = fl_rgb_color(40, 34, 28);
        fl_color(paper);
        fl_rectf(0, 0, w(), h());
        fl_color(fl_rgb_color(150, 132, 104));
        fl_rect(6, 6, w() - 12, h() - 12);
        if (img_) img_->draw((w() - img_->w()) / 2, 22);
        int y = 22 + (img_ ? img_->h() : 340) + 14;
        fl_color(ink);
        fl_font(FL_TIMES_ITALIC, 18);
        fl_draw("Max und Moritz, Wilhelm Busch (1865)", 0, y, w(), 24, FL_ALIGN_CENTER);
        fl_color(fl_rgb_color(110, 96, 80));
        fl_font(FL_HELVETICA, 10);
        fl_draw("Moritz goes nowhere without Max  ·  click to close", 0, h() - 26, w(), 14, FL_ALIGN_CENTER);
    }
    int handle(int e) override {
        bool modifier = Fl::event_key() >= FL_Shift_L && Fl::event_key() <= FL_Meta_R;
        if (e == FL_PUSH || ((e == FL_KEYBOARD || e == FL_SHORTCUT) && !modifier)) { close(); return 1; }
        return Fl_Double_Window::handle(e);
    }
    void close() {
        if (closing_) return;
        closing_ = true;
        Fl::remove_timeout(timeUp, this);
        hide();
        Fl::delete_widget(this);
    }

private:
    Fl_Image* img_;
    bool closing_ = false;
};
}  // namespace

void App::showBanner() {
    std::string base = exeDir();
    Fl_Image* img = nullptr;
    for (std::string p : {base + "/../Resources/maxundmoritz.png", base + "/resources/maxundmoritz.png"}) {
        Fl_PNG_Image png(p.c_str());
        if (png.fail() == 0) { img = png.copy(440, 440 * png.h() / std::max(1, png.w())); break; }
    }
    auto* b = new Banner(img);
    centerOnScreen(b, frontDoc());
    b->show();
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

void App::about() {
    Fl_Double_Window dlg(360, 330, "About Moritz");
    centerOnScreen(&dlg, frontDoc());
    dlg.color(kBgLocked);
    Fl_Box icon(116, 18, 128, 128);
    std::string base = exeDir();
    Fl_PNG_Image* png = nullptr;
    for (std::string p : {base + "/../Resources/about.png", base + "/resources/about.png"}) {
        png = new Fl_PNG_Image(p.c_str());
        if (png->fail() == 0) break;
        delete png;
        png = nullptr;
    }
    Fl_Image* scaled = png ? png->copy(128, 128) : nullptr;
    if (scaled) icon.image(scaled);
    auto text = [](int y, int h, const char* s, int size, Fl_Font f, Fl_Color c) {
        auto* b = new Fl_Box(10, y, 340, h);
        b->copy_label(s);
        b->labelsize(size);
        b->labelfont(f);
        b->labelcolor(c);
        return b;
    };
    text(152, 30, "Moritz", 24, FL_HELVETICA_BOLD, kText);
    text(182, 18, ("version " MORITZ_VERSION), 12, FL_HELVETICA, kDim);
    text(206, 20, "A Max-like environment for sound and music computing", 12, FL_HELVETICA, kText);
    text(236, 20, "Created by Carmine-Emanuele Cella (2026)", 13, FL_HELVETICA_BOLD, kAccent);
    text(258, 18, "BSD 2-Clause License", 11, FL_HELVETICA, kDim);
    Fl_Return_Button ok(260, 288, 85, 28, "OK");
    ok.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.end();
    dlg.set_modal();
    dlg.show();
    while (dlg.shown()) Fl::wait();
    icon.image(nullptr);
    delete scaled;
    delete png;
}

void App::midiSetup() {
    midi.refresh();
    std::vector<std::string> ins = midi.inputPorts(), outs = midi.outputPorts();
    Fl_Double_Window dlg(440, 170, "MIDI Setup");
    centerOnScreen(&dlg, frontDoc());
    Fl_Choice inC(130, 15, 290, 25, "Input:");
    Fl_Choice outC(130, 50, 290, 25, "Output:");
    Fl_Box note(20, 82, 400, 40,
                "Other programs see Moritz's output as the MIDI port \"Moritz\" "
                "(macOS and Linux). Devices plugged in later are found automatically.");
    note.labelsize(11);
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button cancel(240, 130, 85, 28, "Cancel");
    Fl_Return_Button ok(335, 130, 85, 28, "OK");
    dlg.end();
    dlg.set_modal();

    inC.add("All inputs", 0, nullptr);
    for (auto& n : ins) inC.add(escapeMenu(n).c_str(), 0, nullptr);
    inC.value(0);
    for (size_t k = 0; k < ins.size(); ++k)
        if (ins[k] == midi.input()) inC.value((int)k + 1);
#ifdef _WIN32
    outC.add("None", 0, nullptr);
#else
    outC.add("Virtual port \"Moritz\"", 0, nullptr);
#endif
    for (auto& n : outs) outC.add(escapeMenu(n).c_str(), 0, nullptr);
    outC.value(0);
    for (size_t k = 0; k < outs.size(); ++k)
        if (outs[k] == midi.output()) outC.value((int)k + 1);

    int result = 0;
    ok.callback([](Fl_Widget* w, void* r) { *(int*)r = 1; w->window()->hide(); }, &result);
    cancel.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.show();
    while (dlg.shown()) Fl::wait();
    if (result != 1) return;
    midi.setInput(inC.value() > 0 ? ins[(size_t)inC.value() - 1] : "");
    midi.setOutput(outC.value() > 0 ? outs[(size_t)outC.value() - 1] : "");
    log(midi.describe());
    if (!midi.error.empty()) { log("MIDI: " + midi.error); midi.error.clear(); }
}

void App::audioSetup() {
    audio.refreshDevices();
    Fl_Double_Window dlg(440, 300, "Audio Setup");
    centerOnScreen(&dlg, frontDoc());
    Fl_Choice outC(130, 15, 290, 25, "Output device:");
    Fl_Choice inC(130, 50, 290, 25, "Input device:");
    Fl_Choice srC(130, 85, 120, 25, "Sample rate:");
    Fl_Choice bufC(130, 120, 120, 25, "Buffer size:");
    Fl_Choice ochC(130, 155, 120, 25, "Output channels:");
    Fl_Choice ichC(130, 190, 120, 25, "Input channels:");
    Fl_Box note(20, 218, 400, 34,
                "All: every channel of the device (dac~ 1 ... n, adc~ 1 ... n). Input opens only "
                "when a patch contains adc~.");
    note.labelsize(11);
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button cancel(240, 262, 85, 28, "Cancel");
    Fl_Return_Button ok(335, 262, 85, 28, "OK");
    dlg.end();
    dlg.set_modal();

    outC.add("Default", 0, nullptr);
    for (auto& d : audio.outputs) outC.add(escapeMenu(d.name).c_str(), 0, nullptr);
    outC.value(audio.settings.output + 1 < outC.size() ? audio.settings.output + 1 : 0);

    inC.add("None", 0, nullptr);
    inC.add("Default", 0, nullptr);
    for (auto& d : audio.inputs) inC.add(escapeMenu(d.name).c_str(), 0, nullptr);
    inC.value(audio.settings.input + 2 < inC.size() ? audio.settings.input + 2 : 1);

    const int rates[] = {0, 44100, 48000, 88200, 96000};
    for (int r : rates) srC.add(r ? std::to_string(r).c_str() : "Default", 0, nullptr);
    srC.value(0);
    for (int k = 0; k < 5; ++k)
        if (rates[k] == audio.settings.sampleRate) srC.value(k);

    const int bufs[] = {64, 128, 256, 512, 1024, 2048};
    for (int b : bufs) bufC.add(std::to_string(b).c_str(), 0, nullptr);
    bufC.value(2);
    for (int k = 0; k < 6; ++k)
        if (bufs[k] == audio.settings.bufferFrames) bufC.value(k);

    const int counts[] = {0, 1, 2, 4, 6, 8, 12, 16, 24, 32, 64};
    for (int c : counts) {
        ochC.add(c ? std::to_string(c).c_str() : "All", 0, nullptr);
        ichC.add(c ? std::to_string(c).c_str() : "All", 0, nullptr);
    }
    ochC.value(0);
    ichC.value(0);
    for (int k = 0; k < 11; ++k) {
        if (counts[k] == audio.settings.outChannels) ochC.value(k);
        if (counts[k] == audio.settings.inChannels) ichC.value(k);
    }

    int result = 0;
    ok.callback([](Fl_Widget* w, void* r) { *(int*)r = 1; w->window()->hide(); }, &result);
    cancel.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.show();
    while (dlg.shown()) Fl::wait();
    if (result != 1) return;

    audio.settings.output = outC.value() - 1;
    audio.settings.input = inC.value() - 2;
    audio.settings.sampleRate = rates[srC.value()];
    audio.settings.bufferFrames = bufs[bufC.value()];
    audio.settings.outChannels = counts[ochC.value()];
    audio.settings.inChannels = counts[ichC.value()];
    setAudio(true);
}

// ---- toolbar buttons

ToolButton::ToolButton(int x, int y, int w, int h, Icon icon, const char* label, bool toggle)
    : Fl_Button(x, y, w, h), icon_(icon) {
    type(toggle ? FL_TOGGLE_BUTTON : FL_NORMAL_BUTTON);
    copy_label(label);
    box(FL_NO_BOX);
    visible_focus(0);
}

void ToolButton::draw() {
    bool on = value() || lit;
    int X = x(), Y = y(), W = w(), H = h();
    Fl_Color fill = on ? (icon_ == Power ? kOn : icon_ == Record ? fl_rgb_color(170, 40, 40) : fl_rgb_color(52, 92, 160))
                       : kBoxFill;
    fl_color(fill);
    fl_rounded_rectf(X, Y, W, H, H / 2);
    fl_color(on ? fl_color_average(fill, FL_WHITE, 0.6f) : kBorder);
    fl_rounded_rect(X, Y, W, H, H / 2);
    // icon
    int cx = X + H / 2 + 4, cy = Y + H / 2, r = H / 2 - 6;
    fl_color(on ? FL_WHITE : kDim);
    fl_line_style(FL_SOLID | FL_CAP_ROUND, 2);
    if (icon_ == Power) {
        fl_arc(cx - r, cy - r, 2 * r, 2 * r, 120, 420);   // open ring with a gap at the top
        fl_line(cx, cy - r - 1, cx, cy);
    } else if (icon_ == Pencil) {
        fl_line(cx - r, cy + r, cx + r, cy - r);           // a pencil stroke
        fl_line(cx - r, cy + r, cx - r + 3, cy + r);
    } else if (icon_ == Code) {                            // </>
        int h = r - 1;
        fl_line(cx - 2, cy - h, cx - r - 1, cy, cx - 2, cy + h);
        fl_line(cx + 2, cy - h, cx + r + 1, cy, cx + 2, cy + h);
    } else if (icon_ == Minus || icon_ == Plus) {          // centred in a small round button
        int mx = X + W / 2, my = Y + H / 2, a = 5;
        fl_line(mx - a, my, mx + a, my);
        if (icon_ == Plus) fl_line(mx, my - a, mx, my + a);
    } else if (icon_ == Browser) {                         // a grid of four squares
        int q = r - 1;
        fl_line_style(FL_SOLID, 1);
        fl_rect(cx - q - 1, cy - q - 1, q, q);
        fl_rect(cx + 1, cy - q - 1, q, q);
        fl_rect(cx - q - 1, cy + 1, q, q);
        fl_rect(cx + 1, cy + 1, q, q);
    } else if (icon_ == Console) {                         // >_
        fl_line(cx - r, cy - r + 1, cx - 1, cy, cx - r, cy + r - 1);
        fl_line(cx + 1, cy + r - 1, cx + r + 1, cy + r - 1);
    } else if (icon_ == Record) {
        fl_color(on ? FL_WHITE : fl_rgb_color(220, 70, 70));
        fl_pie(cx - r + 1, cy - r + 1, 2 * r - 2, 2 * r - 2, 0, 360);
    }
    fl_line_style(0);
    if (alert) {                                           // a red dot at the top right
        fl_color(fl_rgb_color(235, 70, 70));
        fl_pie(X + W - 12, Y + 2, 8, 8, 0, 360);
    }
    fl_font(FL_HELVETICA_BOLD, 12);
    fl_color(on ? FL_WHITE : kText);
    if (icon_ == None) fl_draw(label(), X, Y, W, H, FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    else if (icon_ != Minus && icon_ != Plus) fl_draw(label(), X + H + 4, Y, W - H - 8, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

// ---- toolbar meters: CPU load and output level

ToolMeters::ToolMeters(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    tooltip("CPU: the share of each audio buffer spent computing the patches (red above 80%).\n"
            "Level: the peaks of every output channel; the light stays red for 2 s after a clip.");
}

ToolMeters::~ToolMeters() = default;

void ToolMeters::update(float cpu, const std::vector<float>& peaks) {
    cpu_ = cpu;
    if (level_.size() != peaks.size()) { level_.assign(peaks.size(), 0.f); hold_.assign(peaks.size(), 0.f); }
    for (size_t c = 0; c < peaks.size(); ++c) {
        level_[c] = std::max(peaks[c], level_[c] * 0.82f);          // a falling bar
        hold_[c] = std::max(peaks[c], hold_[c] * 0.97f);
        if (peaks[c] >= 0.999f)
            clipUntil_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + 2;
    }
    redraw();
}

void ToolMeters::draw() {
    int X = x(), Y = y(), W = w(), H = h();
    fl_color(kStatusBg);
    fl_rectf(X, Y, W, H);
    fl_font(FL_HELVETICA, 11);
    // CPU
    fl_color(kDim);
    fl_draw("CPU", X, Y, 28, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    int bx = X + 30, bw = 46, bh = 8, byy = Y + (H - bh) / 2;
    fl_color(kBoxFill);
    fl_rectf(bx, byy, bw, bh);
    float c = std::clamp(cpu_, 0.f, 1.f);
    fl_color(c > 0.8f ? fl_rgb_color(225, 70, 70) : c > 0.5f ? fl_rgb_color(225, 180, 60) : kAccent);
    fl_rectf(bx, byy, (int)(bw * c), bh);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", (int)std::lround(cpu_ * 100));
    fl_color(kText);
    fl_draw(buf, bx + bw + 4, Y, 34, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    // levels, dB from -60 to 0: up to two channels as thin rows, more as columns
    int lx = X + 118, lw = W - 118 - 12;
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto frac = [](float a) { double db = a > 1e-6f ? 20 * std::log10(a) : -120; return std::clamp((db + 60) / 60, 0.0, 1.0); };
    auto colour = [](double f) { return f > 0.9 ? fl_rgb_color(225, 180, 60) : fl_rgb_color(90, 190, 120); };
    int n = (int)level_.size();
    if (n <= 2) {
        for (int ch = 0; ch < 2; ++ch) {
            int ly = Y + H / 2 - 5 + ch * 6;
            fl_color(kBoxFill);
            fl_rectf(lx, ly, lw, 4);
            float lv = ch < n ? level_[(size_t)ch] : (n == 1 ? level_[0] : 0.f);
            float hv = ch < n ? hold_[(size_t)ch] : (n == 1 ? hold_[0] : 0.f);
            double f = frac(lv);
            fl_color(colour(f));
            fl_rectf(lx, ly, (int)(lw * f), 4);
            fl_color(kText);
            if (int hp = (int)(lw * frac(hv)); hp > 0) fl_yxline(lx + hp - 1, ly, ly + 3);
        }
    } else {
        int top = Y + 2, hh = H - 4, gap = n > 16 ? 1 : 2;
        int cw = std::max(1, (lw - gap * (n - 1)) / n);
        for (int ch = 0; ch < n; ++ch) {
            int cx = lx + ch * (cw + gap);
            fl_color(kBoxFill);
            fl_rectf(cx, top, cw, hh);
            double f = frac(level_[(size_t)ch]);
            int bhh = (int)(hh * f);
            fl_color(colour(f));
            fl_rectf(cx, top + hh - bhh, cw, bhh);
            fl_color(kText);
            if (int hp = (int)(hh * frac(hold_[(size_t)ch])); hp > 0) fl_xyline(cx, top + hh - hp, cx + cw - 1);
        }
    }
    fl_color(now < clipUntil_ ? fl_rgb_color(235, 60, 60) : kBoxFill);
    fl_pie(X + W - 9, Y + H / 2 - 4, 8, 8, 0, 360);
}

}  // namespace moritz
