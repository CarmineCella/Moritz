// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_orchidea.cpp — assisted orchestration, after Orchidea, on Musil's engine (musil/src/music/orchidea.h,
// music/granular.h and the C++ half of music.h): orchestra (a database of instrumental samples and the players) and
// orchidea (a target sound orchestrated by a genetic search, the orchestra as a granulator, or the granulator driven by
// a target sound, as background jobs).
//
// The analysis of the target (orchidea::analyse), the search (orchidea::search_segments), the database's file
// (db_parse, db_sounds_folder, db_locate), the seating and the merging of held notes (continuation_plan) are Musil's
// C++, the very code orchidea-analyse, orchidea-search and orchestrate-mimetic run: for the same target, database,
// orchestra and seed the object and Musil find the same notes (tests/cross/orchidea.moritz). What is Moritz's own is
// the plumbing: buffer~s in and out, the notes as messages, the sum of the chosen samples. The granular and
// morphological modes run Musil's granulator (musil::granular::granulate, orchestrate_morphological), the very code of
// orchestrate-granular and orchestrate-morphological: the same parameters and seed write the same notes
// (tests/cross/orchidea-granular.moritz, orchidea-morphological.moritz).

#include "musil_host.h"
#include "datasets.h"
#include "fileio.h"
#include "offline.h"
#include "transport.h"

#include "music/score.h"
#include "music/score_layout.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace moritz {

namespace orch = musil::orchidea;
namespace gr = musil::granular;

namespace {

// a database of sounds as the search wants it, read once and then shared (immutable) by the jobs
struct OrchDb {
    std::string path, root, type;
    int block = 0, hop = 0, ncoeff = 0;
    std::vector<orch::entry> entries;          // index = place in the file
    std::vector<std::string> files;            // the file as the feature file names it
    std::vector<std::string> paths;            // where the sound is on disk (as db-path makes it)
    std::vector<char> present;                 // is it there?
};
using DbPtr = std::shared_ptr<const OrchDb>;

// what the orchidea object takes from an orchestra when it runs
struct OrchSpec {
    DbPtr db;
    std::vector<std::string> players;                             // a player is an instrument, or "Fl|Picc" (a doubling)
    std::vector<int> groups;                                      // each player's paired group ("Vn+Vc"), or -1
    std::map<std::string, std::pair<int, int>> ranges;            // instrument -> lowest and highest MIDI pitch allowed
    std::vector<std::string> styles, dynamics;                    // only these techniques and dynamics (empty: all)
    musil::dbq::selection sel;                                    // select: the sounds a query chooses (Musil's db-select)
};
using SpecPtr = std::shared_ptr<const OrchSpec>;

// a sound of the database as a query sees it (music/db_query.h; Musil's db_sample_of makes the same of an entry)
musil::dbq::sample sampleOf(const OrchDb& db, size_t k) {
    const orch::entry& e = db.entries[k];
    musil::dbq::sample s;
    s.instr = e.instr; s.tech = e.tech; s.pitch = e.pitch; s.dyn = e.dyn; s.other = e.other; s.file = db.files[k];
    s.midi = e.midi;
    if (db.type == "moments" && e.features.size() >= 4) {
        s.has_moments = true;
        for (int j = 0; j < 4; ++j) s.moments[j] = e.features[(size_t)j];
    }
    return s;
}
// does the orchestra let its players have sound k: the instrument's range, and the selection
bool rangeAllows(const OrchSpec& s, const orch::entry& e) {
    auto r = s.ranges.find(e.instr);
    return r == s.ranges.end() || e.midi < 0 || (e.midi >= r->second.first && e.midi <= r->second.second);
}
bool inSelection(const OrchSpec& s, size_t k) { return s.sel.empty() || s.sel.matches(sampleOf(*s.db, k)); }
bool listed(const std::vector<std::string>& l, const std::string& x) { return l.empty() || std::find(l.begin(), l.end(), x) != l.end(); }
// the instruments the players play (Fl|Picc: both)
std::vector<std::string> instrumentsOf(const OrchSpec& s) {
    std::vector<std::string> out;
    for (auto& p : s.players) {
        std::vector<std::string> alts;
        orch::tokenize(p, alts, '|');
        for (auto& a : alts) if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    }
    return out;
}
// how many sounds the orchestra's filters leave (ranges, techniques, dynamics, selection), in all and per instrument
musil::dbq::counts spaceCounts(const OrchSpec& s) {
    musil::dbq::counts c;
    for (size_t k = 0; k < s.db->entries.size(); ++k) {
        const orch::entry& e = s.db->entries[k];
        c.add(e.instr, rangeAllows(s, e) && listed(s.styles, e.tech) && listed(s.dynamics, e.dyn) && inSelection(s, k));
    }
    return c;
}
// what the orchestra's filters say, for the messages: "select dynamics p..f; Vn: string 1; techniques ord"
std::string filtersText(const OrchSpec& s) {
    std::string t = s.sel.empty() ? "" : "select " + s.sel.text();
    auto add = [&](const char* what, const std::vector<std::string>& l) {
        if (l.empty()) return;
        t += (t.empty() ? "" : "; ") + std::string(what);
        for (auto& x : l) t += " " + x;
    };
    add("techniques", s.styles);
    add("dynamics", s.dynamics);
    for (auto& r : s.ranges) t += (t.empty() ? "" : "; ") + ("range " + r.first + " " + musil::midi_to_pitch(r.second.first) + " " + musil::midi_to_pitch(r.second.second));
    return t.empty() ? "no filter" : t;
}
// the space an orchestra leaves its players, said in a line: "14 of 32 sounds remain (select family strings): Ob 0 of
// 8, Hn 0 of 8, Vn 6 of 8, Vc 8"; empty: every player's instrument is left without a sound
std::string spaceText(const OrchSpec& s, bool& empty) {
    auto c = spaceCounts(s);
    auto ins = instrumentsOf(s);
    auto silent = c.silent(ins);
    empty = !ins.empty() && silent.size() == ins.size();
    std::string t = c.text(ins);
    size_t colon = t.find(':');
    std::string head = colon == std::string::npos ? t : t.substr(0, colon), tail = colon == std::string::npos ? "" : t.substr(colon);
    return head + " remain (" + filtersText(s) + ")" + tail;
}

// the players of an orchestra as the granulator keeps them between runs that carry on ('continue): their bookings,
// last pitches and seats; one run at a time books them
struct CarryState {
    std::string key;                                              // the players they are (another orchestra: fresh ones)
    std::vector<gr::player> players;
    bool inFlight = false;
};
using CarryPtr = std::shared_ptr<CarryState>;

// Orchidea's default orchestra (static_orchidea.mu): what the database has of it
const char* const kDefaultOrchestra[] = {"Fl", "Fl", "Ob", "Ob", "ClBb", "ClBb", "Bn", "Bn", "Hn", "Hn", "TpC", "TpC",
                                         "Tbn", "Tbn", "BTb", "Vn", "Vn", "Va", "Va", "Vc", "Vc", "Cb", "Cb"};

// reads a feature file and finds its sounds (on a job's thread): as db-load does in music.mu
std::shared_ptr<OrchDb> readDatabase(const std::string& full, Job& job, std::string& err) {
    auto db = std::make_shared<OrchDb>();
    db->path = full;
    musil::db_header h;
    try {
        h = musil::db_parse(full, full, [&](musil::db_record&& r) {
            orch::entry e;
            e.index = (int)db->entries.size();
            e.midi = r.midi;
            e.instr = r.instr; e.tech = r.tech; e.pitch = r.pitch; e.dyn = r.dyn; e.other = r.other;
            e.features.assign(r.features.begin(), r.features.end());
            db->files.push_back(r.file);
            db->entries.push_back(std::move(e));
        });
    } catch (const std::exception& e) {
        err = e.what();
        return nullptr;
    }
    if (job.cancelled()) return nullptr;
    job.progress(.5f);
    db->type = h.type;
    db->block = (int)std::clamp(h.block, 0L, 1L << 24);
    db->hop = (int)std::clamp(h.hop, 0L, 1L << 24);
    db->ncoeff = (int)std::clamp(h.ncoeff, 0L, 1L << 24);
    std::string dir = std::filesystem::path(full).parent_path().generic_string();
    if (dir.empty()) dir = ".";
    std::vector<std::string> probes(db->files.begin(), db->files.begin() + (long)std::min<size_t>(20, db->files.size()));
    db->root = musil::db_sounds_folder(dir, probes);
    for (size_t k = 0; k < db->files.size(); ++k) {
        const std::string& rel = db->files[k];
        std::string p = musil::db_locate(db->root, rel);
        db->present.push_back(!p.empty());
        db->paths.push_back(p.empty() ? db->root + (rel.size() && rel[0] == '/' ? "" : "/") + rel : p);
        if (k % 256 == 0) {
            job.progress(.5f + .5f * (float)k / (float)db->files.size());
            if (job.cancelled()) return nullptr;
        }
    }
    return db;
}

// a number from a message as an int in lo..hi (NaN: lo), without the undefined conversion of a huge float
int clampInt(float v, int lo, int hi) {
    if (!std::isfinite(v)) return v > 0 ? hi : lo;
    return (int)std::clamp((double)v, (double)lo, (double)hi);
}
// a pitch given as a name ("G3") or a MIDI number; -1 if neither
int pitchOf(const Atom& a) {
    if (a.isFloat()) return clampInt(std::round(a.f), -1, 1000);
    return musil::pitch_to_midi(a.str());
}

// ---------------------------------------------------------------- orchestra
// orchestra name [database]: a database of instrumental samples (a feature file in the *SOL layout, as Musil's
// db-load reads it) and the players who play it, shared by name with orchidea. The database is a file or a dataset's
// name (fullsol, tinysol, microsol: datasets.h, the same names as Musil's (dataset name)).
//   read file.db | read name     loads a database (a background job); the sounds are found next to it
//   analyse folder [type block hop ncoeff]   Orchidea's dbgen: every WAV under the folder analysed (Musil's
//                                db_generate; spectrum 2048 256 1024 by default) into <folder>.spectrum.db next to it
//                                (never over an existing file), which is then read
//   players Vn 2 Va Vc Fl|Picc Vn+Vc   the players: an instrument a player, a number after one repeats it, Fl|Picc a
//                                player alternating (an ossia), Vn+Vc players paired (the granulator's groups, sounding
//                                together); no instrument: Orchidea's default orchestra, of what the database has
//   add instrument [n]           more players; remove instrument: every player of it
//   range instrument lo hi       its pitches limited (names or MIDI numbers); range instrument: no limit
//   techniques t...              only these techniques (ord, pizz-lv...); none: all. dynamics d...: the same
//   select query                 only the sounds a query chooses (Musil's db-select, music/db_query.h): fields and
//                                values joined by and, or, not, parentheses: select technique ord|pizz* and dynamics
//                                p..f and pitch C4..G5; select instrument query: that instrument's sounds only
//                                (select Vn string 1..2); select instrument: its query cleared; select: every sound.
//                                Each says how many sounds remain, in all and per instrument of the players
//   rest                         the players' bookings forgotten (orchidea's granular runs that continue start afresh)
//   info, stop
// Outlets: progress, bang when the database is ready, number of sounds, the players (a list).
class OrchestraBox : public OfflineBox {
public:
    explicit OrchestraBox(const Message& a) : name(argstr(a, 0)), file_(argstr(a, 1)) {
        setPorts({P::Control}, {P::Control, P::Control, P::Control, P::Control});
    }
    ~OrchestraBox() override { job_.cancel(); }
    std::string name;

