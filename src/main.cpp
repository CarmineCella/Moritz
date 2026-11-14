// main.cpp — entry point: the GUI, or a headless `--render` mode for tests
//
//   minlsp [patch.mlsp]
//   minlsp --render patch.mlsp seconds [out.wav] [bang=<id>] [send=<id>:<message>] ...

#include "gui.h"

#include <FL/platform.H>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef MINLSP_VERSION
#define MINLSP_VERSION "0.2"
#endif

namespace {

mlsp::MainWindow* gWin = nullptr;

void openFromFinder(const char* path) {
    if (gWin && gWin->confirmDiscard()) gWin->openFile(path);
}

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

// minlsp --render patch.mlsp seconds [out.wav] [bang=<id> ...]
// Runs a patch offline (no GUI, no audio device) and prints levels.
int renderCli(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: minlsp --render patch.mlsp seconds [out.wav] [bang=id] [send=id:message] ...\n");
        return 2;
    }
    std::string src, err, path = argv[2];
    if (!mlsp::readFile(path, src)) { std::fprintf(stderr, "can't read %s\n", argv[2]); return 1; }
    const int sr = 44100, ch = 2, blk = dsp::kBlock;
    long frame = 0;
    mlsp::Patch p;
    p.onStatus = [](const std::string& s) { std::fprintf(stderr, "status: %s\n", s.c_str()); };
    p.onPrint = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    p.clock = [&frame] { return (double)frame / sr; };     // the scheduler follows sample time
    p.sampleRate = sr;
    p.dspRunning = true;
    size_t slash = path.find_last_of('/');
    p.dir = slash == std::string::npos ? "." : path.substr(0, slash);
    if (std::string ex = mlsp::examplesDir(); !ex.empty()) mlsp::abstractionPath().push_back(ex);
    if (!p.load(src, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (!err.empty()) std::fprintf(stderr, "warning: %s\n", err.c_str());
    if (!p.dspError.empty()) std::fprintf(stderr, "dsp: %s\n", p.dspError.c_str());
    p.loadbangAll();
    const char* wav = nullptr;
    for (int k = 4; k < argc; ++k) {
        std::string a = argv[k];
        bool isBang = a.rfind("bang=", 0) == 0, isSend = a.rfind("send=", 0) == 0;
        if (isBang || isSend) {
            std::string rest = a.substr(5), id = rest.substr(0, rest.find(':'));
            mlsp::Message m = isBang ? mlsp::Message{mlsp::Atom("bang")}
                                     : mlsp::parseMessage(rest.substr(rest.find(':') + 1));
            mlsp::Box* b = p.find(id);
            if (!b && id.find('/') != std::string::npos) {     // inner box: p1/osc
                mlsp::Patch* q = &p;
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
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--render") return renderCli(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::printf("minlsp %s\n", MINLSP_VERSION);
        return 0;
    }

    Fl::scheme("gtk+");
    Fl::visual(FL_DOUBLE | FL_RGB);
    Fl::background(42, 44, 50);          // dark widgets for dialogs and choices
    Fl::background2(24, 25, 28);
    Fl::foreground(226, 228, 234);
    Fl::set_color(FL_SELECTION_COLOR, 52, 92, 160);
    if (std::string ex = mlsp::examplesDir(); !ex.empty()) mlsp::abstractionPath().push_back(ex);
    auto* win = new mlsp::MainWindow(960, 640);
    gWin = win;
    fl_open_callback(openFromFinder);
    win->show();
    for (int k = 1; k < argc; ++k)
        if (argv[k][0] != '-') { win->openFile(argv[k]); break; }
    int r = Fl::run();
    delete win;
    return r;
}
