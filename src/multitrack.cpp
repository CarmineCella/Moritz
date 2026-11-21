// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// multitrack.cpp — the strips' values, the track pool, streaming from disk, overviews and recording (multitrack.h)

#include "multitrack.h"
#include "fileio.h"
#include "miniaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <semaphore.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif

namespace moritz {
namespace mt {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- strips

StripParams::StripParams() {
    for (auto& s : send) s.store(0.f, std::memory_order_relaxed);
    for (int c = 0; c < 2; ++c) {
        peak[c].store(0.f, std::memory_order_relaxed);
        rms[c].store(0.f, std::memory_order_relaxed);
    }
}

namespace {
std::string wordOf(const sc::Fields& f, const char* key) {
    const sc::Datum* d = sc::field(f, key);
    return d && (d->kind == sc::Datum::Str || d->kind == sc::Datum::Sym) ? d->s : std::string();
}
void putNum(sc::Fields& f, const char* key, double v, double def) {
    if (v == def) sc::eraseField(f, key);
    else sc::setField(f, key, sc::Datum::number(v));
}
void putWord(sc::Fields& f, const char* key, const std::string& v) {
    if (v.empty()) sc::eraseField(f, key);
    else sc::setField(f, key, sc::Datum::string(v));
}
}  // namespace

RowSettings rowSettings(const sc::Row& r) {
    RowSettings s;
    s.out = wordOf(r.extra, "out");
    s.bus = sc::numField(r.extra, "bus", 0) != 0;
    if (const sc::Datum* d = sc::field(r.extra, "sends"); d && d->kind == sc::Datum::List)
        for (size_t k = 0; k + 1 < d->list.size() && s.sends.size() < (size_t)kMaxSends; k += 2) {
            const sc::Datum &n = d->list[k], &g = d->list[k + 1];
            if ((n.kind == sc::Datum::Str || n.kind == sc::Datum::Sym) && g.kind == sc::Datum::Num && std::isfinite(g.num))
                s.sends.push_back({n.s, std::max(0.0, g.num)});
        }
    s.insert = wordOf(r.extra, "insert");
    s.instrument = wordOf(r.extra, "instrument");
    s.input = std::clamp((int)sc::numField(r.extra, "input", 0), 0, 64);
    s.inputCh = std::clamp((int)sc::numField(r.extra, "input-ch", 1), 1, 2);
    s.arm = sc::numField(r.extra, "arm", 0) != 0;
    s.monitor = sc::numField(r.extra, "monitor", 0) != 0;
    return s;
}

void setRowSettings(sc::Row& r, const RowSettings& s) {
    putWord(r.extra, "out", s.out);
    putNum(r.extra, "bus", s.bus ? 1 : 0, 0);
    if (s.sends.empty()) sc::eraseField(r.extra, "sends");
    else {
        std::vector<sc::Datum> l;
        for (auto& x : s.sends) {
            l.push_back(sc::Datum::string(x.bus));
            l.push_back(sc::Datum::number(x.gain));
        }
        sc::setField(r.extra, "sends", sc::Datum::of(std::move(l)));
    }
    putWord(r.extra, "insert", s.insert);
    putWord(r.extra, "instrument", s.instrument);
    putNum(r.extra, "input", s.input, 0);
    putNum(r.extra, "input-ch", s.inputCh, 1);
    putNum(r.extra, "arm", s.arm ? 1 : 0, 0);
    putNum(r.extra, "monitor", s.monitor ? 1 : 0, 0);
}

SessionSettings sessionSettings(const sc::Score& s) {
    SessionSettings v;
    v.masterGain = std::max(0.0, sc::numField(s.extra, "master-gain", 1));
    v.masterInsert = wordOf(s.extra, "master-insert");
    v.countIn = std::clamp((int)sc::numField(s.extra, "count-in", 0), 0, 16);
    v.punch = sc::numField(s.extra, "punch", 0) != 0;
    v.punchIn = std::max(0.0, sc::numField(s.extra, "punch-in", 0));
    v.punchOut = std::max(0.0, sc::numField(s.extra, "punch-out", 0));
    v.latency = std::clamp((int)sc::numField(s.extra, "latency", 0), -100000, 100000);
    v.crossfade = sc::numField(s.extra, "auto-crossfade", 0) != 0;
    return v;
}

void setSessionSettings(sc::Score& s, const SessionSettings& v) {
    putNum(s.extra, "master-gain", v.masterGain, 1);
    putWord(s.extra, "master-insert", v.masterInsert);
    putNum(s.extra, "count-in", v.countIn, 0);
    putNum(s.extra, "punch", v.punch ? 1 : 0, 0);
    putNum(s.extra, "punch-in", v.punchIn, 0);
    putNum(s.extra, "punch-out", v.punchOut, 0);
    putNum(s.extra, "latency", v.latency, 0);
    putNum(s.extra, "auto-crossfade", v.crossfade ? 1 : 0, 0);
}

void loadStrip(StripParams& p, const sc::Row& r) {
    sc::Strip st = sc::stripOf(r);
    RowSettings rs = rowSettings(r);
    p.gain.store((float)st.gain, std::memory_order_relaxed);
    p.az.store((float)st.az, std::memory_order_relaxed);
    p.mute.store(st.mute, std::memory_order_relaxed);
    p.solo.store(st.solo, std::memory_order_relaxed);
    p.monitor.store(rs.monitor, std::memory_order_relaxed);
    for (int k = 0; k < kMaxSends; ++k)
        p.send[k].store(k < (int)rs.sends.size() ? (float)rs.sends[(size_t)k].gain : 0.f, std::memory_order_relaxed);
}

std::string dbText(double gain) {
    if (!(gain > 1e-6)) return "-inf";
    char b[32];
    double db = 20 * std::log10(gain);
    std::snprintf(b, sizeof b, "%.1f", std::fabs(db) < 0.05 ? 0.0 : db);
    return b;
}
double dbToGain(double db) { return db <= -120 ? 0.0 : std::pow(10.0, db / 20); }
double gainToDb(double gain) { return gain > 1e-6 ? 20 * std::log10(gain) : -120.0; }

// ---------------------------------------------------------------- the track pool

namespace {
inline void cpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}
}  // namespace

struct TrackPool::Sem {
#if defined(__APPLE__)
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    void post() { dispatch_semaphore_signal(s); }
    void wait() { dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER); }
#elif defined(_WIN32)
    HANDLE h = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    void post() { ReleaseSemaphore(h, 1, nullptr); }
    void wait() { WaitForSingleObject(h, INFINITE); }
#else
    sem_t s;
    Sem() { sem_init(&s, 0, 0); }
    void post() { sem_post(&s); }
    void wait() { while (sem_wait(&s) != 0 && errno == EINTR) {} }
#endif
};

