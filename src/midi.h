// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// midi.h — MIDI input and output through RtMidi (CoreMIDI on macOS, ALSA on
// Linux, the Windows multimedia API on Windows)
//
// RtMidi calls back on its own thread. Incoming messages are queued there
// and delivered on the GUI thread by drain(), so the patch only ever sees
// them on the GUI thread, like every other message.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class RtMidiIn;
class RtMidiOut;

namespace moritz {

// The last problem, for the console: RtMidi reports from its own thread, the
// GUI reads and clears it; a mutex keeps the two apart.
class MidiError {
public:
    void set(const std::string& s) {
        std::lock_guard<std::mutex> lk(m_);
        text_ = s;
    }
    bool empty() const {
        std::lock_guard<std::mutex> lk(m_);
        return text_.empty();
    }
    void clear() { set(""); }
    std::string str() const {
        std::lock_guard<std::mutex> lk(m_);
        return text_;
    }
    operator std::string() const { return str(); }
    friend std::string operator+(const char* a, const MidiError& e) { return a + e.str(); }
    friend std::string operator+(const std::string& a, const MidiError& e) { return a + e.str(); }

private:
    mutable std::mutex m_;
    std::string text_;
};

class MidiEngine {
public:
    MidiEngine();
    ~MidiEngine();

    // MIDI thread: a message arrived (wake the GUI thread, which calls drain())
    std::function<void()> wake;
    // GUI thread: each queued message, from drain()
    std::function<void(const std::vector<unsigned char>&)> onMessage;
    void drain();

    // GUI thread. refresh() opens inputs that appeared and closes the ones
    // that went away; call it now and then (devices come and go).
    void refresh();
    std::vector<std::string> inputPorts();
    std::vector<std::string> outputPorts();
    void setInput(const std::string& name);    // "" = all inputs
    void setOutput(const std::string& name);   // "" = the virtual port "Moritz" (macOS, Linux)
    const std::string& input() const { return input_; }
    const std::string& output() const { return output_; }
    void send(const std::vector<unsigned char>& msg);
    std::string describe() const;              // for the console: what is open
    MidiError error;                           // the last problem, for the console

private:
    struct In {
        std::string name;
        std::unique_ptr<RtMidiIn> port;
    };
    static void received(double stamp, std::vector<unsigned char>* msg, void* self);
    std::unique_ptr<RtMidiIn> probe_;          // lists the input ports
    std::vector<In> ins_;
    std::unique_ptr<RtMidiOut> out_;
    std::string input_, output_, outName_;
    std::mutex queueMutex_;
    std::deque<std::vector<unsigned char>> queue_;
};

}  // namespace moritz
