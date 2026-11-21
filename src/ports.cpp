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
                       "start [ms], stop, loop 1/0, envelope C, threshold x, robot 1/0, whisper 1/0, "
                       "cross mode [buffer] (1 multiplicative, 2 spectral flattener, 3 morph, 0 off)",
                       "pitch ratio (2 = an octave up)", "formant ratio (with envelope: moves the formants)",
                       "cross-synthesis amount, 0 to 1", "the second sound for cross-synthesis (without a cross buffer~)"},
                      {"output (channel)", "read position, 0 to 1"}}},
        {"conv~", {{"signal to convolve with the buffer~ (channel); set name, gain x, normalize 0/1",
                    "signal for the next channel (unconnected: the channel before)"},
                   {"convolved signal (channel)"}}},
        {"condec", {{"conv x h out [scale mix] or deconv y h out [threshold scale]: buffer~ names"},
                    {"bang when the result is in the out buffer~"}}},
        {"function", {{"bang: the curve for line~; a number (ms): the value there; domain, range, clear, set"},
                      {"value at the time received", "the curve for line~: first value, then value-time pairs"}}},
        {"render", {{"open patch, send name value, input buffer, render buffer seconds, clear, stop"},
                     {"progress, 0 to 1", "bang: the buffer~ is ready"}}},
        {"nmf", {{"run source dest (unsupervised); learn example, separate mix dest, forget (supervised); "
                  "components k, iterations n, size n, seed n, stop"},
                 {"progress, 0 to 1", "bang: done (dest has one part, or one source, per channel)"}}},
        {"orchestra", {{"read file.db or a dataset's name (fullsol, tinysol, microsol); analyse folder [type block hop ncoeff]; "
                        "players Vn 2 Va ...; add, remove, range, techniques, dynamics, info, stop"},
                       {"progress of reading (or analysing), 0 to 1", "bang: the database is read",
                        "the number of sounds in the database",
                        "the players, a list of instruments (Fl|Picc: one player who doubles); bang: none"}}},
        {"orchidea", {{"run target [out]: buffer~ names; population, epochs, seed, segmentation, partials... ; "
                       "choose segment n, notes, score, show, report, write file, roll buffer, stop"},
                      {"progress of the search, 0 to 1", "bang: done (the notes are out)",
                       "a note a message: number, instrument, pitch, dynamics, technique, onset (ms), duration (ms), "
                       "the sound's path, cents off the target's partial, player",
                       "the distance: the mean cost of the chosen solutions (lower is nearer the target)",
                       "the solution as a score (score token): into a score object, which shows it as a roll and plays it"}}},
        {"specenv~", {{"amplitudes (inside pfft~: one frame); iterations n", "order (cepstral coefficients: the resolution)"},
                      {"spectral envelope", "fine structure: the amplitudes divided by the envelope"}}},
        {"filterdesign", {{"frequency (Hz), or a type: lowpass highpass bandpass notch peak lowshelf highshelf allpass; bang",
                           "Q", "gain in dB (peak and shelves)"},
                          {"coefficients a0 a1 a2 b1 b2, for biquad~"}}},
        {"filterplot", {{"biquad~ coefficients a0 a1 a2 b1 b2 (or several sets: a cascade)"},
                        {"frequency and gain at the mouse, while dragging"}}},
        {"nebula~", {{"1/0, start, stop; density d [r], length ms [r], freq f [r], morph m, amp a, gliss g, jitter j [r], "
                      "speed s, grainlets n, window w, poly n, proto1 x, proto2 x, pitch1 hz, pitch2 hz, seed n",
                      "density (grains per second)", "grain length (ms)",
                      "frequency (Hz; for a buffer~, a speed ratio, or Hz once its pitch is given)",
                      "morph: 0 prototype 1 .. 1 prototype 2"},
                     {"left", "right"}}},
        {"ambienc~", {{"the signal to place", "azimuth (degrees, 0 front, 90 left)", "elevation (degrees, up)"},
                      {"sound field channel (ACN: W Y Z X ...)"}}},
        {"ambirot~", {{"sound field channel (ACN)", "sound field channel (ACN), or the yaw in degrees (last inlet)"},
                      {"rotated sound field channel"}}},
        {"ambidec~", {{"sound field channel (ACN)"}, {"speaker, in the order of the azimuths"}}},
        {"binaural~", {{"sound field channel (ACN: W Y Z X ...)"}, {"left ear", "right ear"}}},
        {"hrtf~", {{"the signal to place", "azimuth (degrees, 0 front, 90 left, 180 behind)", "elevation (degrees, -40 to 90)"},
                   {"left ear", "right ear"}}},
        // atoms and types
        {"atoms", {{"gabor [min max] [fmax], gammatone ..., frames folder [ms hop], onsets folder [threshold max], files folder [max], "
                    "add buffer [mode], get k buffer, clear, write file, read file, info, stop"},
                   {"progress, 0 to 1", "bang: the dictionary is ready", "number of atoms"}}},
        {"pursuit", {{"run sound dictionary book [residual], synth book dictionary out, cross book from to out; atoms n, snr dB, "
                      "components n, candidates n, segments onsets|blocks, block ms, threshold x, ratio r, stretch s, tails 0/1, keep f, stop"},
                     {"progress, 0 to 1", "bang: done", "atoms found", "signal-to-residual (dB)"}}},
        {"soundtypes", {{"analyse sound, analyse book dictionary; segment frames | onsets [threshold gap], pitch 1/0, types k1 k2 k3, levels n, threshold z, smooth n, size n, hop n, seed n; "
                         "labels buffer, transitions level buffer, play level type, write file, info, stop"},
                        {"progress, 0 to 1", "bang: done", "units per level, a list", "play: start end (ms)"}}},
        {"typegen", {{"generate seconds out [labels], next, reset; classic n, markov [order count], oracle [continuity context], "
                      "level n, inner c [context], model name, seed n, xfade ms, stop"},
                     {"progress, 0 to 1", "bang: done", "next: the unit's type", "next: start end (ms) in the original"}}},
        {"typematch", {{"run A B out [level] [labels]; level 0-3, fit 1/0, contour c, space own/joint, xfade ms, stop"}, {"progress, 0 to 1", "bang: done"}}},
        // analysis and MIR
        {"spectrogram", {{"run sound matrix; maxfreq Hz, size n, hop n, stop"},
                         {"progress, 0 to 1", "bang: matrix holds the spectrogram (a channel a bin, dB)"}}},
        {"melgram", {{"run sound matrix; bands n, size n, hop n, stop"},
                     {"progress, 0 to 1", "bang: matrix holds the mel spectrogram (a channel a band, dB)"}}},
        {"mfccgram", {{"run sound matrix; coeffs n, bands n, size n, hop n, stop"},
                      {"progress, 0 to 1", "bang: matrix holds the MFCCs (a channel a coefficient)"}}},
        {"chromagram", {{"run sound matrix; size n, hop n, stop"},
                        {"progress, 0 to 1", "bang: matrix holds the chromagram (12 channels, C first)"}}},
        {"onsets", {{"run sound [curve]; segment k; threshold x, width n, gap ms, size n, hop n, stop"},
                    {"progress, 0 to 1", "bang: done", "onset times (ms), a list", "segment k: start end (ms)",
                     "number of segments"}}},
        {"beats", {{"run sound [curve]; tempogram sound matrix; clicks buffer; prior bpm octaves, tightness x, size n, hop n, stop"},
                   {"progress, 0 to 1", "bang: done", "tempo (bpm)", "beat times (ms), a list"}}},
        {"keyfind", {{"run sound [from to] (ms); size n, hop n, stop"},
                     {"progress, 0 to 1", "bang: done", "the key: \"A minor\"", "its correlation with the profile (-1..1)",
                      "pitch-class distribution: 12 values, C first", "correlation with the 24 keys: C major, C minor, C# major..."}}},
        {"similarity", {{"run features matrix; frames n (at most), standardize 0/1, stop"},
                        {"progress, 0 to 1", "bang: matrix holds the self-similarity matrix"}}},
        {"novelty", {{"run similarity curve; section k; width n (frames), threshold x, gap ms, stop"},
                     {"progress, 0 to 1", "bang: done", "section boundaries (ms), a list", "section k: start end (ms)",
                      "number of sections"}}},
        {"curves", {{"loudness sound curve, register sound curves, polyphony sound curve; smooth n, size n, hop n, stop"},
                    {"progress, 0 to 1", "bang: the curve buffer~ is ready"}}},
        {"melbands~", {{"signal to analyse; bang analyses now; hop n"}, {"mel bands (dB), a list every hop"}}},
        {"mfcc~", {{"signal to analyse; bang analyses now; hop n"}, {"MFCCs, a list every hop"}}},
        {"chroma~", {{"signal to analyse; bang analyses now; hop n"}, {"12 pitch classes (C first, max 1), a list every hop"}}},
        {"onset~", {{"signal to analyse; threshold x, gap ms, gate dB, hop n"}, {"bang: an onset", "spectral flux, every hop"}}},
        {"plot", {{"buffer name [channel], a list, a number; mode m, range lo hi, xrange from to, rows first last, normalize 0/1, ylog 1/0, marks, cursor ms, title, "
                   "labels, colors, history n, clear, open, export file.png",
                   "marks: times (ms on a time axis) drawn as vertical lines"},
                  {"x clicked (ms on a time axis)", "y clicked: the row of an image, the value of a curve"}}},
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
        {"transport", {{"play, stop, locate bar [beat], seek ms, loop bar1 bar2, tempo bpm, meter n d, bang"},
                       {"bar (from 1)", "beat (from 1)", "position in ms", "1 when it starts, 0 when it stops"}}},
        {"score", {{"read/write file.score, clear, add note|sound|msg ..., play, stop, locate bar, info"}, {}}},
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
        {"random", {{"bang: output a random integer; seed x: repeatable", "range (the next integers are 0 .. range-1)"}, {"random integer 0 .. range-1"}}},
        {"counter", {{"bang: output the count, then step", "next value"}, {"count"}}},
        // routing
        {"code", {{"bang runs the script; anything else becomes in1 and runs it; set name values, stop, reset, open, read file.mu",
                   "becomes in2, in3... (stored: nothing runs)"},
                  {"what the script sends with (out k ...), k from 1; the last outlet: bang when the script has run"}}},
        {"pack", {{"first item: outputs the list", "item (stored)"}, {"the list"}}},
        {"prepend", {{"a message: output with the arguments before it", "new arguments"}, {"the message, prefixed"}}},
        {"append", {{"a message: output with the arguments after it", "new arguments"}, {"the message, extended"}}},
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
        {"link", {{"bang: open the target; set target: change it"}, {}}},
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
    if (c == "score" && outlet) {
        int ch = nOuts - 3;
        if (i < ch) text = "its sounds, channel " + std::to_string(i + 1) + " of " + std::to_string(ch);
        else if (i == ch) text = "notes: pitch velocity duration-ms row dynamics technique";
        else if (i == ch + 1) text = "messages: name values... (patch events: patch name duration-ms args...)";
        else text = "info: end when it plays past its last event; info rows events seconds";
    } else if (c == "trigger" || c == "t") {
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
            {"periodicity", "periodicity: YIN's 1 - aperiodicity, 0..1"},
            {"f0", "f0 (Hz) by YIN; 0 when unvoiced"},
            {"mfcc", "mfcc: a list of 13 MFCCs (40 mel bands)"},
            {"chroma", "chroma: a list of the 12 pitch classes, C first"},
            {"melbands", "melbands: a list of 40 mel bands (dB)"}};
        std::vector<std::string> chosen;
        for (auto& x : b->args)
            if (x.isSymbol())
                for (auto& e : d)
                    if (e.first == x.s) chosen.push_back(e.second);
        if (chosen.empty())
            for (size_t k = 0; k < 11; ++k) chosen.push_back(d[k].second);   // the scalars
        if (!outlet) text = "signal to analyse; bang: analyse now; hop n; threshold t; range lo hi";
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
