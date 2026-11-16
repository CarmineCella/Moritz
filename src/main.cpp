// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// main.cpp — entry point: the GUI, or a headless `--render` mode for tests
//
//   moritz [--no-audio] [patch.moritz ...]
//   moritz --import patch.maxpat [out.moritz]   converts a Max patch (to stdout,
//          or to out.moritz with its abstractions converted next to the original)
//   moritz --render patch.moritz seconds [out.wav] [bang=<id>] [send=<id>:<message>] ...
//          (bang=<id>@<seconds> and send=<id>:<message>@<seconds> act later in the render)
//          [png=<file.png>] [edit] [size=<w>x<h>] [extra=<patch.moritz>]
//          [midi=<seconds>:<bytes>]   e.g. midi=0.5:144 60 100 (MIDI output is printed)
//
// png= draws the patch, as it looks after the render, into an image (locked
// mode, or edit mode with "edit"), so drawing can be checked without a screen.

#include "gui.h"
#include "maximport.h"

#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_PNG_Image.H>
#ifdef __APPLE__
#include <FL/platform.H>   // fl_open_callback (on Linux it would pull in X11's macros)
#else
void fl_open_display();    // declared in FL/platform.H, which drags in X11 here
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef MORITZ_VERSION
#define MORITZ_VERSION "dev"
#endif

