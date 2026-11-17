// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// gui.h — FLTK editor: canvas, main window, subpatcher windows
#pragma once

#include <cmath>
#include <functional>

#include "jobs.h"
#include "midi.h"
#include "moritz.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Preferences.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Sys_Menu_Bar.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>

namespace moritz {

class MainWindow;
class CodeWindow;

// A folder bundled with the app ("examples", "library", "help"): inside the
// .app's Resources, or next to the executable when run from the source tree.
std::string resourceDir(const std::string& name);
inline std::string examplesDir() { return resourceDir("examples"); }
// Fills libraryPath(), abstractionPath() and helpPath(): the user's library
// (~/Documents/Moritz/library) first, then the bundled library and examples.
void setupSearchPaths();


class Canvas : public Fl_Group {
public:
    Canvas(int x, int y, int w, int h, MainWindow* win, Patch* patch);
    Patch* patch() const { return patch_; }

    void draw() override;
    int handle(int e) override;

    void setLocked(bool l);
    bool locked() const { return locked_; }
    void newBoxAt(const std::string& cls, int x, int y);   // Patch > New
    void newBoxAtMouse(const std::string& cls);
    void deleteSelection();
    void selectAll();
    void commitEdit();
    void cancelEdit();
    void reset();          // forget selection and edit state (after load/undo)
    void duplicateSelection(int dx, int dy);   // option-drag, Cmd+D: boxes and their cables
    void copySelection();        // to the system clipboard, as .moritz text
    void cutSelection();
    void paste();                // at the mouse; works between windows and documents
    void pasteText(const std::string& text);

    // object-name completion while typing in a new/edited object box
    void updateSuggestions();
    bool hasSuggestions() const { return !sugg_.empty(); }
    void moveSuggestion(int delta);
    bool acceptSuggestion(bool commit);
    void clearSuggestions();
    void fitEditor();      // size the text field to what is being typed
    bool readOnly = false;   // help patches: always locked
    // scrolling: the view's top-left corner in patch coordinates
    int scrollX() const { return sx_; }
    int scrollY() const { return sy_; }
    void scrollTo(int sx, int sy);
    bool scrollable = true;  // false for --render snapshots, which show the whole patch
    // zoom: the patch drawn larger or smaller (1 = 100%); the view, not the file
    double zoom() const { return zoom_; }
    void setZoom(double z);
    void zoomBy(int steps);  // one step of the zoom ladder up or down; 0: back to 100%
    std::function<void()> onZoom;

private:
    struct Hit {
        enum What { None, Body, Inlet, Outlet, Cable } what = None;
        Box* box = nullptr;
        int port = -1;
        int cable = -1;
    };
    Hit hitTest(int mx, int my) const;
    int inletAt(Box* b, int mx) const;
    void portPos(Box* b, bool out, int i, int& px, int& py) const;
    void layoutBox(Box* b) const;
    std::string bufferOf(Box* b) const;
    void drawBox(Box* b);
    void beginEdit(Box* b, bool isNew);
    void clearSelection();
    std::vector<Box*> selectedBoxes() const;
    void numberDrag(int dy);
    static void editCb(Fl_Widget* w, void* data);

    MainWindow* doc() const;   // the document (for help patches: the front one)
    // the patch's origin in unzoomed screen units: drawing goes through the
    // zoom transform (zoom.h) centred on the canvas's corner
    int ox() const { return x() - sx_; }
    int oy() const { return y() - sy_; }
    // a mouse position (window coordinates) in patch coordinates
    int patchX(int ex) const { return (int)std::floor((ex - x()) / zoom_) + sx_; }
    int patchY(int ey) const { return (int)std::floor((ey - y()) / zoom_) + sy_; }
    // a patch position in window coordinates
    int screenX(int px) const { return x() + (int)std::lround((px - sx_) * zoom_); }
    int screenY(int py) const { return y() + (int)std::lround((py - sy_) * zoom_); }
    int viewW() const { return (int)std::ceil(w() / zoom_); }   // the view, in patch units
    int viewH() const { return (int)std::ceil(h() / zoom_); }
    double zoom_ = 1;
    void contentBounds(int& x0, int& y0, int& x1, int& y1) const;
    void updateScrollbars();
    static constexpr int kScrollW = 12;
    int sx_ = 0, sy_ = 0;
    Fl_Scrollbar* hbar_;
    Fl_Scrollbar* vbar_;
    MainWindow* win_;          // nullptr in help windows and --render snapshots
    Patch* patch_;
    bool locked_ = false;
    Fl_Input* editor_;
    Box* editing_ = nullptr;
    bool editingNew_ = false;

