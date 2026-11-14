// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// mainwindow.cpp — the main window (menus, toolbar, files, undo, audio
// setup, console) and the windows of subpatchers.

#include "gui.h"
#include "objects.h"

#include <FL/Fl_Choice.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/filename.H>

#include <chrono>
#include <cmath>
#include <fstream>

#ifdef __APPLE__
#include <FL/platform.H>
#include <mach-o/dyld.h>
#endif

#ifndef MINLSP_VERSION
#define MINLSP_VERSION "dev"
#endif

namespace mlsp {

namespace {
#ifdef __APPLE__
constexpr int kMenuH = 0;        // the macOS menu bar lives at the top of the screen
#else
constexpr int kMenuH = 25;
#endif
constexpr int kStatusH = 22;
constexpr int kToolH = 34;
constexpr const char* kExt = "mlsp";

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
            return s.substr(0, s.find_last_of('/'));
        }
    }
#endif
    return ".";
}

}  // namespace

std::string examplesDir() {
    std::string d = exeDir();
    for (std::string c : {d + "/../Resources/examples", d + "/examples"})
        if (fl_filename_isdir(c.c_str())) return c;
    return "";
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

struct NewItem {
    MainWindow* win;
    std::string cls;
};
}  // namespace

MainWindow::MainWindow(int w, int h)
    : Fl_Double_Window(w, h, "minlsp"), audio(patch) {
    menu_ = new Fl_Sys_Menu_Bar(0, 0, w, kMenuH);
    auto* bar = new Fl_Group(0, kMenuH, w, kToolH);
    bar->box(FL_FLAT_BOX);
    bar->color(kStatusBg);
    audioBtn_ = new ToolButton(8, kMenuH + 5, 104, kToolH - 10, ToolButton::Power, "Audio");
    audioBtn_->tooltip("Start or stop audio (Cmd+/)");
    audioBtn_->callback([](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->toggleAudio(); }, this);
    editBtn_ = new ToolButton(118, kMenuH + 5, 92, kToolH - 10, ToolButton::Pencil, "Edit");
    editBtn_->tooltip("Edit mode on/off (Cmd+E)");
    editBtn_->callback([](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->toggleLock(); }, this);
    auto* spacer = new Fl_Box(216, kMenuH, w - 216, kToolH);
    spacer->box(FL_NO_BOX);
    bar->resizable(spacer);
    bar->end();
    canvas = new Canvas(0, kMenuH + kToolH, w, h - kMenuH - kToolH - kStatusH, this, &patch);
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
    Patch::onDestroyed = [this](Patch* p) { closeSubpatchWindows(p); };
    patch.onStatus = [this](const std::string& s) { setStatus(s); };
    patch.onDsp = [this](bool on) { setAudio(on); };
    patch.onPrint = [this](const std::string& s) { print(s); };
    static const auto t0 = std::chrono::steady_clock::now();
    patch.clock = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    patch.onSchedule = [this] { armTimer(); };
    patch.onStructure = [this] {
        audio.updateInputNeed();
        refreshStatus();
        canvas->redraw();                 // an inner edit can change a box's ports
    };

    callback([](Fl_Widget*, void* d) {
        // Escape must not close the window; everything else (close box, Quit) does.
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
        static_cast<MainWindow*>(d)->quit();
    }, this);

    buildMenu();
    centerOnScreen(this);
    updateTitle();
    setAudio(true);
    log("minlsp " MINLSP_VERSION " - created by Carmine-Emanuele Cella (2026)");
    log("this console shows print objects, errors, audio and file events");
    setStatus("double-click to create an object · Cmd+E toggles edit mode");
}

MainWindow::~MainWindow() {
    audio.stop();
    Patch::onDestroyed = nullptr;
    for (auto* w : subwindows_) w->detach();
}

