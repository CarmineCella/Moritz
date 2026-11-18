// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// recorder.h — the quick recorder: whatever goes to the dac, written to a
// 24-bit WAV file. The audio thread pushes into a lock-free ring buffer; a
// writer thread drains it to the file, so the audio never waits on the disk.
#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace moritz {

class Recorder {
public:
    ~Recorder() { stop(); }
    // starts writing to path (channels at sr); false if the file can't be opened
    bool start(const std::string& path, int channels, int sr);
    void stop();
    bool recording() const { return on_.load(std::memory_order_acquire); }
    double seconds() const { return sr_ > 0 ? (double)written_.load() / sr_ : 0.0; }
    const std::string& path() const { return path_; }
    bool overflowed() const { return overflow_.load(); }
    // audio thread: interleaved frames
    void push(const float* x, int frames, int channels);

private:
    void writer();
    void drain();
    void finishHeader();
    std::vector<float> ring_;
    std::atomic<size_t> head_{0}, tail_{0};    // written by the audio thread / the writer
    std::atomic<bool> on_{false}, quit_{false}, overflow_{false};
    std::atomic<int> pushing_{0};                // the audio thread is in push(): start() waits it out
    std::atomic<long long> written_{0};          // frames
    std::thread thread_;
    std::FILE* file_ = nullptr;
    std::string path_;
    int channels_ = 2, sr_ = 0;
    long long dataBytes_ = 0;
};

}  // namespace moritz
