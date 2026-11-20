// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// datasets_ui.cpp — the Datasets window (Extras > Datasets...): the datasets known and where they are (the same list as
// Musil's (datasets)), the datasets folder, a feature file registered by name (FullSOL, where the user keeps it), TinySOL
// downloaded, a zip installed, a folder of sounds analysed into a feature file; the work as a background job with its
// progress and a Cancel. The act=datasets... actions of the interface's tests (guitest.cpp) drive it without dialogs.
#include "datasets.h"
#include "gui.h"
#include "theme.h"
#include "ui_util.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/fl_ask.H>

#include <chrono>
#include <cstdio>
#include <filesystem>

namespace moritz {

namespace ds = musil::datasets;

namespace {

struct Window {
    Fl_Double_Window* win = nullptr;
    Fl_Box* folder = nullptr;
    Fl_Hold_Browser* list = nullptr;
    Fl_Progress* progress = nullptr;
    Fl_Box* status = nullptr;
    Fl_Button* cancel = nullptr;
    std::vector<Fl_Widget*> actions;        // inactive while a job runs
    std::vector<ds::Entry> entries;         // the rows of the list
    DatasetTask task;
    std::string statusText, folderText;
    std::string lastFile, lastError;        // of the last job (the tests read them)

    void say(const std::string& s) {
        statusText = s;
        if (status) { status->copy_label(s.c_str()); status->redraw(); }
    }
    void busy(bool on) {
        for (auto* w : actions) { if (on) w->deactivate(); else w->activate(); }
        if (cancel) { if (on) cancel->activate(); else cancel->deactivate(); }
        if (progress && !on) progress->value(0);
    }
    // the list again, from the configuration and the folders
    void refresh() {
        entries.clear();
        ds::Place p = datasetsPlace();
        try {
            ds::Config c = ds::read_config(p.config);
            entries = ds::list(p, c);
            folderText = ds::folder_in_use(p, c);
        } catch (const std::exception& e) {
            folderText = ds::default_folder();
            say(e.what());
        }
        if (!win) return;
        folder->copy_label(("Datasets folder: " + folderText).c_str());
        list->clear();
        for (auto& e : entries) {
            std::string where = e.where == "registered" ? "registered" : e.where == "folder" ? "in the folder"
                              : e.where == "default" ? "in the default folder" : "comes with Moritz";
            std::string line = e.name + "\t" + where + (e.present ? "" : " (missing)") + "\t" + e.path;
            list->add(line.c_str());
        }
        win->redraw();
    }
};

Window& dialog() {
    static Window d;
    return d;
}

void started(bool ok) {
    if (ok) { dialog().busy(true); dialog().say("working..."); }
}

void hookTask() {
    Window& d = dialog();
    d.task.onProgress = [](float p, const std::string& what) {
        Window& w = dialog();
        if (w.progress) w.progress->value(p);
        w.say(what + " " + std::to_string((int)(p * 100)) + "%");
    };
    d.task.onDone = [](const std::string& file, const std::string& err) {
        Window& w = dialog();
        w.lastFile = file;
        w.lastError = err;
        w.busy(false);
        w.refresh();
        w.say(err.empty() ? "ready: " + file : err);
        if (!err.empty() && !batchMode() && w.win && w.win->shown()) alertUser("%s", err.c_str());
    };
}

std::string chooseFolder(const char* title) {
    Fl_Native_File_Chooser fc;
    fc.title(title);
    fc.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    if (fc.show() != 0 || !fc.filename()) return "";
    return portablePath(fc.filename());
}

// a feature file registered under a name (asked, its file's name proposed)
void registerFile(const std::string& path, std::string name) {
    if (path.empty()) return;
    if (name.empty()) name = ds::short_key(ds::name_of_file(path));
    try {
        ds::register_dataset(datasetsPlace().config, name, path);
        dialog().say(ds::name_key(name) + " is " + path);
    } catch (const std::exception& e) {
        dialog().say(e.what());
        if (!batchMode()) alertUser("%s", e.what());
    }
    dialog().refresh();
}

// the folder, the name, the feature type and sizes of an analysis (dbgen's defaults); false when cancelled
bool askAnalysis(const std::string& folder, std::string& name, musil::db_gen_params& p) {
    const int W = 420, H = 230;
    Fl_Double_Window win(W, H, "Analyse a folder of sounds");
    win.color(kPopup);
    auto label = [](Fl_Widget* w) { w->labelcolor(kText); w->labelsize(12); };
    auto* info = new Fl_Box(14, 10, W - 28, 36);
    std::string text = "Every WAV under " + folder + ", named as in the *SOL databases (Vn-ord-C4-mf.wav), analysed into " +
                       "<name>.spectrum.db next to the folder, then registered by that name.";
    info->copy_label(text.c_str());
    info->labelsize(11);
    info->labelcolor(kDim);
    info->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    auto* in = new Fl_Input(110, 54, 200, 24, "Name");
    label(in);
    in->value(name.c_str());
    auto* type = new Fl_Choice(110, 84, 140, 24, "Features");
    label(type);
    for (const char* t : {"spectrum", "logspec", "specpeaks", "specenv", "mfcc", "moments"}) type->add(t);
    type->value(0);
    auto* block = new Fl_Int_Input(110, 114, 70, 24, "Block");
    auto* hop = new Fl_Int_Input(240, 114, 60, 24, "Hop");
    auto* nc = new Fl_Int_Input(110, 144, 70, 24, "Coefficients");
    for (auto* w : {block, hop, nc}) label(w);
    block->value(std::to_string(p.block).c_str());
    hop->value(std::to_string(p.hop).c_str());
    nc->value(std::to_string(p.ncoeff).c_str());
    auto* hint = new Fl_Box(190, 144, W - 200, 24, "spectrum: 1024 (the *SOL databases'); mfcc: 13; moments: 4");
    hint->labelsize(10);
    hint->labelcolor(kDim);
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    int answer = -1;
    auto* ok = new Fl_Button(W - 200, H - 40, 90, 26, "Analyse");
    auto* no = new Fl_Button(W - 104, H - 40, 90, 26, "Cancel");
    ok->callback([](Fl_Widget* w, void* a) { *static_cast<int*>(a) = 1; w->window()->hide(); }, &answer);
    no->callback([](Fl_Widget* w, void* a) { *static_cast<int*>(a) = 0; w->window()->hide(); }, &answer);
    win.end();
    win.set_modal();
    centerOnScreen(&win, dialog().win);
    win.show();
    while (win.shown()) Fl::wait();
    if (answer != 1) return false;
    name = in->value();
    p.type = type->text() ? type->text() : "spectrum";
    p.block = std::atoi(block->value());
    p.hop = std::atoi(hop->value());
    p.ncoeff = std::atoi(nc->value());
    return true;
}

void analyse(const std::string& folder, std::string name, musil::db_gen_params p, bool ask) {
    if (folder.empty()) return;
    if (name.empty()) {
        std::filesystem::path f = std::filesystem::path(folder).lexically_normal();
        if (f.filename().empty()) f = f.parent_path();
        name = ds::lower(f.filename().string());
        for (auto& c : name) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) c = '-';
    }
    if (ask && !askAnalysis(folder, name, p)) return;
    std::string out = analysisPath(folder, name);
    bool replace = false;
    std::error_code ec;
    if (std::filesystem::exists(out, ec)) {
        if (askUser("%s is already there: replace it with a new analysis?", "Cancel", "Replace", nullptr, out.c_str()) != 1) return;
        replace = true;
    }
    started(dialog().task.analyse(folder, name, p, replace));
}

void build() {
    Window& d = dialog();
    const int W = 680, H = 420;
    d.win = new Fl_Double_Window(W, H, "Datasets");
    d.win->color(kStatusBg);
    d.folder = new Fl_Box(14, 10, W - 300, 24);
    d.folder->labelsize(12);
    d.folder->labelcolor(kText);
    d.folder->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    auto button = [&](int x, int y, int w, const char* text, Fl_Callback* cb, const char* tip) {
        auto* b = new Fl_Button(x, y, w, 24, text);
        b->labelsize(12);
        b->callback(cb);
        b->tooltip(tip);
        d.actions.push_back(b);
        return b;
    };
    button(W - 282, 10, 84, "Choose...", [](Fl_Widget*, void*) { chooseDatasetsFolder(false); },
           "The folder Moritz and Musil look in, and download into");
    button(W - 194, 10, 84, "Default", [](Fl_Widget*, void*) { chooseDatasetsFolder(true); },
           "Back to ~/Documents/Moritz/datasets");
    button(W - 106, 10, 92, "Show", [](Fl_Widget*, void*) {
        std::error_code ec;
        std::filesystem::create_directories(dialog().folderText, ec);
        openWithSystem(dialog().folderText);
    }, "Open the datasets folder");
    d.list = new Fl_Hold_Browser(14, 42, W - 28, H - 170);
    static const int widths[] = {110, 170, 0};
    d.list->column_widths(widths);
    d.list->column_char('\t');
    d.list->format_char(0);
    d.list->textsize(12);
    d.list->tooltip("A dataset is found by its name (orchestra band fullsol, (db-load \"fullsol\")): a registered file "
                    "first, then the datasets folder, then what comes with Moritz");
    int y = H - 120;
    button(14, y, 150, "Register a file...", [](Fl_Widget*, void*) {
        std::string f = chooseFile(false, "Register a dataset: its feature file (X.spectrum.db)", "Feature files\t*.db", "");
        if (f.empty()) return;
        std::string name = ds::short_key(ds::name_of_file(f));
        const char* typed = fl_input("The name to find it by (orchestra band <name>):", name.c_str());
        if (!typed) return;
        registerFile(f, typed);
    }, "A feature file that lives elsewhere (FullSOL, where you keep it), found by name from now on");
    button(170, y, 80, "Forget", [](Fl_Widget*, void*) {
        Window& w = dialog();
        int k = w.list->value();
        if (k < 1 || k > (int)w.entries.size()) return;
        const ds::Entry& e = w.entries[(size_t)k - 1];
        if (e.where != "registered") { w.say(e.name + " is not registered: it is found in its folder"); return; }
        try { ds::forget_dataset(datasetsPlace().config, e.name); w.say(e.name + " forgotten (its files stay where they are)"); }
        catch (const std::exception& x) { w.say(x.what()); }
        w.refresh();
    }, "The registration of the selected dataset removed (its files stay)");
    button(256, y, 150, "Download TinySOL", [](Fl_Widget*, void*) {
        const ds::Source* s = ds::source("tinysol");
        Window& w = dialog();
        std::string q = std::string(s->about) + ", from " + s->url + ", into " + w.folderText + ".";
        if (askUser("%s", "Cancel", "Download", nullptr, q.c_str()) != 1) return;
        started(w.task.fetch("tinysol"));
    }, "TinySOL downloaded from Musil's release assets, checked, unpacked into the datasets folder");
    button(412, y, 120, "Install a zip...", [](Fl_Widget*, void*) {
        std::string f = chooseFile(false, "Install a dataset from a zip (TinySOL.zip, or X.spectrum.db with X/)", "Zip archives\t*.zip", "");
        if (f.empty()) return;
        std::string name = ds::short_key(ds::name_of_file(f.substr(0, f.size() - (f.size() > 4 ? 4 : 0))));
        started(dialog().task.install(name, f));
    }, "A zip on disk (TinySOL.zip downloaded by hand, checked) unpacked into the datasets folder");
    button(538, y, 128, "Analyse a folder...", [](Fl_Widget*, void*) {
        std::string f = chooseFolder("A folder of sounds named as in the *SOL databases (Vn-ord-C4-mf.wav)");
        analyse(f, "", musil::db_gen_params(), true);
    }, "Orchidea's dbgen: the features of every WAV under a folder, a feature file next to it, registered by name");
    d.progress = new Fl_Progress(14, H - 82, W - 128, 18);
    d.progress->minimum(0);
    d.progress->maximum(1);
    d.progress->selection_color(kText);
    d.cancel = new Fl_Button(W - 106, H - 84, 92, 24, "Cancel");
    d.cancel->labelsize(12);
    d.cancel->callback([](Fl_Widget*, void*) {
        dialog().task.cancel();
        dialog().busy(false);
        dialog().say("cancelled");
        dialog().refresh();
    });
    d.cancel->deactivate();
    d.status = new Fl_Box(14, H - 56, W - 28, 44);
    d.status->labelsize(11);
    d.status->labelcolor(kDim);
    d.status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP);
    d.win->end();
    d.win->resizable(d.list);
    d.win->size_range(560, 320);
    centerOnScreen(d.win, App::get().frontDoc());
    hookTask();
}

}  // namespace

