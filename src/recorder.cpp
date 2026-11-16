// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// recorder.cpp — the quick recorder (recorder.h)

#include "recorder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace moritz {

namespace {
void put16(std::FILE* f, uint32_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; std::fwrite(b, 1, 2, f); }
void put32(std::FILE* f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void header(std::FILE* f, int channels, int sr, uint32_t dataBytes) {
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);                                  // PCM
    put16(f, (uint32_t)channels);
    put32(f, (uint32_t)sr);
    put32(f, (uint32_t)(sr * channels * 3));
    put16(f, (uint32_t)(channels * 3));
    put16(f, 24);
    std::fwrite("data", 1, 4, f);
    put32(f, dataBytes);
}
}  // namespace

bool Recorder::start(const std::string& path, int channels, int sr) {
    stop();
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    path_ = path;
    channels_ = std::max(1, channels);
    sr_ = sr;
    dataBytes_ = 0;
    written_ = 0;
    overflow_ = false;
    header(file_, channels_, sr_, 0);
    ring_.assign((size_t)sr_ * (size_t)channels_ * 8 + 1, 0.f);     // 8 s of slack for the disk
    head_ = tail_ = 0;
    quit_ = false;
    on_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { writer(); });
    return true;
}

void Recorder::stop() {
    if (!thread_.joinable()) return;
    on_.store(false, std::memory_order_release);
    quit_ = true;
    thread_.join();
    drain();
    finishHeader();
    std::fclose(file_);
    file_ = nullptr;
}

void Recorder::push(const float* x, int frames, int channels) {
    if (!on_.load(std::memory_order_acquire) || ring_.empty()) return;
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
    if (!file_ || ring_.empty()) return;
    size_t n = ring_.size(), t = tail_.load(std::memory_order_relaxed), h = head_.load(std::memory_order_acquire);
    std::vector<uint8_t> bytes;
    size_t count = 0;
    while (t != h) {
        double v = std::clamp((double)ring_[t], -1.0, 1.0);
        int32_t s = (int32_t)std::lround(v * 8388607.0);
        bytes.push_back((uint8_t)s);
        bytes.push_back((uint8_t)(s >> 8));
        bytes.push_back((uint8_t)(s >> 16));
        t = t + 1 == n ? 0 : t + 1;
        ++count;
    }
    tail_.store(t, std::memory_order_release);
    if (!bytes.empty()) {
        std::fwrite(bytes.data(), 1, bytes.size(), file_);
        dataBytes_ += (long long)bytes.size();
        written_ += (long long)(count / (size_t)channels_);
    }
}

void Recorder::writer() {
    while (!quit_) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void Recorder::finishHeader() {
    std::fflush(file_);
    std::fseek(file_, 0, SEEK_SET);
    header(file_, channels_, sr_, (uint32_t)std::min<long long>(dataBytes_, 0xFFFFFFF0LL));
}

}  // namespace moritz
