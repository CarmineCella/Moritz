// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_window.cpp — a score's window (score_window.h): the timeline's and the roll's panels as views switched in
// place, the transport bar they share (ScorePlayback), the breadcrumb, the status line; how the application opens,
// saves and closes it; the test actions (act=tl..., act=roll..., act=sc...), which drive it as the mouse does.

#include "score_window.h"
#include "arranger.h"
#include "multitrack_ops.h"
#include "fileio.h"
#include "roll.h"
#include "sound_editor.h"
#include "timeline.h"
#include "transport.h"
#include "ui_util.h"

#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
void fl_open_display();   // FL/platform.H (not included: it drags in X11's macros on Linux, None among them)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace moritz {

namespace {
constexpr int kCrumbsH = 24;
std::vector<ScoreWindow*>& windows() {
    static std::vector<ScoreWindow*> w;
    return w;
}
std::string baseOf(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}
std::string clockText(double s) {
    if (!(s >= 0)) s = 0;
    int m = (int)(s / 60);
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%06.3f", m, s - m * 60);
    return b;
}
}  // namespace

const char* scoreViewName(ScoreView v) {
    switch (v) {
    case ScoreView::Timeline: return "timeline";
    case ScoreView::Roll: return "roll";
    case ScoreView::Piano: return "piano roll";
    case ScoreView::List: return "list";
    }
    return "roll";
}

bool parseScoreView(const std::string& s, ScoreView& v) {
    if (s == "timeline") v = ScoreView::Timeline;
    else if (s == "roll") v = ScoreView::Roll;
    else if (s == "piano" || s == "pianoroll" || s == "piano roll") v = ScoreView::Piano;
    else if (s == "list") v = ScoreView::List;
    else return false;
    return true;
}

// ================================================================ the transport of a score's views

ScorePlayback::ScorePlayback(std::shared_ptr<ScoreDoc> doc, Patch* root) : doc_(std::move(doc)), root_(root) {
    // a selection made after the playhead was placed: Play starts at its start
    token_ = doc_->listen([this](unsigned what) {
        if (what & ScoreDoc::Selection) selNewer_ = !doc_->selection().empty();
    });
}

ScorePlayback::~ScorePlayback() { doc_->unlisten(token_); }

Patch* ScorePlayback::root() const { return root_ ? root_ : &App::get().host; }

// the score's box (its own player for a score with none: a file, a new score, a score placed inside another, a box
// gone), and so its clock: never another one, so the button shows what plays this score
Patch* ScorePlayback::clock() const {
    Box* b = doc_->playerBox();
    return b ? b->patch : nullptr;
}

bool ScorePlayback::running() const {
    Patch* p = clock();
    return p && transportOf(p)->playing();
}

double ScorePlayback::transportSeconds() const {
    Patch* p = clock();
    if (!p) return playhead_;
    auto t = transportOf(p);
    int lap;
    double pos;
    t->where(p, lap, pos);
    return t->seconds(pos);
}

bool ScorePlayback::ours() const {
    Patch* p = clock();
    return startedHere_ && p && transportOf(p)->changes() == changes_;
}

void ScorePlayback::claim() {
    if (Patch* p = clock()) changes_ = transportOf(p)->changes();
}

bool ScorePlayback::playing() const {
    Patch* p = clock();
    if (!p) return false;
    auto t = transportOf(p);
    if (!t->playing()) return false;
    if (ours()) return true;                              // its own play, until it ends (tick) or is stopped
    if (t->looping()) return true;
    return transportSeconds() < doc_->score().duration() + 0.05;   // the patch's clock past the score's end: not this score
}

double ScorePlayback::position() const { return playing() ? transportSeconds() : playhead_; }

double ScorePlayback::startPoint() const {
    if (selNewer_) {
        double first = INFINITY;
        for (uint64_t id : doc_->selection())
            if (auto e = doc_->score().find(id)) first = std::min(first, e->at);
        if (std::isfinite(first)) return first;
    }
    return playhead_;
}

void ScorePlayback::setPlayhead(double t) {
    playhead_ = std::max(0.0, t);
    selNewer_ = false;
    if (playing()) {
        bool mine = ours();
        Patch* p = clock();
        transportOf(p)->locate(p, playhead_);
        startAt_ = playhead_;
        if (mine) claim();
    }
}

bool ScorePlayback::play() {
    Box* b = doc_->player(root());                      // its box, or its own player (made the first time)
    if (!b) {
        message = "nothing can play this score now";
        return false;
    }
    const sc::Score& s = doc_->score();
    if (s.empty()) {
        message = "the score is empty: nothing to play";
        return false;
    }
    Patch* p = b->patch;
    auto t = transportOf(p);
    double end = s.duration(), from = startPoint();
    std::string where;
    bool looped = loop_ && loopB_ > loopA_;
    if (looped && from >= loopB_) {                     // past the loop: from its start
        from = loopA_;
        where = " (the loop's start)";
    } else if (!looped && end > 0 && from >= end - 1e-9) {   // at or past the end: from the start (it would play nothing)
        from = 0;
        where = " (the playhead was at the end)";
    }
    t->setTempo(p, s.bpm);                              // bars and beats as the score counts them
    t->setMeter(p, s.meterNum, s.meterDen);
    t->setLoop(p, loop_, loopA_, loopB_);
    t->locate(p, from);                                 // first: a transport already running goes there too
    t->play(p);
    playhead_ = from;
    startAt_ = from;
    startedHere_ = true;
    selNewer_ = false;
    claim();
    bool off = !p->root()->offline && !App::get().audio.running();
    message = "playing from " + clockText(from) + where;
    if (off) message += ", but audio is off: turn it on (the Audio button, " + shortcutText(FL_COMMAND + '/') + ") to hear it";
    return true;
}

void ScorePlayback::stop() {
    if (Patch* p = clock()) {
        auto t = transportOf(p);
        if (t->playing()) {
            if (playing()) playhead_ = transportSeconds();
            t->stop(p);
        }
    }
    startedHere_ = false;
    message = "stopped";
}

void ScorePlayback::toggle() {
    if (playing()) stop();
    else play();
}

void ScorePlayback::setLoop(bool on) {
    loop_ = on && loopB_ > loopA_;
    if (running()) {
        bool mine = ours();
        Patch* p = clock();
        transportOf(p)->setLoop(p, loop_, loopA_, loopB_);
        if (mine) claim();
    }
    message = on && !loop_ ? "drag in the ruler to say where the loop is" : loop_ ? "loop on" : "loop off";
}

void ScorePlayback::setLoopRange(double a, double b) {
    loopA_ = std::max(0.0, std::min(a, b));
    loopB_ = std::max(a, b);
}

void ScorePlayback::tick() {
    Patch* p = clock();
    if (!p) { startedHere_ = false; return; }
    auto t = transportOf(p);
    // stopped, or started, located or stopped by another since (a transport object, another window of the patch): not
    // this play any more, left alone
    if (!t->playing() || !ours()) { startedHere_ = false; return; }
    if (t->looping()) return;
    double end = std::max(doc_->score().duration(), startAt_ + 0.05);
    if (transportSeconds() >= end) {                    // the score's end: a play started here stops, back to its start
        t->stop(p);
        startedHere_ = false;
        playhead_ = startAt_;
        message = "the end of the score";
    }
}

// ================================================================ the window