void MainWindow::buildMenu() {
#define MW_CB(method) [](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->method(); }
    menu_->add("File/New", FL_COMMAND + 'n', MW_CB(newPatch), this);
    menu_->add("File/Open...", FL_COMMAND + 'o', MW_CB(openPatch), this);
    menu_->add("File/Save", FL_COMMAND + 's', MW_CB(save), this);
    menu_->add("File/Close", FL_COMMAND + 'w', MW_CB(closeFront), this);
    menu_->add("File/Save as...", FL_COMMAND + FL_SHIFT + 's', MW_CB(saveAs), this,
               FL_MENU_DIVIDER);
    menu_->add("File/Quit", FL_COMMAND + 'q', MW_CB(quit), this);
    menu_->add("Edit/Undo", FL_COMMAND + 'z', MW_CB(undo), this);
    menu_->add("Edit/Redo", FL_COMMAND + FL_SHIFT + 'z', MW_CB(redo), this, FL_MENU_DIVIDER);
    menu_->add("Edit/Select All", FL_COMMAND + 'a',
               [](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->activeCanvas()->selectAll(); },
               this);

    static std::vector<std::unique_ptr<NewItem>> items;
    for (auto& c : classInfos()) {
        if (c.group.empty()) continue;                 // short aliases
        items.push_back(std::make_unique<NewItem>(NewItem{this, c.name}));
        std::string path = "Patch/New/" + c.group + "/" + escapeMenu(c.name);
        menu_->add(path.c_str(), 0,
                   [](Fl_Widget*, void* d) {
                       auto it = static_cast<NewItem*>(d);
                       it->win->activeCanvas()->newBoxAtMouse(it->cls);
                   },
                   items.back().get());
    }
    menu_->add("Patch/Edit Mode", FL_COMMAND + 'e', MW_CB(toggleLock), this, FL_MENU_TOGGLE);
    menu_->add("Patch/Audio On", FL_COMMAND + '/', MW_CB(toggleAudio), this,
               FL_MENU_TOGGLE | FL_MENU_DIVIDER);
    menu_->add("Patch/Audio Setup...", 0, MW_CB(audioSetup), this, FL_MENU_DIVIDER);
    menu_->add("Patch/Console", FL_COMMAND + FL_SHIFT + 'm', MW_CB(showConsole), this);
#ifdef __APPLE__
    fl_mac_set_about(MW_CB(about), this);          // minlsp > About minlsp
#else
    menu_->add("Help/About minlsp", 0, MW_CB(about), this);
#endif
#undef MW_CB
}

static void setMenuCheck(Fl_Sys_Menu_Bar* m, const char* path, bool on) {
    auto it = const_cast<Fl_Menu_Item*>(m->find_item(path));
    if (!it) return;
    if (on) it->set();
    else it->clear();
    m->update();
}

void MainWindow::refreshStatus() {
    std::string s = canvas->locked() ? "LOCKED" : "EDIT";
    if (audio.running())
        s += "   ·   audio " + std::to_string(audio.sampleRate()) + " Hz" +
             (audio.hasInput() ? " in/out" : " out");
    else
        s += "   ·   audio off";
    if (!patch.dspError.empty()) s += "   ·   " + patch.dspError;
    if (!message_.empty()) s += "   ·   " + message_;
    statusBar_->copy_label(("  " + s).c_str());
    setMenuCheck(menu_, "Patch/Edit Mode", !canvas->locked());
    setMenuCheck(menu_, "Patch/Audio On", audio.running());
    audioBtn_->value(audio.running());
    audioBtn_->copy_label(audio.running() ? "Audio On" : "Audio Off");
    editBtn_->value(!canvas->locked());
    editBtn_->copy_label(canvas->locked() ? "Locked" : "Edit");
    audioBtn_->redraw();
    editBtn_->redraw();
}

namespace {
void schedulerTimer(void* d) {
    auto* w = static_cast<MainWindow*>(d);
    w->patch.runDue(w->patch.time());           // re-arms through onSchedule
}
}  // namespace

void MainWindow::armTimer() {
    Fl::remove_timeout(schedulerTimer, this);
    double next = patch.nextDue();
    if (std::isfinite(next))
        Fl::add_timeout(std::max(0.0, next - patch.time()), schedulerTimer, this);
}

void MainWindow::showConsole() {
    if (!console_) {
        console_ = new Fl_Double_Window(560, 320, "minlsp Console");
        centerOnScreen(console_, this);
        console_->color(kStatusBg);
        consoleBuf_ = new Fl_Text_Buffer();
        consoleView_ = new Fl_Text_Display(0, 0, 560, 290);
        auto* clear = new Fl_Button(470, 294, 82, 22, "Clear");
        clear->labelsize(11);
        clear->callback([](Fl_Widget*, void* d) { static_cast<MainWindow*>(d)->clearConsole(); }, this);
        auto* hint = new Fl_Box(8, 294, 450, 22,
                                "print objects, errors, audio and file events");
        hint->labelsize(11);
        hint->labelcolor(kDim);
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        consoleView_->buffer(consoleBuf_);
        consoleView_->box(FL_FLAT_BOX);
        consoleView_->color(fl_rgb_color(20, 21, 24));
        consoleView_->textcolor(kText);
        consoleView_->textfont(FL_COURIER);
        consoleView_->textsize(12);
        console_->resizable(consoleView_);
        console_->end();
        for (auto& line : consoleLines_) consoleBuf_->append((line + "\n").c_str());
    }
    console_->show();
    consoleView_->scroll(consoleBuf_->count_lines(0, consoleBuf_->length()), 0);
}

void MainWindow::clearConsole() {
    consoleLines_.clear();
    if (consoleBuf_) consoleBuf_->text("");
}

void MainWindow::print(const std::string& s) {
    log(s);
    setStatus(s);
}

