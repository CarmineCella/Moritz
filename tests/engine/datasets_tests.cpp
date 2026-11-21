// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// datasets_tests.cpp — datasets by name in Moritz (datasets.h, musil/src/music/datasets.h): orchestra reading a name
// (bundled, registered, missing), orchestra's analyse (a folder of sounds into a feature file, then read), and the
// Datasets window's background work on its thread: a folder analysed and registered, an archive installed, each also
// cancelled (nothing left, no done). Nothing is downloaded.
//
// What writes the user's configuration (~/.musil/datasets.mu) runs only in the scratch home CMake gives these tests
// (HOME=.../engine-tests/home): by hand, with the real home, those checks are skipped.

#include "datasets.h"
#include "musil_host.h"
#include "rig.h"
#include "../../musil/tests/stored_zip.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace moritz;
using etest::Rig;
namespace fs = std::filesystem;
namespace ds = musil::datasets;

namespace {

class Catch : public Box {
public:
    Catch() { setPorts({Port::Control}, {}); cls = "catch"; }
    void message(int, const Message& m) override { got.push_back(m); }
    std::vector<Message> got;
};

// an orchestra whose number of sounds (outlet 3) is caught
struct OrchestraRig {
    Rig r;
    Catch* count;
    explicit OrchestraRig(const std::string& args) : r("(patch (obj or orchestra band" + (args.empty() ? "" : " " + args) + "))") {
        auto c = std::make_unique<Catch>();
        count = c.get();
        Box* b = r.p.addBox(std::move(c), 0, 300);
        r.p.connect(r.box("or"), 2, b, 0);
        // what the box read when it was made came out before the catcher was there: its info says it
        if (!args.empty()) {
            r.box("or")->message(0, {Atom("info")});
            for (auto& s : r.printed)
                if (s.find(" sounds (") != std::string::npos) count->got.push_back({Atom((float)std::atof(s.substr(s.find(": ") + 2).c_str()))});
        }
    }
    void send(const Message& m) { r.box("or")->message(0, m); }
    float sounds() const { return count->got.empty() ? -1.f : count->got.back()[0].f; }
};

// is HOME the scratch home of the tests? (then the configuration may be written)
bool scratchHome() {
    const char* h = std::getenv("HOME");
    return h && std::string(h).find("engine-tests") != std::string::npos;
}

fs::path scratch(const std::string& name) {
    fs::path d = fs::temp_directory_path() / ("moritz_datasets_" + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// MicroSOL's sounds copied into a folder of the given name
fs::path copyOfMicroSOL(const fs::path& where, const std::string& name) {
    fs::path to = where / name;
    std::error_code ec;
    fs::copy("data/microsol/microsol", to, fs::copy_options::recursive, ec);
    return to;
}

// up to two minutes: an analysis under ThreadSanitizer on a busy machine took more than the twenty seconds this had
void drainUntil(const std::function<bool()>& cond) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!cond() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        drainMain();
    }
}

size_t hiddenOrTemporary(const fs::path& d) {
    size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        std::string f = it->path().filename().string();
        n += f[0] == '.' || f.find(".tmp-") != std::string::npos;
    }
    return n;
}

}  // namespace

// orchestra band microsol: the bundled MicroSOL by name; a missing name says so
TEST(orchestra_reads_a_dataset_by_name) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    {
        OrchestraRig o("microsol");
        CHECK(o.sounds() == 32.f);
        o.send({Atom("read"), Atom("MicroSOL")});
        CHECK(o.count->got.size() == 2 && o.sounds() == 32.f);
        o.r.printed.clear();
        o.send({Atom("read"), Atom("nosuchsol")});
        CHECK(o.r.printedSomething("no dataset named \"nosuchsol\""));
        CHECK(o.r.printedSomething("register it once"));
        CHECK(o.count->got.size() == 2);
        // a path still reads as a path
        o.send({Atom("read"), Atom("data/microsol/microsol.spectrum.db")});
        CHECK(o.count->got.size() == 3 && o.sounds() == 32.f);
    }
    CHECK(resolveDataset("microsol").find("microsol/microsol.spectrum.db") != std::string::npos);
    jobsSynchronous = sync;
}

// a registered name (as Extras > Datasets..., Register a file, writes it), read by name, then forgotten
TEST(orchestra_reads_a_registered_dataset) {
    if (!scratchHome()) { std::printf("  (skipped: HOME is not the tests' scratch home)\n"); return; }
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    std::string config = datasetsPlace().config;
    ds::register_dataset(config, "testsol", fs::absolute("data/microsol/microsol.spectrum.db").string());
    {
        OrchestraRig o("testsol");
        CHECK(o.sounds() == 32.f);
    }
    CHECK(ds::forget_dataset(config, "testsol"));
    {
        OrchestraRig o("testsol");
        CHECK(o.sounds() == -1.f);
        CHECK(o.r.printedSomething("no dataset named \"testsol\""));
    }
    jobsSynchronous = sync;
}