std::atomic<int> TrackPool::threads{0};
std::atomic<int> TrackPool::minTracks{4};

TrackPool& TrackPool::get() {
    static TrackPool* pool = new TrackPool();      // never destroyed: its workers may outlive main's end
    return *pool;
}

TrackPool::TrackPool() : sem_(new Sem) {
    unsigned hw = std::thread::hardware_concurrency();
    int n = std::clamp((int)hw - 1, 0, 7);
    for (int k = 0; k < n; ++k) {
        threads_.emplace_back([this] { worker(); });
        threads_.back().detach();
    }
}

namespace {
thread_local bool tlPoolWorker = false;            // a worker, or a caller inside run(): nested jobs run alone
}

bool TrackPool::help(uint64_t t, bool& ran) {
    ran = false;
    uint32_t gen = (uint32_t)(t >> 32), idx = (uint32_t)t;
    Job& j = jobs2_[gen & 1];
    int n = j.n.load(std::memory_order_acquire);
    if ((int)idx >= n) return false;
    if (!ticket_.compare_exchange_strong(t, t + 1, std::memory_order_acq_rel)) return true;   // look again
    auto f = j.f.load(std::memory_order_acquire);
    void* ctx = j.ctx.load(std::memory_order_acquire);
    f(ctx, (int)idx);
    j.done.fetch_add(1, std::memory_order_acq_rel);
    ran = true;
    return true;
}

