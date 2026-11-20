// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_editor.h — the sound editor: a panel (SoundEditor, an Fl_Group that can sit in any window) showing a
// SoundDoc (sound_doc.h), and SoundWindow, the panel in a window of its own (a buffer~ double-clicked, a sound
// file opened).
//
// The panel, top to bottom: its toolbar (transport, position, Wave / Spectrum / Both, zoom, the Edit menu), the
// overview (the whole sound and the part shown, dragged to scroll), the view (time ruler, a slot for marker lanes,
// a waveform lane per channel and the spectrogram), a bottom panel (empty and hidden for now: regions go there)
// and the status line. The view's selection and playhead are the document's: whatever acts on the selection
// later (the Process menu) reads them there.
//
// The waveform is drawn from the document's summary (min and max per pixel column at any zoom). The spectrogram is
// computed by a background job from the document's snapshot when it is first shown and after each edit, and kept
// as an image of its frames (the job is cancelled with the panel). The preview plays through the audio engine
// (preview.h); the panel's timer follows its position, and the buffer~ when another object replaces its samples.
#pragma once

#include "gui.h"
#include "sound_doc.h"

#include <FL/Fl_Menu_Button.H>

#include <memory>

namespace moritz {

class SoundEditor;

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
    int laneX() const { return x() + kGutter; }
    int laneW() const { return std::max(1, w() - kGutter); }
    // the parts of the view (window coordinates)
    void areas(int& waveY, int& waveH, int& specY, int& specH) const;
    // a press, a drag and a release at x (the tests' select-by-drag goes through these, as the mouse does)
    void press(int ex, bool inRuler, bool extend);
    void dragTo(int ex);
    void release(int ex);

private:
    void drawRuler(int X, int Y, int W);
    void drawWave(int X, int Y, int W, int H);
    void drawSpectrum(int X, int Y, int W, int H);
    SoundEditor* ed_;
    long anchor_ = 0;
    bool dragging_ = false, rulerDrag_ = false, moved_ = false;
    std::vector<unsigned char> rgb_;      // the spectrogram's pixels, made again for each draw
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

    // ---- showing what changed
    void contentsChanged();                    // the samples (an edit, undo, the buffer~ replaced)
    void selectionChanged();
    void setStatus(const std::string& s);      // a message on the status line
    void refreshInfo();                        // the status line's facts and the position
    std::function<void()> onTitle;             // the title should change (edited, saved, the buffer~ gone)

    int handle(int e) override;
    void resize(int x, int y, int w, int h) override;
    SoundView* view() const { return view_; }
    SoundOverview* overview() const { return overview_; }
    Fl_Group* bottomPanel() const { return bottom_; }   // regions go there (hidden while empty)
    const Spectrogram* spectrogram() const { return spec_.get(); }
    const std::string& info() const { return infoText_; }      // the status line's facts
    bool spectrogramPending() const { return job_.running(); }

private:
    friend class SoundView;
    friend class SoundOverview;
    void layoutChildren();
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
    SoundOverview* overview_;
    SoundView* view_;
    Fl_Group* lanes_;                          // marker lanes (none yet)
    Fl_Group* bottom_;
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
