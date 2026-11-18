// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// codewindow.h — the code editor (code.cpp): a Musil script, coloured, with completion, help on the
// name under the cursor, an outline, the session's variables, and Run.
#pragma once

#include "codebox.h"
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
    bool startFile(const std::string& path);      // a file not there yet: empty, written by Save
    bool save();
    bool saveAs();
    bool confirmDiscard();                        // asks to save when edited; false: cancel
    bool dirty() const { return dirty_; }
    const std::string& path() const { return path_; }
    // the script of a code box: Save puts it back into the box (or into the file the box names), and it
    // runs in the box's interpreter
    void bind(CodeSource* s);
    CodeSource* source() const { return source_; }
    // the box is gone (App::codeSourceClosed): unbind() at once; then rebind() to the box that took its
    // place (its text again unless edited here), or keepUnbound(): the text stays, Save asks for a file
    void unbind();
    void rebind(CodeSource* s);
    void keepUnbound();
    void setText(const std::string& t);
    Fl_Text_Editor* editor() { return editor_; }
    void applyFont();                             // the code font and size of the settings

    void runBlock();                              // the selection, or the paragraph at the cursor
    void runFile();
    void stop();
    void toggleComment();
    void complete();                              // Tab after a name: the names that start like it
    void cursorMoved();                           // help on the name under the cursor
    void textChanged(int pos, int nIns, int nDel);

private:
    std::string path_;
    CodeSource* source_ = nullptr;
    std::string formerTitle_;                     // unbound: the box it was the script of
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
    Fl_Box* status_ = nullptr;
    std::string helpName_;
    std::string statusText_;

    std::shared_ptr<LangContext> context();       // the one chosen
    void run(const std::string& code, int firstLine, int lastLine);
    void restyle();                               // all of it
    void restyle(int from, int to);               // the lines from..to, more if a string spans them
    void refreshTitle();
    void refreshOutline();
    void refreshVariables();
    void setStatus(const std::string& s);
    static void outlineTimer(void* data);
    static void flashTimer(void* data);
};

// the Console's prompt (code.cpp): built into the console window, below its text
void buildConsolePrompt(Fl_Double_Window* win, int x, int y, int w, int h);

}  // namespace moritz