void TrackPool::run(int n, void (*f)(void*, int), void* ctx) {
    int want = threads.load(std::memory_order_relaxed);
    bool alone = n <= 1 || threads_.empty() || want == 1 || tlPoolWorker;
    bool expected = false;
    if (!alone && !busy_.compare_exchange_strong(expected, true, std::memory_order_acquire)) alone = true;
    if (alone) {
        for (int k = 0; k < n; ++k) f(ctx, k);
        return;
    }
    tlPoolWorker = true;
    uint64_t gen = (ticket_.load(std::memory_order_acquire) >> 32) + 1;
    Job& j = jobs2_[gen & 1];
    AudioContext ac = audioContext();
    j.f.store(f, std::memory_order_relaxed);
    j.ctx.store(ctx, std::memory_order_relaxed);
    j.blockStart.store(ac.blockStart, std::memory_order_relaxed);
    j.buffers.store(ac.buffers, std::memory_order_relaxed);
    j.done.store(0, std::memory_order_relaxed);
    j.n.store(n, std::memory_order_release);
    ticket_.store(gen << 32, std::memory_order_release);
    jobs_.fetch_add(1, std::memory_order_relaxed);
    int most = want > 1 ? want - 1 : (int)threads_.size();
    int wake = std::min({sleepers_.load(std::memory_order_acquire), n - 1, most});
    for (int k = 0; k < wake; ++k) sem_->post();
    // the caller takes items too
    for (;;) {
        uint64_t t = ticket_.load(std::memory_order_acquire);
        bool ran;
        if ((t >> 32) != gen || !help(t, ran)) break;
    }
    while (j.done.load(std::memory_order_acquire) < n) cpuRelax();
    tlPoolWorker = false;
    busy_.store(false, std::memory_order_release);
}

void TrackPool::worker() {
    tlPoolWorker = true;
    using clk = std::chrono::steady_clock;
    auto last = clk::now();
    for (;;) {
        uint64_t t = ticket_.load(std::memory_order_acquire);
        Job& j = jobs2_[(uint32_t)(t >> 32) & 1];
        if ((int)(uint32_t)t < j.n.load(std::memory_order_acquire)) {
            AudioContext ac{j.blockStart.load(std::memory_order_relaxed), j.buffers.load(std::memory_order_relaxed)};
            WorkerScope ws(ac);
            bool ran = false;
            help(t, ran);
            if (ran) helped_.fetch_add(1, std::memory_order_relaxed);
            last = clk::now();
            continue;
        }
        // nothing: spin a little (the next block comes soon within a callback), then sleep
        if (clk::now() - last < std::chrono::microseconds(400)) {
            for (int k = 0; k < 64; ++k) cpuRelax();
            continue;
        }
        sleepers_.fetch_add(1, std::memory_order_acq_rel);
        if (ticket_.load(std::memory_order_acquire) == t) sem_->wait();
        sleepers_.fetch_sub(1, std::memory_order_acq_rel);
        last = clk::now();
    }
}

// ---------------------------------------------------------------- streams

std::atomic<double> streamThreshold{30.0};
std::atomic<int> DiskStream::testDelayMs{0};

struct DiskStream::Dec {
    ma_decoder d;
};

std::shared_ptr<DiskStream> DiskStream::open(const std::string& path, std::string& err) {
    auto s = std::shared_ptr<DiskStream>(new DiskStream());
    s->dec_ = std::make_unique<Dec>();
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_file(path.c_str(), &cfg, &s->dec_->d) != MA_SUCCESS) {
        s->dec_.reset();
        err = "can't read audio file " + path;
        return nullptr;
    }
    ma_uint64 len = 0;
    if (ma_decoder_get_length_in_pcm_frames(&s->dec_->d, &len) != MA_SUCCESS || len == 0) {
        ma_decoder_uninit(&s->dec_->d);
        s->dec_.reset();
        err = "can't tell the length of " + path;
        return nullptr;
    }
    s->path_ = path;
    s->channels_ = (int)std::max<ma_uint32>(1, s->dec_->d.outputChannels);
    s->sr_ = s->dec_->d.outputSampleRate;
    s->frames_ = (int64_t)len;
    size_t n = (size_t)(kChunk + 1) * (size_t)s->channels_;
    for (auto& sl : s->slots_) sl.data.assign(n, 0.f);
    for (auto& sl : s->pins_) sl.data.assign(n, 0.f);
    s->scratch_.assign(n, 0.f);
    DiskReader::get().add(s);
    return s;
}

DiskStream::~DiskStream() {
    if (dec_) ma_decoder_uninit(&dec_->d);
}

// state of a slot: its chunk + 1, shifted by 2, and two bits: 1 the audio reads it, 2 the reader fills it
namespace {
constexpr int64_t kReading = 1, kFilling = 2;
inline int64_t readyState(int64_t chunk) { return (chunk + 1) << 2; }
inline int64_t chunkOfState(int64_t s) { return (s >> 2) - 1; }
}  // namespace

