// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// code.cpp — Musil in the application: the code editor (codewindow.h) and the Console's prompt.
// Both send code to the session (session.h).

#include "codewindow.h"

#include "fileio.h"
#include "gui.h"
#include "theme.h"
#include "settings.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace moritz {

namespace {

// ---- what the text is made of

bool identChar(char c) {
    return std::isalnum((unsigned char)c) || std::strchr("-?!><=_*+/#~.%$", c);
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> k = {"var", "set", "function", "return", "if", "while", "for", "break",
                                            "continue", "try", "catch", "do", "quote", "expr", "eval", "apply",
                                            "load", "help", "patch", "obj", "connect", "pos", "object"};
    return k;
}

const std::set<std::string>& documented() {
    static const std::set<std::string> s(languageNames().begin(), languageNames().end());
    return s;
}

// Styles: A plain, B comment, C string, D keyword, E bracket, F number, G quoted symbol, H a documented
// name, I the line of an error, J the lines just run.
Fl_Font codeFont() { return kCodeFontSlot; }    // named by the settings (settings.cpp)

Fl_Text_Display::Style_Table_Entry* styleTable() {
    static Fl_Text_Display::Style_Table_Entry s[10];
    static bool made = false;
    for (auto& e : s) e.size = settings().codeSize;      // the size may have changed since
    if (!made) {
        Fl_Font f = codeFont();
        const Fl_Color c[8] = {fl_rgb_color(214, 216, 222), fl_rgb_color(120, 160, 128), fl_rgb_color(231, 174, 115),
                               fl_rgb_color(199, 146, 234), fl_rgb_color(124, 127, 136), fl_rgb_color(247, 140, 108),
                               fl_rgb_color(127, 219, 202), fl_rgb_color(130, 170, 255)};
        const int z = settings().codeSize;
        for (int k = 0; k < 8; ++k) s[k] = {c[k], f, z, 0, 0};
        s[8] = {fl_rgb_color(255, 214, 214), f, z, Fl_Text_Display::ATTR_BGCOLOR_EXT, fl_rgb_color(110, 40, 46)};
        s[9] = {fl_rgb_color(214, 216, 222), f, z, Fl_Text_Display::ATTR_BGCOLOR_EXT, fl_rgb_color(45, 58, 85)};
        made = true;
    }
    return s;
}

void styleText(const std::string& text, std::string& style) {
    style.assign(text.size(), 'A');
    size_t i = 0, n = text.size();
    bool lineStart = true;
    while (i < n) {
        char c = text[i];
        if (c == '\n') { ++i; lineStart = true; continue; }
        if (c == ';') {
            while (i < n && text[i] != '\n') style[i++] = 'B';
            continue;
        }
        if (c == '"') {
            style[i++] = 'C';
            while (i < n && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < n) style[i++] = 'C';
                style[i++] = 'C';
            }
            if (i < n) style[i++] = 'C';
            continue;
        }
        if (std::strchr("(){}", c)) { style[i++] = 'E'; lineStart = false; continue; }
        if (c == '\'' && i + 1 < n && identChar(text[i + 1])) {
            size_t j = i + 1;
            while (j < n && identChar(text[j])) ++j;
            std::fill(style.begin() + (long)i, style.begin() + (long)j, 'G');
            i = j;
            continue;
        }
        if (identChar(c)) {
            size_t j = i;
            while (j < n && identChar(text[j])) ++j;
            std::string w = text.substr(i, j - i);
            char s = 'A';
            char* end = nullptr;
            std::strtod(w.c_str(), &end);
            if (end && *end == '\0') s = 'F';
            else if (keywords().count(w)) s = 'D';
            else if (documented().count(w)) s = 'H';
            std::fill(style.begin() + (long)i, style.begin() + (long)j, s);
            i = j;
            lineStart = false;
            continue;
        }
        (void)lineStart;
        ++i;
    }
}

