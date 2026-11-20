// datasets.h — the sample databases by name, in one place that Musil alone and Moritz both look in.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A dataset is a feature file in the *SOL layout (X.spectrum.db next to a folder X/ holding the sounds, see music.h).
// It is found by name: "fullsol", "tinysol", "microsol", or the name of any X.spectrum.db found, in this order:
//   1. a path registered for that name: a dataset that lives elsewhere (FullSOL, proprietary and large, where the
//      user keeps it), pointed at once;
//   2. the datasets folder the user chose;
//   3. the default datasets folder, ~/Documents/Moritz/datasets: the same for Musil alone and for Moritz (Moritz's
//      folder in Documents already holds the user's library, and a code box may write there); visible, backed up, and
//      easy to empty, as a few gigabytes of samples should be;
//   4. what comes with the program: MicroSOL (data/microsol), so that every example runs from a fresh copy.
// In a folder, X.spectrum.db is looked for at its top and one folder down (FullSOL2020_release/FullSOL2020.spectrum.db).
// The name of a feature file is its name without .spectrum.db (or .db), in lower case and without a trailing number:
// FullSOL2020.spectrum.db is fullsol; the full name (fullsol2020) finds it too.
//
// The configuration is one small file in Musil's data syntax, ~/.musil/datasets.mu, read at each use (what Moritz's
// Datasets window writes, a running script sees at once), written atomically (a temporary file in the same folder,
// renamed) and under a mutex (one program's windows and interpreters register at once):
//   folder "/Volumes/Samples/datasets"
//   dataset fullsol "/Users/me/Datasets/FullSOL2020_release/FullSOL2020.spectrum.db"
// It is outside the folders a code box may write (its patch's folder, ~/Documents/Moritz), so a patch cannot point a
// dataset name at other files behind the user's back.
//
// TinySOL is downloaded from Musil's release assets (fetch): curl into a temporary folder inside the datasets folder,
// its size and SHA-256 checked, unzipped there, then moved into place, the sounds' folder first and the feature file
// last, so that a TinySOL.spectrum.db in the folder is always a complete one. Programs run here: curl and unzip (tar on
// Windows), each with a cancel that stops it.

#pragma once
#include "../core.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace musil {

// music.h includes this file after the database file's parser (db_parse) and db_sounds_folder, which install uses
namespace datasets {

namespace fs = std::filesystem;

// a download or an installation stopped by its caller
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("cancelled") {} };

// --- where things are ----------------------------------------------------------------------------------------------
inline std::string home_dir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE"); if (!h || !*h) h = std::getenv("HOME");
#else
    const char* h = std::getenv("HOME");
#endif
    return h && *h ? fs::path(h).generic_string() : std::string();
}
// ~/.musil/datasets.mu
inline std::string default_config() { std::string h = home_dir(); return h.empty() ? "" : h + "/.musil/datasets.mu"; }
// ~/Documents/Moritz/datasets
inline std::string default_folder() { std::string h = home_dir(); return h.empty() ? "" : h + "/Documents/Moritz/datasets"; }

// what a program knows of where to look: the configuration file, the default folder, the feature files it comes with
struct Place {
    std::string config = default_config();
    std::string default_folder = datasets::default_folder();
    std::vector<std::string> bundled;          // MicroSOL's feature file, as the program finds it
};
// the bundled MicroSOL next to a folder of Musil's libraries (musil/src: ../../data) or of the program (../data, data)
inline void add_bundled_near(Place& p, const std::string& dir) {
    if (dir.empty()) return;
    for (const char* up : {"/../../data", "/../data", "/data"}) {
        std::error_code ec; fs::path f = fs::path(dir + up) / "microsol" / "microsol.spectrum.db";
        if (!fs::is_regular_file(f, ec)) continue;
        std::string s = fs::weakly_canonical(f, ec).generic_string(); if (ec) s = f.lexically_normal().generic_string();
        if (std::find(p.bundled.begin(), p.bundled.end(), s) == p.bundled.end()) p.bundled.push_back(s);
    }
}