    enum class Drag { None, Move, Cable, Rubber, Number, UI } drag_ = Drag::None;
    int pressX_ = 0, pressY_ = 0, curX_ = 0, curY_ = 0;
    bool moved_ = false;
    Box* dragBox_ = nullptr;
    std::string uiBefore_;          // what an interface box saved before a mouse edit
    int dragPort_ = 0;
    double dragStartValue_ = 0.0, dragStep_ = 1.0;
    Box* typingBox_ = nullptr;     // number box receiving typed digits
    std::string typed_;
    int mouseX_ = 60, mouseY_ = 60;

    // port help: shown after the mouse rests on an inlet/outlet in edit mode
    Box* tipBox_ = nullptr;
    bool tipOut_ = false;
    int tipPort_ = -1;
    bool tipShown_ = false;
    void updatePortTip(int mx, int my);
    void hidePortTip();
    void drawPortTip();
    static void tipTimer(void* data);

    std::vector<std::string> sugg_;
    int suggSel_ = 0;
    int suggX_ = 0, suggY_ = 0, suggW_ = 0;   // popup geometry (window coords)
    void drawSuggestions();
    int suggestionAt(int ex, int ey) const;
};

// A window showing a subpatcher, abstraction or pfft~ patch, or a help patch.
class PatchWindow : public Fl_Double_Window {
public:
    // doc: the document the patch belongs to (nullptr for help patches)
    PatchWindow(MainWindow* doc, Patch* p, const std::string& title, bool help = false);
    ~PatchWindow() override;
    int handle(int e) override;
    void detach();          // forget the patch (it is being destroyed)
    Canvas* canvas = nullptr;
    Patch* patch;
    MainWindow* doc;
    bool help;
    std::string helpClass;     // help windows: the object they are the help of
};

// A patch as text, in its own window: a second view on the same patch,
// kept in sync both ways (see textview.cpp).
class PatchTextWindow : public Fl_Double_Window {
public:
    PatchTextWindow(MainWindow* doc, Patch* p, const std::string& title, bool readOnly);
    ~PatchTextWindow() override;
    void detach();                 // the patch is being destroyed
    void applyNow();               // apply a pending text edit
    void refresh(bool force);      // show the patch's current text
    Fl_Text_Editor* editor() const { return editor_; }
    Patch* patch;

private:
    static void modified(int pos, int nIns, int nDel, int nRestyled, const char* deleted, void* d);
    static void applyLater(void* d);
    static void poll(void* d);
    void setStatus(const std::string& s, bool error = false);
    MainWindow* doc_;
    bool readOnly_;
    Fl_Text_Buffer* text_;
    Fl_Text_Buffer* style_;
    Fl_Text_Editor* editor_;
    Fl_Box* status_;
    std::string shown_;            // the text last in sync with the patch
    bool pending_ = false;         // edited, not applied yet
    bool syncing_ = false;
};

// Round toolbar button (audio on/off, edit mode, code view).
class ToolButton : public Fl_Button {
public:
    enum Icon { Power, Pencil, Code, Minus, Plus, None, Browser, Console, Record };
    // toggle: keeps its state (audio, edit); otherwise a push button
    ToolButton(int x, int y, int w, int h, Icon icon, const char* label, bool toggle = true);
    void draw() override;
    bool alert = false;          // a red dot (the console has errors)
    bool lit = false;            // drawn on without being a toggle (the record button)

private:
    Icon icon_;
};

// The toolbar's meters: CPU load of the audio thread and the output's level.
class ToolMeters : public Fl_Widget {
public:
    ToolMeters(int x, int y, int w, int h);
    ~ToolMeters() override;
    void draw() override;
    void update(float cpu, const std::vector<float>& peaks);   // a peak per output channel

private:
    float cpu_ = 0;
    std::vector<float> level_, hold_;
    double clipUntil_ = 0;
};

Fl_Preferences& appPrefs();      // the user's settings (browser.cpp)

// One per program: the audio and MIDI engines, the console, help windows
// and the open documents. Every open patch (and help patch) is an extra of
// `host`, so they all play through the same audio output and share the
// scheduler and the send/receive and buffer~ names, as in Max.
class App {
public:
    static App& get();
    Patch host;
    AudioEngine audio{host};
    MidiEngine midi;
    std::vector<MainWindow*> docs;
    bool quitting = false;
    bool startAudio = true;     // --no-audio: start with audio off