// the bracket depth at the start of every line (strings and comments left out)
std::vector<int> lineDepths(const std::string& text) {
    std::vector<int> d{0};
    int depth = 0;
    bool str = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (str) {
            if (c == '\\') ++i;
            else if (c == '"') str = false;
            else if (c == '\n') d.push_back(depth);
            continue;
        }
        if (c == '"') str = true;
        else if (c == ';') { while (i + 1 < text.size() && text[i + 1] != '\n') ++i; }
        else if (c == '(' || c == '{') ++depth;
        else if (c == ')' || c == '}') depth = std::max(0, depth - 1);
        else if (c == '\n') d.push_back(depth);
    }
    return d;
}

int bracketBalance(const std::string& text) {
    std::string t = text + "\n";
    auto d = lineDepths(t);
    return d.back();
}

std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        size_t b = t.find('\n', a);
        out.push_back(t.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

bool blank(const std::string& l) { return l.find_first_not_of(" \t\r") == std::string::npos; }

// a popup of the names starting with prefix, at x y; the one chosen, or ""
std::string pickName(const std::string& prefix, const std::vector<std::string>& extra, int x, int y) {
    std::set<std::string> all(languageNames().begin(), languageNames().end());
    all.insert(extra.begin(), extra.end());
    for (auto& k : keywords()) all.insert(k);
    std::vector<std::string> hits;
    for (auto it = all.lower_bound(prefix); it != all.end() && it->rfind(prefix, 0) == 0; ++it) hits.push_back(*it);
    if (hits.empty()) return "";
    if (hits.size() == 1) return hits[0];
    if (hits.size() > 40) hits.resize(40);
    std::vector<std::string> labels;
    for (auto& h : hits) {
        std::string l = h;
        if (auto* e = languageHelp(h); e && e->signature.size() > h.size() + 2) {
            std::string sig = e->signature;
            if (sig.size() > 60) sig = sig.substr(0, 57) + "...";
            l += "      " + sig;
        }
        std::string esc;
        for (char c : l) { if (c == '/' || c == '\\' || c == '&' || c == '_') esc += '\\'; esc += c; }
        labels.push_back(esc);
    }
    std::vector<Fl_Menu_Item> items(hits.size() + 1);
    for (size_t k = 0; k < hits.size(); ++k) {
        std::memset(&items[k], 0, sizeof(Fl_Menu_Item));
        items[k].text = labels[k].c_str();
        items[k].labelfont_ = codeFont();
        items[k].labelsize_ = 12;
        items[k].labelcolor_ = kText;
    }
    std::memset(&items.back(), 0, sizeof(Fl_Menu_Item));
    const Fl_Menu_Item* m = items[0].popup(x, y);
    if (!m) return "";
    return hits[(size_t)(m - &items[0])];
}

std::string wordBefore(const std::string& text, int pos) {
    int a = pos;
    while (a > 0 && identChar(text[(size_t)a - 1])) --a;
    return text.substr((size_t)a, (size_t)(pos - a));
}

std::string wordAt(Fl_Text_Buffer* b, int pos) {
    int len = b->length();
    int a = pos, z = pos;
    while (a > 0 && identChar(b->char_at(a - 1))) --a;
    while (z < len && identChar(b->char_at(z))) ++z;
    if (z <= a) return "";
    char* s = b->text_range(a, z);
    std::string w = s;
    std::free(s);
    return w;
}

std::string helpText(const std::string& name) {
    auto* e = languageHelp(name);
    if (!e) {
        if (keywords().count(name)) return name + "\n\na form of Musil (see the Musil manual)";
        return "";
    }
    std::string t = e->signature + "\n\n";
    for (auto& l : e->text) t += l + "\n";
    return t;
}

// ---- the editor widget

class CodeEditor : public Fl_Text_Editor {
public:
    CodeWindow* owner = nullptr;
    CodeEditor(int x, int y, int w, int h) : Fl_Text_Editor(x, y, w, h) {}
    int handle(int e) override {
        if (e == FL_KEYBOARD) {
            int k = Fl::event_key();
            bool cmd = Fl::event_state(FL_COMMAND), shift = Fl::event_state(FL_SHIFT);
            if (cmd && (k == FL_Enter || k == FL_KP_Enter)) {
                if (shift) owner->runFile();
                else owner->runBlock();
                return 1;
            }
            if ((cmd && k == '.') || k == FL_Escape) { owner->stop(); return 1; }
            if (cmd && k == '/') { owner->toggleComment(); return 1; }
            if (k == FL_Tab && !shift && !cmd) {
                int p = insert_position();
                if (p > 0 && identChar(buffer()->char_at(p - 1))) owner->complete();
                else insert("    ");
                return 1;
            }
        }
        int r = Fl_Text_Editor::handle(e);
        if (e == FL_KEYBOARD || e == FL_RELEASE || e == FL_PUSH) owner->cursorMoved();
        return r;
    }
};

}  // namespace