    void attached() override {
        if (!file_.empty() && !db_ && !job_.running()) read(file_);
    }
    std::vector<std::string> menuItems() override { return {"Info"}; }
    void menuAction(int) override { message(0, {Atom("info")}); }

    // the orchestra as it is now; nullptr when there is no database yet
    SpecPtr spec() const {
        if (!db_) return nullptr;
        auto s = std::make_shared<OrchSpec>();
        s->db = db_;
        s->ranges = ranges_;
        s->styles = styles_;
        s->dynamics = dynamics_;
        s->sel = sel_;
        int group = 0;
        for (const auto& p : players()) {
            std::vector<std::string> parts;
            orch::tokenize(p, parts, '+');
            if (parts.size() < 2) { s->players.push_back(p); s->groups.push_back(-1); continue; }
            for (auto& q : parts) { s->players.push_back(q); s->groups.push_back(group); }
            group++;
        }
        return s;
    }
    // the players' state between granular runs that carry on (shared with the runs: one at a time books them)
    CarryPtr carry() const { return carry_; }
    bool loading() const { return job_.running(); }
    // fn runs when the database being read is ready (at once when none is being read); dropped if this box goes first
    void whenReady(std::function<void()> fn) {
        if (!job_.running()) fn();
        else waiting_.push_back(std::move(fn));
    }

    void message(int, const Message& m) override {
        const Atom& h = m[0];
        if (h.is("stop")) {
            job_.cancel();
            waiting_.clear();
        } else if (h.is("read") && m.size() > 1) {
            read(m[1].str());
        } else if (h.is("analyse") || h.is("analyze")) {
            analyse(m);
        } else if (h.is("players")) {
            explicit_.clear();
            addPlayers(m, 1);
            outlet(3, playerList());
        } else if (h.is("add") && m.size() > 1) {
            if (explicit_.empty()) explicit_ = players();
            addPlayers(m, 1);
            outlet(3, playerList());
        } else if (h.is("remove") && m.size() > 1) {
            if (explicit_.empty()) explicit_ = players();
            std::string i = m[1].str();
            explicit_.erase(std::remove(explicit_.begin(), explicit_.end(), i), explicit_.end());
            if (explicit_.empty()) patch->print("orchestra " + name + ": no player left: the default orchestra plays");
            outlet(3, playerList());
        } else if (h.is("range") && m.size() > 1) {
            std::string i = m[1].str();
            if (m.size() < 4) { ranges_.erase(i); return; }
            int lo = pitchOf(m[2]), hi = pitchOf(m[3]);
            if (lo < 0 || hi < 0 || lo > 127 || hi > 127) { patch->print("orchestra " + name + ": range " + i + ": pitches are names (G3) or MIDI numbers"); return; }
            ranges_[i] = {std::min(lo, hi), std::max(lo, hi)};
        } else if (h.is("techniques")) {
            styles_.clear();
            for (size_t k = 1; k < m.size(); ++k) styles_.push_back(m[k].str());
        } else if (h.is("dynamics")) {
            dynamics_.clear();
            for (size_t k = 1; k < m.size(); ++k) dynamics_.push_back(m[k].str());
        } else if (h.is("select")) {
            select(m);
        } else if (h.is("rest")) {
            carry_->players.clear();
            carry_->key.clear();
        } else if (h.is("info")) {
            info();
        }
    }

private:
    // select [instrument] query: the query compiled (a mistake is said and the selection kept as it was), then how
    // many sounds remain
    void select(const Message& m) {
        std::string who = "orchestra " + name + ": select";
        if (m.size() < 2) {
            sel_ = {};
            sayRemaining();
            return;
        }
        std::string first = m[1].str();
        bool whole = musil::dbq::starts_query(first);
        if (!whole && db_ && !hasInstrument(first)) {
            patch->print(who + ": '" + first + "' is neither a field (" + musil::dbq::fields_text(db_->type == "moments") +
                         ") nor an instrument of the database (the selection is unchanged)");
            return;
        }
        std::string text;
        for (size_t k = whole ? 1 : 2; k < m.size(); ++k) text += (text.empty() ? "" : " ") + m[k].str();
        musil::dbq::options o;
        o.type = db_ ? db_->type : "";
        musil::dbq::query q;
        try {
            q = musil::dbq::compile(text, o);
        } catch (const std::exception& e) {
            patch->print(who + (whole ? "" : " " + first) + ": " + e.what() + " (the selection is unchanged)");
            return;
        }
        if (whole) { sel_.all = q; sel_.all_text = text; }
        else if (!q) sel_.per.erase(first);
        else sel_.per[first] = {q, text};
        sayRemaining();
    }
    bool hasInstrument(const std::string& i) const {
        for (auto& e : db_->entries) if (e.instr == i) return true;
        return false;
    }
    // a database read: the queries compiled again for its features (centroid... needs a database of moments)
    void recompileSelection() {
        musil::dbq::options o;
        o.type = db_->type;
        auto again = [&](musil::dbq::query& q, const std::string& text) {
            if (!q) return true;
            try { q = musil::dbq::compile(text, o); return true; }
            catch (const std::exception& e) { patch->print("orchestra " + name + ": select " + text + ": " + e.what() + ": dropped"); return false; }
        };
        if (!again(sel_.all, sel_.all_text)) { sel_.all = nullptr; sel_.all_text.clear(); }
        for (auto it = sel_.per.begin(); it != sel_.per.end();) {
            if (again(it->second.first, it->second.second)) ++it;
            else it = sel_.per.erase(it);
        }
    }
    // how many sounds the filters leave the players, said in the Console; a player left without any, and an orchestra
    // left without any, said clearly
    void sayRemaining() {
        if (!db_) {
            patch->print("orchestra " + name + ": the selection is " + sel_.text() + "; how many sounds remain is said once a database is read");
            return;
        }
        SpecPtr s = spec();
        bool empty = false;
        patch->print("orchestra " + name + ": " + spaceText(*s, empty));
        if (empty) {
            patch->print("orchestra " + name + ": no sound is left to any player: every segment orchidea orchestrates will be a rest");
            return;
        }
        auto silent = spaceCounts(*s).silent(instrumentsOf(*s));
        if (!silent.empty()) {
            std::string t;
            for (auto& i : silent) t += " " + i;
            patch->print("orchestra " + name + ": no sound is left for" + t + ": " + (silent.size() == 1 ? "its players are" : "their players are") + " silent");
        }
    }
    void addPlayers(const Message& m, size_t from) {
        for (size_t k = from; k < m.size(); ++k) {
            if (m[k].isFloat()) {                      // a count: the instrument before it, that many times in all
                if (explicit_.empty()) continue;
                int n = clampInt(m[k].f, 0, 256);
                std::string last = explicit_.back();
                explicit_.pop_back();
                for (int j = 0; j < n; ++j) explicit_.push_back(last);
                continue;
            }
            explicit_.push_back(m[k].str());
        }
        if (explicit_.size() > 1024) explicit_.resize(1024);
    }
    std::vector<std::string> players() const {
        if (!explicit_.empty()) return explicit_;
        std::vector<std::string> out;
        if (!db_) return out;
        std::set<std::string> here;
        for (size_t k = 0; k < db_->entries.size(); ++k)
            if (db_->present[k] && db_->entries[k].midi >= 0) here.insert(db_->entries[k].instr);
        for (const char* i : kDefaultOrchestra)
            if (here.count(i)) out.push_back(i);
        return out;
    }
    Message playerList() const {
        Message m;
        for (auto& p : players()) m.push_back(Atom(p));
        if (m.empty()) m.push_back(Atom("bang"));
        return m;
    }
    // a file, or a dataset's name (no folder in it, no .db at its end, not a file next to the patch): found as Musil's
    // (dataset name) finds it, on the job's thread
    void read(const std::string& file) {
        if (busy()) return;
        std::string full = patch->resolvePath(file);
        std::error_code ec;
        bool byName = musil::datasets::is_name(file) && !std::filesystem::is_regular_file(full, ec);
        musil::datasets::Place place = datasetsPlace();
        auto out = std::make_shared<std::shared_ptr<OrchDb>>();
        auto err = std::make_shared<std::string>();
        run(
            [full, byName, file, place, out, err](Job& job) {
                try { *out = readDatabase(byName ? musil::datasets::resolve(place, file) : full, job, *err); }
                catch (const std::exception& e) { *out = nullptr; *err = e.what(); }
            },
            [this, out, err, file] { loaded(*out, *err, file); });
    }
    // analyse folder [type block hop ncoeff]: the feature file made next to the folder (a background job), then read
    void analyse(const Message& m) {
        if (busy()) return;
        if (m.size() < 2) { patch->print("orchestra " + name + ": analyse folder [type block hop ncoeff]"); return; }
        std::string folder = patch->resolvePath(m[1].str());
        musil::db_gen_params p;
        if (m.size() > 2) p.type = m[2].str();
        if (m.size() > 3) p.block = m[3].isFloat() ? clampInt(m[3].f, -1, 1 << 21) : -1;
        if (m.size() > 4) p.hop = m[4].isFloat() ? clampInt(m[4].f, -1, 1 << 21) : -1;
        if (m.size() > 5) p.ncoeff = m[5].isFloat() ? clampInt(m[5].f, -1, 1 << 21) : -1;
        else if (p.type == "mfcc") p.ncoeff = 13;
        else if (p.type == "moments") p.ncoeff = 4;
        try { musil::db_gen_check(p); } catch (const std::exception& e) { patch->print("orchestra " + name + ": analyse: " + e.what()); return; }
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) { patch->print("orchestra " + name + ": analyse: not a folder: " + m[1].str()); return; }
        std::filesystem::path f = std::filesystem::path(folder).lexically_normal();
        if (f.filename().empty()) f = f.parent_path();
        std::string db = analysisPath(f.string(), f.filename().string());
        if (std::filesystem::exists(db, ec)) {
            patch->print("orchestra " + name + ": " + db + " is already there: read it, or delete it to analyse the folder again");
            return;
        }
        if (std::string why; !patchMayWrite(patch, db, why)) {       // a patch from elsewhere writes only where it may
            patch->print("orchestra " + name + ": analyse: " + why + " (the Datasets window, Analyse a folder..., analyses one anywhere)");
            return;
        }
        auto out = std::make_shared<std::shared_ptr<OrchDb>>();
        auto err = std::make_shared<std::string>();
        run(
            [folder, db, p, out, err](Job& job) {
                try {
                    size_t n = musil::db_generate(folder, db, p, [&job](size_t k, size_t total) {
                        job.progress(total ? .5f * (float)k / (float)total : 0.f);
                        return !job.cancelled();
                    });
                    if (n == 0) { std::error_code e; std::filesystem::remove(db, e); *err = "no WAV file under " + folder; return; }
                    *out = readDatabase(db, job, *err);
                } catch (const std::exception& e) { *out = nullptr; *err = e.what(); }
            },
            [this, out, err, db] { loaded(*out, *err, db); });
    }
    void loaded(std::shared_ptr<OrchDb> got, const std::string& error, const std::string& file) {
        if (!got) {
            patch->print("orchestra " + name + ": " + (error.empty() ? "no database in " + file : error));
            waiting_.clear();
            return;
        }
        db_ = got;
        if (!sel_.empty()) {
            recompileSelection();
            sayRemaining();
        }
        size_t missing = 0;
        for (char c : db_->present) missing += c ? 0 : 1;
        if (missing == db_->present.size() && missing > 0)
            patch->print("orchestra " + name + ": none of the " + std::to_string(missing) + " sounds of " + file + " is on disk (looked in " + db_->root + ")");
        outletFloat(2, (float)db_->entries.size());
        outlet(3, playerList());
        auto w = std::move(waiting_);
        waiting_.clear();
        for (auto& fn : w) fn();
    }
    void info() {
        if (!db_) { patch->print("orchestra " + name + ": no database (read one)"); return; }
        std::map<std::string, std::vector<size_t>> by;
        size_t onDisk = 0;
        for (size_t k = 0; k < db_->entries.size(); ++k) {
            by[db_->entries[k].instr].push_back(k);
            onDisk += db_->present[k] ? 1 : 0;
        }
        patch->print("orchestra " + name + ": " + std::to_string(db_->entries.size()) + " sounds (" + std::to_string(onDisk) + " on disk) in " +
                     db_->path + ", features " + db_->type + " " + std::to_string(db_->ncoeff));
        for (auto& kv : by) {
            int lo = 1000, hi = -1;
            std::set<std::string> techs;
            for (size_t k : kv.second) {
                const auto& e = db_->entries[k];
                if (e.midi >= 0) { lo = std::min(lo, e.midi); hi = std::max(hi, e.midi); }
                techs.insert(e.tech);
            }
            std::string s = "  " + kv.first + ": " + std::to_string(kv.second.size()) + " sounds";
            if (hi >= 0) s += ", " + musil::midi_to_pitch(lo) + " to " + musil::midi_to_pitch(hi);
            s += ";";
            for (auto& t : techs) s += " " + t;
            auto r = ranges_.find(kv.first);
            if (r != ranges_.end()) s += "; played " + musil::midi_to_pitch(r->second.first) + " to " + musil::midi_to_pitch(r->second.second);
            patch->print(s);
        }
        std::string p = "  players:";
        for (auto& x : players()) p += " " + x;
        if (explicit_.empty()) p += " (the default orchestra)";
        patch->print(p);
        bool empty = false;
        patch->print("  " + spaceText(*spec(), empty));
    }

