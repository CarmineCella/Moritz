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
        buf.sr = patch->root()->sampleRate.load();     // an empty buffer runs at the audio rate
        buf.frames = (long)(ms_ / 1000.0 * buf.sr);
        buf.data.assign((size_t)buf.frames * buf.channels, 0.f);
    }
    void message(int, const Message& m) override {
        std::string err;
        if (m[0].is("read") && m.size() > 1) {
            read(m[1].str());
        } else if (m[0].is("write") && m.size() > 1) {
            SampleBuffer copy;
            { std::lock_guard<std::mutex> lk(patch->mutex()); copy = buf; }
            if (!writeWavFile(patch->resolvePath(m[1].str()), copy, err)) patch->print(err);
        } else if (m[0].is("size") && m.size() > 1 && m[1].isFloat()) {
            {
                std::lock_guard<std::mutex> lk(patch->mutex());
                buf.frames = std::max(0L, (long)(m[1].f / 1000.0 * buf.sr));
                buf.data.resize((size_t)buf.frames * buf.channels, 0.f);
                ++version;
            }
            patch->rebuild();
        } else if (m[0].is("clear")) {
            {
                std::lock_guard<std::mutex> lk(patch->mutex());
                std::fill(buf.data.begin(), buf.data.end(), 0.f);
                ++version;
            }
            patch->rebuild();
        }
    }
    std::string name;
    SampleBuffer buf;        // touched by the audio thread only under the DSP lock
    int version = 0;         // changes with the contents (read, size, clear): conv~ reloads

private:
    void read(const std::string& file) {
        SampleBuffer b;
        std::string err;
        if (!decodeAudioFile(patch->resolvePath(file), b, err)) { patch->print("buffer~ " + name + ": " + err); return; }
        {
            std::lock_guard<std::mutex> lk(patch->mutex());
            buf = std::move(b);
            ++version;
        }
        patch->rebuild();        // objects that prepare the data (conv~) reload it
    }
    std::string file_;
    float ms_ = 0.f;
};

BufferBox* findBuffer(Patch* p, const std::string& name);   // searches the whole patch tree

// "cycle~ · inlet 1 (signal)\nfrequency in Hz ...": help for a port (ports.cpp)
std::string portHelp(const Box* b, bool outlet, int index);

}  // namespace moritz
