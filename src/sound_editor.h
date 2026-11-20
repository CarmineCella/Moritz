// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_editor.h — the sound editor: a panel (SoundEditor, an Fl_Group that can sit in any window) showing a
// SoundDoc (sound_doc.h), and SoundWindow, the panel in a window of its own (a buffer~ double-clicked, a sound
// file opened).
//
// The panel, top to bottom: its toolbar (transport, position, the tools Select / Draw / Marker, Wave / Spectrum /
// Both, zoom, the Edit, Process and Markers menus), the overview (the whole sound and the part shown, dragged to
// scroll), the view (time ruler, the regions lane, the markers lane, a waveform lane per channel and the
// spectrogram), the regions panel (a table of the regions, the buttons that make them, what is selected) and the
// status line. The view's selection and playhead are the document's: the Process menu reads them there.
//
// The tools: Select drags a selection; Draw, zoomed in until each sample has a pixel or more, moves samples with
// the mouse (a stroke redraws every sample it crosses, joined between the mouse's events; one undo step); Marker
// places a marker where it is clicked. In any tool, M puts a marker at the playhead, a marker is dragged in its
// lane, and the right button on it renames or deletes it.
//
// The Process menu runs an offline object's engine over the selection (all of the sound when nothing is
// selected), a .moritz patch or a Musil script (sound_process.h): a dialog for its settings, then a background job
// (progress on the status line; Escape or Process > Stop stops it). The result replaces the selection (one undo
// step) or opens as a new sound, as the menu's Result says (a new sound also when the sound was edited while it
// was being processed). The patches and scripts used last are listed in the menu.
//
// The markers lane and the regions lane, and the table, are the document's markers and regions (sound_marks.h):
// each edit of them is an undo step. Regions come from a selection (Add region), from soundtypes' units (colored
// by type) or from onsets; markers from onsets, beats or novelty (background jobs, as the objects that analyse).
// To coll writes the regions as rows; To score waits for the score object (0.13).
//
// The waveform is drawn from the document's summary (min and max per pixel column at any zoom). The spectrogram is
// computed by a background job from the document's snapshot when it is first shown and after each edit, and kept
// as an image of its frames (the job is cancelled with the panel). The preview plays through the audio engine
// (preview.h); the panel's timer follows its position, and the buffer~ when another object replaces its samples.
#pragma once

#include "gui.h"
#include "sound_doc.h"

#include <FL/Fl_Menu_Button.H>

#include <map>
#include <memory>

namespace moritz {

class SoundEditor;
class PatchProcess;
class ScriptProcess;
struct ProcessSpec;

// the overview strip
class SoundOverview : public Fl_Widget {
public:
    SoundOverview(int x, int y, int w, int h, SoundEditor* ed) : Fl_Widget(x, y, w, h), ed_(ed) {}
    void draw() override;
    int handle(int e) override;

private:
    SoundEditor* ed_;
    double grab_ = 0;            // where in the visible range the mouse took it (frames)
};

// the ruler, the lanes and the spectrogram
class SoundView : public Fl_Widget {
public:
    SoundView(int x, int y, int w, int h, SoundEditor* ed) : Fl_Widget(x, y, w, h), ed_(ed) {}
    void draw() override;
    int handle(int e) override;
    static constexpr int kGutter = 52;    // the labels' column on the left
    static constexpr int kRulerH = 20;
    static constexpr int kRegionsH = 18;  // the regions lane, under the ruler
    static constexpr int kMarkersH = 16;  // the markers lane, under it
    int laneX() const { return x() + kGutter; }
    int laneW() const { return std::max(1, w() - kGutter); }
    // the parts of the view (window coordinates)
    void areas(int& waveY, int& waveH, int& specY, int& specH) const;
    int regionsY() const { return y() + kRulerH; }
    int markersY() const { return y() + kRulerH + kRegionsH; }
    // the y of a value in channel c's lane (the Draw tool's tests aim with it)
    int valueY(int c, double v) const;
    // a press, a drag and a release at x (the tests' select-by-drag goes through these, as the mouse does)
    void press(int ex, bool inRuler, bool extend);
    void dragTo(int ex);
    void release(int ex);
    int markerAt(int ex) const;           // the marker drawn under x, or -1
    int regionAt(int ex) const;           // the region under x, or -1
    bool stroking() const { return stroke_.active; }

private:
    void drawRuler(int X, int Y, int W);
    void drawLanes(int X, int W);
    void drawWave(int X, int Y, int W, int H);
    void drawSpectrum(int X, int Y, int W, int H);
    int channelAt(int ey, double* value) const;   // the waveform lane under y, and the value there
    void strokeTo(double frame, double value);
    void endStroke();
    void markerMenu(int k);
    SoundEditor* ed_;
    long anchor_ = 0;
    bool dragging_ = false, rulerDrag_ = false, moved_ = false;
    int dragMarker_ = -1;                 // a marker being moved: its index, where it goes
    long dragMarkerTo_ = 0;
    struct Stroke {                       // the Draw tool's stroke, until it is released
        bool active = false;
        int ch = 0;
        long lo = 0;                      // the first frame it changed
        std::vector<float> v;             // the channel's values from lo on (the others are untouched)
        long lastF = 0;
        double lastV = 0;
    } stroke_;
    std::vector<unsigned char> rgb_;      // the spectrogram's pixels, made again for each draw
    friend class SoundEditor;
};

// the regions panel's table: a row a region (name, start, end, type, energy, pitch)
class RegionTable : public Fl_Widget {
public:
    RegionTable(int x, int y, int w, int h, SoundEditor* ed) : Fl_Widget(x, y, w, h), ed_(ed) {}
    void draw() override;
    int handle(int e) override;
    int rowAt(int ey) const;              // the region of the row under y, or -1
    void showRow(int k);                  // scrolled so that row k is seen
    static constexpr int kRowH = 18;

private:
    SoundEditor* ed_;
    int top_ = 0;                         // the first row shown
};

class SoundEditor : public Fl_Group {
public:
    enum class Mode { Wave, Spectrum, Both };
    SoundEditor(int x, int y, int w, int h, std::shared_ptr<SoundDoc> doc);
    ~SoundEditor() override;
    SoundDoc& doc() { return *doc_; }
    const std::shared_ptr<SoundDoc>& docPtr() const { return doc_; }