// --- names -------------------------------------------------------------------------------------------------------
inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// a name as written (FullSOL, TinySOL): lower case
inline std::string name_key(const std::string& s) { return lower(s); }
// without a trailing number or separator: fullsol2020 -> fullsol (itself when nothing would be left)
inline std::string short_key(const std::string& k) {
    size_t e = k.size();
    while (e > 0 && (std::isdigit((unsigned char)k[e - 1]) || k[e - 1] == '-' || k[e - 1] == '_' || k[e - 1] == '.' || k[e - 1] == ' ')) --e;
    return e == 0 ? k : k.substr(0, e);
}
// the name of a feature file: FullSOL2020.spectrum.db -> fullsol2020
inline std::string name_of_file(const std::string& path) {
    std::string n = fs::path(path).filename().string();
    for (const char* ext : {".spectrum.db", ".db"}) {
        size_t l = std::strlen(ext);
        if (n.size() > l && lower(n.substr(n.size() - l)) == ext) { n = n.substr(0, n.size() - l); break; }
    }
    return lower(n);
}
inline bool is_feature_file(const std::string& filename) {
    std::string l = lower(filename);
    return l.size() > 12 && l.compare(l.size() - 12, 12, ".spectrum.db") == 0;
}
// does a name written by a user (fullsol, FullSOL2020) name this dataset?
inline bool matches(const std::string& dataset, const std::string& wanted) {
    std::string w = name_key(wanted);
    return !w.empty() && (dataset == w || short_key(dataset) == w);
}
// is it a dataset's name rather than a path: no folder separator, no .db at its end, not empty
inline bool is_name(const std::string& s) {
    if (s.empty() || s.size() > 200) return false;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
    std::string l = lower(s);
    return !(l.size() >= 3 && l.compare(l.size() - 3, 3, ".db") == 0);
}
// a name a user may register: letters, digits, - _ . (not only dots)
inline bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > 64 || s.find_first_not_of(".") == std::string::npos) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

// --- the configuration file ----------------------------------------------------------------------------------------
struct Config {
    std::string folder;                                              // the datasets folder the user chose ("": the default)
    std::vector<std::pair<std::string, std::string>> registered;     // name (lower case) -> feature file
};
// the file read: a missing file is an empty configuration; a malformed one throws std::runtime_error naming the file
// and the line
inline Config parse_config(const std::string& text, const std::string& shown) {
    Config c;
    vptr prog;
    try {
        Parser p(text, std::make_shared<std::string>(shown)); p.literal = false;
        prog = p.program();
    } catch (const Error& e) {
        throw std::runtime_error("datasets: " + shown + " is not readable: " + std::string(e.what()));
    }
    auto bad = [&](const vptr& line, const std::string& why) {
        throw std::runtime_error("datasets: " + shown + ":" + std::to_string(line ? line->line : 0) + ": " + why +
                                 " (each line is folder \"path\" or dataset name \"path\")");
    };
    for (size_t k = 1; k < prog->l.size(); ++k) {
        const vptr& ln = prog->l[k];
        if (!ln || ln->t != Value::LIST || ln->l.empty() || !ln->l[0] || ln->l[0]->t != Value::SYM) bad(ln, "not a setting");
        const std::string& head = ln->l[0]->s;
        auto text_at = [&](size_t j) -> const std::string& {
            if (j >= ln->l.size() || !ln->l[j] || (ln->l[j]->t != Value::STR && ln->l[j]->t != Value::SYM)) bad(ln, head + ": a text is missing");
            return ln->l[j]->s;
        };
        if (head == "folder") {
            if (ln->l.size() != 2) bad(ln, "folder takes one path");
            c.folder = text_at(1);
        } else if (head == "dataset") {
            if (ln->l.size() != 3) bad(ln, "dataset takes a name and a path");
            std::string n = name_key(text_at(1)), p = text_at(2);
            if (!valid_name(n)) bad(ln, "not a dataset's name: " + n);
            if (p.empty()) bad(ln, "dataset " + n + ": an empty path");
            auto it = std::find_if(c.registered.begin(), c.registered.end(), [&](auto& r) { return r.first == n; });
            if (it != c.registered.end()) it->second = p; else c.registered.emplace_back(n, p);
        } else bad(ln, "unknown setting " + head);
    }
    return c;
}
inline Config read_config(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    if (!fs::is_regular_file(path, ec)) throw std::runtime_error("datasets: " + path + " is not a file");
    if (fs::file_size(path, ec) > (1u << 20)) throw std::runtime_error("datasets: " + path + " is too large for a configuration (over 1 MB)");
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("datasets: cannot read " + path);
    std::stringstream ss; ss << f.rdbuf();
    return parse_config(ss.str(), path);
}
// a text as Musil reads it back: quoted, \ " and line ends escaped
inline std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else o += c;
    }
    return o + "\"";
}
inline std::string config_text(const Config& c) {
    std::string t = "; the datasets of Musil and Moritz: (dataset name) and the orchestra object find them by name.\n"
                    "; Written by dataset-register!, datasets-folder! and Moritz's Datasets window; one setting a line:\n"
                    ";   folder \"path\"           the datasets folder (else ~/Documents/Moritz/datasets)\n"
                    ";   dataset name \"path\"     a feature file that lives elsewhere\n";
    if (!c.folder.empty()) t += "folder " + quoted(c.folder) + "\n";
    for (auto& r : c.registered) t += "dataset " + r.first + " " + quoted(r.second) + "\n";
    return t;
}
// a file written whole or not at all: a temporary file in the same folder, the write and the close checked, renamed
inline void write_file_atomically(const std::string& path, const std::string& text) {
    fs::path p(path); std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp = p; tmp += ".tmp-" + std::to_string((unsigned long long)stamp % 1000000007ull);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + tmp.string());
        f << text; f.flush();
        if (!f) { f.close(); fs::remove(tmp, ec); throw std::runtime_error("cannot write " + tmp.string()); }
        f.close();
        if (f.fail()) { fs::remove(tmp, ec); throw std::runtime_error("cannot close " + tmp.string()); }
    }
    fs::rename(tmp, p, ec);
    if (ec) { std::error_code e2; fs::remove(tmp, e2); throw std::runtime_error("cannot replace " + path + ": " + ec.message()); }
}
inline std::mutex& config_mutex() { static std::mutex m; return m; }
// read, change, write: one at a time in this program
inline Config update_config(const std::string& path, const std::function<void(Config&)>& change) {
    if (path.empty()) throw std::runtime_error("datasets: no configuration file (no home folder)");
    std::lock_guard<std::mutex> g(config_mutex());
    Config c = read_config(path);
    change(c);
    write_file_atomically(path, config_text(c));
    return c;
}
// the folder datasets are downloaded into: the one chosen, else the default
inline std::string folder_in_use(const Place& p, const Config& c) { return c.folder.empty() ? p.default_folder : c.folder; }