bool DiskStream::fill(Slot& s, int64_t c) {
    // the slot is ours (Filling): nobody reads it
    if (int ms = testDelayMs.load(std::memory_order_relaxed); ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    int64_t first = c * kChunk;
    ma_uint64 got = 0;
    {
        std::lock_guard<std::mutex> lk(decMutex_);
        if (!dec_ || ma_decoder_seek_to_pcm_frame(&dec_->d, (ma_uint64)first) != MA_SUCCESS) return false;
        ma_decoder_read_pcm_frames(&dec_->d, s.data.data(), (ma_uint64)(kChunk + 1), &got);
    }
    if ((int64_t)got < kChunk + 1)
        std::fill(s.data.begin() + (long)got * channels_, s.data.end(), 0.f);
    reads_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

DiskStream::Slot* DiskStream::find(int64_t c) {
    int np = npins_.load(std::memory_order_acquire);
    for (int k = 0; k < np; ++k)
        if (chunkOfState(pins_[k].chunk.load(std::memory_order_acquire)) == c) return &pins_[k];
    return &slots_[c % kSlots];
}

void DiskStream::pin(int64_t frame) {
    if (frame < 0 || frame >= frames_) return;
    int64_t c0 = frame / kChunk;
    for (int64_t c = c0; c <= c0 + 1 && c * kChunk < frames_; ++c) {
        int np = npins_.load(std::memory_order_acquire);
        bool have = false;
        for (int k = 0; k < np; ++k) have = have || chunkOfState(pins_[k].chunk.load(std::memory_order_acquire) & ~3LL) == c;
        if (have) continue;
        // a free pin, else the oldest after the first two (the region's start stays)
        int k = np < kPins ? np : 2 + (int)((c / 1) % 2);
        Slot& s = pins_[k];
        int64_t st = s.chunk.load(std::memory_order_acquire);
        if (k < np) {
            if (st & kReading) continue;                       // the audio reads it now: next time
            if (!s.chunk.compare_exchange_strong(st, st | kFilling)) continue;
        } else {
            s.chunk.store(kFilling, std::memory_order_release);
        }
        if (!fill(s, c)) { s.chunk.store(0, std::memory_order_release); continue; }
        s.chunk.store(readyState(c), std::memory_order_release);
        if (k >= np) npins_.store(np + 1, std::memory_order_release);
    }
}

void DiskStream::hint(int64_t frame) {
    hint_.store(std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, frames_ - 1)), std::memory_order_relaxed);
    DiskReader::get().wake();
}

bool DiskStream::view(int64_t frame, View& v) {
    if (frame < 0 || frame >= frames_) return false;
    int64_t c = frame / kChunk;
    Slot* s = find(c);
    int64_t ready = readyState(c);
    int64_t st = s->chunk.load(std::memory_order_acquire);
    if (st != ready || !s->chunk.compare_exchange_strong(st, ready | kReading, std::memory_order_acq_rel)) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        want_.store(frame, std::memory_order_relaxed);
        return false;
    }
    v.data = s->data.data();
    v.lo = c * kChunk;
    v.hi = std::min(c * kChunk + kChunk, frames_ - 1);
    v.slot = s;
    v.state = ready;
    return true;
}

void DiskStream::release(View& v) {
    if (!v.slot) return;
    static_cast<Slot*>(v.slot)->chunk.store(v.state, std::memory_order_release);
    v.slot = nullptr;
    v.data = nullptr;
}

bool DiskStream::serve() {
    int64_t h = hint_.exchange(-1, std::memory_order_relaxed);
    int64_t w = h >= 0 ? h : want_.load(std::memory_order_relaxed);
    if (w < 0) return false;
    if (h >= 0) want_.store(h, std::memory_order_relaxed);
    int64_t c0 = w / kChunk, last = (frames_ - 1) / kChunk;
    for (int64_t c = c0; c <= std::min(last, c0 + kSlots - 3); ++c) {
        Slot* pinned = nullptr;
        int np = npins_.load(std::memory_order_acquire);
        for (int k = 0; k < np; ++k)
            if (chunkOfState(pins_[k].chunk.load(std::memory_order_acquire) & ~3LL) == c) pinned = &pins_[k];
        if (pinned) continue;
        Slot& s = slots_[c % kSlots];
        int64_t st = s.chunk.load(std::memory_order_acquire);
        if ((st & ~3LL) == readyState(c)) continue;                          // there
        if (st & (kReading | kFilling)) continue;                             // busy: later
        if (!s.chunk.compare_exchange_strong(st, kFilling, std::memory_order_acq_rel)) continue;
        if (!fill(s, c)) { s.chunk.store(0, std::memory_order_release); return false; }
        s.chunk.store(readyState(c), std::memory_order_release);
        return true;                                                          // one chunk at a time: the others too
    }
    return false;
}

