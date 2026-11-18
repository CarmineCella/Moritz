// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// main.cpp — entry point: the GUI, or a headless `--render` mode for tests
//
//   moritz [--no-audio] [patch.moritz | script.mu ...]
//   moritz --run script.mu [-- args...] | --repl    Musil, the scripting language, without a window
//   moritz --import patch.maxpat [out.moritz]   converts a Max patch (to stdout,
//          or to out.moritz with its abstractions converted next to the original)
//   moritz --render patch.moritz seconds [out.wav] [bang=<id>] [send=<id>:<message>] ...
//          (bang=<id>@<seconds> and send=<id>:<message>@<seconds> act later in the render)
//          [png=<file.png>] [edit] [size=<w>x<h>] [zoom=<z>] [extra=<patch.moritz>]
//          [midi=<seconds>:<bytes>]   e.g. midi=0.5:144 60 100 (MIDI output is printed)
//
// png= draws the patch, as it looks after the render, into an image (locked
// mode, or edit mode with "edit"), so drawing can be checked without a screen.

#include "musil_cli.h"
#include <chrono>
#include "codewindow.h"
#include "session.h"
#include "gui.h"
#include "settings.h"
#include "reference.h"
#include "maximport.h"
#include "fileio.h"

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
// double-clicked in the Finder: its own window (once the application is ready: at launch the system
// can ask before the command line has been read)
void openFromFinder(const char* path) { moritz::App::get().openLater(path); }
#endif

