// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// timeline_window.cpp — the timeline in a window of its own, and how the application opens it: a score box
// double-clicked (or its menu's Open as Timeline), File > Open Score... (a .score file), File > New Score, a sub-score
// clip double-clicked. One window per score (ScoreDoc: one document per score); closing a window whose score has
// edits only it holds (a file, a new score) asks first; a score kept in a patch is the patch's (File > Save saves it).
// The test actions (act=tl...) drive the panel as the mouse does.

#include "timeline.h"
#include "fileio.h"
#include "roll.h"
#include "transport.h"
#include "ui_util.h"

#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/platform.H>

#include <cstdio>

namespace moritz {

namespace {
std::vector<TimelineWindow*>& windows() {
    static std::vector<TimelineWindow*> w;
    return w;
}
std::string baseOf(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}
}  // namespace

TimelineWindow::TimelineWindow(std::shared_ptr<ScoreDoc> doc) : Fl_Double_Window(1320, 720) {
    color(kStatusBg);
#ifndef __APPLE__
    menu_ = new Fl_Menu_Bar(0, 0, w(), kMenuH);
    App::get().buildMenu(menu_);
#endif
    panel_ = new TimelinePanel(0, kMenuH, w(), h() - kMenuH, std::move(doc));
    end();
    resizable(panel_);
    size_range(820, 420);
    panel_->onTitle = [this] { updateTitle(); };
    callback([](Fl_Widget* w, void*) {
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;   // Escape doesn't close it
        auto* tw = static_cast<TimelineWindow*>(w);
        if (tw->confirmDiscard()) closeTimeline(tw);
    });
    updateTitle();
}

TimelineWindow::~TimelineWindow() {
    if (menu_) App::get().forgetMenu(menu_);
}

void TimelineWindow::updateTitle() {
    std::string t = doc().title() + " — timeline — Moritz";
    if (!label() || t != label()) copy_label(t.c_str());
}

int TimelineWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        panel_->stop();
        return 1;
    }
    int r = Fl_Double_Window::handle(e);
    if (e == FL_FOCUS || e == FL_SHOW) App::get().refreshMenus();
    return r;
}

bool TimelineWindow::save() {
    bool ok = saveScoreDocument(panel_->docPtr(), [this](const std::string& m) { panel_->setStatus(m); });
    updateTitle();
    return ok;
}

bool TimelineWindow::saveAs() {
    bool ok = saveScoreDocumentAs(panel_->docPtr(), [this](const std::string& m) { panel_->setStatus(m); });
    updateTitle();
    return ok;
}

bool TimelineWindow::confirmDiscard() { return confirmDiscardScoreDocument(panel_->docPtr(), this); }

// ---------------------------------------------------------------- saving and closing a score's windows

bool saveScoreDocument(const std::shared_ptr<ScoreDoc>& dp, const std::function<void(const std::string&)>& status) {
    if (!dp) return false;
    ScoreDoc& d = *dp;
    if (d.where() == ScoreDoc::Where::Box && d.path().empty()) {     // kept in its patch: the patch is saved
        if (Box* b = d.box())
            if (DocumentWindow* w = App::get().docOf(b->patch)) return w->save();
        return saveScoreDocumentAs(dp, status);
    }
    if (d.where() == ScoreDoc::Where::Sub && d.path().empty()) {     // embedded: the score it is inside holds it
        auto top = d.chain().front();
        if (top == dp) { status("this score is kept inside another: save that one"); return false; }
        return saveScoreDocument(top, status);
    }
    if (!d.canSaveInPlace()) return saveScoreDocumentAs(dp, status);
    std::string err;
    if (!d.save(err)) {
        alertUser("Can't save %s:\n%s\n\nThe file on the disk is as it was.", baseOf(d.path()).c_str(), err.c_str());
        App::get().print("can't save: " + err);
        return false;
    }
    App::get().log("saved " + d.path());
    status("saved " + baseOf(d.path()));
    return true;
}

bool saveScoreDocumentAs(const std::shared_ptr<ScoreDoc>& dp, const std::function<void(const std::string&)>& status) {
    if (!dp) return false;
    ScoreDoc& d = *dp;
    bool copy = d.where() == ScoreDoc::Where::Box || d.where() == ScoreDoc::Where::Sub;
    Fl_Native_File_Chooser fc;
    fc.title(copy ? "Save a Copy of the Score" : "Save Score As");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.filter("Score\t*.score");
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
    std::string name = d.path().empty() ? (d.score().name.empty() ? std::string("untitled") : d.score().name) : baseOf(d.path());
    if (size_t dot = name.find_last_of('.'); dot != std::string::npos) name.resize(dot);
    if (!d.folder().empty()) fc.directory(d.folder().c_str());
    fc.preset_file((name + ".score").c_str());
    std::string p = showSaveChooser(fc);
    if (p.empty()) return false;
    if (p.size() < 6 || p.compare(p.size() - 6, 6, ".score") != 0) p += ".score";
    std::string err;
    if (!d.saveAs(p, err)) {
        alertUser("Can't save %s:\n%s", baseOf(p).c_str(), err.c_str());
        App::get().print("can't save: " + err);
        return false;
    }
    App::get().log("saved " + p);
    status((copy ? "a copy saved as " : "saved ") + baseOf(p));
    if (!copy) App::get().addRecent(p);
    return true;
}