    std::string file_;
    DbPtr db_;
    std::vector<std::string> explicit_;
    std::map<std::string, std::pair<int, int>> ranges_;
    std::vector<std::string> styles_, dynamics_;
    musil::dbq::selection sel_;
    std::vector<std::function<void()>> waiting_;
    CarryPtr carry_ = std::make_shared<CarryState>();
};

OrchestraBox* findOrchestra(Patch* p, const std::string& name) {
    OrchestraBox* found = nullptr;
    p->root()->forEachBox([&](Box* b) {
        if (auto o = dynamic_cast<OrchestraBox*>(b))
            if (!found && o->name == name) found = o;
    });
    return found;
}

// a note of the solution, as orchestrate-mimetic and its connection make it (and the granulator: the dynamics asked
// for, which the sample may not have, its pitch and its gain)
struct OrchNote {
    double at = 0, dur = 0, az = 0, gain = 1;
    int entry = 0, player = 0, cents = 0;
    int midi = -1;                             // -1: the sample's
    std::string dyn;                           // "": the sample's
};

// what a run found, kept for choose, solutions, report and write
struct OrchResult {
    SpecPtr spec;
    std::vector<orch::segment> segments;       // the target's analysis (its notes unwidened: the cents)
    orch::search_result search;
    std::vector<int> choices;                  // the solution chosen per segment (the connection's, or choose's)
    bool seating = true;
    double hold = 1e9, sr = 44100;
    std::string settings;                      // what the search ran with (report says it)
    std::string space;                         // how many sounds the orchestra's filters left (report says it)
};

// the chosen solutions as notes (orchestrate-mimetic, then connection: sorted by onset, held notes merged)
std::vector<OrchNote> connectionOf(const OrchResult& r) {
    std::vector<OrchNote> all;
    for (size_t k = 0; k < r.search.segments.size() && k < r.segments.size(); ++k) {
        const auto& s = r.search.segments[k];
        const auto& g = r.segments[k];
        if (s.solutions.empty()) continue;
        const auto& sol = s.solutions[(size_t)std::clamp(r.choices[k], 0, (int)s.solutions.size() - 1)].second;
        for (size_t j = 0; j < sol.size() && j < s.players.size(); ++j) {
            if (sol[j] < 0) continue;
            const orch::entry& e = r.spec->db->entries[(size_t)sol[j]];
            OrchNote n;
            n.at = g.start;
            n.dur = g.length;
            n.entry = sol[j];
            n.player = s.players[j];
            auto c = g.notes.find(e.pitch);
            n.cents = c == g.notes.end() ? 0 : c->second;
            n.az = r.seating ? orch::seat_of(e.instr) : 0;
            all.push_back(n);
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const OrchNote& a, const OrchNote& b) { return a.at < b.at; });
    std::vector<musil::span> spans;
    for (auto& n : all) {
        const orch::entry& e = r.spec->db->entries[(size_t)n.entry];
        spans.push_back({n.at, n.dur, e.instr + "|" + std::to_string(e.midi) + "|" + std::to_string(n.player)});
    }
    std::vector<OrchNote> out;
    for (auto& p : musil::continuation_plan(spans, r.hold)) {
        OrchNote n = all[p.first];
        n.dur = p.second;
        out.push_back(n);
    }
    return out;
}

// what a granular or morphological run found: a realisation per solution (granular.h's run_result: its notes, its
// report, what it said), the one chosen
struct GranResult {
    SpecPtr spec;
    std::string mode;                          // granular, morphological
    std::vector<gr::run_result> runs;
    int choice = 0;
    double start = 0, sr = 44100, seconds = 0;
    bool seating = true;
    std::string settings;
};
// the chosen realisation's notes as orchestrate-granular's connection makes them (music.mu's connection: in time order,
// a note continuing one of the same player, instrument and pitch merged into it), placed at the run's start
std::vector<OrchNote> granularNotes(const GranResult& r) {
    std::vector<OrchNote> out;
    if (r.runs.empty()) return out;
    const auto& run = r.runs[(size_t)std::clamp(r.choice, 0, (int)r.runs.size() - 1)];
    for (auto& p : gr::connection_plan(run.notes, 1e9)) {
        const gr::note& n = run.notes[p.first];
        OrchNote o;
        o.at = r.start + n.at;
        o.dur = std::max(0.0, p.second);
        o.entry = n.entry;
        o.player = n.player;
        o.cents = (int)std::lround(n.cents);
        o.midi = n.midi;
        o.dyn = n.dyn;
        o.gain = n.gain;
        o.az = r.seating ? orch::seat_of(n.instr) : 0;
        out.push_back(o);
    }
    return out;
}