// ---------------------------------------------------------------- the disk reader

bool DiskStream::filling() const {
    for (auto& sl : slots_)
        if (sl.chunk.load(std::memory_order_acquire) & kFilling) return true;
    return false;
}

DiskReader& DiskReader::get() {
    static DiskReader* r = new DiskReader();       // never destroyed, as the track pool
    return *r;
}

DiskReader::DiskReader() {
    thread_ = std::thread([this] { loop(); });
    thread_.detach();
}

void DiskReader::add(const std::shared_ptr<DiskStream>& s) {
    std::lock_guard<std::mutex> lk(m_);
    streams_.erase(std::remove_if(streams_.begin(), streams_.end(), [](const std::weak_ptr<DiskStream>& w) { return w.expired(); }),
                   streams_.end());
    streams_.push_back(s);
}

void DiskReader::wake() { poke_.store(true, std::memory_order_release); }

void DiskReader::flush() {
    for (int guard = 0; guard < 100000; ++guard) {
        std::vector<std::shared_ptr<DiskStream>> live;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto& w : streams_)
                if (auto s = w.lock()) live.push_back(s);
        }
        bool any = false, pending = false;
        for (auto& s : live) {
            any = s->serve() || any;
            pending = pending || s->filling();
        }
        if (!any && !pending) return;
        if (!any) std::this_thread::sleep_for(std::chrono::microseconds(100));   // the reader's thread finishes one
    }
}

void DiskReader::loop() {
    std::vector<std::shared_ptr<DiskStream>> live;
    for (;;) {
        live.clear();
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto& w : streams_)
                if (auto s = w.lock()) live.push_back(std::move(s));
        }
        bool any = false;
        for (auto& s : live) any = s->serve() || any;
        live.clear();                                  // a stream let go of elsewhere ends here
        if (any) continue;
        for (int k = 0; k < 20 && !poke_.exchange(false, std::memory_order_acq_rel); ++k)
            std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
}

double fileSeconds(const std::string& path) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder d;
    if (ma_decoder_init_file(path.c_str(), &cfg, &d) != MA_SUCCESS) return -1;
    ma_uint64 len = 0;
    double sec = -1;
    if (ma_decoder_get_length_in_pcm_frames(&d, &len) == MA_SUCCESS && d.outputSampleRate > 0)
        sec = (double)len / d.outputSampleRate;
    ma_decoder_uninit(&d);
    return sec;
}

// ---------------------------------------------------------------- overviews

void Peaks::range(int c, long a, long b, float& lo, float& hi) const {
    lo = hi = 0.f;
    if (c < 0 || c >= channels || b <= a) return;
    long nb = (long)(mn.size() / (size_t)std::max(1, channels));
    long i0 = std::clamp(a / kBlock, 0L, nb), i1 = std::clamp((b + kBlock - 1) / kBlock, i0, nb);
    if (i1 <= i0) return;
    lo = 1e30f;
    hi = -1e30f;
    for (long i = i0; i < i1; ++i) {
        lo = std::min(lo, mn[(size_t)i * channels + c]);
        hi = std::max(hi, mx[(size_t)i * channels + c]);
    }
}

bool computePeaks(const std::string& path, Peaks& out, std::string& err) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder d;
    if (ma_decoder_init_file(path.c_str(), &cfg, &d) != MA_SUCCESS) {
        err = "can't read audio file " + path;
        return false;
    }
    Peaks p;
    p.channels = (int)std::max<ma_uint32>(1, d.outputChannels);
    p.sr = d.outputSampleRate;
    std::vector<float> buf((size_t)Peaks::kBlock * 64 * p.channels);
    long frames = 0;
    for (;;) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&d, buf.data(), (ma_uint64)(Peaks::kBlock * 64), &got);
        for (ma_uint64 f = 0; f < got; ++f, ++frames) {
            if (frames % Peaks::kBlock == 0) {
                p.mn.insert(p.mn.end(), (size_t)p.channels, 1e30f);
                p.mx.insert(p.mx.end(), (size_t)p.channels, -1e30f);
            }
            size_t b = p.mn.size() - (size_t)p.channels;
            for (int c = 0; c < p.channels; ++c) {
                float v = buf[(size_t)f * p.channels + c];
                p.mn[b + c] = std::min(p.mn[b + c], v);
                p.mx[b + c] = std::max(p.mx[b + c], v);
            }
        }
        if (r != MA_SUCCESS || got == 0) break;
    }
    ma_decoder_uninit(&d);
    p.frames = frames;
    out = std::move(p);
    return true;
}

