// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_window.h — a score's window: one window per score document (score_doc.h) whose views, Timeline, Roll,
// Piano roll and List, are switched in place, as the views of one clip in a DAW, so that they are seen to be views of
// one thing. The window hosts the panels (the timeline's, timeline.h; the roll's, roll.h: each written once, as a
// panel that can sit in any window, mockup 8) and what they share: the view switcher and the transport bar (to start,
// play, loop, the position), the breadcrumb, the status line, Save, Undo and closing (it asks when edits only it
// shows would be lost). Switching keeps the selection (the document's), the playhead (the window's ScorePlayback) and
// the visible span of time.
//
// What opens one: a score box double-clicked (the view used last for that score; the first time the box's @view,
// Roll by default) or its menu's Open; File > Open Score... and File > New Score (the timeline); a sub-score clip
// double-clicked in the timeline (its own window, with the breadcrumb).
//
// Playing (ScorePlayback): Play plays the score from where the user last pointed (the playhead placed, or the start of
// what was selected after it), locating the transport first, so it always plays, whatever the transport was doing;
// pressed again (or Space, whatever is selected) it stops. A play started here stops at the score's end (unless it
// loops) and the playhead goes back to where it started. The transport is the patch's (transport.h): a play started by
// the patch (a transport object, a score's play message) goes on past the score's end, as the patch's clock, and the
// window shows its score as playing only while the transport is inside it; either way Play starts it again from the
// playhead. The button shows the real state.
#pragma once

#include "score_doc.h"

#include <FL/Fl_Double_Window.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Group;
class Fl_Menu_Bar;

