// music.h — music, C++ half: sample databases in the SOL/Orchidea layout, pitch names, Orchidea's search, Maple's
// matching pursuit (music/maple.h) and SoundTypes (music/soundtypes.h).
// The score itself is a C++ structure (music/score.h), a value of the language (music/score_musil.h), written to
// .score files as Musil data (music/score_text.h); the Musil half (music.mu) renders it, plays it, draws its roll.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A database is a folder of sound files and one feature file (a `.db`) at its root:
//   first line:   <type> <block size> <hop> <number of coefficients>
//   other lines:  <path relative to the root>;<f1>;<f2>;...
// Sound file names carry the metadata, separated by dashes: instrument-technique-pitch-dynamics-other
// (Vn-ord-D#5-mf-3c-N.wav); a technique may be several tokens (Vn-pizz-lv-C4-mf-...), so the pitch is the
// first token after the instrument that reads as one; a sharp may be written # or _ on disk. Only the metadata and the
// features are read here; the sounds are opened on demand (music.mu caches what it plays).

#pragma once
#include "core.h"
#include "system.h"
#include "signals.h"
#include "music/midi.h"
#include "music/orchidea.h"
#include "music/maple.h"
#include "music/soundtypes.h"
#include "music/score_musil.h"
#include <cmath>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#ifdef OPAQUE      // Windows defines it (wingdi.h, through system.h's sockets); Musil has a value type of that name
#undef OPAQUE
#endif

