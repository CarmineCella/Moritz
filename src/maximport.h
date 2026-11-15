// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// maximport.h — converting Max patches (.maxpat, .maxhelp) to Moritz
#pragma once

#include <map>
#include <string>
#include <vector>

namespace moritz {

struct MaxImportReport {
    int boxes = 0, cables = 0, skipped = 0;
    std::map<std::string, int> unknown;      // Max classes with no Moritz equivalent
    std::map<std::string, int> renamed;      // ezdac~ -> dac~ ...
    std::map<std::string, int> attributes;   // "metro @active" ...
    std::map<std::string, int> lost;         // details that did not come along
    std::vector<std::string> abstractions;   // .moritz files written
    std::vector<std::string> kept;           // .moritz files that existed already
    std::vector<std::string> problems;
    std::string summary() const;
};

// Converts a Max patch into the text of a Moritz patch. Abstractions it uses
// that sit next to it as .maxpat files are converted too, and written beside
// them as .moritz files when writeAbstractions is true.
bool importMaxPatch(const std::string& path, bool writeAbstractions, std::string& out,
                    MaxImportReport& report, std::string& err);

}  // namespace moritz
