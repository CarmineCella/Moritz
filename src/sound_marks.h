// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// sound_marks.h — the markers and regions of a sound in the sound editor, and how they are kept.
//
// A marker is a frame with a name and where it came from (by hand, onsets, beats, novelty). A region is a span of
// frames with a name, a type (a soundtypes type, -1 for none), its energy (RMS, 0..1) and pitch (MIDI, 0 for
// none). They are a value (SoundMarks): the sound document keeps a snapshot that never changes, and each edit of
// them is an undo step holding the snapshots before and after, so undo is exact. An edit of the samples moves them
// with the frames (shift): a marker in a part deleted goes to the edge, a region inside it goes.
//
// Kept with the sound:
//   - a sound file: in a file beside it, <file>.markers (beachboys.wav.markers), written as Musil data (as Moritz
//     writes its other data, .score files among them): forms read with Musil's reader, never evaluated. In
//     seconds, so the file stays right if the sound is resampled. Why not the WAV's cue and label chunks: they
//     hold positions and names only (no type, energy or pitch), AIFF, FLAC and MP3 have none of them, other
//     programs drop or rewrite them, and a marker moved would rewrite the whole sound file. A sidecar is read
//     the same way whatever the format, by Moritz and by a script: (parse (read "beachboys.wav.markers")).
//   - a buffer~: with the buffer~ box, in memory, for as long as the patch is open (BufferBox::marks), and
//     beside the WAV file when it is saved from the editor (Save as WAV...).
//
// The file, every number in seconds:
//   ; markers and regions of beachboys.wav (Musil data: read by Moritz's sound editor, or (parse (read ...)))
//   (sound-marks (version 1) (duration 24.53)
//     (marker 0.42 "onset 1" onsets)
//     (region 0 1.214 "intro" (type 0) (energy 0.42) (pitch 62)))
// Reading checks everything a file from elsewhere could get wrong (its size, the nesting, numbers that are not
// finite, spans out of the sound, too many entries): the problem is said, nothing is half read.
//
// To coll: the regions as rows, one per region (index start end type energy pitch name), a list of lists in Musil
// data, which the coll object (to come) reads with readCollRows; a header row names the columns.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace moritz {

struct Marker {
    long at = 0;                 // frame
    std::string name;
    std::string source = "hand"; // hand onsets beats novelty
};

struct Region {
    long start = 0, end = 0;     // frames, start < end
    std::string name;
    int type = -1;               // a soundtypes type (0: A), -1: none
    double energy = 0;           // RMS of the span, 0..1
    double pitch = 0;            // MIDI (fractional), 0: none found
    std::string source = "hand"; // hand types onsets
};

struct SoundMarks {
    std::vector<Marker> markers; // in time order
    std::vector<Region> regions; // in time order of their starts
    bool empty() const { return markers.empty() && regions.empty(); }
    void sort();
    // the frames [at, at + removed) were replaced by inserted frames: what is after moves, what was inside is
    // clamped to the edge (markers) or cut (regions; one entirely inside goes)
    void shift(long at, long removed, long inserted);
    void clamp(long frames);     // nothing past the end (a sound replaced by a shorter one)
    bool operator==(const SoundMarks& o) const;
};

using MarksPtr = std::shared_ptr<const SoundMarks>;

// "A" for type 0 ... "Z", then "T26" ...; "-" for none
std::string typeName(int type);

constexpr size_t kMaxMarks = 200000;                    // markers or regions in one file
constexpr size_t kMaxMarksFile = 16u << 20;             // bytes

// the sidecar's name for a sound file
std::string marksPath(const std::string& soundPath);
// the text of a .markers file (sr: frames to seconds; duration of the sound in seconds)
std::string marksToText(const SoundMarks& m, double sr, double duration, const std::string& soundName);
// read a .markers text: frames at sr, clamped to frames; false (err says what, with the line) when it is not one
bool marksFromText(const std::string& text, double sr, long frames, SoundMarks& out, std::string& err);
// a sound file's markers: none when it has no sidecar (true); false when the sidecar can't be read
bool readMarksFor(const std::string& soundPath, double sr, long frames, SoundMarks& out, std::string& err);
// write (or, with no marks, remove) the sidecar: a temporary file in the same folder, renamed
bool writeMarksFor(const std::string& soundPath, const SoundMarks& m, double sr, double duration, std::string& err);

// ---- To score: a .score (Musil's score, as the score object reads it) with a sound event for each region, at
// its own time, playing that region of the sound (a file's path, or a buffer~'s name when buffer); the region's
// name is the event's label, its type, energy and pitch its fields
std::string scoreText(const SoundMarks& m, double sr, const std::string& source, bool buffer, const std::string& name);
// a sound file's path as a .score at scorePath names it: relative to the score's folder when the sound is in it or in a
// folder below (x.wav, sounds/x.wav), or one or two folders up (../sounds/x.wav: a project's scores and sounds side by
// side), so that the folder moves with its score (the score object and Musil look next to the score first); else
// absolute (another disk, far apart)
std::string soundPathForScore(const std::string& sound, const std::string& scorePath);

// ---- To coll: the regions as rows (a list of lists, Musil data), and the hook for the coll object
struct CollRow {
    std::vector<std::string> cells;   // as written: numbers as text, strings unquoted
};
std::string collText(const SoundMarks& m, double sr, const std::string& soundName);
// the rows of a coll file (the header row of column names, when there is one, goes to columns); false with err
bool readCollRows(const std::string& text, std::vector<std::string>& columns, std::vector<CollRow>& rows, std::string& err);

}  // namespace moritz