namespace musil {

// --- pitch names ---
// (pitch->midi "D#5") => 75: a note name with sharps (#, _) or flats (b) and an octave (C4 = 60); -1 if not a pitch
inline int pitch_to_midi(const std::string& s) {
    if (s.size() < 2) return -1;
    static const int base[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    char c = (char)std::toupper((unsigned char)s[0]); if (c < 'A' || c > 'G') return -1;
    int semi = base[c - 'A']; size_t p = 1;
    while (p < s.size() && (s[p] == '#' || s[p] == '_' || s[p] == 'b' || s[p] == 'x')) { if (s[p] == 'b') semi--; else if (s[p] == 'x') semi += 2; else semi++; p++; }
    if (p >= s.size()) return -1;
    bool neg = s[p] == '-'; if (neg) p++;
    if (p >= s.size() || !std::isdigit((unsigned char)s[p])) return -1;
    int oct = 0; while (p < s.size() && std::isdigit((unsigned char)s[p])) oct = oct * 10 + (s[p++] - '0');
    if (p != s.size()) return -1;
    if (neg) oct = -oct;
    return (oct + 1) * 12 + semi;
}
inline vptr mus_pitch_to_midi(vlist& a, Interp& i) { return v_num(pitch_to_midi(i.str(a[0]))); }
// (midi->pitch 75) => "D#5"
inline std::string midi_to_pitch(int m) {
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int oct = m / 12 - 1, semi = m % 12; if (semi < 0) { semi += 12; oct--; }
    return std::string(names[semi]) + std::to_string(oct);
}
inline vptr mus_midi_to_pitch(vlist& a, Interp& i) { return v_str(midi_to_pitch((int)std::lround(i.scalar(a[0])))); }

// a record's value by key (a symbol or a string), or nil
inline vptr rec_get(const vptr& rec, const char* key) {
    if (!rec || rec->t != Value::LIST) return nullptr;
    for (auto& p : rec->l) if (p && p->t == Value::LIST && p->l.size() == 2 && p->l[0] && (p->l[0]->t == Value::SYM || p->l[0]->t == Value::STR) && p->l[0]->s == key) return p->l[1];
    return nullptr;
}

// --- the database file ---
// The parsing is C++ of its own (db_parse), shared by db-read and by the programs that embed Musil's engines (Moritz's
// orchestra object): the header, then one record per sound, its metadata read off the file name.
struct db_header { std::string type; long block = 0, hop = 0, ncoeff = 0; };
struct db_record { std::string file, instr, tech, pitch, dyn, other; int midi = -1; std::vector<double> features; };
// the metadata of a sound file's name: instrument-technique-pitch-dynamics-other...: the instrument, then the technique
// (one token or several joined by dashes: pizz-lv, art-harm), then the first token that is a pitch, the dynamics, and
// whatever follows; an unpitched sound (N, or no pitch) gets midi -1
inline void db_name_fields(const std::string& file, db_record& r) {
    std::string name = file; size_t sl = name.find_last_of("/\\"); if (sl != std::string::npos) name = name.substr(sl + 1);
    size_t dot = name.find_last_of('.'); if (dot != std::string::npos) name = name.substr(0, dot);
    std::vector<std::string> syms; { std::stringstream ss(name); std::string t; while (std::getline(ss, t, '-')) syms.push_back(t); }
    r.instr = syms.empty() ? "N" : syms[0]; r.tech = "N"; r.pitch = "N"; r.dyn = "N"; r.other = "N"; r.midi = -1;
    size_t pp = std::string::npos;
    for (size_t j = 1; j < syms.size(); j++) if (pitch_to_midi(syms[j]) >= 0) { pp = j; break; }
    if (pp == std::string::npos) {                        // no pitch: instrument-technique-N-dynamics... in the old layout, or unpitched
        if (syms.size() > 1) r.tech = syms[1]; if (syms.size() > 3) r.dyn = syms[3]; if (syms.size() > 2 && syms[2] != "N") r.pitch = syms[2];
        std::string rest; for (size_t j = 4; j < syms.size(); j++) rest += (j > 4 ? "-" : "") + syms[j]; if (!rest.empty()) r.other = rest;
    } else {
        std::string t; for (size_t j = 1; j < pp; j++) t += (j > 1 ? "-" : "") + syms[j]; if (!t.empty()) r.tech = t;
        r.pitch = syms[pp]; r.midi = pitch_to_midi(r.pitch); if (pp + 1 < syms.size()) r.dyn = syms[pp + 1];
        std::string rest; for (size_t j = pp + 2; j < syms.size(); j++) rest += (j > pp + 2 ? "-" : "") + syms[j]; if (!rest.empty()) r.other = rest;
    }
}
// the feature file at path read: => its header; each record is handed to `each` as it is read (a large database is
// not held twice); `shown` is the name the messages give. Throws std::runtime_error ("db-read: ...")
inline db_header db_parse(const std::string& path, const std::string& shown, const std::function<void(db_record&&)>& each) {
    std::ifstream f(path); if (!f) throw std::runtime_error("db-read: cannot open " + shown);
    db_header h; f >> h.type >> h.block >> h.hop >> h.ncoeff;
    if (f.fail() || h.ncoeff <= 0) throw std::runtime_error("db-read: the first line must be: type block hop ncoeff");
    if (h.ncoeff > 1000000) throw std::runtime_error("db-read: " + std::to_string(h.ncoeff) + " coefficients: not a feature file");
    std::string line; std::getline(f, line);
    long lineno = 1;
    while (std::getline(f, line)) {
        lineno++;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        size_t semi = line.find(';'); std::string file = line.substr(0, semi);
        if (file.empty()) continue;
        db_record r; r.file = file; r.features.assign((size_t)h.ncoeff, 0.0);
        size_t k = 0, p = semi == std::string::npos ? line.size() : semi + 1;
        while (p < line.size() && k < (size_t)h.ncoeff) { size_t q = line.find(';', p); r.features[k++] = std::atof(line.c_str() + p); p = q == std::string::npos ? line.size() : q + 1; }
        if (k != (size_t)h.ncoeff) throw std::runtime_error("db-read: line " + std::to_string(lineno) + " has " + std::to_string(k) + " features, expected " + std::to_string(h.ncoeff));
        db_name_fields(file, r);
        each(std::move(r));
    }
    return h;
}
// (db-read path) => (list type block hop ncoeff entries): the feature file parsed; each entry a record
//   (file instr tech pitch dyn other midi features), the file relative to the database's root
inline vptr mus_db_read(vlist& a, Interp& i) {
    std::string path = i.read_path(i.str(a[0]));
    vlist entries; db_header h;
    try {
        h = db_parse(path, i.str(a[0]), [&](db_record&& r) {
            varr feats(r.features.size()); for (size_t k = 0; k < r.features.size(); k++) feats[k] = r.features[k];
            entries.push_back(v_list({ v_list({ v_sym("file"), v_str(r.file) }), v_list({ v_sym("instr"), v_str(r.instr) }), v_list({ v_sym("tech"), v_str(r.tech) }),
                                       v_list({ v_sym("pitch"), v_str(r.pitch) }), v_list({ v_sym("dyn"), v_str(r.dyn) }), v_list({ v_sym("other"), v_str(r.other) }),
                                       v_list({ v_sym("midi"), v_num(r.midi) }), v_list({ v_sym("features"), v_arr(std::move(feats)) }) }));
        });
    } catch (std::runtime_error& e) { i.bad(e.what()); }
    return v_list({ v_str(h.type), v_num((double)h.block), v_num((double)h.hop), v_num((double)h.ncoeff), v_list(std::move(entries)) });
}

// --- making a database: the features of every sound under a folder (Orchidea's dbgen) -----------
// The features of a sound: its magnitude spectrum averaged over the frames (Hann window of block
// samples, every hop), each frame weighted by its energy, normalised by the total energy, at 44100 Hz
// (other rates are resampled, stereo is summed). Then, by type:
//   spectrum   the first ncoeff bins             logspec    their logs
//   specpeaks  the local maxima above the mean (others 0), first ncoeff bins
//   specenv    the cepstral envelope (160 coefficients), first ncoeff bins
//   mfcc       ncoeff mel-frequency cepstral coefficients (40 filters)
//   moments    centroid, spread, skewness, kurtosis (ncoeff >= 4)
inline std::vector<double> average_spectrum(const std::vector<double>& x, int block, int hop) {
    std::vector<double> win(block), avg(block, 0.0), fr(2 * block); double total = 0;
    for (int k = 0; k < block; k++) win[k] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * k / block);
    for (size_t i = 0; i < x.size(); i += (size_t)hop) {
        std::fill(fr.begin(), fr.end(), 0.0); double nrg = 0; size_t rsize = std::min<size_t>((size_t)block, x.size() - i);
        for (size_t j = 0; j < rsize; j++) { fr[2 * j] = x[i + j] * win[j]; nrg += x[i + j] * x[i + j]; }
        nrg /= (double)rsize; total += nrg;
        fft_inplace(fr.data(), (size_t)block, -1);
        for (int j = 0; j < block; j++) avg[j] += std::hypot(fr[2 * j], fr[2 * j + 1]) * nrg;
    }
    if (total > 0) for (auto& v : avg) v /= total;
    return avg;
}
inline std::vector<double> db_features_of(const std::vector<double>& mono, int block, int hop, int ncoeff, const std::string& type) {
    std::vector<double> spec = average_spectrum(mono, block, hop), out((size_t)ncoeff, 0.0);
    const double sr = 44100.0, eps = 1e-12;
    if (type == "spectrum") { for (int j = 0; j < ncoeff; j++) out[j] = spec[j]; }
    else if (type == "logspec") { for (int j = 0; j < ncoeff; j++) out[j] = std::log(spec[j] + eps); }
    else if (type == "specpeaks") {
        int half = block / 2; double mean = 0; for (int j = 0; j < half; j++) mean += spec[j]; mean /= half;
        for (int j = 1; j < std::min(half - 1, ncoeff); j++) if (spec[j] > spec[j - 1] && spec[j] >= spec[j + 1] && spec[j] > mean) out[j] = spec[j];
    }
    else if (type == "specenv") {                     // cepstral smoothing: the first 160 quefrencies of the log spectrum
        std::vector<double> c(2 * block, 0.0); for (int j = 0; j < block; j++) c[2 * j] = std::log(spec[j] + eps);
        fft_inplace(c.data(), (size_t)block, -1);
        int keep = 160; for (int j = 0; j < block; j++) { bool k = j < keep || j > block - keep; c[2 * j] = k ? c[2 * j] / block : 0; c[2 * j + 1] = k ? c[2 * j + 1] / block : 0; }
        fft_inplace(c.data(), (size_t)block, 1);
        for (int j = 0; j < ncoeff; j++) out[j] = std::exp(c[2 * j]);
    }
    else if (type == "mfcc") {                        // 40 mel filters over the half spectrum, log energies, DCT-II
        int nf = 40, half = block / 2; auto mel = [](double f) { return 2595.0 * std::log10(1 + f / 700.0); }; auto imel = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1); };
        double mlo = mel(0), mhi = mel(sr / 2); std::vector<double> centers(nf + 2); for (int k = 0; k < nf + 2; k++) centers[k] = imel(mlo + (mhi - mlo) * k / (nf + 1)) / (sr / block);
        std::vector<double> e(nf, 0.0);
        for (int k = 0; k < nf; k++) { double a = centers[k], b = centers[k + 1], c2 = centers[k + 2];
            for (int j = 0; j < half; j++) { double w = j < a || j > c2 ? 0 : j <= b ? (j - a) / std::max(b - a, 1e-9) : (c2 - j) / std::max(c2 - b, 1e-9); e[k] += w * spec[j]; }
            e[k] = std::log(e[k] + eps); }
        for (int j = 0; j < ncoeff; j++) { double acc = 0; for (int k = 0; k < nf; k++) acc += e[k] * std::cos(3.14159265358979323846 * j * (k + 0.5) / nf); out[j] = acc; }
    }
    else if (type == "moments") {
        int half = block / 2; double bw = sr / block, sum = 0, c = 0; for (int j = 0; j < half; j++) { sum += spec[j]; c += j * bw * spec[j]; }
        double centroid = sum > 0 ? c / sum : 0, sp = 0, sk = 0, ku = 0;
        for (int j = 0; j < half; j++) { double d = j * bw - centroid; sp += d * d * spec[j]; }
        double spread = sum > 0 ? std::sqrt(sp / sum) : 0;
        for (int j = 0; j < half; j++) { double d = j * bw - centroid; sk += d * d * d * spec[j]; ku += d * d * d * d * spec[j]; }
        if (ncoeff < 4) throw std::runtime_error("moments: ncoeff must be >= 4");
        out[0] = centroid; out[1] = spread; out[2] = sum > 0 && spread > 0 ? sk / sum / std::pow(spread, 3) : 0; out[3] = sum > 0 && spread > 0 ? ku / sum / std::pow(spread, 4) : 0;
    }
    else throw std::runtime_error("unknown feature type " + type + " (spectrum, logspec, specpeaks, specenv, mfcc, moments)");
    return out;
}
// (db-gen folder path type block hop ncoeff) => the number of sounds analysed: every WAV under folder (recursively), its
//   features written to path in the database format (first line: type block hop ncoeff; then the file's path
//   relative to folder and the features); the file names carry the metadata (instr-tech-pitch-dyn-other).
//   Sounds that cannot be read are skipped with a message.
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
inline vptr mus_db_gen(vlist& a, Interp& i) {
    namespace fs = std::filesystem;
    std::string folder = i.read_path(i.str(a[0])), outpath = i.str(a[1]), type = i.str(a[2]);
    int block = (int)i.scalar(a[3]), hop = (int)i.scalar(a[4]), ncoeff = (int)i.scalar(a[5]);
    if (block < 16 || (block & (block - 1))) i.bad("block must be a power of 2 >= 16");
    if (hop < 1 || hop > block) i.bad("hop must be in 1..block");
    if (ncoeff < 1 || ncoeff > block / 2) i.bad("ncoeff must be in 1..block/2");
    std::error_code ec; if (!fs::is_directory(folder, ec)) i.bad("db-gen: not a folder: " + i.str(a[0]));
    std::vector<fs::path> files;
    db_walk(folder, [&](const fs::path& p) { std::string ext = p.extension().string(); for (auto& c : ext) c = (char)std::tolower((unsigned char)c); if (ext == ".wav" && p.filename().string()[0] != '.') files.push_back(p); });
    std::sort(files.begin(), files.end());
    std::ofstream out(outpath); if (!out) i.bad("db-gen: cannot write " + outpath);
    out << type << " " << block << " " << hop << " " << ncoeff << "\n";
    size_t done = 0;
    for (size_t k = 0; k < files.size(); k++) {
        std::string rel = fs::relative(files[k], folder, ec).generic_string();
        WAVHeader h{}; std::vector<std::vector<double>> chans;
        try { chans = read_wav_raw(files[k].string().c_str(), h); } catch (std::exception& e) { *i.out << "db-gen: skipped " << rel << ": " << e.what() << "\n"; continue; }
        if (chans.empty()) continue;
        std::vector<double> mono(chans[0].size(), 0.0); for (auto& c : chans) for (size_t j = 0; j < mono.size() && j < c.size(); j++) mono[j] += c[j] / chans.size();
        if (h.sampleRate != 44100) { varr v(mono.size()); for (size_t j = 0; j < mono.size(); j++) v[j] = mono[j]; varr r = resample_sinc(v, 44100.0 / h.sampleRate); mono.assign(std::begin(r), std::end(r)); }
        std::vector<double> f;
        try { f = db_features_of(mono, block, hop, ncoeff, type); } catch (std::exception& e) { i.bad(std::string("db-gen: ") + e.what()); }
        out << "/" << rel; for (double v : f) out << ";" << (float)v; out << "\n";
        done++;
        if (files.size() >= 50 && (k + 1) % std::max<size_t>(1, files.size() / 10) == 0) { *i.out << "db-gen: " << (k + 1) << "/" << files.size() << "\n" << std::flush; }
        i.yield_check();
    }
    return v_num((double)done);
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
inline vptr mus_db_locate(vlist& a, Interp& i) { return v_str(db_locate(i.read_path(i.str(a[0])), i.str(a[1]))); }
inline vptr mus_db_index_size(vlist& a, Interp& i) { return v_num((double)db_index_of(i.read_path(i.str(a[0])))->size()); }
// where the sounds of a feature file in dir are: the folder next to it (in the order of their names) under which one of
// the probes (the first entries' files) is found, whatever the folder is called (TinySOL/, tinysol/, a copy named
// otherwise); failing that, the first folder next to it that holds sounds at all; failing that, dir itself (a note
// then says the sound is missing)
inline std::string db_sounds_folder(const std::string& dir, const std::vector<std::string>& probes) {
    if (probes.empty()) return dir;
    namespace fs = std::filesystem; std::error_code ec; std::vector<std::string> names;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end());
    std::vector<std::string> subs;
    for (auto& n : names) { std::string p = dir + "/" + n; if (fs::is_directory(p, ec)) subs.push_back(p); }
    for (auto& s : subs) for (auto& rel : probes) if (!db_locate(s, rel).empty()) return s;
    for (auto& s : subs) if (!db_index_of(s)->empty()) return s;
    return dir;
}
// (db-sounds-folder dir entries)   where the sounds of a feature file in dir are (db-load uses it): the folder next to it
//   (or dir itself) under which one of the first 20 entries' files is found, whatever the folder is called; else the
//   first folder next to it holding any sound; else dir (a relative dir is looked for as a file to read is: here, else
//   next to the script)
inline vptr mus_db_sounds_folder(vlist& a, Interp& i) {
    std::string dir = i.str(a[0]), here = i.read_path(dir); i.require_read(here);
    std::vector<std::string> probes;
    if (a[1]->t == Value::LIST) for (auto& e : a[1]->l) { if (probes.size() >= 20) break; vptr f = rec_get(e, "file"); if (f) probes.push_back(str_of(f)); }
    if (probes.empty()) return v_str(dir);
    std::string found = db_sounds_folder(here, probes);
    return v_str(found == here ? dir : found);
}