std::string datasetsFolderText() {
    ds::Place p = datasetsPlace();
    try { return ds::folder_in_use(p, ds::read_config(p.config)); } catch (const std::exception&) { return p.default_folder; }
}

void chooseDatasetsFolder(bool useDefault) {
    std::string f = useDefault ? "" : chooseFolder("The datasets folder (Moritz and Musil)");
    if (!useDefault && f.empty()) return;
    try { ds::set_folder(datasetsPlace().config, f); }
    catch (const std::exception& e) { if (!batchMode()) alertUser("%s", e.what()); }
    dialog().refresh();
}

void showDatasets() {
    if (!dialog().win) build();
    dialog().refresh();
    dialog().win->show();
}

// act=datasets                         print the datasets known (state: dataset name where present path)
// act=datasetsfolder:<path>            the datasets folder chosen ("" the default)
// act=datasetsregister:<name>:<path>   a feature file registered (as Register a file... does, the name typed)
// act=datasetsforget:<name>            a registration removed
// act=datasetsanalyse:<name>:<folder>  a folder analysed (as Analyse a folder... does, with dbgen's defaults), waited for
bool datasetsTestAction(const std::string& verb, const std::string& arg) {
    Window& d = dialog();
    if (!d.win) build();
    auto split = [&](std::string& a, std::string& b) {
        size_t c = arg.find(':');
        if (c == std::string::npos) return false;
        a = arg.substr(0, c);
        b = arg.substr(c + 1);
        return true;
    };
    auto print = [](const std::string& s) { std::printf("state: %s\n", s.c_str()); std::fflush(stdout); };
    auto wait = [&] {
        auto t0 = std::chrono::steady_clock::now();
        while (d.task.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) Fl::wait(0.01);
        for (int k = 0; k < 20; ++k) Fl::wait(0.01);        // the done callback, posted to this thread
    };
    std::string a, b;
    if (verb == "datasets") {
        d.refresh();
        print("datasets folder " + d.folderText);
        for (auto& e : d.entries) print("dataset " + e.name + " " + e.where + " " + (e.present ? "present " : "missing ") + e.path);
        return true;
    }
    if (verb == "datasetsfolder") {
        try { ds::set_folder(datasetsPlace().config, arg); } catch (const std::exception& e) { print(e.what()); return false; }
        d.refresh();
        return true;
    }
    if (verb == "datasetsregister" && split(a, b)) {
        registerFile(b, a);
        print("datasets status " + d.statusText);
        return true;
    }
    if (verb == "datasetsforget") {
        try { ds::forget_dataset(datasetsPlace().config, arg); } catch (const std::exception& e) { print(e.what()); return false; }
        d.refresh();
        return true;
    }
    if (verb == "datasetsanalyse" && split(a, b)) {
        d.lastFile.clear();
        d.lastError.clear();
        analyse(b, a, musil::db_gen_params(), false);
        wait();
        print("datasets analysed " + (d.lastError.empty() ? d.lastFile : "error " + d.lastError));
        return true;
    }
    return false;
}

}  // namespace moritz