ScoreWindow::ScoreWindow(std::shared_ptr<ScoreDoc> doc, ScoreView view, bool arranger)
    : Fl_Double_Window(arranger ? 1480 : 1320, arranger ? 940 : 820), doc_(std::move(doc)) {
    color(kStatusBg);
    play_ = std::make_shared<ScorePlayback>(doc_);
    // a score without a box plays on a player of its own: made now, not at the first Play (its patch compiled, its
    // files decoded), so Play starts at once
    if (!doc_->box()) doc_->player(&App::get().host);
    target_ = makeRollTarget(doc_);
#ifndef __APPLE__
    menu_ = new Fl_Menu_Bar(0, 0, w(), kMenuH);
    App::get().buildMenu(menu_);
#endif
    int Y = kMenuH;
    bar_ = new Fl_Group(0, Y, w(), kToolH);
    bar_->box(FL_FLAT_BOX);
    bar_->color(kStatusBg);
    int bx = 8;
    const int by = Y + 5, bh = kToolH - 10;
    using W = ScoreWindow;
    // the views of the score, switched here
    const char* names[] = {"Timeline", "Roll", "Piano roll", "List"};
    const char* tips[] = {"The score as tracks: clips, lanes, the inspector", "Staves, a row each, proportional time",
                          "A keyboard of pitches", "The events as a table, each cell edited"};
    const int tw[] = {80, 56, 86, 50};
    for (int k = 0; k < 4; ++k) {
        tabs_[k] = new ToolButton(bx, by, tw[k], bh, ToolButton::None, names[k], true);
        tabs_[k]->copy_tooltip((std::string(tips[k]) + ": a view of the same score (the same edits, undo and selection)").c_str());
        tabs_[k]->callback([](Fl_Widget* w, void* d) {
            auto* sw = static_cast<W*>(d);
            for (int j = 0; j < 4; ++j)
                if (sw->tabs_[j] == w) sw->setView((ScoreView)j);
        }, this);
        bx += tw[k] + 2;
    }
    bx += 18;
    // the transport
    auto* toStart = new ToolButton(bx, by, 34, bh, ToolButton::ToStart, "", false);
    toStart->tooltip("The playhead to the start (Home)");
    toStart->callback([](Fl_Widget*, void* d) {
        auto* sw = static_cast<W*>(d);
        sw->play_->setPlayhead(0);
        sw->timeline_->setView(0, sw->timeline_->pixelsPerSecond());
        sw->refresh();
        sw->redraw();
    }, this);
    bx += 38;
    playBtn_ = new ToolButton(bx, by, 40, bh, ToolButton::Play, "", true);
    playBtn_->tooltip("Play from the playhead, or from the start of what was selected after it; again: Stop (Space)");
    playBtn_->callback([](Fl_Widget*, void* d) {
        auto* sw = static_cast<W*>(d);
        sw->roll_->applyText();
        sw->play_->toggle();
        sw->setStatus(sw->play_->message);
        sw->refresh();
        sw->redraw();
    }, this);
    bx += 44;
    if (arranger) {
        recordBtn_ = new ToolButton(bx, by, 34, bh, ToolButton::Record, "", true);
        recordBtn_->tooltip("Record into the armed tracks from the playhead (R in a track's header arms it); again: Stop. "
                            "Right-click: the count-in, punch in and out, the latency");
        recordBtn_->callback([](Fl_Widget*, void* d) {
            auto* sw = static_cast<W*>(d);
            if (Fl::event_button() == FL_RIGHT_MOUSE) {
                sw->recordBtn_->value(sw->recording());
                sw->recordOptions();
                return;
            }
            sw->toggleRecord();
            sw->refresh();
        }, this);
        bx += 38;
    }
    loopBtn_ = new ToolButton(bx, by, 34, bh, ToolButton::Loop, "", true);
    loopBtn_->tooltip("Loop (L): drag in the timeline's ruler to say where");
    loopBtn_->callback([](Fl_Widget* w, void* d) {
        auto* sw = static_cast<W*>(d);
        sw->play_->setLoop(static_cast<ToolButton*>(w)->value() != 0);
        sw->setStatus(sw->play_->message);
        sw->refresh();
        sw->timeline_->view()->redraw();
    }, this);
    bx += 42;
    position_ = new Fl_Box(bx, by, 190, bh);
    position_->box(FL_FLAT_BOX);
    position_->color(fl_rgb_color(14, 15, 17));
    position_->labelfont(FL_COURIER_BOLD);
    position_->labelsize(13);
    position_->labelcolor(fl_rgb_color(110, 220, 140));
    position_->tooltip("Where the playhead is: bar.beat.tick and minutes:seconds");
    bx += 190 + 14;
    audioBtn_ = new ToolButton(bx, by, 96, bh, ToolButton::Power, "Audio", true);
    audioBtn_->copy_tooltip(withShortcut("Start or stop audio", "patch.audio").c_str());
    audioBtn_->callback([](Fl_Widget*, void* d) {
        App::get().toggleAudio();
        static_cast<W*>(d)->refresh();
    }, this);
    bx += 100;
    auto* spacer = new Fl_Box(bx, Y, std::max(1, w() - bx), kToolH);
    spacer->box(FL_NO_BOX);
    bar_->resizable(spacer);
    bar_->end();
    // the breadcrumb (made again when it changes: updateCrumbs)
    crumbs_ = new Fl_Group(0, Y + kToolH, w(), kCrumbsH);
    crumbs_->box(FL_FLAT_BOX);
    crumbs_->color(fl_rgb_color(30, 32, 40));
    crumbs_->end();
    crumbInfo_ = nullptr;
    // the views
    int cy = Y + kToolH + kCrumbsH, ch = h() - cy - kStatusH;
    timeline_ = new TimelinePanel(0, cy, w(), ch, doc_, play_);
    roll_ = new RollPanel(0, cy, w(), ch, target_, play_);
    if (arranger) {
        browser_ = new FileBrowserPanel(0, cy, 220, ch);
        drawer_ = new Drawer(0, cy, w(), 300, doc_, play_);
        recorder_ = std::make_unique<mt::SessionRecorder>();
        std::string dir = doc_->folder();
        browser_->setFolder(!dir.empty() && dir != "." ? dir : homeDir());
        browser_->onOpen = [this](const std::string& path) {
            std::string l = path;
            for (auto& c : l) c = (char)std::tolower((unsigned char)c);
            auto ends = [&](const char* e) { return l.size() > std::strlen(e) && l.compare(l.size() - std::strlen(e), std::strlen(e), e) == 0; };
            std::string track = timeline_->currentTrack();
            if (ends(".score")) timeline_->after(timeline::addSubScore(*doc_, path, track, play_->playhead()) != 0);
            else if (ends(".moritz")) timeline_->after(timeline::addPatchClip(*doc_, path, track, play_->playhead(), 0) != 0);
            else timeline_->addSoundFile(path, track, play_->playhead());
        };
        drawer_->onStatus = [this](const std::string& m) { setStatus(m); };
        docToken_ = doc_->listen([this](unsigned what) {
            if (!drawer_) return;
            if (what & ScoreDoc::Selection) drawer_->selectionChanged();
            if ((what & ScoreDoc::Content) && drawer_->tab() == Drawer::Tab::Track) drawer_->setRole(drawer_->role());
        });
    }
    // the status line
    infoBox_ = new Fl_Box(0, h() - kStatusH, w() / 2, kStatusH);
    messageBox_ = new Fl_Box(w() / 2, h() - kStatusH, w() - w() / 2, kStatusH);
    for (Fl_Box* b : {infoBox_, messageBox_}) {
        b->box(FL_FLAT_BOX);
        b->color(kStatusBg);
        b->labelcolor(kDim);
        b->labelsize(11);
    }
    infoBox_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    messageBox_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    end();
    resizable(timeline_);
    size_range(900, 560);
    timeline_->onStatus = [this](const std::string& s) { if (view_ == ScoreView::Timeline) setStatus(s); };
    timeline_->onInfo = [this] { refresh(); };
    timeline_->onTitle = [this] { updateTitle(); };
    roll_->onStatus = [this](const std::string& s) { if (view_ != ScoreView::Timeline) setStatus(s); };
    roll_->onChanged = [this] {
        updateCrumbs();
        refresh();
    };
    roll_->onTitle = [this] { updateTitle(); };
    target_->setListener([this](unsigned what) {
        if (what & ScoreDoc::State) updateTitle();
        updateCrumbs();
    });
    callback([](Fl_Widget* w, void*) {
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;   // Escape stops or goes up, not away
        auto* sw = static_cast<W*>(w);
        sw->roll_->applyText();
        if (sw->confirmDiscard()) closeScoreWindow(sw);
    });
    view_ = view;
    if (arranger) timeline_->setSession(true);
    setView(view);
    if (arranger) layout();
    Fl::add_timeout(1.0 / 30, tick, this);
}

