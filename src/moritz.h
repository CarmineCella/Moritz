// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// moritz.h — Moritz, a Max-like environment for sound and music computing
//
// Patch model (boxes, connections, messages), patch file format and audio
// engine. DSP kernels live in dsp.h, the GUI in gui.h.
//
// Threading model
//   * Everything except Patch::process() runs on the GUI (main) thread:
//     editing, message passing, file I/O.
//   * The audio callback calls Patch::process(). It only try_locks the
//     patch's DSP mutex, so it never blocks; while the GUI holds the lock
//     during a structural edit, one block of silence is produced.
//   * Values the GUI sends to signal objects while audio runs (floats on
//     signal inlets, bangs to click~) go through atomics; ramps for line~
//     go through a lock-free queue. Audio -> GUI values (meter~, scope~,
//     snapshot~) are atomics read by the GUI.
//   * Timed objects (metro, delay, line, ...) use the patch's scheduler,
//     which runs on the GUI thread (like Max with Overdrive off).
//   * A patch the render object runs belongs to the render's worker thread
//     while it renders (Patch::setDispatchThread): messages other threads
//     send into it (a code box's output, its (send ...), a rebuild) are
//     queued and dispatched by the worker between blocks, so one thread
//     only ever dispatches its messages and touches its scheduler.
//
// Nesting
//   * Subpatchers (p), abstractions and pfft~ own a child Patch. The top
//     patch (root) owns everything shared: the DSP lock, sample rate,
//     scheduler, audio I/O, send/receive and buffer~ names. A child's signal
//     chain is run by its owner box from inside the root's chain.

#pragma once

#include "recorder.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "miniaudio.h"

namespace moritz {

// ====================================================================== atoms

struct Atom {
    enum Type { Float, Symbol };
    Type type = Float;
    float f = 0.f;
    std::string s;
    // A number written with a decimal point ("2.", "0.5"): in arguments it
    // makes Max objects work in floats ([+ 2.], [pack 0. 0]); kept on save.
    bool dec = false;

    Atom() = default;
    Atom(float v) : type(Float), f(v) {}
    Atom(double v) : type(Float), f((float)v) {}
    Atom(int v) : type(Float), f((float)v) {}
    Atom(const std::string& v) : type(Symbol), s(v) {}
    Atom(const char* v) : type(Symbol), s(v) {}

    bool isFloat() const { return type == Float; }
    bool isSymbol() const { return type == Symbol; }
    bool is(const char* sym) const { return type == Symbol && s == sym; }
    bool isDecimal() const { return type == Float && dec; }
    static Atom decimal(float v) { Atom a(v); a.dec = true; return a; }
    std::string str() const;
};

using Message = std::vector<Atom>;

std::string formatFloat(float v);
Atom parseAtom(const std::string& token);            // number if it parses, else symbol
std::vector<std::string> splitTokens(const std::string& text);  // ';' and ',' are tokens
Message parseMessage(const std::string& text);

// ---- paths (portable: macOS, Linux, Windows)
std::string homeDir();                          // HOME, or USERPROFILE on Windows
bool isAbsolutePath(const std::string& p);      // /x, C:/x, C:\x, \\server
std::string portablePath(std::string p);        // on Windows, backslashes become slashes
std::string dirOf(const std::string& path);     // folder of a file path, "." if none
std::string messageToString(const Message& m);

// ====================================================================== s-expressions

struct SExpr {
    bool isList = false;
    bool quoted = false;     // atom was a "string"
    std::string text;        // atom text
    std::vector<SExpr> items;
    int line = 0;
};

bool parseSExprs(const std::string& src, std::vector<SExpr>& out, std::string& err);

// ====================================================================== boxes

class Patch;

// Tap: the special tapin~ -> tapout~ link. It shares a delay line without
// ordering the two objects, so feedback through a delay is allowed.
enum class Port { Control, Signal, Tap };
enum class BoxKind { Object, Message, Number, UI, Comment, Broken };

class Box {
public:
    virtual ~Box() = default;

