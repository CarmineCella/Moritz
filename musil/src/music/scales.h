// scales.h — the scales Musil knows by name: the steps of each in semitones above its tonic. One table for
// scale-pitches and pitch-field (music.mu, through scale-steps), the harmonizer's scale mode (signals/psola.h:
// Musil's harmonize, Moritz's harmonize~) and anything else that names a scale. Self-contained.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace musil {
namespace music {

struct ScaleDef {
    const char* name;
    int n;
    int steps[12];
};

inline const ScaleDef* scale_table(int& count) {
    static const ScaleDef table[] = {
        {"major", 7, {0, 2, 4, 5, 7, 9, 11}},
        {"minor", 7, {0, 2, 3, 5, 7, 8, 10}},
        {"dorian", 7, {0, 2, 3, 5, 7, 9, 10}},
        {"phrygian", 7, {0, 1, 3, 5, 7, 8, 10}},
        {"lydian", 7, {0, 2, 4, 6, 7, 9, 11}},
        {"mixolydian", 7, {0, 2, 4, 5, 7, 9, 10}},
        {"aeolian", 7, {0, 2, 3, 5, 7, 8, 10}},
        {"locrian", 7, {0, 1, 3, 5, 6, 8, 10}},
        {"harmonic-minor", 7, {0, 2, 3, 5, 7, 8, 11}},
        {"melodic-minor", 7, {0, 2, 3, 5, 7, 9, 11}},
        {"chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
        {"whole", 6, {0, 2, 4, 6, 8, 10}},
        {"pentatonic", 5, {0, 2, 4, 7, 9}},
        {"minor-pentatonic", 5, {0, 3, 5, 7, 10}},
        {"blues", 6, {0, 3, 5, 6, 7, 10}},
    };
    count = (int)(sizeof(table) / sizeof(table[0]));
    return table;
}

// the scale of that name, or nullptr
inline const ScaleDef* scale_named(const std::string& name) {
    int n = 0;
    const ScaleDef* t = scale_table(n);
    for (int k = 0; k < n; ++k)
        if (name == t[k].name) return &t[k];
    return nullptr;
}

inline std::vector<std::string> scale_names() {
    int n = 0;
    const ScaleDef* t = scale_table(n);
    std::vector<std::string> out;
    for (int k = 0; k < n; ++k) out.push_back(t[k].name);
    return out;
}

// the pitch classes of a scale as a 12-bit mask (bit 0 the tonic), its steps transposed to a key 0..11
inline int scale_mask(const int* steps, int n, int key) {
    int m = 0;
    for (int k = 0; k < n; ++k) m |= 1 << (((steps[k] + key) % 12 + 12) % 12);
    return m;
}

// a key's pitch class: 0..11, or a name (C, C#, Db, ... B, upper or lower case); -1 if it is neither
inline int key_class(const std::string& s) {
    if (s.empty()) return -1;
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    char c = (char)std::toupper((unsigned char)s[0]);
    if (c < 'A' || c > 'G') return -1;
    int pc = base[c - 'A'];
    for (size_t k = 1; k < s.size(); ++k) {
        if (s[k] == '#' || s[k] == 's') ++pc;
        else if (s[k] == 'b') --pc;
        else return -1;
    }
    return (pc % 12 + 12) % 12;
}

}  // namespace music
}  // namespace musil
