// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_doc.cpp — a sound being edited (sound_doc.h): snapshots, splices and their undo, the buffer~ it may be,
// the waveform's summary and the spectrogram

#include "sound_doc.h"
#include "fileio.h"
#include "mir.h"              // Musil's STFT frame (mir::FrameSpectrum)
#include "sound_process.h"    // Musil's resample
#include "signals/edit.h"     // Musil's gain, fades, normalisation, reversal, channel conversion

#include <cstdio>
#include <limits>
#include <set>

namespace moritz {

namespace edit = musil::dsp::edit;

Sound& soundClipboard() {
    static Sound clip;
    return clip;
}

namespace {
std::string secs(double s) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f s", s);
    return b;
}
bool endsWithCase(const std::string& s, const char* tail) {
    size_t n = std::char_traits<char>::length(tail);
    if (s.size() < n) return false;
    for (size_t k = 0; k < n; ++k)
        if (std::tolower((unsigned char)s[s.size() - n + k]) != tail[k]) return false;
    return true;
}
}  // namespace

// ---------------------------------------------------------------- the waveform's summary

void WaveSummary::build(const SampleBuffer& s) {
    levels_.clear();
    ch_ = std::max(1, s.channels);
    const long frames = s.frames;
    if (frames <= kBase * 8) return;                     // short: columns read the samples
    Level l0;
    l0.block = kBase;
    long nb = (frames + kBase - 1) / kBase;
    l0.mn.assign((size_t)nb * ch_, std::numeric_limits<float>::max());
    l0.mx.assign((size_t)nb * ch_, -std::numeric_limits<float>::max());
    const float* x = s.data.data();
    for (long f = 0; f < frames; ++f) {
        size_t b = (size_t)(f / kBase) * ch_;
        for (int c = 0; c < ch_; ++c) {
            float v = x[(size_t)f * ch_ + c];
            if (v < l0.mn[b + c]) l0.mn[b + c] = v;
            if (v > l0.mx[b + c]) l0.mx[b + c] = v;
        }
    }
    levels_.push_back(std::move(l0));
    while (nb > kFactor * 8) {                           // coarser levels, while they hold more than a few blocks
        const Level& p = levels_.back();
        Level l;
        l.block = p.block * kFactor;
        long nb2 = (nb + kFactor - 1) / kFactor;
        l.mn.assign((size_t)nb2 * ch_, std::numeric_limits<float>::max());
        l.mx.assign((size_t)nb2 * ch_, -std::numeric_limits<float>::max());
        for (long b = 0; b < nb; ++b)
            for (int c = 0; c < ch_; ++c) {
                size_t i = (size_t)(b / kFactor) * ch_ + c, j = (size_t)b * ch_ + c;
                l.mn[i] = std::min(l.mn[i], p.mn[j]);
                l.mx[i] = std::max(l.mx[i], p.mx[j]);
            }
        nb = nb2;
        levels_.push_back(std::move(l));
    }
}