// a granular or morphological parameter given by a message (granular.h's pval): a number, two numbers (a range), names
// (a set: styles ord pizz; one name for method, duration-law, decision, fundamental), chords separated by / (chords C4
// E4 G4 / D4 F4 A4), a balance (balance brass -6 Hn -3), a density per group separated by / (group-density 1 2 / 8),
// or an envelope of any of them: at time value at time value ... (density at 0 4 6 at 10 0.5 1)
const std::set<std::string>& granularKeys() {
    static const std::set<std::string> k = {"density", "register", "duration", "duration-law", "styles", "dynamics", "dynamics-strict",
        "chords", "chord-weight", "method", "interval", "fundamental", "harmonicity", "just", "weight", "tilt", "leap", "inertia",
        "seat", "spread", "polyphony", "coupling", "group-density", "balance", "steal", "voices", "min-gain", "floor", "persist",
        "min-length", "max-length", "short", "width", "range", "density-scale", "min-density", "step", "decision"};
    return k;
}
// a message's number as the decimal it was written as (0.1, not the float nearest it): the shortest decimal that gives
// the float back, read as a double, so that the parameters are Musil's own (0.1 there is the double 0.1)
double decimalOf(float f) {
    if (!std::isfinite(f)) return f;
    char buf[40];
    for (int digits = 6; digits <= 9; ++digits) {
        std::snprintf(buf, sizeof buf, "%.*g", digits, (double)f);
        if ((float)std::strtod(buf, nullptr) == f) return std::strtod(buf, nullptr);
    }
    return f;
}
bool parseValue(const std::string& key, const Message& m, size_t a, size_t b, gr::pval& out, std::string& err) {
    if (a >= b) { err = key + ": a value"; return false; }
    auto atomVal = [](const Atom& x) { return x.isFloat() ? gr::pval::number(decimalOf(x.f)) : gr::pval::text(x.str()); };
    auto groups = [&](std::vector<std::pair<size_t, size_t>>& g) {
        size_t from = a;
        for (size_t k = a; k <= b; ++k)
            if (k == b || m[k].is("/")) { if (k > from) g.push_back({from, k}); from = k + 1; }
    };
    if (key == "chords") {
        std::vector<std::pair<size_t, size_t>> g; groups(g);
        out = gr::pval::of({});
        for (auto& r : g) { gr::pval c = gr::pval::of({}); for (size_t k = r.first; k < r.second; ++k) c.list.push_back(atomVal(m[k])); out.list.push_back(c); }
        if (out.list.empty()) { err = "chords: pitches, chords separated by /"; return false; }
        return true;
    }
    if (key == "group-density") {
        std::vector<std::pair<size_t, size_t>> g; groups(g);
        out = gr::pval::of({});
        for (auto& r : g) {
            gr::pval v;
            if (!parseValue("density", m, r.first, r.second, v, err)) return false;
            out.list.push_back(v);
        }
        return true;
    }
    if (key == "balance") {
        out = gr::pval::of({});
        for (size_t k = a; k + 1 < b; k += 2) {
            if (!m[k + 1].isFloat()) { err = "balance: names and decibels (balance brass -6 Hn -3)"; return false; }
            out.list.push_back(gr::pval::of({gr::pval::text(m[k].str()), gr::pval::number(decimalOf(m[k + 1].f))}));
        }
        if ((b - a) % 2) { err = "balance: names and decibels (balance brass -6 Hn -3)"; return false; }
        return true;
    }
    bool numbers = true;
    for (size_t k = a; k < b; ++k) numbers = numbers && m[k].isFloat() && std::isfinite(m[k].f);
    if (numbers) {
        if (b - a == 1) out = gr::pval::number(decimalOf(m[a].f));
        else { out = gr::pval::of({}); for (size_t k = a; k < b; ++k) out.list.push_back(gr::pval::number(decimalOf(m[k].f))); }
        return true;
    }
    if (key == "method" || key == "duration-law" || key == "decision" || key == "fundamental") { out = atomVal(m[a]); return true; }
    out = gr::pval::of({});
    for (size_t k = a; k < b; ++k) out.list.push_back(atomVal(m[k]));
    return true;
}
bool parseParam(const std::string& key, const Message& m, gr::pval& out, std::string& err) {
    if (m.size() > 1 && m[1].is("at")) {                 // an envelope: at t value at t value ...
        out = gr::pval::of({});
        size_t k = 1;
        while (k < m.size()) {
            if (!m[k].is("at") || k + 2 >= m.size() + 0 || !m[k + 1].isFloat()) { err = key + ": at time value at time value ..."; return false; }
            size_t e = k + 2;
            while (e < m.size() && !m[e].is("at")) ++e;
            gr::pval v;
            if (!parseValue(key, m, k + 2, e, v, err)) return false;
            out.list.push_back(gr::pval::of({gr::pval::number(decimalOf(m[k + 1].f)), v}));
            k = e;
        }
        return true;
    }
    return parseValue(key, m, 1, m.size(), out, err);
}

// the notes summed into stereo at sr, as music.mu's score-render places them (note-sound, fit-duration, pan-azimuth):
// each sound resampled to sr and by its cents, cut to its duration with a fade of 20 ms; => left, right
std::vector<std::vector<float>> mixOf(const std::vector<OrchNote>& notes, const OrchDb& db, double sr, Job& job, size_t& missing) {
    double end = 0;
    for (auto& n : notes) end = std::max(end, n.at + n.dur);
    size_t total = (size_t)std::max(1.0, std::floor(end * sr) + std::floor(0.6 * sr));
    std::vector<std::vector<double>> out(2, std::vector<double>(total, 0.0));
    std::map<std::string, std::shared_ptr<SampleBuffer>> cache;
    missing = 0;
    for (size_t k = 0; k < notes.size(); ++k) {
        if (job.cancelled()) return {};
        job.progress(.9f + .1f * (float)k / (float)notes.size());
        const OrchNote& n = notes[k];
        const std::string& path = db.paths[(size_t)n.entry];
        auto& w = cache[path];
        if (!w) {
            w = std::make_shared<SampleBuffer>();
            std::string err;
            if (!db.present[(size_t)n.entry] || !decodeAudioFile(path, *w, err)) w->frames = 0;
        }
        if (w->frames <= 0 || w->sr <= 0) { ++missing; continue; }
        int moved = n.midi >= 0 && db.entries[(size_t)n.entry].midi >= 0 ? n.midi - db.entries[(size_t)n.entry].midi : 0;
        double shift = moved + n.cents / 100.0, need = 0.2 + n.dur, ratio = std::pow(2.0, -shift / 12.0);
        size_t srcLen = (size_t)std::min<double>((double)w->frames, std::ceil(std::ceil(need) * w->sr / ratio));
        std::vector<musil::varr> chans;
        for (int c = 0; c < w->channels; ++c) {
            musil::varr x(srcLen);
            for (size_t f = 0; f < srcLen; ++f) x[f] = w->at((long)f, c);
            if (w->sr != sr) x = musil::resample_sinc(x, sr / w->sr);
            if (shift != 0) x = musil::resample_sinc(x, ratio);
            size_t len = (size_t)std::floor(n.dur * sr);                   // fit-duration
            if (x.size() > len) {
                size_t fade = std::max<size_t>(1, std::min<size_t>((size_t)std::floor(0.02 * sr), len / 4));
                musil::varr y(len);
                for (size_t f = 0; f < len; ++f) y[f] = x[f];
                for (size_t j = 0; j < fade && j < len; ++j) y[len - 1 - j] *= (double)j / (double)fade;
                x = y;
            }
            chans.push_back(std::move(x));
        }
        std::vector<musil::varr> placed;
        if (chans.size() == 2) placed = chans;
        else {
            musil::varr mono = chans[0];
            for (size_t c = 1; c < chans.size(); ++c) mono += chans[c];
            if (chans.size() > 1) mono /= (double)chans.size();
            double az = n.az > 90 ? 180 - n.az : n.az < -90 ? -180 - n.az : n.az;   // pan-azimuth: behind folded onto the front
            double p = std::clamp(-std::clamp(az, -90.0, 90.0) / 90.0, -1.0, 1.0);
            placed.push_back(mono * std::sqrt(0.5 * (1 - p)));
            placed.push_back(mono * std::sqrt(0.5 * (1 + p)));
        }
        if (n.gain != 1) for (auto& c : placed) c *= n.gain;
        size_t start = (size_t)std::floor(n.at * sr);
        for (size_t c = 0; c < 2; ++c)
            for (size_t f = 0; f < placed[c].size() && start + f < total; ++f) out[c][start + f] += placed[c][f];
    }
    std::vector<std::vector<float>> res(2, std::vector<float>(total));
    for (size_t c = 0; c < 2; ++c)
        for (size_t f = 0; f < total; ++f) res[c][f] = (float)out[c][f];
    return res;
}

// ---------------------------------------------------------------- orchidea
// orchidea orchestra: a target sound orchestrated, after Orchidea, by the players of an orchestra object: the target
// is cut into segments (one for a static target), each analysed into the database's features and the pitches of its
// partials, and a genetic search finds for each the combination of sounds (one per player, or a rest) whose summed
// spectrum is nearest the target's. A background job; Musil's orchestrate-mimetic with the same seed finds the same.
//   run target [out]             orchestrates the buffer~ target; the sum of the chosen samples (stereo, the
//                                players seated as in Orchidea's mixes) goes to the buffer~ out
//   population n (300), epochs n (300), sparsity x (0.001), seed n (1), pursuit k (0), crossover x (0.8),
//   mutation x (0.01), positive x (0.5), negative x (10), regularization x (0), patience n (150)
//   segmentation none|flux|adaptive|frames (none: static), onsets ms..., threshold x (2), ratio x (2: adaptive's flux
//   peaks above ratio times their local median), timegate ms (100)
//   partials x (0.2; 0: no pitch filter), window n (32768), pitches C4 ... (always admitted), octaves 0 -1 ...
//   connection closest|best|path, solutions n (10), hysteresis x, dovetail x, movement x, sustain 1/0, hold ms
//   seating 1/0, choose segment n, notes, score, show segment, report, write file, roll buffer (a frame a note: onset
//   s, pitch with cents, player, duration s: plot scatter draws it), stop
// mode mimetic|granular|morphological: what run does (mimetic, the search above, by default)
//   granular: run seconds [out]  the orchestra as a granulator (Musil's orchestrate-granular): a stochastic process
//                                over seconds whose parameters may change over time, each event a note a player can
//                                play as recorded; the parameters are messages, each a number, a range (two
//                                numbers), a set of names, or an envelope (at t value at t value ...) or a buffer~'s
//                                curve (density buffer name: its frames over the run, two channels a range):
//                                density, register (octaves), duration, duration-law uniform|log|x, styles, dynamics
//                                (labels, or a level 0..1), dynamics-strict, chords (C4 E4 / D4 F4), chord-weight,
//                                method random|cluster|pivots|chordinterp|markov|harmonic, interval, fundamental,
//                                harmonicity, just, markov C4 D4 E4 ... (the line it learns), weight, tilt, leap,
//                                inertia, seat, spread, polyphony, coupling, group-density, balance brass -6 ...;
//                                continue 1 (the orchestra's players carry their bookings from run to run, one run at
//                                a time: the sections of a piece; its rest message clears them), start s (where this
//                                run sits in the piece: its notes are placed there)
//   morphological: run target [out]   the granulator driven by a target sound (orchestrate-morphological): its density,
//                                register and dynamics read off the buffer~ target (hop n, 1024; attack-threshold x, 2;
//                                fine n, 8192), at every event the free players' sounds at the target's peaks pursued
//                                against its spectrum less what sounds: voices, min-gain, floor, persist, min-length,
//                                max-length, short, width, range, density-scale, min-density, step, styles, decision
//                                pursuit|genetic (population, epochs, sparsity... then Orchidea's search at every event),
//                                and any granular parameter in place of the target's
//   in these modes solutions n (1) makes n realisations, choose 1 n takes one, seed n (1) the draws
// Outlets: progress, done, the notes (k instrument pitch dynamics technique onset duration path cents player; ms),
// the distance (the mean cost of the chosen solutions; for a granular or morphological run its saturation: the
// fraction of the events due skipped because every able player was busy), and the solution as a score (score <token>,
// for a score box: a row an instrument in orchestral order, each note with its dynamics, technique, cents, gain,
// place and its sample as its sound, so the score box plays it and its roll edits it), after each run and choose, and
// for score.
class OrchideaBox : public OfflineBox {
    std::set<std::string> given_;                         // the settings a message chose

public:
    explicit OrchideaBox(const Message& a) : orchestra_(argstr(a, 0)) {
        setPorts({P::Control}, {P::Control, P::Control, P::Control, P::Control, P::Control});
    }
    ~OrchideaBox() override {
        *alive_ = false;
        job_.cancel();
        releaseCarry();
    }