    void init();                                    // after FLTK is set up
    MainWindow* newDocument();                      // File > New: an empty window
    MainWindow* openDocument(const std::string& path);   // reuses an empty Untitled window
    void openDialog();                              // File > Open...
    void importDialog();                            // File > Import from Max...
    MainWindow* importMax(const std::string& path);
    void closeDocument(MainWindow* w);              // already confirmed
    void closeFront();                              // Cmd+W
    void quit();                                    // asks about unsaved documents
    MainWindow* frontDoc();                         // document of the front patch window
    Canvas* activeCanvas();                         // canvas of the front patch window

    void setAudio(bool on);
    void toggleAudio() { setAudio(!audio.running()); }
    void audioSetup();
    void midiSetup();
    void about();
    // the browser (browser.cpp): tutorials, examples, objects, recent files
    void showBrowser();
    void hideBrowser();
    bool browserAtStartup();
    void setBrowserAtStartup(bool on);
    std::vector<std::string> recentFiles();
    void addRecent(const std::string& path);
    void showBanner();                  // Shift+Cmd+J: Max und Moritz
    void showConsole();
    void ensureConsole();               // shows the console if it is hidden, without raising it
    // the code editors (code.cpp): Musil scripts (.mu)
    CodeWindow* newCode();
    CodeWindow* openCode(const std::string& path);
    void closeCode(CodeWindow* w);      // already confirmed
    CodeWindow* frontCode();            // the code window in front, if one is
    const std::vector<CodeWindow*>& codeWindows() const { return codeWindows_; }
    Fl_Double_Window* consoleWindow() { return console_; }
    void clearConsole();
    bool consoleAlert = false;          // an error since the console was last shown
    // the quick recorder (Extras): what goes to the dac, into a WAV file
    void toggleRecording();
    void showRecorder();
    std::string recordingsFolder();
    void chooseRecordingsFolder();
    void refreshTools();                // toolbar meters, record button, recorder window
    void zoomFront(int steps);          // View > Zoom: +1, -1, 0 (100%)
    void showMonitor();                 // Extras > Binaural Monitor: speakers on headphones
    void applyMonitor();                // after a change of its settings, or of the audio
    void showTester();                  // Extras > Channel Tester: noise on one channel at a time
    void refreshTester();
    void scaleInterface(float factor);  // View > Interface: every window larger or smaller
    void print(const std::string& s);   // console, and the front document's status bar
    void log(const std::string& s);     // console only
    void refreshStatus();               // every document
    void armTimer();                    // schedule the next patch timer event