namespace {

#ifdef __APPLE__
void openFromFinder(const char* path) {    // double-clicked in the Finder: its own window
    moritz::App::get().openDocument(path);
}
#endif

void writeWav(const char* path, const std::vector<float>& data, int ch, int sr) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = (uint32_t)data.size() * 2;
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16((uint16_t)ch); u32(sr); u32(sr * ch * 2); u16((uint16_t)(ch * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    for (float s : data) {
        int16_t v = (int16_t)std::lrint(std::clamp(s, -1.f, 1.f) * 32767.f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// Draws the patch into a PNG the size of its contents (or w x h).
int snapshot(moritz::Patch& p, const std::string& path, bool edit, int W, int H) {
    fl_open_display();
    Fl_Window win(2000, 2000);
    auto* canvas = new moritz::Canvas(0, 0, 2000, 2000, nullptr, &p);
    canvas->scrollable = false;
    win.end();
    canvas->setLocked(!edit);
    {                                     // lay the boxes out: sizes depend on the font
        Fl_Image_Surface probe(8, 8);
        Fl_Surface_Device::push_current(&probe);
        canvas->draw();
        Fl_Surface_Device::pop_current();
    }
    if (W <= 0 || H <= 0) {
        W = 200; H = 120;
        for (auto& b : p.boxes) {
            W = std::max(W, b->x + b->w + 20);
            H = std::max(H, b->y + b->h + 20);
        }
    }
    canvas->resize(0, 0, W, H);
    Fl_Image_Surface surf(W, H);
    Fl_Surface_Device::push_current(&surf);
    canvas->draw();
    Fl_Surface_Device::pop_current();
    Fl_RGB_Image* img = surf.image();
    int r = fl_write_png(path.c_str(), img);
    delete img;
    if (r != 0) { std::fprintf(stderr, "can't write %s\n", path.c_str()); return 1; }
    return 0;
}

// moritz --render patch.moritz seconds [out.wav] [bang=<id> ...]
// Runs a patch offline (no GUI, no audio device) and prints levels.
int renderCli(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: moritz --render patch.moritz seconds [out.wav] [bang=id] [send=id:message] ...\n");
        return 2;
    }
    std::string src, err, path = argv[2];
    if (!moritz::readFile(path, src)) { std::fprintf(stderr, "can't read %s\n", argv[2]); return 1; }
    const int sr = 44100, ch = 2, blk = dsp::kBlock;
    long frame = 0;
    moritz::Patch p;
    p.onStatus = [](const std::string& s) { std::fprintf(stderr, "status: %s\n", s.c_str()); };
    p.onPrint = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    p.onMidiOut = [](const std::vector<unsigned char>& m) {
        std::printf("midi out:");
        for (auto b : m) std::printf(" %d", b);
        std::printf("\n");
    };
    p.clock = [&frame] { return (double)frame / sr; };     // the scheduler follows sample time
    p.sampleRate = sr;
    p.dspRunning = true;
    p.ioBufferFrames = blk;
    moritz::jobsSynchronous = true;        // background jobs finish before the next message
    p.dir = moritz::dirOf(path);
    struct Later { double at; moritz::Box* box; moritz::Message msg; };
    std::vector<Later> later;
    moritz::setupSearchPaths();
    if (!p.load(src, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (!err.empty()) std::fprintf(stderr, "warning: %s\n", err.c_str());
    if (!p.dspError.empty()) std::fprintf(stderr, "dsp: %s\n", p.dspError.c_str());
    p.loadbangAll();
    const char* wav = nullptr;
    std::string png;
    bool editMode = false;
    int shotW = 0, shotH = 0;
    for (int k = 4; k < argc; ++k) {
        std::string a = argv[k];
        if (a.rfind("--", 0) == 0) continue;     // an option (--no-audio...): never an output file
        if (a.rfind("png=", 0) == 0) { png = a.substr(4); continue; }
        if (a.rfind("midi=", 0) == 0) {       // a MIDI message at a time, as if from a device
            std::string rest = a.substr(5);
            size_t colon = rest.find(':');
            double t = colon == std::string::npos ? 0.0 : std::atof(rest.substr(0, colon).c_str());
            std::vector<unsigned char> bytes;
            for (auto& at : moritz::parseMessage(colon == std::string::npos ? rest : rest.substr(colon + 1)))
                if (at.isFloat()) bytes.push_back((unsigned char)at.f);
            p.scheduleAt(nullptr, t, [&p, bytes] { p.midiIn(bytes); });
            continue;
        }
        if (a.rfind("extra=", 0) == 0) {      // run another patch alongside, as help windows do
            std::string xsrc, xerr, xpath = a.substr(6);
            if (!moritz::readFile(xpath, xsrc)) { std::fprintf(stderr, "can't read %s\n", xpath.c_str()); return 1; }
            auto xp = std::make_unique<moritz::Patch>();
            xp->dir = moritz::dirOf(xpath);
            moritz::Patch* q = p.addExtra(std::move(xp));
            if (!q->load(xsrc, xerr)) { std::fprintf(stderr, "error: %s\n", xerr.c_str()); return 1; }
            q->loadbangAll();
            continue;
        }
        if (a == "edit") { editMode = true; continue; }
        if (a.rfind("size=", 0) == 0) { std::sscanf(a.c_str() + 5, "%dx%d", &shotW, &shotH); continue; }
        bool isBang = a.rfind("bang=", 0) == 0, isSend = a.rfind("send=", 0) == 0;
        if (isBang || isSend) {
            std::string rest = a.substr(5);
            double at = 0;                          // bang=id@4.5: at 4.5 seconds into the render
            if (size_t k = rest.rfind('@'); k != std::string::npos) {
                char* end = nullptr;
                double t = std::strtod(rest.c_str() + k + 1, &end);
                if (end && *end == '\0' && k + 1 < rest.size()) { at = t; rest.resize(k); }
            }
            std::string id = rest.substr(0, rest.find(':'));
            moritz::Message m = isBang ? moritz::Message{moritz::Atom("bang")}
                                     : moritz::parseMessage(rest.substr(rest.find(':') + 1));
            moritz::Box* b = p.find(id);
            if (!b && id.find('/') != std::string::npos) {     // inner box: p1/osc
                moritz::Patch* q = &p;
                size_t start = 0;
                for (;;) {
                    size_t end = id.find('/', start);
                    b = q ? q->find(id.substr(start, end == std::string::npos ? end : end - start)) : nullptr;
                    if (end == std::string::npos || !b) break;
                    q = b->inner();
                    start = end + 1;
                }
            }
            if (b) {
                if (m.empty()) continue;
                if (at > 0) later.push_back({at, b, m});
                else b->message(0, m);
            } else {
                std::fprintf(stderr, "no box '%s'\n", id.c_str());
            }
        } else {
            wav = argv[k];
        }
    }
    long total = (long)(std::atof(argv[3]) * sr);
    std::vector<float> all, buf(blk * ch);
    for (; frame < total; frame += blk) {
        for (auto& l : later)                     // timed bang= and send=
            if (l.box && l.at <= (double)frame / sr) { l.box->message(0, l.msg); l.box = nullptr; }
        p.runDue((double)frame / sr);
        int n = (int)std::min<long>(blk, total - frame);
        p.process(nullptr, 0, buf.data(), ch, n);
        all.insert(all.end(), buf.begin(), buf.begin() + n * ch);
    }
    for (int c = 0; c < ch; ++c) {
        double peak = 0, sum = 0;
        long n = 0;
        for (size_t i = c; i < all.size(); i += ch, ++n) {
            peak = std::max(peak, (double)std::fabs(all[i]));
            sum += all[i] * all[i];
        }
        std::printf("ch%d peak %.4f rms %.4f\n", c + 1, peak, n ? std::sqrt(sum / n) : 0.0);
    }
    if (wav) writeWav(wav, all, ch, sr);
    if (!png.empty()) return snapshot(p, png, editMode, shotW, shotH);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // moritz.exe is a GUI program: from a command prompt, print to that
    // console (unless the output is already redirected to a file or pipe)
    if (argc >= 2 && argv[1][0] == '-' && AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_CHAR) {
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
        }
    }
#endif
    if (argc >= 2 && std::string(argv[1]) == "--render") return renderCli(argc, argv);
    if (argc >= 3 && std::string(argv[1]) == "--import") {     // Max patch -> Moritz text
        moritz::setupSearchPaths();
        std::string text, err;
        moritz::MaxImportReport rep;
        bool write = argc >= 4;
        if (!moritz::importMaxPatch(moritz::portablePath(argv[2]), write, text, rep, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "%s\n", rep.summary().c_str());
        if (!write) { std::fputs(text.c_str(), stdout); return 0; }
        FILE* f = std::fopen(argv[3], "wb");
        if (!f || std::fputs(text.c_str(), f) < 0) { std::fprintf(stderr, "can't write %s\n", argv[3]); return 1; }
        std::fclose(f);
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::printf("Moritz %s\n", MORITZ_VERSION);
        return 0;
    }

    Fl::lock();                          // lets the MIDI thread wake the GUI (Fl::awake)
    Fl::scheme("gtk+");
    Fl::visual(FL_DOUBLE | FL_RGB);
    Fl::background(42, 44, 50);          // dark widgets for dialogs and choices
    Fl::background2(24, 25, 28);
    Fl::foreground(226, 228, 234);
    Fl::set_color(FL_SELECTION_COLOR, 52, 92, 160);
    moritz::setupSearchPaths();
    auto& app = moritz::App::get();
    app.init();
#ifdef __APPLE__
    fl_open_callback(openFromFinder);    // double-clicked .moritz files in the Finder
#endif
    for (int k = 1; k < argc; ++k)
        if (std::string(argv[k]) == "--no-audio") app.startAudio = false;
    for (int k = 1; k < argc; ++k)       // each patch on the command line in its window
        if (argv[k][0] != '-') app.openDocument(moritz::portablePath(argv[k]));
    bool opened = !app.docs.empty();
    if (app.docs.empty()) app.newDocument();
    if (!opened && app.browserAtStartup()) app.showBrowser();   // a first look around
    // On macOS the program stays open without windows (the menu bar is
    // still there); elsewhere closing the last window quits.
    while (!app.quitting && !Fl::program_should_quit()) {
#ifndef __APPLE__
        if (!Fl::first_window()) break;
#endif
        Fl::wait(1e20);
    }
    app.audio.stop();
    return 0;
}