ScoreWindow::~ScoreWindow() {
    Fl::remove_timeout(tick, this);
    if (docToken_) doc_->unlisten(docToken_);
    if (recorder_ && recorder_->recording()) {           // what was recorded is kept (its takes are clips)
        std::string m;
        recorder_->stop(m);
    }
    target_->setListener(nullptr);
    timeline_->onStatus = nullptr;
    timeline_->onInfo = nullptr;
    timeline_->onTitle = nullptr;
    roll_->onStatus = nullptr;
    roll_->onChanged = nullptr;
    roll_->onTitle = nullptr;
    if (menu_) App::get().forgetMenu(menu_);
}

void ScoreWindow::resize(int X, int Y, int W, int H) {
    Fl_Double_Window::resize(X, Y, W, H);   // the window (its children then placed by layout())
    layout();
}

void ScoreWindow::layout() {
    int W = w(), H = h();
    if (menu_) menu_->resize(0, 0, W, kMenuH);
    int Y = kMenuH;
    bar_->resize(0, Y, W, kToolH);
    crumbs_->resize(0, Y + kToolH, W, kCrumbsH);
    if (crumbInfo_) crumbInfo_->resize(W / 2, Y + kToolH + 2, W / 2 - 10, kCrumbsH - 4);
    int cy = Y + kToolH + kCrumbsH, ch = std::max(100, H - cy - kStatusH);
    if (drawer_) {                                       // the arranger: the browser, the views, the drawer under them
        int dh = std::clamp(ch * 36 / 100, 200, 380), bw = 220;
        int mh = std::max(100, ch - dh);
        browser_->resize(0, cy, bw, mh);
        timeline_->resize(bw, cy, W - bw, mh);
        roll_->resize(bw, cy, W - bw, mh);
        drawer_->resize(0, cy + mh, W, ch - mh);
    } else {
        timeline_->resize(0, cy, W, ch);
        roll_->resize(0, cy, W, ch);
    }
    infoBox_->resize(0, H - kStatusH, W / 2, kStatusH);
    messageBox_->resize(W / 2, H - kStatusH, W - W / 2, kStatusH);
    updateCrumbs();
    redraw();
}

void ScoreWindow::syncTime(ScoreView from, ScoreView to) {
    bool fromRoll = from == ScoreView::Roll || from == ScoreView::Piano;
    bool toRoll = to == ScoreView::Roll || to == ScoreView::Piano;
    RollCore& c = roll_->core();
    if (!c.path.empty()) return;                        // the roll inside a score placed: its own time
    double lead = c.xOf(c.t0);                          // the roll's gutter and margin
    if (from == ScoreView::Timeline && toRoll) {
        double span = timeline_->view()->laneW() / std::max(1e-9, timeline_->pixelsPerSecond());
        c.t0 = timeline_->start();
        c.pps = std::max(1e-3, (c.width() - lead - 8) / std::max(1e-6, span));
        roll_->changed();
    } else if (fromRoll && to == ScoreView::Timeline) {
        double span = (c.width() - lead - 8) / std::max(1e-9, c.pps);
        timeline_->setView(c.t0, timeline_->view()->laneW() / std::max(1e-6, span));
    }
}

void ScoreWindow::setView(ScoreView v) {
    ScoreView from = view_;
    view_ = v;
    for (int k = 0; k < 4; ++k) {
        tabs_[k]->value(k == (int)v);
        tabs_[k]->redraw();
    }
    if (v == ScoreView::Timeline) {
        roll_->applyText();
        roll_->hide();
        timeline_->show();
        setStatus(timeline_->status());
    } else {
        roll_->setView(v == ScoreView::Piano ? RollPanel::View::Piano : v == ScoreView::List ? RollPanel::View::List : RollPanel::View::Roll);
        timeline_->hide();
        roll_->show();
        setStatus(roll_->status());
    }
    if (from != v) syncTime(from, v);
    doc_->lastView = (int)v;                            // the view it opens in next time
    if (Box* b = doc_->box()) setScoreBoxView(b, (int)v);
    Fl_Widget* f = v == ScoreView::Timeline ? (Fl_Widget*)timeline_->view() : v == ScoreView::List ? (Fl_Widget*)roll_->list()
                                                                                               : (Fl_Widget*)roll_->rollWidget();
    f->take_focus();
    updateCrumbs();
    updateTitle();
    refresh();
    redraw();
    App::get().refreshMenus();
}

bool ScoreWindow::shows(const ScoreDoc* d) const {
    if (d == doc_.get()) return true;
    for (auto& x : target_->documents())
        if (x.get() == d) return true;
    return false;
}

std::shared_ptr<ScoreDoc> ScoreWindow::current() const {
    if (view_ != ScoreView::Timeline)
        if (auto f = target_->focused()) return f;
    return doc_;
}

void ScoreWindow::updateTitle() {
    std::string t = (view_ == ScoreView::Timeline ? doc_->title() : target_->title()) + " — " + scoreViewName(view_) + " — Moritz";
    if (!label() || t != label()) copy_label(t.c_str());
}

void ScoreWindow::setStatus(const std::string& s) {
    if (s == message_) return;
    message_ = s;
    messageBox_->copy_label((s + "  ").c_str());
    messageBox_->redraw();
}

std::string ScoreWindow::crumbs() const {
    std::string c;
    for (auto& d : doc_->chain()) c += d->title() + "|";
    if (view_ != ScoreView::Timeline) {
        auto names = roll_->core().crumbs();
        for (size_t k = 1; k < names.size(); ++k) c += names[k] + "|";
    }
    return c;
}

