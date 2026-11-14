// minlsp.h — a minimal Max/MSP-like patcher
//
// Patch model (boxes, connections, messages), patch file format, audio
// engine and GUI declarations. DSP kernels live in dsp.h.
//
// Threading model
//   * Everything except Patch::process() runs on the GUI (main) thread:
//     editing, message passing, file I/O.
//   * The audio callback calls Patch::process(). It only try_locks the
//     patch's DSP mutex, so it never blocks; while the GUI holds the lock
//     during a structural edit, one block of silence is produced.
//   * Values the GUI sends to signal objects while audio runs (floats on
//     signal inlets, bangs to click~) go through atomics.

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dsp.h"
#include "miniaudio.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Sys_Menu_Bar.H>

namespace mlsp {

// ====================================================================== atoms

struct Atom {
    enum Type { Float, Symbol };
    Type type = Float;
    float f = 0.f;
    std::string s;

    Atom() = default;
    Atom(float v) : type(Float), f(v) {}
    Atom(double v) : type(Float), f((float)v) {}
    Atom(int v) : type(Float), f((float)v) {}
    Atom(const std::string& v) : type(Symbol), s(v) {}
    Atom(const char* v) : type(Symbol), s(v) {}

    bool isFloat() const { return type == Float; }
    bool isSymbol() const { return type == Symbol; }
    bool is(const char* sym) const { return type == Symbol && s == sym; }
    std::string str() const;
};

using Message = std::vector<Atom>;

std::string formatFloat(float v);
Atom parseAtom(const std::string& token);            // number if it parses, else symbol
std::vector<std::string> splitTokens(const std::string& text);  // ';' and ',' are tokens
Message parseMessage(const std::string& text);
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

enum class Port { Control, Signal };
enum class BoxKind { Object, Message, Number, Broken };

class Box {
public:
    virtual ~Box() = default;

    // ---- identity / layout
    std::string id;          // name used in the patch file
    std::string cls;         // object class, e.g. "cycle~"
    Message args;            // creation arguments
    int x = 0, y = 0, w = 60, h = 22;
    Patch* patch = nullptr;
    BoxKind kind = BoxKind::Object;
    std::vector<Port> ins, outs;

    // ---- behaviour
    virtual void message(int inlet, const Message& m) {}  // GUI thread
    virtual void dsp(int n) {}                            // audio thread
    virtual void loadbang() {}
    virtual std::string text() const;                     // text shown in the box
    virtual Message saveArgs() const { return args; }     // args written to file
    virtual bool wantsInput() const { return false; }     // uses audio input (adc~)

    bool isSignal() const;
    int numIns() const { return (int)ins.size(); }
    int numOuts() const { return (int)outs.size(); }

    // ---- control output (GUI thread)
    void outlet(int i, const Message& m);
    void outletFloat(int i, float v) { outlet(i, Message{Atom(v)}); }
    void outletBang(int i) { outlet(i, Message{Atom("bang")}); }

    // ---- signal plumbing
    void setPorts(std::vector<Port> in, std::vector<Port> out);
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

// Creates a box of class `cls`. Unknown classes produce a "broken" box that
// keeps its text so the patch round-trips unchanged.
std::unique_ptr<Box> makeBox(const std::string& cls, const Message& args);

// Checks port ranges, signal/control compatibility and duplicates.
bool canConnect(const std::vector<Connection>& conns, Box* from, int outlet, Box* to,
                int inlet, std::string* err);
const std::vector<std::string>& boxClasses();   // classes offered in Patch > New
std::vector<std::string> allClassNames();       // every creatable class, sorted
const char* describeClass(const std::string& cls);  // one-line help, "" if unknown

// ====================================================================== patch

class Patch {
public:
    Patch();
    ~Patch();

    std::vector<std::unique_ptr<Box>> boxes;
    std::vector<Connection> conns;

    // GUI hooks
    std::function<void()> onRedraw;
    std::function<void(const std::string&)> onStatus;
    std::function<void(bool)> onDsp;     // "start"/"stop" sent to dac~/adc~
    std::function<void()> onStructure;   // boxes or connections changed

    // ---- editing (GUI thread)
    Box* addBox(std::unique_ptr<Box> b, int x, int y, const std::string& id = "");
    Box* replaceBox(Box* old, std::unique_ptr<Box> nb);   // keeps valid connections
    void removeBoxes(const std::vector<Box*>& bs);
    bool connect(Box* from, int outlet, Box* to, int inlet, std::string* err = nullptr);
    void removeConnections(const std::vector<int>& indices);
    void clear();
    Box* find(const std::string& id) const;
    std::string uniqueId(const std::string& cls) const;

    // ---- file format
    std::string serialize() const;
    bool load(const std::string& src, std::string& err);
    void loadbangAll();

