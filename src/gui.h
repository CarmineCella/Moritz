// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// gui.h — FLTK editor: canvas, document windows, subpatcher windows, the application
//
//   Document (document.h)        a patch file: the patch, its file, what is unsaved, undo; no interface
//   PatchEditor (editor.h)       what a canvas asks of its document
//   Canvas (canvas.cpp)          draws a patch, turns the mouse and keys into edits through its editor
//   DocumentWindow               a document's window: toolbar, canvas, status bar (document_window.cpp)
//   PatchWindow                  a subpatcher or abstraction (patch_window.cpp); a help patch is a document
//   App                          one per program: audio, MIDI, the windows (app.cpp, menus.cpp ...)
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

#include "document.h"
#include "editor.h"
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

class Fl_Native_File_Chooser;

namespace moritz {

class DocumentWindow;
class CodeWindow;
class CodeSource;
class SoundWindow;

// A folder bundled with the app ("examples", "library", "help"): inside the
// .app's Resources, or next to the executable when run from the source tree.
std::string resourceDir(const std::string& name);
inline std::string examplesDir() { return resourceDir("examples"); }
// Fills libraryPath(), abstractionPath() and helpPath(): the user's library
// (~/Documents/Moritz/library) first, then the bundled library and examples.
void setupSearchPaths();

// A box's size on the canvas, from its text in the patch font (unzoomed units, measured at the zoom
// being drawn). Works without a window: a snapshot or a new window's size measures with it too.
void layoutBox(Box* b);
void layoutPatch(Patch* p);

// Questions and alerts that wait for the user (fl_choice, fl_alert). In batch mode (--window-png, the
// tests) nothing waits: the text is printed, and askUser answers batchAnswer (answer=<n>, default 0).
void setBatchMode(bool on);
bool batchMode();
void setBatchAnswer(int a);
int askUser(const char* fmt, const char* b0, const char* b1, const char* b2, ...);
void alertUser(const char* fmt, ...);
// A Save As chooser shown: the file chosen (portable path), "" when cancelled. In batch mode nothing is
// shown: what it proposes is printed, and the answer is saveas=<path> (none: cancelled).
std::string showSaveChooser(Fl_Native_File_Chooser& fc);
void setBatchSavePath(const std::string& path);
// In batch mode copy and paste go through this, not the system's clipboard: tests leave the user's alone
std::string& batchClipboard();
// --window-png ... act=...: the actions of a test, in order (guitest.cpp); the number that failed
int runTestActions(int argc, char** argv);

// The name of the command key in texts: "Cmd" on macOS, "Ctrl" elsewhere; and a shortcut as text
// ("Shift+Cmd+B" on macOS, "Ctrl+Shift+B" elsewhere)
const char* commandKeyName();
std::string shortcutText(int shortcut);

class Canvas : public Fl_Group {
public:
    // editor: what edits go through (nullptr: a detached canvas, as --render's snapshots)
    Canvas(int x, int y, int w, int h, PatchEditor* editor, Patch* patch);
    ~Canvas() override;
    Patch* patch() const { return patch_; }
    // the patch is being destroyed: the canvas forgets it and stops its timers (the window may be
    // deleted later, by FLTK; until then it draws nothing and takes no events)
    void detach();
    PatchEditor* editor() const { return ed_; }
    bool readOnly() const { return !ed_->editable(); }   // always locked, nothing edited

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
    // a patch position in window coordinates (tests click there)
    int screenX(int px) const { return x() + (int)std::lround((px - sx_) * zoom_); }
    int screenY(int py) const { return y() + (int)std::lround((py - sy_) * zoom_); }
    // the box a pointer the canvas kept still names (nullptr when it was deleted or replaced)
    Box* alive(Box* b) const;
    // a box dragged with Shift held: the index of the cable it would go into, drawn lit (-1: none)
    int litCable() const { return drag_ == Drag::Move ? spliceCable_ : -1; }

private:
    struct Hit {
        enum What { None, Body, Inlet, Outlet, Cable } what = None;
        Box* box = nullptr;
        int port = -1;
        int cable = -1;
    };
    Hit hitTest(int mx, int my) const;
    int inletAt(Box* b, int mx) const;
    int outletAt(Box* b, int mx) const;
    // a cable's ends, where it leaves its outlet and enters its inlet
    void cableEnds(const Connection& c, int& ax, int& ay, int& bx, int& by) const;
    void moveCableEnd(int mx, int my);   // a cable's end dropped there (Drag::CableEnd)
    // the cable box b would go into if dropped with Shift held: one that crosses it and can take it (-1: none)
    int cableUnder(Box* b) const;
    static constexpr int kCableEndGrab = 18;   // how near an end a click on a cable takes that end
    void portPos(Box* b, bool out, int i, int& px, int& py) const;
    void layout(Box* b);           // layoutBox, through the cache
    std::string bufferOf(Box* b) const;
    void drawBox(Box* b);
    void beginEdit(Box* b, bool isNew);
    void clearSelection();
    std::vector<Box*> selectedBoxes() const;
    void numberDrag(int dy);
    void beginMove();              // the selected boxes are about to be dragged: where each one starts
    static void editCb(Fl_Widget* w, void* data);
    static void flashTimer(void* data);
    void damageBox(Box* b);        // redraw only that box's area