// --- mimetic orchestration: Orchidea's search (music/orchidea.h) -------------------------------------------------
// The analysis of the target and the driver of the search are C++ of their own (orchidea::analyse and
// orchidea::search_segments), which orchidea-analyse and orchidea-search wrap and which programs embedding Musil's
// engines (Moritz's orchidea object) call directly: one implementation, the same result for the same seed.
namespace orchidea {
// how the target is cut and read (orchidea-analyse's options)
struct analysis_options {
    std::string segmentation = "flux";          // "flux", "frames", "none" (or "static"), "list" (the onsets given)
    std::vector<double> onsets;                 // for "list": seconds
    double threshold = 2, timegate = 0.1;       // the flux's peaks above threshold of its maximum, timegate seconds apart
    int partials_window = 32768;                // the window the partials are read with (a power of two)
    double partials = 0.2;                      // the threshold on the partials, 0..1 (0: no pitch filter)
    std::vector<std::string> extra_pitches;     // added to every segment's pitches
};
// the target x at sr (resampled to 44100 Hz, the databases' rate) cut at its onsets, each segment analysed into the
// features of the database's type (block, hop, ncoeff, as db-gen makes them) and standardised, and into the pitches of
// its partials with their cents. tick() runs after each segment: false stops (=> the segments so far). Throws
// std::runtime_error for bad options or a database whose analysis parameters are absurd
inline std::vector<segment> analyse(varr x, double sr, const std::string& type, int block, int hop, int ncoeff, const analysis_options& o,
                                    const std::function<bool()>& tick = {}) {
    if (!(sr > 0) || !std::isfinite(sr)) throw std::runtime_error("orchidea-analyse: the sample rate must be > 0");
    if (block < 16 || block > (1 << 20) || (block & (block - 1))) throw std::runtime_error("orchidea-analyse: the database's block must be a power of two, 16 to 1048576");
    if (hop < 1 || hop > block) throw std::runtime_error("orchidea-analyse: the database's hop must be in 1..block");
    if (ncoeff < 1 || ncoeff > block) throw std::runtime_error("orchidea-analyse: the database's number of coefficients must be in 1..block");
    if (sr != 44100.0) x = resample_sinc(x, 44100.0 / sr);
    std::vector<double> mono(std::begin(x), std::end(x)); const double SR = 44100.0;
    for (auto& v : mono) if (!std::isfinite(v)) v = 0;
    if (mono.size() < (size_t)block) mono.resize((size_t)block, 0.0);
    // the onsets
    std::vector<double> onsets; const std::string& segmentation = o.segmentation;
    if (segmentation == "flux") onsets = flux_onsets(mono, block, hop, SR, o.threshold, o.timegate, [](double* d, size_t n, int sg) { fft_inplace(d, n, sg); });
    else if (segmentation == "frames") { for (size_t p = 0; p < mono.size(); p += (size_t)block) onsets.push_back((double)p / SR); }
    else if (segmentation == "none" || segmentation == "static") onsets.clear();
    else if (segmentation == "list") { for (double t : o.onsets) if (std::isfinite(t) && t >= 0) onsets.push_back(t); }
    else throw std::runtime_error("orchidea-analyse: segmentation is flux, frames, none, or a list of onsets");
    if (onsets.empty()) onsets.push_back(0);
    std::sort(onsets.begin(), onsets.end()); if (onsets[0] > 0) onsets.insert(onsets.begin(), 0.0);
    // the segments
    int pwin = o.partials_window;
    if (pwin < 64 || pwin > (1 << 20) || (pwin & (pwin - 1))) throw std::runtime_error("orchidea-analyse: partials-window must be a power of two, 64 to 1048576");
    std::vector<segment> out;
    for (size_t k = 0; k < onsets.size(); k++) {
        size_t start = (size_t)std::floor(onsets[k] * SR); if (start >= mono.size()) break;
        size_t end = k + 1 < onsets.size() ? std::min(mono.size(), (size_t)std::floor(onsets[k + 1] * SR)) : mono.size();
        if (end <= start) continue;
        std::vector<double> piece(mono.begin() + (long)start, mono.begin() + (long)end);
        std::vector<double> f = db_features_of(piece, block, hop, ncoeff, type);
        segment g; g.start = (double)start / SR; g.length = (double)(end - start) / SR;
        g.features.assign(f.begin(), f.end()); standardise(g.features);
        if (o.partials > 0) {
            g.notes = partials_to_notes(piece, pwin, pwin / 4, o.partials, SR,
                [](const std::vector<double>& y, int w, int h) { return average_spectrum(y, w, h); }, [](int m) { return midi_to_pitch(m); });
            for (auto& e : o.extra_pitches) g.notes[e] = 0;
        }
        out.push_back(std::move(g));
        if (tick && !tick()) break;
    }
    return out;
}

// the search's options besides Orchidea's parameters (orchidea-search's opts)
struct search_options {
    params p;                                             // population, epochs ... seed (its progress is not used: see search_segments)
    std::vector<std::string> styles, dynamics, others;    // only the sounds with one of them enter the search (empty: all)
    std::vector<int> octaves { 0 };                       // the pitch filter widened by octaves: {0, -1} admits the octave below
    std::string connection = "closest";                   // "closest", "best" or "path"
    bool sustain = true; double hold = 1e9;               // a player whose pitch the target keeps holds its sound on, hold seconds at most
};
// what the search found for a segment
struct segment_result {
    std::vector<std::pair<float, std::vector<int>>> solutions;   // (cost, the database index per player or -1), the best first
    std::vector<float> curve;                                    // the population's total fitness per epoch
    std::vector<int> players;                                    // the index in the orchestra of each of the solutions' slots
    std::vector<std::string> missing;                            // the target's pitches no sound of the orchestra covers
    int used = 0, held = 0; size_t space = 0;                    // players the chosen solution sounds, players holding on, sounds in the space
};
struct search_result {
    std::vector<segment_result> segments; std::vector<int> choices;   // the connection's solution per segment
    size_t cache_hits = 0, cache_misses = 0;
    bool stopped = false;                                             // progress asked to stop: the result is not complete
};
// the options checked and brought into range, as orchidea-search always did (throws std::runtime_error)
inline void check_options(search_options& o) {
    params& p = o.p;
    p.pop_size = std::max(2, p.pop_size); p.pop_size = (p.pop_size / 2) * 2;
    p.max_epochs = std::max(1, p.max_epochs); p.pursuit = std::max(0, p.pursuit); p.patience = std::max(1, p.patience);
    if (p.dovetail < 0) throw std::runtime_error("orchidea-search: dovetail must be >= 0");
    if (!(p.mutation_rate > 0 && p.mutation_rate <= 1)) throw std::runtime_error("orchidea-search: mutation must be in (0, 1]");
    if (!(p.xover_rate > 0 && p.xover_rate <= 1)) throw std::runtime_error("orchidea-search: crossover must be in (0, 1]");
    if (!(p.sparsity >= 0 && p.sparsity <= 1)) throw std::runtime_error("orchidea-search: sparsity must be in 0..1");
    if (!(p.hysteresis >= 0 && p.positive >= 0 && p.negative >= 0)) throw std::runtime_error("orchidea-search: the penalisations and the hysteresis must be >= 0");
    if (o.connection != "closest" && o.connection != "best" && o.connection != "path") throw std::runtime_error("orchidea-search: connection is closest, best or path");
    if (o.octaves.empty()) o.octaves.push_back(0);
}
// Orchidea's search over the segments of a target, the driver of orchidea-search: segment by segment the genetic
// search, then the choice for this segment (closest to the previous choice, or the best; the path is found at the end),
// which the next segment's search remembers (hysteresis, dovetailing); a player whose pitch the target keeps holds its
// sound on into the next segment ('sustain): it is not searched again. progress(segment, segments, epoch, epochs) runs
// at every epoch: false stops the search (=> a result marked stopped). Throws std::runtime_error ("segment k: ...")
inline search_result search_segments(const std::vector<entry>& db, const std::vector<std::string>& orchestra, std::vector<segment> segments,
                                     search_options o, const std::function<bool(size_t, size_t, int, int)>& progress = {}) {
    check_options(o);
    if (orchestra.empty()) throw std::runtime_error("orchidea-search: an empty orchestra");
    if (segments.empty()) throw std::runtime_error("orchidea-search: no segments");
    params& p = o.p; const std::string& conn = o.connection;
    bool widen = o.octaves.size() != 1 || o.octaves[0] != 0;
    if (widen) for (auto& g : segments) if (!g.notes.empty()) {
        std::map<std::string, int> wider;
        for (auto& kv : g.notes) { int m = pitch_to_midi(kv.first); for (int oc : o.octaves) { if (m < 0 && oc != 0) continue; wider[oc == 0 ? kv.first : midi_to_pitch(m + 12 * oc)] = kv.second; } }
        g.notes = wider;
    }
    genetic ga(p);
    std::vector<model> models(segments.size());
    std::vector<int> choices; std::vector<float> prev_forecast; std::vector<double> held_since(orchestra.size(), -1.0); std::vector<int> held_count;
    search_result r; bool stop = false;
    for (size_t k = 0; k < segments.size(); k++) {
        try { make_model(db, orchestra, segments[k], o.styles, o.dynamics, o.others, models[k]); }
        catch (std::exception& e) { throw std::runtime_error(std::string("segment ") + std::to_string(k + 1) + ": " + e.what()); }
        int holding = 0;
        if (o.sustain && k > 0 && conn != "path") holding = hold_on(models[k], models[k - 1], models[k - 1].solutions[(size_t)choices[k - 1]], o.hold, held_since);
        held_count.push_back(holding);
        size_t seg = k, nsegs = segments.size();
        ga.p.progress = [&, seg, nsegs](int epoch, int epochs) -> bool { if (progress && !progress(seg, nsegs, epoch, epochs)) { stop = true; return false; } return true; };
        ga.search(models[k]);
        if (stop) { r.stopped = true; return r; }
        if (models[k].solutions.empty()) throw std::runtime_error(std::string("segment ") + std::to_string(k + 1) + ": the search found no solution");
        int choice = conn == "closest" ? closest_choice(models[k], prev_forecast, p) : 0;
        choices.push_back(choice);
        const solution& chosen = models[k].solutions[(size_t)choice];
        for (size_t j = 0; j < chosen.indices.size(); j++) {                 // when each player's present sound began (for the hold)
            int slot = models[k].slots[j];
            if (chosen.indices[j] == -1) held_since[(size_t)slot] = -1;
            else if (models[k].fixed[j] < 0 || held_since[(size_t)slot] < 0) held_since[(size_t)slot] = segments[k].start;
        }
        ga.remember(models[k], chosen);
        prev_forecast.assign(models[k].seg->features.size(), 0.0f); additive_forecast(chosen, models[k].database, prev_forecast); standardise(prev_forecast);
    }
    if (conn == "path") choices = shortest_path(models, p.movement);
    for (size_t k = 0; k < models.size(); k++) {
        model& m = models[k]; segment_result s;
        for (auto& sol : m.solutions) { std::vector<int> idx; for (int j : sol.indices) idx.push_back(j == -1 ? -1 : m.database[(size_t)j]->index); s.solutions.push_back({ m.cost_of(sol), std::move(idx) }); }
        s.curve = m.curve; s.players = m.slots; s.missing = m.missing; s.held = held_count[k]; s.space = m.database.size();
        const solution& chosen = m.solutions[(size_t)choices[k]]; for (int j : chosen.indices) if (j != -1) s.used++;
        r.segments.push_back(std::move(s));
    }
    r.choices = choices; r.cache_hits = ga.cache_hits; r.cache_misses = ga.cache_misses;
    return r;
}
// where an instrument sits, as an azimuth in degrees (left positive): the seating Orchidea's mixes use (violins left,
// cellos right, the winds in the middle); 0 for one it does not know
inline double seat_of(const std::string& instr) {
    static const std::pair<const char*, double> seats[] = { { "Vn", 30 }, { "Va", -5 }, { "Vc", -30 }, { "Cb", -20 }, { "Fl", 0 }, { "Picc", 0 }, { "Ob", -10 }, { "ClBb", 10 },
        { "Cl", 10 }, { "Bn", -5 }, { "Hn", 20 }, { "TpC", 10 }, { "Tp", 10 }, { "Tbn", -5 }, { "BTb", -15 }, { "Hp", 20 }, { "Acc", 20 }, { "ASax", -20 } };
    for (auto& s : seats) if (instr == s.first) return s.second;
    return 0;
}
} // namespace orchidea

