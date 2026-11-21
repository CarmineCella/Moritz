// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// timeline.h — the timeline: a score seen as tracks (mockup 4). TimelinePanel is the panel (an Fl_Group that can
// sit in any window) showing a ScoreDoc (score_doc.h) through the operations of timeline_ops.h; TimelineWindow is
// the panel in a window of its own (a score box's menu's Open as Timeline, the roll's Timeline button, File > Open
// Score..., File > New Score, a sub-score clip double-clicked).
//
// The panel, top to bottom: the transport bar (to start, play, stop, loop, the position in bars.beats.ticks and in
// time, the tempo and meter, snap, zoom, the Add and Edit menus, Render, and Timeline / Roll / List: the views of the
// same document, the roll in its own window, roll.h), the breadcrumb (where this score is placed,
// when it is inside another), the view (a ruler, then for each track its header and its clips, its automation lanes
// under it), the inspector on the right (the selected clip, point or track), and the status line.
//
// Playing: the transport is the patches' (transport.h: one for every open patch); the score plays on the box it is
// bound to, or on a box of its own (ScoreDoc::player). The playhead follows the transport (a timer).
//
// For a later multitrack mode the model is ready (a track is a row with its strip; inputs, streaming and track
// patches go on the row too); what is drawn of the strip is small for now (M, S, gain and pan in the header and the
// inspector).
#pragma once

#include "gui.h"
#include "score_doc.h"
#include "sound_doc.h"
#include "timeline_ops.h"

#include <FL/Fl_Menu_Button.H>

#include <map>
#include <memory>

namespace moritz {

class TimelinePanel;

// the ruler, the track headers, the clips and the lanes
class TimelineView : public Fl_Widget {
public:
    TimelineView(int x, int y, int w, int h, TimelinePanel* p) : Fl_Widget(x, y, w, h), tp_(p) {}
    void draw() override;
    int handle(int e) override;
    static constexpr int kHeadW = 184;     // the track headers
    static constexpr int kRulerH = 26;
    static constexpr int kTrackH = 64;
    static constexpr int kLaneH = 38;
    int laneX() const { return x() + kHeadW; }
    int laneW() const { return std::max(1, w() - kHeadW); }

    // one row of the view: a track, or one of its lanes
    struct Row {
        std::string track;
        int lane = -1;                     // -1: the track itself
        std::string laneName;
        double lo = 0, hi = 1;
        int y = 0, h = 0;                  // in content coordinates (0: under the ruler)
    };
    const std::vector<Row>& rows() const { return rows_; }
    void layoutRows();                     // after the score or the lanes changed
    int contentH() const { return contentH_; }
    const Row* rowAt(int ey) const;        // a window y
    // where a clip is drawn (window coordinates); false when its track is not shown
    bool clipRect(const sc::Event& e, int& X, int& Y, int& W, int& H) const;
    uint64_t clipAt(int ex, int ey, int* zone = nullptr) const;   // zone: -1 left edge, 1 right edge, 0 body
    uint64_t pointAt(int ex, int ey) const;
    // the mouse as a user moves it (the tests drive these as the events do)
    void press(int ex, int ey, int clicks, bool shift, bool right);
    void dragTo(int ex, int ey);
    void release(int ex, int ey);

private:
    enum class Drag { None, Move, TrimL, TrimR, Point, Ruler, Loop } drag_ = Drag::None;
    void drawRuler();
    void drawHeader(const Row& r, int Y);
    void drawTrack(const Row& r, int Y);
    void drawLane(const Row& r, int Y);
    void drawClip(const sc::Event& e, int X, int Y, int W, int H, bool sel);
    void dropFiles(const std::string& text, int ex, int ey);
    int headerButton(const Row& r, int ex, int ey) const;   // 1 mute, 2 solo, 0 none
    TimelinePanel* tp_;
    std::vector<Row> rows_;
    int contentH_ = 0;
    // a drag going on: where it began, what it moves (drawn moved until it is released)
    int pressX_ = 0, pressY_ = 0;
    bool moved_ = false;
    uint64_t dragId_ = 0;
    double dragAt_ = 0, dragEnd_ = 0;
    int dragRow_ = -1;
    double dragV_ = 0;
    std::string dragTrack_;

public:
    // the drag as it shows now (for drawing): a move by dt seconds and dTrack tracks, a trimmed edge, a point
    double dragDt = 0;
    int dragDTrack = 0;
    double dragEdge = 0, dragPointT = 0, dragPointV = 0;
    bool dragging() const { return drag_ != Drag::None && moved_; }
    bool movingSelection() const { return dragging() && drag_ == Drag::Move; }
    bool trimming(uint64_t id) const { return dragging() && (drag_ == Drag::TrimL || drag_ == Drag::TrimR) && dragId_ == id; }
    bool trimmingLeft() const { return drag_ == Drag::TrimL; }
    bool movingPoint(uint64_t id) const { return dragging() && drag_ == Drag::Point && dragId_ == id; }
};

// The waveforms of the sounds clips show: decoded in the background (a file) or copied from a buffer~, kept by name
class WaveCache {
public:
    struct Entry {
        Sound sound;
        WaveSummary summary;
        bool failed = false;
        int version = -1;                  // a buffer~'s
    };
    ~WaveCache();
    // the waveform of a file (a path) or of a buffer~ of root; nullptr while it is being read (redraw then)
    const Entry* get(const std::string& path, bool buffer, Patch* root);
    std::function<void()> onReady;

private:
    void next();
    std::map<std::string, std::shared_ptr<Entry>> entries_;
    std::vector<std::string> queue_;
    std::unique_ptr<Job> job_;
};

class TimelinePanel : public Fl_Group {
public:
    TimelinePanel(int x, int y, int w, int h, std::shared_ptr<ScoreDoc> doc);
    ~TimelinePanel() override;
    ScoreDoc& doc() { return *doc_; }
    const std::shared_ptr<ScoreDoc>& docPtr() const { return doc_; }
    TimelineView* view() const { return view_; }