void WaveSummary::range(const SampleBuffer& s, int c, long a, long b, float& lo, float& hi) const {
    a = std::clamp(a, 0L, s.frames);
    b = std::clamp(b, a, s.frames);
    lo = hi = 0.f;
    if (b <= a || c < 0 || c >= s.channels) return;
    const Level* L = nullptr;
    for (auto& l : levels_) {
        if (l.block * 8 > b - a) break;                  // a level whose blocks are an eighth of the column or less
        L = &l;
    }
    lo = std::numeric_limits<float>::max();
    hi = -lo;
    if (!L) {
        for (long f = a; f < b; ++f) {
            float v = s.data[(size_t)f * s.channels + c];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return;
    }
    long i0 = a / L->block, i1 = (b + L->block - 1) / L->block;
    for (long i = i0; i < i1; ++i) {
        lo = std::min(lo, L->mn[(size_t)i * ch_ + c]);
        hi = std::max(hi, L->mx[(size_t)i * ch_ + c]);
    }
}

// ---------------------------------------------------------------- the spectrogram

bool computeSpectrogram(const SampleBuffer& s, Spectrogram& out, const std::function<bool(double)>& tick, int maxCols,
                        int rows) {
    Spectrogram g;
    g.sr = s.sr > 0 ? s.sr : 44100.0;
    g.n = g.sr <= 50000 ? 2048 : 4096;
    g.rows = std::max(8, rows);
    g.fmax = g.sr / 2;
    g.fmin = std::min(std::max(40.0, 2 * g.sr / g.n), g.fmax / 2);
    const long frames = s.frames;
    g.hop = std::max<long>(64, (frames + maxCols - 1) / std::max(1, maxCols));
    g.cols = frames > 0 ? (int)(frames / g.hop) + 1 : 0;
    g.v.assign((size_t)g.cols * g.rows, 0);
    // each row: the bins under it (their largest magnitude), or, where a row is narrower than a bin, the
    // magnitude between the two nearest bins
    std::vector<double> b0((size_t)g.rows), b1((size_t)g.rows);
    for (int r = 0; r < g.rows; ++r) {
        b0[(size_t)r] = g.rowFreq(r) * g.n / g.sr;
        b1[(size_t)r] = g.rowFreq(r + 1) * g.n / g.sr;
    }
    musil::mir::FrameSpectrum fs;
    fs.init(g.n);
    std::vector<double> buf((size_t)g.n);
    musil::mir::Vec mag;
    const int ch = std::max(1, s.channels);
    const int bins = g.n / 2 + 1;
    for (int col = 0; col < g.cols; ++col) {
        long start = (long)col * g.hop - g.n / 2;
        for (int k = 0; k < g.n; ++k) {
            long f = start + k;
            double v = 0;
            if (f >= 0 && f < frames) {
                const float* x = &s.data[(size_t)f * ch];
                for (int c = 0; c < ch; ++c) v += x[c];
                v /= ch;
            }
            buf[(size_t)k] = v;
        }
        fs.magnitudes(buf.data(), mag);
        unsigned char* dst = &g.v[(size_t)col * g.rows];
        for (int r = 0; r < g.rows; ++r) {
            double lo = b0[(size_t)r], hi = b1[(size_t)r], m;
            int i0 = (int)std::ceil(lo), i1 = (int)std::floor(hi);
            if (i1 >= i0) {
                m = 0;
                for (int i = std::max(0, i0); i <= std::min(bins - 1, i1); ++i) m = std::max(m, mag[(size_t)i]);
            } else {
                double c = 0.5 * (lo + hi);
                int i = std::clamp((int)std::floor(c), 0, bins - 2);
                double f = std::clamp(c - i, 0.0, 1.0);
                m = mag[(size_t)i] + f * (mag[(size_t)i + 1] - mag[(size_t)i]);
            }
            double db = std::clamp(20.0 * std::log10(m + 1e-12), -100.0, 0.0);
            dst[r] = (unsigned char)std::lround((db + 100.0) * 2.55);
        }
        if (tick && col % 64 == 0 && !tick((double)col / g.cols)) return false;
    }
    out = std::move(g);
    return true;
}

// ---------------------------------------------------------------- the document

size_t SoundDoc::undoBudget = (size_t)512 << 20;

SoundDoc::SoundDoc() {
    auto s = std::make_shared<SampleBuffer>();
    s->channels = 1;
    sound_ = s;
    marks_ = std::make_shared<const SoundMarks>();
}

void SoundDoc::setMarksNow(MarksPtr m) {
    marks_ = std::move(m);
    ++marksGen_;
    if (bound_)
        if (BufferBox* b = buffer()) b->marks = marks_;  // kept with the buffer~ while its patch is open
    if (onMarks) onMarks();
}

bool SoundDoc::setMarks(const std::string& name, SoundMarks next) {
    next.clamp(frames());
    next.sort();
    if (next == *marks_) {
        message = name + ": nothing changed";
        return false;
    }
    Step st;
    st.name = name;
    st.selBefore = st.selAfter = sel;
    st.playBefore = st.playAfter = playhead;
    st.marksBefore = marks_;
    st.marksAfter = std::make_shared<const SoundMarks>(std::move(next));
    st.serial = nextSerial_++;
    redo_.clear();
    MarksPtr after = st.marksAfter;
    undo_.push_back(std::move(st));
    trimHistory();
    setMarksNow(std::move(after));
    return true;
}

bool SoundDoc::writeSidecar(const std::string& soundPath, std::string& err) const {
    if (marks_->empty() && keepSidecar_) return true;     // one that couldn't be read is left as it is
    return writeMarksFor(soundPath, *marks_, sr(), seconds(frames()), err);
}

void SoundDoc::setSound(SampleBuffer&& b) {
    if (b.channels < 1) b.channels = 1;
    b.frames = (long)(b.data.size() / (size_t)b.channels);
    sound_ = std::make_shared<const SampleBuffer>(std::move(b));
    summary_.build(*sound_);
    undo_.clear();
    redo_.clear();
    savedSerial_ = 0;                                    // as it is now: nothing unsaved
    select(sel.start, sel.end);
    playhead = std::clamp(playhead, 0L, frames());
    ++generation_;
    if (onChanged) onChanged();
    if (!marks_->empty()) {                              // nothing past the new end
        SoundMarks m = *marks_;
        m.clamp(frames());
        if (!(m == *marks_)) setMarksNow(std::make_shared<const SoundMarks>(std::move(m)));
    }
}

bool SoundDoc::load(const std::string& path, std::string& err) {
    SampleBuffer b;
    if (!decodeAudioFile(path, b, err)) return false;
    bound_ = false;
    bufName_.clear();
    root_ = nullptr;
    path_ = fileio::absolute(path);
    sel = {};
    playhead = 0;
    savedSerial_ = 0;
    marks_ = std::make_shared<const SoundMarks>();
    keepSidecar_ = false;
    setSound(std::move(b));
    SoundMarks m;
    std::string merr;
    if (!readMarksFor(path_, sr(), frames(), m, merr)) {
        keepSidecar_ = true;
        message = "its markers can't be read (" + merr + "): the file is left as it is";
    } else if (!m.empty()) {
        setMarksNow(std::make_shared<const SoundMarks>(std::move(m)));
    }
    return true;
}

bool SoundDoc::savesInPlace() const {
    return !path_.empty() && (endsWithCase(path_, ".wav") || endsWithCase(path_, ".wave"));
}

bool SoundDoc::save(std::string& err) {
    if (!savesInPlace()) {
        err = path_.empty() ? "the sound has no file yet" : "only WAV files are written: save it as a WAV file";
        return false;
    }
    if (!writeSidecar(path_, err)) return false;         // the markers first: a failure leaves the sound as it was
    if (!writeWavFile(path_, *sound_, err, 24)) return false;
    savedSerial_ = topSerial();
    return true;
}

bool SoundDoc::saveTo(const std::string& path, std::string& err) {
    if (!writeSidecar(path, err)) return false;
    if (!writeWavFile(path, *sound_, err, 24)) return false;
    path_ = fileio::absolute(path);
    keepSidecar_ = false;
    savedSerial_ = topSerial();
    return true;
}

bool SoundDoc::exportTo(const std::string& path, std::string& err) const {
    if (!marks_->empty() && !writeMarksFor(path, *marks_, sr(), seconds(frames()), err)) return false;
    return writeWavFile(path, *sound_, err, 24);
}

void SoundDoc::bindBuffer(Patch* root, const std::string& name) {
    root_ = root;
    bufName_ = name;
    bound_ = true;
    path_.clear();
    savedSerial_ = 0;
    sel = {};
    playhead = 0;
    seenBox_ = nullptr;
    seenVersion_ = -1;
    keepSidecar_ = false;
    BufferBox* b = buffer();
    marks_ = b && b->marks ? b->marks : std::make_shared<const SoundMarks>();
    followBuffer();
    message.clear();
}

BufferBox* SoundDoc::buffer() const { return bound_ && root_ ? findBuffer(root_, bufName_) : nullptr; }

bool SoundDoc::followBuffer(bool writes) {
    BufferBox* b = buffer();
    if (!b) return false;
    unsigned w = b->writes.load(std::memory_order_relaxed);
    bool replaced = b != seenBox_ || b->version != seenVersion_;
    if (!replaced && (!writes || w == seenWrites_)) return false;
    bool first = !seenBox_;
    seenBox_ = b;
    seenVersion_ = b->version;
    seenWrites_ = w;
    setSound(b->copy());
    if (!first)
        message = "buffer~ " + bufName_ + (replaced ? " was changed elsewhere" : " was written by record~ or poke~") +
                  ": shown as it is now (undo starts again)";
    return true;
}

std::vector<std::string> SoundDoc::users(int* patches) const {
    std::vector<std::string> classes;
    std::set<const Patch*> where;
    if (bound_ && root_)
        root_->forEachBox([&](Box* b) {
            if (b->cls == "buffer~" || b->kind == BoxKind::Comment || b->kind == BoxKind::Message) return;
            if (b->args.empty() || !b->args[0].isSymbol() || b->args[0].s != bufName_) return;
            classes.push_back(b->cls);
            where.insert(b->patch);
        });
    if (patches) *patches = (int)where.size();
    return classes;
}

void SoundDoc::select(long a, long b) {
    a = std::clamp(a, 0L, frames());
    b = std::clamp(b, 0L, frames());
    if (b < a) std::swap(a, b);
    sel = {a, b};
}

// ---- edits

size_t SoundDoc::Step::bytes() const {
    size_t n = 0;
    for (auto& s : splices) n += (s.removed.size() + (s.inserted ? s.inserted->size() : 0)) * sizeof(float);
    return n;
}

size_t SoundDoc::historyBytes() const {
    size_t n = 0;
    for (auto& s : undo_) n += s.bytes();
    for (auto& s : redo_) n += s.bytes();
    return n;
}

Sound SoundDoc::spliced(const Sound& s, long at, long n, const float* ins, long insFrames) const {
    const size_t ch = (size_t)s->channels;
    auto out = std::make_shared<SampleBuffer>();
    out->channels = s->channels;
    out->sr = s->sr;
    out->frames = s->frames - n + insFrames;
    out->data.resize((size_t)out->frames * ch);
    float* d = out->data.data();
    const float* x = s->data.data();
    std::copy(x, x + (size_t)at * ch, d);
    if (insFrames > 0) std::copy(ins, ins + (size_t)insFrames * ch, d + (size_t)at * ch);
    std::copy(x + (size_t)(at + n) * ch, x + (size_t)s->frames * ch, d + (size_t)(at + insFrames) * ch);
    return out;
}

bool SoundDoc::needSelection(const char* what) {
    if (bound_ && followBuffer()) return false;          // the buffer~ is not what the selection was made on
    if (!sel.empty()) return true;
    message = std::string(what) + ": select a part of the sound first";
    return false;
}

void SoundDoc::changed(Sound next) {
    sound_ = std::move(next);
    summary_.build(*sound_);
    ++generation_;
    if (bound_)
        if (BufferBox* b = buffer()) {
            SampleBuffer copy = *sound_;                 // made here, swapped in under the lock, the old freed after
            b->replace(std::move(copy));
            seenBox_ = b;
            seenVersion_ = b->version;
            seenWrites_ = b->writes.load(std::memory_order_relaxed);
        }
    if (onChanged) onChanged();
}

void SoundDoc::trimHistory() {
    constexpr size_t kMaxSteps = 200;
    size_t total = historyBytes();
    while (undo_.size() > 1 && (total > undoBudget || undo_.size() > kMaxSteps)) {
        total -= undo_.front().bytes();
        undo_.erase(undo_.begin());
    }
}

void SoundDoc::commit(Step&& st, Sound next) {
    redo_.clear();
    sel = st.selAfter;
    playhead = st.playAfter;
    st.serial = nextSerial_++;
    st.marksBefore = st.marksAfter = marks_;
    if (!marks_->empty()) {                              // the markers move with the frames
        SoundMarks m = *marks_;
        for (auto& sp : st.splices) m.shift(sp.at, sp.removedFrames, sp.insertedFrames);
        if (!(m == *marks_)) st.marksAfter = std::make_shared<const SoundMarks>(std::move(m));
    }
    MarksPtr after = st.marksAfter;
    undo_.push_back(std::move(st));
    trimHistory();
    changed(std::move(next));
    if (after != marks_) setMarksNow(std::move(after));
}

bool SoundDoc::copy() {
    if (!needSelection("Copy")) return false;
    auto c = std::make_shared<SampleBuffer>();
    c->channels = channels();
    c->sr = sr();
    c->frames = sel.length();
    const size_t ch = (size_t)channels();
    c->data.assign(sound_->data.begin() + (long)((size_t)sel.start * ch), sound_->data.begin() + (long)((size_t)sel.end * ch));
    soundClipboard() = c;
    message = "copied " + secs(seconds(sel.length()));
    return true;
}

bool SoundDoc::erase() {
    if (!needSelection("Delete")) return false;
    const size_t ch = (size_t)channels();
    Step st;
    st.name = "Delete";
    Splice sp;
    sp.at = sel.start;
    sp.removedFrames = sel.length();
    sp.removed.assign(sound_->data.begin() + (long)((size_t)sel.start * ch), sound_->data.begin() + (long)((size_t)sel.end * ch));
    st.selBefore = sel;
    st.playBefore = playhead;
    st.selAfter = {sel.start, sel.start};
    st.playAfter = sel.start;
    Sound next = spliced(sound_, sp.at, sp.removedFrames, nullptr, 0);
    st.splices.push_back(std::move(sp));
    message = "deleted " + secs(seconds(sel.length()));
    commit(std::move(st), std::move(next));
    return true;
}

bool SoundDoc::cut() {
    if (!copy()) return false;
    long n = sel.length();
    erase();
    undo_.back().name = "Cut";
    message = "cut " + secs(seconds(n));
    return true;
}

bool SoundDoc::paste() {
    if (bound_ && followBuffer()) return false;
    Sound clip = soundClipboard();
    if (!clip || clip->frames <= 0) {
        message = "Paste: nothing copied yet";
        return false;
    }
    std::shared_ptr<const std::vector<float>> ins;
    const bool other = clip->sr > 0 && sr() > 0 && std::lround(clip->sr) != std::lround(sr());
    if (clip->channels == channels() && !other) {
        ins = std::shared_ptr<const std::vector<float>>(clip, &clip->data);   // shared with the clipboard
    } else {
        std::vector<float> v = clip->channels == channels()
                                   ? clip->data
                                   : edit::convert_channels(clip->data.data(), (size_t)clip->frames, clip->channels, channels());
        if (other) v = resampleFrames(v, channels(), sr() / clip->sr);      // Musil's resample: it keeps its pitch
        ins = std::make_shared<const std::vector<float>>(std::move(v));
    }
    const long insFrames = (long)(ins->size() / (size_t)channels());
    const size_t ch = (size_t)channels();
    Step st;
    st.name = "Paste";
    Splice sp;
    sp.at = sel.empty() ? std::clamp(playhead, 0L, frames()) : sel.start;
    sp.removedFrames = sel.length();
    sp.removed.assign(sound_->data.begin() + (long)((size_t)sp.at * ch),
                      sound_->data.begin() + (long)((size_t)(sp.at + sp.removedFrames) * ch));
    sp.insertedFrames = insFrames;
    sp.inserted = ins;
    st.selBefore = sel;
    st.playBefore = playhead;
    st.selAfter = {sp.at, sp.at + insFrames};
    st.playAfter = sp.at;
    Sound next = spliced(sound_, sp.at, sp.removedFrames, ins->data(), sp.insertedFrames);
    message = "pasted " + secs(clip->sr > 0 ? clip->frames / clip->sr : 0.0);
    if (clip->channels != channels())
        message += " (" + std::to_string(clip->channels) + " channel" + (clip->channels == 1 ? "" : "s") + " made " +
                   std::to_string(channels()) + ")";
    if (other)
        message += " (resampled from " + std::to_string(std::lround(clip->sr)) + " to " + std::to_string(std::lround(sr())) +
                   " Hz)";
    st.splices.push_back(std::move(sp));
    commit(std::move(st), std::move(next));
    return true;
}

bool SoundDoc::trim() {
    if (!needSelection("Trim")) return false;
    if (sel.start == 0 && sel.end == frames()) {
        message = "Trim: the selection is the whole sound";
        return false;
    }
    const size_t ch = (size_t)channels();
    Step st;
    st.name = "Trim";
    st.selBefore = sel;
    st.playBefore = playhead;
    Splice tail, head;                                   // the end first, then the start: positions stay right
    tail.at = sel.end;
    tail.removedFrames = frames() - sel.end;
    tail.removed.assign(sound_->data.begin() + (long)((size_t)sel.end * ch), sound_->data.end());
    head.at = 0;
    head.removedFrames = sel.start;
    head.removed.assign(sound_->data.begin(), sound_->data.begin() + (long)((size_t)sel.start * ch));
    Sound next = spliced(sound_, tail.at, tail.removedFrames, nullptr, 0);
    next = spliced(next, 0, head.removedFrames, nullptr, 0);
    st.selAfter = {0, sel.length()};
    st.playAfter = 0;
    st.splices.push_back(std::move(tail));
    st.splices.push_back(std::move(head));
    message = "trimmed to " + secs(seconds(sel.length()));
    commit(std::move(st), std::move(next));
    return true;
}

bool SoundDoc::process(const std::string& name, std::function<void(std::vector<float>&)> op) {
    const size_t ch = (size_t)channels();
    Step st;
    st.name = name;
    Splice sp;
    sp.at = sel.start;
    sp.removedFrames = sp.insertedFrames = sel.length();
    sp.removed.assign(sound_->data.begin() + (long)((size_t)sel.start * ch), sound_->data.begin() + (long)((size_t)sel.end * ch));
    std::vector<float> made = sp.removed;
    op(made);
    sp.regen = std::move(op);                            // redo makes it again from what it removed
    st.selBefore = st.selAfter = sel;
    st.playBefore = st.playAfter = playhead;
    Sound next = spliced(sound_, sp.at, sp.removedFrames, made.data(), sp.insertedFrames);
    st.splices.push_back(std::move(sp));
    commit(std::move(st), std::move(next));
    return true;
}

bool SoundDoc::silence() {
    if (!needSelection("Silence")) return false;
    message = "silenced " + secs(seconds(sel.length()));
    return process("Silence", [](std::vector<float>& x) { std::fill(x.begin(), x.end(), 0.f); });
}

bool SoundDoc::fadeIn() {
    if (!needSelection("Fade In")) return false;
    const int ch = channels();
    message = "faded in over " + secs(seconds(sel.length()));
    return process("Fade In", [ch](std::vector<float>& x) {
        size_t n = x.size() / (size_t)ch;
        edit::fade_in(x.data(), n, ch, n);
    });
}

bool SoundDoc::fadeOut() {
    if (!needSelection("Fade Out")) return false;
    const int ch = channels();
    message = "faded out over " + secs(seconds(sel.length()));
    return process("Fade Out", [ch](std::vector<float>& x) {
        size_t n = x.size() / (size_t)ch;
        edit::fade_out(x.data(), n, ch, n);
    });
}

bool SoundDoc::gain(double db) {
    if (!needSelection("Gain")) return false;
    if (!std::isfinite(db) || db == 0.0) {
        message = "Gain: 0 dB changes nothing";
        return false;
    }
    const int ch = channels();
    char b[64];
    std::snprintf(b, sizeof b, "gain %+.2f dB", db);
    message = b;
    const double g = edit::db_to_amp(db);
    return process("Gain", [ch, g](std::vector<float>& x) { edit::gain(x.data(), x.size() / (size_t)ch, ch, g); });
}

bool SoundDoc::normalize(double db) {
    if (!needSelection("Normalize")) return false;
    const int ch = channels();
    const size_t ch_ = (size_t)ch;
    double p = edit::peak(sound_->data.data() + (size_t)sel.start * ch_, (size_t)sel.length(), ch);
    if (p == 0) {
        message = "Normalize: the selection is silent";
        return false;
    }
    const double target = edit::db_to_amp(db);
    char b[96];
    std::snprintf(b, sizeof b, "normalized: peak %.2f dB to %.2f dB", 20 * std::log10(p), db);
    message = b;
    return process("Normalize",
                   [ch, target](std::vector<float>& x) { edit::normalize_peak(x.data(), x.size() / (size_t)ch, ch, target); });
}

bool SoundDoc::reverse() {
    if (!needSelection("Reverse")) return false;
    const int ch = channels();
    message = "reversed " + secs(seconds(sel.length()));
    return process("Reverse", [ch](std::vector<float>& x) { edit::reverse(x.data(), x.size() / (size_t)ch, ch); });
}

bool SoundDoc::replaceSelection(const std::string& name, const SampleBuffer& r, bool mixTail) {
    if (bound_ && followBuffer()) return false;          // the buffer~ is not what was processed
    Selection s = sel.empty() ? Selection{0, frames()} : sel;
    const int rc = std::max(1, r.channels), ch = channels();
    std::vector<float> v = r.data;
    v.resize((v.size() / (size_t)rc) * (size_t)rc);
    if (rc != ch) v = edit::convert_channels(v.data(), v.size() / (size_t)rc, rc, ch);
    bool other = r.sr > 0 && sr() > 0 && std::lround(r.sr) != std::lround(sr());
    if (other) v = resampleFrames(v, ch, sr() / r.sr);
    for (auto& x : v)
        if (!std::isfinite(x)) x = 0.f;
    const size_t chs = (size_t)ch;
    long tail = 0;                                       // frames of the result past the selection, mixed over what follows
    if (mixTail && (long)(v.size() / chs) > s.length()) {
        tail = (long)(v.size() / chs) - s.length();
        long over = std::min(tail, frames() - s.end);    // what follows the selection under the tail
        for (size_t k = 0; k < (size_t)over * chs; ++k) v[(size_t)s.length() * chs + k] += sound_->data[(size_t)s.end * chs + k];
        Step st;
        st.name = name;
        Splice sp;
        sp.at = s.start;
        sp.removedFrames = s.length() + over;
        sp.removed.assign(sound_->data.begin() + (long)((size_t)s.start * chs), sound_->data.begin() + (long)((size_t)(s.end + over) * chs));
        sp.insertedFrames = (long)(v.size() / chs);
        sp.inserted = std::make_shared<const std::vector<float>>(std::move(v));
        st.selBefore = sel;
        st.playBefore = playhead;
        st.selAfter = {s.start, s.start + sp.insertedFrames};
        st.playAfter = s.start;
        Sound next = spliced(sound_, sp.at, sp.removedFrames, sp.inserted->data(), sp.insertedFrames);
        message = name + ": " + secs(seconds(s.length())) + " processed, its tail of " + secs(seconds(tail)) + " mixed over what follows" +
                  (sp.insertedFrames > sp.removedFrames ? " (the sound grew by " + secs(seconds(sp.insertedFrames - sp.removedFrames)) + ")" : "");
        st.splices.push_back(std::move(sp));
        commit(std::move(st), std::move(next));
        return true;
    }
    Step st;
    st.name = name;
    Splice sp;
    sp.at = s.start;
    sp.removedFrames = s.length();
    sp.removed.assign(sound_->data.begin() + (long)((size_t)s.start * chs), sound_->data.begin() + (long)((size_t)s.end * chs));
    sp.insertedFrames = (long)(v.size() / chs);
    sp.inserted = std::make_shared<const std::vector<float>>(std::move(v));
    st.selBefore = sel;
    st.playBefore = playhead;
    st.selAfter = {s.start, s.start + sp.insertedFrames};
    st.playAfter = s.start;
    Sound next = spliced(sound_, sp.at, sp.removedFrames, sp.inserted->data(), sp.insertedFrames);
    message = sp.insertedFrames == s.length() ? name + ": " + secs(seconds(s.length())) + " processed"
                                              : name + ": " + secs(seconds(s.length())) + " became " + secs(seconds(sp.insertedFrames));
    st.splices.push_back(std::move(sp));
    commit(std::move(st), std::move(next));
    return true;
}

bool SoundDoc::replaceFrames(const std::string& name, long at, const std::vector<float>& f) {
    if (bound_ && followBuffer()) return false;
    const size_t ch = (size_t)channels();
    long n = (long)(f.size() / ch);
    if (n <= 0 || at < 0 || at + n > frames()) {
        message = name + ": outside the sound";
        return false;
    }
    Step st;
    st.name = name;
    Splice sp;
    sp.at = at;
    sp.removedFrames = sp.insertedFrames = n;
    sp.removed.assign(sound_->data.begin() + (long)((size_t)at * ch), sound_->data.begin() + (long)((size_t)(at + n) * ch));
    sp.inserted = std::make_shared<const std::vector<float>>(f.begin(), f.begin() + (long)((size_t)n * ch));
    st.selBefore = st.selAfter = sel;
    st.playBefore = st.playAfter = playhead;
    Sound next = spliced(sound_, at, n, sp.inserted->data(), n);
    st.splices.push_back(std::move(sp));
    message = name + ": " + std::to_string(n) + " sample" + (n == 1 ? "" : "s");
    commit(std::move(st), std::move(next));
    return true;
}

bool SoundDoc::undo() {
    if (undo_.empty()) {
        message = "nothing to undo";
        return false;
    }
    if (bound_ && followBuffer()) return false;
    Step st = std::move(undo_.back());
    undo_.pop_back();
    Sound cur = sound_;
    for (auto it = st.splices.rbegin(); it != st.splices.rend(); ++it)
        cur = spliced(cur, it->at, it->insertedFrames, it->removed.data(), it->removedFrames);
    sel = st.selBefore;
    playhead = st.playBefore;
    message = "undid " + st.name;
    MarksPtr m = st.marksBefore;
    bool samples = !st.splices.empty();
    redo_.push_back(std::move(st));
    if (samples) changed(std::move(cur));
    if (m && m != marks_) setMarksNow(std::move(m));
    return true;
}

bool SoundDoc::redo() {
    if (redo_.empty()) {
        message = "nothing to redo";
        return false;
    }
    if (bound_ && followBuffer()) return false;
    Step st = std::move(redo_.back());
    redo_.pop_back();
    Sound cur = sound_;
    for (auto& sp : st.splices) {
        if (sp.inserted) {
            cur = spliced(cur, sp.at, sp.removedFrames, sp.inserted->data(), sp.insertedFrames);
        } else if (sp.regen) {
            std::vector<float> made = sp.removed;
            sp.regen(made);
            cur = spliced(cur, sp.at, sp.removedFrames, made.data(), sp.insertedFrames);
        } else {
            cur = spliced(cur, sp.at, sp.removedFrames, nullptr, 0);
        }
    }
    sel = st.selAfter;
    playhead = st.playAfter;
    message = "redid " + st.name;
    MarksPtr m = st.marksAfter;
    bool samples = !st.splices.empty();
    undo_.push_back(std::move(st));
    if (samples) changed(std::move(cur));
    if (m && m != marks_) setMarksNow(std::move(m));
    return true;
}

}  // namespace moritz
