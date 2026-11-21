// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// roll.h — the roll: a score drawn as staves with proportional time (or as a piano roll), edited by hand.
//
// RollCore draws a score and turns the mouse and the keys into edit intents (roll_edit.h); it draws with the zoomed
// calls of the canvas (zoom.h) in coordinates of its own, so the same code draws the roll in its window and, compact,
// inside a score box (objects_score.cpp), where it is edited in place when the patch is locked. The staves follow
// Musil's layout (music/score_layout.h: rows in orchestral order, clefs, steps on the staff): a note is a head at its
// onset and a line for its duration, with a sharp, its cents when it is a microtone and its dynamics where they
// change; a score placed inside is a faint block with its notes and its placement (+7 x1.5 -6 dB); sounds, patches
// and messages are blocks on lines of their own.
//
// RollPanel is the roll's whole editor, a panel that can sit in any window (RollWindow for now): its toolbar (play and
// stop on the patch's transport, Roll / Piano roll / List, Select / Draw / Erase, snap, quantize, zoom), the
// breadcrumb (where the view is among the scores placed inside one another: a double-click enters a score placed,
// Esc goes up), the view, a lane of dynamics, the list of events, the score as Musil data (score_text.h), edited and
// kept in sync both ways (nothing applied while it does not read), the Transform panel and a slot for Generate.
#pragma once

#include "roll_edit.h"

#include "music/score_layout.h"

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Widget.H>

#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Choice;
class Fl_Input;
class Fl_Text_Buffer;
class Fl_Text_Editor;
class Fl_Menu_Button;
class Fl_Menu_Bar;

namespace moritz {

class ToolButton;

class RollCore {
public:
    enum class Mode { Staves, Piano };
    enum class Tool { Select, Draw, Erase };
    explicit RollCore(std::shared_ptr<RollEditTarget> t);
    RollEditTarget* target() const { return target_.get(); }
    const std::shared_ptr<RollEditTarget>& targetPtr() const { return target_; }
    void setTarget(std::shared_ptr<RollEditTarget> t);

    // ---- where it is drawn: a rectangle in coordinates of its own; draw() puts it at an origin
    void setSize(double w, double h);
    double width() const { return w_; }
    double height() const { return h_; }
    bool compact = false;            // inside a box: no ruler, no clefs' glyphs, the whole score in view
    Mode mode = Mode::Staves;
    Tool tool = Tool::Select;
    double snap = 1.0 / 16;          // the grid, as a fraction of a whole note (0: none)

    // ---- what is shown: the score at path (the scores placed, from the top down)
    ScorePath path;
    std::shared_ptr<const sc::Score> shown() const;
    std::vector<std::string> crumbs() const;      // the names along path, from the top
    void enter(uint64_t scoreEvent);              // into a score placed
    void up();                                    // out of it
    // what was there when the view went in: placements from the top down to the score shown
    struct Placement {
        double at = 0, dur = 0, stretch = 1, transpose = 0, gain = 1;
    };
    std::vector<Placement> placements() const;

    // ---- the view: seconds from t0, pps pixels a second; rows scrolled by vscroll pixels
    double t0 = 0, pps = 60, vscroll = 0;
    double rowScale = 1;             // the staves' height
    void fit();                      // the whole score shown (the compact roll always is)
    void zoom(double factor, double aroundX);
    double xOf(double t) const;      // a time of the score shown as x
    double tOf(double x) const;
    double snapSeconds() const;      // the grid in seconds (0: none)
    double snapTime(double t) const;
    double barSeconds() const;
    double playhead = NAN;           // the transport's position, seconds of the top score (NaN: not shown)
    double shownTime(double topTime) const;   // the same moment in the score shown (NaN: outside it)

    // ---- the selection (ids of the score shown)
    std::set<uint64_t> selection;
    Ids selectedIds() const { return Ids(selection.begin(), selection.end()); }
    void selectAll();

    // ---- drawing and the mouse (coordinates of its own; ox, oy: where its corner is drawn)
    void draw(double ox, double oy);
    // FL_PUSH, FL_DRAG, FL_RELEASE (or FL_MOVE) at x, y; keys: Fl::event_state; clicks: Fl::event_clicks. => handled
    bool mouse(int event, double x, double y, int keys, int clicks);
    bool key(int key, int keys);
    // the edits the keys and buttons make on the selection (all of it when nothing is selected)
    void removeSelection();
    void nudge(double dt, double dpitch);
    void quantizeSelection();
    bool transformSelection(ScoreTransform t);
    std::function<void(uint64_t)> onEnter;                 // a score placed was double-clicked (compact: the window)
    std::function<void()> onChange;                        // something to draw again
    std::function<void(const std::string&)> onStatus;      // a message for the status line

