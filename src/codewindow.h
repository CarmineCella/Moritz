// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// codewindow.h — the code editor (code.cpp): a file of the Moritz language, coloured, with
// completion, help on the name under the cursor, an outline, the context's variables, and Run.
#pragma once

#include "session.h"

#include <FL/Fl_Double_Window.H>

#include <memory>
#include <string>

class Fl_Text_Editor;
class Fl_Text_Buffer;
class Fl_Text_Display;
class Fl_Hold_Browser;
class Fl_Browser;
class Fl_Box;
class Fl_Choice;
class Fl_Input;

namespace moritz {

class CodeWindow : public Fl_Double_Window {
public:
    CodeWindow();
    ~CodeWindow() override;
    bool load(const std::string& path);
    bool save();
    bool saveAs();
    bool confirmDiscard();                        // asks to save when edited; false: cancel
    const std::string& path() const { return path_; }
    Fl_Text_Editor* editor() { return editor_; }

    void runBlock();                              // the selection, or the paragraph at the cursor
    void runFile();
    void stop();
    void toggleComment();
    void complete();                              // Tab after a name: the names that start like it
    void cursorMoved();                           // help on the name under the cursor
    void textChanged();

private:
    std::string path_;
    bool dirty_ = false;
    int errorLine_ = 0;                           // highlighted until the next edit
    int flashFrom_ = 0, flashTo_ = 0;             // lines just sent to run
    Fl_Text_Editor* editor_ = nullptr;
    Fl_Text_Buffer* text_ = nullptr;
    Fl_Text_Buffer* style_ = nullptr;
    Fl_Hold_Browser* outline_ = nullptr;
    std::vector<int> outlineLines_;
    Fl_Browser* vars_ = nullptr;
    Fl_Text_Display* help_ = nullptr;
    Fl_Text_Buffer* helpBuf_ = nullptr;
    Fl_Choice* context_ = nullptr;
    std::vector<const void*> contextKeys_;       // nullptr: the session; else an open patch
    Fl_Box* status_ = nullptr;
    std::string helpName_;
    std::string statusText_;

    std::shared_ptr<LangContext> context();       // the one chosen
    void fillContexts();
    void run(const std::string& code, int firstLine, int lastLine);
    void restyle();
    void refreshTitle();
    void refreshOutline();
    void refreshVariables();
    void setStatus(const std::string& s);
    static void outlineTimer(void* data);
    static void flashTimer(void* data);
};

// the Console's prompt (code.cpp): built into the console window, below its text
void buildConsolePrompt(Fl_Double_Window* win, int x, int y, int w, int h);
void refreshConsoleContexts();

// a file of the language that is not a patch (no (patch ...) at its top level) opens as code
bool isCodeText(const std::string& text);

}  // namespace moritz