    // the patch's origin in unzoomed screen units: drawing goes through the
    // zoom transform (zoom.h) centred on the canvas's corner
    int ox() const { return x() - sx_; }
    int oy() const { return y() - sy_; }
    // a mouse position (window coordinates) in patch coordinates
    int patchX(int ex) const { return (int)std::floor((ex - x()) / zoom_) + sx_; }
    int patchY(int ey) const { return (int)std::floor((ey - y()) / zoom_) + sy_; }
    int viewW() const { return (int)std::ceil(w() / zoom_); }   // the view, in patch units
    int viewH() const { return (int)std::ceil(h() / zoom_); }
    double zoom_ = 1;
    void contentBounds(int& x0, int& y0, int& x1, int& y1) const;
    void updateScrollbars();
    static constexpr int kScrollW = 12;
    int sx_ = 0, sy_ = 0;
    Fl_Scrollbar* hbar_;
    Fl_Scrollbar* vbar_;
    PatchEditor* ed_;          // never null
    Patch* patch_;
    bool locked_ = false;
    Fl_Input* editor_;
    // Pointers to boxes the canvas keeps between events: an edit elsewhere (the code view, undo, a
    // message) can delete or replace the box, so each is checked with alive() before use.
    Box* editing_ = nullptr;
    bool editingNew_ = false;

    enum class Drag { None, Move, Cable, CableEnd, Rubber, Number, UI } drag_ = Drag::None;
    int pressX_ = 0, pressY_ = 0, curX_ = 0, curY_ = 0;
    bool moved_ = false;
    // a move: where each dragged box was when the drag began (each event places them from there)
    std::unordered_map<const Box*, std::pair<int, int>> moveFrom_;
    Box* dragBox_ = nullptr;
    std::string uiBefore_;          // what an interface box saved before a mouse edit
    int dragPort_ = 0;
    // CableEnd: the cable whose end is dragged to another port, and which end (its inlet's or its outlet's)
    Connection dragConn_{};
    bool dragInletEnd_ = false;
    int spliceCable_ = -1;          // Move with Shift held: the cable the box goes into on release (drawn lit)
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

