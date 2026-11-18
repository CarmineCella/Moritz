// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects.h — internals shared by the object files (objects_*.cpp), the
// class table and the canvas. Not part of the public model in moritz.h.
#pragma once

#include "moritz.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace moritz {

double now();   // seconds, steady clock (GUI flashes and animation)

using P = Port;

inline float argf(const Message& a, size_t i, float def) {
    return i < a.size() && a[i].isFloat() ? a[i].f : def;
}
inline std::string argstr(const Message& a, size_t i, const std::string& def = "") {
    return i < a.size() ? a[i].str() : def;
}
inline bool atomEq(const Atom& x, const Atom& y) {
    return x.type == y.type && (x.isFloat() ? x.f == y.f : x.s == y.s);
}
inline Message tailOf(const Message& m, size_t from = 1) {
    return from < m.size() ? Message(m.begin() + (long)from, m.end()) : Message{};
}
inline bool isIntegral(float v) { return std::isfinite(v) && v == std::floor(v); }

// "start" / "stop" sent to adc~, dac~ or "; dsp ..."
bool handleDspMessage(Patch* p, const Message& m);

// ---------------------------------------------------------------- class table
// Each objects_*.cpp file registers its classes; classes.cpp orders them by
// menu group and adds Max's short names.

using Factory = std::function<std::unique_ptr<Box>(const Message&, Patch* context)>;

struct ClassDef {
    const char* name;
    const char* group;   // "" = alias (completion only)
    const char* desc;
    Factory make;
};
using ClassTable = std::vector<ClassDef>;

template <class T>
Factory with() { return [](const Message& a, Patch*) { return std::make_unique<T>(a); }; }
template <class T>
Factory plain() { return [](const Message&, Patch*) { return std::make_unique<T>(); }; }

void registerSignalObjects(ClassTable& t);
void registerControlObjects(ClassTable& t);
void registerSampleObjects(ClassTable& t);
void registerPatcherObjects(ClassTable& t);
void registerMidiObjects(ClassTable& t);
void registerSoundObjects(ClassTable& t);
void registerOfflineObjects(ClassTable& t);
void registerLearnObjects(ClassTable& t);
void registerSpectralObjects(ClassTable& t);
void registerFilterObjects(ClassTable& t);
void registerSpatialObjects(ClassTable& t);
void registerMirObjects(ClassTable& t);
void registerPlotObjects(ClassTable& t);
void registerMapleObjects(ClassTable& t);
void registerSoundtypesObjects(ClassTable& t);
void registerCodeObjects(ClassTable& t);

// abstraction lookup, used by makeBox for unknown class names
std::unique_ptr<Box> makeAbstraction(const std::string& path, const Message& args, Patch* context);

// ---------------------------------------------------------------- boxes the canvas and patch use
// ---------------------------------------------------------------- broken
class BrokenBox : public Box {
public:
    BrokenBox() { kind = BoxKind::Broken; setPorts({}, {}); }
};


// ---------------------------------------------------------------- flonum / number
class NumberBox : public Box {
public:
    NumberBox(bool isFloat, const Message& a) : isFloat_(isFloat) {
        kind = BoxKind::Number;
        setPorts({P::Control}, {P::Control});
        value_ = fix(argf(a, 0, 0.f));
    }
    bool isFloat() const { return isFloat_; }
    double value() const { return value_; }
    std::string text() const override {
        if (!isFloat_) return formatFloat((float)value_);
        std::string t = formatFloat((float)value_);
        if (t.find_first_of(".en") == std::string::npos) t += ".";
        return t;
    }
    Message saveArgs() const override { return {Atom((float)value_)}; }
    void set(double v, bool output) {
        value_ = fix(v);
        if (patch) patch->redraw();
        if (output) outletFloat(0, (float)value_);
    }
    void message(int, const Message& m) override {
        if (m[0].isFloat()) set(m[0].f, true);
        else if (m[0].is("bang")) set(value_, true);
        else if (m[0].is("set") && m.size() > 1 && m[1].isFloat()) set(m[1].f, false);
    }

private:
    double fix(double v) const { return isFloat_ ? v : std::trunc(v); }
    bool isFloat_;
    double value_ = 0.0;
};