void ScoreWindow::updateCrumbs() {
    auto chain = doc_->chain();
    std::vector<std::string> inner;
    if (view_ != ScoreView::Timeline) {
        auto names = roll_->core().crumbs();
        inner.assign(names.size() > 1 ? names.begin() + 1 : names.end(), names.end());
    }
    std::string info;
    if (view_ == ScoreView::Timeline) {
        if (doc_->parent()) {
            int which = 0, n = timeline::instances(doc_->parent()->score(), &doc_->score(), &which, doc_->placement());
            if (n > 0) info = "instance " + std::to_string(which) + " of " + std::to_string(n) + "     ";
        }
        if (doc_->boxGone()) info += "its box is gone: the score is kept here     ";
    } else {
        info = roll_->crumbInfo();
    }
    std::string key = crumbs() + "\n" + std::to_string(w());
    if (key == crumbKey_ && crumbInfo_) {
        if (info != (crumbInfo_->label() ? crumbInfo_->label() : "")) { crumbInfo_->copy_label(info.c_str()); crumbInfo_->redraw(); }
        return;
    }
    crumbKey_ = key;
    crumbs_->clear();
    crumbDocs_.clear();
    rollDepth_ = (int)inner.size();
    crumbs_->begin();
    int bx = crumbs_->x() + 8, cy = crumbs_->y() + 2, chh = kCrumbsH - 4;
    fl_font(FL_HELVETICA, 12);
    auto sep = [&] {
        auto* s = new Fl_Box(bx, cy, 14, chh, "›");
        s->labelcolor(kDim);
        s->labelsize(12);
        bx += 14;
    };
    size_t total = chain.size() + inner.size();
    size_t k = 0;
    for (auto& d : chain) {
        crumbDocs_.push_back(d);
        bool last = ++k == total;
        std::string t = d->title();
        auto* b = new Fl_Button(bx, cy, (int)fl_width(t.c_str()) + 16, chh);
        b->copy_label(t.c_str());
        b->box(FL_FLAT_BOX);
        b->color(fl_rgb_color(30, 32, 40));
        b->labelsize(12);
        b->labelcolor(last ? kText : kAccent);
        b->clear_visible_focus();
        if (d != doc_) {                                 // a score this one is placed in: its window
            b->tooltip("Open this score");
            b->callback([](Fl_Widget* w, void* data) {
                auto* sw = static_cast<ScoreWindow*>(w->window());
                size_t i = (size_t)(intptr_t)data;
                if (i < sw->crumbDocs_.size())
                    if (auto s = sw->crumbDocs_[i].lock()) openScore(s);
            }, (void*)(intptr_t)(crumbDocs_.size() - 1));
        } else if (!inner.empty()) {                     // the roll went inside: back to the top
            b->tooltip("Back to this score (Esc goes up one)");
            b->callback([](Fl_Widget* w, void*) {
                auto* sw = static_cast<ScoreWindow*>(w->window());
                RollCore& c = sw->roll_->core();
                while (!c.path.empty()) c.up();
                sw->roll_->changed();
            });
        }
        bx += b->w();
        if (!last) sep();
    }
    for (size_t i = 0; i < inner.size(); ++i) {
        bool last = ++k == total;
        auto* b = new Fl_Button(bx, cy, (int)fl_width(inner[i].c_str()) + 16, chh);
        b->copy_label(inner[i].c_str());
        b->box(FL_FLAT_BOX);
        b->color(fl_rgb_color(30, 32, 40));
        b->labelsize(12);
        b->labelcolor(last ? kText : kAccent);
        b->clear_visible_focus();
        if (!last) {
            b->tooltip("Up to this score");
            b->callback([](Fl_Widget* w, void* data) {
                auto* sw = static_cast<ScoreWindow*>(w->window());
                RollCore& c = sw->roll_->core();
                size_t depth = (size_t)(intptr_t)data + 1;
                while (c.path.size() > depth) c.up();
                sw->roll_->changed();
            }, (void*)(intptr_t)i);
        }
        bx += b->w();
        if (!last) sep();
    }
    crumbInfo_ = new Fl_Box(std::max(bx + 20, crumbs_->x() + crumbs_->w() / 2), cy,
                            std::max(10, crumbs_->x() + crumbs_->w() - 10 - std::max(bx + 20, crumbs_->x() + crumbs_->w() / 2)), chh);
    crumbInfo_->labelsize(11);
    crumbInfo_->labelcolor(kDim);
    crumbInfo_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    crumbInfo_->copy_label(info.c_str());
    crumbs_->end();
    crumbs_->redraw();
}

void ScoreWindow::refresh() {
    App& app = App::get();
    bool on = app.audio.running();
    if (audioBtn_->value() != (on ? 1 : 0) || !audioBtn_->label() || std::string(audioBtn_->label()) != (on ? "Audio On" : "Audio Off")) {
        audioBtn_->value(on);
        audioBtn_->copy_label(on ? "Audio On" : "Audio Off");
        audioBtn_->redraw();
    }
    // the button shows what the transport really does: its state and its icon each set from it (a click toggles the
    // button's state itself before its callback, so the state alone can agree while the icon does not)
    bool playing = play_->playing();
    ToolButton::Icon icon = playing ? ToolButton::Stop : ToolButton::Play;
    if (playBtn_->value() != (playing ? 1 : 0) || playBtn_->icon() != icon) {
        playBtn_->value(playing);
        playBtn_->setIcon(icon);
        playBtn_->redraw();
    }
    if (loopBtn_->value() != (play_->loop() ? 1 : 0)) {
        loopBtn_->value(play_->loop());
        loopBtn_->redraw();
    }
    double ph = play_->position();
    std::string pos = timeline::bbt(doc_->score(), ph) + "  " + clockText(ph);
    if (pos != positionText_) {
        positionText_ = pos;
        position_->copy_label(pos.c_str());
        position_->redraw();
    }
    std::string inf;
    if (view_ == ScoreView::Timeline) inf = timeline_->info();
    else {
        RollCore& c = roll_->core();
        inf = "  " + c.summary();
        if (!c.selection.empty()) inf += " · " + std::to_string(c.selection.size()) + " selected";
    }
    if (inf != info_) {
        info_ = inf;
        infoBox_->copy_label(inf.c_str());
        infoBox_->redraw();
        updateCrumbs();
    }
}

bool ScoreWindow::recording() const { return recorder_ && recorder_->recording(); }

// the recording's options, kept with the session: a count-in, punch in and out at the loop's range, a latency offset
void ScoreWindow::recordOptions() {
    mt::SessionSettings v = mt::sessionSettings(doc_->score());
    Fl_Menu_Item items[] = {{"No Count-in", 0, nullptr, nullptr, FL_MENU_RADIO | (v.countIn == 0 ? FL_MENU_VALUE : 0), 0, 0, 0, 0},
                            {"Count-in: One Bar", 0, nullptr, nullptr, FL_MENU_RADIO | (v.countIn == 1 ? FL_MENU_VALUE : 0), 0, 0, 0, 0},
                            {"Count-in: Two Bars", 0, nullptr, nullptr, FL_MENU_RADIO | FL_MENU_DIVIDER | (v.countIn == 2 ? FL_MENU_VALUE : 0), 0, 0, 0, 0},
                            {"Punch In and Out at the Loop's Range", 0, nullptr, nullptr, FL_MENU_TOGGLE | (v.punch ? FL_MENU_VALUE : 0), 0, 0, 0, 0},
                            {"Crossfade Overlapping Clips", 0, nullptr, nullptr, FL_MENU_TOGGLE | FL_MENU_DIVIDER | (v.crossfade ? FL_MENU_VALUE : 0), 0, 0, 0, 0},
                            {"Latency Offset...", 0, nullptr, nullptr, 0, 0, 0, 0, 0},
                            {nullptr, 0, nullptr, nullptr, 0, 0, 0, 0, 0}};
    const Fl_Menu_Item* m = items[0].popup(recordBtn_->x(), recordBtn_->y() + recordBtn_->h());
    if (!m) return;
    long k = m - items;
    std::string what;
    if (k <= 2) { v.countIn = (int)k; what = "Count-in"; }
    else if (k == 3) {
        v.punch = !v.punch;
        if (v.punch && !(play_->loopTo() > play_->loopFrom())) {
            setStatus("punch in and out at the loop's range: drag in the ruler to set it first");
            return;
        }
        v.punchIn = play_->loopFrom();
        v.punchOut = play_->loopTo();
        what = "Punch";
    } else if (k == 4) { v.crossfade = !v.crossfade; what = "Crossfades"; }
    else if (k == 5) {
        const char* r = fl_input("Samples added to the device's latency (%d reported now): a take is moved that much earlier",
                                 std::to_string(v.latency).c_str(), App::get().audio.latencyFrames());
        if (!r) return;
        v.latency = std::atoi(r);
        what = "Latency Offset";
    }
    if (!mt::setSession(*doc_, v, what)) setStatus(doc_->message);
    else setStatus(what + ": " + (k == 3 ? (v.punch ? "on" : "off") : k == 4 ? (v.crossfade ? "on" : "off") : "set"));
}