namespace moritz {

class TimelinePanel;
class RollPanel;
class ScoreDocTarget;
class ToolButton;

enum class ScoreView { Timeline = 0, Roll = 1, Piano = 2, List = 3 };
const char* scoreViewName(ScoreView v);                  // "timeline", "roll", "piano roll", "list"
bool parseScoreView(const std::string& s, ScoreView& v); // those names (and "piano"); false: not one

// The transport as a score's views use it (GUI thread)
class ScorePlayback {
public:
    // root: the patch a score of its own plays under (nullptr: the application's, every open patch's)
    explicit ScorePlayback(std::shared_ptr<ScoreDoc> doc, Patch* root = nullptr);
    ~ScorePlayback();
    ScorePlayback(const ScorePlayback&) = delete;
    ScorePlayback& operator=(const ScorePlayback&) = delete;
    ScoreDoc& doc() const { return *doc_; }
    Patch* clock() const;                  // the patch whose transport the score plays on
    bool running() const;                  // the transport plays (whoever started it)
    bool playing() const;                  // this score plays: the transport runs inside it (or loops)
    double position() const;               // seconds: the transport's while the score plays, the playhead otherwise
    double playhead() const { return playhead_; }
    double startPoint() const;             // where Play starts
    void setPlayhead(double t);            // placed by the user (the transport goes there too while it plays)
    bool play();                           // from startPoint(), located first; false: nothing can play it (message)
    void stop();                           // the transport stops; the playhead stays where it was
    void toggle();                         // the Play button and Space
    bool loop() const { return loop_; }
    void setLoop(bool on);                 // only with a range (setLoopRange)
    double loopFrom() const { return loopA_; }
    double loopTo() const { return loopB_; }
    void setLoopRange(double a, double b);
    bool startedHere() const { return startedHere_; }
    void tick();                           // a timer: a play started here stops at the score's end
    std::string message;                   // what the last play or stop did, or why it did nothing

private:
    double transportSeconds() const;
    Patch* root() const;
    std::shared_ptr<ScoreDoc> doc_;
    Patch* root_ = nullptr;
    int token_ = 0;
    double playhead_ = 0, startAt_ = 0;
    bool loop_ = false, startedHere_ = false, selNewer_ = false;
    double loopA_ = 0, loopB_ = 0;
};

class ScoreWindow : public Fl_Double_Window {
public:
    ScoreWindow(std::shared_ptr<ScoreDoc> doc, ScoreView view);
    ~ScoreWindow() override;
    ScoreDoc& doc() { return *doc_; }
    const std::shared_ptr<ScoreDoc>& docPtr() const { return doc_; }
    ScorePlayback& playback() { return *play_; }
    TimelinePanel* timeline() const { return timeline_; }
    RollPanel* roll() const { return roll_; }
    ScoreDocTarget* rollTarget() const { return target_.get(); }
    ScoreView view() const { return view_; }
    void setView(ScoreView v);             // in place: the selection, the playhead and the time shown kept
    bool shows(const ScoreDoc* d) const;   // its document, or one placed inside it that the roll went into
    std::shared_ptr<ScoreDoc> current() const;   // the document the view edits (the roll: the score it went into)
    bool save();                           // as saveScoreDocument
    bool saveAs();
    bool confirmDiscard();                 // asks when closing would lose edits only it shows; false: cancel
    void undo();
    void redo();
    void updateTitle();
    void refresh();                        // the transport bar, the breadcrumb and the status line
    void setStatus(const std::string& s);
    const std::string& status() const { return message_; }
    const std::string& info() const { return info_; }
    std::string crumbs() const;            // the breadcrumb as text (the tests print it)
    int handle(int e) override;
    void resize(int x, int y, int w, int h) override;
    ToolButton* playButton() const { return playBtn_; }

private:
    static void tick(void* data);
    void layout();
    void updateCrumbs();
    void syncTime(ScoreView from, ScoreView to);
    std::shared_ptr<ScoreDoc> doc_;
    std::shared_ptr<ScorePlayback> play_;
    std::shared_ptr<ScoreDocTarget> target_;
    ScoreView view_ = ScoreView::Timeline;
    Fl_Menu_Bar* menu_ = nullptr;
    Fl_Group* bar_;
    ToolButton* tabs_[4];
    ToolButton* playBtn_;
    ToolButton* loopBtn_;
    ToolButton* audioBtn_;
    Fl_Box* position_;
    Fl_Group* crumbs_;
    Fl_Box* crumbInfo_;
    TimelinePanel* timeline_;
    RollPanel* roll_;
    Fl_Box* infoBox_;
    Fl_Box* messageBox_;
    std::string info_, message_, positionText_, crumbKey_;
    std::vector<std::weak_ptr<ScoreDoc>> crumbDocs_;
    int rollDepth_ = 0;                    // how many scores placed inside the roll went into (for the breadcrumb)
};

// ---- the application's side
// the window of a document (already open: brought forward), in a view (-1: the one used last for it)
ScoreWindow* openScore(std::shared_ptr<ScoreDoc> doc, int view = -1);
ScoreWindow* openScoreBox(Box* box, int view = -1);      // a score box's (nullptr: not one)
ScoreWindow* openScoreFile(const std::string& path);      // a .score file (File > Open Score...)
void openScoreDialog();
ScoreWindow* newScore();                                  // File > New Score
ScoreWindow* frontScore();                                // the score window in front, if one is
const std::vector<ScoreWindow*>& scoreWindows();
void closeScoreWindow(ScoreWindow* w);                    // already confirmed
void closeAllScores();                                    // at once, nothing asked (quitting, the tests' end)
bool confirmCloseScores();                                // Quit: each one asked; false: cancel
// the view a score box's window opens in: the one used last, else its @view, else Roll (objects_score.cpp)
int scoreBoxView(const Box* b);
void setScoreBoxView(Box* b, int view);

// saving a score as its window does: its file; a score kept in a patch: the patch; a score kept inside another: that
// one; a new score: Save As (a score of a box or placed: a copy). status: what was done
bool saveScoreDocument(const std::shared_ptr<ScoreDoc>& d, const std::function<void(const std::string&)>& status);
bool saveScoreDocumentAs(const std::shared_ptr<ScoreDoc>& d, const std::function<void(const std::string&)>& status);
// a window showing d closes (except: that window): asks when edits only it shows would be lost; false: cancel
bool confirmDiscardScoreDocument(const std::shared_ptr<ScoreDoc>& d, const Fl_Window* except);

// the test actions (guitest.cpp): act=tl..., act=roll..., act=sc...
bool scoreTestAction(const std::string& verb, const std::string& arg);

}  // namespace moritz
