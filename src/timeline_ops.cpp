// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// timeline_ops.cpp — the timeline's operations on a score (timeline_ops.h)

#include "timeline_ops.h"

#include "music/score_text.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace moritz {
namespace timeline {

namespace {

constexpr double kMinDur = 0.001;   // a clip is never shorter

const sc::Row* rowOf(const sc::Score& s, const std::string& name) {
    for (auto& r : s.rows)
        if (r.name == name) return &r;
    return nullptr;
}

// every track as a declared row, in the order shown (an implicit track gets its row)
std::vector<sc::Row> declaredRows(const sc::Score& s) {
    std::vector<sc::Row> rows;
    for (auto& t : tracks(s)) {
        if (const sc::Row* r = rowOf(s, t)) rows.push_back(*r);
        else rows.push_back(sc::Row{t, "", {}});
    }
    return rows;
}

bool fail(ScoreDoc& d, const std::string& why) {
    d.message = why;
    return false;
}

std::vector<Lane> declaredLanes(const sc::Row* r) {
    std::vector<Lane> out;
    if (!r) return out;
    const sc::Datum* l = sc::field(r->extra, "lanes");
    if (!l || l->kind != sc::Datum::List) return out;
    for (auto& x : l->list) {
        if (x.kind != sc::Datum::List || x.list.empty() || x.list[0].kind == sc::Datum::List || x.list[0].kind == sc::Datum::Num) continue;
        Lane lane;
        lane.name = x.list[0].s;
        if (x.list.size() > 2 && x.list[1].kind == sc::Datum::Num && x.list[2].kind == sc::Datum::Num && x.list[2].num > x.list[1].num) {
            lane.lo = x.list[1].num;
            lane.hi = x.list[2].num;
        }
        if (x.list.size() > 3 && x.list[3].kind == sc::Datum::Num && std::isfinite(x.list[3].num) && x.list[3].num > 0)
            lane.h = (int)std::lround(std::min(x.list[3].num, 4000.0));
        out.push_back(lane);
    }
    return out;
}

void writeLanes(sc::Row& r, const std::vector<Lane>& lanes) {
    if (lanes.empty()) { sc::eraseField(r.extra, "lanes"); return; }
    std::vector<sc::Datum> l;
    for (auto& x : lanes) {
        std::vector<sc::Datum> e{sc::Datum::symbol(x.name), sc::Datum::number(x.lo), sc::Datum::number(x.hi)};
        if (x.h > 0) e.push_back(sc::Datum::number(x.h));
        l.push_back(sc::Datum::of(std::move(e)));
    }
    sc::setField(r.extra, "lanes", sc::Datum::of(std::move(l)));
}

}  // namespace

// ---------------------------------------------------------------- tracks

std::vector<std::string> tracks(const sc::Score& s) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (auto& r : s.rows)
        if (seen.insert(r.name).second) out.push_back(r.name);
    bool none = false;
    for (auto& e : s.events()) {
        if (e->row.empty()) { none = none || !seen.count(""); continue; }
        if (seen.insert(e->row).second) out.push_back(e->row);
    }
    if (none) out.push_back("");
    return out;
}

int trackIndex(const sc::Score& s, const std::string& name) {
    auto t = tracks(s);
    auto it = std::find(t.begin(), t.end(), name);
    return it == t.end() ? -1 : (int)(it - t.begin());
}

std::string addTrack(ScoreDoc& d, const std::string& given) {
    const sc::Score& s = d.score();
    auto all = tracks(s);
    std::string name = given;
    if (name.empty())
        for (int k = (int)all.size() + 1;; ++k) {
            name = "track " + std::to_string(k);
            if (std::find(all.begin(), all.end(), name) == all.end()) break;
        }
    if (std::find(all.begin(), all.end(), name) != all.end()) { d.message = "there is a track " + name + " already"; return ""; }
    auto rows = declaredRows(s);
    rows.push_back(sc::Row{name, "", {}});
    auto e = d.edit("Add Track");
    e.setRows(std::move(rows));
    return e.commit() ? name : "";
}