    // the layout of each box, measured again only when its text, the settings or the zoom change
    struct Layout {
        std::string label;
        int kind = -1, ports = -1, w = 0, h = 0;
        double zoom = 0;
        unsigned settings = 0;
    };
    std::unordered_map<const Box*, Layout> layouts_;
};

// A window showing a subpatcher, abstraction or pfft~ patch.
class PatchWindow : public Fl_Double_Window {
public:
    // doc: the document the patch belongs to
    PatchWindow(DocumentWindow* doc, Patch* p, const std::string& title);
    ~PatchWindow() override;
    int handle(int e) override;
    void detach();          // forget the patch (it is being destroyed)
    Canvas* canvas = nullptr;
    Patch* patch;
    DocumentWindow* doc;
};

// A patch as text, in its own window: a second view on the same patch,
// kept in sync both ways (see textview.cpp).
class PatchTextWindow : public Fl_Double_Window {
public:
    PatchTextWindow(DocumentWindow* doc, Patch* p, const std::string& title, bool readOnly);
    ~PatchTextWindow() override;
    void detach();                 // the patch is being destroyed
    void applyNow();               // apply a pending text edit
    void refresh(bool force);      // show the patch's current text
    void applyFont();              // the code font and size of the settings
    void replaceText(const std::string& t);   // as if typed (tests)
    Fl_Text_Editor* editor() const { return editor_; }
    Patch* patch;

private:
    static void modified(int pos, int nIns, int nDel, int nRestyled, const char* deleted, void* d);
    static void applyLater(void* d);
    static void poll(void* d);
    void restyle(int from, int to);
    void setStatus(const std::string& s, bool error = false);
    DocumentWindow* doc_;
    bool readOnly_;
    Fl_Text_Buffer* text_;
    Fl_Text_Buffer* style_;
    Fl_Text_Editor* editor_;
    Fl_Box* status_;
    std::string shown_;            // the text last in sync with the patch
    uint64_t shownChange_ = 0;     // App::changes() when it was
    bool pending_ = false;         // edited, not applied yet
    bool syncing_ = false;
};

// Round toolbar button (audio on/off, edit mode, code view).
class ToolButton : public Fl_Button {
public:
    enum Icon { Power, Pencil, Code, Minus, Plus, None, Browser, Console, Record, Play, Stop, Loop, ToStart };
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
    bool clipShown_ = false;
};

Fl_Preferences& appPrefs();      // the user's settings (browser.cpp)

// A menu command (menus.cpp): the menus of every platform are built from the table of them, and
// tooltips and hints name their shortcuts from it.
struct Command {
    const char* id;              // "file.save"
    const char* path;            // where it is in the menu bar: "File/Save"
    int shortcut;                // FL_COMMAND + 's'; 0: none
    int flags;                   // FL_MENU_DIVIDER, FL_MENU_TOGGLE
    void (*run)();
    bool (*enabled)();           // nullptr: always
    bool (*checked)();           // toggles: on or off
};
const std::vector<Command>& commands();
const Command* findCommand(const std::string& id);
// "Edit mode on/off (Cmd+E)": a text and the command's shortcut, the platform's way
std::string withShortcut(const std::string& text, const std::string& commandId);

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
    std::vector<DocumentWindow*> docs;
    bool quitting = false;
    bool startAudio = true;     // --no-audio: start with audio off
    bool ready = false;         // init() done: files the system asks to open are opened at once

