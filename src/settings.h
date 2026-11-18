// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// settings.h — the user's settings (Moritz > Settings, or Edit > Settings): the font of the patches'
// boxes and its size, the font of code (code editor, Console, code view) and its size, and the shape
// of the cables. Kept in the user's preferences; applied at once to every open window.
#pragma once

#include <FL/Enumerations.H>

#include <string>
#include <vector>

class Fl_Double_Window;

namespace moritz {

// the font slots the settings name: every box's text, and every view of code
constexpr Fl_Font kPatchFontSlot = (Fl_Font)(FL_FREE_FONT + 8);
constexpr Fl_Font kCodeFontSlot = (Fl_Font)(FL_FREE_FONT + 7);

struct Settings {
    std::string patchFont;         // a font's name, as the system knows it
    int patchSize = 12;
    std::string codeFont;
    int codeSize = 13;
    bool curvedCables = true;
};

Settings& settings();
Settings defaultSettings();
std::vector<std::string> fontSuggestions(bool monospaced);   // fonts most systems of this kind have
void loadSettings();               // from the preferences, then applySettings()
void saveSettings();
void applySettings();              // fonts and sizes into the slots and the theme (no redraw)
void showSettings();               // the dialog
Fl_Double_Window* settingsWindow();   // the dialog's window (null before it is shown)

}  // namespace moritz