// analyse folder: the feature file next to the folder, the database read; never over a file; bad inputs answered;
// only where the patch may write (a patch from elsewhere does not write next to any folder of the machine)
TEST(orchestra_analyses_a_folder) {
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    fs::path d = scratch("analyse");
    fs::path sounds = copyOfMicroSOL(d, "MySOL");
    {
        OrchestraRig o("");
        o.r.p.dir = (d / "patches").string();               // a patch in another folder: not next to MySOL
        o.send({Atom("analyse"), Atom(sounds.string())});
        CHECK(o.r.printedSomething("not written"));
        CHECK(!fs::exists(d / "MySOL.spectrum.db"));
    }
    {
        OrchestraRig o("");
        o.r.p.dir = d.string();                            // the patch next to the folder: it may write there
        o.send({Atom("analyse"), Atom(sounds.string())});
        CHECK(fs::exists(d / "MySOL.spectrum.db"));
        CHECK(o.sounds() == 32.f);
        CHECK(hiddenOrTemporary(d) == 0);
        // its features are MicroSOL's (the same analysis as the bundled feature file's)
        std::ifstream f(d / "MySOL.spectrum.db");
        std::string head;
        std::getline(f, head);
        CHECK(head == "spectrum 2048 256 1024");
        o.r.printed.clear();
        o.send({Atom("analyse"), Atom(sounds.string())});
        CHECK(o.r.printedSomething("is already there"));
        CHECK(o.count->got.size() == 1);
        o.r.printed.clear();
        o.send({Atom("analyse"), Atom(sounds.string()), Atom("nonsense")});
        CHECK(o.r.printedSomething("unknown feature type"));
        o.r.printed.clear();
        o.send({Atom("analyse"), Atom(sounds.string()), Atom("spectrum"), Atom(1000.f)});
        CHECK(o.r.printedSomething("power of 2"));
        o.r.printed.clear();
        o.send({Atom("analyse"), Atom((d / "nothing").string())});
        CHECK(o.r.printedSomething("not a folder"));
        // mfcc, 13 coefficients by default
        fs::path other = copyOfMicroSOL(d, "Mfcc");
        o.send({Atom("analyse"), Atom(other.string()), Atom("mfcc")});
        std::ifstream g(d / "Mfcc.spectrum.db");
        std::getline(g, head);
        CHECK(head == "mfcc 2048 256 13");
        CHECK(o.sounds() == 32.f);
    }
    std::error_code ec;
    fs::remove_all(d, ec);
    jobsSynchronous = sync;
}