namespace {
constexpr char kPeaksMagic[8] = {'M', 'Z', 'P', 'E', 'A', 'K', 'S', '1'};
struct PeaksHeader {
    char magic[8];
    uint64_t fileSize, fileTime;
    int32_t channels, block;
    double sr;
    int64_t frames, blocks;
};
bool fileStamp(const std::string& path, uint64_t& size, uint64_t& time) {
    std::error_code ec;
    auto sz = fs::file_size(fs::u8path(path), ec);
    if (ec) return false;
    auto t = fs::last_write_time(fs::u8path(path), ec);
    if (ec) return false;
    size = (uint64_t)sz;
    time = (uint64_t)t.time_since_epoch().count();
    return true;
}
}  // namespace

bool loadPeaks(const std::string& path, Peaks& out) {
    std::string data;
    uint64_t size, time;
    if (!fileStamp(path, size, time) || !fileio::read(path + ".peaks", data) || data.size() < sizeof(PeaksHeader)) return false;
    PeaksHeader h;
    std::memcpy(&h, data.data(), sizeof h);
    if (std::memcmp(h.magic, kPeaksMagic, 8) != 0 || h.fileSize != size || h.fileTime != time || h.block != Peaks::kBlock ||
        h.channels < 1 || h.channels > 64 || h.blocks < 0 || h.frames < 0)
        return false;
    size_t n = (size_t)h.blocks * (size_t)h.channels;
    if (h.blocks > (int64_t)1 << 32 || data.size() != sizeof h + 2 * n * sizeof(float)) return false;
    Peaks p;
    p.channels = h.channels;
    p.sr = h.sr;
    p.frames = (long)h.frames;
    p.mn.resize(n);
    p.mx.resize(n);
    std::memcpy(p.mn.data(), data.data() + sizeof h, n * sizeof(float));
    std::memcpy(p.mx.data(), data.data() + sizeof h + n * sizeof(float), n * sizeof(float));
    out = std::move(p);
    return true;
}

bool savePeaks(const std::string& path, const Peaks& p) {
    PeaksHeader h{};
    std::memcpy(h.magic, kPeaksMagic, 8);
    if (!fileStamp(path, h.fileSize, h.fileTime)) return false;
    h.channels = p.channels;
    h.block = Peaks::kBlock;
    h.sr = p.sr;
    h.frames = p.frames;
    h.blocks = (int64_t)(p.mn.size() / (size_t)std::max(1, p.channels));
    std::string data(sizeof h + 2 * p.mn.size() * sizeof(float), '\0');
    std::memcpy(&data[0], &h, sizeof h);
    std::memcpy(&data[sizeof h], p.mn.data(), p.mn.size() * sizeof(float));
    std::memcpy(&data[sizeof h + p.mn.size() * sizeof(float)], p.mx.data(), p.mx.size() * sizeof(float));
    return fileio::writeAtomic(path + ".peaks", data);
}

// ---------------------------------------------------------------- recording

Recording::Recording(std::vector<std::unique_ptr<Take>> takes, int64_t from, int64_t to, double sr)
    : takes_(std::move(takes)), from_(from), to_(to), sr_(sr) {
    for (auto& t : takes_) {
        t->channels = std::clamp(t->channels, 1, 2);
        t->ring.assign((size_t)(sr_ * 10) * (size_t)t->channels + 1, 0.f);   // ten seconds of slack for the disk
    }
}

Recording::~Recording() { stop(); }

