// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// moritz.h — Moritz, a Max-like environment for sound and music computing
//
// Patch model (boxes, connections, messages), patch file format and audio
// engine. DSP kernels live in dsp.h, the GUI in gui.h.
//
// Threading model
//   * Everything except Patch::process() runs on the GUI (main) thread:
//     editing, message passing, file I/O. (A patch rendered by the render
//     object belongs to that render's worker thread instead: messages other
//     threads send into it are queued and dispatched by the worker.)
//   * The signal graph is compiled on the GUI thread into a plan: the order
//     of the boxes, the buffers of their signal ports (owned by the plan) and
//     the plans of the patches inside them. The plan is published to the
//     audio thread with one atomic pointer swap; the audio thread takes the
//     newest plan at the start of each callback and never waits for the GUI.
//     An edit never silences the audio: until the new plan is published the
//     old one keeps playing.
//   * What the audio thread may still be using (old plans, removed boxes,
//     replaced state) is retired, not freed: Patch::retire() keeps it until
//     the audio thread has started a callback after the swap (it says so
//     through an epoch counter), then the GUI thread frees it.
//   * State a box prepares for the audio thread (delay lines sized for the
//     rate, head responses, prepared impulse responses...) is built on the GUI
//     thread and published the same way (Published<T>, objects.h); objects
//     handed over and back go through a Mailbox<T> when each counts, in
//     order (a line~ list), or a Latest<T> when only the newest does (a
//     click~ shape, sfplay~'s file); both return the old one to the GUI
//     thread to be freed.
//   * Buffer~ storage: record~ and poke~ write it, play~ & co. read it. The
//     audio thread try_locks the patch's mutex at the start of each callback;
//     the GUI holds it only to swap a buffer~'s storage (O(1)), or briefly in
//     code that changes one in place. If the try_lock fails, buffer~ readers
//     see no buffer for that one callback; everything else plays on.
//   * Timestamped events: a message to a signal object (a number on a signal
//     inlet, bang to click~, line~ ramps, play~/record~/count~ commands...)
//     is stamped with a target sample time and queued for the box (one
//     single-producer queue per box). The audio thread applies it at that
//     sample inside the block. The stamp comes from the patch clock: the time
//     the scheduler meant the message to happen (a metro's tick, not the
//     moment the GUI timer fired) mapped onto the sample count, plus two
//     device buffers of latency (real time: the callback may already have
//     computed a buffer ahead, and the GUI timer may fire up to a buffer
//     late; a timer later than that lands late, at the start of the next
//     block). Relative timers (Patch::schedule) count from that same time,
//     so a metro never drifts. The mapping is anchored at each audio
//     callback (wall time against samples, smoothed). Rendering offline, the
//     clock is the sample count itself: no latency, and the scheduler runs
//     ahead to the end of each block, so timers land on their exact sample.
//     Events are applied at their offset by the box (Box::events(),
//     eventPieces); a value for a signal inlet changes there by itself
//     (sigIn). Inside pfft~ a call is a frame, not time: events apply at once.
//   * Audio -> GUI values (meter~, scope~, snapshot~) are atomics read by
//     the GUI.
//   * Timed objects (metro, delay, line, ...) use the patch's scheduler,
//     which runs on the GUI thread (like Max with Overdrive off).
//
// Nesting
//   * Subpatchers (p), abstractions and pfft~ own a child Patch. The top
//     patch (root) owns everything shared: the plan, the buffer lock, sample
//     rate, scheduler, audio I/O, send/receive and buffer~ names. A child's
//     plan is run by its owner box (runInner) from inside the root's plan.

#pragma once

#include "recorder.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
struct PatchPlan;

// Tap: the special tapin~ -> tapout~ link. It shares a delay line without
// ordering the two objects, so feedback through a delay is allowed.
enum class Port { Control, Signal, Tap };
enum class BoxKind { Object, Message, Number, UI, Comment, Broken };