// the orchestra deleted while it analyses: the job is cancelled and waited for, no file is left half written
TEST(orchestra_deleted_while_analysing) {
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    fs::path d = scratch("analyse_deleted");
    fs::path sounds = copyOfMicroSOL(d, "Gone");
    {
        OrchestraRig o("");
        o.send({Atom("analyse"), Atom(sounds.string())});
        o.r.p.removeBoxes({o.r.box("or")});
        for (int k = 0; k < 20; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
    }
    CHECK(hiddenOrTemporary(d) == 0);
    std::error_code ec;
    fs::remove_all(d, ec);
    jobsSynchronous = sync;
}

// the Datasets window's work on its thread: a folder analysed and registered; then one cancelled at once
TEST(dataset_task_analyses_and_registers) {
    if (!scratchHome()) { std::printf("  (skipped: HOME is not the tests' scratch home)\n"); return; }
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    fs::path d = scratch("task");
    fs::path sounds = copyOfMicroSOL(d, "samples");
    std::string file, error;
    int dones = 0;
    float last = -1.f;
    {
        DatasetTask t;
        t.onProgress = [&](float p, const std::string&) { last = p; };
        t.onDone = [&](const std::string& f, const std::string& e) { file = f; error = e; ++dones; };
        CHECK(t.analyse(sounds.string(), "TaskSOL", musil::db_gen_params(), false));
        drainUntil([&] { return dones > 0; });
        CHECK(dones == 1 && error.empty());
        CHECK(file == (d / "TaskSOL.spectrum.db").generic_string());
        CHECK(last == 1.f);
        CHECK(resolveDataset("tasksol") == fs::path(file).generic_string() || fs::equivalent(resolveDataset("tasksol"), file));
        // not over a file already there, unless asked
        dones = 0;
        CHECK(!t.analyse(sounds.string(), "TaskSOL", musil::db_gen_params(), false));
        CHECK(dones == 1 && error.find("already there") != std::string::npos);
        CHECK(!t.analyse(sounds.string(), "a b", musil::db_gen_params(), false));
        CHECK(error.find("not a dataset's name") != std::string::npos);
        // cancelled at once: no done; either nothing (cancelled in time) or the whole file; nothing temporary left
        dones = 0;
        CHECK(t.analyse(sounds.string(), "StoppedSOL", musil::db_gen_params(), false));
        t.cancel();
        for (int k = 0; k < 50; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        CHECK(dones == 0);
        CHECK(!t.running());
        CHECK(hiddenOrTemporary(d) == 0);
        std::string why;
        bool registered = !ds::find(datasetsPlace(), ds::read_config(datasetsPlace().config), "stoppedsol", &why).empty();
        CHECK(registered == fs::exists(d / "StoppedSOL.spectrum.db"));
        // deleted while working: waits for its job, drops its done
        DatasetTask* u = new DatasetTask;
        u->onDone = [&](const std::string&, const std::string&) { ++dones; };
        fs::remove(d / "StoppedSOL.spectrum.db");
        CHECK(u->analyse(sounds.string(), "StoppedSOL", musil::db_gen_params(), true));
        delete u;
        for (int k = 0; k < 50; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
        CHECK(dones == 0);
        CHECK(hiddenOrTemporary(d) == 0);
    }
    ds::forget_dataset(datasetsPlace().config, "tasksol");
    ds::forget_dataset(datasetsPlace().config, "stoppedsol");
    std::error_code ec;
    fs::remove_all(d, ec);
    jobsSynchronous = sync;
}

// the Datasets window's Install a zip...: unpacked into the datasets folder on the job's thread, found by name
TEST(dataset_task_installs_an_archive) {
    if (!scratchHome()) { std::printf("  (skipped: HOME is not the tests' scratch home)\n"); return; }
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    fs::path d = scratch("install");
    std::string config = datasetsPlace().config;
    ds::set_folder(config, (d / "datasets").string());
    std::string zip = (d / "ZipSOL.zip").string();
    CHECK(ziptest::write(zip, {{"ZipSOL/", ""}, {"ZipSOL/Vn-ord-C4-mf.wav", ziptest::wav()},
                               {"ZipSOL.spectrum.db", "spectrum 2048 256 1\n/Vn-ord-C4-mf.wav;1\n"}}));
    std::string file, error;
    int dones = 0;
    {
        DatasetTask t;
        t.onDone = [&](const std::string& f, const std::string& e) { file = f; error = e; ++dones; };
        CHECK(t.install("zipsol", zip));
        drainUntil([&] { return dones > 0; });
        if (error.find("cannot run") != std::string::npos) std::printf("  (%s: not tested here)\n", error.c_str());
        else {
            CHECK(error.empty());
            CHECK(fs::exists(d / "datasets" / "ZipSOL" / "Vn-ord-C4-mf.wav"));
            CHECK(fs::equivalent(resolveDataset("zipsol"), d / "datasets" / "ZipSOL.spectrum.db"));
            OrchestraRig o("");
            jobsSynchronous = true;
            o.send({Atom("read"), Atom("zipsol")});
            jobsSynchronous = false;
            CHECK(o.sounds() == 1.f);
            // cancelled at once: nothing more in the folder, no done
            dones = 0;
            fs::path other = d / "other";
            ds::set_folder(config, other.string());
            CHECK(t.install("zipsol", zip));
            t.cancel();
            for (int k = 0; k < 50; ++k) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); drainMain(); }
            CHECK(dones == 0);
            CHECK(hiddenOrTemporary(other) == 0);
        }
    }
    ds::set_folder(config, "");
    std::error_code ec;
    fs::remove_all(d, ec);
    jobsSynchronous = sync;
}

namespace moritz {
std::string portHelp(const Box* b, bool outlet, int i);   // ports.cpp
}

// the outlets of orchestra and orchidea say what comes out of them (the tooltips and OBJECTS.md read portHelp)
TEST(orchestra_and_orchidea_outlets_are_described) {
    Rig r("(patch (obj or orchestra pband) (obj oi orchidea pband))");
    Box* o = r.box("or");
    Box* i = r.box("oi");
    CHECK(o && i);
    if (!o || !i) return;
    auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };
    CHECK(has(portHelp(o, true, 0), "progress"));
    CHECK(has(portHelp(o, true, 1), "the database is read"));
    CHECK(has(portHelp(o, true, 2), "number of sounds"));
    CHECK(has(portHelp(o, true, 3), "players"));
    CHECK(has(portHelp(o, false, 0), "dataset's name"));
    CHECK(has(portHelp(i, true, 0), "progress"));
    CHECK(has(portHelp(i, true, 1), "done"));
    CHECK(has(portHelp(i, true, 2), "instrument, pitch, dynamics, technique, onset"));
    CHECK(has(portHelp(i, true, 3), "distance"));
    for (int k = 0; k < 4; ++k) CHECK(!has(portHelp(o, true, k), "messages") && !has(portHelp(i, true, k), "messages"));
}
