// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// timeline_ops.h — what the timeline does to a score, without the interface: each operation is one Edit of a
// ScoreDoc (score_doc.h), so one undo step, and says in the document's message why it did nothing when it did not.
//
// The model, as the timeline shows it:
//   tracks    the score's rows, in their order (then the rows that only events name, then the events with none).
//             A track's mixer strip (gain, az, mute, solo) is kept on its row (score.h's Strip): the score box plays it.
//   clips     the events of a track: sounds (a region of a file or a buffer~: from, to; fades in its extra), notes,
//             sub-scores (a placement: start, length, stretch, transpose, gain; an instance shared with the other
//             placements of the same score until Make Unique copies it), patches (they do not run yet).
//   lanes     the automation of a track: the message events of its row, one lane per name (the receive name they
//             drive), each point one message (name value) at its time. A lane's range and order are kept on the row
//             (extra "lanes": (list (list name lo hi) ...)), so a lane without points stays.
// Times are seconds, as in the score; snapping to beats and bars counts them at the score's tempo and meter.
#pragma once

#include "score_doc.h"

#include <string>
#include <vector>

namespace moritz {
namespace timeline {

// ---- tracks
std::vector<std::string> tracks(const sc::Score& s);
int trackIndex(const sc::Score& s, const std::string& name);    // -1: none
// a new track (named name, or "track N"); its name ("" when refused)
std::string addTrack(ScoreDoc& d, const std::string& name = "");
bool renameTrack(ScoreDoc& d, const std::string& from, const std::string& to);   // its row and its events
bool moveTrack(ScoreDoc& d, const std::string& name, int delta);                   // up (-1) or down (+1)
bool deleteTrack(ScoreDoc& d, const std::string& name);                            // its row and its events
sc::Strip stripOf(const sc::Score& s, const std::string& track);
bool setStrip(ScoreDoc& d, const std::string& track, const sc::Strip& st);

// ---- lanes
struct Lane {
    std::string name;
    double lo = 0, hi = 1;
};
std::vector<Lane> lanes(const sc::Score& s, const std::string& track);
bool addLane(ScoreDoc& d, const std::string& track, const std::string& name, double lo = 0, double hi = 1);
bool removeLane(ScoreDoc& d, const std::string& track, const std::string& name);   // and its points
// the points of a lane, by time
std::vector<const sc::Event*> points(const sc::Score& s, const std::string& track, const std::string& lane);
uint64_t addPoint(ScoreDoc& d, const std::string& track, const std::string& lane, double t, double v);
bool movePoint(ScoreDoc& d, uint64_t id, double t, double v);
double pointValue(const sc::Event& e);           // its first number (0 when none)

// ---- clips
bool isClip(const sc::Event& e);                 // shown on the track itself (not a lane point)
// moved by dt seconds (none before 0) and dTrack tracks (clamped to the tracks there are)
bool moveClips(ScoreDoc& d, const std::vector<uint64_t>& ids, double dt, int dTrack = 0);
bool trimStart(ScoreDoc& d, uint64_t id, double at);    // the start to at (a sound: from follows)
bool trimEnd(ScoreDoc& d, uint64_t id, double end);     // the end to end (a sound: to follows)
// the sound clips among ids that t falls inside, cut in two there; the ids of the right halves
std::vector<uint64_t> split(ScoreDoc& d, const std::vector<uint64_t>& ids, double t);
bool change(ScoreDoc& d, const sc::Event& now, const std::string& what);   // one event given new fields
bool removeClips(ScoreDoc& d, const std::vector<uint64_t>& ids, const std::string& what = "Delete");
bool makeUnique(ScoreDoc& d, uint64_t id);              // a sub-score instance becomes a copy of its own (embedded)
int instances(const sc::Score& top, const sc::Score* sub, int* which = nullptr, uint64_t id = 0);   // placements of sub

// ---- the clipboard of clips (copy in one timeline, paste in another)
struct Clips {
    std::vector<sc::Event> events;               // copies, without their score
    double origin = 0;                           // the earliest onset
    bool empty() const { return events.empty(); }
};
Clips& clipboard();
Clips copy(const sc::Score& s, const std::vector<uint64_t>& ids);
// at: where the earliest goes; track: the one they go to ("" their own); the new ids (selected)
std::vector<uint64_t> paste(ScoreDoc& d, const Clips& c, double at, const std::string& track = "");
std::vector<uint64_t> duplicate(ScoreDoc& d, const std::vector<uint64_t>& ids);   // right after them

// ---- adding (each one undo step; 0 when refused, the document's message says why)
uint64_t addSound(ScoreDoc& d, const std::string& source, bool buffer, const std::string& track, double at, double dur);
uint64_t addSubScore(ScoreDoc& d, const std::string& file, const std::string& track, double at);
uint64_t addPatchClip(ScoreDoc& d, const std::string& file, const std::string& track, double at, double dur);
// a path as the score keeps it: relative to its folder when inside it
std::string sourceName(const ScoreDoc& d, const std::string& path);
// a source as a path to read (relative: next to the score, then the patch's search)
std::string sourcePath(const ScoreDoc& d, const std::string& source);

// ---- snapping
enum class Snap { Off, Beat, Bar };
double snap(const sc::Score& s, double t, Snap mode);
double beatSeconds(const sc::Score& s);
std::string bbt(const sc::Score& s, double t);   // "1.1.000"

}  // namespace timeline
}  // namespace moritz