// A timestamped event for a box's audio side (see the threading model).
// kind kEvScalar is a value for signal inlet `inlet`; kinds from kEvUser on
// are the box's own (click~'s bang, line~'s ramps ...).
struct Event {
    uint64_t at = 0;          // target sample time
    float a = 0.f, b = 0.f;
    int16_t inlet = -1;
    uint8_t kind = 0;
};
enum : uint8_t { kEvScalar = 0, kEvUser = 16 };

// What a box's signal ports are connected to in one plan: built on the GUI
// thread with the plan, read-only afterwards.
struct BoxIO {
    std::vector<float*> out;                       // per outlet: its buffer (nullptr: not a signal)
    std::vector<std::vector<const float*>> src;    // per inlet: the buffers summed into it
    std::vector<float*> scratch;                   // per signal inlet: room for a sum or its value
    std::vector<const PatchPlan*> inner;           // the plans of the patches the box holds
    float* sink = nullptr;                         // where an outlet the plan does not know writes
    // the double-precision path: positions past 2^24 samples (count~ -> index~,
    // line~ -> play~). outD: the outlet's double companion, if the box has one;
    // srcD: an inlet fed by exactly one such outlet sees it there
    std::vector<double*> outD;
    std::vector<const double*> srcD;
};

class Box {
public:
    Box();
    virtual ~Box();
    Box(const Box&) = delete;
    Box& operator=(const Box&) = delete;

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
    // while nothing is connected there (timestamped: it changes at its sample).
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
    // During compile, on the GUI thread, while the audio thread may still be
    // running the previous plan (and this box's dsp): look up buffers, taps,
    // names... into atomics, a BufferRef or Published<> state (objects.h);
    // never change in place what dsp() is using.
    virtual void resolve() {}
    // a buffer~'s contents were replaced (GUI thread): boxes that prepare from one look again
    virtual void buffersChanged() {}
    virtual void audioChanged() {}     // audio started, stopped or reopened (GUI thread)
    virtual void midiIn(const std::vector<unsigned char>& msg) {}   // a MIDI message arrived (GUI thread)
    // send~ / receive~: the chain runs a send~ before the receive~s of its name
    virtual std::string signalSendName() const { return ""; }
    virtual std::string signalReceiveName() const { return ""; }
    // the tap link (tapin~ -> tapout~) may end at this inlet
    virtual bool acceptsTap(int inlet) const { return false; }
    // this signal outlet also gives its values in double precision (count~, line~)
    virtual bool preciseOut(int outlet) const { return false; }

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

    // ---- signal plumbing (audio thread, inside dsp(), through the running plan)
    void setPorts(std::vector<Port> in, std::vector<Port> out);
    const float* sigIn(int i, int n);      // summed input, or the inlet's value (sample-accurate)
    float* sigOut(int i);
    bool connected(int i) const;           // a signal reaches inlet i
    double* sigOutPrecise(int i);          // the outlet's double companion (preciseOut), or nullptr
    const double* sigInPrecise(int i);     // the inlet's input in double precision, or nullptr
    int outCount() const;                  // the outlets the running plan knows (sigOutIf: nullptr
    float* sigOutIf(int i);                // for one that is not a signal): ports that change
    int innerCount() const;                // the inner plans it runs
    void runInner(int k, int n);           // runs the k-th inner patch (n samples, or bins in pfft~)
    // ---- control values: scalar(i) is the latest value given to inlet i;
    // setScalar (GUI thread) timestamps it when i is a signal inlet
    float scalar(int i) const;
    void setScalar(int i, float v);

    // ---- timed events. post(): GUI thread (or the patch's dispatch thread),
    // stamped with Patch::stamp(). In dsp(): the events due in this block,
    // in order, each at its offset 0..n-1.
    void post(uint8_t kind, float a = 0.f, float b = 0.f, int inlet = -1);
    int events() const;
    const Event& event(int k) const;
    int eventOffset(int k) const;

