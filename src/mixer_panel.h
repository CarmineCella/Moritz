// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// mixer_panel.h — a session's mixer (mockups 4 and 8): a strip for each track, then each bus, then the master. A strip
// shows its name, mute, solo and arm, where it goes (the master or a bus), its sends (a level each, + to add one), its
// insert, its pan and its fader (dB) beside a meter (peak and RMS). Its values are the score's rows' (multitrack.h):
// dragged, a fader or a pan reaches the audio thread at once through the strip's atomics, and at the release the change
// is one undo step of the score (no new plan: the score box sees the strips only changed). A panel, written once: in the
// arranger's drawer, or anywhere a score is shown.
#pragma once

#include "score_doc.h"

#include <FL/Fl_Widget.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moritz {

class MixerPanel : public Fl_Widget {
public:
    MixerPanel(int x, int y, int w, int h, std::shared_ptr<ScoreDoc> doc);
    ~MixerPanel() override;
    void draw() override;
    int handle(int e) override;
    static constexpr int kStripW = 96;

    // a strip as laid out now (window coordinates)
    struct Strip {
        std::string name;                  // a track or a bus; mt::kMaster for the master
        bool bus = false, master = false;
        int x = 0;
        int fy0 = 0, fy1 = 0;              // the fader's travel
        int panY = 0;                      // the pan's line
        int sendY = 0;                     // the first send's line
        int outY = 0, insY = 0, btnY = 0;
    };
    const std::vector<Strip>& strips() const { return strips_; }
    void layoutStrips();
    // the values a strip shows (the document's, or the drag going on)
    double gainOf(const std::string& name) const;
    double panOf(const std::string& name) const;
    int fyOf(const Strip& s, double gain) const;            // the fader's y for a gain
    double gainAt(const Strip& s, int y) const;
    // the mouse, as the events drive it (the tests too)
    void press(int ex, int ey, int clicks);
    void dragTo(int ex, int ey);
    void release(int ex, int ey);
    // what it says: one line a strip (the tests print it)
    std::string describe(const std::string& name) const;
    std::function<void(const std::string&)> onStatus;
    std::function<void(const std::string& track)> onTrack;   // a strip's name clicked: that track current
    std::function<void(const std::string& track)> onInsert;  // its insert clicked: the drawer shows it
    int scrollX = 0;

private:
    static void tick(void* data);
    void commitGain(const std::string& name, double gain);
    void commitPan(const std::string& name, double az);
    Box* player() const;
    std::shared_ptr<ScoreDoc> doc_;
    std::vector<Strip> strips_;
    enum class Drag { None, Fader, Pan, Send } drag_ = Drag::None;
    std::string dragName_;
    int dragSend_ = -1, pressY_ = 0, pressX_ = 0;
    double dragStart_ = 0, dragValue_ = 0;
    struct Held {
        float peak[2] = {0, 0};
    };
    std::vector<Held> held_;
};

}  // namespace moritz