// --- what is there -------------------------------------------------------------------------------------------------
struct Entry {
    std::string name;      // fullsol
    std::string path;      // its feature file
    std::string where;     // registered, folder (the one chosen), default (the default folder), bundled
    bool present = true;   // is the feature file on disk (a registered one may be gone)
};
// the feature files at the top of a folder and one folder down, in the order of their names
inline void scan(const std::string& folder, const std::string& where, std::vector<Entry>& out) {
    if (folder.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return;
    auto files_in = [&](const fs::path& d, std::vector<fs::path>& files, std::vector<fs::path>* dirs) {
        std::vector<fs::path> all;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            if (all.size() >= 4096) break;
            all.push_back(it->path());
        }
        std::sort(all.begin(), all.end());
        for (auto& q : all) {
            std::string n = q.filename().string();
            if (n.empty() || n[0] == '.') continue;                       // hidden, and our temporary folders
            std::error_code e2;
            if (fs::is_directory(q, e2)) { if (dirs) dirs->push_back(q); }
            else if (is_feature_file(n) && fs::is_regular_file(q, e2)) files.push_back(q);
        }
    };
    std::vector<fs::path> files, dirs;
    files_in(folder, files, &dirs);
    for (auto& d : dirs) files_in(d, files, nullptr);
    for (auto& f : files) out.push_back({short_key(name_of_file(f.string())), f.generic_string(), where, true});
}
// every dataset known, in the order a name is resolved
inline std::vector<Entry> list(const Place& p, const Config& c) {
    std::vector<Entry> out;
    for (auto& r : c.registered) { std::error_code ec; out.push_back({r.first, r.second, "registered", fs::is_regular_file(r.second, ec)}); }
    std::string chosen = c.folder;
    scan(chosen, "folder", out);
    auto same = [](const std::string& a, const std::string& b) { return path_key(resolved_path(a)) == path_key(resolved_path(b)); };
    if (chosen.empty() || !same(chosen, p.default_folder)) scan(p.default_folder, "default", out);
    for (auto& b : p.bundled) { std::error_code ec; if (fs::is_regular_file(b, ec)) out.push_back({short_key(name_of_file(b)), b, "bundled", true}); }
    return out;
}
// does a name written by a user answer to this entry: its name, or its file's (a registered fullsol is also fullsol2020)
inline bool answers(const Entry& e, const std::string& wanted) {
    return matches(e.name, wanted) || matches(name_of_file(e.path), wanted);
}
// the feature file of a dataset; "" with the reason in why when there is none
inline std::string find(const Place& p, const Config& c, const std::string& name, std::string* why = nullptr) {
    std::vector<Entry> all = list(p, c);
    for (auto& e : all) if (e.present && answers(e, name)) return e.path;
    if (why) {
        std::string k = name_key(name), s;
        for (auto& e : all) if (!e.present && answers(e, name)) s += "; it is registered at " + e.path + ", which is not on disk";
        std::string folders = c.folder.empty() ? p.default_folder : c.folder + (p.default_folder.empty() ? "" : " and " + p.default_folder);
        s = "no dataset named \"" + k + "\" (looked in " + (folders.empty() ? std::string("no folder") : folders) + " and what comes with the program" + s + ")";
        s += k == "tinysol" ? "; download it with (dataset-fetch \"tinysol\"), or Moritz's Extras > Datasets..."
                            : "; register it once with (dataset-register! \"" + k + "\" \"path/to/" + k + ".spectrum.db\"), or Moritz's Extras > Datasets...";
        *why = s;
    }
    return "";
}
// the same, reading the configuration; throws std::runtime_error when there is none or the configuration is malformed
inline std::string resolve(const Place& p, const std::string& name) {
    Config c = read_config(p.config);
    std::string why, f = find(p, c, name, &why);
    if (f.empty()) throw std::runtime_error(why);
    return f;
}