void MainWindow::log(const std::string& s) {
    consoleLines_.push_back(s);
    if (consoleLines_.size() > 2000) consoleLines_.erase(consoleLines_.begin());
    if (consoleBuf_) {
        consoleBuf_->append((s + "\n").c_str());
        if (consoleBuf_->length() > 200000) consoleBuf_->remove(0, consoleBuf_->length() - 150000);
        consoleView_->scroll(consoleBuf_->count_lines(0, consoleBuf_->length()), 0);
    }
}

void MainWindow::setStatus(const std::string& s) {
    message_ = s;
    refreshStatus();
}

void MainWindow::updateTitle() {
    std::string t = (path_.empty() ? std::string("Untitled") : baseName(path_)) +
                    (dirty_ ? " — edited" : "") + " — minlsp";
    copy_label(t.c_str());
}

void MainWindow::markDirty() {
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
    return Fl_Double_Window::handle(e);
}

// ---- File

bool MainWindow::confirmDiscard() {
    canvas->commitEdit();
    if (!dirty_) return true;
    std::string name = path_.empty() ? "Untitled" : baseName(path_);
    int r = fl_choice("Save changes to \"%s\"?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return save();
    return true;
}

void MainWindow::closeFront() {
    Fl_Window* w = Fl::first_window();
    if (auto pw = dynamic_cast<PatchWindow*>(w)) {
        pw->canvas->commitEdit();
        pw->hide();
        return;
    }
    if (w && w != this && w == console_) {
        w->hide();
        return;
    }
    if (!confirmDiscard()) return;
    dirty_ = false;                       // already asked
    newPatch();
    setStatus("closed");
}

void MainWindow::about() {
    Fl_Double_Window dlg(360, 330, "About minlsp");
    centerOnScreen(&dlg, this);
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
    text(152, 30, "minlsp", 24, FL_HELVETICA_BOLD, kText);
    text(182, 18, ("version " MINLSP_VERSION), 12, FL_HELVETICA, kDim);
    text(206, 20, "A minimal Max/MSP-style patcher", 13, FL_HELVETICA, kText);
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

void MainWindow::newPatch() {
    if (!confirmDiscard()) return;
    canvas->reset();
    patch.clear();
    path_.clear();
    patch.dir.clear();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    canvas->setLocked(false);
    updateTitle();
    setStatus("new patch");
}

void MainWindow::openPatch() {
    if (!confirmDiscard()) return;
    Fl_Native_File_Chooser fc;
    fc.title("Open Patch");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("minlsp patch\t*.mlsp\nAll files\t*");
    std::string dir = path_.empty() ? examplesDir() : path_.substr(0, path_.find_last_of('/'));
    if (!dir.empty()) fc.directory(dir.c_str());
    if (fc.show() != 0) return;
    dirty_ = false;          // already confirmed
    openFile(fc.filename());
}

bool MainWindow::openFile(const std::string& path) {
    std::string src, err;
    if (!readFile(path, src)) {
        fl_alert("Can't read %s", path.c_str());
        return false;
    }
    canvas->reset();
    std::string oldDir = patch.dir;
    patch.dir = path.substr(0, path.find_last_of('/'));
    if (!patch.load(src, err)) {
        patch.dir = oldDir;
        fl_alert("Can't open %s:\n%s", baseName(path).c_str(), err.c_str());
        return false;
    }
    path_ = path;
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    canvas->setLocked(true);           // Max opens patches locked
    updateTitle();
    log("opened " + path);
    if (!err.empty()) print(baseName(path) + ":\n" + err);
    else setStatus("opened " + baseName(path));
    if (!patch.dspError.empty()) log(baseName(path) + ": " + patch.dspError);
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
                std::ofstream f(p->file, std::ios::binary);
                if (!(f << p->serialize())) {
                    fl_alert("Can't write %s", p->file.c_str());
                    return false;
                }
                log("saved abstraction " + p->file);
                setStatus("saved abstraction " + baseName(p->file) +
                          " (other copies update when the patch is reopened)");
                return true;
            }
    }
    if (path_.empty()) return saveAs();
    std::ofstream f(path_, std::ios::binary);
    if (!(f << patch.serialize())) {
        fl_alert("Can't write %s", path_.c_str());
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
    fc.filter("minlsp patch\t*.mlsp");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    fc.preset_file(path_.empty() ? "Untitled.mlsp" : baseName(path_).c_str());
    if (fc.show() != 0) return false;
    std::string p = fc.filename();
    const char* ext = fl_filename_ext(p.c_str());
    if (!ext || !*ext) p += std::string(".") + kExt;
    path_ = p;
    patch.dir = p.substr(0, p.find_last_of('/'));
    return save();
}

void MainWindow::quit() {
    if (!confirmDiscard()) return;
    audio.stop();
    hide();
}

// ---- Edit

void MainWindow::pushUndo() {
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

void MainWindow::toggleAudio() { setAudio(!audio.running()); }

void MainWindow::setAudio(bool on) {
    std::string err;
    if (on) {
        if (!audio.start(err)) { print("audio: " + err); return; }
        log("audio on: " + audio.deviceName() + ", " + std::to_string(audio.sampleRate()) +
            " Hz, " + std::to_string(audio.bufferFrames()) + "-sample buffer, " +
            (audio.hasInput() ? "input and output" : "output only"));
        if (!err.empty()) log("audio: " + err);
        setStatus(err);              // the status bar already says "audio on"
    } else {
        if (audio.running()) log("audio off");
        audio.stop();
        setStatus("");
    }
}

void MainWindow::audioSetup() {
    audio.refreshDevices();
    Fl_Double_Window dlg(440, 230, "Audio Setup");
    centerOnScreen(&dlg, this);
    Fl_Choice outC(130, 15, 290, 25, "Output device:");
    Fl_Choice inC(130, 50, 290, 25, "Input device:");
    Fl_Choice srC(130, 85, 120, 25, "Sample rate:");
    Fl_Choice bufC(130, 120, 120, 25, "Buffer size:");
    Fl_Box note(20, 150, 400, 30,
                "Input opens only when the patch contains adc~.");
    note.labelsize(11);
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button cancel(240, 190, 85, 28, "Cancel");
    Fl_Return_Button ok(335, 190, 85, 28, "OK");
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
    setAudio(true);
}

// ---- subpatcher windows

namespace {
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
}  // namespace

Canvas* MainWindow::activeCanvas() {
    for (Fl_Window* w = Fl::first_window(); w; w = Fl::next_window(w)) {
        if (w == this) return canvas;
        if (auto pw = dynamic_cast<PatchWindow*>(w)) return pw->canvas;
        if (w->modal() || w == console_) continue;
    }
    return canvas;
}

void MainWindow::openSubpatch(Box* b) {
    Patch* p = b ? b->inner() : nullptr;
    if (!p) return;
    for (auto* w : subwindows_)
        if (w->patch == p) { w->show(); return; }
    int W = 420, H = 300;
    fl_font(kFont, kFontSize);
    for (auto& x : p->boxes) {
        int bw = std::max(x->w, (int)fl_width(x->label().c_str()) + 24);
        W = std::max(W, x->x + bw + 40);
        H = std::max(H, x->y + x->h + 50);
    }
    auto* w = new PatchWindow(this, p, patchTitle(p));
    w->size(std::min(W, 1400), std::min(H + 22, 900));
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

PatchWindow::PatchWindow(MainWindow* main, Patch* p, const std::string& title)
    : Fl_Double_Window(560, 420), patch(p) {
    copy_label(title.c_str());
    canvas = new Canvas(0, 0, 560, 420 - kStatusH, main, p);
    auto* status = new Fl_Box(0, 420 - kStatusH, 560, kStatusH);
    status->box(FL_FLAT_BOX);
    status->color(kStatusBg);
    status->labelcolor(kDim);
    status->labelsize(11);
    status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    status->copy_label(p->isAbstraction
                           ? "  abstraction · Cmd+S saves its file · Cmd+E toggles edit mode"
                           : "  subpatcher · saved with the main patch · Cmd+E toggles edit mode");
    end();
    resizable(canvas);
    size_range(300, 200);
    canvas->setLocked(true);
    p->onRedraw = [this] { canvas->redraw(); };
    p->onStructure = [this] { canvas->redraw(); };
    callback([](Fl_Widget* w, void* d) {
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
        auto* pw = static_cast<PatchWindow*>(w);
        pw->canvas->commitEdit();
        pw->hide();                       // kept: reopening is instant
        (void)d;
    }, main);
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

// ---- toolbar buttons

ToolButton::ToolButton(int x, int y, int w, int h, Icon icon, const char* label)
    : Fl_Button(x, y, w, h), icon_(icon) {
    type(FL_TOGGLE_BUTTON);
    copy_label(label);
    box(FL_NO_BOX);
    visible_focus(0);
}

void ToolButton::draw() {
    bool on = value();
    int X = x(), Y = y(), W = w(), H = h();
    Fl_Color fill = on ? (icon_ == Power ? kOn : fl_rgb_color(52, 92, 160)) : kBoxFill;
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
    } else {
        fl_line(cx - r, cy + r, cx + r, cy - r);           // a pencil stroke
        fl_line(cx - r, cy + r, cx - r + 3, cy + r);
    }
    fl_line_style(0);
    fl_font(FL_HELVETICA_BOLD, 12);
    fl_color(on ? FL_WHITE : kText);
    fl_draw(label(), X + H + 4, Y, W - H - 8, H, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
}

}  // namespace mlsp