    // GUI state
    bool selected = false;
    double flashUntil = 0.0;

private:
    friend class Patch;
    friend struct PatchPlan;
    struct Scalars;
    struct Inbox;
    void beginBlock(uint64_t t0, int n, bool untimed);   // audio: take the events due
    void endBlock();                                     // audio: the values they set stay
    Inbox* inbox();                                      // GUI: made on first use
    std::atomic<Scalars*> scal_{nullptr};
    std::vector<std::unique_ptr<Scalars>> oldScalars_;   // kept: the audio thread may still read one
    std::atomic<Inbox*> inbox_{nullptr};
    const BoxIO* io_ = nullptr;                          // audio: its wiring in the running plan
    uint64_t lastStamp_ = 0;                             // GUI: stamps never go back
    // outlet(): the cables of each outlet (rebuilt when the patch's cables change)
    std::vector<std::vector<std::pair<Box*, int>>> targets_;
    uint64_t targetsGen_ = ~0ull;
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

// ====================================================================== plans

// The compiled signal graph of one patch (see the threading model): its
// boxes in running order with their wiring, the memory of their signal
// ports, and the plans of the patches inside them. Immutable once published.
struct PatchPlan {
    struct Step {
        Box* box;
        BoxIO io;
    };
    std::vector<Step> steps;
    std::vector<std::unique_ptr<PatchPlan>> owned;   // the inner patches' plans (and the root's extras)
    std::vector<const PatchPlan*> extras;            // root: the extras' plans, run after its own
    std::vector<float> arena;                        // the signal buffers
    std::vector<double> arenaD;                      // and their double companions
    int vs = 64;                                     // vector size (a pfft~ frame's bins inside one)
    bool spectral = false;                           // inside pfft~: a call is a frame, not time
    void run(int n) const;                           // audio thread
};

class ReceiveBox;
class BufferBox;

// ====================================================================== patch

class Patch {
public:
    Patch();
    ~Patch();
    Patch(const Patch&) = delete;
    Patch& operator=(const Patch&) = delete;

    std::vector<std::unique_ptr<Box>> boxes;
    std::vector<Connection> conns;

    // ---- nesting
    Patch* parent = nullptr;     // enclosing patch; nullptr for the top patch
    Box* owner = nullptr;        // the p / abstraction / pfft~ box holding this patch
    Patch* root() { Patch* p = this; while (p->parent) p = p->parent; return p; }
    const Patch* root() const { const Patch* p = this; while (p->parent) p = p->parent; return p; }
    // The buffer~ lock: the audio thread try_locks it for each callback; hold
    // it only to swap a buffer~'s storage, or to change one in place.
    std::mutex& mutex() { return root()->dspMutex; }
    void forEachBox(const std::function<void(Box*)>& fn);   // this patch and all inner ones
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
    // the receive boxes and the buffer~s of a name, anywhere under the root
    // (an index, rebuilt after the structure changes)
    const std::vector<ReceiveBox*>& receivers(const std::string& name);
    BufferBox* bufferNamed(const std::string& name);
    void namesChanged() { root()->indexDirty_ = true; }   // a receive or buffer~ was renamed

    // ---- the thread that dispatches messages: normally the GUI thread; a
    // patch the render object runs belongs to its worker for the render.
    // Messages from other threads (a code box's output, its (send ...)) are
    // queued and run by the owner at the next block (drainForeign).
    void setDispatchThread(std::thread::id t);
    bool foreignThread() const;                       // the caller is not the dispatch thread
    void runOnDispatchThread(std::function<void()> fn);
    void drainForeign();

    // ---- scheduler (GUI thread). Times are in seconds on `clock`.
    std::function<double()> clock;     // set on the root
    double time() const { return root()->clock ? root()->clock() : 0.0; }
    void scheduleAt(Box* owner, double t, std::function<void()> fn);
    // relative to the time the message being dispatched belongs to (a timer's own
    // time while it runs): a metro ticks at exact intervals, however late its timer fires
    void schedule(Box* owner, double delay, std::function<void()> fn) {
        scheduleAt(owner, logicalTime() + delay, std::move(fn));
    }
    void cancel(Box* owner);
    void runDue(double t);       // runs the timers due at or before t, in time order
    double nextDue() const;      // +inf when idle
    // the time the message being dispatched belongs to: a timer's own time
    // while it runs (not the moment it fired), else the clock
    double logicalTime() const;