bool ScoreWindow::toggleRecord(int latency) {
    if (!recorder_) return false;
    if (recorder_->recording()) {
        std::string m;
        recorder_->stop(m);
        setStatus(m);
        timeline_->contentsChanged();
        refresh();
        return true;
    }
    if (doc_->path().empty()) {                          // its takes go next to its file: saved first
        setStatus("save the session first: its recordings go next to its file");
        if (!saveAs() || doc_->path().empty()) { refresh(); return false; }
    }
    App& app = App::get();
    if (latency < 0) {
        if (!app.audio.running()) app.toggleAudio();
        if (!app.audio.running()) { setStatus("audio is off: turn it on to record"); refresh(); return false; }
        app.audio.updateInputNeed();
        latency = app.audio.latencyFrames();
    }
    std::string err;
    bool deaf = app.audio.running() && !app.audio.hasInput();
    if (!recorder_->start(doc_, &app.host, play_->startPoint(), latency, err)) {
        setStatus(err);
        refresh();
        return false;
    }
    setStatus("recording from " + timeline::bbt(doc_->score(), recorder_->startedAt()) + " (latency " + std::to_string(latency) + " samples)" +
              (deaf ? ": but the device has no input open (Audio Setup), the takes will be silent" : ""));
    refresh();
    return true;
}

void ScoreWindow::tick(void* data) {
    auto* sw = static_cast<ScoreWindow*>(data);
    bool was = sw->playBtn_->value() != 0;
    sw->play_->tick();
    if (sw->recorder_ && sw->recorder_->recording() && !sw->play_->running()) sw->toggleRecord();   // stopped: the takes kept
    if (sw->recordBtn_ && sw->recordBtn_->value() != (sw->recording() ? 1 : 0)) {
        sw->recordBtn_->value(sw->recording());
        sw->recordBtn_->redraw();
    }
    if (sw->drawer_ && sw->timeline_->currentTrack() != sw->drawer_->track()) sw->drawer_->setTrack(sw->timeline_->currentTrack());
    sw->refresh();
    if (was && !sw->play_->playing() && !sw->play_->message.empty()) sw->setStatus(sw->play_->message);
    Fl::repeat_timeout(1.0 / 30, tick, data);
}

bool ScoreWindow::save() {
    roll_->applyText();
    bool ok = saveScoreDocument(current(), [this](const std::string& m) { setStatus(m); });
    updateTitle();
    return ok;
}

bool ScoreWindow::saveAs() {
    roll_->applyText();
    bool ok = saveScoreDocumentAs(current(), [this](const std::string& m) { setStatus(m); });
    updateTitle();
    return ok;
}

bool ScoreWindow::confirmDiscard() {
    roll_->applyText();
    if (!confirmDiscardScoreDocument(doc_, this)) return false;
    for (auto& d : target_->documents())
        if (d != doc_ && !confirmDiscardScoreDocument(d, this)) return false;
    return true;
}

void ScoreWindow::undo() {
    if (view_ == ScoreView::Timeline) timeline_->undo();
    else roll_->undo();
}

void ScoreWindow::redo() {
    if (view_ == ScoreView::Timeline) timeline_->redo();
    else roll_->redo();
}

int ScoreWindow::handle(int e) {
    if (e == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        if (view_ != ScoreView::Timeline && roll_->core().key(FL_Escape, 0)) roll_->changed();
        else if (play_->playing()) { play_->stop(); refresh(); redraw(); }
        return 1;
    }
    // Space plays or stops, whatever is selected (not while typing)
    if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && Fl::event_key() == ' ' && !Fl::event_state(FL_COMMAND | FL_CTRL | FL_ALT) &&
        !dynamic_cast<Fl_Input*>(Fl::focus()) && !dynamic_cast<Fl_Text_Editor*>(Fl::focus())) {
        playBtn_->do_callback();
        return 1;
    }
    int r = Fl_Double_Window::handle(e);
    if (e == FL_FOCUS || e == FL_SHOW) App::get().refreshMenus();
    return r;
}

// ================================================================ saving and closing a score's windows

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
        if (w != except && w->shows(dp.get())) return true;
    std::string name = dp->path().empty() ? std::string("Untitled score") : baseOf(dp->path());
    int r = askUser("Save changes to the score \"%s\"?", "Cancel", "Save", "Don't Save", name.c_str());
    if (r == 0) return false;
    if (r == 1) return saveScoreDocument(dp, [](const std::string&) {});
    return true;
}

// ================================================================ the application's side

ScoreWindow* openScore(std::shared_ptr<ScoreDoc> doc, int view, bool arranger) {
    if (!doc) return nullptr;
    for (auto* w : windows())
        if (w->docPtr() == doc) {
            if (view >= 0 && view <= 3) w->setView((ScoreView)view);
            showInFront(w);
            return w;
        }
    ScoreView v = ScoreView::Timeline;
    if (view >= 0 && view <= 3) v = (ScoreView)view;
    else if (Box* b = doc->box()) v = (ScoreView)std::clamp(scoreBoxView(b), 0, 3);   // used last, its @view, or Roll
    else if (doc->lastView >= 0 && doc->lastView <= 3) v = (ScoreView)doc->lastView;
    fl_open_display();
    auto* w = new ScoreWindow(doc, v, arranger || isSession(doc->score()));
    int k = (int)windows().size();
    centerOnScreen(w, App::get().frontDoc(), 24 * (k % 6), 24 * (k % 6));
    windows().push_back(w);
    showInFront(w);
    return w;
}

ScoreWindow* openScoreBox(Box* box, int view) { return openScore(ScoreDoc::ofBox(box), view); }

ScoreWindow* openScoreFile(const std::string& given) {
    std::string path = fileio::absolute(given);
    std::string err;
    auto d = ScoreDoc::open(path, err);
    if (!d) {
        alertUser("Can't open %s:\n%s", baseOf(path).c_str(), err.c_str());
        return nullptr;
    }
    App::get().addRecent(path);
    App::get().log("opened score " + path);
    return openScore(d);
}

void openScoreDialog() {
    Fl_Native_File_Chooser fc;
    fc.title("Open Score");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Score\t*.score\nAll files\t*");
    if (fc.show() != 0) return;
    openScoreFile(portablePath(fc.filename()));
}

ScoreWindow* newScore() { return openScore(ScoreDoc::make("untitled"), (int)ScoreView::Timeline); }

ScoreWindow* frontScore() { return dynamic_cast<ScoreWindow*>(Fl::first_window()); }

const std::vector<ScoreWindow*>& scoreWindows() { return windows(); }

void closeScoreWindow(ScoreWindow* w) {
    auto& all = windows();
    all.erase(std::remove(all.begin(), all.end(), w), all.end());
    if (w->playback().startedHere()) w->playback().stop();   // its own play (the patch's clock goes on)
    w->hide();
    Fl::delete_widget(w);
    App::get().refreshMenus();
}

void closeAllScores() {
    auto all = std::move(windows());
    windows().clear();
    for (auto* w : all) {
        w->hide();
        delete w;                                // its render and waveform jobs stopped and waited for
    }
}

bool confirmCloseScores() {
    for (auto* w : std::vector<ScoreWindow*>(windows())) {
        showInFront(w);
        if (!w->confirmDiscard()) return false;
    }
    return true;
}