// is a file a feature file: its first line reads type block hop ncoeff
inline bool looks_like_feature_file(const std::string& path, std::string* why = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (why) *why = "cannot open " + path; return false; }
    std::string line; std::getline(f, line);
    if (line.size() > 4096) { if (why) *why = path + " is not a feature file"; return false; }
    std::istringstream ss(line); std::string type; long block = 0, hop = 0, n = 0;
    ss >> type >> block >> hop >> n;
    if (ss.fail() || n <= 0) { if (why) *why = path + " is not a feature file (its first line is not: type block hop ncoeff)"; return false; }
    return true;
}
// name -> path in the configuration (the path absolute); throws when the file is not a feature file
inline Config register_dataset(const std::string& config, const std::string& name, const std::string& path) {
    std::string n = name_key(name);
    if (!valid_name(n)) throw std::runtime_error("not a dataset's name: \"" + name + "\" (letters, digits, - _ .)");
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw std::runtime_error(path + " is not on disk");
    std::string why; if (!looks_like_feature_file(path, &why)) throw std::runtime_error(why);
    std::string full = resolved_path(path).generic_string();
    return update_config(config, [&](Config& c) {
        auto it = std::find_if(c.registered.begin(), c.registered.end(), [&](auto& r) { return r.first == n; });
        if (it != c.registered.end()) it->second = full; else c.registered.emplace_back(n, full);
    });
}
inline bool forget_dataset(const std::string& config, const std::string& name) {
    std::string n = name_key(name); bool gone = false;
    update_config(config, [&](Config& c) {
        auto e = std::remove_if(c.registered.begin(), c.registered.end(), [&](auto& r) { return r.first == n; });
        gone = e != c.registered.end(); c.registered.erase(e, c.registered.end());
    });
    return gone;
}
inline Config set_folder(const std::string& config, const std::string& folder) {
    std::string f = folder.empty() ? "" : resolved_path(folder).generic_string();
    return update_config(config, [&](Config& c) { c.folder = f; });
}