// ---------------------------------------------------------------- message
// Click or bang to send the contents. "$1".."$9" are replaced by the
// incoming message. "," separates messages, ";" starts a remote message
// whose first word names a receive object:  "; freq 440; amp 0.2"
class MessageBox : public Box {
public:
    explicit MessageBox(const Message& a) {
        kind = BoxKind::Message;
        setPorts({P::Control}, {P::Control});
        content = messageToString(a);
    }
    std::string text() const override { return content; }
    Message saveArgs() const override { return {Atom(content)}; }
    void message(int, const Message& m) override {
        if (m[0].is("set")) {
            content = messageToString(Message(m.begin() + 1, m.end()));
            if (patch) patch->redraw();
            return;
        }
        trigger(m[0].is("bang") ? Message{} : m);
    }
    void trigger(const Message& in) {
        flashUntil = now() + 0.15;
        patch->redraw();
        Message cur;
        std::string target;
        bool wantTarget = false;
        auto flush = [&] {
            if (cur.empty()) return;
            if (target.empty()) outlet(0, cur);
            else patch->sendTo(target, cur);
            cur.clear();
        };
        for (auto& tok : splitTokens(content)) {
            if (tok == ";") { flush(); target.clear(); wantTarget = true; continue; }
            if (tok == ",") { flush(); continue; }
            Atom a = parseAtom(tok);
            if (tok.size() > 1 && tok[0] == '$' && std::isdigit((unsigned char)tok[1])) {
                size_t k = (size_t)std::atoi(tok.c_str() + 1);
                a = (k >= 1 && k <= in.size()) ? in[k - 1] : Atom(0.f);
            }
            if (wantTarget) { target = a.str(); wantTarget = false; continue; }
            cur.push_back(a);
        }
        flush();
    }
    std::string content;
};


class ReceiveBox : public Box {
public:
    explicit ReceiveBox(const Message& a) {
        setPorts({}, {P::Control});
        if (!a.empty()) name = a[0].str();
    }
    void receive(const Message& m) { outlet(0, m); }
    std::string name;
};

// ---------------------------------------------------------------- handing things to the audio thread
// Published<T>: state a box prepares on the GUI thread (in resolve(), or for
// a message) for its dsp(): delay lines sized for the rate, head responses,
// prepared impulse responses. The audio thread reads get() (each callback:
// never keep the pointer longer); set() swaps in the new state and retires
// the old one, which is freed once the audio thread has let go.
template <class T>
class Published {
public:
    T* get() const { return p_.load(std::memory_order_acquire); }
    void set(Patch* p, std::unique_ptr<T> v) {           // GUI thread
        std::shared_ptr<T> old = std::move(own_);
        own_ = std::shared_ptr<T>(std::move(v));
        p_.store(own_.get(), std::memory_order_release);
        if (old && p) p->retire(std::move(old));          // without a patch nothing runs: freed now
    }

private:
    std::atomic<T*> p_{nullptr};
    std::shared_ptr<T> own_;
};

// Mailbox<T>: objects handed to the audio thread one after the other (a
// click~ shape, a line~ list). send() on the GUI thread; take() on the audio
// thread makes the next one current and hands the one it replaces back,
// to be freed by the next send() (or by the destructor, when the box goes).
template <class T, size_t N = 32>
class Mailbox {
public:
    Mailbox() = default;
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;
    ~Mailbox() {
        T* p;
        while (in_.pop(p)) delete p;
        collect();
        delete cur_;
    }
    bool send(std::unique_ptr<T> v) {                     // GUI thread; false when full (v dropped)
        collect();
        T* p = v.release();
        if (in_.push(p)) return true;
        delete p;
        return false;
    }
    void collect() {                                      // GUI thread: frees what came back
        T* p;
        while (back_.pop(p)) delete p;
    }
    T* take() {                                           // audio thread: the next one, or nullptr
        T* p;
        if (!in_.pop(p)) return nullptr;
        if (cur_) back_.push(cur_);                       // never full: send() empties it first
        cur_ = p;
        return p;
    }
    T* current() const { return cur_; }                   // audio thread

private:
    dsp::Spsc<T*, N> in_, back_;
    T* cur_ = nullptr;
};

