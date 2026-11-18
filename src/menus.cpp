// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// menus.cpp — the menu bar: one for all windows on macOS, one per document window elsewhere

#include "gui.h"
#include "ui_util.h"
#include "settings.h"
#include "codewindow.h"

#include <FL/filename.H>
#ifdef __APPLE__
#include <FL/platform.H>
#endif

#include <functional>

namespace moritz {

void setMenuCheck(Fl_Menu_Bar* m, const char* path, bool on) {
    if (!m) return;
    auto it = const_cast<Fl_Menu_Item*>(m->find_item(path));
    if (!it || (it->value() != 0) == on) return;   // the macOS bar is rebuilt by update(): only on a change
    if (on) it->set();
    else it->clear();
    m->update();
}

Fl_Menu_Bar* App::menuFor(MainWindow* w) {
    if (sysMenu_) return w == frontDoc() ? sysMenu_ : nullptr;
    return w ? w->menu_ : nullptr;
}

namespace {
// the text editor the Edit menu should act on: the focused one, or the
// editor of a text view that is the front window
Fl_Text_Editor* frontTextEditor() {
    if (auto* ed = dynamic_cast<Fl_Text_Editor*>(Fl::focus())) return ed;
    if (auto* tw = dynamic_cast<PatchTextWindow*>(Fl::first_window())) return tw->editor();
    return nullptr;
}

struct NewItem {
    std::string cls;
};
}  // namespace

void App::buildMenu(Fl_Menu_Bar* m) {
#define APP_CB(method) [](Fl_Widget*, void*) { App::get().method(); }
#define DOC_CB(method) [](Fl_Widget*, void*) { if (auto* d = App::get().frontDoc()) d->method(); }
    m->add("File/New", FL_COMMAND + 'n', APP_CB(newDocument), nullptr);
    m->add("File/New Code", FL_COMMAND + FL_SHIFT + 'n', [](Fl_Widget*, void*) { App::get().newCode(); }, nullptr);
    m->add("File/Open...", FL_COMMAND + 'o', APP_CB(openDialog), nullptr);
    m->add("File/Import from Max...", FL_COMMAND + FL_SHIFT + 'i', APP_CB(importDialog), nullptr,
           FL_MENU_DIVIDER);
    m->add("File/Save", FL_COMMAND + 's', [](Fl_Widget*, void*) {
        if (auto* c = App::get().frontCode()) { c->save(); return; }
        if (auto* d = App::get().frontDoc()) d->saveFront();
    }, nullptr);
    m->add("File/Close", FL_COMMAND + 'w', APP_CB(closeFront), nullptr);
    m->add("File/Save as...", FL_COMMAND + FL_SHIFT + 's', [](Fl_Widget*, void*) {
        if (auto* c = App::get().frontCode()) { c->saveAs(); return; }
        if (auto* d = App::get().frontDoc()) d->saveAs();
    }, nullptr, FL_MENU_DIVIDER);
    m->add("File/Quit", FL_COMMAND + 'q', APP_CB(quit), nullptr);
    // with a text view in front, the Edit commands work on its text
#define TEXT_OR(kf, call) [](Fl_Widget*, void*) {                                  \
        if (auto* ed = frontTextEditor()) { Fl_Text_Editor::kf(0, ed); return; } \
        call; }
    m->add("Edit/Undo", FL_COMMAND + 'z',
           TEXT_OR(kf_undo, if (auto* d = App::get().frontDoc()) d->undo()), nullptr);
    m->add("Edit/Redo", FL_COMMAND + FL_SHIFT + 'z',
           TEXT_OR(kf_redo, if (auto* d = App::get().frontDoc()) d->redo()), nullptr, FL_MENU_DIVIDER);
    m->add("Edit/Cut", FL_COMMAND + 'x',
           TEXT_OR(kf_cut, if (auto* c = App::get().activeCanvas()) c->cutSelection()), nullptr);
    m->add("Edit/Copy", FL_COMMAND + 'c',
           TEXT_OR(kf_copy, if (auto* c = App::get().activeCanvas()) c->copySelection()), nullptr);
    m->add("Edit/Paste", FL_COMMAND + 'v',
           TEXT_OR(kf_paste, if (auto* c = App::get().activeCanvas()) c->paste()), nullptr);
    m->add("Edit/Duplicate", FL_COMMAND + 'd',
           [](Fl_Widget*, void*) { if (auto* c = App::get().activeCanvas()) c->duplicateSelection(15, 15); },
           nullptr, FL_MENU_DIVIDER);
    m->add("Edit/Select All", FL_COMMAND + 'a',
           TEXT_OR(kf_select_all, if (auto* c = App::get().activeCanvas()) c->selectAll()), nullptr);
#ifndef __APPLE__
    m->add("Edit/Settings...", FL_COMMAND + ',', [](Fl_Widget*, void*) { showSettings(); }, nullptr,
           FL_MENU_DIVIDER);       // (on macOS: Moritz > Settings…)
#endif
#undef TEXT_OR

