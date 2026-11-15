// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_midi.cpp — MIDI objects, after Max's: notein, ctlin, bendin, pgmin,
// noteout, ctlout, makenote, stripnote
//
// Channels are 1..16; an object without a channel argument listens to all
// channels and has an extra outlet for the channel. Outputs go right to
// left, so a pitch arrives last, after its velocity. Input comes from
// Patch::midiIn (the MIDI devices, see midi.h), output goes to
// Patch::midiOut.

#include "objects.h"

#include <map>

namespace moritz {
namespace {

// status byte kind (0x80, 0x90, ...) and channel (1..16) of a message
int kindOf(const std::vector<unsigned char>& m) { return m.empty() ? 0 : m[0] & 0xF0; }
int channelOf(const std::vector<unsigned char>& m) { return m.empty() ? 0 : (m[0] & 0x0F) + 1; }
int channelArg(const Message& a, size_t i) { return std::clamp((int)argf(a, i, 0.f), 0, 16); }   // 0 = all

// ---------------------------------------------------------------- notein
// notein [channel]: outlets pitch, velocity (0 = note-off) and, without a
// channel argument, the channel.
class NoteinBox : public Box {
public:
    explicit NoteinBox(const Message& a) : ch_(channelArg(a, 0)) {
        setPorts({}, std::vector<Port>(ch_ ? 2 : 3, P::Control));
    }
    void midiIn(const std::vector<unsigned char>& m) override {
        int k = kindOf(m);
        if ((k != 0x90 && k != 0x80) || m.size() < 3 || (ch_ && channelOf(m) != ch_)) return;
        int vel = k == 0x80 ? 0 : m[2];
        if (!ch_) outletFloat(2, (float)channelOf(m));
        outletFloat(1, (float)vel);
        outletFloat(0, (float)m[1]);
    }

private:
    int ch_;
};

// ---------------------------------------------------------------- ctlin
// ctlin [controller] [channel]: outlets value, then the controller number
// (without a controller argument) and the channel (without a channel
// argument).
class CtlinBox : public Box {
public:
    explicit CtlinBox(const Message& a) : cc_((int)argf(a, 0, -1.f)), ch_(channelArg(a, 1)) {
        if (cc_ > 127) cc_ = -1;
        setPorts({}, std::vector<Port>((size_t)(1 + (cc_ < 0) + (ch_ == 0)), P::Control));
    }
    void midiIn(const std::vector<unsigned char>& m) override {
        if (kindOf(m) != 0xB0 || m.size() < 3) return;
        if ((cc_ >= 0 && m[1] != cc_) || (ch_ && channelOf(m) != ch_)) return;
        int o = numOuts() - 1;
        if (!ch_) outletFloat(o--, (float)channelOf(m));
        if (cc_ < 0) outletFloat(o--, (float)m[1]);
        outletFloat(0, (float)m[2]);
    }

private:
    int cc_, ch_;
};

// ---------------------------------------------------------------- bendin / pgmin
// bendin [channel]: pitch bend as 0..127 (64 = centre), as Max's bendin.
// pgmin [channel]: program changes as 1..128.
class ChannelValueBox : public Box {
public:
    ChannelValueBox(int kind, const Message& a) : kind_(kind), ch_(channelArg(a, 0)) {
        setPorts({}, std::vector<Port>(ch_ ? 1 : 2, P::Control));
    }
    void midiIn(const std::vector<unsigned char>& m) override {
        if (kindOf(m) != kind_ || m.size() < 2 || (ch_ && channelOf(m) != ch_)) return;
        float v = kind_ == 0xE0 ? (float)(m.size() > 2 ? m[2] : 0) : (float)(m[1] + 1);
        if (!ch_) outletFloat(1, (float)channelOf(m));
        outletFloat(0, v);
    }

private:
    int kind_, ch_;
};

// ---------------------------------------------------------------- noteout / ctlout
// noteout [channel]: inlets pitch (sends), velocity, channel; "pitch
// velocity" in the left inlet sends too. Velocity 0 is a note-off.
class NoteoutBox : public Box {
public:
    explicit NoteoutBox(const Message& a) : ch_(std::max(1, channelArg(a, 0))) {
        setPorts({P::Control, P::Control, P::Control}, {});
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 1) { vel_ = std::clamp((int)m[0].f, 0, 127); return; }
        if (in == 2) { ch_ = std::clamp((int)m[0].f, 1, 16); return; }
        if (m.size() > 1 && m[1].isFloat()) vel_ = std::clamp((int)m[1].f, 0, 127);
        int p = std::clamp((int)m[0].f, 0, 127);
        patch->midiOut({(unsigned char)(0x90 | (ch_ - 1)), (unsigned char)p, (unsigned char)vel_});
    }

private:
    int ch_, vel_ = 0;
};

// ctlout [controller] [channel]: inlets value (sends), controller, channel.
class CtloutBox : public Box {
public:
    explicit CtloutBox(const Message& a)
        : cc_(std::clamp((int)argf(a, 0, 7.f), 0, 127)), ch_(std::max(1, channelArg(a, 1))) {
        setPorts({P::Control, P::Control, P::Control}, {});
    }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 1) { cc_ = std::clamp((int)m[0].f, 0, 127); return; }
        if (in == 2) { ch_ = std::clamp((int)m[0].f, 1, 16); return; }
        int v = std::clamp((int)m[0].f, 0, 127);
        patch->midiOut({(unsigned char)(0xB0 | (ch_ - 1)), (unsigned char)cc_, (unsigned char)v});
    }