bool renameTrack(ScoreDoc& d, const std::string& from, const std::string& to) {
    const sc::Score& s = d.score();
    if (from == to) return fail(d, "the same name");
    if (to.empty()) return fail(d, "a track needs a name");
    if (trackIndex(s, to) >= 0) return fail(d, "there is a track " + to + " already");
    if (trackIndex(s, from) < 0) return fail(d, "no track " + from);
    auto rows = declaredRows(s);
    for (auto& r : rows)
        if (r.name == from) r.name = to;
    auto e = d.edit("Rename Track");
    e.setRows(std::move(rows));
    for (auto& ev : s.events())
        if (ev->row == from) {
            sc::Event now = *ev;
            now.row = to;
            e.replace(now);
        }
    return e.commit();
}

bool moveTrack(ScoreDoc& d, const std::string& name, int delta) {
    auto rows = declaredRows(d.score());
    int k = -1;
    for (int i = 0; i < (int)rows.size(); ++i)
        if (rows[(size_t)i].name == name) k = i;
    if (k < 0) return fail(d, "no track " + name);
    int to = std::clamp(k + delta, 0, (int)rows.size() - 1);
    if (to == k) return fail(d, "it is there already");
    sc::Row r = rows[(size_t)k];
    rows.erase(rows.begin() + k);
    rows.insert(rows.begin() + to, r);
    auto e = d.edit("Move Track");
    e.setRows(std::move(rows));
    return e.commit();
}

bool deleteTrack(ScoreDoc& d, const std::string& name) {
    const sc::Score& s = d.score();
    if (trackIndex(s, name) < 0) return fail(d, "no track " + name);
    auto rows = declaredRows(s);
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const sc::Row& r) { return r.name == name; }), rows.end());
    std::vector<uint64_t> ids;
    for (auto& ev : s.events())
        if (ev->row == name) ids.push_back(ev->id);
    auto e = d.edit("Delete Track");
    for (uint64_t id : ids) e.remove(id);
    e.setRows(std::move(rows));
    return e.commit();
}

sc::Strip stripOf(const sc::Score& s, const std::string& track) {
    const sc::Row* r = rowOf(s, track);
    return r ? sc::stripOf(*r) : sc::Strip{};
}

bool setStrip(ScoreDoc& d, const std::string& track, const sc::Strip& st) {
    if (!(st.gain >= 0) || !std::isfinite(st.gain) || !std::isfinite(st.az)) return fail(d, "a gain of 0 or more, an azimuth in degrees");
    auto rows = declaredRows(d.score());
    bool found = false;
    for (auto& r : rows)
        if (r.name == track) { sc::setStrip(r, st); found = true; }
    if (!found) return fail(d, "no track " + track);
    sc::Strip old = stripOf(d.score(), track);
    const char* what = old.mute != st.mute ? "Mute" : old.solo != st.solo ? "Solo" : old.gain != st.gain ? "Track Gain" : "Track Pan";
    auto e = d.edit(what);
    e.setRows(std::move(rows));
    return e.commit();
}

// ---------------------------------------------------------------- heights

int trackHeight(const sc::Score& s, const std::string& track) {
    const sc::Row* r = rowOf(s, track);
    if (!r) return 0;
    double h = sc::numField(r->extra, "height", 0);
    return h > 0 ? (int)std::lround(std::min(h, 4000.0)) : 0;
}

bool setHeights(ScoreDoc& d, const std::vector<Height>& hs) {
    const sc::Score& s = d.score();
    auto rows = declaredRows(s);
    bool changed = false;
    for (auto& h : hs) {
        int v = std::max(0, std::min(h.h, 4000));
        auto it = std::find_if(rows.begin(), rows.end(), [&](const sc::Row& r) { return r.name == h.track; });
        if (it == rows.end()) return fail(d, "no track " + h.track);
        if (h.lane.empty()) {
            if (trackHeight(s, h.track) == v) continue;
            if (v > 0) sc::setField(it->extra, "height", sc::Datum::number(v));
            else sc::eraseField(it->extra, "height");
            changed = true;
            continue;
        }
        auto ls = declaredLanes(&*it);
        for (auto& l : lanes(s, h.track))           // the lanes shown (some only by their points) are kept
            if (std::none_of(ls.begin(), ls.end(), [&](const Lane& x) { return x.name == l.name; })) ls.push_back(l);
        auto lt = std::find_if(ls.begin(), ls.end(), [&](const Lane& l) { return l.name == h.lane; });
        if (lt == ls.end()) return fail(d, "no lane " + h.lane + " on " + h.track);
        if (lt->h == v) continue;
        lt->h = v;
        writeLanes(*it, ls);
        changed = true;
    }
    if (!changed) return fail(d, "the same height");
    auto e = d.edit("Track Height");
    e.setRows(std::move(rows));
    return e.commit();
}