    void init();                                    // after FLTK is set up
    DocumentWindow* newDocument();                  // File > New: an empty window
    DocumentWindow* openDocument(const std::string& path);   // reuses an empty Untitled window
    void openLater(const std::string& path);        // the system asked (Finder): opened once ready
    void setReady();                                // the command line is read: open what was asked
    void openDialog();                              // File > Open...
    void importDialog();                            // File > Import from Max...
    DocumentWindow* importMax(const std::string& path);
    void closeDocument(DocumentWindow* w);          // already confirmed
    void closeFront();                              // Cmd+W
    void quit();                                    // asks about unsaved documents
    DocumentWindow* frontDoc();                     // document of the front patch window (help windows skipped)
    // the document the commands act on: the front patch window's; nullptr when that is a help window
    DocumentWindow* targetDoc();
    Fl_Window* frontPatchWindow();                  // the front window showing a patch (any kind)
    bool helpInFront();                             // the front patch window shows a help patch
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
    void addRecent(const std::string& path);        // stored as an absolute path
    void showBanner();                  // Shift+Cmd+J: Max und Moritz
    void showConsole();
    void settingsChanged();             // the settings changed: every window shows them
    void ensureConsole();               // shows the console if it is hidden, without raising it
    // the code editors (code.cpp): Musil scripts (.mu)
    CodeWindow* newCode();
    CodeWindow* openCode(const std::string& path);
    CodeWindow* openCodeFor(CodeSource* s);          // the script of a code box (double-click)
    // the box is gone (deleted, replaced by undo or the code view): its editor is bound again to the
    // box that took its place, closed when it has nothing unsaved, or kept as a buffer of its own
    void codeSourceClosed(CodeSource* s);
    void markEdited(Patch* p);                       // the document showing p is now edited
    void pushUndoFor(Patch* p);                      // an undo step in the document showing p
    DocumentWindow* docOf(Patch* p);                 // the document p belongs to
    void abstractionSaved(Patch* a);                 // other copies of its file follow it
    void closeCode(CodeWindow* w);      // already confirmed
    CodeWindow* frontCode();            // the code window in front, if one is
    // the sound editors (sound_window.cpp): a sound file, or a buffer~ of the open patches
    SoundWindow* openSound(const std::string& path);         // a file (already open: brought forward)
    void openSoundDialog();                                  // File > Open Sound...
    SoundWindow* editBuffer(Patch* root, const std::string& name);   // the buffer~ of that name
    void closeSound(SoundWindow* w);                         // already confirmed
    SoundWindow* frontSound();                               // the sound window in front, if one is
    const std::vector<SoundWindow*>& soundWindows() const { return soundWindows_; }
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
    // something in a patch changed (an edit, a structure change, a box redrawn): the code views
    // compare this count to know when to read their patch again
    uint64_t changes() const { return changes_; }
    void patchChanged() { ++changes_; }

    void showText();                         // Patch > Code View: the front patch as text
    void showText(DocumentWindow* doc, Patch* p, const std::string& title, bool readOnly);
    void closeText(Patch* p);                // hide its text window, if any
    PatchTextWindow* textWindow(Patch* p);
    void openHelp(const std::string& cls);   // the object's help patch (a document of its own), or its description
    void showReference(const std::string& cls);   // the object's reference, in its window
    // the menus (menus.cpp)
    void buildMenu(Fl_Menu_Bar* m);
    void refreshMenus();                     // enabled and checked items; the macOS bar only when they change
    // refreshMenus() whenever any window comes to the front (a code window, a text view, the console...)
    // and before each key goes to a shortcut: the states follow what the commands act on
    void followFrontWindow();
    bool menuItemActive(const std::string& id);   // the state shown in the (first) menu bar (tests)
    void rebuildRecentMenus();               // File > Open Recent follows the list
    Fl_Menu_Bar* menuFor(DocumentWindow* w); // the bar showing w's menu state
    void forgetMenu(Fl_Menu_Bar* m);         // its window is closing

private:
    App() = default;
    std::vector<PatchTextWindow*> textWindows_;
    std::vector<CodeWindow*> codeWindows_;
    std::vector<SoundWindow*> soundWindows_;
    Fl_Menu_Bar* sysMenu_ = nullptr;          // macOS: one menu bar for all windows
    Fl_Double_Window* console_ = nullptr;
    Fl_Text_Display* consoleView_ = nullptr;
    Fl_Text_Buffer* consoleBuf_ = nullptr;
    std::vector<std::string> consoleLines_;   // kept until the console opens: a ring of kConsoleKeep
    size_t consoleFirst_ = 0;                 // the oldest line in consoleLines_, once it is full
    int consoleBufLines_ = 0;                 // lines in consoleBuf_
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
    uint64_t changes_ = 0;
    std::vector<std::string> openLater_;     // asked to open before init() was done
    // code editors whose box went away while something was being rebuilt: settled from the event loop
    struct Orphan {
        CodeWindow* window;
        DocumentWindow* doc;     // the document the box was in (nullptr: closed)
        std::string where;       // the patch it was in, as a path of box ids
        std::string id;          // the box
    };
    std::vector<Orphan> orphans_;
    static void settleOrphans(void* data);
    // menus: the items each bar has for the commands, and their last state
    struct MenuState {
        Fl_Menu_Bar* bar;
        std::vector<int> index;            // per command, -1: not in this bar
        std::vector<int> flags;            // per command, the active/checked flags last set
    };
    std::vector<MenuState> menus_;
    void indexMenu(MenuState& s);
    friend class PatchWindow;
};

