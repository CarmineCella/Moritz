// continuation.h — a note that goes on where a note of the same sound ends becomes one longer note: the merging of
// held notes an orchestration's connection does (music.mu's merge-continuations, Moritz's orchidea, the solutions
// kept in a score: score_solutions.h). No interpreter: Musil and Moritz include it.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace musil {

// spans in order of onset, each (at, dur, key): a span whose key is empty never merges; one whose key a span already
// kept has and which starts within 30 ms of that span's end, and with which it would last no more than hold seconds,
// lengthens it (to where it ends, never shorter). => the spans kept: (the index of the span that begins it, its length)
struct span { double at = 0, dur = 0; std::string key; };
inline std::vector<std::pair<size_t, double>> continuation_plan(const std::vector<span>& spans, double hold) {
    std::vector<std::pair<size_t, double>> out; std::map<std::string, std::vector<size_t>> by_key;   // key -> the kept spans of that sound
    for (size_t k = 0; k < spans.size(); k++) {
        const span& x = spans[k]; long prev = -1;
        if (!x.key.empty()) { auto it = by_key.find(x.key);
            if (it != by_key.end()) for (size_t j : it->second) { double at = spans[out[j].first].at, d = out[j].second;
                if (std::fabs(at + d - x.at) < 0.03 && x.at + x.dur - at <= hold) { prev = (long)j; break; } } }
        if (prev < 0) { out.push_back({ k, x.dur }); if (!x.key.empty()) by_key[x.key].push_back(out.size() - 1); }
        else { double at = spans[out[(size_t)prev].first].at; out[(size_t)prev].second = std::max(out[(size_t)prev].second, x.at + x.dur - at); }
    }
    return out;
}

}  // namespace musil