// (orchidea-analyse x sr type block hop ncoeff opts)   the target's segments for a mimetic orchestration: x a vector at
//   sr (resampled to 44100 Hz, the databases' rate), cut at its onsets, each segment analysed into the features of the
//   database's type (block, hop, ncoeff, as db-gen makes them) and standardised, and into the pitches of its partials
//   with their cents. opts a record: 'segmentation ("flux": the onsets of the spectral flux above 'threshold, a
//   'timegate apart; "frames": every block; "none": one segment; or a list of onset times in seconds), 'threshold
//   (2: above 1 there is one segment, a static target), 'timegate (0.1 s), 'partials-window (32768), 'partials
//   (0.2: the threshold on the partials, 0..1, 0 for no pitch filter), 'extra-pitches (names added to every segment)
//   => a list of records (at dur features notes), notes a record from pitch name to cents
inline vptr mus_orchidea_analyse(vlist& a, Interp& i) {
    varr x = i.num(a[0]); double sr = i.scalar(a[1]); std::string type = i.str(a[2]);
    int block = (int)i.scalar(a[3]), hop = (int)i.scalar(a[4]), ncoeff = (int)i.scalar(a[5]);
    vptr opts = a[6];
    auto opt_num = [&](const char* key, double dflt) { vptr v = rec_get(opts, key); return v && v->t == Value::NUM && v->num.size() == 1 ? v->num[0] : dflt; };
    orchidea::analysis_options o;
    vptr segv = rec_get(opts, "segmentation");
    if (segv && (segv->t == Value::STR || segv->t == Value::SYM)) o.segmentation = segv->s;
    if (segv && segv->t == Value::LIST) { for (auto& e : segv->l) o.onsets.push_back(i.scalar(e)); o.segmentation = "list"; }
    else if (segv && segv->t == Value::NUM) { for (double t : segv->num) o.onsets.push_back(t); o.segmentation = "list"; }
    o.threshold = opt_num("threshold", 2); o.timegate = opt_num("timegate", 0.1);
    o.partials_window = (int)opt_num("partials-window", 32768); o.partials = opt_num("partials", 0.2);
    if (vptr ev = rec_get(opts, "extra-pitches")) { if (ev->t == Value::LIST) for (auto& e : ev->l) o.extra_pitches.push_back(str_of(e)); }
    std::vector<orchidea::segment> segs;
    try { segs = orchidea::analyse(x, sr, type, block, hop, ncoeff, o, [&]() { i.yield_check(); return true; }); }
    catch (std::runtime_error& e) { i.bad(e.what()); }
    vlist out;
    for (auto& g : segs) {
        varr fv(g.features.size()); for (size_t j = 0; j < g.features.size(); j++) fv[j] = g.features[j];
        vlist notes; for (auto& kv : g.notes) notes.push_back(v_list({ v_str(kv.first), v_num(kv.second) }));
        out.push_back(v_list({ v_list({ v_sym("at"), v_num(g.start) }), v_list({ v_sym("dur"), v_num(g.length) }),
                               v_list({ v_sym("features"), v_arr(std::move(fv)) }), v_list({ v_sym("notes"), v_list(std::move(notes)) }) }));
    }
    return v_list(std::move(out));
}
// (orchidea-search entries orchestra segments opts)   Orchidea's genetic search: entries a database's entries (records
//   with instr tech pitch dyn other features), orchestra a list of players as strings ("Fl", "Fl|Picc"), segments as
//   orchidea-analyse gives them, opts a record: 'population 'epochs 'pursuit 'crossover 'mutation 'sparsity 'positive
//   'negative 'hysteresis 'regularization 'dovetail 'movement 'solutions (the best kept per segment) 'connection
//   ("closest", "best" or "path") 'octaves (the pitch filter widened by octaves) 'styles 'dynamics 'others (lists of
//   names that filter the search space) 'seed 'quiet
//   => a record: 'segments a list, one per segment, each a record ('solutions (list (list cost (list index...)) ...),
//   the best first, an index -1 for a silent player; 'curve the fitness per epoch; 'players the index in the
//   orchestra of each of the solution's slots: the players that had sounds in the search space; 'missing the target's
//   pitches no sound of the orchestra covers; 'used how many players the chosen solution sounds; 'held how many hold
//   their sound on from the segment before ('sustain 1: a player whose pitch the target keeps is not searched again,
//   for at most 'hold seconds); 'space the sounds in the search space), 'choices the connection's solution per
//   segment, 'cache the fitness cache's hits and misses; 'patience (150) stops a search that stalls
inline vptr mus_orchidea_search(vlist& a, Interp& i) {
    vptr entries = a[0]; if (entries->t != Value::LIST) i.bad("orchidea-search: entries must be a list");
    std::vector<orchidea::entry> db; db.reserve(entries->l.size());
    auto field = [&](const vptr& e, const char* key) { vptr v = rec_get(e, key); return v ? str_of(v) : std::string("N"); };
    for (size_t k = 0; k < entries->l.size(); k++) {
        const vptr& e = entries->l[k]; orchidea::entry x; x.index = (int)k; x.instr = field(e, "instr"); x.tech = field(e, "tech"); x.pitch = field(e, "pitch"); x.dyn = field(e, "dyn"); x.other = field(e, "other");
        if (vptr mv = rec_get(e, "midi")) if (mv->t == Value::NUM && mv->num.size() == 1) x.midi = (int)mv->num[0];
        vptr f = rec_get(e, "features"); if (!f || f->t != Value::NUM) i.bad("orchidea-search: an entry without features");
        x.features.assign(std::begin(f->num), std::end(f->num)); db.push_back(std::move(x));
    }
    std::vector<std::string> orchestra; if (a[1]->t != Value::LIST) i.bad("orchidea-search: the orchestra must be a list"); for (auto& p : a[1]->l) orchestra.push_back(str_of(p));
    if (orchestra.empty()) i.bad("orchidea-search: an empty orchestra");
    vptr segs = a[2]; if (segs->t != Value::LIST || segs->l.empty()) i.bad("orchidea-search: no segments");
    std::vector<orchidea::segment> segments;
    for (auto& s : segs->l) { orchidea::segment g; vptr at = rec_get(s, "at"), dur = rec_get(s, "dur"), f = rec_get(s, "features"), notes = rec_get(s, "notes");
        if (!at || !dur || !f || at->t != Value::NUM || dur->t != Value::NUM || f->t != Value::NUM || at->num.size() == 0 || dur->num.size() == 0) i.bad("orchidea-search: a segment is a record (at dur features notes)");
        g.start = at->num[0]; g.length = dur->num[0]; g.features.assign(std::begin(f->num), std::end(f->num));
        if (notes && notes->t == Value::LIST) for (auto& kv : notes->l) if (kv->t == Value::LIST && kv->l.size() == 2 && kv->l[1]->t == Value::NUM && kv->l[1]->num.size() > 0) g.notes[str_of(kv->l[0])] = (int)std::lround(kv->l[1]->num[0]);
        segments.push_back(std::move(g)); }
    vptr opts = a[3];
    auto opt_num = [&](const char* key, double dflt) { vptr v = rec_get(opts, key); return v && v->t == Value::NUM && v->num.size() == 1 ? v->num[0] : dflt; };
    auto opt_list = [&](const char* key) { std::vector<std::string> out; vptr v = rec_get(opts, key); if (v && v->t == Value::LIST) for (auto& e : v->l) out.push_back(str_of(e)); return out; };
    orchidea::search_options o; orchidea::params& p = o.p;
    p.pop_size = (int)std::max(-1e9, std::min(1e9, opt_num("population", 300))); p.max_epochs = (int)std::max(-1e9, std::min(1e9, opt_num("epochs", 300)));
    p.pursuit = (int)std::max(-1e9, std::min(1e9, opt_num("pursuit", 0)));
    p.xover_rate = opt_num("crossover", 0.8); p.mutation_rate = opt_num("mutation", 0.01); p.sparsity = opt_num("sparsity", 0.001);
    p.positive = opt_num("positive", 0.5); p.negative = opt_num("negative", 10); p.hysteresis = opt_num("hysteresis", 0); p.regularization = opt_num("regularization", 0);
    p.max_solutions = (int)std::max(-1e9, std::min(1e9, opt_num("solutions", 10))); p.dovetail = opt_num("dovetail", 0); p.movement = opt_num("movement", 1);
    p.patience = (int)std::max(-1e9, std::min(1e9, opt_num("patience", 150)));
    o.sustain = opt_num("sustain", 1) != 0; o.hold = opt_num("hold", 1e9);
    o.octaves.clear();
    if (vptr ov = rec_get(opts, "octaves")) { if (ov->t == Value::LIST) for (auto& e : ov->l) o.octaves.push_back((int)std::lround(i.scalar(e))); else if (ov->t == Value::NUM) for (double d : ov->num) o.octaves.push_back((int)std::lround(d)); }
    vptr sv = rec_get(opts, "seed"); p.seed = sv && sv->t == Value::NUM && sv->num.size() > 0 ? (unsigned)seed_of(i, sv->num[0]) : (unsigned)i.rng();
    bool quiet = opt_num("quiet", 0) != 0; if (vptr cv = rec_get(opts, "connection")) o.connection = str_of(cv);
    o.styles = opt_list("styles"); o.dynamics = opt_list("dynamics"); o.others = opt_list("others");
    int last_shown = -1; size_t last_seg = (size_t)-1;
    orchidea::search_result r;
    try {
        r = orchidea::search_segments(db, orchestra, std::move(segments), o, [&](size_t seg, size_t nsegs, int epoch, int epochs) -> bool {
            i.yield_check();                                                     // Esc in the IDE, the windows kept alive
            if (seg != last_seg) { last_seg = seg; last_shown = -1; }
            if (!quiet && epochs >= 10 && epoch % std::max(1, epochs / 10) == 0 && epoch != last_shown) { last_shown = epoch;
                *i.out << "orchestrate-mimetic: segment " << (seg + 1) << "/" << nsegs << ", epoch " << epoch << "/" << epochs << "\n" << std::flush; }
            return true; });
    } catch (std::runtime_error& e) { i.bad(e.what()); }
    vlist out_segments;
    for (auto& s : r.segments) {
        vlist sols;
        for (auto& sol : s.solutions) { vlist idx; for (int j : sol.second) idx.push_back(v_num(j)); sols.push_back(v_list({ v_num(sol.first), v_list(std::move(idx)) })); }
        varr curve(s.curve.size()); for (size_t j = 0; j < s.curve.size(); j++) curve[j] = s.curve[j];
        vlist slots; for (int q : s.players) slots.push_back(v_num(q));
        vlist missing; for (auto& q : s.missing) missing.push_back(v_str(q));
        out_segments.push_back(v_list({ v_list({ v_sym("solutions"), v_list(std::move(sols)) }), v_list({ v_sym("curve"), v_arr(std::move(curve)) }), v_list({ v_sym("players"), v_list(std::move(slots)) }),
                                        v_list({ v_sym("missing"), v_list(std::move(missing)) }), v_list({ v_sym("used"), v_num(s.used) }), v_list({ v_sym("held"), v_num(s.held) }),
                                        v_list({ v_sym("space"), v_num((double)s.space) }) }));
    }
    vlist ch; for (int c : r.choices) ch.push_back(v_num(c));
    return v_list({ v_list({ v_sym("segments"), v_list(std::move(out_segments)) }), v_list({ v_sym("choices"), v_list(std::move(ch)) }),
                    v_list({ v_sym("cache"), v_list({ v_num((double)r.cache_hits), v_num((double)r.cache_misses) }) }) });
}
// (mimetic-seating instr)  where an instrument sits, as an azimuth in degrees (left positive): the seating Orchidea's
//   mixes use (violins left, cellos right, the winds in the middle); 0 for one it does not know
inline vptr mus_mimetic_seating(vlist& a, Interp& i) { return v_num(orchidea::seat_of(str_of(a[0]))); }