// A document: one patch file in its own window, with its subpatcher windows,
// undo history and status bar. It is the editor of its patches' canvases.
class DocumentWindow : public Fl_Double_Window, public PatchEditor {
public:
    DocumentWindow(int w, int h);
    ~DocumentWindow() override;

    Document doc;
    Patch& patch;           // an extra of App::host, removed when the window closes

    // File
    bool openFile(const std::string& path);   // into this window
    // a patch converted from elsewhere: Untitled, edited, saved as name.moritz in dir
    bool loadConverted(const std::string& src, const std::string& dir, const std::string& name);
    // the help patch of a class, into this window (Document::loadHelp): locked, its Reference button shown
    bool openHelp(const std::string& path, const std::string& cls);
    bool isHelp() const { return !doc.helpFile().empty(); }
    const std::string& helpClass() const { return helpClass_; }
    bool save();                              // the patch (Save As when it has no file yet)
    bool saveAs();
    bool saveAbstraction(Patch* a);           // an abstraction edited in its window, into its file
    bool saveFront();                         // File > Save: the abstraction of the front window, or the patch
    void revert();                            // File > Revert: the file as saved (asks first)
    // asks about each unsaved part (code editors of its boxes, abstractions, the patch): false: cancel
    bool confirmDiscard();
    bool isEmptyUntitled() const { return doc.isEmptyUntitled(); }
    const std::string& path() const { return doc.path(); }
    // Edit
    void undo();
    void redo();
    // Patch
    void toggleLock();

    // PatchEditor: what the canvases of its patches ask
    bool editable() const override { return true; }
    void pushUndo(Patch* p) override;
    void dropUndo() override;
    void markDirty(Patch* p) override;
    void print(const std::string& s) override;   // append to the console and the status bar
    void setStatus(const std::string& s) override;
    void lockChanged() override { refreshStatus(); }
    void openSubpatch(Box* b) override;

    // subpatcher windows
    void closeSubpatchWindows(Patch* p);    // p is being destroyed
    void closeAllSubwindows();
    void forgetWindow(PatchWindow* w);
    Canvas* activeCanvas();                 // this document's front canvas
    PatchWindow* frontSubwindow();          // its front subpatcher window, if it is in front of it

    void refreshStatus();
    void log(const std::string& s) { App::get().log(s); }
    void updateTitle();
    int handle(int e) override;
    // the toolbar, as the application sees it
    void showRecording(bool on, const char* label);
    void showConsoleAlert(bool on);
    void updateMeters(float cpu, const std::vector<float>& peaks);
    Fl_Menu_Bar* menuBar() const { return menu_; }

    Canvas* canvas = nullptr;

private:
    void afterRestore(Patch* restored);
    Fl_Menu_Bar* menu_ = nullptr;   // Windows and Linux: each window has its menu
    Fl_Box* statusBar_;
    Fl_Button* refBtn_ = nullptr;   // a help patch's Reference
    std::string helpClass_;         // a help patch: the class it is the help of
    void showHelpBar(bool on);
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
    std::string message_;
    std::string shownStatus_;
};

}  // namespace moritz
