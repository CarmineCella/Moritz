// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// fixes_tests.cpp — one test per fixed bug of the engine and its objects (each fails without its fix)

#include "rig.h"

#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <thread>

using namespace moritz;
using etest::Rig;

namespace {

[[maybe_unused]] float peak(const std::vector<float>& x, size_t from = 0, size_t to = ~size_t(0)) {
    float p = 0.f;
    for (size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

// a box that remembers the messages it gets and the thread they come on
class Probe : public Box {
public:
    Probe() { setPorts({Port::Control}, {}); cls = "probe"; }
    void message(int, const Message& m) override {
        if (!m.empty() && m[0].isFloat()) values.push_back(m[0].f);
        threads.push_back(std::this_thread::get_id());
    }
    std::vector<float> values;
    std::vector<std::thread::id> threads;
};

[[maybe_unused]] std::string tempPath(const std::string& name) {
    const char* t = std::getenv("TMPDIR");
    std::string d = t && *t ? t : "/tmp";
    if (d.back() != '/') d += '/';
    return d + name;
}

}  // namespace

// groove~: a NaN rate made its position NaN for good (the clamp let it through): silence forever
TEST(groove_recovers_from_a_nan_rate) {
    Rig r("(patch (obj b buffer~ snd data/Vox.wav) (obj s sig~ 1) (obj g groove~ snd 1) (obj o dac~ 1)"
          " (connect s 0 g 0) (connect g 0 o 0))");
    r.box("g")->message(0, {Atom("loop"), Atom(1.f)});
    r.run(4410);
    r.box("s")->message(0, {Atom(std::nanf(""))});
    r.run(640);
    r.box("s")->message(0, {Atom(1.f)});
    auto out = r.run(8820);
    CHECK(peak(etest::channel(out, 2, 0), 4410) > 0.f);
}

// A patch owned by another thread (the render object's worker): messages sent from elsewhere wait
// for it, and it dispatches them, in order, between blocks
TEST(foreign_messages_run_on_the_dispatch_thread) {
    Rig r("(patch (obj r receive x (pos 0 0)))");
    auto probe = std::make_unique<Probe>();
    Probe* pr = probe.get();
    Box* p = r.p.addBox(std::move(probe), 0, 40);
    r.p.connect(r.box("r"), 0, p, 0);
    std::atomic<bool> owned{false}, stop{false};
    std::thread::id worker;
    std::thread w([&] {
        worker = std::this_thread::get_id();
        r.p.setDispatchThread(worker);
        owned = true;
        std::vector<float> buf(128);
        while (!stop) {
            r.p.drainForeign();
            r.p.process(nullptr, 0, buf.data(), 2, 64);
            std::this_thread::yield();
        }
        r.p.drainForeign();
        r.p.setDispatchThread(std::thread::id());
    });
    while (!owned) std::this_thread::yield();
    for (int k = 0; k < 200; ++k) {
        if (k % 2) r.p.sendTo("x", {Atom((float)k)});
        else r.box("r")->outlet(0, {Atom((float)k)});
    }
    stop = true;
    w.join();
    CHECK(pr->values.size() == 200);
    bool inOrder = true, onWorker = true;
    for (size_t k = 0; k < pr->values.size(); ++k) inOrder = inOrder && pr->values[k] == (float)k;
    for (auto& t : pr->threads) onWorker = onWorker && t == worker;
    CHECK(inOrder);
    CHECK(onWorker);
}

// The render object's worker flushes denormals, as the audio callback does: a decaying filter
// gives zeros, not denormal floats
TEST(render_worker_flushes_denormals) {
    std::string inner = tempPath("moritz-engine-ftz.moritz");
    {
        std::ofstream f(inner);
        f << "(patch (obj l loadbang) (obj c click~) (obj b biquad~ 1 0 0 -0.9 0) (obj o dac~ 1)"
             " (connect l 0 c 0) (connect c 0 b 0) (connect b 0 o 0))\n";
    }
    bool sync = jobsSynchronous;
    jobsSynchronous = false;
    Rig r("(patch (obj buf buffer~ out) (obj rn render " + inner + " 1))");
    r.box("rn")->message(0, {Atom("render"), Atom("out"), Atom(0.05f)});
    BufferBox* b = findBuffer(&r.p, "out");
    for (int k = 0; k < 2000 && b && b->buf.frames == 0; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        drainMain();
    }
    jobsSynchronous = sync;
    CHECK(b && b->buf.frames > 1000);
    bool denormal = false;
    if (b)
        for (float v : b->buf.data) denormal = denormal || (v != 0.f && std::fabs(v) < FLT_MIN);
    CHECK(!denormal);
    std::remove(inner.c_str());
}

// The quick recorder: start() re-made its ring while the audio thread could be pushing into the old
// one (start/stop over and over while another thread pushes: clean under the sanitizers)
TEST(recorder_restarts_while_the_audio_thread_pushes) {
    Recorder rec;
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> x(2 * 256, 0.25f);
        while (!stop) rec.push(x.data(), 256, 2);
    });
    std::string path = tempPath("moritz-engine-recorder.wav");
    bool ok = true;
    for (int k = 0; k < 30; ++k) ok = ok && rec.start(path, 2, 8000 + 1000 * (k % 3));
    rec.stop();
    stop = true;
    audio.join();
    CHECK(ok);
    std::remove(path.c_str());
}

// buffer~ joined the patch (and record~ found it) before it had its memory: frames were set before
// the data, without the lock, while record~ wrote. Buffers come and go while one records.
TEST(buffer_gets_its_memory_before_record_writes_it) {
    Rig r("(patch (obj s sig~ 0.5) (obj rec record~ tape) (connect s 0 rec 0))");
    r.box("rec")->message(0, {Atom("loop"), Atom(1.f)});
    r.box("rec")->message(0, {Atom(1.f)});
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> buf(2 * 64);
        while (!stop) r.p.process(nullptr, 0, buf.data(), 2, 64);
    });
    for (int k = 0; k < 40; ++k) {
        Box* b = r.p.addBox(r.p.create("buffer~", {Atom("tape"), Atom(50.f + k)}), 0, 100);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        r.p.removeBoxes({b});
    }
    stop = true;
    audio.join();
    CHECK(true);
}

// line~ kept at most 64 segments of a list and dropped the rest without a word; now a list holds
// as many as its queue (1023), and what does not fit is reported
TEST(line_keeps_long_lists_and_reports_what_it_drops) {
    Rig r("(patch (obj l line~) (obj o dac~ 1) (connect l 0 o 0))");
    Message m;
    for (int k = 1; k <= 100; ++k) {
        m.push_back(Atom(k * 0.01f));          // dac~ clips at 1
        m.push_back(Atom(1.f));
    }
    r.box("l")->message(0, m);
    auto y = etest::channel(r.run(44100 / 5), 2, 0);

    CHECK(std::fabs(y.back() - 1.f) < 1e-4f);
    CHECK(!r.printedSomething("dropped"));
    Message big;
    for (int k = 0; k < 1100; ++k) {
        big.push_back(Atom(0.f));
        big.push_back(Atom(1.f));
    }
    r.box("l")->message(0, big);
    CHECK(r.printedSomething("line~: 78 segments dropped"));
}
