// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_export.h — a score written as MusicXML or as a standard MIDI file, by Musil's writers (one implementation,
// in Musil): MusicXML by music.mu's score-notate and musicxml-text, on an interpreter of its own and on its thread (the
// quantization can take a while; notation-report prints what it did to the Console); MIDI by music/midi.h's
// score_midi, at once (a track per part; microtones rounded, or each note on a channel of its own with a pitch bend).
// The scores placed inside are brought to the top first, so that their notes are written. Files are written whole or
// not at all (fileio::writeAtomic). The score window's File > Export MusicXML... and Export MIDI..., and the score
// box's export message, call it.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace musil { namespace score { struct Score; } }

namespace moritz {

enum class ScoreFormat { MusicXML, Midi };

// "musicxml" (or "xml") and "midi" (or "mid"); false: neither
bool parseScoreFormat(const std::string& word, ScoreFormat& f);
// the extension of a format's files, with its dot
const char* scoreFormatExtension(ScoreFormat f);

struct ScoreExportOptions {
    bool bend = false;              // MIDI: microtones by pitch bend (else rounded to the semitone)
};

// how many notes of s (and of the scores placed in it) sound between two semitones (more than half a cent off)
int microtonalNotes(const musil::score::Score& s);

struct ScoreExported {
    bool ok = false;
    std::string message;            // what was written, or why nothing was
    int notes = 0;                  // the notes written
};

// Writes s as f at path (absolute). done runs on the main thread (at once under moritz --render, where jobs are
// synchronous). MusicXML runs on a thread of its own; the score is copied first, so the caller may change its own at once.
void exportScore(const musil::score::Score& s, ScoreFormat f, const std::string& path, const ScoreExportOptions& o,
                 std::function<void(const ScoreExported&)> done);

}  // namespace moritz
