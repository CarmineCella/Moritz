// midi.h — standard MIDI files read into notes with times in seconds, and a score written as one.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// Formats 0 and 1; the tempo map (meta 0x51) of every track applies to all; note-on with velocity 0 is a
// note-off; SMPTE divisions are refused. Each note is a record: track, channel, pitch (MIDI), velocity,
// at and dur in seconds, and the track's name when it has one. Writing (score_midi, below): a score's notes
// as a format 1 file, a track per part.

#pragma once
#include "../core.h"
#include "score.h"
#include "score_layout.h"
#include <fstream>
#include <cstring>
#include <deque>
#include <map>

namespace musil {

struct midi_note { int track, channel, pitch, velocity; double at, dur; };
struct midi_file { int ppq = 480; std::vector<midi_note> notes; std::vector<std::string> track_names; std::vector<std::pair<long, double>> tempos; double seconds = 0; };

namespace midi_detail {
inline uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint16_t be16(const unsigned char* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t varlen(const unsigned char* d, size_t n, size_t& p) { uint32_t v = 0; for (int k = 0; k < 4 && p < n; k++) { unsigned char b = d[p++]; v = (v << 7) | (b & 0x7f); if (!(b & 0x80)) break; } return v; }
}

inline midi_file parse_midi(const std::vector<unsigned char>& d, const std::string& path);
inline midi_file read_midi(const std::string& path) {
    std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("cannot open MIDI file " + path);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse_midi(d, path);
}
// the notes of the bytes of a MIDI file (path names it in the errors)
inline midi_file parse_midi(const std::vector<unsigned char>& d, const std::string& path) {
    using namespace midi_detail;
    if (d.size() < 14 || std::memcmp(d.data(), "MThd", 4) != 0) throw std::runtime_error("not a MIDI file: " + path);
    midi_file m; int format = be16(d.data() + 8); int ntracks = be16(d.data() + 10); int division = be16(d.data() + 12);
    if (division & 0x8000) throw std::runtime_error("SMPTE time division is not supported"); m.ppq = division > 0 ? division : 480;
    (void)format;
    struct raw { long tick; int track, channel, pitch, velocity; bool on; }; std::vector<raw> events;
    size_t hdr = be32(d.data() + 4); if (hdr < 6 || hdr > d.size() - 8) throw std::runtime_error("bad MIDI header: " + path);
    size_t p = 8 + hdr; int track = 0;
    // every read is checked against the end of its chunk: a truncated or corrupt file gives the events read up to the
    // damage (that track stops there), never a read past the data
    while (p + 8 <= d.size() && track < ntracks) {
        if (std::memcmp(d.data() + p, "MTrk", 4) != 0) throw std::runtime_error("bad track chunk");
        size_t len = be32(d.data() + p + 4), end = len > d.size() - p - 8 ? d.size() : p + 8 + len; p += 8; long tick = 0; unsigned char status = 0; std::string name;
        while (p < end) {
            tick += varlen(d.data(), end, p); if (p >= end) break;
            unsigned char b = d[p];
            if (b == 0xff) {                                   // meta
                if (p + 2 > end) break;
                p++; unsigned char type = d[p++]; uint32_t l = varlen(d.data(), end, p);
                if (l > end - p) break;                        // truncated
                if (type == 0x51 && l == 3) m.tempos.push_back({ tick, (double)((d[p] << 16) | (d[p + 1] << 8) | d[p + 2]) });
                if (type == 0x03 && name.empty()) name.assign((const char*)d.data() + p, l);
                p += l; continue;
            }
            if (b == 0xf0 || b == 0xf7) { p++; uint32_t l = varlen(d.data(), end, p); if (l > end - p) break; p += l; continue; }
            if (b & 0x80) { status = b; p++; }
            if (!(status & 0x80)) break;                       // data bytes without a status: corrupt
            int kind = status & 0xf0, ch = status & 0x0f;
            size_t need = kind == 0xc0 || kind == 0xd0 ? 1 : 2;
            if (kind == 0xf0 || need > end - p) break;         // a system message in a track, or truncated
            if (kind == 0x90 || kind == 0x80) { int pitch = d[p] & 0x7f, vel = d[p + 1] & 0x7f; events.push_back({ tick, track, ch, pitch, vel, kind == 0x90 && vel > 0 }); }
            p += need;
        }
        m.track_names.push_back(name); p = end; track++;
    }
    // the tempo map: microseconds per quarter, from tick 0 (500000 = 120 bpm by default)
    std::sort(m.tempos.begin(), m.tempos.end()); if (m.tempos.empty() || m.tempos[0].first > 0) m.tempos.insert(m.tempos.begin(), { 0, 500000.0 });
    auto seconds_at = [&](long tick) { double s = 0; for (size_t k = 0; k < m.tempos.size(); k++) { long t0 = m.tempos[k].first, t1 = k + 1 < m.tempos.size() ? m.tempos[k + 1].first : tick; if (tick <= t0) break; long span = std::min(tick, t1) - t0; s += span * m.tempos[k].second / 1e6 / m.ppq; if (tick <= t1) break; } return s; };
    std::stable_sort(events.begin(), events.end(), [](const raw& a, const raw& b) { return a.tick < b.tick; });
    // the notes sounding, per track, channel and pitch: a note-on of a pitch already sounding opens a second note
    // (the overlap is kept), and a note-off ends the oldest one (first in, first out)
    std::map<int, std::deque<raw>> open;                      // key: track * 4096 + channel * 128 + pitch
    for (auto& e : events) {
        int key = e.track * 4096 + e.channel * 128 + e.pitch;
        if (e.on) { open[key].push_back(e); }
        else { auto it = open.find(key); if (it != open.end() && !it->second.empty()) { const raw& on = it->second.front(); midi_note n{ e.track, e.channel, e.pitch, on.velocity, seconds_at(on.tick), std::max(0.0, seconds_at(e.tick) - seconds_at(on.tick)) }; m.notes.push_back(n); m.seconds = std::max(m.seconds, n.at + n.dur); it->second.pop_front(); } }
    }
    std::stable_sort(m.notes.begin(), m.notes.end(), [](const midi_note& a, const midi_note& b) { return a.at < b.at; });
    return m;
}

// --- writing: a score as a standard MIDI file --------------------------------------------------------------------------
// Format 1: a first track with the score's name, its tempo and its meter, then a track per part: per row (the
// instrument of its notes), and per player within a row when the notes say who plays them (the extra field player, as
// the orchestrators give: Vn 1, Vn 2), in orchestral order, as the parts of score-musicxml. Only notes are written
// (sounds, patches and messages are not MIDI); the scores placed inside are brought to the top first (flatten), and a
// chord is written as its notes (score-midi writes a detached copy). Times are exact at the score's one tempo (ppq
// ticks a quarter; a note lasts a tick at least); a note's velocity is its dynamics' (velocityOf: mf 80) times its
// gain, from 1 to 127. Each part plays on a channel of its own (channel 10 is left to the drums; past 15 parts the
// channels are used again), with the General MIDI program of its instrument when it is known (Vn: Violin...).
// Microtones: rounded to the nearest semitone by default (the notes rounded are counted). With bend, every note sounds
// on a channel whose pitch bend gives its deviation (a bend range of bend_range semitones, set on each channel by RPN 0
// when it is first used): the 15 melodic channels are shared by all the parts, a note taking a free channel (one
// already at its bend and its program first, else the one free the longest), so that up to 15 notes of different
// tunings sound at once; beyond that a note shares a busy channel (one at its bend if there is one) and is rounded to
// the nearest pitch that channel's bend gives (counted as rounded). A player that ignores pitch bend plays them rounded.
struct midi_write_options { int ppq = 480; bool bend = false; double bend_range = 2; };
struct midi_written { std::string bytes; int tracks = 0; size_t notes = 0, rounded = 0; std::vector<std::string> parts; };

namespace midi_detail {
inline void put_be(std::string& b, uint32_t v, int n) { for (int k = n - 1; k >= 0; k--) b += (char)((v >> (8 * k)) & 0xff); }
inline void put_varlen(std::string& b, uint32_t v) {
    v = std::min<uint32_t>(v, 0x0fffffff);
    unsigned char t[4]; int n = 0; t[n++] = v & 0x7f;
    while ((v >>= 7) && n < 4) t[n++] = (unsigned char)((v & 0x7f) | 0x80);
    while (n) b += (char)t[--n];
}
// an event of a track: at its tick, in its order there (0 note-offs, 1 settings, 2 note-ons), then as made
struct out_event { long tick; int order; long seq; std::string bytes; };
inline std::string meta(int type, const std::string& data) {
    std::string b; b += (char)0xff; b += (char)type; put_varlen(b, (uint32_t)data.size()); return b + data;
}
inline std::string track_chunk(std::vector<out_event> evs) {
    std::stable_sort(evs.begin(), evs.end(), [](const out_event& a, const out_event& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order != b.order ? a.order < b.order : a.seq < b.seq; });
    std::string d; long last = 0;
    for (auto& e : evs) { put_varlen(d, (uint32_t)std::max(0L, e.tick - last)); last = std::max(last, e.tick); d += e.bytes; }
    put_varlen(d, 0); d += meta(0x2f, "");
    std::string c = "MTrk"; put_be(c, (uint32_t)d.size(), 4); return c + d;
}
// an instrument's code without a player's number and with the first of a doubling (Fl|Picc 2: Fl)
inline std::string base_code(const std::string& row) {
    std::string c = row.substr(0, row.find('|'));
    size_t sp = c.find_last_of(' ');
    if (sp != std::string::npos && sp + 1 < c.size() && c.find_first_not_of("0123456789", sp + 1) == std::string::npos) c.resize(sp);
    return c;
}
// the General MIDI program (0-127) of an instrument code, -1 when unknown
inline int gm_program(const std::string& code) {
    static const std::pair<const char*, int> table[] = {
        {"Picc", 72}, {"Fl", 73}, {"AFl", 73}, {"BFl", 73}, {"Ob", 68}, {"EH", 69}, {"ClEb", 71}, {"ClBb", 71}, {"Cl", 71},
        {"BCl", 71}, {"CbCl", 71}, {"Bn", 70}, {"CbBn", 70}, {"SSax", 64}, {"ASax", 65}, {"TSax", 66}, {"BSax", 67},
        {"Hn", 60}, {"TpC", 56}, {"Tp", 56}, {"Tbn", 57}, {"BTb", 58}, {"Tba", 58}, {"Timp", 47}, {"Hp", 46}, {"Pno", 0},
        {"Acc", 21}, {"Gtr", 24}, {"Vn", 40}, {"Va", 41}, {"Vc", 42}, {"Cb", 43}};
    for (auto& t : table) if (code == t.first) return t.second;
    return -1;
}
}  // namespace midi_detail

inline midi_written score_midi(const score::Score& s, const midi_write_options& opt = {}) {
    using namespace midi_detail;
    namespace sc = score;
    midi_written w;
    const int ppq = std::clamp(opt.ppq, 24, 9600);
    const double range = std::clamp(opt.bend_range, 1.0, 24.0);
    const int den = s.meterDen > 0 ? s.meterDen : 4, num = std::clamp(s.meterNum, 1, 255);
    // quarters a minute, within what a MIDI tempo can say (24 bits of microseconds a quarter)
    const double qbpm = std::clamp((s.bpm > 0 && std::isfinite(s.bpm) ? s.bpm : 60) * 4.0 / den, 4.0, 1e6);
    auto tick_of = [&](double t) {
        double ticks = std::max(0.0, std::isfinite(t) ? t : 0.0) * qbpm / 60.0 * ppq;
        return (long)std::llround(std::min(ticks, 2e9)); };      // (a long has 32 bits on Windows)
    std::vector<sc::Placed> flat;
    sc::flatten(s, flat);
    // the parts, (row, player), and the notes of each
    struct note_out { long on, off; double pitch; int vel; size_t part; long seq; };
    struct part { std::string row; long player; std::string name; int program; };
    std::vector<part> parts; std::vector<note_out> notes;
    std::map<std::pair<std::string, long>, size_t> index;
    std::map<std::string, size_t> row_first;
    for (auto& p : flat) {
        const sc::Event& e = p.ev;
        if (e.kind != sc::Kind::Note || e.variant == "chord") continue;
        double pitch = e.sounding();
        if (!std::isfinite(pitch)) continue;
        const sc::Datum* pl = sc::field(e.extra, "player");
        long player = pl && pl->kind == sc::Datum::Num && std::isfinite(pl->num) ? (long)pl->num : -1;
        auto it = index.find({e.row, player});
        if (it == index.end()) {
            row_first.emplace(e.row, row_first.size());
            it = index.emplace(std::make_pair(e.row, player), parts.size()).first;
            parts.push_back({e.row, player, "", gm_program(base_code(e.row))});
        }
        long on = tick_of(e.at), off = std::max(on + 1, tick_of(e.at + std::max(0.0, e.dur)));
        int vel = std::clamp((int)std::lround(sc::velocityOf(e.dyn) * (std::isfinite(e.gain) ? e.gain : 1.0)), 1, 127);
        notes.push_back({on, off, pitch, vel, it->second, (long)notes.size()});
    }
    // in orchestral order (then as they came), the players of a row by their number, named as score-musicxml's parts
    std::vector<size_t> order(parts.size());
    for (size_t k = 0; k < order.size(); k++) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const part &x = parts[a], &y = parts[b];
        int rx = sc::layout::instrumentRank(base_code(x.row)), ry = sc::layout::instrumentRank(base_code(y.row));
        if (rx != ry) return rx < ry;
        if (x.row != y.row) return row_first[x.row] < row_first[y.row];
        return x.player < y.player;
    });
    std::vector<size_t> track_of(parts.size());
    std::map<std::string, int> players, numbered;
    for (auto& p : parts) if (p.player >= 0) players[p.row]++;
    for (size_t k = 0; k < order.size(); k++) {
        part& p = parts[order[k]];
        track_of[order[k]] = k;
        std::string row = p.row.empty() ? std::string("notes") : p.row;
        p.name = p.player >= 0 && players[p.row] > 1 ? row + " " + std::to_string(++numbered[p.row]) : row;
        w.parts.push_back(p.name);
    }
    std::vector<std::vector<out_event>> tracks(parts.size());
    long seq = 0;
    auto add = [&](size_t part_index, long tick, int ord, std::string bytes) {
        tracks[track_of[part_index]].push_back({tick, ord, seq++, std::move(bytes)}); };
    auto msg = [](int status, int a, int b = -1) {
        std::string m; m += (char)status; m += (char)(a & 0x7f); if (b >= 0) m += (char)(b & 0x7f); return m; };
    static const int melodic[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};
    for (size_t k = 0; k < parts.size(); k++) add(k, 0, 1, meta(0x03, parts[k].name));
    if (!opt.bend) {
        for (size_t k = 0; k < parts.size(); k++)
            if (parts[k].program >= 0) add(k, 0, 1, msg(0xc0 | melodic[track_of[k] % 15], parts[k].program));
        for (auto& n : notes) {
            int ch = melodic[track_of[n.part] % 15];
            int key = std::clamp((int)std::lround(n.pitch), 0, 127);
            if (std::fabs(n.pitch - key) > 0.005) w.rounded++;
            add(n.part, n.on, 2, msg(0x90 | ch, key, n.vel));
            add(n.part, n.off, 0, msg(0x80 | ch, key, 0));
        }
    } else {
        struct chan { long busy = 0; int bend = 8192, program = -1; bool rpn = false; };
        chan ch[16];
        std::vector<note_out> by_onset = notes;
        std::stable_sort(by_onset.begin(), by_onset.end(), [](const note_out& a, const note_out& b) { return a.on < b.on; });
        for (auto& n : by_onset) {
            int key = std::clamp((int)std::lround(n.pitch), 0, 127);
            int need = std::clamp(8192 + (int)std::lround((n.pitch - key) / range * 8192), 0, 16383);
            int prog = parts[n.part].program;
            int best = -1, best_score = -1;
            for (int c : melodic) {                    // a free one: at its bend and program, at its bend, free the longest
                if (ch[c].busy > n.on) continue;
                int score_ = (ch[c].bend == need ? 2 : 0) + (prog < 0 || ch[c].program == prog ? 1 : 0);
                if (score_ > best_score || (score_ == best_score && ch[c].busy < ch[best].busy)) { best = c; best_score = score_; }
            }
            bool shared = best < 0;
            if (shared) {                              // none free: one at its bend, else the one free soonest
                for (int c : melodic) if (ch[c].bend == need && (best < 0 || ch[c].busy < ch[best].busy)) best = c;
                if (best < 0) for (int c : melodic) if (best < 0 || ch[c].busy < ch[best].busy) best = c;
            }
            chan& c = ch[best];
            if (shared && c.bend != need) {            // the channel keeps its bend: the nearest pitch it gives
                key = std::clamp((int)std::lround(n.pitch - (c.bend - 8192) * range / 8192.0), 0, 127);
                need = c.bend;
            }
            if (std::fabs(key + (need - 8192) * range / 8192.0 - n.pitch) > 0.005) w.rounded++;
            if (!c.rpn) {                              // the bend range (RPN 0), then the null RPN
                int semis = (int)range, cents = std::clamp((int)std::lround((range - semis) * 100), 0, 99);
                const int rpn[6][2] = {{101, 0}, {100, 0}, {6, semis}, {38, cents}, {101, 127}, {100, 127}};
                for (auto& r : rpn) add(n.part, n.on, 1, msg(0xb0 | best, r[0], r[1]));   // an event each
                c.rpn = true;
            }
            if (prog >= 0 && c.program != prog) { add(n.part, n.on, 1, msg(0xc0 | best, prog)); c.program = prog; }
            if (c.bend != need) { add(n.part, n.on, 1, msg(0xe0 | best, need & 0x7f, need >> 7)); c.bend = need; }
            add(n.part, n.on, 2, msg(0x90 | best, key, n.vel));
            add(n.part, n.off, 0, msg(0x80 | best, key, 0));
            c.busy = std::max(c.busy, n.off);
        }
    }
    // the first track: the name, the meter, the tempo
    std::vector<out_event> first;
    first.push_back({0, 0, 0, meta(0x03, s.name)});
    int dd = 0;
    while ((1 << (dd + 1)) <= den && dd < 7) dd++;
    first.push_back({0, 0, 1, meta(0x58, std::string{(char)num, (char)dd, (char)24, (char)8})});
    std::string t;
    put_be(t, (uint32_t)std::clamp(std::llround(60e6 / qbpm), 1LL, 0xffffffLL), 3);
    first.push_back({0, 0, 2, meta(0x51, t)});
    std::string b = "MThd";
    put_be(b, 6, 4); put_be(b, 1, 2); put_be(b, (uint32_t)(tracks.size() + 1), 2); put_be(b, (uint32_t)ppq, 2);
    b += track_chunk(std::move(first));
    for (auto& tr : tracks) b += track_chunk(std::move(tr));
    w.bytes = std::move(b);
    w.tracks = (int)tracks.size() + 1;
    w.notes = notes.size();
    return w;
}