// Runs a block in pieces cut at the box's own timed events (not the values of
// its inlets): piece(from, to) for the samples before each event, then
// event(e); then the rest. Audio thread, inside dsp().
template <class Piece, class Ev>
void eventPieces(Box& b, int n, Piece piece, Ev event) {
    int from = 0;
    for (int k = 0, ne = b.events(); k < ne; ++k) {
        const Event& e = b.event(k);
        if (e.kind == kEvScalar) continue;
        int at = std::min(b.eventOffset(k), n);
        if (at > from) {
            piece(from, at);
            from = at;
        }
        event(e);
    }
    if (from < n) piece(from, n);
}

// ---------------------------------------------------------------- samples (shared with index~/poke~)
struct SampleBuffer {
    std::vector<float> data;   // interleaved
    int channels = 1;
    long frames = 0;
    double sr = 44100.0;

    float at(long frame, int ch) const {
        if (frame < 0 || frame >= frames) return 0.f;
        return data[(size_t)frame * channels + (ch % channels)];
    }
    float interp(double pos, int ch) const {
        if (!(pos >= -1.0 && pos < (double)frames)) return 0.f;      // also NaN
        long i = (long)std::floor(pos);
        float f = (float)(pos - i);
        float a = at(i, ch), b = at(i + 1, ch);
        return a + f * (b - a);
    }
};

bool decodeAudioFile(const std::string& path, SampleBuffer& out, std::string& err);
bool writeWavFile(const std::string& path, const SampleBuffer& b, std::string& err);

// ---------------------------------------------------------------- buffer~
// buffer~ name [file] [ms] [channels]: named sample memory shared with
// play~, groove~ and record~. Messages: read file, write file, size ms, clear.
//
// Its storage, buf, is the GUI's to replace and the audio thread's to use:
// the audio thread reaches it only through a BufferRef while its callback
// holds the patch's mutex; the GUI replaces it with replace(), which swaps
// it under that mutex (O(1)) and frees the old after. Readers on the GUI
// thread read buf directly. (record~ and poke~ write samples into it while
// the GUI may read them: values, never the shape.)
class BufferBox : public Box {
public:
    explicit BufferBox(const Message& a) {
        setPorts({P::Control}, {});
        name = argstr(a, 0);
        size_t k = 1;
        if (k < a.size() && a[k].isSymbol()) file_ = a[k++].s;
        ms_ = argf(a, k, 0.f);
        buf.channels = std::clamp((int)argf(a, k + 1, 1.f), 1, 8);
    }
    void attached() override {
        if (!file_.empty()) { read(file_); return; }
        SampleBuffer b;                                  // an empty buffer runs at the audio rate
        b.channels = buf.channels;
        b.sr = patch->root()->sampleRate.load();
        b.frames = std::max(0L, (long)(ms_ / 1000.0 * b.sr));
        b.data.assign((size_t)b.frames * b.channels, 0.f);
        replace(std::move(b));                           // data and frames together, under the lock
    }
    void message(int, const Message& m) override {
        std::string err;
        if (m[0].is("read") && m.size() > 1) {
            read(m[1].str());
        } else if (m[0].is("write") && m.size() > 1) {
            if (!writeWavFile(patch->resolvePath(m[1].str()), buf, err)) patch->print(err);
        } else if (m[0].is("size") && m.size() > 1 && m[1].isFloat()) {
            SampleBuffer b;
            b.channels = buf.channels;
            b.sr = buf.sr;
            b.frames = std::max(0L, (long)(m[1].f / 1000.0 * buf.sr));
            b.data.assign((size_t)b.frames * b.channels, 0.f);
            std::copy_n(buf.data.begin(), std::min(buf.data.size(), b.data.size()), b.data.begin());
            replace(std::move(b));
        } else if (m[0].is("clear")) {
            SampleBuffer b;
            b.channels = buf.channels;
            b.sr = buf.sr;
            b.frames = buf.frames;
            b.data.assign(buf.data.size(), 0.f);
            replace(std::move(b));
        }
    }
    std::vector<std::string> menuItems() override { return {"Load file...", "Save as WAV...", "Clear"}; }
    void menuAction(int k) override {
        if (k == 0) {
            std::string f = chooseFile(false, "Load into buffer~ " + name, "Audio\t*.{wav,aif,aiff,flac,mp3}");
            if (!f.empty()) read(f);
        } else if (k == 1) {
            std::string f = chooseFile(true, "Save buffer~ " + name, "WAV\t*.wav", name + ".wav");
            if (!f.empty()) message(0, {Atom("write"), Atom(f)});
        } else if (k == 2) {
            message(0, {Atom("clear")});
        }
    }
    // GUI thread: new contents, swapped in under the lock (the old ones are
    // freed after it); the boxes that prepare from it (conv~) look again
    void replace(SampleBuffer&& b) {
        {
            std::lock_guard<std::mutex> lk(patch->mutex());
            std::swap(buf, b);
            ++version;
        }
        b = SampleBuffer();                              // the old storage, freed outside the lock
        patch->root()->forEachBox([](Box* x) { x->buffersChanged(); });
    }
    std::string name;
    SampleBuffer buf;        // see above
    int version = 0;         // changes with the contents (read, size, clear): conv~ reloads

private:
    void read(const std::string& file) {
        SampleBuffer b;
        std::string err;
        if (!decodeAudioFile(patch->resolvePath(file), b, err)) { patch->print("buffer~ " + name + ": " + err); return; }
        replace(std::move(b));
    }
    std::string file_;
    float ms_ = 0.f;
};