    // ---- the view: seconds from start(), pixelsPerSecond() of them
    double start() const { return t0_; }
    double pixelsPerSecond() const { return pps_; }
    void setView(double t0, double pps);
    void zoom(double factor, double aroundSeconds);
    void zoomIn() { zoom(1.5, centre()); }
    void zoomOut() { zoom(1 / 1.5, centre()); }
    void fit();                                // the whole score
    double centre() const;
    double timeAt(int ex) const;               // the seconds under a window x in the lanes
    int xOf(double t) const;
    int scrollY() const { return sy_; }
    void scrollYTo(int y);
    timeline::Snap snapMode() const { return snap_; }
    void setSnap(timeline::Snap s);
    double snapped(double t) const { return timeline::snap(doc_->score(), t, snap_); }

    // ---- the track that adding goes to, the playhead
    std::string currentTrack() const;
    void setCurrentTrack(const std::string& t);
    double playhead() const;                   // where the transport is (seconds), or the playhead set
    void setPlayhead(double t);                // and the transport there
    bool loop() const { return loop_; }
    void setLoop(bool on);
    double loopFrom() const { return loopA_; }
    double loopTo() const { return loopB_; }
    void setLoopRange(double a, double b);

    // ---- playing
    void togglePlay();
    void play();
    void stop();
    bool playing() const;
    Box* player();                             // the box that plays the score

    // ---- the commands (menus, keys, the inspector); each one undo step
    void cut();
    void copy();
    void paste();
    void duplicate();
    void erase();
    void splitAtPlayhead();
    void makeUnique();
    void selectAll();
    void undo();
    void redo();
    void addTrack();
    void addSoundDialog();
    void addScoreDialog();
    void addPatchDialog();
    void addLaneDialog();
    void tempoDialog();
    void renameTrackDialog(const std::string& track);
    void renderDialog();
    void showRoll(int view);                   // the Roll and List buttons: the same document in the roll (0: Roll, 2: List)
    bool addSoundFile(const std::string& path, const std::string& track, double at);   // a file dropped or chosen
    void openClip(uint64_t id);                // a sub-score: its timeline; a patch: the patch; a sound: its editor
    bool rendering() const { return render_.running(); }

    // ---- showing
    void contentsChanged();                    // the score changed
    void selectionChanged();
    void stateChanged();                       // the title, saved, bound
    void setStatus(const std::string& s);
    void refreshInfo();
    const std::string& info() const { return infoText_; }
    const std::string& status() const { return messageText_; }
    std::function<void()> onTitle;             // the window's title should change
    WaveCache& waves() { return waves_; }