// ---- the window

CodeWindow::CodeWindow() : Fl_Double_Window(1060, 680, "Untitled — code") {
    color(kStatusBg);
    const int tb = 36, sb = 22, left = 180, right = 260;
    int W = w(), H = h();
    // toolbar
    auto* bar = new Fl_Group(0, 0, W, tb);
    bar->box(FL_FLAT_BOX);
    bar->color(fl_rgb_color(32, 33, 37));
    auto button = [&](int x, int wd, const char* label, Fl_Callback* cb, const char* tip) {
        auto* b = new Fl_Button(x, 6, wd, 24, label);
        b->labelsize(12);
        b->box(FL_FLAT_BOX);
        b->color(fl_rgb_color(48, 50, 56));
        b->labelcolor(kText);
        b->callback(cb, this);
        b->tooltip(tip);
        b->clear_visible_focus();
        return b;
    };
    button(8, 96, "Run block", [](Fl_Widget*, void* d) { static_cast<CodeWindow*>(d)->runBlock(); },
           "Run the selection, or the paragraph at the cursor (Cmd-Enter)")->color(fl_rgb_color(40, 90, 190));
    button(108, 80, "Run file", [](Fl_Widget*, void* d) { static_cast<CodeWindow*>(d)->runFile(); },
           "Run the whole file (Shift-Cmd-Enter)");
    button(192, 60, "Stop", [](Fl_Widget*, void* d) { static_cast<CodeWindow*>(d)->stop(); },
           "Stop the code that is running (Cmd-. or Esc)");
    button(262, 84, "Comment", [](Fl_Widget*, void* d) { static_cast<CodeWindow*>(d)->toggleComment(); },
           "Comment or uncomment the lines (Cmd-/)");
    button(352, 76, "Console", [](Fl_Widget*, void*) { App::get().showConsole(); },
           "The Console: what the code prints, and a prompt");
    bar->end();
    // the outline, the editor, the variables and the help, side by side
    auto* tile = new Fl_Tile(0, tb, W, H - tb - sb);
    outline_ = new Fl_Hold_Browser(0, tb, left, H - tb - sb);
    outline_->textsize(12);
    outline_->color(fl_rgb_color(30, 31, 35));
    outline_->textcolor(kText);
    outline_->selection_color(fl_rgb_color(45, 58, 85));
    outline_->box(FL_FLAT_BOX);
    outline_->callback([](Fl_Widget*, void* d) {
        auto* cw = static_cast<CodeWindow*>(d);
        int i = cw->outline_->value();
        if (i < 1 || i > (int)cw->outlineLines_.size()) return;
        int pos = cw->text_->skip_lines(0, cw->outlineLines_[(size_t)i - 1] - 1);
        cw->editor_->insert_position(pos);
        cw->editor_->scroll(std::max(1, cw->outlineLines_[(size_t)i - 1] - 5), 0);
        cw->editor_->take_focus();
        cw->cursorMoved();
    }, this);
    text_ = new Fl_Text_Buffer();
    style_ = new Fl_Text_Buffer();
    auto* ed = new CodeEditor(left, tb, W - left - right, H - tb - sb);
    ed->owner = this;
    editor_ = ed;
    editor_->buffer(text_);
    editor_->highlight_data(style_, styleTable(), 10, 'A', nullptr, nullptr);
    editor_->color(fl_rgb_color(30, 31, 35));
    editor_->textfont(codeFont());
    editor_->textsize(settings().codeSize);
    editor_->textcolor(kText);
    editor_->cursor_color(kText);
    editor_->selection_color(fl_rgb_color(60, 80, 120));
    editor_->box(FL_FLAT_BOX);
    editor_->linenumber_width(44);
    editor_->linenumber_bgcolor(fl_rgb_color(26, 27, 30));
    editor_->linenumber_fgcolor(fl_rgb_color(100, 102, 110));
    editor_->linenumber_font(codeFont());
    editor_->linenumber_size(11);
    editor_->scrollbar_size(12);
    auto* side = new Fl_Group(W - right, tb, right, H - tb - sb);
    int vh = (H - tb - sb) / 2;
    vars_ = new Fl_Browser(W - right, tb, right, vh);
    vars_->textsize(11);
    vars_->textfont(codeFont());
    vars_->color(fl_rgb_color(26, 27, 30));
    vars_->textcolor(kText);
    vars_->box(FL_FLAT_BOX);
    static int widths[] = {84, 0};
    vars_->column_widths(widths);
    vars_->has_scrollbar(Fl_Browser_::VERTICAL);
    vars_->column_char('\t');
    helpBuf_ = new Fl_Text_Buffer();
    help_ = new Fl_Text_Display(W - right, tb + vh, right, H - tb - sb - vh);
    help_->buffer(helpBuf_);
    help_->textsize(12);
    help_->color(fl_rgb_color(26, 27, 30));
    help_->textcolor(kText);
    help_->box(FL_FLAT_BOX);
    help_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    help_->scrollbar_size(10);
    side->resizable(help_);
    side->end();
    tile->end();
    status_ = new Fl_Box(0, H - sb, W, sb);
    status_->box(FL_FLAT_BOX);
    status_->color(kStatusBg);
    status_->labelcolor(kDim);
    status_->labelsize(11);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    resizable(tile);
    end();
    text_->add_modify_callback([](int, int ins, int del, int, const char*, void* d) {
        if (ins || del) static_cast<CodeWindow*>(d)->textChanged();
    }, this);
    callback([](Fl_Widget* w, void*) {
        auto* cw = static_cast<CodeWindow*>(w);
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;   // Esc stops, it does not close
        if (cw->confirmDiscard()) App::get().closeCode(cw);
    });
    refreshVariables();
    setStatus("Cmd-Enter runs the block · Shift-Cmd-Enter the file · Esc stops · Tab completes · Cmd-/ comments");
    helpBuf_->text("Move the cursor onto a name to read its help here.\n\nAbove: what the session defines, after each run.");
    refreshTitle();
}