    // ---- the view: frames from start(), framesPerPixel() of them per pixel column
    double start() const { return start_; }
    double framesPerPixel() const { return spp_; }
    void setView(double start, double spp);   // clamped to the sound
    void zoom(double factor, double aroundFrame);
    void zoomIn() { zoom(0.5, centre()); }
    void zoomOut() { zoom(2.0, centre()); }
    void fit();                                // the whole sound
    void scrollBy(double frames) { setView(start_ + frames, spp_); }
    double centre() const;
    double frameAt(int ex) const;              // the frame under a window x in the view's lanes
    int xOf(double frame) const;
    Mode mode() const { return mode_; }
    void setMode(Mode m);

    // ---- playing
    void togglePlay();
    void play();                               // the selection, or from the playhead to the end
    void stop();
    bool playing() const;
    double playPosition() const;               // the frame the preview is at, or -1
    void setLoop(bool on);
    bool loop() const { return loop_; }

    // ---- tools
    enum class Tool { Select, Draw, Marker };
    Tool tool() const { return tool_; }
    void setTool(Tool t);
    bool canDraw() const { return spp_ <= 1.0; }   // each sample a pixel or more

    // ---- edits (the Edit menu, the keys, the application's menus)
    void cut();
    void copy();
    void paste();
    void erase();
    void trim();
    void silence();
    void fadeIn();
    void fadeOut();
    void gainDialog();
    void gain(double db);
    void normalize();
    void reverse();
    void selectAll();
    void undo();
    void redo();

    // ---- the Process menu
    void processDialog(const std::string& kind);   // its settings asked, then run (in batch mode: printed)
    // run with these settings (missing ones: the defaults; with: "buffer~ NAME", "sound PATH" or "file PATH")
    bool runProcess(const std::string& kind, const std::map<std::string, std::string>& params);
    void choosePatch();                        // A patch...: a file chosen, then processPatch
    void chooseScript();                       // A Musil script...
    bool processPatch(const std::string& path, double tail = 0);
    bool processScript(const std::string& path);
    void stopWork();                           // the process or analysis running
    bool working() const;
    void setResultNew(bool on);                // Result: a new sound (or replace the selection)
    void refreshMenus() { if (menuDirty_) { menuDirty_ = false; buildProcessMenu(); } }   // the timer calls it
    bool resultNew() const { return resultNew_; }
    static std::vector<std::string> recentProcesses();   // the patches and scripts used last (absolute paths)
    // the other sounds a process can take: "buffer~ NAME" of the open patches, "sound PATH" of the open windows
    std::vector<std::string> otherSounds() const;
    const std::string& lastWork() const { return lastWork_; }   // what the last background work ended with

    // ---- markers and regions (each an undo step)
    void addMarkerAt(long frame, const std::string& source = "hand");
    void addMarkerAtPlayhead() { addMarkerAt(doc_->playhead); }
    void deleteMarker(int k);
    void moveMarker(int k, long frame);
    void renameMarker(int k, const std::string& name);
    void clearMarkers();
    void markersFrom(const std::string& what); // onsets beats novelty: a background job
    void regionsFrom(const std::string& what); // types onsets: a background job
    void addRegion();                          // the selection made a region
    void deleteRegion(int k);
    void renameRegion(int k, const std::string& name);
    void renameRegionDialog(int k);
    void clearRegions();
    void selectRegion(int k);                  // the selection becomes its span, its row is shown
    int selectedRegion() const;                // the region the selection is, or -1
    int selectedMarker() const { return selMarker_; }
    void toColl();                             // the regions written as rows (a file chosen)
    bool writeColl(const std::string& path, std::string& err);
    bool scoreAvailable() const;               // a score object exists (To score is active)

