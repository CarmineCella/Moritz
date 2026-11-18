// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// ui_util.h — small things the windows of the application share (not a public interface)
#pragma once

#include "gui.h"
#include "theme.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/filename.H>

#include <algorithm>
#include <string>

namespace moritz {

#ifdef __APPLE__
constexpr int kMenuH = 0;        // the macOS menu bar lives at the top of the screen
#else
constexpr int kMenuH = 25;
#endif
constexpr int kStatusH = 22;
constexpr int kToolH = 34;
constexpr const char* kExt = "moritz";

// a text as a menu label: '/', '\\', '_' and '&' mean something there
inline std::string escapeMenu(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == '_') r += '\\';
        if (c == '&') r += '&';
        r += c;
    }
    return r;
}

inline std::string baseName(const std::string& path) {
    const char* n = fl_filename_name(path.c_str());
    return n ? n : path;
}

// Centres a window on the usable area of the screen showing `ref` (or the
// main screen), shifted by dx, dy.
inline void centerOnScreen(Fl_Window* w, Fl_Window* ref = nullptr, int dx = 0, int dy = 0) {
    int X, Y, W, H;
    int n = ref ? Fl::screen_num(ref->x() + ref->w() / 2, ref->y() + ref->h() / 2) : 0;
    Fl::screen_work_area(X, Y, W, H, n);
    w->position(X + std::max(0, (W - w->w()) / 2) + dx, Y + std::max(0, (H - w->h()) / 2) + dy);
}

std::string exeDir();                       // the folder of the program (resources.cpp)
int eggHandler(int e);                      // Shift+Cmd+J (dialogs.cpp)

// Size of a window that shows all of a patch's boxes.
inline void fitPatch(Patch* p, int& W, int& H) {
    W = 420;
    H = 300;
    layoutPatch(p);                       // as the canvas will draw them
    for (auto& x : p->boxes) {
        int bw = x->w + 8;
        W = std::max(W, x->x + bw + 40);
        H = std::max(H, x->y + x->h + 50);
    }
    W = std::min(W, 1400);
    H = std::min(H + 22, 900);
}

inline std::string patchTitle(Patch* p) {
    std::string t = p->owner ? p->owner->label() : "patch";
    if (p->isAbstraction) t += "  (" + baseName(p->file) + ")";
    return t;
}

// path of box ids from the top patch, e.g. "p1/fm2", to reopen after undo
inline std::string pathOf(Patch* p) {
    std::string r;
    for (; p && p->owner; p = p->parent) r = p->owner->id + (r.empty() ? "" : "/" + r);
    return r;
}

}  // namespace moritz