CodeWindow::~CodeWindow() {
    Fl::remove_timeout(outlineTimer, this);
    Fl::remove_timeout(flashTimer, this);
    editor_->buffer(nullptr);
    help_->buffer(nullptr);
    delete text_;
    delete style_;
    delete helpBuf_;
}

bool CodeWindow::load(const std::string& path) {
    std::string t;
    if (!fileio::read(path, t)) return false;
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    text_->text(t.c_str());
    path_ = path;
    dirty_ = false;
    errorLine_ = 0;
    restyle();
    refreshOutline();
    refreshTitle();
    return true;
}

bool CodeWindow::startFile(const std::string& path) {
    text_->text("");
    path_ = path;
    dirty_ = false;
    errorLine_ = 0;
    restyle();
    refreshOutline();
    refreshTitle();
    setStatus(path.substr(path.find_last_of("/\\") + 1) + " is not there yet: Cmd-S writes it");
    return true;
}

void CodeWindow::applyFont() {
    editor_->highlight_data(style_, styleTable(), 10, 'A', nullptr, nullptr);   // the sizes, measured again
    editor_->textsize(settings().codeSize);
    redraw();
}

void CodeWindow::bind(CodeSource* s) {
    source_ = s;
    helpBuf_->text(("Move the cursor onto a name to read its help here.\n\nAbove: what " + s->title() +
                    " defines, after each run (in1, in2... are its inlets).").c_str());
    refreshTitle();
    refreshVariables();
    if (!s->file().empty() && !fileio::isFile(s->file())) {
        setStatus(s->title() + " is not there yet: Cmd-S writes it · Cmd-Enter runs the block in the box");
        return;
    }
    setStatus(s->file().empty() ? "the script of " + s->title() + ": Cmd-S puts it back into the box · Cmd-Enter runs the block in the box"
                                : "the file of " + s->title() + " · Cmd-Enter runs the block in the box");
}