bool confirmDiscardScoreDocument(const std::shared_ptr<ScoreDoc>& dp, const Fl_Window* except) {
    if (!dp || !dp->needsSaving()) return true;
    for (auto* w : windows())                    // another window shows the same score: it is not lost
        if (w != except && w->panel()->docPtr() == dp) return true;
    for (auto* w : RollWindow::all())
        if (w != except && w->shows(dp.get())) return true;
    std::string name = dp->path().empty() ? std::string("Untitled score") : baseOf(dp->path());
    int r = askUser("Save changes to the score \"%s\"?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return saveScoreDocument(dp, [](const std::string&) {});
    return true;
}

// ---------------------------------------------------------------- the application's side

TimelineWindow* openTimeline(std::shared_ptr<ScoreDoc> doc) {
    if (!doc) return nullptr;
    for (auto* w : windows())
        if (w->panel()->docPtr() == doc) { w->show(); return w; }
    fl_open_display();
    auto* w = new TimelineWindow(doc);
    int k = (int)windows().size();
    centerOnScreen(w, App::get().frontDoc(), 24 * (k % 6), 24 * (k % 6));
    windows().push_back(w);
    w->show();
    return w;
}

TimelineWindow* openScoreBoxTimeline(Box* box) {
    auto d = ScoreDoc::ofBox(box);
    if (!d) return nullptr;
    return openTimeline(d);
}

TimelineWindow* openScoreFile(const std::string& given) {
    std::string path = fileio::absolute(given);
    std::string err;
    auto d = ScoreDoc::open(path, err);
    if (!d) {
        alertUser("Can't open %s:\n%s", baseOf(path).c_str(), err.c_str());
        return nullptr;
    }
    App::get().addRecent(path);
    App::get().log("opened score " + path);
    return openTimeline(d);
}

void openScoreDialog() {
    Fl_Native_File_Chooser fc;
    fc.title("Open Score");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Score\t*.score\nAll files\t*");
    if (fc.show() != 0) return;
    openScoreFile(portablePath(fc.filename()));
}

TimelineWindow* newScoreTimeline() { return openTimeline(ScoreDoc::make("untitled")); }

TimelineWindow* frontTimeline() { return dynamic_cast<TimelineWindow*>(Fl::first_window()); }

const std::vector<TimelineWindow*>& timelineWindows() { return windows(); }

void closeTimeline(TimelineWindow* w) {
    auto& all = windows();
    all.erase(std::remove(all.begin(), all.end(), w), all.end());
    w->hide();
    Fl::delete_widget(w);
    App::get().refreshMenus();
}

void closeAllTimelines() {
    auto all = std::move(windows());
    windows().clear();
    for (auto* w : all) {
        w->hide();
        delete w;                                // its render and waveform jobs stopped and waited for
    }
}

bool confirmCloseTimelines() {
    for (auto* w : std::vector<TimelineWindow*>(windows())) {
        w->show();
        if (!w->confirmDiscard()) return false;
    }
    return true;
}

// ---------------------------------------------------------------- the tests' actions
//   act=tlopen:<box id>              the timeline of a score box of the front document
//   act=tlfile:<path>, act=tlnew     File > Open Score..., File > New Score
//   act=timeline                     print: the front timeline (title, tracks, clips, undo, edited, selection)
//   act=tlclick:<id>[:shift]         a click on a clip; act=tldbl:<id> a double-click
//   act=tldrag:<id>,<seconds>[,<tracks>]   a clip dragged by the mouse (its body), released
//   act=tltrim:<id>,<left|right>,<seconds> its edge dragged to that time, released
//   act=tlpoint:<track>,<lane>,<seconds>,<value>   a click on a lane: a point there
//   act=tlmute:<track>, act=tlsolo:<track>   the header's M or S button
//   act=tlop:<op>                    undo redo cut copy paste duplicate delete split unique selectall addtrack fit play stop
//                                    render (saveas=<path> answers where)
//   act=tlplayhead:<seconds>, act=tlsnap:<off|beat|bar>, act=tladdsound:<file>, act=tllane:<track>,<name>
//   act=tlsave                       File > Save (saveas=<path> answers a Save As)
//   act=tlpng:<file>                 the front timeline window drawn into a PNG file
//   act=tlview:<roll|list>           the toolbar's Roll or List button (the roll's window, on the same document)
namespace {

TimelineWindow* front() {
    if (auto* w = frontTimeline()) return w;
    return windows().empty() ? nullptr : windows().back();
}

void say(const std::string& s) {
    std::printf("state: %s\n", s.c_str());
    std::fflush(stdout);
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t k = 0;
    for (;;) {
        size_t e = s.find(c, k);
        out.push_back(s.substr(k, e == std::string::npos ? std::string::npos : e - k));
        if (e == std::string::npos) break;
        k = e + 1;
    }
    return out;
}

void mouse(TimelineView* v, int ex, int ey, int event, int clicks, bool shift) {
    Fl::e_x = Fl::e_x_root = ex;
    Fl::e_y = Fl::e_y_root = ey;
    Fl::e_state = (shift ? FL_SHIFT : 0) | (event == FL_RELEASE ? 0 : FL_BUTTON1);
    Fl::e_clicks = clicks;
    Fl::e_is_click = 1;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_number = event;
    v->handle(event);
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

}  // namespace

bool timelineTestAction(const std::string& verb, const std::string& arg) {
    App& app = App::get();
    if (verb == "tlopen") {
        DocumentWindow* d = app.docs.empty() ? nullptr : app.docs.front();
        Box* b = d ? d->patch.find(arg) : nullptr;
        return b && openScoreBoxTimeline(b);
    }
    if (verb == "tlfile") return openScoreFile(arg) != nullptr;
    if (verb == "tlnew") return newScoreTimeline() != nullptr;
    TimelineWindow* w = front();
    if (!w && verb == "timeline") { say("timeline none"); return true; }
    if (!w) return false;
    TimelinePanel* p = w->panel();
    TimelineView* v = p->view();
    ScoreDoc& d = p->doc();
    const sc::Score& s = d.score();
    auto centre = [&](uint64_t id, int& ex, int& ey) {
        auto e = s.find(id);
        int X, Y, W, H;
        if (!e || !v->clipRect(*e, X, Y, W, H)) return false;
        ex = X + W / 2;
        ey = Y + H / 2;
        return true;
    };
    if (verb == "timeline") {
        say("timeline " + std::string(w->label() ? w->label() : ""));
        std::string t;
        for (auto& x : timeline::tracks(s)) t += x + "|";
        say("timeline tracks " + t);
        for (auto& e : s.events())
            say("timeline clip " + std::to_string(e->id) + " " + sc::kindName(e->kind) + " " + e->row + " " + num(e->at) +
                " " + num(e->dur) + (e->kind == sc::Kind::Sound ? " from " + num(e->from) + " to " + num(e->to) : std::string()) +
                (e->kind == sc::Kind::Message && !e->values.empty() ? " value " + num(timeline::pointValue(*e)) : std::string()));
        say(std::string("timeline undo ") + (d.canUndo() ? "yes" : "no") + " redo " + (d.canRedo() ? "yes" : "no") + " edited " +
            (d.needsSaving() ? "yes" : "no"));
        std::string sel;
        for (uint64_t id : d.selection()) sel += std::to_string(id) + " ";
        say("timeline selection " + sel);
        say("timeline status " + p->status());
        std::string crumbs;
        for (auto& c : d.chain()) crumbs += c->title() + "|";
        say("timeline crumbs " + crumbs);
        say("timeline windows " + std::to_string(windows().size()));
        return true;
    }
    if (verb == "tlclick" || verb == "tldbl") {
        int ex, ey;
        if (!centre(std::strtoull(arg.c_str(), nullptr, 10), ex, ey)) return false;
        bool shift = arg.find(":shift") != std::string::npos;
        int clicks = verb == "tldbl" ? 1 : 0;
        mouse(v, ex, ey, FL_PUSH, clicks, shift);
        mouse(v, ex, ey, FL_RELEASE, clicks, shift);
        return true;
    }
    if (verb == "tldrag") {
        auto a = split(arg, ',');
        int ex, ey;
        if (a.size() < 2 || !centre(std::strtoull(a[0].c_str(), nullptr, 10), ex, ey)) return false;
        int tx = ex + (int)std::lround(std::atof(a[1].c_str()) * p->pixelsPerSecond());
        int ty = ey + (a.size() > 2 ? std::atoi(a[2].c_str()) * TimelineView::kTrackH : 0);
        if (a.size() > 2 && std::atoi(a[2].c_str()) != 0) {     // past the lanes in between: the track's own row
            auto e = s.find(std::strtoull(a[0].c_str(), nullptr, 10));
            auto all = timeline::tracks(s);
            int k = timeline::trackIndex(s, e->row) + std::atoi(a[2].c_str());
            if (k >= 0 && k < (int)all.size())
                for (auto& r : v->rows())
                    if (r.lane < 0 && r.track == all[(size_t)k]) ty = v->y() + TimelineView::kRulerH + r.y - p->scrollY() + r.h / 2;
        }
        mouse(v, ex, ey, FL_PUSH, 0, false);
        mouse(v, (ex + tx) / 2, (ey + ty) / 2, FL_DRAG, 0, false);
        mouse(v, tx, ty, FL_DRAG, 0, false);
        mouse(v, tx, ty, FL_RELEASE, 0, false);
        return true;
    }
    if (verb == "tltrim") {
        auto a = split(arg, ',');
        if (a.size() < 3) return false;
        auto e = s.find(std::strtoull(a[0].c_str(), nullptr, 10));
        int X, Y, W, H;
        if (!e || !v->clipRect(*e, X, Y, W, H)) return false;
        int ex = a[1] == "left" ? X + 1 : X + W - 2, ey = Y + H / 2;
        int tx = p->xOf(std::atof(a[2].c_str()));
        mouse(v, ex, ey, FL_PUSH, 0, false);
        mouse(v, tx, ey, FL_DRAG, 0, false);
        mouse(v, tx, ey, FL_RELEASE, 0, false);
        return true;
    }
    if (verb == "tlpoint") {
        auto a = split(arg, ',');
        if (a.size() < 4) return false;
        for (auto& r : v->rows())
            if (r.lane >= 0 && r.track == a[0] && r.laneName == a[1]) {
                double val = std::atof(a[3].c_str());
                int Y = v->y() + TimelineView::kRulerH + r.y - p->scrollY();
                int ey = Y + 4 + (int)std::lround((1 - (val - r.lo) / (r.hi - r.lo)) * (r.h - 8));
                int ex = p->xOf(std::atof(a[2].c_str()));
                mouse(v, ex, ey, FL_PUSH, 0, false);
                mouse(v, ex, ey, FL_RELEASE, 0, false);
                return true;
            }
        return false;
    }
    if (verb == "tlmute" || verb == "tlsolo") {
        for (auto& r : v->rows())
            if (r.lane < 0 && r.track == arg) {
                int Y = v->y() + TimelineView::kRulerH + r.y - p->scrollY();
                int ex = v->x() + TimelineView::kHeadW - 50 + (verb == "tlsolo" ? 22 : 0) + 8, ey = Y + 14;
                mouse(v, ex, ey, FL_PUSH, 0, false);
                mouse(v, ex, ey, FL_RELEASE, 0, false);
                return true;
            }
        return false;
    }
    if (verb == "tlview") {                         // act=tlview:roll|list: the toolbar's Roll or List button
        w->panel()->showRoll(arg == "list" ? 2 : 0);
        return true;
    }
    if (verb == "tlop") {
        static const std::pair<const char*, void (TimelinePanel::*)()> ops[] = {
            {"undo", &TimelinePanel::undo},       {"redo", &TimelinePanel::redo},
            {"cut", &TimelinePanel::cut},         {"copy", &TimelinePanel::copy},
            {"paste", &TimelinePanel::paste},     {"duplicate", &TimelinePanel::duplicate},
            {"delete", &TimelinePanel::erase},    {"split", &TimelinePanel::splitAtPlayhead},
            {"unique", &TimelinePanel::makeUnique}, {"selectall", &TimelinePanel::selectAll},
            {"addtrack", &TimelinePanel::addTrack}, {"fit", &TimelinePanel::fit},
            {"play", &TimelinePanel::play},       {"stop", &TimelinePanel::stop},
            {"render", &TimelinePanel::renderDialog}};
        for (auto& o : ops)
            if (arg == o.first) {
                (p->*o.second)();
                return true;
            }
        return false;
    }
    if (verb == "tlplayhead") { p->setPlayhead(std::atof(arg.c_str())); return true; }
    if (verb == "tlsnap") {
        p->setSnap(arg == "off" ? timeline::Snap::Off : arg == "bar" ? timeline::Snap::Bar : timeline::Snap::Beat);
        return true;
    }
    if (verb == "tladdsound") return p->addSoundFile(arg, p->currentTrack(), p->playhead());
    if (verb == "tllane") {
        auto a = split(arg, ',');
        return a.size() == 2 && timeline::addLane(d, a[0], a[1]);
    }
    if (verb == "tlsave") return w->save() || true;
    if (verb == "tlpng") {
        Fl_Image_Surface surf(w->w(), w->h());
        Fl_Surface_Device::push_current(&surf);
        surf.draw(w);
        Fl_Surface_Device::pop_current();
        Fl_RGB_Image* img = surf.image();
        int r = fl_write_png(arg.c_str(), img);
        delete img;
        return r == 0;
    }
    return false;
}

}  // namespace moritz
