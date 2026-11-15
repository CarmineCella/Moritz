// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// ports.cpp — the one-line help shown when the mouse rests on an inlet or
// outlet in edit mode.

#include "objects.h"

#include <map>

namespace moritz {

namespace {

struct PortDocs {
    std::vector<const char*> ins, outs;   // the last entry repeats for extra ports
};

const std::map<std::string, PortDocs>& docs() {
    static const std::map<std::string, PortDocs> d = {
        // oscillators and generators
        {"cycle~", {{"frequency in Hz (signal or float)", "phase 0-1: restarts the cycle there"},
                    {"cosine wave, -1 to 1"}}},
        {"phasor~", {{"frequency in Hz (signal or float)", "phase 0-1: restarts the ramp there"},
                     {"ramp from 0 to 1"}}},
        {"saw~", {{"frequency in Hz (signal or float)"}, {"band-limited sawtooth"}}},
        {"rect~", {{"frequency in Hz (signal or float)", "duty cycle 0-1 (pulse width)"},
                   {"band-limited pulse"}}},
        {"tri~", {{"frequency in Hz (signal or float)", "slope 0-1 (0.5 = symmetric)"},
                  {"triangle wave"}}},
        {"noise~", {{"no messages"}, {"white noise, -1 to 1"}}},
        {"click~", {{"bang: one impulse; a list sets the impulse shape"}, {"impulse"}}},
        {"sig~", {{"number to turn into a signal"}, {"constant signal"}}},
        {"line~", {{"ramps: \"1 10 0 300\" goes to 1 in 10 ms, then to 0 in 300 ms; a float jumps",
                    "ramp time (ms) for the next single float"},
                   {"ramp signal"}}},
        // filters
        {"reson~", {{"signal to filter", "gain", "centre frequency (Hz)", "Q: higher is narrower"},
                    {"band-passed signal"}}},
        {"lores~", {{"signal to filter", "cutoff frequency (Hz)", "resonance 0-1"},
                    {"low-passed signal"}}},
        {"svf~", {{"signal to filter", "frequency (Hz)", "Q 0-1"},
                  {"low-pass", "high-pass", "band-pass", "notch"}}},
        {"biquad~", {{"signal to filter", "a0", "a1", "a2", "b1", "b2"}, {"filtered signal"}}},
        // signal math
        {"+~", {{"signal", "signal or float to add"}, {"sum"}}},
        {"-~", {{"signal", "signal or float to subtract"}, {"difference"}}},
        {"*~", {{"signal", "signal or float to multiply by"}, {"product"}}},
        {"/~", {{"signal", "signal or float to divide by"}, {"quotient (0 where dividing by 0)"}}},
        {"clip~", {{"signal", "minimum", "maximum"}, {"signal limited to min..max"}}},
        {"abs~", {{"signal"}, {"absolute value"}}},
        {"tanh~", {{"signal"}, {"soft-saturated signal"}}},
        {"gain~", {{"signal to scale; a float sets the amplitude (1 = unity)", "ramp time (ms)"},
                   {"scaled signal (changes ramp, no clicks)", "amplitude, when the fader moves"}}},
        {"expr~", {{"value of $v1 (signal or float)", "value of the next $v (signal or float)"},
                   {"result of the expression, per sample"}}},
        {"sah~", {{"signal to sample", "trigger: holds the left signal when this rises above the threshold"},
                  {"held value"}}},
        // delay
        {"tapin~", {{"signal to delay"}, {"delay line: connect to tapout~"}}},
        {"tapout~", {{"delay line from tapin~; a float sets the first tap (ms)", "delay of this tap (ms)"},
                     {"output of this tap"}}},
        {"comb~", {{"signal", "delay (ms)", "a: gain of the input", "b: gain of the delayed input",
                    "c: feedback gain"},
                   {"filtered signal"}}},
        {"allpass~", {{"signal", "delay (ms)", "gain"}, {"filtered signal"}}},
        // samples
        {"buffer~", {{"read file, write file.wav, size ms, clear"}, {}}},
        {"play~", {{"start [from [to]] in ms, stop, bang; or a signal: read position in ms"},
                   {"audio, channel by channel"}}},
        {"groove~", {{"rate (signal, 1 = normal, negative = backwards); a float jumps (ms); loop 1/0",
                      "loop start (ms)", "loop end (ms, 0 = end of the buffer)"},
                     {"audio, channel by channel"}}},
        {"record~", {{"signal to record; 1 starts, 0 stops, loop 1/0", "signal to record (channel)"},
                     {"position in the buffer, 0 to 1"}}},
        {"sfplay~", {{"open file, 1 plays, 0 stops, loop 1/0, speed x"}, {"audio, channel by channel"}}},
        {"count~", {{"bang: count from min; a number: count from there; stop"},
                    {"count, one step per sample"}}},
        {"index~", {{"sample index (signal)"}, {"buffer~ value at that index"}}},
        {"poke~", {{"value to write (signal)", "sample index to write at (signal)"}, {}}},
        // spectral
        {"fftin~", {{}, {"real part", "imaginary part", "bin number, 0 to size/2 - 1"}}},
        {"fftout~", {{"real part", "imaginary part"}, {}}},
        {"cartopol~", {{"real part", "imaginary part"}, {"amplitude", "phase, -pi to pi"}}},
        {"poltocar~", {{"amplitude", "phase"}, {"real part", "imaginary part"}}},
        {"framedelta~", {{"signal (in pfft~: one frame)"}, {"difference from the same bin of the previous frame"}}},
        {"frameaccum~", {{"value to add each frame", "value to restart from", "reset: where not 0, the bin restarts"},
                         {"running sum per bin"}}},
        {"pvlock~", {{"amplitude (peaks are found here)", "analysed phase", "phase difference (frequency)",
                      "reset: not 0 anywhere in a frame restarts the whole frame from the analysed phase"},
                     {"resynthesis phase, locked to the peaks"}}},
        {"lrshift~", {{"signal (in pfft~: one frame)"}, {"shifted by n samples (bins)"}}},
        {"phasewrap~", {{"phase"}, {"phase wrapped to -pi..pi"}}},
        // analysis
        {"snapshot~", {{"signal; bang reports its value now", "report interval (ms, 0 = only on bang)"},
                       {"signal value"}}},
        {"peakamp~", {{"signal; bang reports the peak now", "report interval (ms)"},
                      {"peak absolute value since the last report"}}},
        {"meter~", {{"signal to show"}, {}}},
        {"scope~", {{"signal to show"}, {}}},
        // I/O
        {"adc~", {{"start / stop: turn audio on or off"}, {"audio input channel"}}},
        {"dac~", {{"signal to this output channel; start / stop turn audio on or off"}, {}}},
        {"slide~", {{"signal to smooth; reset jumps to it", "slide up (samples, 1 = none)", "slide down (samples, 1 = none)"},
                    {"smoothed signal"}}},
        {"selector~", {{"which input to pass: 1..n, 0 = none (5 ms crossfade)", "signal to choose"},
                       {"the chosen signal"}}},
        {"gate~", {{"which outlet gets the signal: 1..n, 0 = closed (5 ms crossfade)", "signal to route"},
                   {"the signal, when this outlet is chosen"}}},
        {"send~", {{"signal to send to every receive~ of this name; set name changes it"}, {}}},
        {"receive~", {{"set name: listen to another send~ name"}, {"sum of the send~s of this name"}}},
        {"sfrecord~", {{"open file.wav, 1 starts, 0 stops; signal to record (channel 1)", "signal to record (next channel)"},
                       {"recorded time (ms)"}}},
        {"waveform~", {{"number: cursor (ms); select start end; set buffer~ name"},
                       {"selection start (ms)", "selection end (ms)"}}},
        {"spectroscope~", {{"signal to analyse"}, {}}},
        {"kslider", {{"pitch (or pitch velocity): lights the key and passes through; set p v; flush; clear",
                      "velocity for pitches sent to the left inlet"},
                     {"pitch", "velocity (0 = note-off)"}}},
        {"sparkle~", {{"speed (1 normal, 0 frozen, < 0 backwards), or the input without a buffer~; "
                       "start [ms], stop, loop 1/0, envelope C, threshold x, robot 1/0, whisper 1/0",
                       "pitch ratio (2 = an octave up)", "formant ratio (with envelope: moves the formants)"},
                      {"output (channel)", "read position, 0 to 1"}}},
        {"conv~", {{"signal to convolve with the buffer~ (channel); set name, gain x, normalize 0/1",
                    "signal for the next channel (unconnected: the channel before)"},
                   {"convolved signal (channel)"}}},
        {"condec", {{"conv x h out [scale mix] or deconv y h out [threshold scale]: buffer~ names"},
                    {"bang when the result is in the out buffer~"}}},
        {"function", {{"bang: the curve for line~; a number (ms): the value there; domain, range, clear, set"},
                      {"value at the time received", "the curve for line~: first value, then value-time pairs"}}},
        {"moritz~", {{"open patch, send name value, input buffer, render buffer seconds, clear, stop"},
                     {"progress, 0 to 1", "bang: the buffer~ is ready"}}},
        {"nmf", {{"run source dest, components k, iterations n, size n, seed n, stop"},
                 {"progress, 0 to 1", "bang: one part per channel of dest"}}},
        {"hpss", {{"run source harmonic percussive, kernel n, size n, stop"},
                  {"progress, 0 to 1", "bang: the two buffer~s are ready"}}},
        {"knn", {{"add x... -> y..., a list to predict; k n, weighted 0/1, classify 0/1, normalize 0/1, clear, write/read"},
                 {"prediction: the outputs (or the class)", "index of the nearest example"}}},
        {"kmeans", {{"add x..., train k [iterations], a list to classify; centroid i, seed n, normalize 0/1, clear, write/read"},
                    {"cluster of the list, 0..k-1", "that cluster's centroid"}}},
        {"pca", {{"add x..., train [dims], a list to project; explained, normalize 0/1, clear, write/read"},
                 {"coordinates on the components", "share of the variance of each component"}}},
        {"zscore", {{"add x..., train, a list to standardise; clear, write/read"}, {"(x - mean) / sd"}}},
        {"linreg", {{"add x... -> y..., train, a list to predict; weights, clear, write/read"},
                    {"predicted outputs", "weights: intercept first, output after output"}}},
        {"notein", {{}, {"pitch", "velocity (0 = note-off)", "channel (1-16)"}}},
        {"ctlin", {{}, {"controller value (0-127)", "controller number, or channel", "channel (1-16)"}}},
        {"bendin", {{}, {"pitch bend, 0-127 (64 = centre)", "channel (1-16)"}}},
        {"pgmin", {{}, {"program number, 1-128", "channel (1-16)"}}},
        {"noteout", {{"pitch: sends the note (pitch velocity works too)", "velocity (0 = note-off)", "channel (1-16)"}, {}}},
        {"ctlout", {{"value: sends it (0-127)", "controller number", "channel (1-16)"}, {}}},
        {"makenote", {{"pitch: note-on now, note-off later; stop, clear", "velocity", "duration (ms)"},
                      {"pitch", "velocity (0 for the note-off)"}}},
        {"stripnote", {{"pitch: passes when the velocity is above 0", "velocity"}, {"pitch", "velocity"}}},
        {"dspstate~", {{"bang: report now; a number: also report every ms while audio runs (0 = off)"},
                       {"1 while audio runs, 0 when off", "sample rate (Hz)", "signal vector size (samples)",
                        "I/O buffer size (samples)", "CPU load (% of the buffer's time)"}}},
        // timing
        {"metro", {{"1 or bang starts, 0 or stop stops", "interval (ms)"}, {"bang every interval"}}},
        {"delay", {{"bang starts (or restarts), stop cancels; a number sets the time and starts", "delay time (ms)"},
                   {"bang after the delay"}}},
        {"line", {{"target [time [grain]]: ramp to target", "ramp time (ms)", "grain: ms between outputs"},
                  {"ramp values"}}},
        {"loadbang", {{"bang: fire again"}, {"bang when the patch opens"}}},
        // control math
        {"mtof", {{"MIDI note"}, {"frequency in Hz"}}},
        {"ftom", {{"frequency in Hz"}, {"MIDI note"}}},
        {"dbtoa", {{"decibels"}, {"amplitude"}}},
        {"atodb", {{"amplitude"}, {"decibels"}}},
        {"scale", {{"value to map", "input low", "input high", "output low", "output high", "curve exponent"},
                   {"mapped value"}}},
        {"expr", {{"$f1 / $i1: sets it and outputs; bang outputs", "value of the next $f / $i (stored)"},
                  {"result"}}},
        {"if", {{"$i1 / $f1 (or a word for $s1): sets it and evaluates; bang evaluates; set ...: store only",
                 "value of the next $i / $f (stored)"},
                {"the message after then or else", "messages marked out2"}}},
        {"random", {{"bang: output a random integer; seed x: repeatable"}, {"random integer 0 .. range-1"}}},
        {"counter", {{"bang: output the count, then step", "next value"}, {"count"}}},
        // routing
        {"pack", {{"first item: outputs the list", "item (stored)"}, {"the list"}}},
        {"unpack", {{"list to split"}, {"list item"}}},
        {"gate", {{"which outlet is open (0 closes all)", "data to let through"}, {"output while open"}}},
        {"int", {{"number: store and output; bang: output", "number: store only"}, {"the stored integer"}}},
        {"float", {{"number: store and output; bang: output", "number: store only"}, {"the stored float"}}},
        {"send", {{"message to every receive with this name"}, {}}},
        {"receive", {{}, {"messages sent to this name"}}},
        {"print", {{"anything: shown in the console (Patch > Console)"}, {}}},
        // patching
        {"inlet", {{}, {"what arrives at this inlet of the subpatcher"}}},
        {"inlet~", {{}, {"signal arriving at this inlet of the subpatcher"}}},
        {"outlet", {{"leaves the subpatcher through this outlet"}, {}}},
        {"outlet~", {{"signal leaving the subpatcher through this outlet"}, {}}},
        {"thispoly~", {{"voice envelope: busy while it is not 0; a number sets busy; mute 1/0"},
                       {"voice number (from 1)"}}},
        // interface
        {"message", {{"bang sends it; $1..$9 take the incoming values; set ... changes it"}, {"the message"}}},
        {"flonum", {{"number: show and output; set x: show only"}, {"the number"}}},
        {"number", {{"number: show and output; set x: show only"}, {"the number"}}},
        {"button", {{"anything: flash and bang"}, {"bang"}}},
        {"toggle", {{"number: 1/0 and output; bang: flip"}, {"1 or 0"}}},
        {"slider", {{"number: set and output; set x: set only"}, {"value"}}},
    };
    return d;
}

const char* pick(const std::vector<const char*>& v, int i) {
    if (v.empty()) return nullptr;
    return v[(size_t)std::min(i, (int)v.size() - 1)];
}

std::string trigType(char c) {
    switch (c) {
        case 'b': return "bang";
        case 'f': return "the input as a float";
        case 'i': return "the input as an integer";
        case 'l': return "the whole list";
        case 's': return "the first word";
        default: return "the input as it is";
    }
}

}  // namespace

std::string portHelp(const Box* b, bool outlet, int i) {
    const std::string& c = b->cls;
    int nIns = b->numIns(), nOuts = b->numOuts();
    std::string kind = (outlet ? b->outs[i] : b->ins[i]) == Port::Control ? "control" : "signal";
    auto head = [&] {
        return c + " · " + (outlet ? "outlet " : "inlet ") + std::to_string(i + 1) + " (" + kind + ")";
    };
    std::string text;

    // objects whose ports depend on their arguments
    if (c == "trigger" || c == "t") {
        if (!outlet) text = "anything: sent out of the outlets from right to left";
        else {
            char k = i < (int)b->args.size() ? b->args[(size_t)i].str()[0] : 'a';
            text = trigType(k) + " (outlets fire right to left)";
        }
    } else if (c == "route" || c == "select" || c == "sel") {
        bool route = c == "route";
        if (!outlet) {
            text = route ? "message: its first word picks the outlet"
                         : (i == 0 ? "value to match" : "new value to match (with one key)");
        } else if (i == nOuts - 1) {
            text = route ? "messages that matched nothing" : "input that matched nothing";
        } else {
            std::string key = i < (int)b->args.size() ? b->args[(size_t)i].str() : "?";
            text = route ? "messages starting with \"" + key + "\", without that word"
                         : "bang when the input is " + key;
        }
    } else if (c == "descriptors~") {
        static const std::vector<std::pair<std::string, std::string>> d = {
            {"rms", "rms: root mean square level"},
            {"centroid", "centroid (Hz): amplitude-weighted mean frequency"},
            {"spread", "spread (Hz): spectral standard deviation around the centroid"},
            {"skewness", "skewness: asymmetry of the spectrum"},
            {"kurtosis", "kurtosis: peakedness of the spectrum"},
            {"flux", "flux: how much the spectrum grew since the last frame"},
            {"hfc", "hfc: high frequency content"},
            {"zcr", "zcr: zero crossings per second"},
            {"flatness", "flatness: 0 tonal .. 1 noisy"},
            {"periodicity", "periodicity: autocorrelation peak, 0..1"},
            {"f0", "f0 (Hz) from the autocorrelation; 0 when unvoiced"}};
        std::vector<std::string> chosen;
        for (auto& x : b->args)
            if (x.isSymbol())
                for (auto& e : d)
                    if (e.first == x.s) chosen.push_back(e.second);
        if (chosen.empty())
            for (auto& e : d) chosen.push_back(e.second);
        if (!outlet) text = "signal to analyse; bang: analyse now; hop n";
        else if (i < (int)chosen.size()) text = chosen[(size_t)i];
    } else if (c == "groove~" && outlet && i == nOuts - 1) {
        text = "sync: position in the loop, 0 to 1";
    } else if (b->inner() && c != "p" && c != "pfft~" && c != "poly~") {
        text = std::string(outlet ? "outlet " : "inlet ") + "of the abstraction " + c + ".moritz";
    } else if (c == "p") {
        text = outlet ? "comes from the subpatcher's outlet boxes (left to right)"
                      : "goes to the subpatcher's inlet boxes (left to right)";
    } else if (c == "poly~") {
        if (outlet) text = kind == "signal" ? "sum of all voices" : "from the voices' outlet";
        else text = kind == "signal" ? "signal to every voice"
                                     : "note ...: to a free voice; midinote p v: note-offs reach their voice; "
                                       "target n; mute n 1/0; other messages to the target voice (all when 0)";
    } else if (c == "pfft~") {
        if (outlet) text = kind == "signal" ? "resynthesised from fftout~ " + std::to_string(i + 1)
                                            : "from an outlet inside";
        else text = kind == "signal" ? "analysed by fftin~ " + std::to_string(i + 1) : "to an inlet inside";
    } else if (c == "dac~" || c == "adc~") {
        int ch = i < (int)b->args.size() && b->args[(size_t)i].isFloat() ? (int)b->args[(size_t)i].f : i + 1;
        text = c == "dac~" ? "signal to output channel " + std::to_string(ch) + "; start / stop"
                           : (outlet ? "audio input channel " + std::to_string(ch) : "start / stop");
    } else if (const auto it = docs().find(c); it != docs().end()) {
        const char* s = pick(outlet ? it->second.outs : it->second.ins, i);
        if (s) text = s;
    } else if (c.size() <= 2 && (c == "+" || c == "-" || c == "*" || c == "/" || c == "%" ||
                                 c == "==" || c == "!=" || c == ">" || c == "<" || c == ">=" ||
                                 c == "<=")) {
        text = outlet ? "result" : (i == 0 ? "left operand: outputs the result" : "right operand (stored)");
    }
    if (text.empty()) text = kind == "signal" ? "signal" : "messages";
    (void)nIns;
    return head() + "\n" + text;
}

}  // namespace moritz
