// embed.cpp — Musil inside another program: many interpreters, each on its own thread, created,
// used and destroyed; afterwards nothing they made is left alive, and a host can refuse the
// builtins that wait.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause

#include "musil.h"

#include <filesystem>
#include <fstream>
#include <thread>

using namespace musil;

static int failures = 0;
static void expect(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL " << what << "\n"; ++failures; }
}

int main() {
    const long v0 = live_values().load(), e0 = live_envs().load();
    std::cerr << "embed: threads\n" << std::flush;   // where it stopped, if it crashes
    // eight threads, each making and destroying interpreters that load the libraries and run code
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([t, &errors] {
            for (int k = 0; k < 5; ++k) {
                Interp I;
                make_env(I);
                std::ostringstream out; I.out = &out;
                try {
                    I.run("load \"std.mu\"\n"
                          "function local-sum (xs) {\n"
                          "    var acc 0\n"
                          "    function add (x) (set acc (+ acc x))\n"
                          "    each xs add\n"
                          "    return acc\n"
                          "}\n"
                          "var deep (list)\n"
                          "for (var k 0) (< k 20000) (var k (+ k 1)) (set deep (list deep))\n"
                          "print (local-sum (list 1 2 3 " + std::to_string(t) + "))\n", "<embed>");
                    if (out.str() != std::to_string(6 + t) + "\n") ++errors;
                } catch (const std::exception& ex) { std::cerr << ex.what() << "\n"; ++errors; }
            }
        });
    }
    for (auto& th : threads) th.join();
    expect(errors == 0, "interpreters on threads computed the right results");
    expect(live_values().load() == v0, "every value is freed when the interpreters are gone (left: " +
                                       std::to_string(live_values().load() - v0) + ")");
    expect(live_envs().load() == e0, "every environment is freed when the interpreters are gone (left: " +
                                     std::to_string(live_envs().load() - e0) + ")");
    std::cerr << "embed: blocking\n" << std::flush;   // where it stopped, if it crashes
    // a host refuses builtins that wait
    {
        Interp I;
        make_env(I);
        I.allow_blocking = false;
        bool refused = false;
        try { I.run("sleep 10", "<embed>"); } catch (const Error& e) { refused = std::string(e.what()).find("not allowed") != std::string::npos; }
        expect(refused, "sleep is refused when blocking is not allowed");
    }
    std::cerr << "embed: thread stack\n" << std::flush;   // where it stopped, if it crashes
    // an interpreter on a std::thread (512 KB of stack on macOS) stops a deep recursion with an error: the
    // stack it measures is the thread's own, not the main thread's
    {
        std::string what;
        std::thread th([&what] {
            Interp I; make_env(I);
            std::ostringstream out; I.out = &out;
            try { I.run("function rec (n) (+ 1 (rec (+ n 1)))\nrec 0\n", "<thread>"); what = "no error"; }
            catch (const Error& e) { what = e.what(); }
            try { I.run("var deep (parse (concat (repeat \"(\" 2900) (repeat \")\" 2900)))\n", "<thread>"); }   // the reader within its depth limit
            catch (const Error& e) { (void)e; }
        });
        th.join();
        expect(what.find("stack overflow") != std::string::npos, "a recursion on a small thread ends with a stack overflow error (" + what.substr(0, 80) + ")");
    }
    std::cerr << "embed: stop\n" << std::flush;   // where it stopped, if it crashes
    // Stop: request_stop from another thread ends a loop that catches every error; try does not catch it, and
    // the request stays until clear_stop
    {
        Interp I; make_env(I);
        std::ostringstream out; I.out = &out;
        I.run("load \"std.mu\"\n", "<embed>");
        std::atomic<bool> started{false};
        std::thread stopper([&] { while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1)); std::this_thread::sleep_for(std::chrono::milliseconds(50)); I.request_stop(); });
        I.def("started!", [](vlist&, Interp&) -> vptr { return v_nil(); }, 0, 0);
        bool stopped = false; auto t0 = std::chrono::steady_clock::now();
        started = true;
        try { I.run("var n 0\nwhile 1 { try { set n (+ n 1) } catch e (print e) }\n", "<embed>"); }
        catch (const Stop_signal&) { stopped = true; }
        catch (const std::exception& e) { std::cerr << e.what() << "\n"; }
        stopper.join();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        expect(stopped && secs < 5, "request_stop ends a loop with try in it (Stop_signal reaches the host)");
        expect(I.stopping(), "the stop request stays until the host clears it");
        bool again = false; try { I.run("1", "<embed>"); } catch (const Stop_signal&) { again = true; }
        expect(again, "...a second evaluation is stopped too");
        I.clear_stop();
        bool ok = false; try { ok = I.run("(+ 1 2)", "<embed>")->num[0] == 3; } catch (...) {}
        expect(ok, "after clear_stop the interpreter runs again");
        I.request_stop();
        bool in_sleep = false; try { I.run("sleep 10", "<embed>"); } catch (const Stop_signal&) { in_sleep = true; }
        expect(in_sleep, "a stop requested before sleep ends it at once");
        I.clear_stop();
    }
    std::cerr << "embed: capabilities\n" << std::flush;   // where it stopped, if it crashes
    // the capability profile: checked inside each builtin, whatever the names
    {
        namespace fs = std::filesystem;
        fs::path root = fs::temp_directory_path() / "musil_embed_caps", inside = root / "inside", outside = root / "outside";
        std::error_code ec; fs::remove_all(root, ec); fs::create_directories(inside); fs::create_directories(outside);
        fs::create_directory_symlink(outside, inside / "link", ec);
        Interp I; make_env(I);
        std::ostringstream out; I.out = &out;
        I.caps = Capabilities::sandboxed({ inside.string() });
        auto refused = [&](const std::string& code) {
            try { I.run(code, "<caps>"); return false; }
            catch (const Error& e) { return std::string(e.what()).find("not allowed here") != std::string::npos; }
        };
        auto q = [](const fs::path& p) { return "\"" + p.generic_string() + "\""; };   // '/': no escapes in Musil's strings
        expect(!refused("write " + q(inside / "a.txt") + " \"ok\""), "sandboxed: writing under the root is allowed");
        expect(refused("write " + q(outside / "a.txt") + " \"no\""), "sandboxed: writing outside the root is refused");
        expect(!fs::exists(outside / "a.txt"), "...and nothing was written");
        expect(refused("write " + q(inside / ".." / "outside" / "b.txt") + " \"no\""), "sandboxed: .. cannot step out");
        expect(refused("write " + q(inside / "link" / "c.txt") + " \"no\""), "sandboxed: a symbolic link cannot step out");
        expect(refused("append-file " + q(outside / "a.txt") + " \"no\""), "sandboxed: append-file");
        expect(refused("mkdir " + q(outside / "d")), "sandboxed: mkdir");
        expect(refused("remove " + q(outside)), "sandboxed: remove");
        expect(refused("write-wav " + q(outside / "x.wav") + " 44100 (vec 0 0)"), "sandboxed: write-wav");
        expect(refused("exec \"echo hi\""), "sandboxed: exec");
        expect(refused("udp-send \"127.0.0.1\" 9 \"x\""), "sandboxed: udp-send");
        expect(refused("udp-receive \"127.0.0.1\" 47999 0.01"), "sandboxed: udp-receive");
        expect(refused("serve 47998"), "sandboxed: serve");
#ifndef _WIN32                                       // OSC is not available on Windows
        expect(refused("osc-send \"127.0.0.1\" 9 \"/x\" 1"), "sandboxed: osc-send");
#endif
        expect(refused("sleep 0.01"), "sandboxed: sleep");
        expect(refused("exit 0"), "sandboxed: exit");
        expect(!refused("read " + q(inside / "a.txt")), "sandboxed: reading is allowed");
        { std::ofstream m(outside / "m.mu"); m << "var m 1\n"; }
        I.caps.read_roots = { inside.string() };
        expect(refused("read \"/etc/hosts\""), "read roots: reading outside them is refused");
        expect(refused("load " + q(outside / "m.mu")), "read roots: load of a file outside them is refused");
        bool rebound = false;
        try { I.run("var exec (function (c) c)\nexec \"x\"", "<caps>"); rebound = true; } catch (...) {}
        expect(rebound, "the profile is not a list of names: a program may define its own exec");
        I.caps = Capabilities::full();
        expect(!refused("write " + q(outside / "a.txt") + " \"ok\""), "full: writing anywhere");
        fs::remove_all(root, ec);
    }
    std::cerr << "embed: per-interpreter state\n" << std::flush;   // where it stopped, if it crashes
    // the state of the libraries is each interpreter's: idle hooks and the evaluation port of one are not the other's
    {
        Interp A, B; make_env(A); make_env(B);
        std::ostringstream oa, ob; A.out = &oa; B.out = &ob;
        A.run("function tick () nil\nidle-hook! \"tick\"\n", "<A>");
        expect(A.run("idle-hooks", "<A>")->l.size() == 1 && B.run("idle-hooks", "<B>")->l.empty(), "idle hooks belong to their interpreter");
#ifndef _WIN32
        A.run("serve 47781", "<A>");
        expect(A.run("serving", "<A>")->num[0] == 47781 && B.run("serving", "<B>")->num[0] == 0, "the evaluation port belongs to its interpreter");
        A.run("serve-stop", "<A>");
#endif
    }
    std::cerr << "embed: done\n" << std::flush;
    if (failures == 0) std::cout << "embed: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