    // ---- identity / layout
    std::string id;          // name used in the patch file
    std::string cls;         // object class, e.g. "cycle~"
    Message args;            // creation arguments
    // Inside an abstraction, arguments as written (with #1, #0 ...): shown,
    // edited and saved instead of the substituted ones the box was made from.
    Message rawArgs;
    bool keepRaw = false;
    std::string label() const;   // text shown on the canvas
    int x = 0, y = 0, w = 60, h = 22;
    Patch* patch = nullptr;
    BoxKind kind = BoxKind::Object;
    std::vector<Port> ins, outs;

    // ---- behaviour
    // GUI thread. By default a number on a signal inlet becomes its value
    // while nothing is connected there.
    virtual void message(int inlet, const Message& m) {
        if (inlet >= 0 && inlet < numIns() && ins[(size_t)inlet] == Port::Signal && !m.empty() && m[0].isFloat())
            setScalar(inlet, m[0].f);
    }
    virtual void dsp(int n) {}                            // audio thread
    virtual void loadbang() {}
    virtual std::string text() const;                     // text shown in the box
    virtual Message saveArgs() const { return args; }     // args written to file
    virtual bool wantsInput() const { return false; }     // uses audio input (adc~)
    virtual int outputChannels() const { return 0; }      // the highest output channel it writes (dac~)
    virtual void attached() {}         // box joined a patch (GUI thread): load files, start timers
    virtual void resolve() {}          // during compile, DSP lock held: look up buffers/taps
    // a buffer~'s contents were replaced (GUI thread, lock not held): conv~ prepares again
    virtual void buffersChanged() {}
    virtual void audioChanged() {}     // audio started, stopped or reopened (GUI thread)
    virtual void midiIn(const std::vector<unsigned char>& msg) {}   // a MIDI message arrived (GUI thread)
    // send~ / receive~: the chain runs a send~ before the receive~s of its name
    virtual std::string signalSendName() const { return ""; }
    virtual std::string signalReceiveName() const { return ""; }

    // ---- boxes holding a patch (p, abstractions, pfft~)
    virtual Patch* inner() const { return nullptr; }       // the one a window shows
    virtual std::vector<Patch*> inners() const {           // all of them (poly~ has many)
        Patch* p = inner();
        return p ? std::vector<Patch*>{p} : std::vector<Patch*>{};
    }
    virtual bool savesInner() const { return false; }   // written inline as (patch ...)
    virtual void syncPorts() {}        // during compile: ports follow the inner inlets/outlets
    virtual void loadInner(const SExpr& form, std::vector<std::string>& warnings) {}
    // ---- boxes that keep a text (code): saved as (script "line" "line" ...) after the position
    virtual std::string script() const { return ""; }
    virtual void setScript(const std::string& text) {}

    // ---- interface objects (BoxKind::UI): drawn and clicked in locked mode
    virtual void uiSize(int& w, int& h) const {}
    virtual void drawUI(int X, int Y) {}
    virtual void uiMouse(int event, int mx, int my) {}   // FL_PUSH/DRAG/RELEASE, box coords
    // right-click: extra items of this box's menu, and what each does
    virtual std::vector<std::string> menuItems() { return {}; }
    virtual void menuAction(int item) {}
    virtual bool animated() const { return false; }      // redraw continuously (meter~, scope~)

    bool isSignal() const;
    int numIns() const { return (int)ins.size(); }
    int numOuts() const { return (int)outs.size(); }

    // ---- control output (GUI thread)
    void outlet(int i, const Message& m);
    void outletFloat(int i, float v) { outlet(i, Message{Atom(v)}); }
    void outletBang(int i) { outlet(i, Message{Atom("bang")}); }