// ================================================================ the tests' actions
//   act=tlopen:<box id>              the score window of a score box of the front document, in the Timeline view
//   act=roll:<box id>                the same, in the Roll view (act=dblclick on the box: the view used last)
//   act=tlfile:<path>, act=tlnew     File > Open Score..., File > New Score
//   act=scview:<timeline|roll|piano|list>   the window's view switcher (act=tlview, act=rollview the same;
//                                    act=rolltimeline: Timeline)
//   act=scplay                       the transport bar's Play button; act=scspace: the Space key (to the focused view)
//   act=scpress[:n]                  the Play button clicked with the mouse n times in a row (1)
//   act=scpatchplay:<seconds>        the patch's transport located there and started, as a transport object does
//   act=sctick                       the window's timer once (the end of the score)
//   act=scstate                      print: the view, whether the score plays, the transport, the button, the position;
//                                    the button's icon and state, and the status line
//   act=timeline                     print: the front window's timeline (title, tracks, rows and their heights, clips,
//                                    undo, edited, selection, status, crumbs, windows)
//   act=tlclick:<id>[:shift]         a click on a clip; act=tldbl:<id> a double-click
//   act=tldrag:<id>,<seconds>[,<tracks>]   a clip dragged by the mouse (its body), released
//   act=tltrim:<id>,<left|right>,<seconds> its edge dragged to that time, released
//   act=tlpoint:<track>,<lane>,<seconds>,<value>   a click on a lane: a point there
//   act=tlmute:<track>, act=tlsolo:<track>   the header's M or S button
//   act=tlname:<track>[,<lane>]      a click on the name in a track's (or lane's) header
//   act=tlheaddbl:<track>[,<lane>]   a double-click on the header, away from its name and buttons
//   act=tledge:<track>[,<lane>],<dy>[,alt]   the lower edge of a header dragged dy pixels (alt: every one)
//   act=tlfold:<track>[,<lane>][,alt]   a double-click on the lower edge (alt: every one)
//   act=tlop:<op>                    undo redo cut copy paste duplicate delete split unique selectall addtrack fit play stop
//                                    render (saveas=<path> answers where)
//   act=tlplayhead:<seconds>, act=tlsnap:<off|beat|bar>, act=tladdsound:<file>, act=tllane:<track>,<name>
//   act=tlsave                       File > Save (saveas=<path> answers a Save As)
//   act=tlpng:<file>, act=rollpng:<file>   the front score window drawn into a PNG file
//   act=rolldrag:<event>:<seconds>,<steps>[:alt]   an event dragged by its head that far in time and that many steps
//                                    up its staff (alt: Alt held, a copy)
//   act=rollclick:<id>[:shift]       a click on an event's head in the roll (no drag; shift: Shift held)
//   act=rollaudition:<on|off>        the roll's Audition button set so
//   act=rollundo, act=rollredo, act=rolltext:<file>, act=rollselect:<id>,..., act=rollkey:<key>[:alt][:shift],
//   act=rollcell:<id>:<key>:<value>, act=rollstate   as the roll's (see guitest.cpp)
//   act=rolldynrow:<k>               the dynamics lane's row chooser set to its k-th item; print: its label, the row the
//                                    lane shows, and whether the lane, drawn alone, paints over the chooser
//   act=selected                     print: the selected boxes of every open document
//   act=transports                   print: whether each open document's transport plays, and each score window's clock
namespace {

ScoreWindow* front() {
    if (auto* w = frontScore()) return w;
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

void mouse(Fl_Widget* v, int ex, int ey, int event, int clicks, int keys = 0) {
    Fl::e_x = Fl::e_x_root = ex;
    Fl::e_y = Fl::e_y_root = ey;
    Fl::e_state = keys | (event == FL_RELEASE ? 0 : FL_BUTTON1);
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

bool writePng(Fl_Window* w, const std::string& file) {
    Fl_Image_Surface surf(w->w(), w->h());
    Fl_Surface_Device::push_current(&surf);
    surf.draw(w);
    Fl_Surface_Device::pop_current();
    Fl_RGB_Image* img = surf.image();
    int r = fl_write_png(file.c_str(), img);
    delete img;
    return r == 0;
}

// a row of the timeline's view: a track (lane "") or one of its lanes
const TimelineView::Row* rowOf(TimelineView* v, const std::string& track, const std::string& lane) {
    for (auto& r : v->rows())
        if (r.track == track && (lane.empty() ? r.lane < 0 : r.laneName == lane)) return &r;
    return nullptr;
}

bool rollAction(ScoreWindow* w, const std::string& verb, const std::string& arg) {
    RollPanel* panel = w->roll();
    if (verb == "rollpng") return writePng(w, arg);
    if (verb == "rollview") {
        ScoreView v;
        if (!parseScoreView(arg, v)) v = ScoreView::Roll;
        w->setView(v);
        return true;
    }
    if (verb == "rollclick") {                     // act=rollclick:<id>[:shift]: a click on an event's head (no drag)
        RollWidget* rw = panel->rollWidget();
        RollCore& c = rw->core();
        double x, y, ww, h;
        if (!c.eventBox(std::strtoull(arg.c_str(), nullptr, 10), x, y, ww, h)) return false;
        int keys = arg.find(":shift") != std::string::npos ? FL_SHIFT : 0;
        int ex = rw->x() + (int)std::lround(x + h / 2), ey = rw->y() + (int)std::lround(y + h / 2);
        mouse(rw, ex, ey, FL_PUSH, 0, keys);
        mouse(rw, ex, ey, FL_RELEASE, 0, keys);
        panel->changed();
        return true;
    }
    if (verb == "rollaudition") {                  // act=rollaudition:on|off: the toolbar's Audition button clicked to that
        ToolButton* b = panel->auditionButton();
        if ((b->value() != 0) != (arg == "on")) {
            b->value(arg == "on");
            b->do_callback();
        }
        return true;
    }
    if (verb == "rollundo") { panel->undo(); return true; }
    if (verb == "rollredo") { panel->redo(); return true; }
    if (verb == "rolltimeline") { w->setView(ScoreView::Timeline); return true; }
    if (verb == "rollselect") {
        RollCore& c = panel->core();
        c.selection.clear();
        for (auto& x : split(arg, ',')) c.selection.insert(std::strtoull(x.c_str(), nullptr, 10));
        panel->changed();
        return true;
    }
    if (verb == "rollkey") {
        int key = arg.rfind("up", 0) == 0 ? FL_Up : arg.rfind("down", 0) == 0 ? FL_Down : arg.rfind("left", 0) == 0 ? FL_Left : FL_Right;
        int keys = (arg.find(":alt") != std::string::npos ? FL_ALT : 0) | (arg.find(":shift") != std::string::npos ? FL_SHIFT : 0);
        panel->core().key(key, keys);
        panel->changed();
        return true;
    }
    if (verb == "rollcell") {
        size_t c1 = arg.find(':'), c2 = c1 == std::string::npos ? c1 : arg.find(':', c1 + 1);
        if (c2 == std::string::npos) return false;
        w->setView(ScoreView::List);
        panel->editCell(std::strtoull(arg.c_str(), nullptr, 10), arg.substr(c1 + 1, c2 - c1 - 1), arg.substr(c2 + 1), 10, 10, 60);
        panel->commitCell();
        return true;
    }
    if (verb == "rolltext") {
        std::string src;
        if (!fileio::read(arg, src)) return false;
        panel->setText(src);
        if (!panel->applyText()) say("roll text error " + panel->textError());
        return true;
    }
    if (verb == "rollstate") {
        RollEditTarget* t = panel->target();
        say(std::string("roll ") + (w->label() ? w->label() : ""));
        auto s = t ? t->score() : nullptr;
        if (s)
            for (auto* e : s->byTime())
                if (e->kind == sc::Kind::Note) {
                    say("roll note " + std::to_string(e->id) + " " + sc::numText(e->at) + " " + sc::numText(e->dur) + " " + sc::numText(e->pitch));
                    if (e->cents != 0) say("roll cents " + std::to_string(e->id) + " " + sc::numText(e->cents));
                }
        std::string sel;
        for (uint64_t id : panel->core().selection) sel += " " + std::to_string(id);
        say("roll selection" + sel);
        say(std::string("roll undo ") + (t && t->canUndo() ? "yes" : "no") + " redo " + (t && t->canRedo() ? "yes" : "no"));
        say("roll windows " + std::to_string(windows().size()) + " view " +
            (w->view() == ScoreView::Piano ? "piano" : w->view() == ScoreView::Timeline ? "timeline" : scoreViewName(w->view())));
        std::string text = panel->text();
        for (size_t k = 0; k < text.size();) {
            size_t end = text.find('\n', k);
            std::string line = text.substr(k, end == std::string::npos ? std::string::npos : end - k);
            size_t at = line.find("(note");
            if (at != std::string::npos) say("roll text " + line.substr(at));
            k = end == std::string::npos ? text.size() : end + 1;
        }
        return true;
    }
    if (verb == "rolldrag") {
        uint64_t id = std::strtoull(arg.c_str(), nullptr, 10);
        size_t c1 = arg.find(':');
        if (c1 == std::string::npos) return false;
        double dt = std::atof(arg.c_str() + c1 + 1);
        size_t comma = arg.find(',', c1);
        double steps = comma == std::string::npos ? 0 : std::atof(arg.c_str() + comma + 1);
        int keys = arg.find(":alt") != std::string::npos ? FL_ALT : 0;
        RollWidget* rw = panel->rollWidget();
        RollCore& c = rw->core();
        double x, y, ww, h;
        if (!c.eventBox(id, x, y, ww, h)) return false;
        int row = c.hit(x + h / 2, y + h / 2).row;
        double sp = row >= 0 && row < (int)c.rows().size() ? c.rows()[(size_t)row].sp : 0;
        int ex = rw->x() + (int)std::lround(x + h / 2), ey = rw->y() + (int)std::lround(y + h / 2);
        int ex2 = ex + (int)std::lround(dt * c.pps), ey2 = ey - (int)std::lround(steps * sp / 2);
        mouse(rw, ex, ey, FL_PUSH, 0, keys);
        mouse(rw, (ex + ex2) / 2, (ey + ey2) / 2, FL_DRAG, 0, keys);
        mouse(rw, ex2, ey2, FL_DRAG, 0, keys);
        mouse(rw, ex2, ey2, FL_RELEASE, 0, keys);
        panel->changed();
        return true;
    }
    if (verb == "rollclef") {                      // act=rollclef:<row>: how the roll's clef of that row is drawn
        auto k = panel->core().clefInk(std::atoi(arg.c_str()));
        char spread[96];
        std::snprintf(spread, sizeof spread, " (lower %.2f top %.2f bottom %.2f mean %.2f)", k.lower, k.topX, k.bottomX, k.meanX);
        say("roll clef " + arg + " ink " + (k.drawn && k.ink > 0.04 ? "yes" : "no") + " above " + (k.above ? "yes" : "no") +
            " below " + (k.below ? "yes" : "no") + " upright " + (k.upright ? "yes" : "no") + spread);
        return true;
    }
    if (verb == "rolldynrow") {
        Fl_Choice* ch = panel->dynamicsRow();
        DynamicsLane* lane = panel->dynamicsLane();
        int k = std::atoi(arg.c_str());
        if (k < 0 || k >= ch->size() - 1) return false;
        ch->value(k);
        ch->do_callback();
        // the lane drawn alone (as when only it is redrawn), over a colour of its own: is the chooser still there?
        Fl_Image_Surface surf(lane->w(), lane->h());
        Fl_Surface_Device::push_current(&surf);
        fl_color(fl_rgb_color(255, 0, 255));
        fl_rectf(0, 0, lane->w(), lane->h());
        surf.draw(lane, 0, 0);
        Fl_Surface_Device::pop_current();
        Fl_RGB_Image* img = surf.image();
        bool covered = true;
        int px = ch->x() - lane->x() + ch->w() / 2, py = ch->y() - lane->y() + ch->h() / 2;
        if (img && px >= 0 && py >= 0 && px < img->w() && py < img->h()) {
            const unsigned char* d = (const unsigned char*)img->data()[0];
            int dpix = img->d(), ld = img->ld() ? img->ld() : img->w() * dpix;
            const unsigned char* q = d + py * ld + px * dpix;
            covered = !(q[0] > 240 && q[1] < 20 && q[2] > 240);
        }
        delete img;
        const char* lab = ch->text(ch->value());
        say(std::string("roll dynrow ") + (lab ? lab : "?") + " lane " + (lane->row.empty() ? "all" : lane->row) + " covered " +
            (covered ? "yes" : "no") + " shown " + (ch->visible() && ch->parent() == lane->parent() ? "yes" : "no"));
        return true;
    }
    return false;
}

}  // namespace

bool scoreTestAction(const std::string& verb, const std::string& arg) {
    App& app = App::get();
    if (verb == "tlopen" || verb == "roll") {
        DocumentWindow* d = app.docs.empty() ? nullptr : app.docs.front();
        Box* b = d ? d->patch.find(arg) : nullptr;
        return b && openScoreBox(b, (int)(verb == "tlopen" ? ScoreView::Timeline : ScoreView::Roll));
    }
    if (verb == "tlfile") return openScoreFile(arg) != nullptr;
    if (verb == "tlnew") return newScore() != nullptr;
    if (verb == "selected") {
        for (auto* d : app.docs) {
            std::string sel;
            d->patch.forEachBox([&](Box* b) { if (b->selected) sel += " " + b->id; });
            say("selected " + baseOf(d->path().empty() ? std::string("Untitled") : d->path()) + ":" + sel);
        }
        return true;
    }
    if (verb == "transports") {                    // each open document's transport, and each score window's clock
        for (auto* d : app.docs)
            say("transport " + baseOf(d->path().empty() ? std::string("Untitled") : d->path()) + " " +
                (transportOf(&d->patch)->playing() ? "playing" : "stopped"));
        for (auto* sw : windows())
            say("transport score " + sw->doc().title() + " " + (sw->playback().running() ? "playing" : "stopped"));
        return true;
    }
    ScoreWindow* w = front();
    if (!w && (verb == "timeline" || verb == "scstate")) { say(verb + " none"); return true; }
    if (!w) return false;
    if (verb.rfind("roll", 0) == 0) return rollAction(w, verb, arg);
    TimelinePanel* p = w->timeline();
    TimelineView* v = p->view();
    ScoreDoc& d = p->doc();
    const sc::Score& s = d.score();
    ScorePlayback& pb = w->playback();
    auto centre = [&](uint64_t id, int& ex, int& ey) {
        auto e = s.find(id);
        int X, Y, W, H;
        if (!e || !v->clipRect(*e, X, Y, W, H)) return false;
        ex = X + W / 2;
        ey = Y + H / 2;
        return true;
    };
    if (verb == "scview" || verb == "tlview") {
        ScoreView sv;
        if (!parseScoreView(arg, sv)) return false;
        w->setView(sv);
        return true;
    }
    if (verb == "scplay") {
        w->playButton()->do_callback();
        return true;
    }
    if (verb == "scpress") {                       // the Play button clicked with the mouse (its state toggled by the click)
        Fl_Widget* b = w->playButton();
        int n = arg.empty() ? 1 : std::max(1, std::atoi(arg.c_str()));
        for (int k = 0; k < n; ++k) {
            mouse(b, b->x() + b->w() / 2, b->y() + b->h() / 2, FL_PUSH, 0);
            mouse(b, b->x() + b->w() / 2, b->y() + b->h() / 2, FL_RELEASE, 0);
            Fl::check();
        }
        return true;
    }
    if (verb == "scspace") {                       // Space, as the keyboard sends it: to the focused widget, then up
        Fl::e_keysym = ' ';
        Fl::e_state = 0;
        static char sp[] = " ";
        Fl::e_text = sp;
        Fl::e_length = 1;
        Fl::e_number = FL_KEYBOARD;
        Fl_Widget* f = Fl::focus();
        if (!f || f->window() != w) f = w;
        for (Fl_Widget* x = f; x; x = x->parent())
            if (x->handle(FL_KEYBOARD)) return true;
        Fl::e_number = FL_SHORTCUT;
        w->handle(FL_SHORTCUT);
        return true;
    }
    if (verb == "scpatchplay") {
        Patch* c = pb.clock();
        auto t = transportOf(c);
        t->locate(c, std::atof(arg.c_str()));
        t->play(c);
        return true;
    }
    if (verb == "sctick") {
        pb.tick();
        w->refresh();
        return true;
    }
    if (verb == "scstate") {
        w->refresh();                              // as its timer does
        say(std::string("score view ") + scoreViewName(w->view()) + " playing " + (pb.playing() ? "yes" : "no") + " transport " +
            (pb.running() ? "yes" : "no") + " button " + (w->playButton()->value() ? "on" : "off") + " position " +
            num(std::round(pb.position() * 10) / 10) + " playhead " + num(pb.playhead()));
        say(std::string("score button ") + (w->playButton()->icon() == ToolButton::Stop ? "stop" : "play") + " " +
            (w->playButton()->value() ? "on" : "off") + " status " + w->status());
        return true;
    }
    if (verb == "timeline") {
        say("timeline " + std::string(w->label() ? w->label() : ""));
        std::string t;
        for (auto& x : timeline::tracks(s)) t += x + "|";
        say("timeline tracks " + t);
        for (auto& r : v->rows()) say("timeline row " + r.track + (r.lane >= 0 ? "/" + r.laneName : std::string()) + " " + std::to_string(r.h));
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
        int keys = arg.find(":shift") != std::string::npos ? FL_SHIFT : 0;
        int clicks = verb == "tldbl" ? 1 : 0;
        mouse(v, ex, ey, FL_PUSH, clicks, keys);
        mouse(v, ex, ey, FL_RELEASE, clicks, keys);
        return true;
    }
    if (verb == "tldrag") {
        auto a = split(arg, ',');
        int ex, ey;
        if (a.size() < 2 || !centre(std::strtoull(a[0].c_str(), nullptr, 10), ex, ey)) return false;
        int tx = ex + (int)std::lround(std::atof(a[1].c_str()) * p->pixelsPerSecond());
        int ty = ey;
        if (a.size() > 2 && std::atoi(a[2].c_str()) != 0) {     // past the lanes in between: the track's own row
            auto e = s.find(std::strtoull(a[0].c_str(), nullptr, 10));
            auto all = timeline::tracks(s);
            int k = timeline::trackIndex(s, e->row) + std::atoi(a[2].c_str());
            if (k >= 0 && k < (int)all.size())
                if (auto* r = rowOf(v, all[(size_t)k], "")) ty = v->rowTop(*r) + r->h / 2;
        }
        mouse(v, ex, ey, FL_PUSH, 0);
        mouse(v, (ex + tx) / 2, (ey + ty) / 2, FL_DRAG, 0);
        mouse(v, tx, ty, FL_DRAG, 0);
        mouse(v, tx, ty, FL_RELEASE, 0);
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
        mouse(v, ex, ey, FL_PUSH, 0);
        mouse(v, tx, ey, FL_DRAG, 0);
        mouse(v, tx, ey, FL_RELEASE, 0);
        return true;
    }
    if (verb == "tlpoint") {
        auto a = split(arg, ',');
        if (a.size() < 4) return false;
        auto* r = rowOf(v, a[0], a[1]);
        if (!r) return false;
        double val = std::atof(a[3].c_str());
        int Y = v->rowTop(*r);
        int ey = Y + 4 + (int)std::lround((1 - (val - r->lo) / (r->hi - r->lo)) * (r->h - 8));
        int ex = p->xOf(std::atof(a[2].c_str()));
        mouse(v, ex, ey, FL_PUSH, 0);
        mouse(v, ex, ey, FL_RELEASE, 0);
        return true;
    }
    if (verb == "tlmute" || verb == "tlsolo") {
        auto* r = rowOf(v, arg, "");
        if (!r) return false;
        int by = v->rowTop(*r) + std::min(8, std::max(0, (r->h - 17) / 2));
        int ex = v->x() + v->headW() - 50 + (verb == "tlsolo" ? 22 : 0) + 8, ey = by + 6;
        mouse(v, ex, ey, FL_PUSH, 0);
        mouse(v, ex, ey, FL_RELEASE, 0);
        return true;
    }
    if (verb == "tlname" || verb == "tlheaddbl") {
        auto a = split(arg, ',');
        auto* r = rowOf(v, a[0], a.size() > 1 ? a[1] : "");
        if (!r) return false;
        int Y = v->rowTop(*r);
        int ex, ey;
        if (verb == "tlname") {                    // on the name
            ex = v->x() + (r->lane >= 0 ? 16 : 12) + 3;
            ey = Y + std::min(r->lane >= 0 ? 15 : 20, r->h - 5) - 4;
        } else {                                   // away from it: under it, left of the buttons
            ex = v->x() + v->headW() - 70;
            ey = Y + r->h / 2 + 4;
        }
        if (verb == "tlname" && !v->onName(*r, ex, ey)) return false;
        mouse(v, ex, ey, FL_PUSH, verb == "tlheaddbl" ? 1 : 0);
        mouse(v, ex, ey, FL_RELEASE, 0);
        return true;
    }
    if (verb == "tledge" || verb == "tlfold") {
        auto a = split(arg, ',');
        bool alt = !a.empty() && a.back() == "alt";
        if (alt) a.pop_back();
        std::string lane;
        int dy = 0;
        if (verb == "tledge") {
            if (a.size() < 2) return false;
            dy = std::atoi(a.back().c_str());
            a.pop_back();
        }
        if (a.size() > 1) lane = a[1];
        auto* r = rowOf(v, a[0], lane);
        if (!r) return false;
        int ex = v->x() + 40, ey = v->rowTop(*r) + r->h - 1;
        if (v->edgeAt(ex, ey) != r) return false;
        int keys = alt ? FL_ALT : 0;
        Fl::e_state = keys;
        if (verb == "tlfold") {
            v->press(ex, ey, 1, false, false, alt);
            v->release(ex, ey);
            return true;
        }
        v->press(ex, ey, 0, false, false, alt);
        v->dragTo(ex, ey + dy / 2);
        v->dragTo(ex, ey + dy);
        v->release(ex, ey + dy);
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
            {"render", &TimelinePanel::renderDialog}, {"renderstems", &TimelinePanel::renderStems},
            {"addbus", &TimelinePanel::addBus}};
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
    if (verb == "tlpng") return writePng(w, arg);
    return false;
}

}  // namespace moritz