    static std::vector<std::unique_ptr<NewItem>> items;
    auto addNew = [&](const std::string& path, const std::string& cls) {
        items.push_back(std::make_unique<NewItem>(NewItem{cls}));
        m->add(path.c_str(), 0,
               [](Fl_Widget*, void* d) {
                   if (auto* c = App::get().activeCanvas()) c->newBoxAtMouse(static_cast<NewItem*>(d)->cls);
               },
               items.back().get());
    };
    for (auto& c : classInfos())
        if (!c.group.empty())                            // not the short aliases
            addNew("Patch/New/" + c.group + "/" + escapeMenu(c.name), c.name);
    for (auto& name : libraryNames())                    // abstractions in the libraries
        addNew("Patch/New/Library/" + escapeMenu(name), name);
    m->add("Patch/Edit Mode", FL_COMMAND + 'e', DOC_CB(toggleLock), nullptr, FL_MENU_TOGGLE);
    m->add("Patch/Code View", FL_COMMAND + 't', [](Fl_Widget*, void*) { App::get().showText(); }, nullptr);
    m->add("Patch/Audio On", FL_COMMAND + '/', APP_CB(toggleAudio), nullptr,
           FL_MENU_TOGGLE | FL_MENU_DIVIDER);
    m->add("Patch/Audio Setup...", 0, APP_CB(audioSetup), nullptr);
    m->add("Patch/MIDI Setup...", 0, APP_CB(midiSetup), nullptr, FL_MENU_DIVIDER);
    m->add("Patch/Console", FL_COMMAND + FL_SHIFT + 'm', APP_CB(showConsole), nullptr);
    m->add("View/Zoom In", FL_COMMAND + '=', [](Fl_Widget*, void*) { App::get().zoomFront(1); }, nullptr);
    m->add("View/Zoom Out", FL_COMMAND + '-', [](Fl_Widget*, void*) { App::get().zoomFront(-1); }, nullptr);
    m->add("View/Actual Size", FL_COMMAND + '0', [](Fl_Widget*, void*) { App::get().zoomFront(0); }, nullptr,
           FL_MENU_DIVIDER);
    m->add("View/Interface Larger", 0, [](Fl_Widget*, void*) { App::get().scaleInterface(1.1f); }, nullptr);
    m->add("View/Interface Smaller", 0, [](Fl_Widget*, void*) { App::get().scaleInterface(1 / 1.1f); }, nullptr);
    m->add("View/Interface Normal Size", 0, [](Fl_Widget*, void*) { App::get().scaleInterface(0); }, nullptr);
    m->add("Extras/Quick Recorder...", FL_COMMAND + FL_SHIFT + 'r', APP_CB(showRecorder), nullptr);
    m->add("Extras/Start or Stop Recording", FL_COMMAND + FL_ALT + 'r', APP_CB(toggleRecording), nullptr,
           FL_MENU_DIVIDER);
    m->add("Extras/Binaural Monitor...", 0, APP_CB(showMonitor), nullptr);
    m->add("Extras/Channel Tester...", 0, APP_CB(showTester), nullptr);
    // Help > Tutorials: the numbered patches in the tutorials folder
    static std::vector<std::string> tutorials;
    if (tutorials.empty())
        if (std::string dir = resourceDir("tutorials"); !dir.empty()) {
            dirent** list = nullptr;
            int n = fl_filename_list(dir.c_str(), &list, fl_casealphasort);
            for (int k = 0; k < n; ++k) {
                std::string f = list[k]->d_name;
                if (f.size() > 7 && f.compare(f.size() - 7, 7, ".moritz") == 0) tutorials.push_back(dir + "/" + f);
            }
            fl_filename_free_list(&list, n);
        }
    m->add("Help/Browser...", FL_COMMAND + FL_SHIFT + 'b', APP_CB(showBrowser), nullptr, FL_MENU_DIVIDER);
    // Help > Documents: the PDFs in the docs folder (and its folders), and Musil's manual
    static std::vector<std::string> documents;
    if (documents.empty())
        if (std::string dir = resourceDir("docs"); !dir.empty()) {
            std::function<void(const std::string&, int)> scan = [&](const std::string& d, int depth) {
                dirent** list = nullptr;
                int n = fl_filename_list(d.c_str(), &list, fl_casealphasort);
                for (int k = 0; k < n; ++k) {
                    std::string f = list[k]->d_name;
                    if (!f.empty() && f.back() == '/') f.pop_back();
                    if (f.empty() || f[0] == '.') continue;
                    std::string full = d + "/" + f;
                    if (fl_filename_isdir(full.c_str())) { if (depth < 2) scan(full, depth + 1); }
                    else if (f.size() > 4 && (f.compare(f.size() - 4, 4, ".pdf") == 0 || f.compare(f.size() - 4, 4, ".PDF") == 0))
                        documents.push_back(full);
                }
                fl_filename_free_list(&list, n);
            };
            scan(dir, 0);
        }
    if (static bool musil = false; !musil) {            // and Musil's manual, from Musil's own folder
        musil = true;
        if (std::string d = resourceDir("musil"); !d.empty())
            if (std::string f = d + "/docs/musil_manual.pdf"; fl_access(f.c_str(), 0) == 0) documents.push_back(f);
    }
    for (auto& path : documents) {
        std::string name = baseName(path);
        name = name.substr(0, name.size() - 4);
        for (auto& c : name) if (c == '-' || c == '_') c = ' ';
        if (!name.empty()) name[0] = (char)std::toupper((unsigned char)name[0]);
        m->add(("Help/Documents/" + escapeMenu(name)).c_str(), 0,
               [](Fl_Widget*, void* d) { openWithSystem(*static_cast<std::string*>(d)); }, &path);
    }
    if (documents.empty()) m->add("Help/Documents", 0, nullptr, nullptr, FL_MENU_INACTIVE);
    for (auto& path : tutorials) {
        std::string name = baseName(path);
        name = name.substr(0, name.size() - 7);             // "01-first-patch" -> "01 first patch"
        for (auto& c : name) if (c == '-') c = ' ';
        m->add(("Help/Tutorials/" + escapeMenu(name)).c_str(), 0,
               [](Fl_Widget*, void* d) { App::get().openDocument(*static_cast<std::string*>(d)); },
               &path);
    }
    if (tutorials.empty()) m->add("Help/Tutorials", 0, nullptr, nullptr, FL_MENU_INACTIVE);
#ifndef __APPLE__
    m->add("Help/About Moritz", 0, APP_CB(about), nullptr);
#endif
#undef APP_CB
#undef DOC_CB
}

}  // namespace moritz
