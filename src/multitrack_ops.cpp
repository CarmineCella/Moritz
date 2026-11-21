// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// multitrack_ops.cpp — a session's tracks, buses and options as undo steps; recording into tracks (multitrack_ops.h)

#include "multitrack_ops.h"
#include "fileio.h"
#include "timeline_ops.h"
#include "transport.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace moritz {
namespace mt {

namespace tl = timeline;

namespace {
const sc::Row* rowOf(const sc::Score& s, const std::string& name) {
    for (auto& r : s.rows)
        if (r.name == name) return &r;
    return nullptr;
}
// every track as a declared row, in the timeline's order
std::vector<sc::Row> declaredRows(const sc::Score& s) {
    std::vector<sc::Row> rows;
    for (auto& t : tl::tracks(s)) {
        if (const sc::Row* r = rowOf(s, t)) rows.push_back(*r);
        else rows.push_back(sc::Row{t, "", {}});
    }
    return rows;
}
bool fail(ScoreDoc& d, const std::string& why) {
    d.message = why;
    return false;
}
}  // namespace

RowSettings settingsOf(const sc::Score& s, const std::string& track) {
    const sc::Row* r = rowOf(s, track);
    return r ? rowSettings(*r) : RowSettings{};
}

bool setSettings(ScoreDoc& d, const std::string& track, const RowSettings& rs, const std::string& what) {
    if (tl::trackIndex(d.score(), track) < 0) return fail(d, "no track " + track);
    auto rows = declaredRows(d.score());
    for (auto& r : rows)
        if (r.name == track) setRowSettings(r, rs);
    auto e = d.edit(what);
    e.setRows(std::move(rows));
    return e.commit();
}

std::vector<std::string> buses(const sc::Score& s) {
    std::vector<std::string> out;
    for (auto& r : s.rows)
        if (rowSettings(r).bus) out.push_back(r.name);
    return out;
}

std::string addBus(ScoreDoc& d, const std::string& given) {
    auto all = tl::tracks(d.score());
    std::string name = given;
    if (name.empty())
        for (int k = (int)buses(d.score()).size() + 1;; ++k) {
            name = "bus " + std::to_string(k);
            if (std::find(all.begin(), all.end(), name) == all.end()) break;
        }
    if (std::find(all.begin(), all.end(), name) != all.end()) { d.message = "there is a track " + name + " already"; return ""; }
    auto rows = declaredRows(d.score());
    sc::Row r{name, "", {}};
    RowSettings rs;
    rs.bus = true;
    setRowSettings(r, rs);
    rows.push_back(std::move(r));
    auto e = d.edit("Add Bus");
    e.setRows(std::move(rows));
    return e.commit() ? name : "";
}

bool setSend(ScoreDoc& d, const std::string& track, const std::string& bus, double gain) {
    if (!std::isfinite(gain)) return fail(d, "a send's level is a number");
    auto b = buses(d.score());
    if (std::find(b.begin(), b.end(), bus) == b.end()) return fail(d, "no bus " + bus);
    if (bus == track) return fail(d, "a bus does not send to itself");
    RowSettings rs = settingsOf(d.score(), track);
    auto it = std::find_if(rs.sends.begin(), rs.sends.end(), [&](const RowSettings::Send& s) { return s.bus == bus; });
    if (gain <= 0) {
        if (it == rs.sends.end()) return fail(d, track + " does not send to " + bus);
        rs.sends.erase(it);
    } else if (it != rs.sends.end()) {
        it->gain = gain;
    } else {
        if ((int)rs.sends.size() >= kMaxSends) return fail(d, "at most " + std::to_string(kMaxSends) + " sends a track");
        rs.sends.push_back({bus, gain});
    }
    return setSettings(d, track, rs, "Send");
}

bool setOutput(ScoreDoc& d, const std::string& track, const std::string& bus) {
    if (!bus.empty()) {
        auto b = buses(d.score());
        if (std::find(b.begin(), b.end(), bus) == b.end()) return fail(d, "no bus " + bus);
        if (bus == track) return fail(d, "a bus does not go into itself");
    }
    RowSettings rs = settingsOf(d.score(), track);
    rs.out = bus;
    return setSettings(d, track, rs, "Output");
}

bool setArm(ScoreDoc& d, const std::string& track, bool on, int input, int channels) {
    RowSettings rs = settingsOf(d.score(), track);
    if (rs.bus && on) return fail(d, "a bus records nothing: arm a track");
    rs.arm = on;
    if (input >= 0) rs.input = std::clamp(input, 0, 64);
    if (channels >= 1) rs.inputCh = std::clamp(channels, 1, 2);
    if (on && rs.input == 0) rs.input = 1;               // armed: from the first input unless told
    return setSettings(d, track, rs, on ? "Arm" : "Disarm");
}

bool setMonitor(ScoreDoc& d, const std::string& track, bool on) {
    RowSettings rs = settingsOf(d.score(), track);
    rs.monitor = on;
    return setSettings(d, track, rs, "Monitor");
}

bool setTrackPatch(ScoreDoc& d, const std::string& track, const std::string& role, const std::string& file) {
    RowSettings rs = settingsOf(d.score(), track);
    std::string name = file.empty() ? std::string() : tl::sourceName(d, file);
    if (role == "insert") rs.insert = name;
    else if (role == "instrument") rs.instrument = name;
    else return fail(d, "a track has an insert and an instrument");
    return setSettings(d, track, rs, role == "insert" ? "Insert" : "Instrument");
}

bool setSession(ScoreDoc& d, const SessionSettings& v, const std::string& what) {
    auto s = d.score().clone();
    setSessionSettings(*s, v);
    if (s->extra == d.score().extra) return fail(d, "nothing changed");
    auto e = d.edit(what);
    e.setExtra(s->extra);
    return e.commit();
}

bool setClipFades(ScoreDoc& d, uint64_t id, double fadeIn, double fadeOut, const std::string& inShape, const std::string& outShape) {
    auto ev = d.score().find(id);
    if (!ev || ev->kind != sc::Kind::Sound) return fail(d, "fades are a sound clip's");
    sc::Event now = *ev;
    auto put = [&](const char* k, double v) {
        v = std::clamp(v, 0.0, now.dur);
        if (v > 0) sc::setField(now.extra, k, sc::Datum::number(v));
        else sc::eraseField(now.extra, k);
    };
    put("fade-in", fadeIn);
    put("fade-out", fadeOut);
    auto shape = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        if (v == "linear") sc::eraseField(now.extra, k);
        else sc::setField(now.extra, k, sc::Datum::symbol(v));
    };
    shape("fade-in-shape", inShape);
    shape("fade-out-shape", outShape);
    return tl::change(d, now, "Fades");
}

