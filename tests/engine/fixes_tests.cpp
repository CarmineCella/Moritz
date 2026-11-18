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

// line~ kept at most 64 segments of a list and dropped the rest without a word; now a list goes to
// the audio thread whole (no limit), and however many lists wait, the newest is played
TEST(line_keeps_long_lists_and_the_newest_list) {
    Rig r("(patch (obj l line~) (obj o dac~ 1) (connect l 0 o 0))");
    Message m;
    for (int k = 1; k <= 2000; ++k) {
        m.push_back(Atom(k * 0.0005f));        // dac~ clips at 1
        m.push_back(Atom(0.1f));
    }
    r.box("l")->message(0, m);
    auto y = etest::channel(r.run(44100 / 2), 2, 0);
    CHECK(std::fabs(y.back() - 1.f) < 1e-4f);
    CHECK(!r.printedSomething("dropped"));
    for (int k = 1; k <= 70; ++k) r.box("l")->message(0, {Atom(k * 0.01f)});   // nothing takes them meanwhile
    y = etest::channel(r.run(256), 2, 0);
    CHECK(std::fabs(y.back() - 0.70f) < 1e-6f);
    CHECK(!r.printedSomething("line~"));
}

// line~ with audio off: more lists than its mailbox holds (the newest wait in a slot), and more events
// than the box's queue holds (it catches up at once: the events dropped are not missed); it ends on the
// last list's target
TEST(line_ends_on_the_newest_list_sent_with_audio_off) {
    for (int lists : {100, 300}) {
        Rig r("(patch (obj l line~) (obj o dac~ 1) (connect l 0 o 0))");
        r.p.dspRunning = false;
        for (int k = 1; k <= lists; ++k) r.box("l")->message(0, {Atom(k * 0.001f), Atom(k % 2 ? 0.f : 1.f)});
        r.p.dspRunning = true;
        auto y = etest::channel(r.run(4410), 2, 0);
        CHECK(std::fabs(y.back() - lists * 0.001f) < 1e-6f);
    }
}

// The queue of line~'s events overflowed: it caught up keeping 64 events, and each list whose event was
// dropped stayed in the mailbox: line~ then played, at each new list, an old one
TEST(line_stays_in_step_with_its_lists_after_a_resync) {
    Rig r("(patch (obj l line~) (obj o dac~ 1) (connect l 0 o 0))");
    for (int k = 0; k < 250; ++k) r.box("l")->message(0, {Atom("stop")});
    for (int k = 1; k <= 10; ++k) r.box("l")->message(0, {Atom(k * 0.001f)});   // the last events find it full
    auto first = etest::channel(r.run(256), 2, 0);
    CHECK(std::fabs(first.back() - 0.005f) < 1e-6f);     // the newest 64 events are kept: lists 1 to 5
    r.box("l")->message(0, {Atom(0.77f)});
    auto y = etest::channel(r.run(256), 2, 0);
    CHECK(std::fabs(y.back() - 0.77f) < 1e-6f);
    r.box("l")->message(0, {Atom(0.5f), Atom(10.f)});
    y = etest::channel(r.run(4410), 2, 0);
    CHECK(std::fabs(y.back() - 0.5f) < 1e-6f);
}

// click~ and sfplay~ hand the newest shape or file to the audio thread: with audio off, a mailbox of 16
// shapes (8 files) filled up; then click~ dropped the newer shapes without a word and played an old one,
// sfplay~ refused files and kept all it had decoded
TEST(click_plays_the_newest_shape_sent_with_audio_off) {
    Rig r("(patch (obj c click~) (obj o dac~ 1) (connect c 0 o 0))");
    r.p.dspRunning = false;
    for (int k = 1; k <= 40; ++k) r.box("c")->message(0, {Atom("set"), Atom(k * 0.01f), Atom(0.5f)});
    r.box("c")->message(0, {Atom("bang")});
    r.p.dspRunning = true;
    auto y = etest::channel(r.run(256), 2, 0);
    CHECK(std::fabs(y[0] - 0.40f) < 1e-6f && std::fabs(y[1] - 0.5f) < 1e-6f);
}

TEST(sfplay_plays_the_newest_file_opened_with_audio_off) {
    const char* t = std::getenv("TMPDIR");
    std::string dir = t && *t ? t : "/tmp";
    if (dir.back() != '/') dir += '/';
    Rig r("(patch (obj s sfplay~) (obj o dac~ 1) (connect s 0 o 0))");
    r.p.dspRunning = false;
    std::vector<std::string> files;
    for (int k = 1; k <= 12; ++k) {
        SampleBuffer b;
        b.frames = 4410;
        b.data.assign((size_t)b.frames, k * 0.05f);
        files.push_back(dir + "moritz-engine-sfplay-" + std::to_string(k) + ".wav");
        std::string err;
        CHECK(writeWavFile(files.back(), b, err));
        r.box("s")->message(0, {Atom("open"), Atom(files.back())});
    }
    r.box("s")->message(0, {Atom(1.f)});
    r.p.dspRunning = true;
    auto y = etest::channel(r.run(1024), 2, 0);
    CHECK(!r.printedSomething("sfplay~"));
    CHECK(std::fabs(y[100] - 0.6f) < 1e-3f);
    for (auto& f : files) std::remove(f.c_str());
}

// buffer~ write read the samples while record~ wrote them on the audio thread (a data race: the thread
// sanitizer reports it): they are copied under the patch lock, and the file is written from the copy
TEST(buffer_write_copies_the_samples_under_the_lock) {
    const char* t = std::getenv("TMPDIR");
    std::string path = std::string(t && *t ? t : "/tmp") + "/moritz-engine-buffer-write.wav";
    Rig r("(patch (obj s sig~ 0.5) (obj rec record~ tape) (obj b buffer~ tape 50) (connect s 0 rec 0))");
    r.box("rec")->message(0, {Atom("loop"), Atom(1.f)});
    r.box("rec")->message(0, {Atom(1.f)});
    r.run(64);
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> buf(2 * 64);
        while (!stop) r.p.process(nullptr, 0, buf.data(), 2, 64);
    });
    for (int k = 0; k < 20; ++k) r.box("b")->message(0, {Atom("write"), Atom(path)});
    r.box("b")->message(0, {Atom("size"), Atom(100.f)});
    stop = true;
    audio.join();
    SampleBuffer back;
    std::string err;
    CHECK(decodeAudioFile(path, back, err) && back.frames == (long)(0.05 * 44100));
    CHECK(!r.printedSomething("buffer~"));
    std::remove(path.c_str());
}
