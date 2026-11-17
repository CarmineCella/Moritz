// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// browser.cpp — the browser: tutorials, examples by topic, objects and recent
// files in one window, shown at startup (Help > Browser)
//
// Examples describe themselves: the comment at the top of each file is its
// description, and a "; topic: name" line files it under a topic ("hidden"
// for the patches other examples use inside them). So the browser follows
// the examples folder with no list to maintain.

#include "gui.h"
#include "reference.h"

#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Preferences.H>
#include <FL/Fl_Tree.H>
#include <FL/filename.H>

#include <algorithm>
#include <cctype>
#include <map>

namespace moritz {

namespace {

// a category folder's title: "09-analysis-and-mir" -> "Analysis and MIR"
std::string folderTitle(std::string f) {
    size_t k = 0;
    while (k < f.size() && (std::isdigit((unsigned char)f[k]) || f[k] == '-' || f[k] == '_' || f[k] == ' ')) ++k;
    f = f.substr(k);
    std::string out, word;
    auto flush = [&] {
        if (word.empty()) return;
        std::string w = word;
        if (w == "mir" || w == "midi" || w == "fft" || w == "dsp") for (auto& c : w) c = (char)std::toupper((unsigned char)c);
        if (out.empty()) w[0] = (char)std::toupper((unsigned char)w[0]);
        out += (out.empty() ? "" : " ") + w;
        word.clear();
    };
    for (char c : f) {
        if (c == '-' || c == '_' || c == ' ') flush();
        else word += c;
    }
    flush();
    return out;
}

// the category folders of the examples, in order
std::vector<std::string> exampleFolders(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    dirent** list = nullptr;
    int n = fl_filename_list(dir.c_str(), &list, fl_casealphasort);
    for (int k = 0; k < n; ++k) {
        std::string f = list[k]->d_name;
        if (!f.empty() && f.back() == '/') f.pop_back();
        if (f.empty() || f[0] == '.') continue;
        if (std::string sub = dir + "/" + f; fl_filename_isdir(sub.c_str())) out.push_back(f);
    }
    fl_filename_free_list(&list, n);
    return out;
}

struct Entry {
    enum Kind { Tutorial, Example, Object, Recent } kind;
    std::string name, path, topic, text;      // path: a file, or the class for an object
};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<std::string> patchFiles(const std::string& dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
    dirent** list = nullptr;
    int n = fl_filename_list(dir.c_str(), &list, fl_casealphasort);
    for (int k = 0; k < n; ++k) {
        std::string f = list[k]->d_name;
        if (f.size() > 7 && f.compare(f.size() - 7, 7, ".moritz") == 0 && f.find(".help.") == std::string::npos)
            out.push_back(dir + "/" + f);
    }
    fl_filename_free_list(&list, n);
    return out;
}

// the comment at the top of a patch file: its description, and its topic
void readHeader(const std::string& path, std::string& text, std::string& topic) {
    std::string src;
    text.clear();
    topic.clear();
    if (!readFile(path, src)) return;
    size_t pos = 0;
    bool first = true;
    while (pos < src.size()) {
        size_t end = src.find('\n', pos);
        std::string line = src.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? src.size() : end + 1;
        if (line.empty() || line[0] != ';') break;
        line = line.substr(line.size() > 1 && line[1] == ' ' ? 2 : 1);
        if (line.rfind("topic:", 0) == 0) {
            topic = line.substr(6);
            topic.erase(0, topic.find_first_not_of(' '));
            continue;
        }
        if (first) {                               // "name.moritz - what it is"
            size_t dash = line.find(" - ");
            if (dash != std::string::npos && line.find(".moritz") < dash) line = line.substr(dash + 3);
            first = false;
        }
        text += (text.empty() || line.empty() ? "" : " ") + line;
    }
    // a capital, unless it starts with an object's name (sparkle~, nmf ...)
    std::string word = text.substr(0, text.find_first_of(" '"));
    bool object = word.find('~') != std::string::npos;
    for (auto& c : classInfos()) if (c.name == word) object = true;
    if (!text.empty() && !object) text[0] = (char)std::toupper((unsigned char)text[0]);
}

std::string displayName(const std::string& path) {
    std::string n = fl_filename_name(path.c_str());
    if (n.size() > 7) n.resize(n.size() - 7);
    return n;
}

class BrowserWindow : public Fl_Double_Window {
public:
    BrowserWindow() : Fl_Double_Window(860, 560, "Moritz Browser") {
        color(fl_rgb_color(30, 31, 35));
        auto* title = new Fl_Box(16, 10, 500, 28, "Moritz: tutorials, examples and objects");
        title->labelsize(16);
        title->labelfont(FL_HELVETICA_BOLD);
        title->labelcolor(fl_rgb_color(226, 228, 234));
        title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        search_ = new Fl_Input(600, 12, 244, 24);
        search_->tooltip("search tutorials, examples and objects");
        search_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
        search_->callback([](Fl_Widget*, void* d) {
            auto* b = static_cast<BrowserWindow*>(d);
            if (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) b->openFirst();
            else b->fill();
        }, this);
        auto* sl = new Fl_Box(530, 12, 66, 24, "Search");
        sl->labelsize(12);
        sl->labelcolor(fl_rgb_color(150, 152, 160));
        sl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        tree_ = new Fl_Tree(16, 46, 330, 466);
        tree_->showroot(0);
        tree_->color(fl_rgb_color(24, 25, 28));
        tree_->item_labelfgcolor(fl_rgb_color(214, 216, 222));
        tree_->item_labelbgcolor(fl_rgb_color(24, 25, 28));
        tree_->connectorcolor(fl_rgb_color(70, 72, 80));
        tree_->selection_color(fl_rgb_color(52, 92, 160));
        tree_->item_labelsize(13);
        tree_->selectmode(FL_TREE_SELECT_SINGLE);
        tree_->item_reselect_mode(FL_TREE_SELECTABLE_ALWAYS);
        tree_->callback([](Fl_Widget*, void* d) { static_cast<BrowserWindow*>(d)->picked(); }, this);
        text_ = new Fl_Text_Buffer();
        view_ = new Fl_Text_Display(360, 46, 484, 420);
        view_->buffer(text_);
        view_->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        view_->color(fl_rgb_color(24, 25, 28));
        view_->textcolor(fl_rgb_color(214, 216, 222));
        view_->textsize(13);
        view_->box(FL_FLAT_BOX);
        open_ = new Fl_Button(724, 476, 120, 30, "Open");
        open_->callback([](Fl_Widget*, void* d) { static_cast<BrowserWindow*>(d)->openSelected(); }, this);
        open_->deactivate();
        startup_ = new Fl_Check_Button(16, 520, 300, 26, "Show this window when Moritz starts");
        startup_->labelsize(12);
        startup_->labelcolor(fl_rgb_color(180, 182, 190));
        startup_->value(App::get().browserAtStartup() ? 1 : 0);
        startup_->callback([](Fl_Widget* w, void*) {
            App::get().setBrowserAtStartup(static_cast<Fl_Check_Button*>(w)->value() != 0);
        });
        auto* hint = new Fl_Box(360, 476, 350, 30, "Double-click to open; Help > Browser shows it again");
        hint->labelsize(11);
        hint->labelcolor(fl_rgb_color(130, 132, 140));
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        resizable(view_);
        end();
        collect();
        fill();
    }
    ~BrowserWindow() override {
        view_->buffer(nullptr);
        delete text_;
    }
    void refresh() { collect(); fill(); }

private:
    void collect() {
        entries_.clear();
        for (auto& p : patchFiles(resourceDir("tutorials"))) {
            Entry e{Entry::Tutorial, displayName(p), p, "", ""};
            std::string topic;
            readHeader(p, e.text, topic);
            for (auto& c : e.name) if (c == '-') c = ' ';
            entries_.push_back(e);
        }
        // the examples: a folder a category; a "; topic: hidden" header hides a
        // patch (the voices and parts other examples use)
        topics_.clear();
        std::string exdir = resourceDir("examples");
        auto addExamples = [&](const std::string& dir, const std::string& topic) {
            for (auto& p : patchFiles(dir)) {
                Entry e{Entry::Example, displayName(p), p, "", ""};
                std::string hidden;
                readHeader(p, e.text, hidden);
                if (hidden == "hidden") continue;
                e.topic = topic;
                entries_.push_back(e);
            }
        };
        for (auto& f : exampleFolders(exdir)) {
            topics_.push_back(folderTitle(f));
            addExamples(exdir + "/" + f, topics_.back());
        }
        topics_.push_back("More examples");
        addExamples(exdir, topics_.back());
        for (auto& c : classInfos())
            if (!c.group.empty()) entries_.push_back({Entry::Object, c.name, c.name, c.group, c.desc});
        for (auto& n : libraryNames()) {
            std::string path = findAbstraction(n, nullptr), text, topic;
            if (!path.empty()) readHeader(path, text, topic);
            if (topic == "hidden") continue;                     // the inside of another abstraction
            entries_.push_back({Entry::Object, n, n, "Library", text});
        }
        for (auto& p : App::get().recentFiles())
            entries_.push_back({Entry::Recent, fl_filename_name(p.c_str()), p, "", p});
    }
    void fill() {
        std::string q = lower(search_->value());
        auto match = [&](const Entry& e) {
            return q.empty() || lower(e.name).find(q) != std::string::npos || lower(e.text).find(q) != std::string::npos;
        };
        tree_->clear_children(tree_->root());     // clear() would delete the root too
        items_.clear();
        auto add = [&](Fl_Tree_Item* parent, const Entry& e) {
            Fl_Tree_Item* it = tree_->add(parent, e.name.c_str());
            it->user_data((void*)&e);
            items_.push_back(it);
        };
        auto section = [&](const char* label) { return tree_->add(tree_->root(), label); };
        Fl_Tree_Item* s = nullptr;
        // start here: the tutorials, in order
        for (auto& e : entries_)
            if (e.kind == Entry::Tutorial && match(e)) { if (!s) s = section("Start here: tutorials"); add(s, e); }
        // examples by category, in the folders' order
        Fl_Tree_Item* ex = nullptr;
        for (auto& t : topics_) {
            Fl_Tree_Item* ti = nullptr;
            for (auto& e : entries_)
                if (e.kind == Entry::Example && e.topic == t && match(e)) {
                    if (!ex) ex = section("Examples");
                    if (!ti) ti = tree_->add(ex, t.c_str());
                    add(ti, e);
                }
        }
        // objects by group
        Fl_Tree_Item* ob = nullptr;
        std::map<std::string, Fl_Tree_Item*> groups;
        for (auto& e : entries_)
            if (e.kind == Entry::Object && match(e)) {
                if (!ob) ob = section("Objects (open their help)");
                auto& g = groups[e.topic];
                if (!g) g = tree_->add(ob, e.topic.c_str());
                add(g, e);
            }
        Fl_Tree_Item* rc = nullptr;
        for (auto& e : entries_)
            if (e.kind == Entry::Recent && match(e)) { if (!rc) rc = section("Recent files"); add(rc, e); }
        // everything shown and open: the search only filters (the root too:
        // closing it would hide the whole tree)
        for (Fl_Tree_Item* it = tree_->first(); it; it = tree_->next(it))
            if (it->has_children()) it->open();
        tree_->hposition(0);
        tree_->vposition(0);
        tree_->redraw();
        text_->text(q.empty() ? welcome() : "");
        open_->deactivate();
    }
    static const char* welcome() {
        return "Welcome to Moritz.\n\n"
               "New here? Start with the tutorials, in order: each is a patch that explains itself as you "
               "play with it.\n\n"
               "The examples show what Moritz can do, by topic; open one, turn the audio on and play.\n\n"
               "Every object has a help patch: a small working example. Find objects here by name or by what "
               "they do (try \"reverb\", \"pitch\" or \"granular\"), or right-click any box in a patch.\n\n"
               "Click an entry to read about it; double-click to open it.";
    }
    const Entry* selected() const {
        Fl_Tree_Item* it = tree_->first_selected_item();
        return it ? static_cast<const Entry*>(it->user_data()) : nullptr;
    }
    void picked() {
        const Entry* e = selected();
        if (!e) { open_->deactivate(); return; }
        if (e->kind == Entry::Object) {                   // the object's reference
            std::string ref = referenceText(e->name);
            text_->text((e->topic + "\n\n" + ref + "\nOpen help shows a working example to play with.").c_str());
        } else {
            std::string body = e->text.empty() ? "(no description)" : e->text;
            text_->text((e->name + "\n\n" + body).c_str());
        }
        open_->label(e->kind == Entry::Object ? "Open help" : "Open");
        open_->activate();
        if (Fl::event_clicks() > 0 && tree_->callback_reason() != FL_TREE_REASON_DESELECTED) {
            Fl::event_clicks(0);
            openSelected();
        }
    }
    void openSelected() {
        const Entry* e = selected();
        if (!e) return;
        open(*e);
    }
    void openFirst() {
        for (Fl_Tree_Item* it : items_)
            if (it->visible_r()) { open(*static_cast<const Entry*>(it->user_data())); return; }
    }
    void open(const Entry& e) {
        Entry copy = e;                                    // opening may refresh the list
        if (copy.kind == Entry::Object) App::get().openHelp(copy.path);
        else App::get().openDocument(copy.path);
    }