bool setClipGain(ScoreDoc& d, uint64_t id, double gain) {
    auto ev = d.score().find(id);
    if (!ev || !tl::isClip(*ev)) return fail(d, "no clip");
    if (!(gain >= 0) || !std::isfinite(gain)) return fail(d, "a gain of 0 or more");
    sc::Event now = *ev;
    now.gain = std::min(gain, 16.0);
    return tl::change(d, now, "Clip Gain");
}

// ---------------------------------------------------------------- recording

SessionRecorder::~SessionRecorder() {
    std::string m;
    if (recording()) stop(m);
}

bool SessionRecorder::start(const std::shared_ptr<ScoreDoc>& d, Patch* root, double playhead, int deviceLatency, std::string& err) {
    if (recording()) { err = "already recording"; return false; }
    if (d->path().empty()) { err = "save the session first: its recordings go next to its file"; return false; }
    const sc::Score& s = d->score();
    Box* box = d->player(root);
    if (!box) { err = "nothing plays the session"; return false; }
    double sr = box->patch->root()->sampleRate.load();
    SessionSettings ss = sessionSettings(s);
    std::string folder = mediaFolder(d->path());
    std::vector<std::unique_ptr<Take>> takes;
    for (auto& r : s.rows) {
        RowSettings rs = rowSettings(r);
        if (!rs.arm || rs.bus || rs.input < 1) continue;
        auto t = std::make_unique<Take>();
        t->track = r.name;
        t->path = takePath(folder, r.name);
        t->input = rs.input - 1;
        t->channels = rs.inputCh;
        takes.push_back(std::move(t));
    }
    if (takes.empty()) { err = "no track is armed: arm one (R in its header) to record into it"; return false; }
    playhead = std::max(0.0, playhead);
    int64_t from = (int64_t)std::llround(playhead * sr), to = -1;
    if (ss.punch && ss.punchOut > ss.punchIn) {
        from = (int64_t)std::llround(ss.punchIn * sr);
        to = (int64_t)std::llround(ss.punchOut * sr);
    }
    auto rec = std::make_unique<Recording>(std::move(takes), from, to, sr);
    auto t = transportOf(box->patch);
    double beat = 60.0 / std::max(1.0, t->bpm);
    double pre = ss.countIn * t->num * beat;                    // the count-in: bars before
    double startSec = std::max(0.0, std::min(playhead, from / sr) - pre);
    if (ss.countIn > 0) {
        rec->clickFrom = (int64_t)std::llround(startSec * sr);
        rec->clickBeat = (int64_t)std::llround(beat * sr);
        rec->clickBar = t->num;
    }
    if (!startRecording(box, std::move(rec), err)) return false;
    latency_ = deviceLatency + ss.latency;
    from_ = from / sr;
    doc_ = d;
    box_ = box;
    root_ = root;
    t->locate(box->patch, startSec);
    t->play(box->patch);
    return true;
}