    // ---- what is where (the tests drag through these)
    struct Hit {
        uint64_t id = 0;
        bool end = false;            // the end of a note or a block: resize
        int row = -1;
    };
    Hit hit(double x, double y) const;
    bool eventBox(uint64_t id, double& x, double& y, double& w, double& h) const;   // where an event is drawn
    double yOfPitch(int row, double pitch) const;          // where a pitch is on a row (staves) or in the piano roll
    double pitchAtY(int row, double y) const;              // the pitch a y means (staves: the natural note there)
    int rowAt(double y) const;
    struct RowGeom {
        sc::layout::RowSpec spec;
        double top = 0, h = 0;       // in its own coordinates
        double yBottom = 0, sp = 0;  // a staff: its bottom line and the space between lines
        bool staff = false;
    };
    const std::vector<RowGeom>& rows() const { layoutRows(); return rows_; }
    std::string summary() const;      // "4 rows · 12 events · 0'08""

private:
    struct Item {                    // an event as drawn
        const sc::Event* e = nullptr;
        int row = -1;
        double x = 0, y = 0, w = 0, h = 0;   // its box (a note: from the head to the end of its line)
        int lane = 0;
    };
    void layoutRows() const;
    void drawRuler(double ox, double oy);
    void drawRow(double ox, double oy, const RowGeom& r, int k);
    void drawItems(double ox, double oy, double dt, double dp, double dd, bool ghost);
    void drawNote(double ox, double oy, const Item& it, Fl_Color c, bool sel, bool ghost, double dt, double dp, double dd);
    void drawPiano(double ox, double oy);
    void finishDrag(double x, double y, int keys);
    std::shared_ptr<RollEditTarget> target_;
    double w_ = 400, h_ = 200;
    // made again when the score, the size or the view changes
    mutable uint64_t gen_ = ~0ull;
    mutable double layoutKey_[5] = {-1, -1, -1, -1, -1};
    mutable std::vector<RowGeom> rows_;
    mutable std::vector<Item> items_;
    mutable std::shared_ptr<const sc::Score> cached_;
    mutable double pianoLo_ = 48, pianoHi_ = 84, keyH_ = 8;
    // a gesture
    enum class Drag { None, Move, Resize, Rubber, Draw, Erase } drag_ = Drag::None;
    double px_ = 0, py_ = 0, cx_ = 0, cy_ = 0;
    uint64_t anchor_ = 0;
    int anchorRow_ = -1;
    bool copy_ = false, moved_ = false, additive_ = false;
    double dragDt_ = 0, dragDp_ = 0, dragDd_ = 0;
    double drawAt_ = 0, drawEnd_ = 0, drawPitch_ = 60;
    std::set<uint64_t> erasing_;
    std::set<uint64_t> before_;      // the selection when a rubber band started
};

// a colour for each row (as the mockups: orange, teal, magenta, blue, ...)
Fl_Color rollRowColour(int row);

class RollPanel;

// the roll's view as a widget: RollCore in a part of the panel
class RollWidget : public Fl_Widget {
public:
    RollWidget(int x, int y, int w, int h, RollPanel* panel);
    void draw() override;
    int handle(int e) override;
    void resize(int x, int y, int w, int h) override;
    RollCore& core();

private:
    RollPanel* panel_;
};

// the lane of dynamics under the staves: a bar a note, as high as its dynamics; dragged, it changes them
class DynamicsLane : public Fl_Widget {
public:
    DynamicsLane(int x, int y, int w, int h, RollPanel* panel) : Fl_Widget(x, y, w, h), panel_(panel) {}
    void draw() override;
    int handle(int e) override;
    std::string row;                 // "" every row

private:
    RollPanel* panel_;
    uint64_t dragging_ = 0;
    int level_ = -1;
};

// the events as a table: a click selects, a double-click edits a cell
class EventList : public Fl_Widget {
public:
    EventList(int x, int y, int w, int h, RollPanel* panel) : Fl_Widget(x, y, w, h), panel_(panel) {}
    void draw() override;
    int handle(int e) override;
    int scroll = 0;                  // the first row shown
    std::vector<const sc::Event*> rowsShown() const;
    void editCell(int row, int column);   // an input over the cell; Enter sets the field

private:
    RollPanel* panel_;
};

class RollPanel : public Fl_Group {
public:
    enum class View { Roll, Piano, List };
    RollPanel(int x, int y, int w, int h, std::shared_ptr<RollEditTarget> target);
    ~RollPanel() override;
    RollCore& core() { return core_; }
    RollEditTarget* target() const { return core_.target(); }
    View view() const { return view_; }
    void setView(View v);
    void setTool(RollCore::Tool t);
    void setSnap(double s);
    // something changed (the score, the selection, the view): everything that shows it follows
    void changed();
    void setStatus(const std::string& s);
    std::string status() const { return status_; }
    void play();
    void stop();
    void undo();
    void redo();
    bool save();
    // the text view: the score as Musil data
    std::string text() const;
    void setText(const std::string& t);       // as if typed (applied when it reads, see applyText)
    bool applyText();                         // now: false (and the error shown) when it does not read
    std::string textError() const { return textError_; }
    std::function<void()> onTitle;            // the window's title should change
    int handle(int e) override;
    void resize(int x, int y, int w, int h) override;
    void layoutChildren();
    RollWidget* rollWidget() const { return roll_; }
    EventList* list() const { return list_; }
    // a cell of the list edited: an input over it; Enter sets the field (setField), Escape leaves it
    void editCell(uint64_t id, const std::string& key, const std::string& value, int x, int y, int w);
    void commitCell();
    static void runTransform(RollPanel* p, int kind);   // a button of the Transform panel (kind: ScoreTransform's, 100 diminish)

private:
    friend class RollWidget;
    friend class DynamicsLane;
    friend class EventList;
    static void tick(void* data);
    static void textChanged(int, int, int, int, const char*, void* data);
    static void applyLater(void* data);
    void refreshText(bool force);
    void refreshBar();
    void buildTransform(int x, int y, int w, int h);
    double param(Fl_Input* in, double def) const;
    RollCore core_;
    View view_ = View::Roll;
    // the toolbar
    Fl_Group* bar_;
    ToolButton* playBtn_;
    ToolButton* viewBtn_[3];
    ToolButton* toolBtn_[3];
    Fl_Choice* snap_;
    Fl_Box* crumbs_;
    RollWidget* roll_;
    DynamicsLane* dyn_;
    Fl_Choice* dynRow_;
    EventList* list_;
    Fl_Group* side_;
    Fl_Input* inTranspose_;
    Fl_Input* inAugment_;
    Fl_Input* inStrength_;
    Fl_Input* inHumanize_;
    Fl_Input* inThin_;
    Fl_Input* inDensity_;
    Fl_Text_Editor* textEd_;
    Fl_Text_Buffer* text_;
    Fl_Box* textStatus_;
    Fl_Box* statusBox_;
    std::string status_, crumbText_, textError_, shownText_;
    uint64_t seenGen_ = ~0ull;
    bool settingText_ = false, textPending_ = false;
    uint32_t seed_ = 1;
    std::vector<std::string> dynRows_;
    Fl_Input* cellInput_;
    uint64_t cellId_ = 0;
    std::string cellKey_;
    struct TransformButton {
        RollPanel* panel;
        int kind;
    };
    std::vector<std::unique_ptr<TransformButton>> transformButtons_;
};

// The roll in a window of its own: a score box double-clicked (or its Open Roll item)
class RollWindow : public Fl_Double_Window {
public:
    explicit RollWindow(std::shared_ptr<RollEditTarget> target);
    ~RollWindow() override;
    RollPanel* panel() const { return panel_; }
    void updateTitle();
    int handle(int e) override;
    static std::vector<RollWindow*>& all();

private:
    RollPanel* panel_;
    Fl_Menu_Bar* menu_ = nullptr;
};

// the roll of a score box: its window (already open: brought forward)
RollWindow* openRoll(ScoreSource* s);
// the roll window in front, if one is (the menus act on it: Undo, Redo, Save, Select All, Delete)
RollWindow* frontRoll();
// closes every roll window at once (the tests' exit)
void closeAllRolls();

}  // namespace moritz