// --- continuations: a note that goes on where a note of the same sound ends becomes one longer note ----------------
// spans in order of onset, each (at, dur, key): a span whose key is empty never merges; one whose key a span already
// kept has and which starts within 30 ms of that span's end, and with which it would last no more than hold seconds,
// lengthens it (to where it ends, never shorter). => the spans kept: (the index of the span that begins it, its length)
struct span { double at = 0, dur = 0; std::string key; };
inline std::vector<std::pair<size_t, double>> continuation_plan(const std::vector<span>& spans, double hold) {
    std::vector<std::pair<size_t, double>> out; std::map<std::string, std::vector<size_t>> by_key;   // key -> the kept spans of that sound
    for (size_t k = 0; k < spans.size(); k++) {
        const span& x = spans[k]; long prev = -1;
        if (!x.key.empty()) { auto it = by_key.find(x.key);
            if (it != by_key.end()) for (size_t j : it->second) { double at = spans[out[j].first].at, d = out[j].second;
                if (std::fabs(at + d - x.at) < 0.03 && x.at + x.dur - at <= hold) { prev = (long)j; break; } } }
        if (prev < 0) { out.push_back({ k, x.dur }); if (!x.key.empty()) by_key[x.key].push_back(out.size() - 1); }
        else { double at = spans[out[(size_t)prev].first].at; out[(size_t)prev].second = std::max(out[(size_t)prev].second, x.at + x.dur - at); }
    }
    return out;
}
// (continuation-plan spans hold)   spans a list of (list at dur key), key a string or nil (never merged), in order of
//   onset: => a list of (list k dur), one per note kept: k the index of the span that begins it, dur its length; a span
//   that starts within 30 ms of the end of a kept span with the same key, and with which it lasts no more than hold
//   seconds, lengthens it instead (merge-continuations-within is made of it)
inline vptr mus_continuation_plan(vlist& a, Interp& i) {
    std::vector<span> spans;
    for (auto& s : i.list(a[0])) {
        if (s->t != Value::LIST || s->l.size() < 3) i.bad("continuation-plan: a span is (list at dur key)");
        span x; x.at = i.scalar(s->l[0]); x.dur = i.scalar(s->l[1]);
        if (s->l[2]->t != Value::NIL) { x.key = str_of(s->l[2]); if (x.key.empty()) x.key = " "; }
        spans.push_back(std::move(x));
    }
    vlist out; for (auto& p : continuation_plan(spans, i.scalar(a[1]))) out.push_back(v_list({ v_num((double)p.first), v_num(p.second) }));
    return v_list(std::move(out));
}

