// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// recorder.h — writing WAV files, and the quick recorder.
//
// WavWriter is Moritz's one WAV writer (buffer~ write, sfrecord~, the quick
// recorder, moritz --render): integer PCM (16 or 24 bit), written as it
// comes, into a temporary file in the same folder that becomes the file only
// when finish() has checked the writes and the close. (Musil's wav_tools.h
// writes a whole file of doubles at once; these write streams.)
//
// The quick recorder: whatever goes to the dac, written to a 24-bit WAV
// file. The audio thread pushes into a lock-free ring buffer; a writer thread
// drains it to the file, so the audio never waits on the disk.
#pragma once

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace moritz {

class WavWriter {
public:
    WavWriter() = default;
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    ~WavWriter();                        // not finished: the temporary file is removed
    // starts path (bits 16 or 24); false (and err) if the temporary file can't be made
    bool open(const std::string& path, int channels, int sr, int bits, std::string* err = nullptr);
    // interleaved frames, clipped to -1..1 (NaN: 0)
    bool write(const float* x, size_t frames);
    // the header completed, the file closed and checked, then renamed to path
    bool finish(std::string* err = nullptr);
    bool isOpen() const { return f_ != nullptr; }
    long long frames() const { return frames_; }

private:
    std::FILE* f_ = nullptr;
    std::string path_, temp_;
    int channels_ = 1, sr_ = 44100, bits_ = 16;
    long long frames_ = 0, bytes_ = 0;
    bool ok_ = true;
    std::vector<unsigned char> buf_;
};

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
    std::vector<float> ring_;
    std::atomic<size_t> head_{0}, tail_{0};    // written by the audio thread / the writer
    std::atomic<bool> on_{false}, quit_{false}, overflow_{false};
    std::atomic<int> pushing_{0};                // the audio thread is in push(): start() waits it out
    std::atomic<long long> written_{0};          // frames
    std::thread thread_;
    WavWriter file_;
    std::vector<float> chunk_;                   // the writer's: what it takes from the ring
    std::string path_;
    int channels_ = 2, sr_ = 0;
};

}  // namespace moritz