// ---------------------------------------------------------------- lanes

std::vector<Lane> lanes(const sc::Score& s, const std::string& track) {
    std::vector<Lane> out = declaredLanes(rowOf(s, track));
    for (auto& e : s.events()) {
        if (e->kind != sc::Kind::Message || e->row != track) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const Lane& l) { return l.name == e->source; });
        double v = pointValue(*e);
        if (it == out.end()) {
            Lane l;
            l.name = e->source;
            l.lo = std::min(0.0, v);
            l.hi = std::max(1.0, v);
            out.push_back(l);
            continue;
        }
    }
    // a declared range that a point is outside of is widened (what is drawn, not what is kept)
    for (auto& l : out)
        for (auto* p : points(s, track, l.name)) {
            double v = pointValue(*p);
            l.lo = std::min(l.lo, v);
            l.hi = std::max(l.hi, v);
        }
    return out;
}

bool addLane(ScoreDoc& d, const std::string& track, const std::string& name, double lo, double hi) {
    if (name.empty()) return fail(d, "a lane is named after the receive it drives");
    if (!(hi > lo)) return fail(d, "a lane's range goes from a low value to a higher one");
    auto rows = declaredRows(d.score());
    for (auto& r : rows) {
        if (r.name != track) continue;
        auto ls = declaredLanes(&r);
        for (auto& l : lanes(d.score(), track))     // the lanes shown (some only by their points) are kept
            if (std::none_of(ls.begin(), ls.end(), [&](const Lane& x) { return x.name == l.name; })) ls.push_back(l);
        if (std::any_of(ls.begin(), ls.end(), [&](const Lane& x) { return x.name == name; })) return fail(d, "there is a lane " + name + " already");
        ls.push_back(Lane{name, lo, hi});
        writeLanes(r, ls);
        auto e = d.edit("Add Lane");
        e.setRows(std::move(rows));
        return e.commit();
    }
    return fail(d, "no track " + track);
}

bool removeLane(ScoreDoc& d, const std::string& track, const std::string& name) {
    auto rows = declaredRows(d.score());
    std::vector<uint64_t> ids;
    for (auto* p : points(d.score(), track, name)) ids.push_back(p->id);
    for (auto& r : rows) {
        if (r.name != track) continue;
        auto ls = declaredLanes(&r);
        ls.erase(std::remove_if(ls.begin(), ls.end(), [&](const Lane& l) { return l.name == name; }), ls.end());
        writeLanes(r, ls);
    }
    auto e = d.edit("Remove Lane");
    for (uint64_t id : ids) e.remove(id);
    e.setRows(std::move(rows));
    return e.commit();
}

std::vector<const sc::Event*> points(const sc::Score& s, const std::string& track, const std::string& lane) {
    std::vector<const sc::Event*> out;
    for (const sc::Event* e : s.byTime())
        if (e->kind == sc::Kind::Message && e->row == track && e->source == lane) out.push_back(e);
    return out;
}

double pointValue(const sc::Event& e) {
    for (auto& v : e.values)
        if (v.kind == sc::Datum::Num) return v.num;
    return 0;
}

uint64_t addPoint(ScoreDoc& d, const std::string& track, const std::string& lane, double t, double v) {
    if (!std::isfinite(t) || !std::isfinite(v)) { d.message = "a point needs a time and a value"; return 0; }
    auto e = std::make_shared<sc::Event>();
    e->kind = sc::Kind::Message;
    e->at = std::max(0.0, t);
    e->row = track;
    e->source = lane;
    e->values = {sc::Datum::number(v)};
    auto ed = d.edit("Add Point");
    uint64_t id = ed.insert(e);
    return ed.commit() ? id : 0;
}

bool movePoint(ScoreDoc& d, uint64_t id, double t, double v) {
    auto cur = d.score().find(id);
    if (!cur || cur->kind != sc::Kind::Message) return fail(d, "no such point");
    sc::Event now = *cur;
    now.at = std::max(0.0, t);
    bool set = false;
    for (auto& x : now.values)
        if (x.kind == sc::Datum::Num) { x.num = v; set = true; break; }
    if (!set) now.values.insert(now.values.begin(), sc::Datum::number(v));
    return change(d, now, "Move Point");
}

