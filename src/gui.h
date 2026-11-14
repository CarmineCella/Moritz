// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// gui.h — FLTK editor: canvas, main window, subpatcher windows
#pragma once

#include "minlsp.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Sys_Menu_Bar.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>

namespace mlsp {

class MainWindow;

std::string examplesDir();   // the bundled examples (also searched for abstractions)

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
    void duplicateSelection();   // option-drag: copies boxes and their inner cables

    // object-name completion while typing in a new/edited object box
    void updateSuggestions();
    bool hasSuggestions() const { return !sugg_.empty(); }
    void moveSuggestion(int delta);
    bool acceptSuggestion(bool commit);
    void clearSuggestions();
    void fitEditor();      // size the text field to what is being typed

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
    void drawBox(Box* b);
    void beginEdit(Box* b, bool isNew);
    void clearSelection();
    std::vector<Box*> selectedBoxes() const;
    void numberDrag(int dy);
    static void editCb(Fl_Widget* w, void* data);

    MainWindow* win_;
    Patch* patch_;
    bool locked_ = false;
    Fl_Input* editor_;
    Box* editing_ = nullptr;
    bool editingNew_ = false;

    enum class Drag { None, Move, Cable, Rubber, Number, UI } drag_ = Drag::None;
    int pressX_ = 0, pressY_ = 0, curX_ = 0, curY_ = 0;
    bool moved_ = false;
    Box* dragBox_ = nullptr;
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

// A window showing a subpatcher, abstraction or pfft~ patch.
class PatchWindow : public Fl_Double_Window {
public:
    PatchWindow(MainWindow* main, Patch* p, const std::string& title);
    ~PatchWindow() override;
    int handle(int e) override;
    void detach();          // forget the patch (it is being destroyed)
    Canvas* canvas = nullptr;
    Patch* patch;
};

// Round toolbar button (audio on/off, edit mode).
class ToolButton : public Fl_Button {
public:
    enum Icon { Power, Pencil };
    ToolButton(int x, int y, int w, int h, Icon icon, const char* label);
    void draw() override;

private:
    Icon icon_;
};

class MainWindow : public Fl_Double_Window {
public:
    MainWindow(int w, int h);
    ~MainWindow() override;

    Patch patch;
    AudioEngine audio;

    // File
    void newPatch();
    void closeFront();      // Cmd+W: the front subpatcher/console window, or the patch
    void about();
    void openPatch();
    bool openFile(const std::string& path);
    bool save();
    bool saveAs();
    void quit();
    // Edit
    void pushUndo();
    void dropUndo();     // discard the last pushUndo() when nothing changed
    void undo();
    void redo();
    void markDirty();
    // Patch
    void toggleLock();
    void toggleAudio();
    void setAudio(bool on);
    void clearConsole();
    void audioSetup();

    // subpatcher windows
    void openSubpatch(Box* b);
    void closeSubpatchWindows(Patch* p);    // p is being destroyed
    void forgetWindow(PatchWindow* w);
    Canvas* activeCanvas();                 // canvas of the frontmost patch window

    void setStatus(const std::string& s);
    void refreshStatus();
    void print(const std::string& s);   // append to the console and the status bar
    void log(const std::string& s);     // append to the console only
    void showConsole();
    void armTimer();                    // schedule the next patch timer event
    void updateTitle();
    bool confirmDiscard();
    int handle(int e) override;

    Canvas* canvas = nullptr;

private:
    void buildMenu();
    void restore(const std::string& snapshot);

    Fl_Sys_Menu_Bar* menu_;
    Fl_Box* statusBar_;
    ToolButton* audioBtn_;
    ToolButton* editBtn_;
    std::vector<PatchWindow*> subwindows_;
    std::vector<std::string> openSubpatchPaths() const;
    void reopenSubpatches(const std::vector<std::string>& paths);
    std::string path_;
    bool dirty_ = false;
    std::vector<std::string> undo_, redo_;
    std::string message_;
    Fl_Double_Window* console_ = nullptr;
    Fl_Text_Display* consoleView_ = nullptr;
    Fl_Text_Buffer* consoleBuf_ = nullptr;
    std::vector<std::string> consoleLines_;   // kept until the console opens
};

}  // namespace mlsp