BufferBox* findBuffer(Patch* p, const std::string& name);   // searches the whole patch tree

bool audioBuffersAvailable();   // audio thread: this callback holds the buffer~ lock

// BufferRef: a box's reference to a buffer~ by name (play~, record~,
// index~, conv~ ...). resolve() looks the name up (GUI thread: at compile,
// or at once for `set name`, without compiling anything); get() is the
// storage for the audio thread, or nullptr (no such buffer~, or the lock
// was busy for this callback). The version tells the GUI side when the
// contents were replaced.
class BufferRef {
public:
    explicit BufferRef(std::string name = "") : name_(std::move(name)) {}
    const std::string& name() const { return name_; }
    void resolve(Patch* p) { box_.store(p && !name_.empty() ? findBuffer(p, name_) : nullptr, std::memory_order_release); }
    void set(Patch* p, const std::string& name) {        // another buffer~, now
        name_ = name;
        resolve(p);
    }
    BufferBox* box() const { return box_.load(std::memory_order_acquire); }   // GUI side
    SampleBuffer* get() const {                          // audio thread
        BufferBox* b = box();
        return b && audioBuffersAvailable() ? &b->buf : nullptr;
    }
    // GUI side: the buffer~ or its contents changed since the last call
    bool changed() {
        BufferBox* b = box();
        int v = b ? b->version : -1;
        if (b == seenBox_ && v == seenVersion_) return false;
        seenBox_ = b;
        seenVersion_ = v;
        return true;
    }

private:
    std::string name_;
    std::atomic<BufferBox*> box_{nullptr};
    BufferBox* seenBox_ = nullptr;
    int seenVersion_ = -2;
};

// Classes whose box text is one string (message, comment, expr, expr~): each
// object file registers its own; Patch::build joins their arguments.
std::set<std::string>& wholeTextClasses();
bool takesWholeText(const std::string& cls);

// Musil's spherical harmonics (sh_gains) into g, without allocating: for the audio thread
// (objects_spatial.cpp; the engine tests check it against Musil's)
void shGainsLive(int order, double az, double el, float* g);

// "cycle~ · inlet 1 (signal)\nfrequency in Hz ...": help for a port (ports.cpp)
std::string portHelp(const Box* b, bool outlet, int index);

}  // namespace moritz