// --- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------------------------
class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        len_ = 0; n_ = 0;
    }
    void update(const unsigned char* p, size_t n) {
        len_ += n;
        while (n > 0) {
            size_t k = std::min(n, (size_t)64 - n_);
            std::memcpy(buf_.data() + n_, p, k); n_ += k; p += k; n -= k;
            if (n_ == 64) { block(buf_.data()); n_ = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = len_ * 8;
        unsigned char pad = 0x80; update(&pad, 1);
        unsigned char zero = 0; while (n_ != 56) update(&zero, 1);
        unsigned char l[8]; for (int k = 0; k < 8; ++k) l[k] = (unsigned char)(bits >> (56 - 8 * k));
        update(l, 8);
        static const char* d = "0123456789abcdef";
        std::string o; for (uint32_t w : h_) for (int k = 28; k >= 0; k -= 4) o += d[(w >> k) & 15];
        return o;
    }
private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int t = 0; t < 16; ++t) w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | (uint32_t)p[4 * t + 3];
        for (int t = 16; t < 64; ++t) {
            uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int t = 0; t < 64; ++t) {
            uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    std::array<uint32_t, 8> h_{};
    std::array<unsigned char, 64> buf_{};
    uint64_t len_ = 0; size_t n_ = 0;
};
// progress: fraction done (0..1) and what is being done; false stops (Cancelled is thrown)
using Progress = std::function<bool(double, const std::string&)>;
inline std::string sha256_file(const std::string& path, const Progress& progress = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::error_code ec; double total = (double)std::max<std::uintmax_t>(1, fs::file_size(path, ec));
    Sha256 s; std::vector<unsigned char> buf(1 << 20); double done = 0;
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        s.update(buf.data(), (size_t)n); done += (double)n;
        if (progress && !progress(std::min(1.0, done / total), "checking")) throw Cancelled();
    }
    if (f.bad()) throw std::runtime_error("cannot read " + path);
    return s.hex();
}