    // ---- timed events
    bool offline = false;        // root: rendered offline, the clock is the sample count
    uint64_t stamp();            // GUI thread: the sample at which a message sent now takes effect
    uint64_t sampleTime() const { return root()->sampleTime_.load(std::memory_order_acquire); }

    // ---- files
    std::string dir;          // folder of the patch file, for relative paths ("" = parent's)
    std::string resolvePath(const std::string& p) const;
    void rebuild();           // recompile after a box changed what it refers to
    void rateChanged();       // the sample rate changed: compile again for it (before audio runs)

    // ---- what the audio thread may still use (GUI thread): x is freed once
    // the audio thread has started a callback after this call
    void retire(std::shared_ptr<void> x);
    // frees what can be freed; wait: until the audio thread has let go of
    // everything (at most about a second; what is left is freed later)
    void collectGarbage(bool wait = false);

    // ---- audio (audio thread)
    void process(const float* in, int inCh, float* out, int outCh, int frames);
    std::atomic<double> sampleRate{44100.0};
    bool needsInput() const;
    int maxOutputChannel() const;         // the highest dac~ channel in it, its inner patches and extras
    std::string dspError;   // non-empty when the signal graph has a loop
    std::atomic<bool> dspRunning{false};
    // the device stopped, or is about to start (GUI thread, while no callback runs): the clock is
    // anchored again by the first callback; until then a message is stamped for the next block
    void resetClock() { anchored_.store(false, std::memory_order_release); }
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
    friend struct PatchPlan;
    friend class Box;
    void compile();        // root: compiles everything into a plan and publishes it
    void recompile() { root()->compile(); }
    void buildPlan(PatchPlan& plan);   // this patch (and the ones inside it)
    bool reaches(Box* from, Box* to) const;   // signal path from -> to
    void cablesChanged() { ++connGen_; }
    void retireBoxes(std::vector<std::unique_ptr<Box>> gone);   // after a plan without them
    void rebuildIndex();
    void anchorClock();    // audio thread: where the clock is on the sample count
    // the plan (GUI side owner; the audio thread reads plan_)
    std::shared_ptr<PatchPlan> current_;
    std::atomic<const PatchPlan*> plan_{nullptr};
    // retirement: the GUI counts epochs, the audio thread says which it saw last
    std::atomic<uint64_t> epoch_{0}, seen_{0};
    std::atomic<bool> inside_{false};
    std::vector<std::pair<uint64_t, std::shared_ptr<void>>> garbage_;
    bool collectArmed_ = false;
    double planRate_ = 0.0;            // the sample rate the plan was made for
    // clock anchor: sample count minus wall time (s) * rate, smoothed (audio writes, GUI reads)
    std::atomic<double> clockOffset_{0.0};
    std::atomic<bool> anchored_{false};
    std::atomic<uint64_t> sampleTime_{0};
    double offsetEst_ = 0.0;
    // the cables changed (outlet() caches its targets)
    uint64_t connGen_ = 0;
    // receive and buffer~ names
    std::unordered_map<std::string, std::vector<ReceiveBox*>> receivers_;
    std::unordered_map<std::string, BufferBox*> buffers_;
    bool indexDirty_ = true;
    // messages from other threads, for the dispatch thread
    std::atomic<std::thread::id> dispatch_{};
    std::mutex foreignMutex_;
    std::vector<std::function<void()>> foreign_;
    std::atomic<bool> hasForeign_{false};
    // the scheduler: a binary heap, earliest first
    struct Timer {
        double t;
        uint64_t seq;
        Box* owner;
        std::function<void()> fn;
    };
    std::vector<Timer> timers_;
    uint64_t timerSeq_ = 0;
    bool inRunDue_ = false;
    double dueNow_ = 0.0;
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