    // ---- signal plumbing
    void setPorts(std::vector<Port> in, std::vector<Port> out);
    void ensureBlock(int n);               // grow signal buffers to n samples (compile)
    const float* sigIn(int i, int n);      // summed input, or the inlet's scalar
    float* sigOut(int i) { return outBuf_[i].data(); }
    float scalar(int i) const { return scalar_[i].load(std::memory_order_relaxed); }
    void setScalar(int i, float v) { scalar_[i].store(v, std::memory_order_relaxed); }

    // set by Patch::compile(): source buffers feeding each signal inlet
    std::vector<std::vector<const float*>> inSrc;

    // GUI state
    bool selected = false;
    double flashUntil = 0.0;

private:
    std::vector<std::vector<float>> outBuf_, inBuf_;
    std::unique_ptr<std::atomic<float>[]> scalar_;
};

struct Connection {
    Box* from;
    int outlet;
    Box* to;
    int inlet;
    bool selected = false;
};

// Creates a box of class `cls`. `context` is the patch it is created for:
// it is used to find abstractions (cls.moritz next to the patch). Unknown
// classes produce a "broken" box that keeps its text so the patch
// round-trips unchanged.
std::unique_ptr<Box> makeBox(const std::string& cls, const Message& args,
                             Patch* context = nullptr);

// Folders searched for abstractions after the patch's own folder: first the
// libraries (the user's, then the bundled one), then abstractionPath() (the
// bundled examples). Library abstractions also appear in Patch > New and in
// completion.
std::vector<std::string>& libraryPath();
std::vector<std::string>& abstractionPath();
// Folders where a relative file that is not next to the patch is looked for
// (the bundled examples and their sounds): "sounds/Vox.wav" works anywhere.
std::vector<std::string>& soundPath();
// The head-related impulse responses of a direction, at a sample rate (objects_spatial.cpp)
void hrirFor(double sr, double az, double el, std::vector<float>& left, std::vector<float>& right);
// Opens a file, a folder or a URL with the system (a PDF in its viewer)
bool openWithSystem(const std::string& pathOrUrl);
// A file dialog (the GUI): "" when cancelled. filter as FLTK's ("Audio\t*.wav")
std::string chooseFile(bool save, const std::string& title, const std::string& filter = "",
                       const std::string& preset = "");
// The window of a buffer~ (its waveform, zoom, pan), following its contents
void openBufferWindow(Patch* p, const std::string& name);
std::vector<std::string> libraryNames();        // abstractions in the libraries, sorted
// Help patches: "<name>.help.moritz" in helpPath() or next to a library
// abstraction. Related objects share one (+~ -~ *~ /~ -> arith~). Returns ""
// when there is none.
std::vector<std::string>& helpPath();
std::string findHelp(const std::string& cls);
std::string findAbstraction(const std::string& name, const Patch* context);
bool readFile(const std::string& path, std::string& out);

// Checks port ranges, signal/control compatibility and duplicates.
bool canConnect(const std::vector<Connection>& conns, Box* from, int outlet, Box* to,
                int inlet, std::string* err);
struct ClassInfo {
    std::string name, group, desc;   // group "" = alias, not listed in menus
};
const std::vector<ClassInfo>& classInfos();     // every class, in menu order
std::vector<std::string> allClassNames();       // every creatable class, sorted
const char* describeClass(const std::string& cls);  // one-line help, "" if unknown

// ====================================================================== patch

class Patch {
public:
    Patch();
    ~Patch();

    std::vector<std::unique_ptr<Box>> boxes;
    std::vector<Connection> conns;

    // ---- nesting
    Patch* parent = nullptr;     // enclosing patch; nullptr for the top patch
    Box* owner = nullptr;        // the p / abstraction / pfft~ box holding this patch
    Patch* root() { Patch* p = this; while (p->parent) p = p->parent; return p; }
    const Patch* root() const { const Patch* p = this; while (p->parent) p = p->parent; return p; }
    std::mutex& mutex() { return root()->dspMutex; }
    void forEachBox(const std::function<void(Box*)>& fn);   // this patch and all inner ones
    void runChain(int n) { for (Box* b : chain_) b->dsp(n); }   // called by the owner box
    void structureChanged();     // notify this patch and the ones around it