// ---------------------------------------------------------------- clips

bool isClip(const sc::Event& e) { return e.kind != sc::Kind::Message; }

void crossfadeOverlaps(ScoreDoc::Edit& e, const sc::Score& s) {
    if (sc::numField(s.extra, "auto-crossfade", 0) == 0) return;
    std::map<std::string, std::vector<const sc::Event*>> byTrack;
    for (auto& ep : s.events())
        if (ep->kind == sc::Kind::Sound) byTrack[ep->row].push_back(ep.get());
    for (auto& kv : byTrack) {
        auto& v = kv.second;
        std::stable_sort(v.begin(), v.end(), [](const sc::Event* a, const sc::Event* b) { return a->at < b->at; });
        for (size_t k = 0; k + 1 < v.size(); ++k) {
            const sc::Event *a = v[k], *b = v[k + 1];
            double overlap = std::min(a->end(), b->end()) - b->at;
            if (!(overlap > 1e-9) || !(b->at > a->at)) continue;
            if (sc::numField(a->extra, "fade-out", 0) < overlap - 1e-9) {
                sc::Event na = *a;
                sc::setField(na.extra, "fade-out", sc::Datum::number(overlap));
                sc::setField(na.extra, "fade-out-shape", sc::Datum::symbol("equal"));
                e.replace(na);
            }
            if (sc::numField(b->extra, "fade-in", 0) < overlap - 1e-9) {
                sc::Event nb = *b;
                sc::setField(nb.extra, "fade-in", sc::Datum::number(overlap));
                sc::setField(nb.extra, "fade-in-shape", sc::Datum::symbol("equal"));
                e.replace(nb);
            }
        }
    }
}

bool moveClips(ScoreDoc& d, const std::vector<uint64_t>& ids, double dt, int dTrack) {
    const sc::Score& s = d.score();
    auto all = tracks(s);
    double first = 1e300;
    for (uint64_t id : ids)
        if (auto e = s.find(id)) first = std::min(first, e->at);
    if (first == 1e300) return fail(d, "nothing selected");
    dt = std::max(dt, -first);                          // nothing before 0
    if (dt == 0 && dTrack == 0) return fail(d, "not moved");
    auto e = d.edit(dTrack ? "Move to Track" : "Move");
    for (uint64_t id : ids) {
        auto cur = s.find(id);
        if (!cur) continue;
        sc::Event now = *cur;
        now.at = std::max(0.0, now.at + dt);
        if (dTrack && !all.empty()) {
            int k = trackIndex(s, now.row);
            if (k >= 0) now.row = all[(size_t)std::clamp(k + dTrack, 0, (int)all.size() - 1)];
        }
        e.replace(now);
    }
    crossfadeOverlaps(e, s);
    return e.commit();
}

bool trimStart(ScoreDoc& d, uint64_t id, double at) {
    auto cur = d.score().find(id);
    if (!cur) return fail(d, "no such clip");
    if (cur->kind == sc::Kind::Score) return fail(d, "a placed score is shortened from its end (its start is where it starts)");
    if (cur->kind == sc::Kind::Message) return fail(d, "a message has no length");
    sc::Event now = *cur;
    double delta = at - now.at;
    delta = std::min(delta, now.dur - kMinDur);
    delta = std::max(delta, -now.at);
    if (now.kind == sc::Kind::Sound) {
        delta = std::max(delta, -now.from);                // not before the start of its source
        now.from += delta;
        if (now.to < 0) now.to = cur->from + cur->dur;
    }
    if (delta == 0) return fail(d, "not trimmed");
    now.at += delta;
    now.dur -= delta;
    return change(d, now, "Trim Start");
}

bool trimEnd(ScoreDoc& d, uint64_t id, double end) {
    auto cur = d.score().find(id);
    if (!cur) return fail(d, "no such clip");
    if (cur->kind == sc::Kind::Message) return fail(d, "a message has no length");
    sc::Event now = *cur;
    now.dur = std::max(kMinDur, end - now.at);
    if (now.kind == sc::Kind::Sound) now.to = now.from + now.dur;
    if (now.dur == cur->dur) return fail(d, "not trimmed");
    return change(d, now, "Trim End");
}