// 16-bit, through Moritz's one WAV writer (recorder.h)
void writeWav(const char* path, const std::vector<float>& data, int ch, int sr) {
    moritz::WavWriter w;
    std::string err;
    if (!w.open(path, ch, sr, 16, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return; }
    w.write(data.data(), data.size() / (size_t)ch);
    if (!w.finish(&err)) std::fprintf(stderr, "%s\n", err.c_str());
}

// Draws the patch into a PNG the size of its contents (or w x h).
int snapshot(moritz::Patch& p, const std::string& path, bool edit, int W, int H, double zoom) {
    fl_open_display();
    Fl_Window win(2000, 2000);
    auto* canvas = new moritz::Canvas(0, 0, 2000, 2000, nullptr, &p);
    canvas->scrollable = false;
    win.end();
    canvas->setLocked(!edit);
    {                                     // lay the boxes out: sizes depend on the font
        Fl_Image_Surface probe(8, 8);
        Fl_Surface_Device::push_current(&probe);
        moritz::layoutPatch(&p);
        Fl_Surface_Device::pop_current();
    }
    if (W <= 0 || H <= 0) {
        W = 200; H = 120;
        for (auto& b : p.boxes) {
            W = std::max(W, b->x + b->w + 20);
            H = std::max(H, b->y + b->h + 20);
        }
        W = (int)(W * zoom);
        H = (int)(H * zoom);
    }
    canvas->resize(0, 0, W, H);
    canvas->setZoom(zoom);
    canvas->scrollTo(0, 0);
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
    dsp::FlushDenormalsScope ftz;        // as the audio callback: recursive filters never hit denormals
    const int sr = 44100, blk = dsp::kBlock;
    int ch = 2;                          // as many as the highest dac~ channel (at least 2)
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
    p.offline = true;                      // the clock is the sample count: events land on their sample
    p.ioBufferFrames = blk;
    moritz::jobsSynchronous = true;        // background jobs finish before the next message
    p.dir = moritz::dirOf(path);
    struct Later { double at; moritz::Box* box; moritz::Message msg; };
    std::vector<Later> later;
    moritz::setupSearchPaths();
    if (!p.load(src, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (!err.empty()) std::fprintf(stderr, "warning: %s\n", err.c_str());
    if (!p.dspError.empty()) std::fprintf(stderr, "dsp: %s\n", p.dspError.c_str());
    ch = std::clamp(p.maxOutputChannel(), 2, 64);
    p.loadbangAll();
    const char* wav = nullptr;
    std::string png;
    bool editMode = false;
    int shotW = 0, shotH = 0;
    double shotZoom = 1;
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
        if (a.rfind("zoom=", 0) == 0) { shotZoom = std::atof(a.c_str() + 5); continue; }
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
        int n = (int)std::min<long>(blk, total - frame);
        p.runDue((double)(frame + n) / sr);   // the timers due in this block: their events land inside it
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
    if (!png.empty()) return snapshot(p, png, editMode, shotW, shotH, shotZoom > 0 ? shotZoom : 1);
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
    moritz::loadSettings();      // the fonts the boxes and the code are drawn with (settings.h)
    if (argc >= 2 && std::string(argv[1]) == "--render") return renderCli(argc, argv);
    if (argc >= 2 && (std::string(argv[1]) == "--run" || std::string(argv[1]) == "--repl")) {
        moritz::setupSearchPaths();
        return moritz::musilCli(argc, argv);
    }
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
        std::string werr;
        if (!moritz::fileio::writeAtomic(argv[3], text, &werr)) { std::fprintf(stderr, "%s\n", werr.c_str()); return 1; }
        return 0;
    }
    // --code-names: every name a code box can call (tests/code-names.txt holds the expected list)
    if (argc >= 2 && std::string(argv[1]) == "--code-names") {
        moritz::setupSearchPaths();
        for (auto& n : moritz::codeBoxNames()) std::printf("%s\n", n.c_str());
        std::fflush(nullptr);
        std::_Exit(0);         // a one-shot answer: no static destructors (on Windows one could wait forever)
    }
    // --reference-md: OBJECTS.md from help/reference; --reference-missing: the
    // objects without a reference entry; --list-classes: name and group
    if (argc >= 2 && (std::string(argv[1]) == "--reference-md" || std::string(argv[1]) == "--reference-missing" ||
                      std::string(argv[1]) == "--list-classes")) {
        moritz::setupSearchPaths();
        std::string a = argv[1];
        if (a == "--reference-md") std::fputs(moritz::referenceMarkdown().c_str(), stdout);
        else if (a == "--reference-missing")
            for (auto& n : moritz::classesWithoutReference()) std::printf("%s\n", n.c_str());
        else
            for (auto& c : moritz::classInfos()) std::printf("%s\t%s\n", c.name.c_str(), c.group.empty() ? "(alias)" : c.group.c_str());
        std::fflush(nullptr);
        std::_Exit(0);
    }
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::printf("Moritz %s\n", MORITZ_VERSION);
        return 0;
    }

    Fl::lock();                          // lets the MIDI thread wake the GUI (Fl::awake)
#ifdef __APPLE__
    // before anything opens the display (App::init does): the files the Finder opens the program with
    // arrive as the display opens, and would be lost without it
    fl_open_callback(openFromFinder);
#endif
    Fl::scheme("gtk+");
    Fl::visual(FL_DOUBLE | FL_RGB);
    Fl::background(42, 44, 50);          // dark widgets for dialogs and choices
    Fl::background2(24, 25, 28);
    Fl::foreground(226, 228, 234);
    Fl::set_color(FL_SELECTION_COLOR, 52, 92, 160);
    moritz::setupSearchPaths();
    auto& app = moritz::App::get();
    bool windowPng = false;
    for (int k = 1; k < argc; ++k)
        if (std::string(argv[k]) == "--window-png") windowPng = true;
    // --window-png: drawn for tests, never waiting for an answer (dialogs answer answer=<n>, default 0)
    if (windowPng) moritz::setBatchMode(true);
    app.init();
    for (int k = 1; k < argc; ++k)
        if (std::string(argv[k]) == "--no-audio" || std::string(argv[k]) == "--window-png") app.startAudio = false;
    for (int k = 1; k < argc; ++k)       // each patch on the command line in its window
        if (argv[k][0] != '-' && std::string(argv[k]).find('=') == std::string::npos &&
            (k == 1 || std::string(argv[k - 1]) != "--window-png") && std::string(argv[k]) != "run")
            app.openDocument(moritz::portablePath(argv[k]));
    app.setReady();                      // and the files the system asked for meanwhile
    // --window-png out.png [patch] [zoom=z]: draws the first document window
    // (toolbar, canvas, status bar) into an image, without showing it: for tests.
    // With a code file (or open=<id> of a code box): its editor; run: runs it first; console=out2.png: the
    // Console too; settings=out3.png: the Settings dialog too. act=... are done first, in order
    // (guitest.cpp); out.png "none": no image.
    auto writePng = [](Fl_Window* w, const char* file) {
        if (std::string(file) == "none") return 0;
        Fl_Image_Surface surf(w->w(), w->h());
        Fl_Surface_Device::push_current(&surf);
        surf.draw(w);
        Fl_Surface_Device::pop_current();
        Fl_RGB_Image* img = surf.image();
        int r = fl_write_png(file, img);
        delete img;
        return r;
    };
    // open=<id>: the box sent "open" (a code box: its editor, drawn instead of the patch)
    for (int k = 1; k < argc; ++k)
        if (std::string(argv[k]).rfind("open=", 0) == 0 && !app.docs.empty())
            if (moritz::Box* b = app.docs.front()->patch.find(argv[k] + 5)) b->message(0, {moritz::Atom("open")});
    int failed = windowPng ? moritz::runTestActions(argc, argv) : 0;
    bool acts = false;
    for (int k = 1; k < argc; ++k) acts = acts || std::string(argv[k]).rfind("act=", 0) == 0;
    for (int k = 1; k + 1 < argc; ++k)
        if (std::string(argv[k]) == "--window-png" && !acts && !app.codeWindows().empty()) {
            moritz::CodeWindow* cw = app.codeWindows().front();
            bool run = false;
            std::string console;
            for (int j = 1; j < argc; ++j) {
                std::string a = argv[j];
                if (a == "run") run = true;
                if (a.rfind("console=", 0) == 0) console = a.substr(8);
            }
            if (run) {
                cw->runFile();
                for (int t = 0; t < 400 && moritz::sessionContext()->busy(); ++t) Fl::wait(0.05);
                for (auto t0 = std::chrono::steady_clock::now();
                     std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(700);) Fl::wait(0.05);
            }
            int r = writePng(cw, argv[k + 1]);
            if (!console.empty()) {
                app.ensureConsole();
                for (int t = 0; t < 5; ++t) Fl::wait(0.05);
                r |= writePng(app.consoleWindow(), console.c_str());
            }
            // the session's interpreter thread may still hold its lock: leave without static destructors
            std::fflush(nullptr);
            std::_Exit(r == 0 ? 0 : 1);
        }
    for (int k = 1; k + 1 < argc; ++k)
        if (std::string(argv[k]) == "--window-png") {
            if (app.docs.empty()) app.newDocument();
            moritz::DocumentWindow* w = app.docs.front();
            for (int j = 1; j < argc; ++j)
                if (std::string(argv[j]).rfind("zoom=", 0) == 0) w->canvas->setZoom(std::atof(argv[j] + 5));
            int r = writePng(w, argv[k + 1]);
            for (int j = 1; j < argc; ++j)
                if (std::string(argv[j]).rfind("settings=", 0) == 0) {
                    moritz::showSettings();
                    for (int t = 0; t < 5; ++t) Fl::wait(0.05);
                    r |= writePng(moritz::settingsWindow(), argv[j] + 9);
                }
            std::fflush(nullptr);
            if (acts) std::_Exit(r == 0 && failed == 0 ? 0 : 1);   // code boxes may still be running
            return r == 0 && failed == 0 ? 0 : 1;
        }
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
