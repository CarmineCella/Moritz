// stored_zip.h — for the tests: a zip archive of a few files, stored (not compressed), as unzip and tar read it.
// The installation of a dataset is tested on one of these, so that nothing is downloaded (musil/tests/engines.cpp,
// Moritz's tests/engine/datasets_tests.cpp).
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ziptest {

inline uint32_t crc32(const std::string& s) {
    uint32_t c = 0xffffffffu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
inline void put16(std::string& o, uint32_t v) { o += (char)(v & 255); o += (char)((v >> 8) & 255); }
inline void put32(std::string& o, uint32_t v) { put16(o, v & 0xffff); put16(o, v >> 16); }

// files: (name in the archive, contents); a name ending in / is a folder. => false if it could not be written
inline bool write(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    for (auto& f : files) {
        uint32_t crc = crc32(f.second), size = (uint32_t)f.second.size(), offset = (uint32_t)out.size();
        put32(out, 0x04034b50); put16(out, 10); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0x21);
        put32(out, crc); put32(out, size); put32(out, size); put16(out, (uint32_t)f.first.size()); put16(out, 0);
        out += f.first; out += f.second;
        put32(central, 0x02014b50); put16(central, 20); put16(central, 10); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0x21); put32(central, crc); put32(central, size); put32(central, size);
        put16(central, (uint32_t)f.first.size()); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
        put32(central, f.first.back() == '/' ? 0x10 : 0); put32(central, offset); central += f.first;
    }
    uint32_t cdOffset = (uint32_t)out.size(), cdSize = (uint32_t)central.size();
    out += central;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, (uint32_t)files.size()); put16(out, (uint32_t)files.size());
    put32(out, cdSize); put32(out, cdOffset); put16(out, 0);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << out;
    f.close();
    return !f.fail();
}

// a tiny WAV (16-bit mono, 44.1 kHz, n samples of a ramp)
inline std::string wav(int n = 64) {
    std::string d;
    for (int k = 0; k < n; ++k) put16(d, (uint32_t)(uint16_t)(int16_t)(k * 300 - 9000));
    std::string o = "RIFF"; put32(o, 36 + (uint32_t)d.size()); o += "WAVEfmt "; put32(o, 16); put16(o, 1); put16(o, 1);
    put32(o, 44100); put32(o, 88200); put16(o, 2); put16(o, 16); o += "data"; put32(o, (uint32_t)d.size());
    return o + d;
}

}  // namespace ziptest