private:
    int cc_, ch_;
};

// ---------------------------------------------------------------- makenote
// makenote [velocity] [duration ms]: a pitch outputs that pitch with the
// velocity at once, and the same pitch with velocity 0 after the duration.
// "pitch velocity" sets the velocity too. "stop" sends the pending note-offs
// now; "clear" forgets them.
class MakenoteBox : public Box {
public:
    explicit MakenoteBox(const Message& a)
        : vel_(std::clamp((int)argf(a, 0, 100.f), 0, 127)), dur_(std::max(0.f, argf(a, 1, 250.f))) {
        setPorts({P::Control, P::Control, P::Control}, {P::Control, P::Control});
    }
    void message(int in, const Message& m) override {
        if (m[0].is("stop")) {
            patch->cancel(this);
            auto held = pending_;
            pending_.clear();
            for (auto& [p, n] : held)
                for (int k = 0; k < n; ++k) send(p, 0);
            return;
        }
        if (m[0].is("clear")) { patch->cancel(this); pending_.clear(); return; }
        if (!m[0].isFloat()) return;
        if (in == 1) { vel_ = std::clamp((int)m[0].f, 0, 127); return; }
        if (in == 2) { dur_ = std::max(0.f, m[0].f); return; }
        if (m.size() > 1 && m[1].isFloat()) vel_ = std::clamp((int)m[1].f, 0, 127);
        int p = (int)m[0].f;
        send(p, vel_);
        if (vel_ == 0) return;
        pending_[p]++;
        patch->schedule(this, dur_ / 1000.0, [this, p] {
            if (auto it = pending_.find(p); it != pending_.end() && it->second > 0) {
                if (--it->second == 0) pending_.erase(it);
                send(p, 0);
            }
        });
    }

private:
    void send(int p, int v) {
        outletFloat(1, (float)v);
        outletFloat(0, (float)p);
    }
    int vel_;
    float dur_;
    std::map<int, int> pending_;   // pitch -> note-offs still to come
};

// ---------------------------------------------------------------- stripnote
// stripnote: passes pitch and velocity only for note-ons (velocity > 0).
// Inlets pitch (hot) and velocity; "pitch velocity" in the left works too.
class StripnoteBox : public Box {
public:
    StripnoteBox() { setPorts({P::Control, P::Control}, {P::Control, P::Control}); }
    void message(int in, const Message& m) override {
        if (!m[0].isFloat()) return;
        if (in == 1) { vel_ = m[0].f; return; }
        if (m.size() > 1 && m[1].isFloat()) vel_ = m[1].f;
        if (vel_ <= 0.f) return;
        outletFloat(1, vel_);
        outletFloat(0, m[0].f);
    }

private:
    float vel_ = 0.f;
};

}  // namespace

void registerMidiObjects(ClassTable& t) {
    using M = const Message&;
    t.insert(t.end(), {
        {"notein", "MIDI", "MIDI notes in: [channel]", with<NoteinBox>()},
        {"ctlin", "MIDI", "MIDI controllers in: [controller] [channel]", with<CtlinBox>()},
        {"bendin", "MIDI", "MIDI pitch bend in (0-127): [channel]", [](M a, Patch*) { return std::make_unique<ChannelValueBox>(0xE0, a); }},
        {"pgmin", "MIDI", "MIDI program changes in (1-128): [channel]", [](M a, Patch*) { return std::make_unique<ChannelValueBox>(0xC0, a); }},
        {"noteout", "MIDI", "MIDI notes out: [channel]", with<NoteoutBox>()},
        {"ctlout", "MIDI", "MIDI controllers out: [controller] [channel]", with<CtloutBox>()},
        {"makenote", "MIDI", "note-on now, note-off later: [velocity] [duration ms]", with<MakenoteBox>()},
        {"stripnote", "MIDI", "drop note-offs", plain<StripnoteBox>()},
    });
}

}  // namespace moritz