// --- Maple: matching pursuit (music/maple.h) ------------------------------------------------------------------------
// A dictionary, a book (a decomposition) and a sound-types model are opaque values: (type d) is "opaque:maple-dictionary",
// "opaque:maple-book" or "opaque:soundtypes-model". Options are a list of keys and values, (list 'atoms 500 'snr 20),
// or a record of them.
struct maple_book_value { maple::Book book; std::shared_ptr<const maple::Dictionary> dict; std::vector<float> residual; };

inline std::vector<float> to_floats(const varr& x) { std::vector<float> y(x.size()); for (size_t k = 0; k < x.size(); ++k) y[k] = (float)x[k]; return y; }
inline varr to_varr(const std::vector<float>& x) { varr y(x.size()); for (size_t k = 0; k < x.size(); ++k) y[k] = x[k]; return y; }
// an option: from a record ((key value) pairs) or a plain list of keys and values ('atoms 500 'snr 20), or nullptr
inline const vptr* opt_of(const vptr& rec, const char* key) {
    if (!rec || rec->t != Value::LIST) return nullptr;
    auto is_key = [&](const vptr& k) { return k && (k->t == Value::SYM || k->t == Value::STR) && k->s == key; };
    if (!rec->l.empty() && rec->l[0] && rec->l[0]->t != Value::LIST) {
        for (size_t k = 0; k + 1 < rec->l.size(); k += 2) if (is_key(rec->l[k])) return &rec->l[k + 1];
        return nullptr;
    }
    for (auto& p : rec->l)
        if (p && p->t == Value::LIST && p->l.size() == 2 && is_key(p->l[0])) return &p->l[1];
    return nullptr;
}
inline double opt_num(Interp& i, const vptr& rec, const char* key, double dflt) { auto v = opt_of(rec, key); return v ? i.scalar(*v) : dflt; }
inline std::string opt_word(Interp& i, const vptr& rec, const char* key, const std::string& dflt) {
    auto v = opt_of(rec, key); if (!v) return dflt;
    if ((*v)->t == Value::SYM || (*v)->t == Value::STR) return (*v)->s;
    i.bad(std::string("option ") + key + ": a word"); return dflt;
}
inline std::string word_of(Interp& i, const vptr& v) {
    if (v->t == Value::SYM || v->t == Value::STR) return v->s;
    i.bad("a word"); return "";
}
template <class T> std::shared_ptr<T> opaque_of(Interp& i, const vptr& v, const char* tag, const char* who) {
    if (v->t != Value::OPAQUE || v->opaque_tag != tag) i.bad(std::string(who) + ": not a " + tag);
    return std::static_pointer_cast<T>(v->opaque);
}
// progress for the long computations: the host's yield (so that Stop stops them)
inline maple::Tick musil_tick(Interp& i) { return [&i](double) { if (i.yield_fn) i.yield_fn(); return true; }; }
inline vptr v_dict(std::shared_ptr<maple::Dictionary> d) { return v_opaque("maple-dictionary", std::move(d)); }