bool Recording::start(std::string& err) {
    for (auto& t : takes_) {
        std::error_code ec;
        fs::create_directories(fs::u8path(dirOf(t->path)), ec);
        if (!t->file.open(t->path, t->channels, (int)std::lround(sr_), 24, &err)) return false;
    }
    quit_ = false;
    on_.store(true, std::memory_order_release);
    writer_ = std::thread([this] {
        while (!quit_.load(std::memory_order_acquire)) {
            drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    return true;
}

void Recording::push(const float* in, int inCh, int n, const int64_t* pos, const bool* playing) {
    struct In {
        std::atomic<int>& c;
        explicit In(std::atomic<int>& x) : c(x) { c.fetch_add(1, std::memory_order_seq_cst); }
        ~In() { c.fetch_sub(1, std::memory_order_release); }
    } here(pushing);
    if (!on_.load(std::memory_order_seq_cst)) return;
    for (auto& tp : takes_) {
        Take& t = *tp;
        size_t N = t.ring.size(), h = t.head.load(std::memory_order_relaxed), tl = t.tail.load(std::memory_order_acquire);
        long long pushed = 0;
        for (int i = 0; i < n; ++i) {
            if (!playing[i] || pos[i] < from_ || (to_ > from_ && pos[i] >= to_)) continue;
            size_t free = (tl + N - h - 1) % N;
            if (free < (size_t)t.channels) { overflow_.store(true, std::memory_order_relaxed); break; }
            if (t.firstPos.load(std::memory_order_relaxed) < 0) t.firstPos.store(pos[i], std::memory_order_relaxed);
            for (int c = 0; c < t.channels; ++c) {
                int ch = t.input + c;
                t.ring[h] = in && ch < inCh ? in[(size_t)i * (size_t)inCh + (size_t)ch] : 0.f;
                h = h + 1 == N ? 0 : h + 1;
            }
            ++pushed;
        }
        t.head.store(h, std::memory_order_release);
        if (pushed) t.frames.fetch_add(pushed, std::memory_order_relaxed);
    }
}

void Recording::drain() {
    for (auto& tp : takes_) {
        Take& t = *tp;
        if (!t.file.isOpen()) continue;
        size_t N = t.ring.size(), tl = t.tail.load(std::memory_order_relaxed), h = t.head.load(std::memory_order_acquire);
        chunk_.clear();
        while (tl != h) {
            chunk_.push_back(t.ring[tl]);
            tl = tl + 1 == N ? 0 : tl + 1;
        }
        t.tail.store(tl, std::memory_order_release);
        size_t frames = chunk_.size() / (size_t)t.channels;
        if (frames) {
            t.file.write(chunk_.data(), frames);
            t.written += (long long)frames;
        }
    }
}

void Recording::stop() {
    on_.store(false, std::memory_order_seq_cst);
    while (pushing.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
    if (writer_.joinable()) {
        quit_.store(true, std::memory_order_release);
        writer_.join();
    }
    drain();
}

bool Recording::finish(std::string& err) {
    stop();
    bool ok = true;
    for (auto& t : takes_) {
        if (!t->file.isOpen()) continue;
        if (t->written == 0) continue;               // nothing: its temporary file goes with the writer
        std::string e;
        if (!t->file.finish(&e)) {
            ok = false;
            err = e;
        }
    }
    return ok;
}

std::string takePath(const std::string& folder, const std::string& track) {
    std::string name;
    for (char c : track.empty() ? std::string("track") : track)
        name += (c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' ||
                 (unsigned char)c < ' ')
                    ? '_'
                    : c;
    for (int n = 1; n < 100000; ++n) {
        std::string p = folder + "/" + name + " take " + std::to_string(n) + ".wav";
        if (!fileio::exists(p) && !fileio::exists(p + ".part")) return p;
    }
    return folder + "/" + name + " take.wav";
}

std::string mediaFolder(const std::string& scorePath) {
    fs::path p = fs::u8path(scorePath);
    return (p.parent_path() / fs::u8path(p.stem().u8string() + "_media")).u8string();
}

void placeTake(int64_t firstPos, int64_t latency, double sr, double& at, double& from) {
    int64_t start = firstPos - latency, skip = 0;
    if (start < 0) {
        skip = -start;
        start = 0;
    }
    // seconds that the plan's floor(seconds * rate) turns back into these very samples
    auto exact = [sr](int64_t s) {
        double t = (double)s / sr;
        while ((int64_t)std::floor(t * sr) < s) t = std::nextafter(t, 1e300);
        while (t > 0 && (int64_t)std::floor(t * sr) > s) t = std::nextafter(t, -1e300);
        return t;
    };
    at = exact(start);
    from = exact(skip);
}

}  // namespace mt
}  // namespace moritz
