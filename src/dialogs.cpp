// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// dialogs.cpp — Audio Setup, MIDI Setup, About, and the banner (Shift+Cmd+J)

#include "gui.h"
#include "ui_util.h"

#include <FL/Fl_Choice.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdarg>
#include <cstdio>

#ifndef MORITZ_VERSION
#define MORITZ_VERSION "dev"
#endif

namespace moritz {

namespace {
bool gBatch = false;
int gBatchAnswer = 0;
std::string gBatchSavePath;
}  // namespace

void setBatchMode(bool on) { gBatch = on; }
bool batchMode() { return gBatch; }
void setBatchAnswer(int a) { gBatchAnswer = a; }
void setBatchSavePath(const std::string& path) { gBatchSavePath = path; }

std::string& batchClipboard() {
    static std::string text;
    return text;
}

std::string showSaveChooser(Fl_Native_File_Chooser& fc) {
    if (gBatch) {
        std::printf("save as: %s in %s -> %s\n", fc.preset_file() ? fc.preset_file() : "",
                    fc.directory() && *fc.directory() ? fc.directory() : "(the system's choice)",
                    gBatchSavePath.empty() ? "cancelled" : gBatchSavePath.c_str());
        std::fflush(stdout);
        return gBatchSavePath;
    }
    if (fc.show() != 0 || !fc.filename()) return "";
    return portablePath(fc.filename());
}

int askUser(const char* fmt, const char* b0, const char* b1, const char* b2, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, b2);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (gBatch) {
        const char* b[3] = {b0, b1, b2};
        int a = std::clamp(gBatchAnswer, 0, 2);
        std::printf("ask: %s -> %s\n", text, b[a] ? b[a] : "?");
        std::fflush(stdout);
        return a;
    }
    return fl_choice("%s", b0, b1, b2, text);
}

void alertUser(const char* fmt, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (gBatch) {
        std::printf("alert: %s\n", text);
        std::fflush(stdout);
        return;
    }
    fl_alert("%s", text);
}

// Shift+Cmd+J, from any window: a little homage to Wilhelm Busch
int eggHandler(int e) {
    if (e != FL_SHORTCUT) return 0;
    int s = Fl::event_state();
    if ((s & FL_COMMAND) && (s & FL_SHIFT) && Fl::event_key() == 'j') {
        App::get().showBanner();
        return 1;
    }
    return 0;
}

namespace {
class Banner : public Fl_Double_Window {
public:
    Banner(Fl_Image* img) : Fl_Double_Window(480, 445), img_(img) {
        border(0);
        end();
        Fl::add_timeout(8.0, timeUp, this);
    }
    ~Banner() override {
        Fl::remove_timeout(timeUp, this);
        delete img_;
    }
    static void timeUp(void* d) { static_cast<Banner*>(d)->close(); }
    void draw() override {
        Fl_Color paper = fl_rgb_color(247, 241, 227), ink = fl_rgb_color(40, 34, 28);
        fl_color(paper);
        fl_rectf(0, 0, w(), h());
        fl_color(fl_rgb_color(150, 132, 104));
        fl_rect(6, 6, w() - 12, h() - 12);
        if (img_) img_->draw((w() - img_->w()) / 2, 22);
        int y = 22 + (img_ ? img_->h() : 340) + 14;
        fl_color(ink);
        fl_font(FL_TIMES_ITALIC, 18);
        fl_draw("Max und Moritz, Wilhelm Busch (1865)", 0, y, w(), 24, FL_ALIGN_CENTER);
        fl_color(fl_rgb_color(110, 96, 80));
        fl_font(FL_HELVETICA, 10);
        fl_draw("Moritz goes nowhere without Max  ·  click to close", 0, h() - 26, w(), 14, FL_ALIGN_CENTER);
    }
    int handle(int e) override {
        bool modifier = Fl::event_key() >= FL_Shift_L && Fl::event_key() <= FL_Meta_R;
        if (e == FL_PUSH || ((e == FL_KEYBOARD || e == FL_SHORTCUT) && !modifier)) { close(); return 1; }
        return Fl_Double_Window::handle(e);
    }
    void close() {
        if (closing_) return;
        closing_ = true;
        Fl::remove_timeout(timeUp, this);
        hide();
        Fl::delete_widget(this);
    }

private:
    Fl_Image* img_;
    bool closing_ = false;
};
}  // namespace