// (maple-gabor sr [lo hi fmax]) a dictionary of Gabor atoms (Gaussian-windowed cosines) for sounds at rate sr: every
//   window length from lo to hi samples (powers of 2; 256 and 8192), every frequency up to fmax (16000), every position
inline vptr mus_maple_gabor(vlist& a, Interp& i) {
    return v_dict(std::make_shared<maple::Dictionary>(maple::parametric(false, i.scalar(a[0]), a.size() > 1 ? (int)i.scalar(a[1]) : 256,
        a.size() > 2 ? (int)i.scalar(a[2]) : 8192, a.size() > 3 ? i.scalar(a[3]) : 16000)));
}
// (maple-gammatone sr [lo hi fmax]) the same with a gammatone envelope (sharp attack, slow decay): better on attacks
inline vptr mus_maple_gammatone(vlist& a, Interp& i) {
    return v_dict(std::make_shared<maple::Dictionary>(maple::parametric(true, i.scalar(a[0]), a.size() > 1 ? (int)i.scalar(a[1]) : 256,
        a.size() > 2 ? (int)i.scalar(a[2]) : 8192, a.size() > 3 ? i.scalar(a[3]) : 16000)));
}
// (maple-cut x sr mode [a b] [source]) a dictionary of atoms cut from a sound: mode 'frames (Hann frames a ms long,
//   every b ms; 100 100), 'onsets (the spans between onsets, threshold a, at most b ms; 2 1000) or 'whole (at most
//   a ms; 2000). Each atom has unit energy and remembers its source (a name) and where it was cut
inline vptr mus_maple_cut(vlist& a, Interp& i) {
    double sr = i.scalar(a[1]);
    maple::CutOptions c;
    c.mode = word_of(i, a[2]);
    if (c.mode == "files") c.mode = "whole";
    if (c.mode != "frames" && c.mode != "onsets" && c.mode != "whole") i.bad("maple-cut: mode frames, onsets or whole");
    if (c.mode == "frames") { c.ms = a.size() > 3 ? i.scalar(a[3]) : 100; c.hop = a.size() > 4 ? i.scalar(a[4]) : c.ms; }
    if (c.mode == "onsets") { c.threshold = a.size() > 3 ? i.scalar(a[3]) : 2; c.maxMs = a.size() > 4 ? i.scalar(a[4]) : 1000; }
    if (c.mode == "whole") c.maxMs = a.size() > 3 ? i.scalar(a[3]) : 2000;
    std::string src = a.size() > 5 ? i.str(a[5]) : "sound";
    auto d = std::make_shared<maple::Dictionary>();
    d->kind = "sounds"; d->sr = sr;
    maple::MelDescriptor desc(sr);
    maple::cutSound(to_floats(i.num(a[0])), sr, c, src, desc, d->atoms);
    return v_dict(d);
}
// (maple-merge d1 d2 ...) one dictionary of the atoms of several (dictionaries of sounds at one rate)
inline vptr mus_maple_merge(vlist& a, Interp& i) {
    auto out = std::make_shared<maple::Dictionary>();
    out->kind = "sounds";
    for (size_t k = 0; k < a.size(); ++k) {
        auto d = opaque_of<maple::Dictionary>(i, a[k], "maple-dictionary", "maple-merge");
        if (d->parametric()) i.bad("maple-merge: dictionaries of sounds");
        if (k == 0) out->sr = d->sr; else if (d->sr != out->sr) i.bad("maple-merge: dictionaries at different rates");
        out->atoms.insert(out->atoms.end(), d->atoms.begin(), d->atoms.end());
    }
    return v_dict(out);
}
// (maple-info d) a dictionary as a record: kind (gabor, gammatone, sounds), sr, size (atoms, or scales), and the
//   windows (lo hi) and fmax of a parametric one
inline vptr mus_maple_info(vlist& a, Interp& i) {
    auto d = opaque_of<maple::Dictionary>(i, a[0], "maple-dictionary", "maple-info");
    vlist r = { v_list({ v_sym("kind"), v_str(d->kind) }), v_list({ v_sym("sr"), v_num(d->sr) }), v_list({ v_sym("size"), v_num((double)d->size()) }) };
    if (d->parametric()) { r.push_back(v_list({ v_sym("lo"), v_num(d->minScale) })); r.push_back(v_list({ v_sym("hi"), v_num(d->maxScale) })); r.push_back(v_list({ v_sym("fmax"), v_num(d->fmax) })); }
    return v_list(std::move(r));
}
// (maple-atom d k) atom k of a dictionary of sounds, a vector (unit energy), to listen to or plot
inline vptr mus_maple_atom(vlist& a, Interp& i) {
    auto d = opaque_of<maple::Dictionary>(i, a[0], "maple-dictionary", "maple-atom");
    long k = i.index(a[1]);
    if (d->parametric() || k < 0 || k >= (long)d->atoms.size()) i.bad("maple-atom: no atom " + std::to_string(k));
    return v_arr(to_varr(d->atoms[(size_t)k].x));
}
// (maple-save d path) a dictionary to a file; (maple-load path) one from a file (also those Moritz's atoms object writes)
inline vptr mus_maple_save(vlist& a, Interp& i) {
    auto d = opaque_of<maple::Dictionary>(i, a[0], "maple-dictionary", "maple-save");
    if (!maple::saveDictionary(*d, i.str(a[1]))) i.bad("maple-save: cannot write " + i.str(a[1]));
    return v_nil();
}
// (maple-load path) a dictionary saved with maple-save
inline vptr mus_maple_load(vlist& a, Interp& i) {
    auto d = std::make_shared<maple::Dictionary>();
    std::string path = i.read_path(i.str(a[0]));
    if (!maple::loadDictionary(path, *d, musil_tick(i))) i.bad("maple-load: " + i.str(a[0]) + " is not a dictionary");
    return v_dict(d);
}
// (maple-decompose x sr d [opts]) a sound explained by the atoms of d, the strongest first: a book. Options: 'atoms (2000)
//   and 'snr (30 dB: stop when the residual is that far under the sound) for Gabor and gammatone; for sounds 'components
//   (4 atoms a segment), 'candidates (16, by mel preselection), 'segments ('onsets or 'blocks), 'block (100 ms, the
//   blocks or the longest segment), 'threshold (2, onsets). A sound at another rate than a dictionary of sounds is resampled
inline vptr mus_maple_decompose(vlist& a, Interp& i) {
    auto d = opaque_of<maple::Dictionary>(i, a[2], "maple-dictionary", "maple-decompose");
    vptr o = a.size() > 3 ? a[3] : v_nil();
    maple::SoundPursuitOptions so;
    so.components = std::clamp((int)opt_num(i, o, "components", 4), 1, 64);
    so.candidates = std::clamp((int)opt_num(i, o, "candidates", 16), 1, 100000);
    so.onsets = opt_word(i, o, "segments", "onsets") != "blocks";
    so.block = std::max(.005, opt_num(i, o, "block", 100) / 1000.0);
    so.threshold = std::max(0.0, opt_num(i, o, "threshold", 2));
    auto b = std::make_shared<maple_book_value>();
    b->dict = d;
    if (!maple::decompose(*d, to_floats(i.num(a[0])), i.scalar(a[1]), std::max(1, (int)opt_num(i, o, "atoms", 2000)),
                          opt_num(i, o, "snr", 30), so, b->book, b->residual, musil_tick(i)))
        i.bad("maple-decompose: stopped");
    return v_opaque("maple-book", b);
}
// (maple-book b) a book as 8 rows, one value per atom: time (s, the atom's centre), frequency (Hz), level (dB), atom
//   (-1: Gabor or gammatone), length (samples), amplitude, phase, start (samples). (add-scatter fig (head b) (getidx b 1) "")
//   draws the atom map
inline vptr mus_maple_book(vlist& a, Interp& i) {
    auto b = opaque_of<maple_book_value>(i, a[0], "maple-book", "maple-book");
    vlist rows;
    for (auto& r : maple::bookRows(b->book, *b->dict)) rows.push_back(v_arr(varr(r.data(), r.size())));
    return v_list(std::move(rows));
}
// (maple-residual b) what the atoms of a book leave unexplained, a vector at the book's rate
inline vptr mus_maple_residual(vlist& a, Interp& i) { return v_arr(to_varr(opaque_of<maple_book_value>(i, a[0], "maple-book", "maple-residual")->residual)); }
// (maple-snr b) how well a book explains its sound: the sound's energy over the residual's, in dB
inline vptr mus_maple_snr(vlist& a, Interp& i) { return v_num(maple::snrOf(opaque_of<maple_book_value>(i, a[0], "maple-book", "maple-snr")->book)); }
// (maple-rate b) the rate of a book (its dictionary's, for sounds)
inline vptr mus_maple_rate(vlist& a, Interp& i) { return v_num(opaque_of<maple_book_value>(i, a[0], "maple-book", "maple-rate")->book.sr); }
// (maple-synth b [opts] [d]) the sound rebuilt from a book: options 'ratio (transposition, 1), 'stretch (time, 1), 'tails
//   (1: sound atoms ring on whole; 0: only what matched), 'keep (the share of atoms, strongest first, 1). With another
//   dictionary of sounds d, each atom is replaced by the closest of d: the sound made of the other's material (cross
//   synthesis). Never above 0.99: louder results are scaled down
inline vptr mus_maple_synth(vlist& a, Interp& i) {
    auto b = opaque_of<maple_book_value>(i, a[0], "maple-book", "maple-synth");
    vptr o = a.size() > 1 ? a[1] : v_nil();
    maple::SynthOptions so;
    so.ratio = std::clamp(opt_num(i, o, "ratio", 1), .05, 20.0);
    so.stretch = std::clamp(opt_num(i, o, "stretch", 1), .05, 50.0);
    so.tails = opt_num(i, o, "tails", 1) != 0;
    so.keep = std::clamp(opt_num(i, o, "keep", 1), 0.0, 1.0);
    maple::Book book = b->book;
    std::shared_ptr<const maple::Dictionary> to = b->dict;
    if (a.size() > 2) {
        to = opaque_of<maple::Dictionary>(i, a[2], "maple-dictionary", "maple-synth");
        if (b->dict->parametric() || to->parametric()) i.bad("maple-synth: cross synthesis needs two dictionaries of sounds");
        book = maple::crossBook(book, *b->dict, *to);
    }
    std::vector<float> y = maple::synthesize(book, *to, so);
    float peak = 0;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    if (peak > .99f) for (auto& v : y) v *= .99f / peak;
    return v_arr(to_varr(y));
}

// --- SoundTypes: sounds as sequences of types (music/soundtypes.h) ---------------------------------------------------
inline soundtypes::AnalyseOptions types_options(Interp& i, const vptr& o) {
    soundtypes::AnalyseOptions ao;
    if (auto t = opt_of(o, "types")) {
        ao.types.k.clear();
        if ((*t)->t == Value::LIST) for (auto& v : (*t)->l) ao.types.k.push_back(std::max(1, (int)i.scalar(v)));
        else for (double v : i.num(*t)) ao.types.k.push_back(std::max(1, (int)v));
        while (ao.types.k.size() < 3) ao.types.k.push_back(ao.types.k.empty() ? 12 : std::max(2, ao.types.k.back() / 2));
    }
    ao.types.levels = std::clamp((int)opt_num(i, o, "levels", 2), 1, 3);
    ao.types.threshold = opt_num(i, o, "threshold", 1);
    ao.types.smooth = std::clamp((int)opt_num(i, o, "smooth", 5), 1, 101) | 1;
    ao.types.seed = (unsigned)opt_num(i, o, "seed", 1);
    int n = 1; while (n < (int)opt_num(i, o, "size", 2048)) n <<= 1;
    ao.size = std::clamp(n, 256, 16384);
    ao.hop = std::clamp((int)opt_num(i, o, "hop", 512), 32, 8192);
    ao.onsets = opt_word(i, o, "segment", "frames") == "onsets";
    ao.onsetThreshold = opt_num(i, o, "onset-threshold", 1.5);
    ao.onsetGap = opt_num(i, o, "onset-gap", .08);
    ao.pitch = opt_num(i, o, "pitch", 0) != 0;
    return ao;
}
inline vptr v_model(std::shared_ptr<soundtypes::Model> m) { return v_opaque("soundtypes-model", std::move(m)); }
inline std::shared_ptr<soundtypes::Model> model_of(Interp& i, const vptr& v, const char* who) { return opaque_of<soundtypes::Model>(i, v, "soundtypes-model", who); }
inline int level_of(Interp& i, const soundtypes::Model& m, const vptr& v, const char* who) {
    int l = (int)i.scalar(v);
    if (l < 1 || l > (int)m.levels.size()) i.bad(std::string(who) + ": level 1 to " + std::to_string(m.levels.size()));
    return l - 1;
}