    std::vector<std::string> menuItems() override { return {"Report", "Save the solution..."}; }
    void menuAction(int k) override {
        if (k == 0) { message(0, {Atom("report")}); return; }
        std::string f = chooseFile(true, "Save the orchestration", "Orchidea connection\t*.txt", "orchestration.txt");
        if (!f.empty() && ready()) write(f, true);                  // the user chose it: written there
    }

    void message(int, const Message& m) override {
        const Atom& h = m[0];
        auto num = [&](float def) { float v = argf(m, 1, def); return std::isfinite(v) ? v : def; };   // NaN, inf: the default
        auto& p = opts_.p;
        if (h.is("stop")) { job_.cancel(); releaseCarry(); return; }
        if (h.is("run") && m.size() > 1) {
            if (mode_ == "mimetic") start(m[1].str(), argstr(m, 2));
            else startGranular(m[1], argstr(m, 2));
            return;
        }
        given_.insert(h.str());                           // set by a message: the dynamic defaults leave it alone
        if (h.is("population")) p.pop_size = clampInt(num(300), 2, 20000);
        else if (h.is("epochs")) p.max_epochs = clampInt(num(300), 1, 100000);
        else if (h.is("sparsity")) p.sparsity = std::clamp((double)num(.001f), 0.0, 1.0);
        else if (h.is("seed")) {
            seed64_ = (uint64_t)(int64_t)std::clamp((double)num(1), -9e18, 9e18);   // as Musil's seed_of: (seed 7) there is seed 7 here
            p.seed = (unsigned)seed64_;
        }
        else if (h.is("pursuit")) p.pursuit = clampInt(num(0), 0, 100000);
        else if (h.is("crossover")) p.xover_rate = std::clamp((double)num(.8f), 1e-6, 1.0);
        else if (h.is("mutation")) p.mutation_rate = std::clamp((double)num(.01f), 1e-6, 1.0);
        else if (h.is("positive")) p.positive = std::clamp((double)num(.5f), 0.0, 1e6);
        else if (h.is("negative")) p.negative = std::clamp((double)num(10), 0.0, 1e6);
        else if (h.is("regularization")) p.regularization = std::clamp((double)num(0), 0.0, 1e6);
        else if (h.is("patience")) p.patience = clampInt(num(150), 1, 100000);
        else if (h.is("solutions")) p.max_solutions = clampInt(num(10), 1, 1000);
        else if (h.is("hysteresis")) p.hysteresis = std::clamp((double)num(0), 0.0, 1e6);
        else if (h.is("dovetail")) p.dovetail = std::clamp((double)num(0), 0.0, 1e6);
        else if (h.is("movement")) p.movement = std::clamp((double)num(1), 0.0, 1e6);
        else if (h.is("sustain")) opts_.sustain = num(1) != 0;
        else if (h.is("hold")) opts_.hold = m.size() > 1 ? std::clamp((double)num(0), 0.0, 1e9) / 1000.0 : 1e9;
        else if (h.is("connection")) {
            std::string c = argstr(m, 1, "closest");
            if (c == "closest" || c == "best" || c == "path") opts_.connection = c;
            else patch->print("orchidea: connection is closest, best or path");
        } else if (h.is("octaves")) {
            opts_.octaves.clear();
            for (size_t k = 1; k < m.size(); ++k) if (m[k].isFloat()) opts_.octaves.push_back(clampInt(std::round(m[k].f), -8, 8));
            if (opts_.octaves.empty()) opts_.octaves.push_back(0);
        } else if (h.is("segmentation")) {
            std::string s = argstr(m, 1, "none");
            if (s == "none" || s == "static" || s == "flux" || s == "frames" || s == "adaptive") an_.segmentation = s;
            else patch->print("orchidea: segmentation is none, flux, adaptive or frames (or onsets ms...)");
        } else if (h.is("onsets")) {
            an_.onsets.clear();
            for (size_t k = 1; k < m.size(); ++k) if (m[k].isFloat() && std::isfinite(m[k].f) && m[k].f >= 0) an_.onsets.push_back(m[k].f / 1000.0);
            an_.segmentation = "list";
        } else if (h.is("threshold")) an_.threshold = std::clamp((double)num(2), 0.0, 1e6);
        else if (h.is("ratio")) ratio_ = std::clamp(decimalOf(num(2)), 0.0, 1e6);
        else if (h.is("timegate")) an_.timegate = std::clamp((double)num(100), 0.0, 1e7) / 1000.0;
        else if (h.is("partials")) an_.partials = std::clamp((double)num(.2f), 0.0, 1.0);
        else if (h.is("window")) { int n = 64; while (n < clampInt(num(32768), 64, 1 << 20)) n <<= 1; an_.partials_window = n; }
        else if (h.is("pitches")) {
            an_.extra_pitches.clear();
            for (size_t k = 1; k < m.size(); ++k) {
                int mi = pitchOf(m[k]);
                if (mi >= 0) an_.extra_pitches.push_back(musil::midi_to_pitch(mi));
            }
        } else if (h.is("seating")) seating_ = num(1) != 0;
        else if (h.is("choose") && m.size() > 2) choose(clampInt(argf(m, 1, 1), -1, 1 << 20) - 1, clampInt(argf(m, 2, 1), -1, 1 << 20) - 1);
        else if (h.is("notes")) { if (ready()) outputNotes(); }
        else if (h.is("score")) { if (ready()) outputScore(); }
        else if (h.is("show") && granularLast_) { if (ready()) printRealisations(); }
        else if (h.is("show")) { if (ready()) printSolutions(std::clamp(clampInt(argf(m, 1, 1), 0, 1 << 20) - 1, 0, (int)result_->segments.size() - 1)); }
        else if (h.is("report")) { if (ready()) report(); }
        else if (h.is("write") && m.size() > 1) { if (ready()) write(patch->resolvePath(m[1].str()), false); }
        else if (h.is("roll") && m.size() > 1) { if (ready()) roll(m[1].str()); }
        else if (h.is("mode")) {
            std::string md = argstr(m, 1, "mimetic");
            if (md == "mimetic" || md == "granular" || md == "morphological") mode_ = md;
            else patch->print("orchidea: mode is mimetic, granular or morphological");
        } else if (h.is("start")) start_ = std::clamp(decimalOf(num(0)), 0.0, 1e7);
        else if (h.is("continue")) continue_ = num(1) != 0;
        else if (h.is("hop")) hop_ = clampInt(num(1024), 64, 1 << 16);
        else if (h.is("fine")) { int n = 64; while (n < clampInt(num(8192), 64, 1 << 18)) n <<= 1; fine_ = n; }
        else if (h.is("attack-threshold")) attackThreshold_ = std::clamp((double)num(2), 0.0, 1e6);
        else if (h.is("markov")) {
            std::vector<std::vector<double>> lines(1);
            for (size_t k = 1; k < m.size(); ++k) {
                if (m[k].is("/")) { lines.emplace_back(); continue; }
                int mi = pitchOf(m[k]);
                if (mi >= 0) lines.back().push_back(mi);
            }
            markov_ = gr::markov_learn(lines);
        } else if (granularKeys().count(h.s)) {
            curves_.erase(h.s);
            if (m.size() == 1) { gp_.m.erase(h.s); return; }        // the default again
            if (m.size() == 3 && m[1].is("buffer")) { curves_[h.s] = m[2].str(); return; }
            gr::pval v; std::string err;
            if (!parseParam(h.s, m, v, err)) { patch->print("orchidea: " + err); return; }
            gp_.set(h.s, v);
        } else patch->print("orchidea: unknown message " + h.str());
    }