    Fl_Input* search_;
    Fl_Tree* tree_;
    Fl_Text_Buffer* text_;
    Fl_Text_Display* view_;
    Fl_Button* open_;
    Fl_Check_Button* startup_;
    std::vector<Entry> entries_;
    std::vector<std::string> topics_;     // example categories, in order
    std::vector<Fl_Tree_Item*> items_;
};

BrowserWindow* gBrowser = nullptr;

Fl_Preferences& prefs() {
    static Fl_Preferences p(Fl_Preferences::USER_L, "moritz", "Moritz");
    return p;
}

}  // namespace

Fl_Preferences& appPrefs() { return prefs(); }

void App::showBrowser() {
    if (!gBrowser) gBrowser = new BrowserWindow();
    else gBrowser->refresh();
    if (MainWindow* d = frontDoc())
        gBrowser->position(d->x() + std::max(0, (d->w() - gBrowser->w()) / 2), d->y() + 40);
    gBrowser->show();
}

void App::hideBrowser() {
    if (gBrowser) gBrowser->hide();
}

bool App::browserAtStartup() {
    int v = 1;
    prefs().get("browser_at_startup", v, 1);
    return v != 0;
}

void App::setBrowserAtStartup(bool on) {
    prefs().set("browser_at_startup", on ? 1 : 0);
    prefs().flush();
}

std::vector<std::string> App::recentFiles() {
    std::vector<std::string> out;
    Fl_Preferences r(prefs(), "recent");
    for (int k = 0; k < 10; ++k) {
        char* v = nullptr;
        if (!r.get(std::to_string(k).c_str(), v, "") || !v) break;
        std::string s = v;
        free(v);
        if (s.empty()) break;
        if (fl_filename_isdir(s.c_str())) continue;
        out.push_back(s);
    }
    return out;
}

void App::addRecent(const std::string& path) {
    std::vector<std::string> list = recentFiles();
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > 10) list.resize(10);
    Fl_Preferences r(prefs(), "recent");
    for (size_t k = 0; k < 10; ++k) r.set(std::to_string(k).c_str(), k < list.size() ? list[k].c_str() : "");
    prefs().flush();
}

}  // namespace moritz