    int handle(int e) override;
    void resize(int x, int y, int w, int h) override;

private:
    friend class TimelineView;
    void layoutChildren();
    void after(bool done);                     // an edit's result: the message
    void buildInspector();
    void showInspector();                      // the fields of the selection
    void applyField(int k);
    void updateScrollbars();
    void updateCrumbs();
    static void tick(void* data);
    std::shared_ptr<ScoreDoc> doc_;
    int token_ = 0;
    double t0_ = 0, pps_ = 60;
    int sy_ = 0;
    timeline::Snap snap_ = timeline::Snap::Beat;
    std::string track_;                        // the current track
    double playhead_ = 0;
    bool loop_ = false;
    double loopA_ = 0, loopB_ = 0;
    bool wasPlaying_ = false;
    int followCount_ = 0;
    // the toolbar
    Fl_Group* bar_;
    ToolButton* audioBtn_;
    ToolButton* playBtn_;
    ToolButton* loopBtn_;
    Fl_Box* position_;
    Fl_Button* tempo_;
    Fl_Menu_Button* snapMenu_;
    Fl_Menu_Button* addMenu_;
    Fl_Menu_Button* editMenu_;
    Fl_Button* renderBtn_;
    Fl_Group* crumbs_;
    TimelineView* view_;
    Fl_Scrollbar* hbar_;
    Fl_Scrollbar* vbar_;
    Fl_Group* inspector_;
    Fl_Box* insTitle_;
    Fl_Box* insSub_;
    struct Field {
        std::string key;
        Fl_Box* label;
        Fl_Input* input;
    };
    std::vector<Field> fields_;
    std::vector<Fl_Button*> insButtons_;
    Fl_Box* info_;
    Fl_Box* message_;
    std::string infoText_, messageText_, positionText_, tempoText_, insKey_;
    std::vector<std::weak_ptr<ScoreDoc>> crumbDocs_;   // the breadcrumb's scores
    WaveCache waves_;
    ScoreRender render_;
};

// The panel in a window of its own
class TimelineWindow : public Fl_Double_Window {
public:
    explicit TimelineWindow(std::shared_ptr<ScoreDoc> doc);
    ~TimelineWindow() override;
    TimelinePanel* panel() const { return panel_; }
    ScoreDoc& doc() { return panel_->doc(); }
    bool save();                               // its file (Save As when it has none; a score kept in a patch: a copy)
    bool saveAs();
    bool confirmDiscard();                     // asks when closing would lose edits; false: cancel
    void updateTitle();
    int handle(int e) override;

private:
    TimelinePanel* panel_;
    Fl_Menu_Bar* menu_ = nullptr;
};

// ---- the application's side (timeline_window.cpp)
TimelineWindow* openTimeline(std::shared_ptr<ScoreDoc> doc);   // its window (already open: brought forward)
TimelineWindow* openScoreBoxTimeline(Box* box);                // a score box's
TimelineWindow* openScoreFile(const std::string& path);        // a .score file (File > Open Score...)
void openScoreDialog();
TimelineWindow* newScoreTimeline();                            // File > New Score
TimelineWindow* frontTimeline();                               // the timeline window in front, if one is
// saving a score as its windows (timeline and roll) do: its file; a score kept in a patch: the patch; a score kept
// inside another: that one; a new score: Save As (a score of a box or placed: a copy). status: what was done
bool saveScoreDocument(const std::shared_ptr<ScoreDoc>& d, const std::function<void(const std::string&)>& status);
bool saveScoreDocumentAs(const std::shared_ptr<ScoreDoc>& d, const std::function<void(const std::string&)>& status);
// a window showing d closes (except: that window): asks when edits only it shows would be lost; false: cancel
bool confirmDiscardScoreDocument(const std::shared_ptr<ScoreDoc>& d, const Fl_Window* except);
const std::vector<TimelineWindow*>& timelineWindows();
void closeTimeline(TimelineWindow* w);                         // already confirmed
void closeAllTimelines();                                      // at once, nothing asked (quitting, the tests' end)
bool confirmCloseTimelines();                                  // Quit: each one asked; false: cancel
// the test actions (guitest.cpp): act=tl...
bool timelineTestAction(const std::string& verb, const std::string& arg);

}  // namespace moritz
