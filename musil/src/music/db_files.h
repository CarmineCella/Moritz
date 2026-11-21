// db_files.h — finding a database's sound files on disk: a folder walked safely, and an index of every sound under a
// folder by file name, shared by the interpreters of the process; what music.h's databases and the score's notes (a
// note's sample, score_musil.h) look sounds up with.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause

#pragma once
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace musil {

// every regular file under a folder, depth first in the order the system lists them: a folder that cannot be read
// (permissions, a system folder the sandbox hides) is skipped, never an exception; symbolic links to folders are not
// followed; 64 levels and a million entries at most (a feature file put in a huge folder does not walk the disk)
inline void db_walk(const std::filesystem::path& root, const std::function<void(const std::filesystem::path&)>& file) {
    namespace fs = std::filesystem; size_t seen = 0;
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& d, int depth) {
        std::error_code ec; fs::directory_iterator it(d, ec), end;
        for (; !ec && it != end && seen < 1000000; it.increment(ec)) {
            seen++; std::error_code e2;
            if (it->is_symlink(e2) && it->is_directory(e2)) continue;
            if (it->is_directory(e2)) { if (depth < 64) walk(it->path(), depth + 1); }
            else if (it->is_regular_file(e2)) file(it->path());
        }
    };
    walk(root, 0);
}

// --- finding the sounds: an index of every WAV under a folder, by file name ------------------------
// The paths in a feature file and the folders on disk do not always agree (TinySOL's file says
// /Strings/Violin/ordinario/..., a copy has Strings/Vn/ordinario/...; a sharp is # in one and _ in the
// other), so a sound is found by its file name: the folder is walked once and indexed by name, with the
// case and the spelling of sharps ignored.
inline std::string db_norm_name(std::string s) { size_t sl = s.find_last_of("/\\"); if (sl != std::string::npos) s = s.substr(sl + 1); for (auto& c : s) { c = (char)std::tolower((unsigned char)c); if (c == '#') c = '_'; } return s; }
// The indexes are shared by the interpreters of the process (several run on threads in Moritz): built once per
// folder under a lock, then read-only (a shared pointer to a finished index)
using db_index = std::map<std::string, std::string>;
inline std::shared_ptr<const db_index> db_index_of(const std::string& root) {
    static std::mutex m; static std::map<std::string, std::shared_ptr<const db_index>> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(root); if (it != cache.end()) return it->second;
    auto idx = std::make_shared<db_index>(); std::error_code ec; namespace fs = std::filesystem;
    if (fs::is_directory(root, ec)) db_walk(root, [&](const fs::path& p) { std::string ext = p.extension().string(); for (auto& c : ext) c = (char)std::tolower((unsigned char)c); if (ext == ".wav" || ext == ".aif" || ext == ".aiff") idx->emplace(db_norm_name(p.filename().string()), p.string()); });
    cache[root] = idx;
    return idx;
}
// (db-locate root file) => the path on disk of a sound named file (a path from a feature file) under root, found by its
//   file name whatever the folders are called; "" when it is not there. (db-index-size root) => how many sounds root holds
inline std::string db_locate(const std::string& root, const std::string& rel) {
    std::filesystem::path direct = std::filesystem::path(root) / (rel.size() && (rel[0] == '/' || rel[0] == '\\') ? rel.substr(1) : rel); std::error_code ec;
    if (std::filesystem::exists(direct, ec)) return direct.string();
    auto idx = db_index_of(root); auto it = idx->find(db_norm_name(rel)); return it == idx->end() ? "" : it->second;
}

}  // namespace musil
