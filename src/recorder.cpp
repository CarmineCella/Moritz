// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// recorder.cpp — WavWriter and the quick recorder (recorder.h)

#include "recorder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace moritz {

// ---------------------------------------------------------------- WavWriter

namespace {
void put16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
void put32(unsigned char* p, uint32_t v) {
    for (int k = 0; k < 4; ++k) p[k] = (unsigned char)(v >> (8 * k));
}
// the 44-byte header of a PCM file with dataBytes of samples
void header(unsigned char* h, int channels, int sr, int bits, uint32_t dataBytes) {
    int bps = bits / 8;
    std::copy_n("RIFF", 4, h);
    put32(h + 4, 36 + dataBytes);
    std::copy_n("WAVEfmt ", 8, h + 8);
    put32(h + 16, 16);
    put16(h + 20, 1);                                   // PCM
    put16(h + 22, (uint32_t)channels);
    put32(h + 24, (uint32_t)sr);
    put32(h + 28, (uint32_t)(sr * channels * bps));
    put16(h + 32, (uint32_t)(channels * bps));
    put16(h + 34, (uint32_t)bits);
    std::copy_n("data", 4, h + 36);
    put32(h + 40, dataBytes);
}
}  // namespace

WavWriter::~WavWriter() {
    if (!f_) return;
    std::fclose(f_);
    std::remove(temp_.c_str());
}

bool WavWriter::open(const std::string& path, int channels, int sr, int bits, std::string* err) {
    if (f_) { std::fclose(f_); std::remove(temp_.c_str()); f_ = nullptr; }
    path_ = path;
    temp_ = path + ".part";                             // in the same folder: the rename is atomic
    channels_ = std::max(1, channels);
    sr_ = std::max(1, sr);
    bits_ = bits == 24 ? 24 : 16;
    frames_ = bytes_ = 0;
    ok_ = true;
    f_ = std::fopen(temp_.c_str(), "wb");
    if (!f_) {
        if (err) *err = "can't write " + path;
        return false;
    }
    unsigned char h[44];
    header(h, channels_, sr_, bits_, 0);
    ok_ = std::fwrite(h, 1, 44, f_) == 44;
    return true;
}

bool WavWriter::write(const float* x, size_t frames) {
    if (!f_) return false;
    const int bps = bits_ / 8;
    size_t n = frames * (size_t)channels_;
    buf_.resize(n * (size_t)bps);
    unsigned char* p = buf_.data();
    const double scale = bits_ == 24 ? 8388607.0 : 32767.0;
    for (size_t i = 0; i < n; ++i, p += bps) {
        float v = std::isfinite(x[i]) ? std::clamp(x[i], -1.f, 1.f) : 0.f;
        int32_t s = (int32_t)std::lround(v * scale);
        p[0] = (unsigned char)(s & 0xff);
        p[1] = (unsigned char)((s >> 8) & 0xff);
        if (bps == 3) p[2] = (unsigned char)((s >> 16) & 0xff);
    }
    if (std::fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size()) ok_ = false;
    frames_ += (long long)frames;
    bytes_ += (long long)buf_.size();
    return ok_;
}

bool WavWriter::finish(std::string* err) {
    if (!f_) return false;
    unsigned char h[44];
    header(h, channels_, sr_, bits_, (uint32_t)std::min<long long>(bytes_, 0xFFFFFFD0LL));
    if (bytes_ & 1) ok_ = ok_ && std::fputc(0, f_) != EOF;     // chunks are padded to even sizes
    ok_ = ok_ && std::fflush(f_) == 0 && std::fseek(f_, 0, SEEK_SET) == 0 && std::fwrite(h, 1, 44, f_) == 44;
    ok_ = std::fclose(f_) == 0 && ok_;
    f_ = nullptr;
    if (ok_) {
        std::remove(path_.c_str());                    // Windows does not rename over a file
        ok_ = std::rename(temp_.c_str(), path_.c_str()) == 0;
    }
    if (!ok_) {
        std::remove(temp_.c_str());
        if (err) *err = "can't write " + path_;
    }
    return ok_;
}

// ---------------------------------------------------------------- the quick recorder

bool Recorder::start(const std::string& path, int channels, int sr) {
    stop();
    // the audio thread may still be in a push() that began before the last stop: the ring is not
    // touched until it has left (push() checks on_ after saying it is in)
    while (pushing_.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
    channels_ = std::max(1, channels);
    sr_ = sr;
    if (!file_.open(path, channels_, sr_, 24)) return false;
    path_ = path;
    written_ = 0;
    overflow_ = false;
    ring_.assign((size_t)sr_ * (size_t)channels_ * 8 + 1, 0.f);     // 8 s of slack for the disk
    chunk_.reserve(ring_.size());
    head_ = tail_ = 0;
    quit_ = false;
    on_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { writer(); });
    return true;
}

void Recorder::stop() {
    if (!thread_.joinable()) return;
    on_.store(false, std::memory_order_seq_cst);
    quit_ = true;
    thread_.join();
    drain();
    file_.finish();
}

void Recorder::push(const float* x, int frames, int channels) {
    struct In {
        std::atomic<int>& n;
        explicit In(std::atomic<int>& c) : n(c) { n.fetch_add(1, std::memory_order_seq_cst); }
        ~In() { n.fetch_sub(1, std::memory_order_release); }
    } in(pushing_);
    if (!on_.load(std::memory_order_seq_cst) || ring_.empty()) return;
    size_t n = ring_.size(), h = head_.load(std::memory_order_relaxed), t = tail_.load(std::memory_order_acquire);
    size_t free = (t + n - h - 1) % n, need = (size_t)frames * (size_t)channels_;
    if (need > free) { overflow_ = true; return; }
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < channels_; ++c) {
            ring_[h] = c < channels ? x[(size_t)f * (size_t)channels + (size_t)c] : 0.f;
            h = h + 1 == n ? 0 : h + 1;
        }
    head_.store(h, std::memory_order_release);
}

void Recorder::drain() {
    if (!file_.isOpen() || ring_.empty()) return;
    size_t n = ring_.size(), t = tail_.load(std::memory_order_relaxed), h = head_.load(std::memory_order_acquire);
    chunk_.clear();
    while (t != h) {
        chunk_.push_back(ring_[t]);
        t = t + 1 == n ? 0 : t + 1;
    }
    tail_.store(t, std::memory_order_release);
    size_t frames = chunk_.size() / (size_t)channels_;
    if (frames > 0) {
        file_.write(chunk_.data(), frames);
        written_ += (long long)frames;
    }
}

void Recorder::writer() {
    while (!quit_) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace moritz