bool SessionRecorder::stop(std::string& msg) {
    added.clear();
    if (!recording()) { msg = "not recording"; return false; }
    Box* box = box_;
    box_ = nullptr;
    // the box may have gone with its patch (the document closed): what it recorded is lost with it
    if (doc_->playerBox() != box) { msg = "the session's player is gone: nothing recorded"; doc_.reset(); return false; }
    auto t = transportOf(box->patch);
    t->stop(box->patch);
    auto rec = stopRecording(box);
    if (!rec) { msg = "nothing recorded"; return false; }
    std::string err;
    bool ok = rec->finish(err);
    double sr = rec->sampleRate();
    auto e = doc_->edit("Record");
    int n = 0;
    for (auto& take : rec->takes()) {
        if (take->written == 0) continue;
        double at, from;
        placeTake(take->firstPos.load(), latency_, sr, at, from);
        double dur = (double)take->written / sr - from;
        if (dur <= 0) continue;
        auto ev = std::make_shared<sc::Event>();
        ev->kind = sc::Kind::Sound;
        ev->source = tl::sourceName(*doc_, take->path);
        ev->row = take->track;
        ev->at = at;
        ev->dur = dur;
        ev->from = from;
        ev->to = from + dur;
        ev->label = std::filesystem::u8path(take->path).stem().u8string();
        if (uint64_t id = e.insert(ev)) added.push_back(id);
        ++n;
    }
    if (n == 0) {
        msg = ok ? "nothing recorded (the transport did not reach the start)" : err;
        doc_.reset();
        return false;
    }
    if (!e.commit()) { msg = doc_->message; doc_.reset(); return false; }
    doc_->select(added);
    msg = "recorded " + std::to_string(n) + (n == 1 ? " take" : " takes") + (rec->overflowed() ? " (the disk was too slow: some input was lost)" : "") +
          (ok ? "" : ": " + err);
    doc_.reset();
    return true;
}

std::string stemPath(const std::string& wav, const std::string& track) {
    std::string base = wav;
    if (base.size() > 4 && (base.compare(base.size() - 4, 4, ".wav") == 0 || base.compare(base.size() - 4, 4, ".WAV") == 0))
        base.resize(base.size() - 4);
    std::string name;
    for (char c : track.empty() ? std::string("no track") : track)
        name += (c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|') ? '_' : c;
    return base + "-" + name + ".wav";
}

}  // namespace mt
}  // namespace moritz
