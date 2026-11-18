// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// settings.cpp — the user's settings and their dialog (see settings.h)
#include "settings.h"
#include "gui.h"
#include "theme.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input_Choice.H>
#include <FL/Fl_Preferences.H>
#include <FL/Fl_Spinner.H>

#include <algorithm>

namespace moritz {

Settings& settings() {
    static Settings s = defaultSettings();
    return s;
}

Settings defaultSettings() {
    Settings s;
#if defined(__APPLE__)
    s.patchFont = "Helvetica";
    s.codeFont = "Menlo";
#elif defined(_WIN32)
    s.patchFont = "Arial";
    s.codeFont = "Consolas";
#else
    s.patchFont = "sans";
    s.codeFont = "DejaVu Sans Mono";
#endif
    return s;
}

std::vector<std::string> fontSuggestions(bool mono) {
#if defined(__APPLE__)
    if (mono) return {"Menlo", "Monaco", "Courier", "Courier New", "Andale Mono", "PT Mono"};
    return {"Helvetica", "Helvetica Neue", "Arial", "Avenir Next", "Gill Sans", "Lucida Grande", "Verdana",
            "Futura", "Optima", "Georgia", "Times", "Palatino", "Menlo"};
#elif defined(_WIN32)
    if (mono) return {"Consolas", "Cascadia Mono", "Courier New", "Lucida Console"};
    return {"Arial", "Segoe UI", "Tahoma", "Verdana", "Calibri", "Georgia", "Times New Roman", "Consolas"};
#else
    if (mono) return {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Ubuntu Mono", "monospace"};
    return {"sans", "DejaVu Sans", "Liberation Sans", "Noto Sans", "Ubuntu", "serif", "DejaVu Serif", "monospace"};
#endif
}

namespace {

// Fl::set_font keeps the pointer: the names live as long as the program
const char* keep(const std::string& name) {
    static std::vector<std::string*> names;
    for (auto* n : names)
        if (*n == name) return n->c_str();
    names.push_back(new std::string(name));
    return names.back()->c_str();
}

}  // namespace

void applySettings() {
    Settings& s = settings();
    s.patchSize = std::clamp(s.patchSize, 8, 32);
    s.codeSize = std::clamp(s.codeSize, 8, 32);
    if (s.patchFont.empty()) s.patchFont = defaultSettings().patchFont;
    if (s.codeFont.empty()) s.codeFont = defaultSettings().codeFont;
    Fl::set_font(kPatchFontSlot, keep(s.patchFont));
    Fl::set_font(kCodeFontSlot, keep(s.codeFont));
    kFontSize = s.patchSize;
    kBoxH = std::max(22, s.patchSize + 10);          // 22 at the usual 12
}

void loadSettings() {
    Fl_Preferences p(appPrefs(), "settings");
    Settings d = defaultSettings();
    Settings& s = settings();
    char buf[256];
    p.get("patchFont", buf, d.patchFont.c_str(), sizeof buf);
    s.patchFont = buf;
    p.get("patchSize", s.patchSize, d.patchSize);
    p.get("codeFont", buf, d.codeFont.c_str(), sizeof buf);
    s.codeFont = buf;
    p.get("codeSize", s.codeSize, d.codeSize);
    int curved = 1;
    p.get("curvedCables", curved, d.curvedCables ? 1 : 0);
    s.curvedCables = curved != 0;
    applySettings();
}

void saveSettings() {
    Fl_Preferences p(appPrefs(), "settings");
    const Settings& s = settings();
    p.set("patchFont", s.patchFont.c_str());
    p.set("patchSize", s.patchSize);
    p.set("codeFont", s.codeFont.c_str());
    p.set("codeSize", s.codeSize);
    p.set("curvedCables", s.curvedCables ? 1 : 0);
    appPrefs().flush();
}

namespace {

struct Dialog {
    Fl_Double_Window* win = nullptr;
    Fl_Input_Choice *patchFont = nullptr, *codeFont = nullptr;
    Fl_Spinner *patchSize = nullptr, *codeSize = nullptr;
    Fl_Check_Button* curved = nullptr;

    void show() {                                 // the controls from the settings
        const Settings& s = settings();
        patchFont->value(s.patchFont.c_str());
        codeFont->value(s.codeFont.c_str());
        patchSize->value(s.patchSize);
        codeSize->value(s.codeSize);
        curved->value(s.curvedCables ? 1 : 0);
    }
    void changed() {                              // the settings from the controls, applied at once
        Settings& s = settings();
        s.patchFont = patchFont->value() ? patchFont->value() : "";
        s.codeFont = codeFont->value() ? codeFont->value() : "";
        s.patchSize = (int)patchSize->value();
        s.codeSize = (int)codeSize->value();
        s.curvedCables = curved->value() != 0;
        applySettings();
        saveSettings();
        App::get().settingsChanged();
    }
};

Dialog& dialog() {
    static Dialog d;
    return d;
}

Fl_Box* heading(int x, int y, const char* text) {
    auto* b = new Fl_Box(x, y, 300, 20, text);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    b->labelfont(FL_HELVETICA_BOLD);
    b->labelsize(13);
    b->labelcolor(kText);
    return b;
}

Fl_Input_Choice* fontChoice(int x, int y, int w, bool mono) {
    auto* c = new Fl_Input_Choice(x, y, w, 24, "Font");
    for (auto& f : fontSuggestions(mono)) c->add(f.c_str());
    c->labelcolor(kText);
    c->labelsize(12);
    c->tooltip("A font's name as the system knows it: choose one, or type another and press Enter");
    c->input()->when(FL_WHEN_ENTER_KEY_ALWAYS | FL_WHEN_RELEASE_ALWAYS);
    c->callback([](Fl_Widget*, void*) { dialog().changed(); });
    return c;
}

Fl_Spinner* sizeSpinner(int x, int y) {
    auto* s = new Fl_Spinner(x, y, 64, 24, "Size");
    s->range(8, 32);
    s->step(1);
    s->labelcolor(kText);
    s->labelsize(12);
    s->callback([](Fl_Widget*, void*) { dialog().changed(); });
    return s;
}

}  // namespace

Fl_Double_Window* settingsWindow() { return dialog().win; }

void showSettings() {
    Dialog& d = dialog();
    if (!d.win) {
        const int W = 440, H = 262;
        d.win = new Fl_Double_Window(W, H, "Settings");
        d.win->color(kPopup);
        heading(16, 12, "Patches");
        auto* t1 = new Fl_Box(16, 32, W - 32, 18, "the text of the boxes");
        t1->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        t1->labelsize(11);
        t1->labelcolor(kDim);
        d.patchFont = fontChoice(60, 54, 230, false);
        d.patchSize = sizeSpinner(350, 54);
        d.curved = new Fl_Check_Button(56, 84, 200, 22, "Curved cables");
        d.curved->labelcolor(kText);
        d.curved->labelsize(12);
        d.curved->callback([](Fl_Widget*, void*) { dialog().changed(); });
        heading(16, 120, "Code");
        auto* t2 = new Fl_Box(16, 140, W - 32, 18, "the code editor, the Console and the code view of patches");
        t2->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        t2->labelsize(11);
        t2->labelcolor(kDim);
        d.codeFont = fontChoice(60, 162, 230, true);
        d.codeSize = sizeSpinner(350, 162);
        auto* defaults = new Fl_Button(16, H - 42, 120, 26, "Defaults");
        defaults->callback([](Fl_Widget*, void*) {
            settings() = defaultSettings();
            dialog().show();
            dialog().changed();
        });
        auto* close = new Fl_Button(W - 106, H - 42, 90, 26, "Close");
        close->callback([](Fl_Widget*, void*) { dialog().win->hide(); });
        d.win->end();
        d.win->set_non_modal();
    }
    d.show();
    d.win->show();
}

}  // namespace moritz