std::vector<uint64_t> split(ScoreDoc& d, const std::vector<uint64_t>& ids, double t) {
    const sc::Score& s = d.score();
    std::vector<uint64_t> made;
    auto e = d.edit("Split");
    for (uint64_t id : ids) {
        auto cur = s.find(id);
        if (!cur || cur->kind != sc::Kind::Sound || !(t > cur->at + kMinDur) || !(t < cur->end() - kMinDur)) continue;
        sc::Event left = *cur;
        auto right = std::make_shared<sc::Event>(*cur);
        double cut = t - cur->at;
        double srcEnd = cur->to >= 0 ? cur->to : cur->from + cur->dur;
        left.dur = cut;
        left.to = left.from + cut;
        sc::eraseField(left.extra, "fade-out");
        right->id = 0;
        right->at = t;
        right->dur = cur->dur - cut;
        right->from = cur->from + cut;
        right->to = srcEnd;
        sc::eraseField(right->extra, "fade-in");
        e.replace(left);
        if (uint64_t n = e.insert(right)) made.push_back(n);
    }
    if (made.empty()) {
        d.message = "Split: select a sound clip under the playhead";
        return {};
    }
    if (!e.commit()) return {};
    return made;
}

bool change(ScoreDoc& d, const sc::Event& now, const std::string& what) {
    auto e = d.edit(what);
    e.replace(now);
    if (what != "Fades") crossfadeOverlaps(e, d.score());    // fades set by hand stay as they are set
    return e.commit();
}

bool removeClips(ScoreDoc& d, const std::vector<uint64_t>& ids, const std::string& what) {
    if (ids.empty()) return fail(d, "nothing selected");
    auto e = d.edit(what);
    for (uint64_t id : ids) e.remove(id);
    return e.commit();
}

bool makeUnique(ScoreDoc& d, uint64_t id) {
    auto cur = d.score().find(id);
    if (!cur || cur->kind != sc::Kind::Score || !cur->sub) return fail(d, "Make Unique: select a placed score");
    sc::Event now = *cur;
    now.sub = cur->sub->clone();                  // its events copied; the scores inside it still shared
    now.sub->path.clear();
    now.ref.clear();                              // embedded: kept in this score from now on
    return change(d, now, "Make Unique");
}

int instances(const sc::Score& top, const sc::Score* sub, int* which, uint64_t id) {
    int n = 0;
    for (const sc::Event* e : top.byTime())
        if (e->kind == sc::Kind::Score && e->sub.get() == sub) {
            ++n;
            if (which && e->id == id) *which = n;
        }
    return n;
}

// ---------------------------------------------------------------- the clipboard

Clips& clipboard() {
    static Clips c;
    return c;
}

Clips copy(const sc::Score& s, const std::vector<uint64_t>& ids) {
    Clips c;
    c.origin = 1e300;
    for (uint64_t id : ids)
        if (auto e = s.find(id)) {
            c.events.push_back(*e);
            c.origin = std::min(c.origin, e->at);
        }
    if (c.events.empty()) c.origin = 0;
    return c;
}

std::vector<uint64_t> paste(ScoreDoc& d, const Clips& c, double at, const std::string& track) {
    std::vector<uint64_t> made;
    if (c.empty()) { d.message = "nothing to paste"; return made; }
    std::set<std::string> rows;
    for (auto& e : c.events) rows.insert(e.row);
    auto e = d.edit("Paste");
    for (auto& x : c.events) {
        auto n = std::make_shared<sc::Event>(x);
        n->id = 0;
        n->at = std::max(0.0, at + (x.at - c.origin));
        if (!track.empty() && rows.size() == 1) n->row = track;
        if (uint64_t id = e.insert(n)) made.push_back(id);
    }
    crossfadeOverlaps(e, d.score());
    if (!e.commit()) return {};
    d.select(made);
    return made;
}

std::vector<uint64_t> duplicate(ScoreDoc& d, const std::vector<uint64_t>& ids) {
    Clips c = copy(d.score(), ids);
    if (c.empty()) { d.message = "nothing selected"; return {}; }
    double end = 0;
    for (auto& e : c.events) end = std::max(end, e.end());
    auto made = paste(d, c, end);
    if (!made.empty()) d.message = "Duplicate";
    return made;
}