    // A target cut into segments (flux, frames, onsets) is searched with Musil's dynamic settings
    // (musil/examples/dynamic_orchidea.mu), which keep lines continuous, unless a message set them: population and
    // epochs 500, sparsity 0.01, the partials' pitch filter at 0.3 (0 for a tiny database such as MicroSOL, whose few
    // pitches it would leave unplayable), 5 solutions a segment, sustain on with notes held 4 s at most, dovetail
    // 0.75 (pivot notes) and the path connection (the least melodic movement). A static target keeps Orchidea's.
    void settingsFor(size_t dbSize, orch::analysis_options& an, orch::search_options& so) const {
        if (an.segmentation == "none" || an.segmentation == "static") return;
        auto unset = [&](const char* k) { return given_.count(k) == 0; };
        if (unset("population")) so.p.pop_size = 500;
        if (unset("epochs")) so.p.max_epochs = 500;
        if (unset("sparsity")) so.p.sparsity = 0.01;
        if (unset("solutions")) so.p.max_solutions = 5;
        if (unset("dovetail")) so.p.dovetail = 0.75;
        if (unset("sustain")) so.sustain = true;
        if (unset("hold")) so.hold = 4.0;
        if (unset("connection")) so.connection = "path";
        if (unset("partials")) an.partials = dbSize < 100 ? 0.0 : 0.3;
    }

private:
    bool ready() {
        if (granularLast_ ? (bool)gres_ : (bool)result_) return true;
        patch->print("orchidea: nothing orchestrated yet (run target)");
        return false;
    }
    // the notes of the last run (mimetic, granular or morphological), the database they come from
    std::vector<OrchNote> notesNow() const { return granularLast_ ? granularNotes(*gres_) : connectionOf(*result_); }
    const OrchDb& dbNow() const { return granularLast_ ? *gres_->spec->db : *result_->spec->db; }
    double srNow() const { return granularLast_ ? gres_->sr : result_->sr; }
    static int midiOf(const OrchNote& n, const orch::entry& e) { return n.midi >= 0 ? n.midi : e.midi; }
    static const std::string& dynOf(const OrchNote& n, const orch::entry& e) { return n.dyn.empty() ? e.dyn : n.dyn; }
    // run target [out]: waits for the orchestra when its database is still being read
    void start(const std::string& target, const std::string& out) {
        if (busy()) return;
        OrchestraBox* o = findOrchestra(patch, orchestra_);
        if (!o) { patch->print("orchidea: no orchestra " + orchestra_); return; }
        if (o->loading()) {
            auto alive = alive_;
            o->whenReady([this, alive, target, out] { if (*alive) start(target, out); });
            return;
        }
        SpecPtr spec = o->spec();
        if (!spec) { patch->print("orchidea: orchestra " + orchestra_ + " has no database (read one)"); return; }
        if (spec->players.empty()) { patch->print("orchidea: orchestra " + orchestra_ + " has no player whose instrument the database has"); return; }
        BufferBox* b = findBuffer(patch, target);
        if (!b || b->buf.frames <= 0) { patch->print("orchidea: no sound in buffer~ " + target); return; }
        if (!out.empty() && !findBuffer(patch, out)) { patch->print("orchidea: no buffer~ " + out); return; }
        bool emptySpace = false;
        std::string space = spaceText(*spec, emptySpace);
        if (emptySpace) patch->print("orchidea: orchestra " + orchestra_ + " leaves no sound to any player (" + filtersText(*spec) + "): every segment will be a rest");
        auto x = std::make_shared<musil::varr>((size_t)b->buf.frames);
        {
            std::vector<float> mono = monoOf(b->buf);
            for (size_t f = 0; f < mono.size(); ++f) (*x)[f] = mono[f];
        }
        double sr = b->buf.sr > 0 ? b->buf.sr : 44100;
        auto res = std::make_shared<OrchResult>();
        res->spec = spec;
        res->seating = seating_;
        res->sr = sr;
        auto err = std::make_shared<std::string>();
        auto mix = std::make_shared<std::vector<std::vector<float>>>();
        auto missing = std::make_shared<size_t>(0);
        orch::analysis_options an = an_;
        orch::search_options so = opts_;
        settingsFor(spec->db->entries.size(), an, so);
        double ratio = ratio_;
        res->hold = so.hold;
        res->space = space;
        {
            char b[300];
            std::snprintf(b, sizeof b, "segmentation %s, population %d, epochs %d, sparsity %g, partials %g, solutions %d, sustain %d, "
                          "hold %s, dovetail %g, hysteresis %g, connection %s", an.segmentation.c_str(), so.p.pop_size, so.p.max_epochs,
                          so.p.sparsity, an.partials, so.p.max_solutions, so.sustain ? 1 : 0,
                          so.hold >= 1e8 ? "none" : (std::to_string((int)std::lround(so.hold * 1000)) + " ms").c_str(), so.p.dovetail,
                          so.p.hysteresis, so.connection.c_str());
            res->settings = b;
        }
        so.styles = spec->styles;
        so.dynamics = spec->dynamics;
        bool render = !out.empty();
        runJob(
            [x, sr, res, err, mix, missing, an, so, render, ratio](Job& job) {
                try {
                    const OrchDb& db = *res->spec->db;
                    orch::analysis_options a = an;
                    if (a.segmentation == "adaptive") {           // Musil's mimetic-target 'adaptive: the flux peaks against their median
                        std::vector<double> xs(std::begin(*x), std::end(*x));
                        a.onsets = musil::morph::adaptive_onsets(xs, sr, (size_t)std::max(1, db.block), (size_t)std::max(1, db.hop), ratio, a.timegate);
                        a.segmentation = "list";
                    }
                    std::vector<orch::entry> space;                     // the sounds the ranges and the selection allow
                    for (size_t k = 0; k < db.entries.size(); ++k) {
                        const auto& e = db.entries[k];
                        if (!rangeAllows(*res->spec, e) || !inSelection(*res->spec, k)) continue;
                        space.push_back(e);
                    }
                    res->segments = orch::analyse(*x, sr, db.type, db.block, db.hop, db.ncoeff, a, [&job] { return !job.cancelled(); });
                    if (job.cancelled()) return;
                    job.progress(.05f);
                    res->search = orch::search_segments(space, res->spec->players, res->segments, so,
                        [&job](size_t seg, size_t nsegs, int epoch, int epochs) {
                            job.progress(.05f + .85f * (float)((double)seg + (double)epoch / std::max(1, epochs)) / (float)std::max<size_t>(1, nsegs));
                            return !job.cancelled();
                        });
                    if (res->search.stopped || job.cancelled()) return;
                    res->choices = res->search.choices;
                    if (render) *mix = mixOf(connectionOf(*res), db, sr, job, *missing);
                } catch (const std::exception& e) {
                    *err = e.what();
                }
            },
            [this, res, err, mix, missing, out] {
                if (!err->empty()) { patch->print("orchidea: " + *err); return false; }
                result_ = res;
                granularLast_ = false;
                out_ = out;
                sayRests();
                if (!out.empty() && !mix->empty()) storeMix(*mix, *missing);
                outputNotes();
                outputScore();
                return true;
            });
    }
    // choose segment n: solution n of a segment in place of the one chosen; the notes again, the mix again
    void choose(int seg, int n) {
        if (!ready() || busy()) return;
        if (granularLast_) { chooseGranular(seg, n); return; }
        if (seg < 0 || seg >= (int)result_->search.segments.size()) { patch->print("orchidea: no segment " + std::to_string(seg + 1)); return; }
        int ns = (int)result_->search.segments[(size_t)seg].solutions.size();
        if (n < 0 || n >= ns) { patch->print("orchidea: segment " + std::to_string(seg + 1) + " has " + std::to_string(ns) + " solutions"); return; }
        auto res = std::make_shared<OrchResult>(*result_);
        res->choices[(size_t)seg] = n;
        if (out_.empty() || !findBuffer(patch, out_)) {
            result_ = res;
            outputNotes();
            outputScore();
            outletBang(1);
            return;
        }
        auto mix = std::make_shared<std::vector<std::vector<float>>>();
        auto missing = std::make_shared<size_t>(0);
        runJob([res, mix, missing](Job& job) { *mix = mixOf(connectionOf(*res), *res->spec->db, res->sr, job, *missing); },
               [this, res, mix, missing] {
                   result_ = res;
                   if (!mix->empty()) storeMix(*mix, *missing);
                   outputNotes();
                   outputScore();
                   return true;
               });
    }
    // as OfflineBox::run, but the done outlet bangs only when done() says the work succeeded (an orchestration that
    // failed with a message is not followed by a bang that asks for its notes)
    void runJob(std::function<void(Job&)> work, std::function<bool()> done) {
        outletFloat(0, 0.f);
        job_.start(std::move(work), [this, done = std::move(done)] {
            bool ok = done();
            outletFloat(0, 1.f);
            if (ok) outletBang(1);
        });
    }
    void storeMix(std::vector<std::vector<float>>& mix, size_t missing) {
        float peak = 0;
        for (auto& c : mix) for (float v : c) peak = std::max(peak, std::fabs(v));
        if (peak > .98f) {                                  // as render-buffer: scaled down rather than clipped
            for (auto& c : mix) for (auto& v : c) v *= .98f / peak;
            char msg[128];
            std::snprintf(msg, sizeof msg, "orchidea: the mix peaked at %.2f and was scaled to 0.98", peak);
            patch->print(msg);
        }
        if (missing > 0) patch->print("orchidea: " + std::to_string(missing) + " notes left out of the mix (their sounds are not on disk)");
        storeBuffer(patch, out_, mix, srNow());
    }
    // the notes into a buffer~, a frame a note: onset (s), pitch (MIDI, with its cents), player, duration (s): plot in
    // scatter mode draws it as a roll, the players in colours
    void roll(const std::string& name) {
        auto notes = notesNow();
        std::vector<std::vector<float>> ch(4);
        for (auto& n : notes) {
            const orch::entry& e = dbNow().entries[(size_t)n.entry];
            ch[0].push_back((float)n.at);
            ch[1].push_back(midiOf(n, e) >= 0 ? (float)(midiOf(n, e) + n.cents / 100.0) : 0.f);
            ch[2].push_back((float)n.player);
            ch[3].push_back((float)n.dur);
        }
        storeBuffer(patch, name, ch, 1.0);
    }
    void outputNotes() {
        auto notes = notesNow();
        const OrchDb& db = dbNow();
        if (granularLast_) {
            const auto& r = gres_->runs[(size_t)std::clamp(gres_->choice, 0, (int)gres_->runs.size() - 1)].rep;
            outletFloat(3, (float)r.saturation);
        } else {
            double cost = 0;
            size_t ns = result_->search.segments.size();
            for (size_t k = 0; k < ns; ++k) {
                const auto& s = result_->search.segments[k];
                if (!s.solutions.empty()) cost += s.solutions[(size_t)std::clamp(result_->choices[k], 0, (int)s.solutions.size() - 1)].first;
            }
            outletFloat(3, ns ? (float)(cost / (double)ns) : 0.f);
        }
        for (size_t k = 0; k < notes.size(); ++k) {
            const auto& n = notes[k];
            const orch::entry& e = db.entries[(size_t)n.entry];
            std::string pitch = midiOf(n, e) >= 0 ? musil::midi_to_pitch(midiOf(n, e)) : e.pitch;
            outlet(2, {Atom((float)(k + 1)), Atom(e.instr), Atom(pitch), Atom(dynOf(n, e)), Atom(e.tech), Atom((float)(n.at * 1000.0)),
                       Atom((float)(n.dur * 1000.0)), Atom(db.paths[(size_t)n.entry]), Atom((float)n.cents), Atom((float)n.player)});
        }
    }
public:
    // the solution as a score (Musil's C++ score, score.h): a row an instrument (orchestral order, a clef from its
    // pitches), a note a sound chosen, with its dynamics, technique, cents and place (the seating), and its sample as
    // its sound (data sound, sound-pitch: the score box plays it, transposed by as much as the note is moved), its
    // player (data player)
    std::shared_ptr<musil::score::Score> solutionScore() const {
        namespace sc = musil::score;
        bool have = granularLast_ ? (bool)gres_ : (bool)result_;
        auto s = std::make_shared<sc::Score>("orchidea", have ? srNow() : 44100);
        if (!have) return s;
        const OrchDb& db = dbNow();
        auto notes = notesNow();
        std::map<std::string, std::vector<double>> pitches;
        for (auto& n : notes) {
            const orch::entry& e = db.entries[(size_t)n.entry];
            auto ev = std::make_shared<sc::Event>();
            ev->kind = sc::Kind::Note;
            ev->at = n.at;
            ev->dur = n.dur;
            ev->row = e.instr;
            ev->pitch = midiOf(n, e) >= 0 ? midiOf(n, e) : 60;
            ev->cents = n.cents;
            ev->dyn = dynOf(n, e);
            ev->tech = e.tech;
            ev->az = n.az;
            ev->gain = n.gain;
            sc::setField(ev->extra, "sound", sc::Datum::string(db.paths[(size_t)n.entry]));
            sc::setField(ev->extra, "sound-pitch", sc::Datum::number(e.midi >= 0 ? e.midi : ev->pitch));
            sc::setField(ev->extra, "player", sc::Datum::number(n.player));
            s->insert(ev);
            pitches[e.instr].push_back(ev->pitch);
        }
        std::vector<std::string> rows;
        for (auto& kv : pitches) rows.push_back(kv.first);
        std::stable_sort(rows.begin(), rows.end(), [](const std::string& a, const std::string& b) {
            return sc::layout::instrumentRank(a) < sc::layout::instrumentRank(b);
        });
        for (auto& r : rows) s->rows.push_back({r, sc::layout::clefFor(pitches[r]), {}});
        return s;
    }

private:
    void outputScore() { outlet(4, {Atom("score"), Atom(shareScore(solutionScore()))}); }
    std::string soundsOf(const std::vector<int>& sol) const {
        std::string s;
        for (int j : sol) {
            if (j < 0) continue;
            const orch::entry& e = result_->spec->db->entries[(size_t)j];
            s += (s.empty() ? "" : ", ") + e.instr + " " + e.pitch + " " + e.dyn + " " + e.tech;
        }
        return s.empty() ? "(silence)" : s;
    }
    void printSolutions(int seg) {
        const auto& g = result_->segments[(size_t)seg];
        const auto& s = result_->search.segments[(size_t)seg];
        char head[160];
        std::snprintf(head, sizeof head, "orchidea: segment %d at %.2f s, %.2f s: %d solutions", seg + 1, g.start, g.length, (int)s.solutions.size());
        patch->print(head);
        for (size_t k = 0; k < s.solutions.size(); ++k) {
            char c[64];
            std::snprintf(c, sizeof c, "  %d%s (cost %.3f): ", (int)k + 1, (int)k == result_->choices[(size_t)seg] ? "*" : "", s.solutions[k].first);
            patch->print(c + soundsOf(s.solutions[k].second));
        }
    }
    // per segment, the notes the connection chosen now holds on from the segment before (Musil's held_counts: the
    // notes connectionOf merges)
    std::vector<int> heldNow() const {
        const auto& entries = result_->spec->db->entries;
        return orch::held_counts(result_->segments, result_->search, result_->choices, [&](int i) -> const orch::entry* {
            return i >= 0 && (size_t)i < entries.size() ? &entries[(size_t)i] : nullptr;
        }, result_->hold);
    }
    // the segments that are rests (no sound passed their filters), said after a run
    void sayRests() {
        const auto& segs = result_->search.segments;
        int n = 0;
        for (size_t k = 0; k < segs.size(); ++k) {
            if (segs[k].rest.empty()) continue;
            if (++n <= 3) patch->print("orchidea: segment " + std::to_string(k + 1) + " is a rest: " + segs[k].rest);
        }
        if (n > 3) patch->print("orchidea: " + std::to_string(n - 3) + " more segments are rests (report says which)");
    }
    void report() {
        if (granularLast_) { reportGranular(); return; }
        const auto& r = result_->search;
        char head[200];
        std::snprintf(head, sizeof head, "orchidea: %d %s; the search evaluated %zu combinations, %zu taken from the cache", (int)r.segments.size(),
                      r.segments.size() == 1 ? "segment" : "segments", r.cache_misses, r.cache_hits);
        patch->print(head);
        patch->print("orchidea: " + result_->settings);
        if (!result_->space.empty()) patch->print("orchidea: the space: " + result_->space);
        std::vector<int> held = heldNow();                // what the connection chosen now holds (choose changes it)
        for (size_t k = 0; k < r.segments.size() && k < 40; ++k) {
            const auto& s = r.segments[k];
            const auto& g = result_->segments[k];
            char line[200];
            if (!s.rest.empty()) {
                std::snprintf(line, sizeof line, "  %d: at %.2f s, %.2f s; a rest: ", (int)k + 1, g.start, g.length);
                patch->print(line + s.rest);
                continue;
            }
            if (s.solutions.empty()) continue;
            const auto& sol = s.solutions[(size_t)std::clamp(result_->choices[k], 0, (int)s.solutions.size() - 1)];
            int used = 0;
            for (int j : sol.second) used += j >= 0 ? 1 : 0;
            std::snprintf(line, sizeof line, "  %d: at %.2f s, %.2f s; cost %.1f; %d players, %d holding on; %zu sounds in the space", (int)k + 1, g.start,
                          g.length, sol.first, used, k < held.size() ? held[k] : 0, s.space);
            std::string t = line;
            if (!s.missing.empty()) {
                t += "; not in the orchestra:";
                for (auto& q : s.missing) t += " " + q;
            }
            patch->print(t);
        }
        if (r.segments.size() > 40) patch->print("  ... " + std::to_string(r.segments.size() - 40) + " more");
    }
    // the solution as an Orchidea connection (music.mu's score-save writes the same; score-load reads it): a temporary
    // file next to the destination, checked, then renamed. From a message, only in the patch's folder or
    // ~/Documents/Moritz (patchMayWrite: a patch from elsewhere writes nowhere else); a file the user chose anywhere
    void write(const std::string& path, bool chosen) {
        std::string why;
        if (!chosen && !patchMayWrite(patch, path, why)) { patch->print("orchidea: " + why); return; }
        auto notes = notesNow();
        const OrchDb& db = dbNow();
        std::string text = "[ orchestra";
        std::vector<std::string> seen;
        for (auto& n : notes) {
            const std::string& i = db.entries[(size_t)n.entry].instr;
            if (std::find(seen.begin(), seen.end(), i) == seen.end()) { seen.push_back(i); text += " " + i; }
        }
        text += " ]\n";
        char buf[64];
        for (size_t k = 0; k < notes.size();) {
            double at = notes[k].at;
            std::snprintf(buf, sizeof buf, "%.2f", 1000 * at);
            text += std::string("[ segment ") + buf + "\n\t[ solution 1\n";
            for (; k < notes.size() && std::fabs(notes[k].at - at) < 0.0005; ++k) {
                const auto& n = notes[k];
                const orch::entry& e = db.entries[(size_t)n.entry];
                std::snprintf(buf, sizeof buf, "%.2f", 1000 * n.dur);
                std::string pitch = midiOf(n, e) >= 0 ? musil::midi_to_pitch(midiOf(n, e)) : e.pitch;
                text += std::string("\t\t[ note ") + buf + " " + e.instr + " " + e.tech + " " + pitch + " " + dynOf(n, e) + " " + e.other + " " +
                        db.files[(size_t)n.entry] + " " + std::to_string(n.cents) + " ]\n";
            }
            text += "\t]\n]\n";
        }
        std::string err;
        if (!fileio::writeAtomic(path, text, &err)) patch->print("orchidea: " + err);
    }