    // Patches the root runs alongside its own boxes but never saves: open
    // help patches. Their parent is the root, so they share its audio
    // output, scheduler and send/receive names.
    std::vector<std::unique_ptr<Patch>> extras;
    Patch* addExtra(std::unique_ptr<Patch> p);
    void removeExtra(Patch* p);   // destroys it

    // abstractions: file, arguments (#1..#9) and instance number (#0)
    bool isAbstraction = false;
    std::string file;
    Message absArgs;
    int instance = 0;
    Atom substitute(const Atom& a) const;    // replaces #0..#9 inside abstractions

    // pfft~: set on its patch, which then processes one whole frame (all
    // bins) per dsp() call
    struct Spectral {
        int fftSize = 0, bins = 0;
    } spectral;
    const Spectral* spectralContext() const;   // nearest pfft~ frame, or nullptr
    int vectorSize() const;      // samples per dsp() call: 64, or a pfft~ frame's bins

    // called when any Patch is destroyed (the GUI closes its window)
    static std::function<void(Patch*)> onDestroyed;

    // GUI hooks
    std::function<void()> onRedraw;
    std::function<void(const std::string&)> onStatus;
    std::function<void(bool)> onDsp;     // "start"/"stop" sent to dac~/adc~
    std::function<void()> onStructure;   // boxes or connections changed
    std::function<void(const std::string&)> onPrint;   // print object / errors (console)
    std::function<void()> onSchedule;    // timer queue changed: re-arm the GUI timer

    // ---- editing (GUI thread)
    // Makes a box for this patch from arguments as written: inside an
    // abstraction #0..#9 are substituted (and the written form kept).
    std::unique_ptr<Box> create(const std::string& cls, const Message& written);
    Box* addBox(std::unique_ptr<Box> b, int x, int y, const std::string& id = "");
    Box* replaceBox(Box* old, std::unique_ptr<Box> nb);   // keeps valid connections
    void removeBoxes(const std::vector<Box*>& bs);
    bool connect(Box* from, int outlet, Box* to, int inlet, std::string* err = nullptr);
    void removeConnections(const std::vector<int>& indices);
    void clear();
    Box* find(const std::string& id) const;
    std::string uniqueId(const std::string& cls) const;

    // ---- file format
    std::string serialize(int indent = 0) const;
    bool load(const std::string& src, std::string& err);
    bool load(const SExpr& form, std::string& err);   // a parsed (patch ...) form
    // clipboard: the selected boxes as text, and pasting such a text
    std::string serializeBoxes(const std::vector<Box*>& sel) const;
    bool merge(const std::string& src, int dx, int dy, std::vector<Box*>& added, std::string& err);
    // the text view: applies an edited text, keeping the boxes that did not change
    bool applyText(const std::string& src, std::string& err);
    void loadbangAll();
    bool build(const SExpr& form, std::vector<std::unique_ptr<Box>>& nb,
               std::vector<Connection>& nc, std::vector<std::string>& warnings, std::string& err);

    // ---- messaging (GUI thread)
    void sendTo(const std::string& name, const Message& m);  // send / ; remote
    int depth = 0;                                           // recursion guard
    void status(const std::string& s) {
        if (onStatus) onStatus(s);
        else if (parent) parent->status(s);
    }
    void redraw() { if (onRedraw) onRedraw(); }
    void print(const std::string& s);

    // ---- the thread that dispatches messages: the GUI thread, unless set
    // (the render object's worker, for the patch it renders). From another
    // thread, messages (outlet, sendTo, rebuild) are queued for it, and it
    // runs them with drainForeign().
    void setDispatchThread(std::thread::id t);           // std::thread::id(): the GUI's again
    bool foreignThread() const;                          // the caller is not the dispatch thread
    void runOnDispatchThread(std::function<void()> fn);
    void drainForeign();

