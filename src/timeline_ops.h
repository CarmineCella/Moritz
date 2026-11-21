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
//             (extra "lanes": (list (list name lo hi [height]) ...)), so a lane without points stays.
//             A lane ramps from one point to the next (linear: its values sent at kRampRate while it plays) or holds
//             each point's value until the next (step): the fifth item of its entry (linear or step); without it, a lane
//             of the strip ramps (strip.gain, strip.pan, strip.send.<bus>) and any other steps, as in the files made
//             before ramps (a lane added from the timeline ramps; strip.mute always steps).
//   markers   named times on the ruler, in the score's extra (score.h's Marker); a section is the span from one to the next
//   heights   how high the timeline draws a track (its row's extra "height") and each of its lanes (the fourth number
//             of its entry), in pixels; none: the view's default. Kept with the score, so saved with it.
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
bool moveTrackTo(ScoreDoc& d, const std::string& name, int index);                // to that place among the tracks
sc::Strip stripOf(const sc::Score& s, const std::string& track);
bool setStrip(ScoreDoc& d, const std::string& track, const sc::Strip& st);

// ---- lanes
enum class Ramp { Step, Linear };
constexpr double kRampRate = 100;                // the values a linear lane sends a second, between its points
Ramp defaultRamp(const std::string& lane);       // a lane's when its entry says none (the files made before ramps)
struct Lane {
    std::string name;
    double lo = 0, hi = 1;
    int h = 0;                                   // its height (0: the default)
    Ramp ramp = Ramp::Step;
};
std::vector<Lane> lanes(const sc::Score& s, const std::string& track);
Ramp laneRamp(const sc::Score& s, const std::string& track, const std::string& lane);
std::vector<Lane> rowLanes(const sc::Row& r);    // the lanes a row declares (not those only its points make)
bool addLane(ScoreDoc& d, const std::string& track, const std::string& name, double lo = 0, double hi = 1, Ramp ramp = Ramp::Linear);
bool setLaneRamp(ScoreDoc& d, const std::string& track, const std::string& lane, Ramp ramp);   // one undo step
bool removeLane(ScoreDoc& d, const std::string& track, const std::string& name);   // and its points
// ---- heights (pixels; 0: the view's default)
int trackHeight(const sc::Score& s, const std::string& track);
struct Height {
    std::string track, lane;                     // lane "": the track itself
    int h = 0;
};
bool setHeights(ScoreDoc& d, const std::vector<Height>& hs);   // one undo step ("Track Height")

// the points of a lane, by time
std::vector<const sc::Event*> points(const sc::Score& s, const std::string& track, const std::string& lane);
uint64_t addPoint(ScoreDoc& d, const std::string& track, const std::string& lane, double t, double v);
bool movePoint(ScoreDoc& d, uint64_t id, double t, double v);
double pointValue(const sc::Event& e);           // its first number (0 when none)

// ---- clips
bool isClip(const sc::Event& e);                 // shown on the track itself (not a lane point)
// sound clips overlapping on a track crossfade (equal power) over their overlap: their fades written (made as long as
// the overlap when shorter) into an edit going on, when the score says so (extra auto-crossfade: the multitrack
// sessions' default); moving, trimming, pasting and adding clips do it
void crossfadeOverlaps(ScoreDoc::Edit& e, const sc::Score& s);
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

// ---- markers (each change one undo step; a marker is named by its index in markers(), by time)
std::vector<sc::Marker> markers(const sc::Score& s);
int addMarker(ScoreDoc& d, double t, const std::string& name = "");   // its index (-1 refused); "": "marker N"
bool renameMarker(ScoreDoc& d, int k, const std::string& name);
bool moveMarker(ScoreDoc& d, int k, double t);
bool removeMarker(ScoreDoc& d, int k);
int markerNear(const sc::Score& s, double t, double tolerance);    // the marker closest to t within tolerance (-1 none)
double nextMarker(const sc::Score& s, double t);                     // the first after t (-1 none)
double previousMarker(const sc::Score& s, double t);                 // the last before t (-1 none)
sc::Section section(const sc::Score& s, double t);                   // the span from its marker to the next

// ---- snapping
enum class Snap { Off, Beat, Bar };
double snap(const sc::Score& s, double t, Snap mode);
double beatSeconds(const sc::Score& s);
std::string bbt(const sc::Score& s, double t);   // "1.1.000"

}  // namespace timeline
}  // namespace moritz