    void showText();                         // Patch > Code View: the front patch as text
    void showText(MainWindow* doc, Patch* p, const std::string& title, bool readOnly);
    void closeText(Patch* p);                // hide its text window, if any
    PatchTextWindow* textWindow(Patch* p);
    void openHelp(const std::string& cls);   // the object's help patch, or its description
    void showReference(const std::string& cls);   // the object's reference, in its window
    void closeHelp(Patch* p);
    void buildMenu(Fl_Menu_Bar* m);
    Fl_Menu_Bar* menuFor(MainWindow* w);     // the bar showing w's menu state

private:
    App() = default;
    std::vector<PatchWindow*> helpWindows_;
    std::vector<PatchTextWindow*> textWindows_;
    std::vector<CodeWindow*> codeWindows_;
    Fl_Menu_Bar* sysMenu_ = nullptr;          // macOS: one menu bar for all windows
    Fl_Double_Window* console_ = nullptr;
    Fl_Text_Display* consoleView_ = nullptr;
    Fl_Text_Buffer* consoleBuf_ = nullptr;
    std::vector<std::string> consoleLines_;   // kept until the console opens
    Fl_Double_Window* referenceWin_ = nullptr;
    Fl_Text_Display* referenceView_ = nullptr;
    Fl_Text_Buffer* referenceBuf_ = nullptr;
    Fl_Text_Buffer* referenceStyle_ = nullptr;
    Fl_Double_Window* recorderWin_ = nullptr;
    Fl_Double_Window* monitorWin_ = nullptr;
    Fl_Double_Window* testerWin_ = nullptr;
    Fl_Group* testerGrid_ = nullptr;
    Fl_Box* testerInfo_ = nullptr;
    struct MonitorSettings {
        bool on = false;
        int layout = 0;          // rings (see showMonitor), 3: custom
        int speakers = 0;        // 0: the patches' output channels
        std::string custom;      // "az az az" or "az/el ..."
    } monitorSet_;
    int testerSteps_ = 0;        // stepping through the channels (0: off)
    static void testerTimer(void* data);
    Fl_Box* recorderTime_ = nullptr;
    Fl_Box* recorderFile_ = nullptr;
    ToolButton* recorderBtn_ = nullptr;
    std::string recordingsDir_;
    static void toolTimer(void* data);
    friend class PatchWindow;
};

// A document: one patch file in its own window, with its subpatcher windows,
// undo history and status bar.
class MainWindow : public Fl_Double_Window {
public:
    MainWindow(int w, int h);
    ~MainWindow() override;

    Patch& patch;           // an extra of App::host, removed when the window closes

    // File
    bool openFile(const std::string& path);   // into this window
    // a patch converted from elsewhere: Untitled, edited, saved as name.moritz in dir
    bool loadConverted(const std::string& src, const std::string& dir, const std::string& name);
    bool save();
    bool saveAs();
    bool confirmDiscard();
    bool isEmptyUntitled() const { return path_.empty() && !dirty_ && patch.boxes.empty(); }
    const std::string& path() const { return path_; }
    // Edit
    void pushUndo();
    void dropUndo();     // discard the last pushUndo() when nothing changed
    void undo();
    void redo();
    void markDirty();
    // Patch
    void toggleLock();

    // subpatcher windows
    void openSubpatch(Box* b);
    void openHelp(const std::string& cls) { App::get().openHelp(cls); }
    void closeSubpatchWindows(Patch* p);    // p is being destroyed
    void closeAllSubwindows();
    void forgetWindow(PatchWindow* w);
    Canvas* activeCanvas();                 // this document's front canvas

    void setStatus(const std::string& s);
    void refreshStatus();
    void print(const std::string& s);   // append to the console and the status bar
    void log(const std::string& s) { App::get().log(s); }
    void updateTitle();
    int handle(int e) override;

    Canvas* canvas = nullptr;

private:
    void restore(const std::string& snapshot);

    Fl_Menu_Bar* menu_ = nullptr;   // Windows and Linux: each window has its menu
    friend class App;
    Fl_Box* statusBar_;
    ToolButton* audioBtn_;
    ToolButton* editBtn_;
    ToolButton* textBtn_;
    ToolButton* zoomLabel_;
    ToolButton* consoleBtn_;
    ToolButton* recordBtn_;
    ToolMeters* meters_;
    void updateZoomLabel();
    std::vector<PatchWindow*> subwindows_;
    std::vector<std::string> openSubpatchPaths() const;
    void reopenSubpatches(const std::vector<std::string>& paths);
    std::string path_;
    std::string suggested_;          // Save As proposes this name (imported patches)
    bool dirty_ = false;
    std::vector<std::string> undo_, redo_;
    std::string message_;
};

}  // namespace moritz