    // ---- scheduler (GUI thread). Times are in seconds on `clock`.
    std::function<double()> clock;     // set on the root
    double time() const { return root()->clock ? root()->clock() : 0.0; }
    void scheduleAt(Box* owner, double t, std::function<void()> fn);
    void schedule(Box* owner, double delay, std::function<void()> fn) {
        scheduleAt(owner, time() + delay, std::move(fn));
    }
    void cancel(Box* owner);
    void runDue(double t);
    double nextDue() const;   // +inf when idle

    // ---- files
    std::string dir;          // folder of the patch file, for relative paths ("" = parent's)
    std::string resolvePath(const std::string& p) const;
    void rebuild();           // recompile after a box changed what it refers to
    // the sample rate changed (the audio device reopened): compiles again if the boxes were
    // prepared for another rate, so they prepare (delay lines, head responses) here, on the
    // GUI thread before audio starts, never in dsp()
    void rateChanged();

    // ---- audio (audio thread)
    void process(const float* in, int inCh, float* out, int outCh, int frames);
    std::atomic<double> sampleRate{44100.0};
    bool needsInput() const;
    int maxOutputChannel() const;         // the highest dac~ channel in it, its inner patches and extras
    std::string dspError;   // non-empty when the signal graph has a loop
    std::atomic<bool> dspRunning{false};
    uint64_t blockCount = 0;  // audio thread: 64-sample blocks processed
    // set by the audio engine on the root, for dspstate~
    std::atomic<int> ioBufferFrames{0};   // device buffer (I/O vector) size; kept while off
    std::atomic<float> cpuLoad{0.f};      // share of each buffer's time spent computing, smoothed
    int renderDepth = 0;                  // moritz: how deep this patch is rendered inside others
    uint64_t compiles = 0;                // root: how many times the graph was compiled (tests)
    void audioChanged() { forEachBox([](Box* b) { b->audioChanged(); }); }

    // ---- MIDI (GUI thread): input goes to every box; output to the root's hook
    std::function<void(const std::vector<unsigned char>&)> onMidiOut;
    void midiIn(const std::vector<unsigned char>& msg) { forEachBox([&](Box* b) { b->midiIn(msg); }); }
    void midiOut(const std::vector<unsigned char>& msg) {
        if (Patch* r = root(); r->onMidiOut) r->onMidiOut(msg);
    }

    // current I/O block, valid inside process() for adc~/dac~
    const float* ioIn = nullptr;
    float* ioOut = nullptr;
    int ioInCh = 0, ioOutCh = 0, ioOffset = 0;

    std::mutex dspMutex;

private:
    void compile();        // caller holds the DSP lock; compiles inner patches too
    void recompile() { root()->compile(); }
    bool reaches(Box* from, Box* to) const;   // signal path from -> to
    void addSendReceiveEdges(const std::vector<Box*>& sig, std::map<Box*, std::vector<Box*>>& next,
                             std::map<Box*, int>& indeg);
    std::vector<Box*> chain_;
    struct Timer {
        double t;
        uint64_t seq;
        Box* owner;
        std::function<void()> fn;
    };
    std::vector<Timer> timers_;
    uint64_t timerSeq_ = 0;
    bool inRunDue_ = false;
    double planRate_ = 0.0;      // root: the sample rate of the last compile
    // messages from other threads for the dispatch thread (root)
    std::atomic<std::thread::id> dispatch_{};
    std::mutex foreignMutex_;
    std::vector<std::function<void()>> foreign_;
    std::atomic<bool> hasForeign_{false};
};

// The audio thread is inside Patch::process (for the allocation guard of
// the engine's tests: nothing in there may allocate or free).
bool inAudioProcess();

// ====================================================================== audio

struct AudioDevice {
    std::string name;
    ma_device_id id;
};

struct AudioSettings {
    int output = -1;          // index into AudioEngine::outputs, -1 = default
    int input = -1;           // index into AudioEngine::inputs, -1 = default, -2 = none
    int sampleRate = 0;       // 0 = device default
    int bufferFrames = 256;
    int outChannels = 0;      // channels opened on the devices: 0 = all they have
    int inChannels = 0;
};

class AudioEngine {
public:
    explicit AudioEngine(Patch& p);
    ~AudioEngine();

