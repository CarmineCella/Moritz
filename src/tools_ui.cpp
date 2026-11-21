// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// tools_ui.cpp — the tools: zoom and interface size, the quick recorder, the binaural monitor, the
// channel tester, and the timer that feeds the toolbar's meters

#include "gui.h"
#include "ui_util.h"
#include "preview.h"
#include "sound_editor.h"
#include "timeline.h"

#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/filename.H>

#include <cmath>
#include <cstdlib>
#include <ctime>

namespace moritz {

// ---- tools: zoom, interface size, quick recorder, the toolbar's meters

void App::zoomFront(int steps) {
    if (TimelineWindow* t = frontTimeline()) {       // a timeline: zoomed in time; 0 shows the whole score
        if (steps > 0) t->panel()->zoomIn();
        else if (steps < 0) t->panel()->zoomOut();
        else t->panel()->fit();
        return;
    }
    if (SoundWindow* s = frontSound()) {             // a sound: zoomed in time; 0 shows all of it
        if (steps > 0) s->editor()->zoomIn();
        else if (steps < 0) s->editor()->zoomOut();
        else s->editor()->fit();
        return;
    }
    if (Canvas* c = activeCanvas()) c->zoomBy(steps);
}

void App::scaleInterface(float factor) {
    for (int n = 0; n < Fl::screen_count(); ++n) {
        float s = factor == 0 ? 1.f : std::clamp(Fl::screen_scale(n) * factor, 0.5f, 3.f);
        Fl::screen_scale(n, s);
    }
}

std::string App::recordingsFolder() {
    if (recordingsDir_.empty()) {
        char buf[4096] = "";
        appPrefs().get("recordings_folder", buf, "", sizeof buf);
        recordingsDir_ = buf;
    }
    if (recordingsDir_.empty()) {
        const char* home = std::getenv("HOME");
#ifdef _WIN32
        if (!home) home = std::getenv("USERPROFILE");
#endif
        std::string base = home ? home : ".";
        std::string music = base + "/Music";
        recordingsDir_ = (fl_filename_isdir(music.c_str()) ? music : base) + "/Moritz Recordings";
    }
    return recordingsDir_;
}

void App::chooseRecordingsFolder() {
    Fl_Native_File_Chooser fc;
    fc.title("Folder for recordings");
    fc.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    fc.directory(recordingsFolder().c_str());
    if (fc.show() != 0) return;
    recordingsDir_ = fc.filename();
    appPrefs().set("recordings_folder", recordingsDir_.c_str());
    appPrefs().flush();
    refreshTools();
}

void App::toggleRecording() {
    Recorder& r = audio.recorder;
    if (r.recording()) {
        r.stop();
        print("recorded " + r.path());
        refreshTools();
        return;
    }
    if (!audio.running()) setAudio(true);
    if (!audio.running()) { print("quick recorder: the audio is off"); refreshTools(); return; }
    std::string dir = recordingsFolder();
    fl_make_path(dir.c_str());
    std::time_t t = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", std::localtime(&t));
    std::string path = dir + "/Moritz " + stamp + ".wav";
    if (!r.start(path, audio.channels(), audio.sampleRate())) print("quick recorder: can't write " + path);
    refreshTools();
}

void App::showRecorder() {
    if (!recorderWin_) {
        recorderWin_ = new Fl_Double_Window(420, 132, "Quick Recorder");
        centerOnScreen(recorderWin_, frontDoc());
        recorderWin_->color(kStatusBg);
        recorderBtn_ = new ToolButton(14, 14, 110, 30, ToolButton::Record, "Record", false);
        recorderBtn_->callback([](Fl_Widget*, void*) { App::get().toggleRecording(); });
        recorderTime_ = new Fl_Box(134, 14, 120, 30, "0:00.0");
        recorderTime_->labelfont(FL_COURIER_BOLD);
        recorderTime_->labelsize(18);
        recorderTime_->labelcolor(kText);
        recorderTime_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        auto* hint = new Fl_Box(14, 50, 392, 20, "Records what goes to the dac (24-bit WAV, the audio's sample rate).");
        hint->labelsize(11);
        hint->labelcolor(kDim);
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        recorderFile_ = new Fl_Box(14, 70, 392, 20);
        recorderFile_->labelsize(11);
        recorderFile_->labelcolor(kText);
        recorderFile_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        auto* choose = new Fl_Button(14, 98, 120, 22, "Choose folder...");
        choose->labelsize(11);
        choose->callback([](Fl_Widget*, void*) { App::get().chooseRecordingsFolder(); });
        auto* open = new Fl_Button(142, 98, 120, 22, "Open folder");
        open->labelsize(11);
        open->callback([](Fl_Widget*, void*) {
            std::string dir = App::get().recordingsFolder();
            fl_make_path(dir.c_str());
            openWithSystem(dir);
        });
        recorderWin_->end();
    }
    refreshTools();
    recorderWin_->show();
}

void App::refreshTools() {
    const Recorder& r = audio.recorder;
    bool rec = r.recording();
    char t[32];
    double sec = rec ? r.seconds() : 0;
    std::snprintf(t, sizeof t, "%d:%02d.%d", (int)sec / 60, (int)sec % 60, (int)(sec * 10) % 10);
    for (auto* d : docs) {
        d->showRecording(rec, rec ? t : "Record");
        d->showConsoleAlert(consoleAlert);
    }
    if (recorderWin_) {
        recorderBtn_->lit = rec;
        recorderBtn_->copy_label(rec ? "Stop" : "Record");
        recorderBtn_->redraw();
        recorderTime_->copy_label(t);
        std::string f = rec ? "→ " + baseName(r.path()) : "into " + recordingsFolder();
        if (rec && r.overflowed()) f += "   (the disk was too slow: some audio was lost)";
        recorderFile_->copy_label(f.c_str());
        recorderWin_->redraw();
    }
}

// ---- the binaural monitor

namespace {
// the speakers of a ring of n: 0, 1, 2 the layouts of the choice
std::vector<std::pair<float, float>> ringLayout(int layout, int n) {
    std::vector<std::pair<float, float>> v;
    for (int k = 0; k < n; ++k) {
        double a = layout == 0 ? 180.0 / n + 360.0 * k / n          // 1 front-left, counterclockwise (as ambidec~)
                 : layout == 1 ? 180.0 / n - 360.0 * k / n          // 1 front-left, clockwise
                               : 360.0 * k / n;                     // 1 front, counterclockwise
        v.push_back({(float)std::remainder(a, 360.0), 0.f});
    }
    return v;
}
std::string layoutText(const std::vector<std::pair<float, float>>& v) {
    std::string t;
    char b[32];
    for (auto& s : v) {
        if (s.second != 0) std::snprintf(b, sizeof b, "%g/%g", s.first, s.second);
        else std::snprintf(b, sizeof b, "%g", s.first);
        t += (t.empty() ? "" : " ") + std::string(b);
    }
    return t;
}
std::vector<std::pair<float, float>> parseLayout(const std::string& t) {
    std::vector<std::pair<float, float>> v;
    size_t k = 0;
    while (k < t.size()) {
        while (k < t.size() && (t[k] == ' ' || t[k] == ',' || t[k] == '\t')) ++k;
        size_t e = k;
        while (e < t.size() && t[e] != ' ' && t[e] != ',' && t[e] != '\t') ++e;
        if (e > k) {
            std::string w = t.substr(k, e - k);
            size_t sl = w.find('/');
            float az = (float)std::atof(w.substr(0, sl).c_str()), el = sl == std::string::npos ? 0.f : (float)std::atof(w.substr(sl + 1).c_str());
            v.push_back({az, el});
        }
        k = e;
    }
    return v;
}
struct MonitorUi {
    Fl_Check_Button* on;
    Fl_Choice* layout;
    Fl_Choice* count;
    Fl_Input* dirs;
    Fl_Box* info;
};
MonitorUi gMon;
}  // namespace

void App::applyMonitor() {
    MonitorSettings& m = monitorSet_;
    int n = m.speakers > 0 ? m.speakers : std::max(2, audio.channels());
    std::vector<std::pair<float, float>> sp = m.layout == 3 ? parseLayout(m.custom) : ringLayout(m.layout, n);
    if (!m.on || sp.empty() || !audio.running()) audio.setMonitor({});
    else audio.setMonitor(sp);
    if (monitorWin_) {
        if (m.layout != 3) gMon.dirs->value(layoutText(sp).c_str());
        std::string i = !m.on ? "Off: the device plays the channels it has."
                       : !audio.running() ? "On when the audio is on."
                                          : std::to_string(sp.size()) + " speakers rendered to the device's channels 1 and 2 (headphones). "
                                            "The quick recorder still records the speaker channels.";
        gMon.info->copy_label(i.c_str());
        monitorWin_->redraw();
    }
}

void App::showMonitor() {
    if (!monitorWin_) {
        monitorWin_ = new Fl_Double_Window(480, 240, "Binaural Monitor");
        centerOnScreen(monitorWin_, frontDoc());
        monitorWin_->color(kStatusBg);
        gMon.on = new Fl_Check_Button(14, 12, 440, 24, "Listen to the speakers on headphones (binaural)");
        gMon.on->labelcolor(kText);
        gMon.layout = new Fl_Choice(110, 46, 356, 24, "Speakers:");
        gMon.layout->add("Ring: 1 front-left, counterclockwise (as ambidec~)");
        gMon.layout->add("Ring: 1 front-left, clockwise");
        gMon.layout->add("Ring: 1 front, counterclockwise");
        gMon.layout->add("Custom directions");
        gMon.count = new Fl_Choice(110, 78, 90, 24, "How many:");
        gMon.count->add("Patches");
        for (int c : {2, 4, 5, 6, 8, 10, 12, 16, 24, 32}) gMon.count->add(std::to_string(c).c_str());
        gMon.dirs = new Fl_Input(110, 110, 356, 24, "Directions:");
        gMon.dirs->tooltip("One per channel, in degrees: the azimuth (positive to the left, 0 in front), "
                           "or azimuth/elevation (90/30). Edit them to make a custom layout.");
        auto* hint = new Fl_Box(14, 140, 452, 34,
                                "Each channel is placed as a speaker around you with the MIT KEMAR head responses; "
                                "channels 1 and 2 of the device carry the mix.");
        hint->labelsize(11);
        hint->labelcolor(kDim);
        hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        gMon.info = new Fl_Box(14, 180, 452, 46);
        gMon.info->labelsize(11);
        gMon.info->labelcolor(kText);
        gMon.info->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        for (Fl_Widget* w : {(Fl_Widget*)gMon.layout, (Fl_Widget*)gMon.count, (Fl_Widget*)gMon.dirs}) w->labelcolor(kText);
        auto changed = [](Fl_Widget* w, void*) {
            App& app = App::get();
            MonitorSettings& m = app.monitorSet_;
            m.on = gMon.on->value();
            if (w == gMon.dirs) { m.layout = 3; m.custom = gMon.dirs->value(); gMon.layout->value(3); }
            else {
                m.layout = gMon.layout->value();
                if (m.layout == 3) m.custom = gMon.dirs->value();
                const int counts[] = {0, 2, 4, 5, 6, 8, 10, 12, 16, 24, 32};
                m.speakers = counts[std::clamp(gMon.count->value(), 0, 10)];
            }
            if (m.on && !app.audio.running()) app.setAudio(true);
            app.applyMonitor();
        };
        gMon.on->callback(changed);
        gMon.layout->callback(changed);
        gMon.count->callback(changed);
        gMon.dirs->callback(changed);
        gMon.dirs->when(FL_WHEN_ENTER_KEY | FL_WHEN_RELEASE);
        monitorWin_->end();
        gMon.layout->value(0);
        gMon.count->value(0);
    }
    gMon.on->value(monitorSet_.on);
    applyMonitor();
    monitorWin_->show();
}

// ---- the channel tester

void App::refreshTester() {
    if (!testerWin_) return;
    int n = std::clamp(audio.running() ? audio.channels() : 2, 1, AudioEngine::kMaxBus);
    int cur = audio.testChannel.load();
    if (testerGrid_->children() != n) {
        testerGrid_->clear();
        testerGrid_->begin();
        int cols = 8, bw = 52, bh = 30;
        for (int c = 0; c < n; ++c) {
            auto* b = new ToolButton(testerGrid_->x() + (c % cols) * (bw + 6), testerGrid_->y() + (c / cols) * (bh + 6), bw, bh,
                                     ToolButton::None, std::to_string(c + 1).c_str(), false);
            b->callback([](Fl_Widget* w, void* d) {
                App& app = App::get();
                int ch = (int)(intptr_t)d;
                app.testerSteps_ = 0;
                if (!app.audio.running()) app.setAudio(true);
                app.audio.testChannel = app.audio.testChannel.load() == ch ? -1 : ch;
                app.refreshTester();
                (void)w;
            }, (void*)(intptr_t)c);
        }
        testerGrid_->end();
        int rows = (n + 7) / 8;
        testerWin_->size(testerWin_->w(), 130 + rows * 36);
    }
    for (int c = 0; c < testerGrid_->children(); ++c) {
        auto* b = static_cast<ToolButton*>(testerGrid_->child(c));
        b->lit = c == cur;
        b->redraw();
    }
    std::string i;
    int dev = audio.deviceOutputs();
    if (!audio.running()) i = "The audio is off: click a channel to turn it on.";
    else if (cur < 0) i = "Click a channel: bursts of noise come out of its speaker. Click it again to stop.";
    else if (audio.monitoring()) i = "Channel " + std::to_string(cur + 1) + ": heard through the binaural monitor, where its speaker is.";
    else if (cur >= dev) i = "Channel " + std::to_string(cur + 1) + ": the device has " + std::to_string(dev) + " outputs, so it is not heard (the meter shows it).";
    else i = "Channel " + std::to_string(cur + 1) + " of " + std::to_string(dev) + ": the speaker it comes from is number " + std::to_string(cur + 1) + ".";
    testerInfo_->copy_label(i.c_str());
    testerWin_->redraw();
}

void App::testerTimer(void*) {
    App& app = App::get();
    if (!app.testerWin_ || !app.testerWin_->shown() || app.testerSteps_ == 0) return;
    int n = std::max(1, app.audio.channels());
    app.audio.testChannel = (app.audio.testChannel.load() + 1) % n;
    app.refreshTester();
    Fl::repeat_timeout(1.5, testerTimer);
}

void App::showTester() {
    if (!testerWin_) {
        testerWin_ = new Fl_Double_Window(470, 166, "Channel Tester");
        centerOnScreen(testerWin_, frontDoc());
        testerWin_->color(kStatusBg);
        auto* step = new Fl_Button(14, 12, 130, 26, "Step through all");
        step->labelsize(12);
        step->callback([](Fl_Widget*, void*) {
            App& app = App::get();
            if (!app.audio.running()) app.setAudio(true);
            app.testerSteps_ = 1;
            app.audio.testChannel = 0;
            app.refreshTester();
            Fl::remove_timeout(testerTimer);
            Fl::add_timeout(1.5, testerTimer);
        });
        auto* stop = new Fl_Button(150, 12, 70, 26, "Stop");
        stop->labelsize(12);
        stop->callback([](Fl_Widget*, void*) {
            App& app = App::get();
            app.testerSteps_ = 0;
            app.audio.testChannel = -1;
            app.refreshTester();
        });
        auto* level = new Fl_Value_Slider(290, 12, 166, 26, "Level (dB)");
        level->type(FL_HOR_NICE_SLIDER);
        level->labelsize(11);
        level->labelcolor(kText);
        level->align(FL_ALIGN_LEFT);
        level->range(-40, 0);
        level->step(1);
        level->value(-14);
        level->callback([](Fl_Widget* w, void*) {
            App::get().audio.testLevel = (float)std::pow(10.0, static_cast<Fl_Value_Slider*>(w)->value() / 20.0);
        });
        testerInfo_ = new Fl_Box(14, 44, 442, 36);
        testerInfo_->labelsize(11);
        testerInfo_->labelcolor(kText);
        testerInfo_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        testerGrid_ = new Fl_Group(14, 88, 442, 40);
        testerGrid_->end();
        testerWin_->end();
        testerWin_->callback([](Fl_Widget* w, void*) {           // closing it stops the noise
            App& app = App::get();
            app.testerSteps_ = 0;
            app.audio.testChannel = -1;
            w->hide();
        });
        audio.testLevel = (float)std::pow(10.0, -14 / 20.0);
    }
    refreshTester();
    testerWin_->show();
}

void App::toolTimer(void*) {
    App& app = App::get();
    float cpu = app.audio.running() ? app.host.cpuLoad.load(std::memory_order_relaxed) : 0.f;
    std::vector<float> peaks((size_t)std::clamp(app.audio.channels(), 1, AudioEngine::kMeterChannels));
    for (size_t c = 0; c < peaks.size(); ++c) peaks[c] = app.audio.takePeak((int)c);
    for (auto* d : app.docs) d->updateMeters(cpu, peaks);
    if (app.audio.recorder.recording()) app.refreshTools();
    app.audio.preview().collect();                    // what the sound editor's preview is done with
    Fl::repeat_timeout(1.0 / 15, toolTimer);
}

}  // namespace moritz