void App::showBanner() {
    std::string base = exeDir();
    Fl_Image* img = nullptr;
    for (std::string p : {base + "/../Resources/maxundmoritz.png", base + "/resources/maxundmoritz.png"}) {
        Fl_PNG_Image png(p.c_str());
        if (png.fail() == 0) { img = png.copy(440, 440 * png.h() / std::max(1, png.w())); break; }
    }
    auto* b = new Banner(img);
    centerOnScreen(b, frontDoc());
    b->show();
}

void App::about() {
    Fl_Double_Window dlg(360, 330, "About Moritz");
    centerOnScreen(&dlg, frontDoc());
    dlg.color(kBgLocked);
    Fl_Box icon(116, 18, 128, 128);
    std::string base = exeDir();
    Fl_PNG_Image* png = nullptr;
    for (std::string p : {base + "/../Resources/about.png", base + "/resources/about.png"}) {
        png = new Fl_PNG_Image(p.c_str());
        if (png->fail() == 0) break;
        delete png;
        png = nullptr;
    }
    Fl_Image* scaled = png ? png->copy(128, 128) : nullptr;
    if (scaled) icon.image(scaled);
    auto text = [](int y, int h, const char* s, int size, Fl_Font f, Fl_Color c) {
        auto* b = new Fl_Box(10, y, 340, h);
        b->copy_label(s);
        b->labelsize(size);
        b->labelfont(f);
        b->labelcolor(c);
        return b;
    };
    text(152, 30, "Moritz", 24, FL_HELVETICA_BOLD, kText);
    text(182, 18, ("version " MORITZ_VERSION), 12, FL_HELVETICA, kDim);
    text(206, 20, "A Max-like environment for sound and music computing", 12, FL_HELVETICA, kText);
    text(236, 20, "Created by Carmine-Emanuele Cella (2026)", 13, FL_HELVETICA_BOLD, kAccent);
    text(258, 18, "BSD 2-Clause License", 11, FL_HELVETICA, kDim);
    Fl_Return_Button ok(260, 288, 85, 28, "OK");
    ok.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.end();
    dlg.set_modal();
    dlg.show();
    while (dlg.shown()) Fl::wait();
    icon.image(nullptr);
    delete scaled;
    delete png;
}

void App::midiSetup() {
    midi.refresh();
    std::vector<std::string> ins = midi.inputPorts(), outs = midi.outputPorts();
    Fl_Double_Window dlg(440, 170, "MIDI Setup");
    centerOnScreen(&dlg, frontDoc());
    Fl_Choice inC(130, 15, 290, 25, "Input:");
    Fl_Choice outC(130, 50, 290, 25, "Output:");
    Fl_Box note(20, 82, 400, 40,
                "Other programs see Moritz's output as the MIDI port \"Moritz\" "
                "(macOS and Linux). Devices plugged in later are found automatically.");
    note.labelsize(11);
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button cancel(240, 130, 85, 28, "Cancel");
    Fl_Return_Button ok(335, 130, 85, 28, "OK");
    dlg.end();
    dlg.set_modal();

    inC.add("All inputs", 0, nullptr);
    for (auto& n : ins) inC.add(escapeMenu(n).c_str(), 0, nullptr);
    inC.value(0);
    for (size_t k = 0; k < ins.size(); ++k)
        if (ins[k] == midi.input()) inC.value((int)k + 1);
#ifdef _WIN32
    outC.add("None", 0, nullptr);
#else
    outC.add("Virtual port \"Moritz\"", 0, nullptr);
#endif
    for (auto& n : outs) outC.add(escapeMenu(n).c_str(), 0, nullptr);
    outC.value(0);
    for (size_t k = 0; k < outs.size(); ++k)
        if (outs[k] == midi.output()) outC.value((int)k + 1);

    int result = 0;
    ok.callback([](Fl_Widget* w, void* r) { *(int*)r = 1; w->window()->hide(); }, &result);
    cancel.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.show();
    while (dlg.shown()) Fl::wait();
    if (result != 1) return;
    midi.setInput(inC.value() > 0 ? ins[(size_t)inC.value() - 1] : "");
    midi.setOutput(outC.value() > 0 ? outs[(size_t)outC.value() - 1] : "");
    log(midi.describe());
    if (!midi.error.empty()) { log("MIDI: " + midi.error); midi.error.clear(); }
}