void CodeWindow::unbind() {
    if (source_) formerTitle_ = source_->title();
    source_ = nullptr;
}

void CodeWindow::rebind(CodeSource* s) {
    formerTitle_.clear();
    if (!dirty_) {                                  // nothing edited here: the text of the box that is there now
        std::string f = s->file();
        if (f.empty()) { path_.clear(); setText(s->script()); }
        else if (f != path_ && !load(f)) startFile(f);
    }
    bind(s);
}

void CodeWindow::keepUnbound() {
    refreshTitle();
    refreshVariables();
    setStatus((formerTitle_.empty() ? std::string("its box") : formerTitle_) +
              " is gone: the text is kept here; Save As puts it in a file");
}

void CodeWindow::setText(const std::string& t) {
    text_->text(t.c_str());
    dirty_ = false;
    errorLine_ = 0;
    restyle();
    refreshOutline();
    refreshTitle();
}

bool CodeWindow::save() {
    if (source_ && path_.empty()) {                     // the script of a code box: into the box, an edit of its patch
        char* t = text_->text();
        Patch* p = source_->patchOf();
        App::get().pushUndoFor(p);                      // undo takes the script back
        source_->setScript(t);
        std::free(t);
        App::get().markEdited(p);
        dirty_ = false;
        refreshTitle();
        return true;
    }
    if (path_.empty()) return saveAs();
    char* t = text_->text();
    std::string err;
    bool ok = fileio::writeAtomic(path_, t, &err);   // a failed write leaves the file as it was
    std::free(t);
    if (!ok) { alertUser("Can't save %s:\n%s", path_.c_str(), err.c_str()); return false; }
    dirty_ = false;
    refreshTitle();
    App::get().addRecent(path_);
    return true;
}

bool CodeWindow::saveAs() {
    Fl_Native_File_Chooser fc;
    fc.title("Save Code");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("Musil script\t*.mu");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    fc.preset_file(path_.empty() ? "untitled.mu" : path_.substr(path_.find_last_of("/\\") + 1).c_str());
    if (fc.show() != 0) return false;
    std::string p = portablePath(fc.filename());
    if (p.size() < 3 || p.compare(p.size() - 3, 3, ".mu") != 0) p += ".mu";
    std::string old = path_;
    path_ = p;
    if (!save()) { path_ = old; return false; }   // the editor stays as it was
    formerTitle_.clear();
    return true;
}

