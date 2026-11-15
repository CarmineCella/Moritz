// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// main.cpp — entry point: the GUI, or a headless `--render` mode for tests
//
//   moritz [patch.moritz]
//   moritz --render patch.moritz seconds [out.wav] [bang=<id>] [send=<id>:<message>] ...
//          [png=<file.png>] [edit] [size=<w>x<h>] [extra=<patch.moritz>]
//          [midi=<seconds>:<bytes>]   e.g. midi=0.5:144 60 100 (MIDI output is printed)
//
// png= draws the patch, as it looks after the render, into an image (locked
// mode, or edit mode with "edit"), so drawing can be checked without a screen.

#include "gui.h"

#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_PNG_Image.H>
#ifdef __APPLE__
#include <FL/platform.H>   // fl_open_callback (on Linux it would pull in X11's macros)
#else
void fl_open_display();    // declared in FL/platform.H, which drags in X11 here
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef MORITZ_VERSION
#define MORITZ_VERSION "dev"
#endif

namespace {

moritz::MainWindow* gWin = nullptr;

#ifdef __APPLE__
void openFromFinder(const char* path) {
    if (gWin && gWin->confirmDiscard()) gWin->openFile(path);
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
    size_t slash = path.find_last_of('/');
    p.dir = slash == std::string::npos ? "." : path.substr(0, slash);
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
            size_t xs = xpath.find_last_of('/');
            xp->dir = xs == std::string::npos ? "." : xpath.substr(0, xs);
            moritz::Patch* q = p.addExtra(std::move(xp));
            if (!q->load(xsrc, xerr)) { std::fprintf(stderr, "error: %s\n", xerr.c_str()); return 1; }
            q->loadbangAll();
            continue;
        }
        if (a == "edit") { editMode = true; continue; }
        if (a.rfind("size=", 0) == 0) { std::sscanf(a.c_str() + 5, "%dx%d", &shotW, &shotH); continue; }
        bool isBang = a.rfind("bang=", 0) == 0, isSend = a.rfind("send=", 0) == 0;
        if (isBang || isSend) {
            std::string rest = a.substr(5), id = rest.substr(0, rest.find(':'));
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
                if (!m.empty()) b->message(0, m);
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
    if (argc >= 2 && std::string(argv[1]) == "--render") return renderCli(argc, argv);
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
    auto* win = new moritz::MainWindow(960, 640);
    gWin = win;
#ifdef __APPLE__
    fl_open_callback(openFromFinder);    // double-clicked .moritz files in the Finder
#endif
    win->show();
    for (int k = 1; k < argc; ++k)
        if (argv[k][0] != '-') { win->openFile(argv[k]); break; }
    int r = Fl::run();
    delete win;
    return r;
}