// --- programs: curl and unzip, each stopped when the caller says so -------------------------------------------------
// runs argv[0] with its arguments, its output into log; polls keep_going every 50 ms (false: the program is stopped and
// Cancelled thrown). => its exit status; -1 when it could not be started
inline int run_program(const std::vector<std::string>& argv, const std::string& log, const std::function<bool()>& keep_going) {
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w((size_t)std::max(n, 1), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        w.resize(n > 0 ? (size_t)n - 1 : 0);
        return w;
    };
    auto quote = [](const std::wstring& a) {                // the quoting CommandLineToArgvW undoes
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
        std::wstring o = L"\""; size_t bs = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { ++bs; continue; }
            if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += c; bs = 0; continue; }
            o.append(bs, L'\\'); bs = 0; o += c;
        }
        o.append(bs * 2, L'\\');
        return o + L"\"";
    };
    std::wstring cmd;
    for (auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += quote(wide(a)); }
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE out = CreateFileW(wide(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return -1;
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr; si.hStdOutput = out; si.hStdError = out;
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    BOOL ok = CreateProcessW(wide(argv[0]).c_str(), &line[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(out);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    for (;;) {
        DWORD w = WaitForSingleObject(pi.hProcess, 50);
        if (w == WAIT_OBJECT_0) { DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); return (int)code; }
        if (w == WAIT_FAILED) { CloseHandle(pi.hProcess); return -1; }
        if (keep_going && !keep_going()) {
            TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hProcess);
            throw Cancelled();
        }
    }
#else
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return -1;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);   // 127: not found (glibc)
            return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
        if (r < 0 && errno != EINTR) return -1;
        if (keep_going && !keep_going()) {
            kill(pid, SIGTERM);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            throw Cancelled();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}
// the last lines a program wrote (for an error message)
inline std::string tail_of(const std::string& log) {
    std::ifstream f(log, std::ios::binary); std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.size() > 400) s = "..." + s.substr(s.size() - 400);
    for (auto& c : s) if (c == '\r' || c == '\n') c = ' ';
    return s;
}
inline std::string system_program(const char* name) {
#ifdef _WIN32
    const char* root = std::getenv("SystemRoot");                 // Windows' own curl and tar, not an MSYS2 one first on PATH
    return std::string(root && *root ? root : "C:\\Windows") + "\\System32\\" + name + ".exe";
#else
    return name;
#endif
}
// a temporary folder of ours inside folder (hidden, so that a scan skips it), removed when the guard goes
struct TempFolder {
    fs::path path;
    TempFolder(const std::string& folder, const std::string& what) {
        auto stamp = (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count();
        std::hash<std::thread::id> th;
        path = fs::path(folder) / ("." + what + "-" + std::to_string((stamp ^ (unsigned long long)th(std::this_thread::get_id())) % 100000000ull));
        std::error_code ec; fs::create_directories(path, ec);
        if (ec) throw std::runtime_error("cannot make a folder in " + folder + ": " + ec.message());
    }
    ~TempFolder() { std::error_code ec; fs::remove_all(path, ec); }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
};

// --- what can be downloaded ----------------------------------------------------------------------------------------
struct Source {
    const char* name;      // tinysol
    const char* url;
    const char* sha256;    // of the archive, as the release gives it
    std::uintmax_t size;   // bytes of the archive
    const char* stem;      // TinySOL: TinySOL.spectrum.db next to TinySOL/
    const char* about;
};
inline const Source* source(const std::string& name) {
    static const Source tinysol{"tinysol", "https://github.com/CarmineCella/musil/releases/download/data/TinySOL.zip",
                                "414b89d60016a0fd34f4c3c81b97874b7d6500352695e79fd45f11f21b6b8ae3", 754366383u, "TinySOL",
                                "TinySOL (Ircam): 14 instruments, ordinario, pp mf ff, about 750 MB"};
    return matches("tinysol", name) ? &tinysol : nullptr;
}

// An archive on disk installed into folder: its SHA-256 checked (when sha256 is given), unzipped into a temporary
// folder there, its feature file (*.spectrum.db, at the top or one folder down; stem's when there are several) read
// whole and its sounds' folder found, then the sounds' folder moved into place and the feature file last.
// => the installed feature file. Throws std::runtime_error (Cancelled when progress says stop).
inline std::string install(const std::string& archive, const std::string& folder, const std::string& sha256,
                           const std::string& stem = "", const Progress& progress = nullptr) {
    auto tick = [&](double f, const std::string& what) { if (progress && !progress(f, what)) throw Cancelled(); };
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) throw std::runtime_error(archive + " is not on disk");
    if (folder.empty()) throw std::runtime_error("no datasets folder");
    fs::create_directories(folder, ec);
    if (!fs::is_directory(folder, ec)) throw std::runtime_error("cannot make the datasets folder " + folder);
    if (!sha256.empty()) {
        std::string got = sha256_file(archive, [&](double f, const std::string& w) { return !progress || progress(.5 * f, w); });
        if (lower(got) != lower(sha256))
            throw std::runtime_error("the archive is damaged: its SHA-256 is " + got + ", expected " + lower(sha256));
    }
    TempFolder tmp(folder, "install");
    fs::path unz = tmp.path / "unz"; fs::create_directories(unz, ec);
    tick(.5, "unpacking");
    std::string log = (tmp.path / "unpack.log").string();
#ifdef _WIN32
    std::vector<std::string> cmd = {system_program("tar"), "-xf", archive, "-C", unz.string()};
#else
    std::vector<std::string> cmd = {"unzip", "-q", "-o", archive, "-d", unz.string()};
#endif
    int rc = run_program(cmd, log, [&] { return !progress || progress(.5, "unpacking"); });
    if (rc < 0) throw std::runtime_error(std::string("cannot run ") + cmd[0] + " to unpack the archive: is it installed?");
    if (rc != 0) throw std::runtime_error("the archive could not be unpacked (" + cmd[0] + ": " + tail_of(log) + ")");
    tick(.8, "checking");
    // nothing that points elsewhere: no symbolic links in what was unpacked
    for (fs::recursive_directory_iterator it(unz, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_symlink(ec)) throw std::runtime_error("the archive holds a symbolic link (" + it->path().filename().string() + "): not installed");
    if (ec) throw std::runtime_error("cannot read what was unpacked: " + ec.message());
    std::vector<fs::path> dbs;
    auto look = [&](const fs::path& d) {
        std::error_code e;
        for (fs::directory_iterator it(d, e), end; !e && it != end; it.increment(e))
            if (it->is_regular_file(e) && is_feature_file(it->path().filename().string()) && it->path().filename().string()[0] != '.') dbs.push_back(it->path());
    };
    look(unz);
    if (dbs.empty()) for (fs::directory_iterator it(unz, ec), end; !ec && it != end; it.increment(ec)) if (it->is_directory(ec) && it->path().filename().string()[0] != '.') look(it->path());
    if (dbs.empty()) throw std::runtime_error("the archive holds no feature file (X.spectrum.db)");
    std::sort(dbs.begin(), dbs.end());
    fs::path db = dbs.front();
    if (!stem.empty()) for (auto& d : dbs) if (lower(d.filename().string()) == lower(stem) + ".spectrum.db") db = d;
    std::vector<std::string> probes; size_t count = 0;
    try {
        db_parse(db.string(), db.filename().string(), [&](db_record&& r) { if (probes.size() < 20) probes.push_back(r.file); ++count; });
    } catch (const std::exception& e) { throw std::runtime_error(std::string("the archive's feature file is not readable: ") + e.what()); }
    if (count == 0) throw std::runtime_error("the archive's feature file lists no sound");
    fs::path sounds = db_sounds_folder(db.parent_path().string(), probes);
    if (fs::equivalent(sounds, db.parent_path(), ec) || !fs::is_directory(sounds, ec))
        throw std::runtime_error("the archive's sounds are not in a folder next to " + db.filename().string());
    tick(.9, "installing");
    fs::path dest_db = fs::path(folder) / db.filename(), dest_sounds = fs::path(folder) / sounds.filename();
    if (fs::exists(dest_db, ec)) throw std::runtime_error(dest_db.generic_string() + " is already there");
    if (fs::exists(dest_sounds, ec)) throw std::runtime_error("a folder " + dest_sounds.generic_string() + " is in the way: move it or delete it, then install again");
    fs::rename(sounds, dest_sounds, ec);
    if (ec) throw std::runtime_error("cannot move the sounds into " + folder + ": " + ec.message());
    fs::rename(db, dest_db, ec);
    if (ec) throw std::runtime_error("cannot move the feature file into " + folder + ": " + ec.message());
    tick(1, "installed");
    return dest_db.generic_string();
}

// A known dataset downloaded into folder and installed (see install): nothing is done when it is already there.
// curl writes into a temporary folder inside folder, whose size is the progress. => the installed feature file
inline std::string fetch(const Source& src, const std::string& folder, const Progress& progress = nullptr) {
    std::error_code ec;
    if (folder.empty()) throw std::runtime_error("no datasets folder");
    fs::path have = fs::path(folder) / (std::string(src.stem) + ".spectrum.db");
    if (fs::is_regular_file(have, ec)) return have.generic_string();
    fs::create_directories(folder, ec);
    if (!fs::is_directory(folder, ec)) throw std::runtime_error("cannot make the datasets folder " + folder);
    TempFolder tmp(folder, "download");
    fs::path part = tmp.path / (std::string(src.stem) + ".zip");
    std::string log = (tmp.path / "curl.log").string();
    std::vector<std::string> cmd = {system_program("curl"), "-L", "-f", "-s", "-S", "--retry", "2", "-o", part.string(), src.url};
    auto keep = [&] {
        std::error_code e; double got = fs::exists(part, e) ? (double)fs::file_size(part, e) : 0.0;
        return !progress || progress(.8 * std::min(1.0, got / (double)std::max<std::uintmax_t>(1, src.size)), "downloading");
    };
    if (progress && !progress(0, "downloading")) throw Cancelled();
    int rc = run_program(cmd, log, keep);
    if (rc < 0) throw std::runtime_error("cannot run curl to download " + std::string(src.url) + ": is it installed?");
    if (rc != 0) throw std::runtime_error("the download of " + std::string(src.url) + " failed (curl: " + tail_of(log) + ")");
    std::uintmax_t got = fs::file_size(part, ec);
    if (ec || (src.size && got != src.size))
        throw std::runtime_error("the download is incomplete: " + std::to_string(ec ? 0 : got) + " bytes of " + std::to_string(src.size));
    return install(part.string(), folder, src.sha256, src.stem,
                   [&](double f, const std::string& w) { return !progress || progress(.8 + .2 * f, w); });
}

}  // namespace datasets
}  // namespace musil

#ifdef OPAQUE      // windows.h (wingdi.h) defines it; Musil has a value type of that name
#undef OPAQUE
#endif
