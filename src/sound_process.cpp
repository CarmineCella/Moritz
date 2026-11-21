// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_process.cpp — the sound editor's processes and analyses (sound_process.h): the offline objects' engines
// over a selection, a patch rendered over it, a Musil script's (process x sr), markers and regions

#include "sound_process.h"
#include "gui.h"
#include "mir.h"
#include "musil_host.h"
#include "objects_maple.h"
#include "offline.h"
#include "session.h"
#include "signals/biquad.h"
#include "signals/convolution.h"
#include "signals/edit.h"
#include "signals/hpss.h"
#include "signals/pitch.h"
#include "signals/vocoder.h"
#include "soundtypes.h"

#include <cmath>
#include <cstdlib>

namespace moritz {

namespace edit = musil::dsp::edit;

// ---------------------------------------------------------------- the kinds

const std::vector<ProcessInfo>& processKinds() {
    static const std::vector<ProcessInfo> k = {
        {"pursuit", "pursuit: rebuild with atoms", "pursuit: the selection rebuilt with atoms",
         {{"dictionary", "Atoms", "gabor", {"gabor", "gammatone"}, "Gabor or gammatone atoms, or the atoms object of an open patch"},
          {"atoms", "Atoms at most", "2000", {}, "How many atoms (more: closer to the sound)"},
          {"snr", "Stop at SNR (dB)", "30", {}, "Stop once the residual is this far under the sound"}},
         false},
        {"typematch", "typematch: replace by units of", "typematch: the selection rebuilt with the units of another sound",
         {{"with", "Units of", "", {}, "The sound whose units replace the selection's"},
          {"level", "Level", "1", {"0", "1", "2"}, "0: frame by frame; 1: units; 2: gestures"},
          {"contour", "Loudness contour", "1", {}, "0..1: how much the selection's loudness is imposed"},
          {"xfade", "Crossfade (ms)", "10", {}, "Between units"}},
         true},
        {"hpss", "hpss: keep", "hpss: keep the harmonic or the percussive part",
         {{"keep", "Keep", "harmonic", {"harmonic", "percussive"}, "Sustained sounds, or attacks"},
          {"kernel", "Kernel (frames and bins)", "17", {}, "The median filters' length (odd)"},
          {"size", "FFT size", "2048", {"1024", "2048", "4096"}, ""}},
         false},
        {"nmf", "nmf: keep components", "nmf: keep some of the parts the sound is made of",
         {{"components", "Components", "4", {}, "How many parts to find (2 to 16)"},
          {"keep", "Keep (numbers)", "1", {"text"}, "The parts to keep, as 1 or 1 3"},
          {"iterations", "Iterations", "100", {}, ""}},
         false},
        {"pvoc", "pvoc: stretch or transpose", "pvoc: stretch or transpose the selection",
         {{"stretch", "Stretch", "1", {}, "2: twice as long, 0.5: half"},
          {"transpose", "Transpose (semitones)", "0", {}, ""},
          {"formants", "Formants", "keep", {"keep", "move"}, "keep: a voice stays a voice"},
          {"window", "Window", "2048", {"1024", "2048", "4096"}, ""}},
         false},
        {"cross", "sparkle~: cross with", "sparkle~: cross-synthesis with another sound",
         {{"with", "With", "", {}, "The second sound"},
          {"mode", "Mode", "multiply", {"multiply", "flatten", "morph"}, "multiply: the two spectra; flatten: its envelope on the selection; morph"},
          {"amount", "Amount", "1", {}, "0..1"}},
         true},
        {"filter", "filter (filterdesign)", "filter: a filterdesign biquad over the selection",
         {{"type", "Type", "lowpass", {"lowpass", "highpass", "bandpass", "notch", "peak", "lowshelf", "highshelf", "allpass"}, ""},
          {"freq", "Frequency (Hz)", "1000", {}, ""},
          {"q", "Q", "0.707", {}, ""},
          {"gain", "Gain (dB)", "0", {}, "peak and the shelves"}},
         false},
        {"condec", "condec: convolve with", "condec: convolve with a response (or take one out)",
         {{"with", "Response", "", {}, "An impulse response (a hall, a body)"},
          {"mode", "Mode", "convolve", {"convolve", "deconvolve"}, ""},
          {"dry", "Dry signal kept", "0", {}, "0..1, convolve: the selection mixed in"},
          {"threshold", "Threshold", "0.001", {}, "deconvolve: the regularisation"}},
         true},
    };
    return k;
}

const ProcessInfo* processInfo(const std::string& kind) {
    for (auto& i : processKinds())
        if (i.kind == kind) return &i;
    return nullptr;
}

std::string ProcessSpec::word(const std::string& k) const {
    auto it = p.find(k);
    if (it != p.end()) return it->second;
    if (const ProcessInfo* i = processInfo(kind))
        for (auto& f : i->fields)
            if (f.key == k) return f.def;
    return "";
}

double ProcessSpec::num(const std::string& k) const {
    auto conv = [](const std::string& s, double& v) {
        char* end = nullptr;
        v = std::strtod(s.c_str(), &end);
        return !s.empty() && end && *end == '\0' && std::isfinite(v);
    };
    double v = 0;
    auto it = p.find(k);
    if (it != p.end() && conv(it->second, v)) return v;
    if (const ProcessInfo* i = processInfo(kind))
        for (auto& f : i->fields)
            if (f.key == k && conv(f.def, v)) return v;
    return 0;
}

// ---------------------------------------------------------------- helpers

namespace {

std::vector<float> channel(const SampleBuffer& b, int c) {
    std::vector<float> x((size_t)b.frames);
    for (long f = 0; f < b.frames; ++f) x[(size_t)f] = b.data[(size_t)f * b.channels + c];
    return x;
}

SampleBuffer fromChannels(const std::vector<std::vector<float>>& ch, double sr) {
    SampleBuffer out;
    out.channels = std::max(1, (int)ch.size());
    out.sr = sr;
    size_t n = 0;
    for (auto& c : ch) n = std::max(n, c.size());
    out.frames = (long)n;
    out.data.assign(n * (size_t)out.channels, 0.f);
    for (size_t c = 0; c < ch.size(); ++c)
        for (size_t f = 0; f < ch[c].size(); ++f) {
            float v = ch[c][f];
            out.data[f * (size_t)out.channels + c] = std::isfinite(v) ? v : 0.f;
        }
    return out;
}

// the peak brought under 0.99 (overlapping atoms, a response's gain): what was done, or ""
std::string limit(SampleBuffer& b, const std::string& what) {
    double p = edit::peak(b.data.data(), (size_t)b.frames, b.channels);
    if (p <= .99) return "";
    edit::normalize_peak(b.data.data(), (size_t)b.frames, b.channels, .99);
    char m[128];
    std::snprintf(m, sizeof m, "%s: scaled by %.1f dB (it would clip)", what.c_str(), 20 * std::log10(.99 / p));
    return m;
}

struct Stopped {};

// progress of channel c of C, p in 0..1; false when the job is cancelled
bool tickOf(Job& job, int c, int C, double p) {
    job.progress((float)((c + std::clamp(p, 0.0, 1.0)) / std::max(1, C)));
    return !job.cancelled();
}

std::vector<float> resampled(const std::vector<float>& x, double from, double to) {
    if (from == to || from <= 0 || to <= 0) return x;
    return resampleFrames(x, 1, to / from);
}

}  // namespace

std::vector<float> resampleFrames(const std::vector<float>& x, int channels, double factor) {
    channels = std::max(1, channels);
    size_t frames = x.size() / (size_t)channels;
    if (frames == 0 || !(factor > 0) || factor == 1.0) return x;
    std::vector<float> out;
    size_t outFrames = 0;
    for (int c = 0; c < channels; ++c) {
        musil::varr v(frames);
        for (size_t f = 0; f < frames; ++f) v[f] = x[f * (size_t)channels + c];
        musil::varr r = musil::resample_sinc(v, factor);       // Musil's resample
        if (c == 0) {
            outFrames = r.size();
            out.assign(outFrames * (size_t)channels, 0.f);
        }
        for (size_t f = 0; f < outFrames && f < r.size(); ++f) out[f * (size_t)channels + c] = (float)r[f];
    }
    return out;
}

// ---------------------------------------------------------------- the engines

bool runProcess(const ProcessSpec& s, const SampleBuffer& in, SampleBuffer& out, std::string& msg, Job& job) {
    msg.clear();
    const int C = std::max(1, in.channels);
    const double sr = in.sr > 0 ? in.sr : 44100.0;
    if (in.frames <= 0) { msg = "nothing to process"; return false; }
    std::vector<std::vector<float>> res;
    try {
        if (s.kind == "pursuit") {
            std::shared_ptr<const maple::Dictionary> d = s.dict;
            std::string dn = s.word("dictionary");
            if (!d) {
                if (dn != "gabor" && dn != "gammatone") { msg = "pursuit: no atoms " + dn; return false; }
                d = std::make_shared<maple::Dictionary>(maple::parametric(dn == "gammatone", sr, 256, 8192, 16000));
            }
            int atoms = std::clamp((int)s.num("atoms"), 1, 1000000);
            double snr = s.num("snr");
            for (int c = 0; c < C; ++c) {
                maple::Book book;
                std::vector<float> residual;
                if (!maple::decompose(*d, channel(in, c), sr, atoms, snr, maple::SoundPursuitOptions(), book, residual,
                                      [&](double p) { return tickOf(job, c, C, p * .9); }))
                    return false;
                std::vector<float> y = maple::synthesize(book, *d, maple::SynthOptions());
                y = resampled(y, book.sr, sr);
                y.resize((size_t)in.frames, 0.f);
                res.push_back(std::move(y));
            }
            out = fromChannels(res, sr);
            msg = limit(out, "pursuit");
            return !job.cancelled();
        }
        if (s.kind == "typematch") {
            if (!s.other || s.other->frames <= 0) { msg = "typematch: choose the sound whose units replace the selection"; return false; }
            auto A = std::make_shared<soundtypes::Model>(), B = std::make_shared<soundtypes::Model>();
            A->sound = monoOf(in);
            A->sr = sr;
            B->sound = monoOf(*s.other);
            B->sr = s.other->sr > 0 ? s.other->sr : 44100.0;
            soundtypes::AnalyseOptions o;
            int level = std::clamp((int)s.num("level"), 0, 2);
            o.types.levels = std::max(1, level);
            if (!soundtypes::analyseModel(*A, o, [&](double p) { return tickOf(job, 0, 3, p); }) ||
                !soundtypes::analyseModel(*B, o, [&](double p) { return tickOf(job, 1, 3, p); })) {
                if (!job.cancelled()) msg = "typematch: no types found (is the selection or the other sound too short?)";
                return false;
            }
            std::vector<float> y = soundtypes::rematch(*A, *B, level, true, true, std::clamp(s.num("xfade"), .5, 500.0),
                                                       std::clamp(s.num("contour"), 0.0, 1.0));
            if (y.empty()) { msg = "typematch: nothing matched"; return false; }
            res.push_back(resampled(y, B->sr, sr));
            out = fromChannels(res, sr);
            msg = limit(out, "typematch");
            return !job.cancelled();
        }
        if (s.kind == "hpss") {
            bool harm = s.word("keep") != "percussive";
            int n = 1;
            while (n < std::clamp((int)s.num("size"), 256, 16384)) n <<= 1;
            int kernel = std::clamp((int)s.num("kernel"), 3, 101) | 1;
            for (int c = 0; c < C; ++c) {
                std::vector<float> x = channel(in, c), h, p;
                int calls = 0;
                if (!musil::dsp::hpss<float>(x.data(), x.size(), (size_t)n, (size_t)n / 4, kernel, h, p,
                                             [&] { return tickOf(job, c, C, std::min(.95, .1 * ++calls)); }))
                    return false;
                res.push_back(harm ? std::move(h) : std::move(p));
            }
            out = fromChannels(res, sr);
            return !job.cancelled();
        }
        if (s.kind == "nmf") {
            int K = std::clamp((int)s.num("components"), 2, 16);
            std::vector<int> keep;
            const std::string keepText = s.word("keep");
            for (const char* q = keepText.c_str(); *q;) {
                char* e = nullptr;
                long v = std::strtol(q, &e, 10);
                if (e == q) { ++q; continue; }
                if (v >= 1 && v <= K) keep.push_back((int)v - 1);
                q = e;
            }
            if (keep.empty()) { msg = "nmf: keep which parts? (numbers from 1 to " + std::to_string(K) + ")"; return false; }
            int iters = std::clamp((int)s.num("iterations"), 1, 10000);
            for (int c = 0; c < C; ++c) {        // each channel factored on its own (the same seed)
                std::vector<std::vector<float>> parts;
                if (!nmfSeparate(channel(in, c), K, iters, 2048, 1, job, parts)) return false;
                std::vector<float> y((size_t)in.frames, 0.f);
                for (int k : keep)
                    if (k < (int)parts.size())
                        for (size_t f = 0; f < y.size() && f < parts[(size_t)k].size(); ++f) y[f] += parts[(size_t)k][f];
                res.push_back(std::move(y));
            }
            out = fromChannels(res, sr);
            return !job.cancelled();
        }
        if (s.kind == "pvoc" || s.kind == "cross") {
            musil::dsp::PvocOptions o;
            std::vector<double> other;
            if (s.kind == "pvoc") {
                o.stretch0 = o.stretch1 = std::clamp(s.num("stretch"), 0.05, 50.0);
                o.pitch0 = o.pitch1 = std::pow(2.0, std::clamp(s.num("transpose"), -48.0, 48.0) / 12.0);
                o.env = s.word("formants") == "keep" && o.pitch0 != 1.0 ? 40 : 0;
                o.window = std::clamp((int)s.num("window"), 256, 16384);
            } else {
                if (!s.other || s.other->frames <= 0) { msg = "sparkle~ cross: choose the second sound"; return false; }
                std::string mode = s.word("mode");
                o.xmode = mode == "flatten" ? 2 : mode == "morph" ? 3 : 1;
                o.env = o.xmode == 2 ? 40 : 0;
                o.xa0 = o.xa1 = std::clamp(s.num("amount"), 0.0, 1.0);
                std::vector<float> m = resampled(monoOf(*s.other), s.other->sr, sr);
                other.assign(m.begin(), m.end());
            }
            for (int c = 0; c < C; ++c) {
                std::vector<float> xf = channel(in, c);
                std::vector<double> x(xf.begin(), xf.end());
                uint32_t seed = 1234u + (uint32_t)c;
                std::vector<double> y = musil::dsp::pvoc(x.data(), x.size(), o, other.empty() ? nullptr : other.data(), other.size(),
                                                         [&seed] { return seed++; },
                                                         [&](double p) { return tickOf(job, c, C, p); });
                if (job.cancelled()) return false;
                if (s.kind == "cross") y.resize(x.size(), 0.0);
                res.emplace_back(y.begin(), y.end());
            }
            out = fromChannels(res, sr);
            msg = limit(out, s.kind == "pvoc" ? "pvoc" : "sparkle~ cross");
            return !job.cancelled();
        }
        if (s.kind == "filter") {
            musil::dsp::BiquadType t;
            if (!musil::dsp::biquad_type(s.word("type"), t)) { msg = "filter: unknown type " + s.word("type"); return false; }
            double f0 = std::clamp(s.num("freq"), 1.0, sr / 2 * .999);
            auto k = musil::dsp::rbj(t, sr, f0, std::max(0.01, s.num("q")), s.num("gain"));   // filterdesign's
            const double b[3] = {k.b0, k.b1, k.b2}, a[3] = {k.a0, k.a1, k.a2};
            for (int c = 0; c < C; ++c) {
                std::vector<float> xf = channel(in, c);
                std::vector<double> x(xf.begin(), xf.end()), y(x.size());
                musil::iir_state st;
                musil::iir_run(x.data(), y.data(), x.size(), b, 3, a, 3, st);   // Musil's iir
                res.emplace_back(y.begin(), y.end());
                tickOf(job, c + 1, C, 0);
            }
            out = fromChannels(res, sr);
            msg = limit(out, "filter");
            return !job.cancelled();
        }
        if (s.kind == "condec") {
            if (!s.other || s.other->frames <= 0) { msg = "condec: choose the response"; return false; }
            bool conv = s.word("mode") != "deconvolve";
            double dry = std::clamp(s.num("dry"), 0.0, 1.0), thr = std::max(0.0, s.num("threshold"));
            SampleBuffer h = *s.other;
            if (h.sr > 0 && h.sr != sr) {                  // the response at the sound's rate
                h.data = resampleFrames(h.data, h.channels, sr / h.sr);
                h.frames = (long)(h.data.size() / (size_t)std::max(1, h.channels));
                h.sr = sr;
            }
            for (int c = 0; c < C; ++c) {
                std::vector<float> xf = channel(in, c), hf = channel(h, std::min(c, h.channels - 1));
                std::vector<double> x(xf.begin(), xf.end()), hh(hf.begin(), hf.end()), y;
                if (conv) {
                    y = musil::dsp::convolve(x.data(), x.size(), hh.data(), hh.size(), [&] {
                        if (job.cancelled()) throw Stopped();
                    });
                    for (size_t f = 0; f < x.size(); ++f) y[f] += dry * x[f];
                } else {
                    y = musil::dsp::deconvolve(x, hh, thr);
                }
                res.emplace_back(y.begin(), y.end());
                tickOf(job, c + 1, C, 0);
            }
            out = fromChannels(res, sr);
            msg = limit(out, "condec");
            return !job.cancelled();
        }
    } catch (const Stopped&) {
        return false;
    } catch (const std::exception& e) {
        msg = s.kind + ": " + e.what();
        return false;
    }
    msg = "unknown process " + s.kind;
    return false;
}

// ---------------------------------------------------------------- a patch

PatchProcess::~PatchProcess() { patch_.reset(); }

namespace {
bool isHead(const SExpr& e, const char* h) {
    return e.isList && !e.items.empty() && !e.items[0].isList && e.items[0].text == h;
}
}  // namespace

bool PatchProcess::prepare(const std::string& path, const SampleBuffer& in, double tail, std::string& err) {
    std::string src;
    if (!readFile(path, src)) { err = "can't read " + path; return false; }
    std::vector<SExpr> forms;
    if (!parseSExprs(src, forms, err)) { err = path + ": " + err; return false; }
    const SExpr* inner = nullptr;
    for (auto& f : forms)
        if (isHead(f, "patch")) { inner = &f; break; }
    if (!inner) { err = path + " is not a patch"; return false; }
    std::vector<SExpr> outer;
    std::string e2;
    parseSExprs("(patch (obj fx p process (pos 120 120)))", outer, e2);
    outer[0].items[1].items.push_back(*inner);      // the file's patch, inside a subpatch box
    sr_ = in.sr > 0 ? in.sr : 44100.0;
    inCh_ = std::max(1, in.channels);
    in_ = in.data;
    inFrames_ = in.frames;
    total_ = in.frames + (long)std::max(0.0, std::min(60.0, tail) * sr_);
    prints_ = std::make_shared<std::pair<std::mutex, std::vector<std::string>>>();
    patch_ = std::make_unique<Patch>();
    Patch& p = *patch_;
    p.dir = dirOf(path);
    p.sampleRate = sr_;
    p.dspRunning = true;
    p.offline = true;                     // the clock is the sample count
    p.ioBufferFrames = dsp::kBlock;
    p.renderDepth = 1;
    auto prints = prints_;
    p.onPrint = [prints](const std::string& s) {
        std::lock_guard<std::mutex> lk(prints->first);
        prints->second.push_back(s);
    };
    auto frame = frame_;
    double sr = sr_;
    p.clock = [frame, sr] { return *frame / sr; };
    if (!p.load(outer[0], err)) { err = path + ": " + err; patch_.reset(); return false; }
    err.clear();
    Box* fx = p.find("fx");
    std::vector<int> sigIns, sigOuts;
    for (int k = 0; fx && k < fx->numIns(); ++k)
        if (fx->ins[(size_t)k] == P::Signal) sigIns.push_back(k);
    for (int k = 0; fx && k < fx->numOuts(); ++k)
        if (fx->outs[(size_t)k] == P::Signal) sigOuts.push_back(k);
    if (sigIns.empty() || sigOuts.empty()) {
        err = "the patch needs an inlet~ (the selection goes in) and an outlet~ (the result comes out)";
        patch_.reset();
        return false;
    }
    Message a, b;
    for (int c = 1; c <= inCh_; ++c) a.push_back(Atom((float)c));
    for (size_t c = 1; c <= sigOuts.size(); ++c) b.push_back(Atom((float)c));
    Box* adc = p.addBox(p.create("adc~", a), 120, 40, "in");
    Box* dac = p.addBox(p.create("dac~", b), 120, 200, "out");
    if (!adc || !dac) { err = "can't make adc~ and dac~"; patch_.reset(); return false; }
    for (size_t k = 0; k < sigIns.size(); ++k) p.connect(adc, (int)(k % (size_t)inCh_), fx, sigIns[k]);
    for (size_t k = 0; k < sigOuts.size(); ++k) p.connect(fx, sigOuts[k], dac, (int)k);
    p.loadbangAll();
    out_.assign(sigOuts.size(), std::vector<float>((size_t)total_, 0.f));
    return true;
}

void PatchProcess::run(Job& job) {
    if (patch_) renderBlocks(patch_.get(), *frame_, total_, in_, inCh_, out_, job);
}

SampleBuffer PatchProcess::take(std::vector<std::string>& prints) {
    patch_.reset();
    if (prints_) {
        std::lock_guard<std::mutex> lk(prints_->first);
        prints = prints_->second;
    }
    // the tail kept up to where it falls silent (below -90 dB): a patch without one (a filter, a ring modulator)
    // gives as much as it was given
    size_t keep = (size_t)std::min<long>(inFrames_, total_);
    for (auto& c : out_)
        for (size_t f = c.size(); f > keep; --f)
            if (std::fabs(c[f - 1]) > 3.2e-5f) { keep = f; break; }
    for (auto& c : out_) if (c.size() > keep) c.resize(keep);
    return fromChannels(out_, sr_);
}

// ---------------------------------------------------------------- a Musil script

ScriptProcess::ScriptProcess() = default;

ScriptProcess::~ScriptProcess() { stop(); }

bool ScriptProcess::running() const { return ctx_ && ctx_->busy(); }

void ScriptProcess::stop() {
    if (alive_) alive_->store(false);
    if (ctx_) ctx_->stop();
    alive_.reset();
}

bool ScriptProcess::start(const std::string& path, const SampleBuffer& in,
                          std::function<void(bool, SampleBuffer, std::string)> done) {
    std::string code;
    if (!readFile(path, code)) {
        done(false, {}, "can't read " + path);
        return false;
    }
    stop();
    ContextHooks h;
    h.restricted = true;                 // a code box's permissions: no programs, no network
    ctx_ = std::make_shared<LangContext>("process", h);
    alive_ = std::make_shared<std::atomic<bool>>(true);
    auto alive = alive_;
    auto input = std::make_shared<SampleBuffer>(in);
    auto result = std::make_shared<SampleBuffer>();
    auto then = [input, result](musil::Interp& I) {
        musil::vptr fn = I.global->find("process");
        if (!fn) I.err("the script defines no function process: (define (process x sr) ...) returns the sound");
        musil::vlist chans;
        for (int c = 0; c < input->channels; ++c) {
            musil::varr v((size_t)input->frames);
            for (long f = 0; f < input->frames; ++f) v[(size_t)f] = input->data[(size_t)f * input->channels + c];
            chans.push_back(musil::v_arr(std::move(v)));
        }
        musil::vptr r = I.call_fn(fn, {musil::v_list(std::move(chans)), musil::v_num(input->sr)});
        std::vector<const musil::varr*> got;
        if (r && r->t == musil::Value::NUM) got.push_back(&r->num);
        else if (r && r->t == musil::Value::LIST && !r->l.empty() && r->l.size() <= 64) {
            for (auto& c : r->l) {
                if (!c || c->t != musil::Value::NUM) { got.clear(); break; }
                got.push_back(&c->num);
            }
        }
        if (got.empty()) I.err("process returned no sound: a vector, or a list of channel vectors, is wanted");
        size_t n = 0;
        for (auto* g : got) n = std::max(n, g->size());
        if (n > ((size_t)1 << 31) / got.size()) I.err("process returned a sound too long");
        result->channels = (int)got.size();
        result->sr = input->sr;
        result->frames = (long)n;
        result->data.assign(n * got.size(), 0.f);
        for (size_t c = 0; c < got.size(); ++c)
            for (size_t f = 0; f < got[c]->size(); ++f) {
                double v = (*got[c])[f];
                result->data[f * got.size() + c] = std::isfinite(v) ? (float)v : 0.f;
            }
    };
    bool ok = ctx_->eval(code, path, 1, [alive, result, done](const EvalResult& r) {
        if (!alive->load()) return;                 // stopped, or the editor is gone
        alive->store(false);
        if (!r.ok) {
            std::string where = r.file.empty() ? "" : r.file + (r.line > 0 ? ":" + std::to_string(r.line) : "") + ": ";
            done(false, {}, r.stopped ? "stopped" : where + r.error);
            return;
        }
        done(true, std::move(*result), "");
    }, then);
    if (!ok) done(false, {}, "the script is still running");
    return ok;
}

// ---------------------------------------------------------------- markers and regions

namespace {
mir::Tick tick(Job& job, double a, double b) {
    return [&job, a, b](double p) {
        job.progress((float)(a + (b - a) * p));
        return !job.cancelled();
    };
}
}  // namespace

std::vector<long> onsetFrames(const SampleBuffer& in, const MarkOptions& o, Job& job) {
    std::vector<float> x = monoOf(in);
    const int n = 1024, hop = 256;                         // the onsets object's
    mir::Mat mag = mir::magSpectrogram(x, n, hop, tick(job, 0, .9));
    std::vector<long> out;
    if (mag.empty() || job.cancelled()) return out;
    mir::Vec flux = mir::fluxOf(mag);
    for (double t : mir::onsetTimes(x, in.sr, n, hop, flux, o.threshold, 21, o.gap))
        out.push_back(std::clamp((long)std::llround(t * in.sr), 0L, in.frames));
    return out;
}

std::vector<long> beatFrames(const SampleBuffer& in, Job& job, double* bpmOut) {
    std::vector<float> x = monoOf(in);
    const int n = 1024, hop = 256;                         // the beats object's
    mir::Mat mag = mir::magSpectrogram(x, n, hop, tick(job, 0, .8));
    std::vector<long> out;
    if (mag.empty() || job.cancelled()) return out;
    mir::Vec flux = mir::fluxOf(mag);
    double fps = in.sr / hop;
    double bpm = mir::tempoEstimate(flux, fps, 100, .6);
    if (bpmOut) *bpmOut = bpm;
    if (!(bpm > 0)) return out;
    for (int f : mir::beatTrack(flux, fps, bpm, 100)) out.push_back(std::clamp((long)f * hop, 0L, in.frames));
    return out;
}

std::vector<long> noveltyFrames(const SampleBuffer& in, Job& job) {
    std::vector<float> x = monoOf(in);
    const int n = 4096, hop = 1024;                        // the chromagram object's
    mir::Mat mag = mir::magSpectrogram(x, n, hop, tick(job, 0, .5));
    std::vector<long> out;
    if (mag.size() < 8 || job.cancelled()) return out;
    auto pc = mir::chromaMap(in.sr, n);
    size_t frames = mag.size(), group = (frames + 999) / 1000;   // similarity's 1000 frames at most
    mir::Mat F(12, mir::Vec());
    mir::Vec c;
    for (size_t f = 0; f < frames; f += group) {
        mir::Vec sum(12, 0.0);
        size_t k = f;
        for (; k < frames && k < f + group; ++k) {
            mir::chroma(mag[k], pc, c);
            for (int r = 0; r < 12; ++r) sum[(size_t)r] += c[(size_t)r];
        }
        for (int r = 0; r < 12; ++r) F[(size_t)r].push_back(sum[(size_t)r] / (double)(k - f));
    }
    mir::Mat S = mir::selfSimilarity(F, tick(job, .5, .9));
    if (job.cancelled()) return out;
    double fps = in.sr / hop / (double)group;
    int width = std::clamp((int)F[0].size() / 8, 2, 32);   // novelty's 32 frames, less for a short sound
    mir::Vec nov = mir::footeNovelty(S, width);
    int gap = std::max(1, (int)std::lround(2.0 * fps));    // novelty's 2 s
    for (int f : mir::noveltyPeaks(nov, 1.5, 2 * width + 1, gap))
        if (f / fps > .5) out.push_back(std::clamp((long)std::llround(f / fps * in.sr), 0L, in.frames));
    return out;
}

void measureRegion(const SampleBuffer& in, Region& r) {
    long a = std::clamp(r.start, 0L, in.frames), b = std::clamp(r.end, a, in.frames);
    r.energy = 0;
    r.pitch = 0;
    if (b <= a) return;
    const int C = std::max(1, in.channels);
    std::vector<float> x((size_t)(b - a));
    double e = 0;
    for (long f = a; f < b; ++f) {
        float s = 0;
        for (int c = 0; c < C; ++c) s += in.data[(size_t)f * C + c];
        s /= C;
        x[(size_t)(f - a)] = s;
        e += (double)s * s;
    }
    r.energy = std::sqrt(e / (double)(b - a));
    const int len = 2048;
    if ((long)x.size() < len) return;
    musil::dsp::Yin yin;                                    // Musil's YIN (descriptors~, yin)
    yin.init(len, in.sr > 0 ? in.sr : 44100.0, 50.0, 2000.0);
    if (!yin.ready()) return;
    std::vector<double> f0s;
    long frames = ((long)x.size() - len) / (len / 2) + 1, step = std::max(1L, frames / 48);
    for (long k = 0; k < frames; k += step) {
        auto est = yin.estimate(&x[(size_t)(k * (len / 2))]);
        if (est.f0 > 0 && est.periodicity > .6) f0s.push_back(est.f0);
    }
    if (f0s.size() < 2 || f0s.size() * 3 < (size_t)(frames / step)) return;   // mostly unvoiced: no pitch
    std::nth_element(f0s.begin(), f0s.begin() + (long)f0s.size() / 2, f0s.end());
    r.pitch = 69 + 12 * std::log2(f0s[f0s.size() / 2] / 440.0);
}

std::vector<Region> typeRegions(const SampleBuffer& in, const MarkOptions& o, Job& job) {
    soundtypes::Model m;
    m.sound = monoOf(in);
    m.sr = in.sr > 0 ? in.sr : 44100.0;
    soundtypes::AnalyseOptions ao;                          // the soundtypes object's, one level
    ao.types.k = {std::clamp(o.types, 2, 26)};
    ao.types.levels = 1;
    ao.onsets = o.onsetUnits;
    std::vector<Region> out;
    if (!soundtypes::analyseModel(m, ao, tick(job, 0, .9)) || m.levels.empty()) return out;
    std::vector<int> count(64, 0);
    for (auto& u : m.levels[0].units) {
        Region r;
        r.start = std::clamp(u.start, 0L, in.frames);
        r.end = std::clamp(u.end, r.start, in.frames);
        if (r.end <= r.start) continue;
        r.type = u.type;
        r.name = typeName(u.type) + " " + std::to_string(++count[(size_t)std::clamp(u.type, 0, 63)]);
        r.source = "types";
        measureRegion(in, r);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Region> onsetRegions(const SampleBuffer& in, const MarkOptions& o, Job& job) {
    std::vector<long> on = onsetFrames(in, o, job);
    std::vector<Region> out;
    if (job.cancelled()) return out;
    std::vector<long> edges{0};
    for (long f : on)
        if (f > edges.back()) edges.push_back(f);
    if (edges.back() < in.frames) edges.push_back(in.frames);
    long shortest = (long)(.01 * in.sr);
    for (size_t k = 0; k + 1 < edges.size(); ++k) {
        Region r;
        r.start = edges[k];
        r.end = edges[k + 1];
        if (r.end - r.start < shortest && !out.empty()) { out.back().end = r.end; continue; }
        r.name = "event " + std::to_string(out.size() + 1);
        r.source = "onsets";
        out.push_back(std::move(r));
    }
    for (auto& r : out) measureRegion(in, r);
    return out;
}

std::string pitchName(double midi) {
    if (!(midi > 0)) return "\xe2\x80\x94";                 // an em dash
    return musil::midi_to_pitch((int)std::lround(midi));
}

}  // namespace moritz