// (midi-read path) => a record: ppq, seconds, tracks (names), and notes (records: track channel pitch velocity at dur),
//   times in seconds after the file's tempo map
inline vptr mus_midi_read(vlist& a, Interp& i) {
    midi_file m; try { m = read_midi(i.read_path(i.str(a[0]))); } catch (std::exception& e) { i.bad(std::string("midi-read: ") + e.what()); }
    vlist notes; for (auto& n : m.notes) notes.push_back(v_list({ v_list({ v_sym("track"), v_num(n.track) }), v_list({ v_sym("channel"), v_num(n.channel) }), v_list({ v_sym("pitch"), v_num(n.pitch) }), v_list({ v_sym("velocity"), v_num(n.velocity) }), v_list({ v_sym("at"), v_num(n.at) }), v_list({ v_sym("dur"), v_num(n.dur) }) }));
    vlist names; for (auto& s : m.track_names) names.push_back(v_str(s));
    return v_list({ v_list({ v_sym("ppq"), v_num(m.ppq) }), v_list({ v_sym("seconds"), v_num(m.seconds) }), v_list({ v_sym("tracks"), v_list(std::move(names)) }), v_list({ v_sym("notes"), v_list(std::move(notes)) }) });
}

inline void add_midi(Interp& i) { i.def("midi-read", mus_midi_read, 1, 1); }

} // namespace musil
