// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// midi.cpp — MidiEngine: RtMidi ports, the input queue, hot-plugging

#include "midi.h"

#include "RtMidi.h"

#include <algorithm>

namespace moritz {

namespace {
// Our own virtual output shows up as an input too; never listen to it.
bool isOurs(const std::string& name) { return name.find("Moritz") != std::string::npos; }

// RtMidi reports problems through this callback instead of throwing or
// printing, so a missing MIDI system never stops the program.
void quiet(RtMidiError::Type type, const std::string& text, void* engine) {
    if (type != RtMidiError::WARNING && type != RtMidiError::DEBUG_WARNING)
        static_cast<MidiEngine*>(engine)->error = text;
}
}  // namespace

MidiEngine::MidiEngine() {
    try {
        probe_ = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "Moritz");
        probe_->setErrorCallback(quiet, this);
    } catch (const RtMidiError& e) {
        error = e.getMessage();
    }
    setOutput("");
}

MidiEngine::~MidiEngine() {
    ins_.clear();          // closes the ports: no callbacks after this
    out_.reset();
}

void MidiEngine::received(double, std::vector<unsigned char>* msg, void* self) {
    auto* e = static_cast<MidiEngine*>(self);
    if (!msg || msg->empty()) return;
    {
        std::lock_guard<std::mutex> lk(e->queueMutex_);
        if (e->queue_.size() < 4096) e->queue_.push_back(*msg);
    }
    if (e->wake) e->wake();
}

void MidiEngine::drain() {
    std::deque<std::vector<unsigned char>> q;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        q.swap(queue_);
    }
    for (auto& m : q)
        if (onMessage) onMessage(m);
}

std::vector<std::string> MidiEngine::inputPorts() {
    std::vector<std::string> v;
    if (!probe_) return v;
    try {
        unsigned n = probe_->getPortCount();
        for (unsigned i = 0; i < n; ++i) {
            std::string name = probe_->getPortName(i);
            if (!name.empty() && !isOurs(name)) v.push_back(name);
        }
    } catch (const RtMidiError& e) {
        error = e.getMessage();
    }
    return v;
}

std::vector<std::string> MidiEngine::outputPorts() {
    std::vector<std::string> v;
    try {
        RtMidiOut probe(RtMidi::UNSPECIFIED, "Moritz");
        probe.setErrorCallback(quiet, this);
        unsigned n = probe.getPortCount();
        for (unsigned i = 0; i < n; ++i) {
            std::string name = probe.getPortName(i);
            if (!name.empty() && !isOurs(name)) v.push_back(name);
        }
    } catch (const RtMidiError& e) {
        error = e.getMessage();
    }
    return v;
}

void MidiEngine::refresh() {
    std::vector<std::string> names = inputPorts(), wanted;
    for (auto& n : names)
        if (input_.empty() || n == input_) wanted.push_back(n);
    // close what is gone (or no longer chosen)
    ins_.erase(std::remove_if(ins_.begin(), ins_.end(),
                              [&](const In& in) {
                                  return std::find(wanted.begin(), wanted.end(), in.name) == wanted.end();
                              }),
               ins_.end());
    // open what is new
    for (auto& name : wanted) {
        bool open = false;
        for (auto& in : ins_) open = open || in.name == name;
        if (open) continue;
        try {
            auto port = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "Moritz");
            port->setErrorCallback(quiet, this);
            unsigned n = port->getPortCount();
            for (unsigned i = 0; i < n; ++i)
                if (port->getPortName(i) == name) {
                    port->openPort(i, "Moritz in");
                    port->ignoreTypes(true, true, true);   // no sysex, clock or active sensing
                    port->setCallback(&MidiEngine::received, this);
                    ins_.push_back({name, std::move(port)});
                    break;
                }
        } catch (const RtMidiError& e) {
            error = e.getMessage();
        }
    }
}

void MidiEngine::setInput(const std::string& name) {
    input_ = name;
    refresh();
}

void MidiEngine::setOutput(const std::string& name) {
    output_ = name;
    outName_.clear();
    out_.reset();
    try {
        out_ = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, "Moritz");
        out_->setErrorCallback(quiet, this);
        if (name.empty()) {
#ifndef _WIN32
            out_->openVirtualPort("Moritz");      // other programs can listen to "Moritz"
            outName_ = "virtual port Moritz";
#endif
        } else {
            unsigned n = out_->getPortCount();
            for (unsigned i = 0; i < n; ++i)
                if (out_->getPortName(i) == name) {
                    out_->openPort(i, "Moritz out");
                    outName_ = name;
                    break;
                }
        }
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        out_.reset();
    }
    if (outName_.empty()) out_.reset();
}

void MidiEngine::send(const std::vector<unsigned char>& msg) {
    if (!out_ || msg.empty()) return;
    try {
        out_->sendMessage(&msg);
    } catch (const RtMidiError& e) {
        error = e.getMessage();
    }
}

std::string MidiEngine::describe() const {
    std::string s = "MIDI in: ";
    if (ins_.empty()) s += "none";
    for (size_t k = 0; k < ins_.size(); ++k) s += (k ? ", " : "") + ins_[k].name;
    s += "; out: " + (outName_.empty() ? std::string("none") : outName_);
    return s;
}

}  // namespace moritz