    bool start(std::string& err);
    void stop();
    bool running() const { return running_; }
    bool hasInput() const { return duplex_; }
    int sampleRate() const { return running_ ? (int)device_.sampleRate : 0; }
    std::string deviceName() const { return running_ ? device_.playback.name : ""; }
    int bufferFrames() const { return running_ ? (int)device_.playback.internalPeriodSizeInFrames : 0; }
    void refreshDevices();
    void updateInputNeed();   // reopen with/without input when adc~ appears/disappears

    AudioSettings settings;
    std::vector<AudioDevice> outputs, inputs;
    Recorder recorder;                       // the quick recorder: what goes to the dac
    // the output's peaks since the interface last read them (takePeak)
    static constexpr int kMeterChannels = 64;
    static constexpr int kMaxBus = 64;    // the patch's output channels, at most
    float takePeak(int channel) {
        return channel >= 0 && channel < kMeterChannels ? peak_[channel].exchange(0.f, std::memory_order_relaxed) : 0.f;
    }
    // the patch's output channels: the device's, or more when a dac~ uses
    // more (those are metered and recorded, not heard)
    int channels() const { return running_ ? bus_.load() : 2; }
    int deviceOutputs() const { return running_ ? (int)device_.playback.channels : 0; }
    int deviceInputs() const { return running_ && duplex_ ? (int)device_.capture.channels : 0; }
    // after the patches change: the bus follows the highest dac~ channel; a
    // console note when it is wider than the device
    void updateBus();
    // The binaural monitor: the patch's output channels as speakers around
    // the listener (azimuth, elevation in degrees per channel, azimuth
    // positive to the left), rendered for headphones on the device's
    // channels 1-2. Empty: off.
    void setMonitor(const std::vector<std::pair<float, float>>& speakers);
    bool monitoring() const { return monitor_.load() != nullptr; }
    // The channel tester: bursts of noise on one output channel (from 0; -1: off)
    std::atomic<int> testChannel{-1};
    std::atomic<float> testLevel{0.2f};

private:
    static void callback(ma_device* d, void* out, const void* in, ma_uint32 frames);
    bool open(std::string& err);
    void close();
    Patch& patch_;
    ma_context context_;
    ma_device device_;
    bool contextOk_ = false, running_ = false, duplex_ = false;
    std::atomic<float> peak_[kMeterChannels] = {};
    std::atomic<int> bus_{2};
    std::vector<float> scratch_;              // the bus, when wider than the device (made once)
    static constexpr int kScratchFrames = 8192;
    int notedBus_ = 0;
    struct Monitor {
        int channels = 0, taps = 0, pos = 0;
        std::vector<float> hl, hr;     // [channel * taps + t]
        std::vector<float> hist;       // [channel * 2 taps + i]: each sample written twice
    };
    std::atomic<Monitor*> monitor_{nullptr};
    std::vector<std::unique_ptr<Monitor>> monitors_;   // kept: the audio thread may still use an old one
    uint32_t noise_ = 22222u;
    float pink_[3] = {0.f, 0.f, 0.f};
    long testClock_ = 0;
    int lastTest_ = -1;
    void renderMonitor(Monitor& m, const float* bus, int busCh, float* out, int outCh, int frames);
    void addTest(float* bus, int busCh, int frames, double sr);
};

}  // namespace moritz
