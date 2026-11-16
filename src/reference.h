// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// reference.h — the reference of every object: its syntax, what it does, its
// arguments and messages (help/reference/*.txt), and its inlets and outlets
// (ports.cpp). Shown in the Reference window and in the Browser, and turned
// into OBJECTS.md (moritz --reference-md).
//
// The files hold entries like this one:
//
//   @ buffer~
//   syntax: buffer~ name [file] [ms] [channels]
//   Named sample memory, shared by name with play~, groove~ and record~.
//   arg name: the name the other objects use
//   arg file: a sound file read when the patch opens
//   msg read file: reads a sound file (WAV, AIFF, FLAC, MP3)
//   msg clear: sets every sample to 0
//   see: play~ groove~ record~ waveform~
//
// "@ +~ -~ *~ /~" gives several objects one entry. Other lines are the
// description (an empty line starts a paragraph); "#" starts a comment.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace moritz {

struct RefEntry {
    std::vector<std::string> names;
    std::string syntax, about;
    std::vector<std::pair<std::string, std::string>> args, msgs;   // what, explanation
    std::vector<std::string> see;
};

const RefEntry* findReference(const std::string& cls);      // nullptr when it has none
std::string referenceText(const std::string& cls);          // plain text, with inlets and outlets
std::string referenceMarkdown();                            // OBJECTS.md
std::vector<std::string> classesWithoutReference();         // for checks

}  // namespace moritz