    // ---- showing what changed
    void contentsChanged();                    // the samples (an edit, undo, the buffer~ replaced)
    void marksChanged();                       // the markers or regions
    void selectionChanged();
    void setStatus(const std::string& s);      // a message on the status line
    void refreshInfo();                        // the status line's facts and the position
    std::function<void()> onTitle;             // the title should change (edited, saved, the buffer~ gone)

    int handle(int e) override;
    void resize(int x, int y, int w, int h) override;
    SoundView* view() const { return view_; }
    SoundOverview* overview() const { return overview_; }
    Fl_Group* bottomPanel() const { return bottom_; }   // the regions panel
    RegionTable* regionTable() const { return table_; }
    Fl_Menu_Button* processMenu() const { return processMenu_; }
    Fl_Menu_Button* marksMenu() const { return marksMenu_; }
    Fl_Widget* toScoreButton() const { return toScore_; }
    const Spectrogram* spectrogram() const { return spec_.get(); }
    const std::string& info() const { return infoText_; }      // the status line's facts
    const std::string& status() const { return messageText_; }  // the status line's message
    bool spectrogramPending() const { return job_.running(); }

private:
    friend class SoundView;
    friend class SoundOverview;
    friend class RegionTable;
    void layoutChildren();
    void buildBottom(int x, int y, int w);
    void layoutBottom();
    void buildProcessMenu();
    void buildMarksMenu();
    void updateToolButtons();
    void refreshSelectionInfo();
    // a job of the editor's (a process, an analysis): work on the worker, then done on the GUI thread
    bool startWork(const std::string& name, std::function<void(Job&)> work, std::function<void()> done);
    void deliver(const std::string& name, SampleBuffer&& result, uint64_t generation, const std::string& note);
    static void addRecent(const std::string& path);
    void after(bool done);                     // an edit's result: redraw, message, title
    void updateSpectrogram();                  // computed again if shown and out of date
    void updateModeButtons();
    static void tick(void* data);
    std::shared_ptr<SoundDoc> doc_;
    double start_ = 0, spp_ = 1;
    Mode mode_ = Mode::Both;
    bool loop_ = false;
    uint64_t playId_ = 0;
    long playFrom_ = 0;                        // where the playhead was when it started
    double lastPos_ = -1;
    int followCount_ = 0;
    // the toolbar
    Fl_Group* bar_;
    ToolButton* audioBtn_;
    ToolButton* playBtn_;
    ToolButton* loopBtn_;
    Fl_Box* position_;
    ToolButton* waveBtn_;
    ToolButton* specBtn_;
    ToolButton* bothBtn_;
    Fl_Menu_Button* editMenu_;
    Fl_Menu_Button* processMenu_;
    Fl_Menu_Button* marksMenu_;
    ToolButton* toolBtns_[3];
    Tool tool_ = Tool::Select;
    SoundOverview* overview_;
    SoundView* view_;
    Fl_Group* bottom_;
    RegionTable* table_;
    Fl_Widget* toScore_;
    Fl_Box* selInfo_;
    Fl_Box* regionsTitle_;
    std::vector<Fl_Widget*> regionButtons_;
    std::string selInfoText_;
    int selMarker_ = -1;
    bool resultNew_ = false;
    // background work: one at a time
    Job work_;
    std::string workName_, lastWork_;
    std::unique_ptr<PatchProcess> patchProc_;
    std::unique_ptr<ScriptProcess> script_;
    std::vector<std::string> recentShown_;     // the Process menu's recent patches and scripts
    bool menuDirty_ = false;                   // the Process menu to make again (never inside its own callback)
    Fl_Box* info_;
    Fl_Box* message_;
    std::string infoText_, messageText_, positionText_;
    // the spectrogram
    Job job_;
    std::shared_ptr<Spectrogram> spec_;
    uint64_t specGen_ = ~uint64_t(0), jobGen_ = ~uint64_t(0);
};

// The panel in a window of its own: a sound file, or a buffer~
class SoundWindow : public Fl_Double_Window {
public:
    explicit SoundWindow(std::shared_ptr<SoundDoc> doc);
    static constexpr int kW = 1240, kH = 780;
    ~SoundWindow() override;
    SoundEditor* editor() const { return editor_; }
    SoundDoc& doc() { return editor_->doc(); }
    bool save();                               // a file: its file (Save As when it can't be written there)
    bool saveAs();                             // a buffer~: Save as WAV...
    bool confirmDiscard();                     // asks when a file's edits would be lost; false: cancel
    void updateTitle();
    int handle(int e) override;

private:
    SoundEditor* editor_;
    Fl_Menu_Bar* menu_ = nullptr;              // Windows and Linux: each window has its menu
};

std::string formatTime(double seconds);        // "1:04.350", "0:00.000"

}  // namespace moritz