    // ---- the granular and morphological modes
    // run seconds [out] (granular) or run target [out] (morphological): Musil's granulator on a job, its realisations
    // one after another with one generator (as orchestrate-granular makes its 'solutions)
    void startGranular(const Atom& what, const std::string& out) {
        if (busy()) return;
        OrchestraBox* o = findOrchestra(patch, orchestra_);
        if (!o) { patch->print("orchidea: no orchestra " + orchestra_); return; }
        if (o->loading()) {
            auto alive = alive_;
            Atom w = what;
            o->whenReady([this, alive, w, out] { if (*alive) startGranular(w, out); });
            return;
        }
        SpecPtr spec = o->spec();
        if (!spec) { patch->print("orchidea: orchestra " + orchestra_ + " has no database (read one)"); return; }
        if (spec->players.empty()) { patch->print("orchidea: orchestra " + orchestra_ + " has no player whose instrument the database has"); return; }
        {
            bool emptySpace = false;
            spaceText(*spec, emptySpace);
            if (emptySpace) patch->print("orchidea: orchestra " + orchestra_ + " leaves no sound to any player (" + filtersText(*spec) + "): nothing can be played");
        }
        bool morph = mode_ == "morphological";
        double secs = 0, sr = 44100;
        auto x = std::make_shared<std::vector<double>>();
        if (morph) {
            BufferBox* b = findBuffer(patch, what.str());
            if (!b || b->buf.frames <= 0) { patch->print("orchidea: no sound in buffer~ " + what.str()); return; }
            std::vector<float> mono = monoOf(b->buf);
            x->assign(mono.begin(), mono.end());
            sr = b->buf.sr > 0 ? b->buf.sr : 44100;
            secs = (double)x->size() / sr;
        } else {
            if (!what.isFloat() || !(what.f > 0) || what.f > 3600) { patch->print("orchidea: run seconds (granular: up to 3600)"); return; }
            secs = decimalOf(what.f);
        }
        if (!out.empty() && !findBuffer(patch, out)) { patch->print("orchidea: no buffer~ " + out); return; }
        gr::params p = gp_;
        for (auto& c : curves_) {                     // a buffer~'s curve over the run: at most 1000 breakpoints
            BufferBox* b = findBuffer(patch, c.second);
            if (!b || b->buf.frames <= 0) { patch->print("orchidea: " + c.first + ": no curve in buffer~ " + c.second); return; }
            long n = b->buf.frames, step = std::max(1L, (n + 999) / 1000);
            gr::pval env = gr::pval::of({});
            for (long f = 0; f < n; f += step) {
                double t = n > 1 ? secs * (double)f / (double)(n - 1) : 0;
                gr::pval v = b->buf.channels > 1 ? gr::pval::range(b->buf.at(f, 0), b->buf.at(f, 1)) : gr::pval::number(b->buf.at(f, 0));
                env.list.push_back(gr::pval::of({gr::pval::number(t), v}));
            }
            if (env.list.size() == 1) p.set(c.first, env.list[0].list[1]);
            else p.set(c.first, env);
        }
        if (continue_) { p.set("continue", gr::pval::number(1)); p.set("start", gr::pval::number(start_)); }
        if (morph && p.opt("decision", gr::pval::none()).str() == "genetic") {
            gr::pval g = gr::pval::of({});
            const auto& q = opts_.p;
            auto add = [&](const char* k, double v) { if (given_.count(k)) g.list.push_back(gr::pval::of({gr::pval::text(k), gr::pval::number(v)})); };
            add("population", q.pop_size); add("epochs", q.max_epochs); add("sparsity", q.sparsity); add("mutation", q.mutation_rate);
            add("positive", q.positive); add("negative", q.negative); add("pursuit", q.pursuit); add("patience", q.patience);
            p.set("genetic", g);
        }
        // the players: this run's own, or the orchestra's carried on (one run books them at a time)
        CarryPtr cs = o->carry();
        std::string key;
        for (size_t k = 0; k < spec->players.size(); ++k) key += spec->players[k] + "/" + std::to_string(spec->groups[k]) + " ";
        auto players = std::make_shared<std::vector<gr::player>>();
        if (continue_) {
            if (cs->inFlight) { patch->print("orchidea: another run is booking orchestra " + orchestra_ + "'s players (continue): run this one when it is done"); return; }
            if (cs->key == key) *players = cs->players;
        }
        if (players->empty())
            for (size_t k = 0; k < spec->players.size(); ++k) {
                gr::player q;
                q.instrs = gr::ossia(spec->players[k]);
                q.group = spec->groups[k];
                players->push_back(q);
            }
        int n = given_.count("solutions") ? std::max(1, opts_.p.max_solutions) : 1;
        auto res = std::make_shared<GranResult>();
        res->spec = spec;
        res->mode = mode_;
        res->start = start_;
        res->sr = sr;
        res->seconds = secs;
        res->seating = seating_;
        {
            std::string t;
            for (auto& kv : p.m) if (kv.first != "continue" && kv.first != "start") t += (t.empty() ? "" : ", ") + kv.first + " " + kv.second.str();
            res->settings = mode_ + (morph ? "" : " over " + gr::num_text(secs) + " s") + ", " + std::to_string(n) + (n == 1 ? " realisation" : " realisations") +
                            (continue_ ? ", continuing from " + gr::num_text(start_) + " s" : "") + (t.empty() ? "" : "; " + t);
        }
        if (continue_) { cs->inFlight = true; carryHeld_ = cs; carryKey_ = key; }
        auto err = std::make_shared<std::string>();
        auto mix = std::make_shared<std::vector<std::vector<float>>>();
        auto missing = std::make_shared<size_t>(0);
        bool render = !out.empty();
        uint64_t seed = seed64_;
        int hop = hop_, fine = fine_;
        double thr = attackThreshold_;
        auto markov = std::make_shared<gr::markov_table>(markov_);
        runJob(
            [x, sr, secs, res, err, mix, missing, render, seed, hop, fine, thr, markov, players, p, n, morph](Job& job) {
                try {
                    const OrchDb& db = *res->spec->db;
                    std::vector<char> avail(db.entries.size(), 0);         // the sounds the orchestra allows
                    for (size_t k = 0; k < db.entries.size(); ++k) {
                        const auto& e = db.entries[k];
                        if (!db.present[k]) continue;
                        if (!rangeAllows(*res->spec, e) || !listed(res->spec->styles, e.tech) || !listed(res->spec->dynamics, e.dyn)) continue;
                        if (!inSelection(*res->spec, k)) continue;
                        avail[k] = 1;
                    }
                    gr::dbindex ix = gr::make_index(db.entries, avail);
                    gr::target tg;
                    if (morph) {
                        gr::analysis_options ao;
                        ao.threshold = thr;
                        ao.fine = fine;
                        tg = gr::analyse(*x, sr, db.block > 1 ? db.block : 2048, hop, ao);
                        if (job.cancelled()) return;
                    }
                    double base = morph ? .1 : 0, share = (render ? .8 : .9) - base;
                    gr::rng_t g(seed);
                    for (int k = 0; k < n; ++k) {
                        gr::run_hooks hooks;
                        hooks.progress = [&job, base, share, k, n](double f) {
                            job.progress((float)(base + share * ((double)k + std::clamp(f, 0.0, 1.0)) / n));
                            return !job.cancelled();
                        };
                        gr::run_result r = morph ? gr::orchestrate_morphological(ix, *players, tg, p, g, hooks)
                                                 : gr::granulate(ix, *players, secs, p, nullptr, markov->empty() ? nullptr : markov.get(), g, hooks);
                        if (r.stopped || job.cancelled()) return;
                        res->runs.push_back(std::move(r));
                    }
                    if (render) *mix = mixOf(granularNotes(*res), db, sr, job, *missing);
                } catch (const std::exception& e) {
                    *err = e.what();
                }
            },
            [this, res, err, mix, missing, out, players] {
                bool ok = err->empty() && !res->runs.empty();
                if (ok && carryHeld_) { carryHeld_->players = *players; carryHeld_->key = carryKey_; }
                releaseCarry();
                if (!ok) { patch->print("orchidea: " + (err->empty() ? std::string("nothing orchestrated") : *err)); return false; }
                gres_ = res;
                granularLast_ = true;
                out_ = out;
                std::vector<std::string> said;
                for (auto& r : res->runs)
                    for (auto& m : r.messages)
                        if (std::find(said.begin(), said.end(), m) == said.end()) { said.push_back(m); patch->print("orchidea: " + m); }
                if (!out.empty() && !mix->empty()) storeMix(*mix, *missing);
                outputNotes();
                outputScore();
                return true;
            });
    }
    // the orchestra's players booked by this box's run, released when it ends, is stopped, or the box goes
    void releaseCarry() {
        if (carryHeld_) carryHeld_->inFlight = false;
        carryHeld_.reset();
    }
    void chooseGranular(int seg, int n) {
        int ns = (int)gres_->runs.size();
        if (seg != 0) { patch->print("orchidea: a " + gres_->mode + " orchestration has one segment (choose 1 n)"); return; }
        if (n < 0 || n >= ns) { patch->print("orchidea: there are " + std::to_string(ns) + " realisations"); return; }
        auto res = std::make_shared<GranResult>(*gres_);
        res->choice = n;
        if (out_.empty() || !findBuffer(patch, out_)) {
            gres_ = res;
            outputNotes();
            outputScore();
            outletBang(1);
            return;
        }
        auto mix = std::make_shared<std::vector<std::vector<float>>>();
        auto missing = std::make_shared<size_t>(0);
        runJob([res, mix, missing](Job& job) { *mix = mixOf(granularNotes(*res), *res->spec->db, res->sr, job, *missing); },
               [this, res, mix, missing] {
                   gres_ = res;
                   if (!mix->empty()) storeMix(*mix, *missing);
                   outputNotes();
                   outputScore();
                   return true;
               });
    }
    void printRealisations() {
        patch->print("orchidea: " + gres_->mode + ", " + std::to_string(gres_->runs.size()) + " realisations");
        for (size_t k = 0; k < gres_->runs.size(); ++k) {
            const auto& r = gres_->runs[k].rep;
            char line[160];
            std::snprintf(line, sizeof line, "  %d%s: %d notes, %d events of %d due, saturation %.0f%%", (int)k + 1, (int)k == gres_->choice ? "*" : "",
                          r.notes, r.events, r.due, 100 * r.saturation);
            patch->print(line);
        }
    }
    // as Musil's granulation-report: the events due and written, the saturation, the unplayable, the dynamics
    // substituted, every player's notes and busy fraction
    void reportGranular() {
        const auto& r = gres_->runs[(size_t)std::clamp(gres_->choice, 0, (int)gres_->runs.size() - 1)].rep;
        char line[300];
        std::snprintf(line, sizeof line, "orchidea: %s: %d events due, %d played, %d notes written in %.1f s; %d skipped, every able player busy (saturation %.0f%%); "
                      "%d unplayable; %d at another dynamics than asked", gres_->mode.c_str(), r.due, r.events, r.notes, r.seconds, r.skipped_busy,
                      100 * r.saturation, r.skipped_unplayable, r.substituted);
        patch->print(line);
        patch->print("orchidea: " + gres_->settings);
        for (auto& u : r.unplayables)
            patch->print("  unplayable: " + u.styles.str() + (u.strict ? " at " + u.dyn : "") + " in octaves " + u.reg.str() + " (first at " + gr::num_text(std::round(u.at * 10) / 10) + " s)");
        for (size_t k = 0; k < r.players.size() && k < 200; ++k) {
            const auto& q = r.players[k];
            std::string ins;
            for (auto& i : q.instrs) ins += (ins.empty() ? "" : "|") + i;
            std::snprintf(line, sizeof line, ": %d notes, busy %.0f%%", q.notes, 100 * q.busy);
            patch->print("  " + ins + (q.seat.empty() ? "" : " (seated: " + q.seat + ")") + line);
        }
    }

    std::string mode_ = "mimetic";
    gr::params gp_;                                       // the granular and morphological parameters given
    std::map<std::string, std::string> curves_;           // a parameter read from a buffer~'s curve at the run
    gr::markov_table markov_;
    double start_ = 0, attackThreshold_ = 2, ratio_ = 2;
    bool continue_ = false;
    uint64_t seed64_ = 1;
    int hop_ = 1024, fine_ = 8192;
    std::shared_ptr<const GranResult> gres_;
    bool granularLast_ = false;                           // the last run was granular or morphological
    CarryPtr carryHeld_;
    std::string carryKey_;

    std::string orchestra_, out_;
    orch::analysis_options an_ = [] { orch::analysis_options a; a.segmentation = "none"; return a; }();
    orch::search_options opts_;
    bool seating_ = true;
    std::shared_ptr<const OrchResult> result_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

void registerOrchideaObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"orchestra", "Orchestration", "a database of instrumental samples and its players, for orchidea: name [file.db]", with<OrchestraBox>()},
        {"orchidea", "Orchestration", "assisted orchestration of a buffer~, after Orchidea, as a background job: orchestra", with<OrchideaBox>()},
    });
}

}  // namespace moritz
