// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// console.cpp — the Console (prints, errors, Musil's prompt) and the Reference window

#include "gui.h"
#include "ui_util.h"
#include "settings.h"
#include "codewindow.h"
#include "reference.h"

namespace moritz {

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
        // the lines logged before it opened, oldest first; from now on they go straight into it
        std::string all;
        for (size_t k = 0; k < consoleLines_.size(); ++k)
            all += consoleLines_[(consoleFirst_ + k) % consoleLines_.size()] + "\n";
        consoleBuf_->text(all.c_str());
        consoleBufLines_ = (int)consoleLines_.size();
        consoleLines_.clear();
        consoleFirst_ = 0;
    }
    console_->show();
    consoleView_->scroll(consoleBufLines_, 0);
    consoleAlert = false;
    refreshTools();
}

void App::ensureConsole() {
    if (!console_ || !console_->visible()) showConsole();
}

void App::clearConsole() {
    consoleLines_.clear();
    consoleFirst_ = 0;
    consoleBufLines_ = 0;
    if (consoleBuf_) consoleBuf_->text("");
}

void App::print(const std::string& s) {
    log(s);
    if (auto* d = frontDoc()) d->setStatus(s);
}

void App::log(const std::string& s) {
    if (!consoleAlert) {
        std::string low = s;
        for (auto& c : low) c = (char)std::tolower((unsigned char)c);
        if (low.find("error") != std::string::npos || low.find("can't") != std::string::npos ||
            low.find("unknown") != std::string::npos || low.find("fail") != std::string::npos) {
            consoleAlert = true;
            refreshTools();
        }
    }
    if (!consoleBuf_) {                      // not opened yet: the last kConsoleKeep lines, in a ring
        constexpr size_t kConsoleKeep = 2000;
        if (consoleLines_.size() < kConsoleKeep) consoleLines_.push_back(s);
        else {
            consoleLines_[consoleFirst_] = s;
            consoleFirst_ = (consoleFirst_ + 1) % kConsoleKeep;
        }
        return;
    }
    consoleBuf_->append((s + "\n").c_str());
    consoleBufLines_ += 1 + (int)std::count(s.begin(), s.end(), '\n');
    if (consoleBuf_->length() > 200000) {    // the oldest 50 KB go, from a line's start
        int cut = consoleBuf_->line_end(consoleBuf_->length() - 150000) + 1;
        consoleBufLines_ -= consoleBuf_->count_lines(0, cut);
        consoleBuf_->remove(0, cut);
    }
    consoleView_->scroll(consoleBufLines_, 0);
}

}  // namespace moritz