void App::audioSetup() {
    audio.refreshDevices();
    Fl_Double_Window dlg(440, 300, "Audio Setup");
    centerOnScreen(&dlg, frontDoc());
    Fl_Choice outC(130, 15, 290, 25, "Output device:");
    Fl_Choice inC(130, 50, 290, 25, "Input device:");
    Fl_Choice srC(130, 85, 120, 25, "Sample rate:");
    Fl_Choice bufC(130, 120, 120, 25, "Buffer size:");
    Fl_Choice ochC(130, 155, 120, 25, "Output channels:");
    Fl_Choice ichC(130, 190, 120, 25, "Input channels:");
    Fl_Box note(20, 218, 400, 34,
                "All: every channel of the device (dac~ 1 ... n, adc~ 1 ... n). Input opens only "
                "when a patch contains adc~.");
    note.labelsize(11);
    note.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    Fl_Button cancel(240, 262, 85, 28, "Cancel");
    Fl_Return_Button ok(335, 262, 85, 28, "OK");
    dlg.end();
    dlg.set_modal();

    outC.add("Default", 0, nullptr);
    for (auto& d : audio.outputs) outC.add(escapeMenu(d.name).c_str(), 0, nullptr);
    outC.value(audio.settings.output + 1 < outC.size() ? audio.settings.output + 1 : 0);

    inC.add("None", 0, nullptr);
    inC.add("Default", 0, nullptr);
    for (auto& d : audio.inputs) inC.add(escapeMenu(d.name).c_str(), 0, nullptr);
    inC.value(audio.settings.input + 2 < inC.size() ? audio.settings.input + 2 : 1);

    const int rates[] = {0, 44100, 48000, 88200, 96000};
    for (int r : rates) srC.add(r ? std::to_string(r).c_str() : "Default", 0, nullptr);
    srC.value(0);
    for (int k = 0; k < 5; ++k)
        if (rates[k] == audio.settings.sampleRate) srC.value(k);

    const int bufs[] = {64, 128, 256, 512, 1024, 2048};
    for (int b : bufs) bufC.add(std::to_string(b).c_str(), 0, nullptr);
    bufC.value(2);
    for (int k = 0; k < 6; ++k)
        if (bufs[k] == audio.settings.bufferFrames) bufC.value(k);

    const int counts[] = {0, 1, 2, 4, 6, 8, 12, 16, 24, 32, 64};
    for (int c : counts) {
        ochC.add(c ? std::to_string(c).c_str() : "All", 0, nullptr);
        ichC.add(c ? std::to_string(c).c_str() : "All", 0, nullptr);
    }
    ochC.value(0);
    ichC.value(0);
    for (int k = 0; k < 11; ++k) {
        if (counts[k] == audio.settings.outChannels) ochC.value(k);
        if (counts[k] == audio.settings.inChannels) ichC.value(k);
    }

    int result = 0;
    ok.callback([](Fl_Widget* w, void* r) { *(int*)r = 1; w->window()->hide(); }, &result);
    cancel.callback([](Fl_Widget* w, void*) { w->window()->hide(); });
    dlg.show();
    while (dlg.shown()) Fl::wait();
    if (result != 1) return;

    audio.settings.output = outC.value() - 1;
    audio.settings.input = inC.value() - 2;
    audio.settings.sampleRate = rates[srC.value()];
    audio.settings.bufferFrames = bufs[bufC.value()];
    audio.settings.outChannels = counts[ochC.value()];
    audio.settings.inChannels = counts[ichC.value()];
    setAudio(true);
}

}  // namespace moritz