    // ---- messaging (GUI thread)
    void sendTo(const std::string& name, const Message& m);  // send / ; remote
    int depth = 0;                                           // recursion guard
    void status(const std::string& s) { if (onStatus) onStatus(s); }
    void redraw() { if (onRedraw) onRedraw(); }

    // ---- audio (audio thread)
    void process(const float* in, int inCh, float* out, int outCh, int frames);
    std::atomic<double> sampleRate{44100.0};
    bool needsInput() const;
    std::string dspError;   // non-empty when the signal graph has a loop

    // current I/O block, valid inside process() for adc~/dac~
    const float* ioIn = nullptr;
    float* ioOut = nullptr;
    int ioInCh = 0, ioOutCh = 0, ioOffset = 0;

    std::mutex dspMutex;

private:
    void compile();        // caller holds dspMutex
    bool reaches(Box* from, Box* to) const;   // signal path from -> to
    std::vector<Box*> chain_;
};

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
    void refreshDevices();
    void updateInputNeed();   // reopen with/without input when adc~ appears/disappears

    AudioSettings settings;
    std::vector<AudioDevice> outputs, inputs;

private:
    static void callback(ma_device* d, void* out, const void* in, ma_uint32 frames);
    Patch& patch_;
    ma_context context_;
    ma_device device_;
    bool contextOk_ = false, running_ = false, duplex_ = false;
};

// ====================================================================== GUI

class MainWindow;

class Canvas : public Fl_Group {
public:
    Canvas(int x, int y, int w, int h, MainWindow* win);

    void draw() override;
    int handle(int e) override;

    void setLocked(bool l);
    bool locked() const { return locked_; }
    void newBoxAt(const std::string& cls, int x, int y);   // Patch > New
    void newBoxAtMouse(const std::string& cls);
    void deleteSelection();
    void selectAll();
    void commitEdit();
    void cancelEdit();
    void reset();          // forget selection and edit state (after load/undo)
    void duplicateSelection();   // option-drag: copies boxes and their inner cables

    // object-name completion while typing in a new/edited object box
    void updateSuggestions();
    bool hasSuggestions() const { return !sugg_.empty(); }
    void moveSuggestion(int delta);
    bool acceptSuggestion(bool commit);
    void clearSuggestions();

private:
    struct Hit {
        enum What { None, Body, Inlet, Outlet, Cable } what = None;
        Box* box = nullptr;
        int port = -1;
        int cable = -1;
    };
    Hit hitTest(int mx, int my) const;
    int inletAt(Box* b, int mx) const;
    void portPos(Box* b, bool out, int i, int& px, int& py) const;
    void layoutBox(Box* b) const;
    void drawBox(Box* b);
    void beginEdit(Box* b, bool isNew);
    void clearSelection();
    std::vector<Box*> selectedBoxes() const;
    void numberDrag(int dy);
    static void editCb(Fl_Widget* w, void* data);

    MainWindow* win_;
    bool locked_ = false;
    Fl_Input* editor_;
    Box* editing_ = nullptr;
    bool editingNew_ = false;

    enum class Drag { None, Move, Cable, Rubber, Number } drag_ = Drag::None;
    int pressX_ = 0, pressY_ = 0, curX_ = 0, curY_ = 0;
    bool moved_ = false;
    Box* dragBox_ = nullptr;
    int dragPort_ = 0;
    double dragStartValue_ = 0.0, dragStep_ = 1.0;
    Box* typingBox_ = nullptr;     // number box receiving typed digits
    std::string typed_;
    int mouseX_ = 60, mouseY_ = 60;

    std::vector<std::string> sugg_;
    int suggSel_ = 0;
    int suggX_ = 0, suggY_ = 0, suggW_ = 0;   // popup geometry (window coords)
    void drawSuggestions();
    int suggestionAt(int ex, int ey) const;
};

class MainWindow : public Fl_Double_Window {
public:
    MainWindow(int w, int h);
    ~MainWindow() override;

    Patch patch;
    AudioEngine audio;

    // File
    void newPatch();
    void openPatch();
    bool openFile(const std::string& path);
    bool save();
    bool saveAs();
    void quit();
    // Edit
    void pushUndo();
    void dropUndo();     // discard the last pushUndo() when nothing changed
    void undo();
    void redo();
    void markDirty();
    // Patch
    void toggleLock();
    void toggleAudio();
    void setAudio(bool on);
    void audioSetup();

    void setStatus(const std::string& s);
    void refreshStatus();
    void updateTitle();
    bool confirmDiscard();
    int handle(int e) override;

    Canvas* canvas = nullptr;

private:
    void buildMenu();
    void restore(const std::string& snapshot);

    Fl_Sys_Menu_Bar* menu_;
    Fl_Box* statusBar_;
    std::string path_;
    bool dirty_ = false;
    std::vector<std::string> undo_, redo_;
    std::string message_;
};

}  // namespace mlsp