// ---------------------------------------------------------------- adding

std::string sourceName(const ScoreDoc& d, const std::string& path) {
    std::string dir = d.folder();
    if (!dir.empty() && dir != "." && path.size() > dir.size() + 1 && path.compare(0, dir.size(), dir) == 0 &&
        (path[dir.size()] == '/' || path[dir.size()] == '\\'))
        return path.substr(dir.size() + 1);
    return path;
}

std::string sourcePath(const ScoreDoc& d, const std::string& source) {
    if (source.empty() || isAbsolutePath(source)) return source;
    std::string dir = d.folder();
    if (!dir.empty()) {
        std::string f = dir + "/" + source;
        if (std::FILE* fp = std::fopen(f.c_str(), "rb")) { std::fclose(fp); return f; }
    }
    if (Box* b = d.box()) return b->patch->resolvePath(source);
    for (auto& folder : soundPath()) {
        std::string f = folder + "/" + source;
        if (std::FILE* fp = std::fopen(f.c_str(), "rb")) { std::fclose(fp); return f; }
    }
    return source;
}

uint64_t addSound(ScoreDoc& d, const std::string& source, bool buffer, const std::string& track, double at, double dur) {
    if (source.empty() || !(dur > 0) || !std::isfinite(at)) { d.message = "a sound needs a file (or a buffer~) and a length"; return 0; }
    auto e = std::make_shared<sc::Event>();
    e->kind = sc::Kind::Sound;
    e->buffer = buffer;
    e->source = buffer ? source : sourceName(d, source);
    e->at = std::max(0.0, at);
    e->dur = dur;
    e->from = 0;
    e->to = dur;
    e->row = track;
    auto ed = d.edit("Add Sound");
    uint64_t id = ed.insert(e);
    crossfadeOverlaps(ed, d.score());
    if (!ed.commit()) return 0;
    d.select({id});
    return id;
}

uint64_t addSubScore(ScoreDoc& d, const std::string& file, const std::string& track, double at) {
    std::shared_ptr<sc::Score> sub;
    std::string err;
    std::string path = sc::normalPath(file);
    if (auto open = ScoreDoc::open(path, err)) sub = open->scorePtr();   // the open one, if it is: one score
    if (!sub) { d.message = err; return 0; }
    auto e = std::make_shared<sc::Event>();
    e->kind = sc::Kind::Score;
    e->sub = sub;
    e->ref = sourceName(d, path);
    e->at = std::max(0.0, at);
    double len = sub->duration();
    e->dur = len > 0 ? len : beatSeconds(d.score()) * d.score().meterNum;
    e->row = track;
    auto ed = d.edit("Add Score");
    uint64_t id = ed.insert(e);                   // a score that contains this one is refused here
    if (!ed.commit()) return 0;
    d.select({id});
    return id;
}

uint64_t addPatchClip(ScoreDoc& d, const std::string& file, const std::string& track, double at, double dur) {
    if (file.empty()) { d.message = "a patch clip names a patch"; return 0; }
    auto e = std::make_shared<sc::Event>();
    e->kind = sc::Kind::Patch;
    e->source = sourceName(d, file);
    e->at = std::max(0.0, at);
    e->dur = dur > 0 ? dur : beatSeconds(d.score()) * d.score().meterNum;
    e->row = track;
    auto ed = d.edit("Add Patch");
    uint64_t id = ed.insert(e);
    if (!ed.commit()) return 0;
    d.select({id});
    return id;
}

// ---------------------------------------------------------------- snapping

double beatSeconds(const sc::Score& s) { return s.bpm > 0 ? 60.0 / s.bpm : 1.0; }

double snap(const sc::Score& s, double t, Snap mode) {
    if (mode == Snap::Off || !std::isfinite(t)) return t;
    double step = beatSeconds(s) * (mode == Snap::Bar ? std::max(1, s.meterNum) : 1);
    return std::max(0.0, std::round(t / step) * step);
}

std::string bbt(const sc::Score& s, double t) {
    auto b = sc::toBBT(t, s.bpm, s.meterNum);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%ld.%d.%03d", b.bar, b.beat, b.tick);
    return buf;
}

}  // namespace timeline
}  // namespace moritz
