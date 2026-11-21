// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// menus.cpp — the commands, and the menu bar built from them: one for all windows on macOS (at the top
// of the screen), one in each document window elsewhere
//
// Each command says where it is in the menus, its shortcut, and when it can be used. The enabled and
// checked states are refreshed when something they depend on changes (refreshMenus); the macOS menu
// bar, which is rebuilt as a whole by update(), only when one of them really changed.

#include "gui.h"
#include "roll.h"
#include "ui_util.h"
#include "settings.h"
#include "codewindow.h"
#include "datasets.h"
#include "sound_editor.h"
#include "score_window.h"
#include "arranger.h"
#include "timeline.h"

#include <FL/filename.H>
#ifdef __APPLE__
#include <FL/platform.H>
#endif

#include <cctype>
#include <deque>
#include <functional>

namespace moritz {

const char* commandKeyName() {
#ifdef __APPLE__
    return "Cmd";
#else
    return "Ctrl";
#endif
}

std::string shortcutText(int sc) {
    if (!sc) return "";
    int key = sc & FL_KEY_MASK;
    std::string k;
    if (key == '=') k = "+";                       // Zoom In: the key with + on it
    else if (key >= 33 && key < 127) k = std::string(1, (char)std::toupper(key));
    else if (key == FL_Enter) k = "Enter";
    else if (key == FL_Escape) k = "Esc";
    else if (key == FL_BackSpace) k = "Backspace";
    else if (key == FL_Delete) k = "Delete";
    std::string m;
#ifdef __APPLE__
    if (sc & FL_SHIFT) m += "Shift+";
    if (sc & FL_ALT) m += "Option+";
    if (sc & FL_COMMAND) m += "Cmd+";
    if (sc & FL_CTRL) m += "Ctrl+";
#else
    if (sc & (FL_COMMAND | FL_CTRL)) m += "Ctrl+";
    if (sc & FL_SHIFT) m += "Shift+";
    if (sc & FL_ALT) m += "Alt+";
#endif
    return m + k;
}

namespace {

// the text editor the Edit menu should act on: the focused one, or the
// editor of a text view that is the front window
Fl_Text_Editor* frontTextEditor() {
    // the focused editor only if its window is in front: a code window behind a help window can keep the
    // keyboard focus (X11 without a window manager moves it to no one), and Undo must not change its hidden text
    if (auto* ed = dynamic_cast<Fl_Text_Editor*>(Fl::focus()); ed && ed->top_window() == Fl::first_window()) return ed;
    if (auto* tw = dynamic_cast<PatchTextWindow*>(Fl::first_window())) return tw->editor();
    return nullptr;
}

App& app() { return App::get(); }

// what the commands act on
DocumentWindow* doc() { return app().targetDoc(); }
bool haveDoc() { return doc() != nullptr; }
SoundEditor* sound() { return app().frontSound() ? app().frontSound()->editor() : nullptr; }
// a score's window in front (score_window.h): its timeline, when it shows it; its roll, when it shows the roll (and
// its text is not what the keys go to)
TimelinePanel* timeline() {
    ScoreWindow* w = frontScore();
    return w && w->view() == ScoreView::Timeline ? w->timeline() : nullptr;
}
RollPanel* roll() {
    ScoreWindow* w = frontScore();
    return w && w->view() != ScoreView::Timeline && !frontTextEditor() ? w->roll() : nullptr;
}
bool textOrDoc() { return sound() || timeline() || frontTextEditor() || roll() || haveDoc(); }
bool canvasOrText() { return sound() || timeline() || frontTextEditor() || roll() || app().activeCanvas(); }
bool editableCanvas() {
    return sound() || timeline() || frontTextEditor() || roll() || (app().activeCanvas() && !app().activeCanvas()->readOnly());
}

// a timeline in front: on its clips; a sound window: on its sound; a text: on its text; else on the canvas
void editText(int (*kf)(int, Fl_Text_Editor*), void (Canvas::*onCanvas)(), void (SoundEditor::*onSound)(),
              void (TimelinePanel::*onTimeline)() = nullptr) {
    if (auto* t = timeline()) { if (onTimeline) (t->*onTimeline)(); return; }
    if (auto* s = sound()) { (s->*onSound)(); return; }
    if (auto* ed = frontTextEditor()) { kf(0, ed); return; }
    if (auto* r = roll()) {                       // the roll: Select All and Delete on its notes
        if (onCanvas == &Canvas::selectAll) r->core().selectAll();
        else if (onCanvas == &Canvas::deleteSelection) r->core().removeSelection();
        r->changed();
        return;
    }
    if (auto* c = app().activeCanvas()) (c->*onCanvas)();
}

const std::vector<Command>& table() {
    static const std::vector<Command> t = {
        // ---- File
        {"file.new", "File/New", FL_COMMAND + 'n', 0, [] { app().newDocument(); }, nullptr, nullptr},
        {"file.newcode", "File/New Code", FL_COMMAND + FL_SHIFT + 'n', 0, [] { app().newCode(); }, nullptr, nullptr},
        {"file.open", "File/Open...", FL_COMMAND + 'o', 0, [] { app().openDialog(); }, nullptr, nullptr},
        {"file.opensound", "File/Open Sound...", FL_COMMAND + FL_SHIFT + 'o', 0, [] { app().openSoundDialog(); },
         nullptr, nullptr},
        {"file.newscore", "File/New Score", 0, 0, [] { newScore(); }, nullptr, nullptr},
        {"file.openscore", "File/Open Score...", 0, 0, [] { openScoreDialog(); }, nullptr, nullptr},
        {"file.newsession", "File/New Session", 0, 0, [] { newSession(); }, nullptr, nullptr},
        {"file.opensession", "File/Open Session...", 0, 0, [] { openSessionDialog(); }, nullptr, nullptr},
        {"@recent", "File/Open Recent", 0, 0, nullptr, nullptr, nullptr},
        {"file.import", "File/Import from Max...", FL_COMMAND + FL_SHIFT + 'i', FL_MENU_DIVIDER,
         [] { app().importDialog(); }, nullptr, nullptr},
        {"file.save", "File/Save", FL_COMMAND + 's', 0,
         [] {
             if (auto* t = frontScore()) { t->save(); return; }
             if (auto* s = app().frontSound()) { s->save(); return; }
             if (auto* c = app().frontCode()) { c->save(); return; }
             if (auto* d = doc()) d->saveFront();
         },
         [] { return frontScore() || app().frontSound() || app().frontCode() != nullptr || haveDoc(); }, nullptr},
        {"file.saveas", "File/Save As...", FL_COMMAND + FL_SHIFT + 's', 0,
         [] {
             if (auto* t = frontScore()) { t->saveAs(); return; }
             if (auto* s = app().frontSound()) { s->saveAs(); return; }
             if (auto* c = app().frontCode()) { c->saveAs(); return; }
             if (auto* d = doc()) d->saveAs();
         },
         [] { return frontScore() || app().frontSound() || app().frontCode() != nullptr || haveDoc(); }, nullptr},
        {"file.revert", "File/Revert to Saved", 0, FL_MENU_DIVIDER, [] { if (auto* d = doc()) d->revert(); },
         [] {
             auto* d = app().frontCode() || app().frontSound() || frontScore() ? nullptr : doc();
             return d && !d->path().empty() && d->doc.anyDirty();
         },
         nullptr},
        {"file.close", "File/Close", FL_COMMAND + 'w', FL_MENU_DIVIDER, [] { app().closeFront(); }, nullptr, nullptr},
        {"file.quit", "File/Quit", FL_COMMAND + 'q', 0, [] { app().quit(); }, nullptr, nullptr},
        // ---- Edit: with a text in front, on its text
        {"edit.undo", "Edit/Undo", FL_COMMAND + 'z', 0,
         [] {
             if (auto* t = timeline()) { t->undo(); return; }
             if (auto* s = sound()) { s->undo(); return; }
             if (auto* ed = frontTextEditor()) { Fl_Text_Editor::kf_undo(0, ed); return; }
             if (auto* r = roll()) { r->undo(); return; }
             if (auto* c = app().activeCanvas(); c && c->uiUndo(false)) return;   // a score box clicked last: its score's
             if (auto* d = doc()) d->undo();
         },
         textOrDoc, nullptr},
        {"edit.redo", "Edit/Redo", FL_COMMAND + FL_SHIFT + 'z', FL_MENU_DIVIDER,
         [] {
             if (auto* t = timeline()) { t->redo(); return; }
             if (auto* s = sound()) { s->redo(); return; }
             if (auto* ed = frontTextEditor()) { Fl_Text_Editor::kf_redo(0, ed); return; }
             if (auto* r = roll()) { r->redo(); return; }
             if (auto* c = app().activeCanvas(); c && c->uiUndo(true)) return;   // a score box clicked last: its score's
             if (auto* d = doc()) d->redo();
         },
         textOrDoc, nullptr},
        {"edit.cut", "Edit/Cut", FL_COMMAND + 'x', 0, [] { editText(Fl_Text_Editor::kf_cut, &Canvas::cutSelection, &SoundEditor::cut, &TimelinePanel::cut); },
         editableCanvas, nullptr},
        {"edit.copy", "Edit/Copy", FL_COMMAND + 'c', 0, [] { editText(Fl_Text_Editor::kf_copy, &Canvas::copySelection, &SoundEditor::copy, &TimelinePanel::copy); },
         canvasOrText, nullptr},
        {"edit.paste", "Edit/Paste", FL_COMMAND + 'v', 0, [] { editText(Fl_Text_Editor::kf_paste, &Canvas::paste, &SoundEditor::paste, &TimelinePanel::paste); },
         editableCanvas, nullptr},
        {"edit.duplicate", "Edit/Duplicate", FL_COMMAND + 'd', 0,
         [] {
             if (auto* t = timeline()) { t->duplicate(); return; }
             if (auto* c = app().activeCanvas()) c->duplicateSelection(15, 15);
         },
         [] { return timeline() || (app().activeCanvas() && !app().activeCanvas()->readOnly()); }, nullptr},
        // no shortcut: Backspace and Delete are the canvas's own (a menu would take them from text fields)
        {"edit.delete", "Edit/Delete", 0, FL_MENU_DIVIDER,
         [] { editText(Fl_Text_Editor::kf_delete, &Canvas::deleteSelection, &SoundEditor::erase, &TimelinePanel::erase); }, editableCanvas, nullptr},
        {"edit.selectall", "Edit/Select All", FL_COMMAND + 'a', 0,
         [] { editText(Fl_Text_Editor::kf_select_all, &Canvas::selectAll, &SoundEditor::selectAll, &TimelinePanel::selectAll); }, canvasOrText, nullptr},
#ifndef __APPLE__
        {"edit.settings", "Edit/Settings...", FL_COMMAND + ',', FL_MENU_DIVIDER, [] { showSettings(); }, nullptr,
         nullptr},                               // (on macOS: Moritz > Settings…)
#endif
        // ---- Patch
        {"@new", "Patch/New", 0, 0, nullptr, nullptr, nullptr},
        {"patch.edit", "Patch/Edit Mode", FL_COMMAND + 'e', FL_MENU_TOGGLE, [] { if (auto* d = doc()) d->toggleLock(); },
         haveDoc, [] { auto* c = app().activeCanvas(); return c && !c->locked(); }},
        {"patch.code", "Patch/Code View", FL_COMMAND + 't', 0, [] { app().showText(); }, nullptr, nullptr},
        {"patch.audio", "Patch/Audio On", FL_COMMAND + '/', FL_MENU_TOGGLE | FL_MENU_DIVIDER,
         [] { app().toggleAudio(); }, nullptr, [] { return app().audio.running(); }},
        {"patch.audiosetup", "Patch/Audio Setup...", 0, 0, [] { app().audioSetup(); }, nullptr, nullptr},
        {"patch.midisetup", "Patch/MIDI Setup...", 0, FL_MENU_DIVIDER, [] { app().midiSetup(); }, nullptr, nullptr},
        {"patch.console", "Patch/Console", FL_COMMAND + FL_SHIFT + 'm', 0, [] { app().showConsole(); }, nullptr, nullptr},
        // ---- View
        {"view.zoomin", "View/Zoom In", FL_COMMAND + '=', 0, [] { app().zoomFront(1); }, nullptr, nullptr},
        {"view.zoomout", "View/Zoom Out", FL_COMMAND + '-', 0, [] { app().zoomFront(-1); }, nullptr, nullptr},
        {"view.actual", "View/Actual Size", FL_COMMAND + '0', FL_MENU_DIVIDER, [] { app().zoomFront(0); }, nullptr,
         nullptr},
        {"view.larger", "View/Interface Larger", 0, 0, [] { app().scaleInterface(1.1f); }, nullptr, nullptr},
        {"view.smaller", "View/Interface Smaller", 0, 0, [] { app().scaleInterface(1 / 1.1f); }, nullptr, nullptr},
        {"view.normal", "View/Interface Normal Size", 0, 0, [] { app().scaleInterface(0); }, nullptr, nullptr},
        // ---- Extras
        {"extras.recorder", "Extras/Quick Recorder...", FL_COMMAND + FL_SHIFT + 'r', 0, [] { app().showRecorder(); },
         nullptr, nullptr},
        {"extras.record", "Extras/Start or Stop Recording", FL_COMMAND + FL_ALT + 'r', FL_MENU_DIVIDER,
         [] { app().toggleRecording(); }, nullptr, nullptr},
        {"extras.monitor", "Extras/Binaural Monitor...", 0, 0, [] { app().showMonitor(); }, nullptr, nullptr},
        {"extras.tester", "Extras/Channel Tester...", 0, 0, [] { app().showTester(); }, nullptr, nullptr},
        {"extras.datasets", "Extras/Datasets...", 0, 0, [] { showDatasets(); }, nullptr, nullptr},
        // ---- Help
        {"help.browser", "Help/Browser...", FL_COMMAND + FL_SHIFT + 'b', FL_MENU_DIVIDER, [] { app().showBrowser(); },
         nullptr, nullptr},
        {"@documents", "Help/Documents", 0, 0, nullptr, nullptr, nullptr},
        {"@tutorials", "Help/Tutorials", 0, 0, nullptr, nullptr, nullptr},
#ifndef __APPLE__
        {"help.about", "Help/About Moritz", 0, 0, [] { app().about(); }, nullptr, nullptr},
#endif
    };
    return t;
}

void runCommand(Fl_Widget*, void* d) {
    auto* c = static_cast<const Command*>(d);
    if (c->run && (!c->enabled || c->enabled())) c->run();
    App::get().refreshMenus();
}

struct NewItem {
    std::string cls;
};

std::deque<std::string>& recentPaths() {    // what File > Open Recent's items open
    static std::deque<std::string> v;
    return v;
}

void addRecentItems(Fl_Menu_Bar* m) {
    auto& paths = recentPaths();
    if (paths.empty()) {
        m->add("File/Open Recent/No Recent Files", 0, nullptr, nullptr, FL_MENU_INACTIVE);
        return;
    }
    for (auto& p : paths) {
        std::string dir = baseName(dirOf(p));
        std::string label = escapeMenu(baseName(p) + (dir.empty() || dir == "." ? "" : "   (" + dir + ")"));
        m->add(("File/Open Recent/" + label).c_str(), 0,
               [](Fl_Widget*, void* d) { App::get().openDocument(*static_cast<std::string*>(d)); }, &p);
    }
}

}  // namespace

const std::vector<Command>& commands() { return table(); }

const Command* findCommand(const std::string& id) {
    for (auto& c : table())
        if (id == c.id) return &c;
    return nullptr;
}

std::string withShortcut(const std::string& text, const std::string& id) {
    const Command* c = findCommand(id);
    if (!c || !c->shortcut) return text;
    return text + " (" + shortcutText(c->shortcut) + ")";
}

Fl_Menu_Bar* App::menuFor(DocumentWindow* w) {
    if (sysMenu_) return sysMenu_;
    return w ? w->menuBar() : nullptr;
}

void App::forgetMenu(Fl_Menu_Bar* m) {
    menus_.erase(std::remove_if(menus_.begin(), menus_.end(), [m](const MenuState& s) { return s.bar == m; }),
                 menus_.end());
}

void App::buildMenu(Fl_Menu_Bar* m) {
    static std::vector<std::unique_ptr<NewItem>> items;
    auto addNew = [&](const std::string& path, const std::string& cls) {
        items.push_back(std::make_unique<NewItem>(NewItem{cls}));
        m->add(path.c_str(), 0,
               [](Fl_Widget*, void* d) {
                   if (auto* c = App::get().activeCanvas()) c->newBoxAtMouse(static_cast<NewItem*>(d)->cls);
               },
               items.back().get());
    };
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
    // Help > Documents: the PDFs in the docs folder (and its folders), and Musil's manual
    static std::vector<std::string> documents;
    static bool scanned = false;
    if (!scanned) {
        scanned = true;
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
        if (std::string d = resourceDir("musil"); !d.empty())       // and Musil's manual, from Musil's own folder
            if (std::string f = d + "/docs/musil_manual.pdf"; fl_access(f.c_str(), 0) == 0) documents.push_back(f);
    }
    if (recentPaths().empty()) for (auto& p : recentFiles()) recentPaths().push_back(p);

    for (auto& c : table()) {
        std::string id = c.id;
        if (id == "@recent") {
            addRecentItems(m);
        } else if (id == "@new") {
            for (auto& ci : classInfos())
                if (!ci.group.empty())                            // not the short aliases
                    addNew("Patch/New/" + ci.group + "/" + escapeMenu(ci.name), ci.name);
            for (auto& name : libraryNames())                    // abstractions in the libraries
                addNew("Patch/New/Library/" + escapeMenu(name), name);
        } else if (id == "@documents") {
            for (auto& path : documents) {
                std::string name = baseName(path);
                name = name.substr(0, name.size() - 4);
                for (auto& ch : name) if (ch == '-' || ch == '_') ch = ' ';
                if (!name.empty()) name[0] = (char)std::toupper((unsigned char)name[0]);
                m->add(("Help/Documents/" + escapeMenu(name)).c_str(), 0,
                       [](Fl_Widget*, void* d) { openWithSystem(*static_cast<std::string*>(d)); }, &path);
            }
            if (documents.empty()) m->add("Help/Documents", 0, nullptr, nullptr, FL_MENU_INACTIVE);
        } else if (id == "@tutorials") {
            for (auto& path : tutorials) {
                std::string name = baseName(path);
                name = name.substr(0, name.size() - 7);             // "01-first-patch" -> "01 first patch"
                for (auto& ch : name) if (ch == '-') ch = ' ';
                m->add(("Help/Tutorials/" + escapeMenu(name)).c_str(), 0,
                       [](Fl_Widget*, void* d) { App::get().openDocument(*static_cast<std::string*>(d)); },
                       &path);
            }
            if (tutorials.empty()) m->add("Help/Tutorials", 0, nullptr, nullptr, FL_MENU_INACTIVE);
        } else {
            m->add(c.path, c.shortcut, runCommand, (void*)&c, c.flags);
        }
    }
    MenuState s{m, {}, {}};
    indexMenu(s);
    menus_.push_back(std::move(s));
}

void App::indexMenu(MenuState& s) {
    auto& t = table();
    s.index.assign(t.size(), -1);
    s.flags.assign(t.size(), -1);
    for (size_t k = 0; k < t.size(); ++k)
        if (t[k].run && (t[k].enabled || t[k].checked)) s.index[k] = s.bar->find_index(t[k].path);
}

void App::refreshMenus() {
    auto& t = table();
    // the states once for all bars
    std::vector<int> want(t.size(), 0);
    for (size_t k = 0; k < t.size(); ++k) {
        if (!t[k].run || (!t[k].enabled && !t[k].checked)) continue;
        if (t[k].enabled && !t[k].enabled()) want[k] |= FL_MENU_INACTIVE;
        if (t[k].checked && t[k].checked()) want[k] |= FL_MENU_VALUE;
    }
    for (auto& s : menus_) {
        bool changed = false;
        for (size_t k = 0; k < t.size(); ++k) {
            int i = s.index[k];
            if (i < 0 || i >= s.bar->size()) continue;
            auto* item = const_cast<Fl_Menu_Item*>(s.bar->menu() + i);
            int f = (item->flags & ~(FL_MENU_INACTIVE | FL_MENU_VALUE)) | want[k];
            if (f != item->flags) {
                item->flags = f;
                changed = true;
            }
        }
        if (changed) s.bar->update();   // the macOS bar is rebuilt: only when something changed
    }
}

namespace {
void refreshLater(void*) { App::get().refreshMenus(); }

int dispatch(int e, Fl_Window* w) {
    // a key: the states as they are now, before the menus look for its shortcut
    if (e == FL_KEYBOARD || e == FL_SHORTCUT) App::get().refreshMenus();
    int r = Fl::handle_(e, w);
    if (e == FL_FOCUS || e == FL_SHOW || e == FL_PUSH) {
        App::get().refreshMenus();
        // and once the system has finished bringing the window to the front (on macOS the window
        // becomes the first one after it has the focus)
        Fl::remove_timeout(refreshLater);
        Fl::add_timeout(0.0, refreshLater);
    }
    return r;
}
}  // namespace

void App::followFrontWindow() { Fl::event_dispatch(dispatch); }

bool App::menuItemActive(const std::string& id) {
    auto& t = table();
    for (size_t k = 0; k < t.size(); ++k)
        if (id == t[k].id)
            for (auto& s : menus_) {
                int i = s.index[k];
                if (i >= 0 && i < s.bar->size()) return !(s.bar->menu()[i].flags & FL_MENU_INACTIVE);
            }
    return false;
}

void App::rebuildRecentMenus() {
    auto& paths = recentPaths();
    paths.clear();
    for (auto& p : recentFiles()) paths.push_back(p);
    for (auto& s : menus_) {
        int i = s.bar->find_index("File/Open Recent");
        if (i < 0) continue;
        s.bar->clear_submenu(i);
        addRecentItems(s.bar);
        indexMenu(s);
        s.bar->update();
    }
    refreshMenus();
}

}  // namespace moritz
