// score_layout.h — how a score is laid out on staves: the rows in orchestral order, a clef for each, where a pitch
// sits on its staff. Pure functions of the score (score.h), without the interpreter or a display: Musil's read-only
// roll (plot.h, music.mu's score-roll) and Moritz's editable roll draw with the same rules.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// Rows. A score's rows are those it declares (Score::rows, in their order, with their clefs) and then those its
// events name without a declaration, in orchestral order (instrumentRank: Picc Fl ... Vc Cb, unknown names after,
// shorter first); events of no row are gathered by kind (notes, sounds, patches, scores, messages), last.
// Clefs. A declared clef is kept (treble, bass, alto, none); otherwise a row of notes is on a treble staff, or on a
// bass staff when the median of its pitches is below middle C (music.mu's score-roll), and a row of other things
// is a line.
// Staves. A pitch is a diatonic step (C4 is 28: seven a octave, sharps on the step below, as notes are spelled in
// the roll); a staff's bottom line is E4 (treble), G2 (bass) or F3 (alto), and each step is half a space.
#pragma once
#include "score.h"

#include <algorithm>
#include <string>
#include <vector>

namespace musil {
namespace score {
namespace layout {

// the instrument codes of the *SOL databases in the order of an orchestral score
// (never destroyed: a detached thread, an export or a script, may still read it while the program exits)
inline const std::vector<std::string>& orchestralOrder() {
    static const std::vector<std::string>& v = *new std::vector<std::string>{"Picc", "Fl", "AFl", "BFl", "Ob", "EH", "ClEb", "ClBb", "Cl", "BCl", "CbCl",
                                               "Bn", "CbBn", "SSax", "ASax", "TSax", "BSax", "Hn", "TpC", "Tp", "Tbn", "BTb",
                                               "Tba", "Timp", "Perc", "Hp", "Pno", "Acc", "Gtr", "Vn", "Va", "Vc", "Cb"};
    return v;
}
// where an instrument sits in the orchestral order (unknown ones after, 1000 plus the length of the name)
inline int instrumentRank(const std::string& instr) {
    const auto& o = orchestralOrder();
    auto it = std::find(o.begin(), o.end(), instr);
    return it != o.end() ? (int)(it - o.begin()) : 1000 + (int)instr.size();
}

// the median of the pitches (the mean of the middle two of an even count), as Musil's median
inline double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
// the clef of a row of these pitches: none without pitches, bass when their median is below C4, else treble
inline std::string clefFor(const std::vector<double>& pitches) {
    if (pitches.empty()) return "none";
    return median(pitches) < 60 ? "bass" : "treble";
}
inline bool hasStaff(const std::string& clef) { return clef == "treble" || clef == "bass" || clef == "alto"; }

// a MIDI pitch as a diatonic step (C4 = 28) and whether it is written with a sharp
inline int diatonicStep(int midi, bool& sharp) {
    static const int step[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    static const bool sh[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int pc = ((midi % 12) + 12) % 12;
    sharp = sh[pc];
    return (midi / 12 - 1) * 7 + step[pc];
}
// the natural pitch of a diatonic step (the inverse of diatonicStep for the white keys)
inline int pitchOfStep(int step) {
    static const int white[7] = {0, 2, 4, 5, 7, 9, 11};
    int oct = step >= 0 ? step / 7 : -((-step + 6) / 7);
    return (oct + 1) * 12 + white[step - oct * 7];
}
// the step of a staff's bottom line: E4 (treble), G2 (bass), F3 (alto)
inline int bottomStep(const std::string& clef) {
    bool s;
    return diatonicStep(clef == "bass" ? 43 : clef == "alto" ? 53 : 64, s);
}
// how many half spaces above the bottom line of its staff a pitch is written (the nearest semitone below a microtone
// is the one spelled; its cents are shown apart)
inline int halfSpaces(double midi, const std::string& clef) {
    bool s;
    return diatonicStep((int)std::floor(midi + 1e-9), s) - bottomStep(clef);
}

// --- the rows of a score ----------------------------------------------------------------------------------------------
struct RowSpec {
    std::string name;            // the row's name ("" for a row of a kind: its events have no row)
    std::string clef;            // treble, bass, alto or none
    Kind kind = Kind::Note;      // for a row of a kind: which
    bool byKind = false;
    bool declared = false;       // in Score::rows
    bool pitched = false;        // holds notes
};
inline const char* kindRowName(Kind k) {
    switch (k) { case Kind::Note: return "notes"; case Kind::Sound: return "sounds"; case Kind::Patch: return "patches";
                 case Kind::Score: return "scores"; case Kind::Message: return "messages"; }
    return "?";
}
// the rows as a roll shows them (see the top of the file)
inline std::vector<RowSpec> rows(const Score& s) {
    std::vector<RowSpec> out;
    auto findNamed = [&](const std::string& n) -> long {
        for (size_t k = 0; k < out.size(); k++) if (!out[k].byKind && out[k].name == n) return (long)k;
        return -1;
    };
    for (auto& r : s.rows) {
        if (findNamed(r.name) >= 0) continue;
        RowSpec spec;
        spec.name = r.name; spec.clef = r.clef; spec.declared = true;
        out.push_back(spec);
    }
    std::vector<RowSpec> named;
    bool kinds[5] = {false, false, false, false, false};
    for (auto& ep : s.events()) {
        const Event& e = *ep;
        if (e.row.empty()) { kinds[(int)e.kind] = true; continue; }
        if (findNamed(e.row) >= 0) continue;
        bool seen = false;
        for (auto& n : named) seen = seen || n.name == e.row;
        if (!seen) { RowSpec spec; spec.name = e.row; named.push_back(spec); }
    }
    std::stable_sort(named.begin(), named.end(), [](const RowSpec& a, const RowSpec& b) { return instrumentRank(a.name) < instrumentRank(b.name); });
    out.insert(out.end(), named.begin(), named.end());
    const Kind order[] = {Kind::Note, Kind::Sound, Kind::Patch, Kind::Score, Kind::Message};
    for (Kind k : order)
        if (kinds[(int)k]) { RowSpec spec; spec.byKind = true; spec.kind = k; out.push_back(spec); }
    // the clefs: declared, or from the pitches of the row's notes
    std::vector<std::vector<double>> pitches(out.size());
    for (auto& ep : s.events()) {
        if (ep->kind != Kind::Note) continue;
        for (size_t k = 0; k < out.size(); k++) {
            bool in = ep->row.empty() ? out[k].byKind && out[k].kind == Kind::Note : !out[k].byKind && out[k].name == ep->row;
            if (in) { pitches[k].push_back(ep->pitch); break; }
        }
    }
    for (size_t k = 0; k < out.size(); k++) {
        out[k].pitched = !pitches[k].empty();
        if (out[k].clef.empty()) out[k].clef = clefFor(pitches[k]);
    }
    return out;
}
// the row of an event among rows (as rows() made them); -1 when it is in none
inline int rowOf(const std::vector<RowSpec>& rs, const Event& e) {
    for (size_t k = 0; k < rs.size(); k++) {
        if (e.row.empty() ? rs[k].byKind && rs[k].kind == e.kind : !rs[k].byKind && rs[k].name == e.row) return (int)k;
    }
    return -1;
}
// what a row is called on the screen
inline std::string rowLabel(const RowSpec& r) { return r.byKind ? kindRowName(r.kind) : r.name; }

}  // namespace layout
}  // namespace score
}  // namespace musil