// (soundtypes-analyse x sr [opts]) a sound as types (SoundTypes): its frames (MFCCs, centroid, loudness) clustered into
//   types, runs of one type the units of level 1; where the next type is surprising a gesture ends, and the gestures
//   are clustered into the types of level 2, and so on. Options: 'types (list 12 6 4), 'levels (2, at most 3),
//   'threshold (1: surprise above the mean plus 1 sd), 'smooth (5 frames), 'seed, 'size (2048), 'hop (512), 'segment
//   ('frames, or 'onsets: level 1 the spans between onsets, 'onset-threshold 1.5 'onset-gap 0.08 s), 'pitch (1 adds
//   the chroma: types follow harmony too)
inline vptr mus_soundtypes_analyse(vlist& a, Interp& i) {
    auto m = std::make_shared<soundtypes::Model>();
    m->sound = to_floats(i.num(a[0]));
    m->sr = i.scalar(a[1]);
    if (!soundtypes::analyseModel(*m, types_options(i, a.size() > 2 ? a[2] : v_nil()), musil_tick(i))) i.bad("soundtypes-analyse: the analysis found nothing (too short?)");
    return v_model(m);
}
// (soundtypes-analyse-book b [opts]) the same on the atoms of a pursuit book (maple-decompose): units are rebuilt
//   from their atoms (the frames are those of the atoms; 'segment 'onsets needs a sound)
inline vptr mus_soundtypes_analyse_book(vlist& a, Interp& i) {
    auto b = opaque_of<maple_book_value>(i, a[0], "maple-book", "soundtypes-analyse-book");
    auto m = std::make_shared<soundtypes::Model>();
    m->book = std::make_shared<maple::Book>(b->book);
    m->dict = b->dict;
    m->sr = b->book.sr;
    soundtypes::AnalyseOptions o = types_options(i, a.size() > 1 ? a[1] : v_nil());
    o.onsets = false;
    if (!soundtypes::analyseModel(*m, o, musil_tick(i))) i.bad("soundtypes-analyse-book: the analysis found nothing");
    return v_model(m);
}
// (soundtypes-levels m) the number of units at each level, a list
inline vptr mus_soundtypes_levels(vlist& a, Interp& i) {
    auto m = model_of(i, a[0], "soundtypes-levels");
    vlist r; for (auto& L : m->levels) r.push_back(v_num((double)L.units.size()));
    return v_list(std::move(r));
}
// (soundtypes-units m level) the units of a level (1, 2, 3), each (list start end type), in seconds
inline vptr mus_soundtypes_units(vlist& a, Interp& i) {
    auto m = model_of(i, a[0], "soundtypes-units");
    int l = level_of(i, *m, a[1], "soundtypes-units");
    vlist r;
    for (auto& u : m->levels[(size_t)l].units) r.push_back(v_list({ v_num(u.start / m->sr), v_num(u.end / m->sr), v_num(u.type) }));
    return v_list(std::move(r));
}
// (soundtypes-labels m) the type of every frame, a vector per level (one value per hop): plot them as images
inline vptr mus_soundtypes_labels(vlist& a, Interp& i) {
    auto m = model_of(i, a[0], "soundtypes-labels");
    vlist r; for (auto& s : soundtypes::typeStrips(*m)) r.push_back(v_arr(to_varr(s)));
    return v_list(std::move(r));
}
// (soundtypes-transitions m level) how often each type follows each other at a level: a matrix (row: from, column: to)
inline vptr mus_soundtypes_transitions(vlist& a, Interp& i) {
    auto m = model_of(i, a[0], "soundtypes-transitions");
    int l = level_of(i, *m, a[1], "soundtypes-transitions");
    vlist r; for (auto& row : soundtypes::transitions(m->levels[(size_t)l])) r.push_back(v_arr(to_varr(row)));
    return v_list(std::move(r));
}
// (soundtypes-generate m level seconds [opts]) a new sequence of the units of a level, made sound: (list sound units),
//   the units' numbers in the order played. Options: 'method ('oracle: a factor oracle, continue the original with
//   'continuity 0.8 or jump where the last 'context 1 units repeat; 'markov: the longest context seen 'min-count 2
//   times, up to 'order; 'classic: n-grams of 'order 2), 'seed, 'xfade (10 ms), 'inner (1; under 1, each gesture of
//   level 2 or 3 is played by new units of level 1 with that continuity, 'inner-context 1)
inline vptr mus_soundtypes_generate(vlist& a, Interp& i) {
    auto m = model_of(i, a[0], "soundtypes-generate");
    int l = level_of(i, *m, a[1], "soundtypes-generate");
    vptr o = a.size() > 3 ? a[3] : v_nil();
    soundtypes::GenOptions go;
    std::string method = opt_word(i, o, "method", "oracle");
    go.method = method == "classic" ? soundtypes::Method::Classic : method == "markov" ? soundtypes::Method::Markov : soundtypes::Method::Oracle;
    if (method != "classic" && method != "markov" && method != "oracle") i.bad("soundtypes-generate: method oracle, markov or classic");
    go.order = std::clamp((int)opt_num(i, o, "order", 2), 1, 16);
    go.minCount = std::max(1, (int)opt_num(i, o, "min-count", 2));
    go.continuity = std::clamp(opt_num(i, o, "continuity", .8), 0.0, 1.0);
    go.context = std::max(0, (int)opt_num(i, o, "context", 1));
    go.seed = (unsigned)opt_num(i, o, "seed", 1);
    std::vector<int> seq = soundtypes::generate(*m, l, std::max(.1, i.scalar(a[2])), go);
    std::vector<float> y = soundtypes::realise(*m, l, seq, std::clamp(opt_num(i, o, "xfade", 10), .5, 500.0),
                                               std::clamp(opt_num(i, o, "inner", 1), 0.0, 1.0), std::max(0, (int)opt_num(i, o, "inner-context", 1)), go.seed);
    float peak = 0; for (float v : y) peak = std::max(peak, std::fabs(v));
    if (peak > .99f) for (auto& v : y) v *= .99f / peak;
    varr units(seq.size()); for (size_t k = 0; k < seq.size(); ++k) units[k] = seq[k];
    return v_list({ v_arr(to_varr(y)), v_arr(units) });
}
// (soundtypes-match a b level [opts]) SoundTypes' rematch: sound a rebuilt with the material of b. Level 0: frame by
//   frame (b's frames overlap-added at a's); 1, 2...: unit by unit, each of a's replaced by the closest of b in its
//   closest type. Options: 'fit (1: at a's times and durations; 0: one after the other), 'contour (0..1: a's loudness
//   imposed on the result), 'space ('own: each sound's features in its own range; 'joint: one space), 'xfade (10 ms).
//   Both models analysed the same way (both sounds, or both books); the result is at b's rate
inline vptr mus_soundtypes_match(vlist& a, Interp& i) {
    auto A = model_of(i, a[0], "soundtypes-match"), B = model_of(i, a[1], "soundtypes-match");
    if (A->frames.empty() || B->frames.empty() || A->frames[0].size() != B->frames[0].size())
        i.bad("soundtypes-match: the two must be analysed the same way (both sounds, or both books)");
    int level = std::max(0, (int)i.scalar(a[2]));
    vptr o = a.size() > 3 ? a[3] : v_nil();
    std::vector<float> y = soundtypes::rematch(*A, *B, level, opt_num(i, o, "fit", 1) != 0, opt_word(i, o, "space", "own") != "joint",
                                               std::clamp(opt_num(i, o, "xfade", 10), .5, 500.0), std::clamp(opt_num(i, o, "contour", 0), 0.0, 1.0));
    float peak = 0; for (float v : y) peak = std::max(peak, std::fabs(v));
    if (peak > .99f) for (auto& v : y) v *= .99f / peak;
    return v_arr(to_varr(y));
}

inline void add_atoms_and_types(Interp& i) {
    i.def("maple-gabor", mus_maple_gabor, 1, 4); i.def("maple-gammatone", mus_maple_gammatone, 1, 4);
    i.def("maple-cut", mus_maple_cut, 3, 6); i.def("maple-merge", mus_maple_merge, 1, -1);
    i.def("maple-info", mus_maple_info, 1, 1); i.def("maple-atom", mus_maple_atom, 2, 2);
    i.def("maple-save", mus_maple_save, 2, 2); i.def("maple-load", mus_maple_load, 1, 1);
    i.def("maple-decompose", mus_maple_decompose, 3, 4); i.def("maple-book", mus_maple_book, 1, 1);
    i.def("maple-residual", mus_maple_residual, 1, 1); i.def("maple-snr", mus_maple_snr, 1, 1); i.def("maple-rate", mus_maple_rate, 1, 1);
    i.def("maple-synth", mus_maple_synth, 1, 3);
    i.def("soundtypes-analyse", mus_soundtypes_analyse, 2, 3); i.def("soundtypes-analyse-book", mus_soundtypes_analyse_book, 1, 2);
    i.def("soundtypes-levels", mus_soundtypes_levels, 1, 1); i.def("soundtypes-units", mus_soundtypes_units, 2, 2);
    i.def("soundtypes-labels", mus_soundtypes_labels, 1, 1); i.def("soundtypes-transitions", mus_soundtypes_transitions, 2, 2);
    i.def("soundtypes-generate", mus_soundtypes_generate, 3, 4); i.def("soundtypes-match", mus_soundtypes_match, 3, 4);
}

inline void add_music(Interp& i) {
    i.def("pitch->midi", mus_pitch_to_midi, 1, 1); i.def("midi->pitch", mus_midi_to_pitch, 1, 1);
    i.def("db-read", mus_db_read, 1, 1); i.def("db-gen", mus_db_gen, 6, 6); add_midi(i); i.def("db-locate", mus_db_locate, 2, 2); i.def("db-index-size", mus_db_index_size, 1, 1);
    i.def("orchidea-analyse", mus_orchidea_analyse, 7, 7); i.def("orchidea-search", mus_orchidea_search, 4, 4);
    i.def("mimetic-seating", mus_mimetic_seating, 1, 1); i.def("continuation-plan", mus_continuation_plan, 2, 2); i.def("db-sounds-folder", mus_db_sounds_folder, 2, 2);
    add_atoms_and_types(i);
    add_score(i);
}

} // namespace musil
