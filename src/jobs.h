// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// jobs.h — background jobs: long offline work (NMF, HPSS, rendering a patch)
// on a worker thread, so the editor never freezes.
//
// Rules: the work gets copies of everything it needs (taken on the GUI
// thread before it starts) and touches nothing else; its result is applied
// by done(), which runs on the GUI thread. A box deleted while its job runs
// cancels it and waits for it, and its pending done() is dropped.
//
// With jobsSynchronous (moritz --render) or when started from inside another
// job, the work runs at once, on the calling thread.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace moritz {

// Runs fn on the GUI thread soon (from any thread). The GUI sets mainWake to
// wake itself (Fl::awake) and calls drainMain() then.
void postToMain(std::function<void()> fn);
void drainMain();
extern std::function<void()> mainWake;
extern bool jobsSynchronous;

class Job {
public:
    Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    ~Job() { cancel(); }

    // work runs on the worker thread (it may call progress() and should
    // check cancelled() now and then); done runs afterwards on the GUI
    // thread, unless the job was cancelled.
    void start(std::function<void(Job&)> work, std::function<void()> done);
    void cancel();                                      // stop and wait (GUI thread)
    bool running() const { return running_.load(); }

    // for the work
    bool cancelled() const { return cancel_.load(); }
    void progress(float p);                             // 0..1, reported on the GUI thread

    std::function<void(float)> onProgress;              // GUI thread

private:
    std::thread thread_;
    std::atomic<bool> cancel_{false}, running_{false};
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<float> lastSent_{-1.f};
    bool sync_ = false;
};

}  // namespace moritz