bool CodeWindow::confirmDiscard() {
    if (!dirty_) return true;
    std::string name = source_ && path_.empty() ? source_->title() : path_.empty() ? "Untitled" : path_.substr(path_.find_last_of("/\\") + 1);
    int r = askUser("Save the changes to %s?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return save();
    dirty_ = false;             // discarded: it closes (or its box goes) without asking again
    return true;
}

void CodeWindow::refreshTitle() {
    std::string name = source_ && path_.empty() ? source_->title()
                       : !path_.empty()          ? path_.substr(path_.find_last_of("/\\") + 1)
                       : !formerTitle_.empty()   ? formerTitle_ + " (its box is gone)"
                                                 : "Untitled";
    copy_label((name + (dirty_ ? " *" : "") + " — code").c_str());
}

void CodeWindow::setStatus(const std::string& s) {
    statusText_ = "  " + s;
    status_->label(statusText_.c_str());
    status_->redraw();
}

std::shared_ptr<LangContext> CodeWindow::context() { return source_ ? source_->context() : sessionContext(); }

void CodeWindow::run(const std::string& code, int firstLine, int lastLine) {
    auto ctx = context();
    if (ctx->busy()) { setStatus("busy: " + ctx->label + " is still running (Stop to end it)"); return; }
    errorLine_ = 0;
    flashFrom_ = firstLine;
    flashTo_ = lastLine;
    restyle();
    Fl::remove_timeout(flashTimer, this);
    Fl::add_timeout(0.35, flashTimer, this);
    std::string file = path_.empty() ? (source_ ? source_->title() : "untitled.mu") : path_;
    setStatus("running in " + ctx->label + "...");
    App::get().ensureConsole();
    show();
    auto alive = std::make_shared<Fl_Widget_Tracker>(this);
    ctx->eval(code, file, firstLine, [this, alive, file, ctx](const EvalResult& r) {
        if (!alive->exists()) return;
        if (r.ok) {
            if (!r.value.empty()) App::get().log("→ " + r.value);
            setStatus(r.value.empty() ? "done" : "→ " + r.value.substr(0, 200));
        } else {
            App::get().log("error: " + r.error);
            std::string first = r.error.substr(0, r.error.find('\n'));
            if (first.rfind(file, 0) == 0) first = file.substr(file.find_last_of("/\\") + 1) + first.substr(file.size());
            setStatus(r.stopped ? "stopped" : first);
            if (!r.stopped && r.file == file && r.line > 0) {
                errorLine_ = r.line;
                restyle();
                editor_->scroll(std::max(1, r.line - 5), 0);
            }
        }
        refreshVariables();
    });
}

void CodeWindow::runBlock() {
    int a = 0, b = 0;
    char* all = text_->text();
    std::string t = all;
    std::free(all);
    auto lines = splitLines(t);
    int first, last;
    if (text_->selection_position(&a, &b) && b > a) {
        first = text_->count_lines(0, a) + 1;
        last = text_->count_lines(0, b > 0 && text_->char_at(b - 1) == '\n' ? b - 1 : b) + 1;
    } else {
        // the paragraph at the cursor: lines between blank lines that are outside every bracket
        auto depth = lineDepths(t);
        int cur = text_->count_lines(0, editor_->insert_position());   // 0-based
        auto boundary = [&](int k) { return k < 0 || k >= (int)lines.size() || (blank(lines[(size_t)k]) && depth[(size_t)k] == 0); };
        if (boundary(cur)) {                                        // on a blank line: the paragraph above
            while (cur >= 0 && boundary(cur)) --cur;
            if (cur < 0) return;
        }
        int s = cur, e = cur;
        while (!boundary(s - 1)) --s;
        while (!boundary(e + 1)) ++e;
        first = s + 1;
        last = e + 1;
    }
    std::string code;
    for (int k = first - 1; k < last && k < (int)lines.size(); ++k) code += lines[(size_t)k] + "\n";
    run(code, first, last);
}

void CodeWindow::runFile() {
    char* all = text_->text();
    std::string t = all;
    std::free(all);
    run(t, 1, text_->count_lines(0, text_->length()) + 1);
}

void CodeWindow::stop() {
    context()->stop();
    setStatus("stopping...");
}

void CodeWindow::toggleComment() {
    int a = 0, b = 0;
    if (!text_->selection_position(&a, &b)) a = b = editor_->insert_position();
    int s = text_->line_start(a), e = text_->line_end(b > a && text_->char_at(b - 1) == '\n' ? b - 1 : b);
    char* sel = text_->text_range(s, e);
    std::string t = sel;
    std::free(sel);
    auto lines = splitLines(t);
    bool all = true;
    for (auto& l : lines) {
        size_t i = l.find_first_not_of(" \t");
        if (i != std::string::npos && l[i] != ';') all = false;
    }
    std::string out;
    for (size_t k = 0; k < lines.size(); ++k) {
        std::string l = lines[k];
        size_t i = l.find_first_not_of(" \t");
        if (all) {
            if (i != std::string::npos && l[i] == ';') l.erase(i, i + 1 < l.size() && l[i + 1] == ' ' ? 2 : 1);
        } else if (i != std::string::npos) {
            l.insert(i, "; ");
        }
        out += l + (k + 1 < lines.size() ? "\n" : "");
    }
    text_->replace(s, e, out.c_str());
    text_->select(s, s + (int)out.size());
}

void CodeWindow::complete() {
    char* all = text_->text();
    std::string t = all;
    std::free(all);
    int pos = editor_->insert_position();
    std::string prefix = wordBefore(t, pos);
    if (prefix.empty()) return;
    int x = 0, y = 0;
    editor_->position_to_xy(pos, &x, &y);
    std::string pick = pickName(prefix, context()->names(), x, y + 16);
    if (pick.size() > prefix.size()) editor_->insert(pick.substr(prefix.size()).c_str());
    cursorMoved();
}

void CodeWindow::cursorMoved() {
    int pos = editor_->insert_position();
    std::string w = wordAt(text_, pos);
    if (w.empty() && pos > 0) w = wordAt(text_, pos - 1);
    std::string line = "line " + std::to_string(text_->count_lines(0, pos) + 1);
    if (!w.empty() && w != helpName_) {
        std::string h = helpText(w);
        if (!h.empty()) {
            helpName_ = w;
            helpBuf_->text(h.c_str());
        }
    }
    (void)line;
}

void CodeWindow::textChanged() {
    if (!dirty_) {
        dirty_ = true;
        refreshTitle();
    }
    errorLine_ = 0;
    restyle();
    Fl::remove_timeout(outlineTimer, this);
    Fl::add_timeout(0.5, outlineTimer, this);
}

void CodeWindow::restyle() {
    char* all = text_->text();
    std::string t = all;
    std::free(all);
    std::string st;
    styleText(t, st);
    auto mark = [&](int lineFrom, int lineTo, char s) {
        int a = text_->skip_lines(0, lineFrom - 1), b = text_->skip_lines(0, lineTo);
        for (int k = a; k < b && k < (int)st.size(); ++k) st[(size_t)k] = s;
    };
    if (flashFrom_ > 0) mark(flashFrom_, flashTo_, 'J');
    if (errorLine_ > 0) mark(errorLine_, errorLine_, 'I');
    style_->text(st.c_str());
    editor_->redisplay_range(0, text_->length());
}

void CodeWindow::refreshOutline() {
    char* all = text_->text();
    std::string t = all;
    std::free(all);
    auto lines = splitLines(t);
    outline_->clear();
    outlineLines_.clear();
    for (size_t k = 0; k < lines.size(); ++k) {
        const std::string& l = lines[k];
        for (const char* kw : {"function ", "var ", "object "}) {
            size_t n = std::strlen(kw);
            if (l.compare(0, n, kw) != 0) continue;
            size_t e = l.find_first_of(" (", n);
            std::string name = l.substr(n, e == std::string::npos ? std::string::npos : e - n);
            std::string mark = std::string(kw) == "function " ? "λ " : std::string(kw) == "object " ? "◆ " : "· ";
            outline_->add((mark + name).c_str());
            outlineLines_.push_back((int)k + 1);
        }
    }
}

void CodeWindow::refreshVariables() {
    vars_->clear();
    auto ctx = context();
    vars_->add(("@C" + std::to_string(kDim) + "@s" + ctx->label).c_str());
    for (auto& v : ctx->variables()) {
        std::string line = "@b" + v.name + "\t@C" + std::to_string(kDim) + v.kind + "  " + v.preview;
        vars_->add(line.c_str());
    }
}

void CodeWindow::outlineTimer(void* data) { static_cast<CodeWindow*>(data)->refreshOutline(); }

void CodeWindow::flashTimer(void* data) {
    auto* cw = static_cast<CodeWindow*>(data);
    cw->flashFrom_ = cw->flashTo_ = 0;
    cw->restyle();
}

// ---- the Console's prompt

namespace {

struct Prompt {
    std::vector<std::string> history;
    int historyAt = 0;
    std::string pending;                 // lines of a form not closed yet
};
Prompt& prompt() {
    static Prompt p;
    return p;
}

std::shared_ptr<LangContext> promptContext() { return sessionContext(); }

class PromptInput : public Fl_Input {
public:
    PromptInput(int x, int y, int w, int h) : Fl_Input(x, y, w, h) {}
    int handle(int e) override {
        auto& p = prompt();
        if (e == FL_KEYBOARD) {
            int k = Fl::event_key();
            if (k == FL_Enter || k == FL_KP_Enter) { submit(); return 1; }
            if (k == FL_Escape || (Fl::event_state(FL_COMMAND) && k == '.')) { promptContext()->stop(); return 1; }
            if ((k == FL_Up || k == FL_Down) && !p.history.empty()) {
                p.historyAt = std::clamp(p.historyAt + (k == FL_Up ? -1 : 1), 0, (int)p.history.size());
                value(p.historyAt < (int)p.history.size() ? p.history[(size_t)p.historyAt].c_str() : "");
                insert_position(size());
                return 1;
            }
            if (k == FL_Tab) {
                std::string t = value();
                int pos = insert_position();
                std::string prefix = wordBefore(t, pos);
                if (!prefix.empty()) {
                    fl_font(textfont(), textsize());
                    int x = this->x() + 4 + (int)fl_width(t.substr(0, (size_t)pos).c_str()), y = this->y();
                    std::string pick = pickName(prefix, promptContext()->names(), x, y);
                    if (pick.size() > prefix.size()) insert(pick.substr(prefix.size()).c_str());
                }
                return 1;
            }
        }
        return Fl_Input::handle(e);
    }
    void submit() {
        auto& p = prompt();
        std::string line = value();
        value("");
        if (line.empty() && p.pending.empty()) return;
        if (!line.empty()) { p.history.push_back(line); p.historyAt = (int)p.history.size(); }
        auto ctx = promptContext();
        App::get().log((p.pending.empty() ? "musil> " : "   ... ") + line);
        p.pending += line + "\n";
        if (bracketBalance(p.pending) > 0) return;          // the form goes on, on the next line
        std::string code = p.pending;
        p.pending.clear();
        if (!ctx->eval(code, "<console>", 1, [](const EvalResult& r) {
                if (r.ok) { if (!r.value.empty()) App::get().log("→ " + r.value); }
                else App::get().log(r.stopped ? "stopped" : "error: " + r.error);
            }))
            App::get().log(ctx->label + " is busy: Esc stops it");
    }
};

}  // namespace

void buildConsolePrompt(Fl_Double_Window* win, int x, int y, int w, int h) {
    auto* in = new PromptInput(x, y, w - 66, h);
    in->textfont(codeFont());
    in->textsize(12);
    in->color(fl_rgb_color(16, 17, 19));
    in->textcolor(kText);
    in->cursor_color(kText);
    in->box(FL_FLAT_BOX);
    in->tooltip("Musil: Enter runs, Up/Down history, Tab completes, Esc stops");
    auto* stopB = new Fl_Button(x + w - 60, y, 60, h, "Stop");
    stopB->labelsize(11);
    stopB->callback([](Fl_Widget*, void*) { promptContext()->stop(); });
    stopB->clear_visible_focus();
    (void)win;
    in->take_focus();
}

}  // namespace moritz
