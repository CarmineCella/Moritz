// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// jobs.cpp — the worker threads of background jobs and the queue back to the GUI

#include "jobs.h"

#include <cmath>
#include <deque>
#include <mutex>

namespace moritz {

std::function<void()> mainWake;
bool jobsSynchronous = false;

namespace {
std::mutex queueMutex;
std::deque<std::function<void()>> queue;
thread_local bool inJob = false;      // jobs started from inside a job run at once
}  // namespace

void postToMain(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(queueMutex);
        queue.push_back(std::move(fn));
    }
    if (mainWake) mainWake();
}

void drainMain() {
    std::deque<std::function<void()>> q;
    {
        std::lock_guard<std::mutex> lk(queueMutex);
        q.swap(queue);
    }
    for (auto& fn : q) fn();
}

void Job::start(std::function<void(Job&)> work, std::function<void()> done) {
    cancel();
    cancel_ = false;
    lastSent_ = -1.f;
    sync_ = jobsSynchronous || inJob;
    if (sync_) {
        running_ = true;
        work(*this);
        running_ = false;
        if (!cancel_) done();
        return;
    }
    running_ = true;
    auto alive = alive_;
    thread_ = std::thread([this, work = std::move(work), done = std::move(done), alive]() mutable {
        inJob = true;
        work(*this);
        bool ok = !cancel_.load();
        running_ = false;
        if (ok)
            postToMain([alive, done = std::move(done)] {
                if (alive->load()) done();
            });
    });
}

void Job::cancel() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    // a done() already queued belongs to the job that just ended: drop it
    alive_->store(false);
    alive_ = std::make_shared<std::atomic<bool>>(true);
}

void Job::progress(float p) {
    p = std::fmin(1.f, std::fmax(0.f, p));
    if (std::fabs(p - lastSent_.load()) < 0.01f && p < 1.f) return;   // about a hundred updates at most
    lastSent_ = p;
    if (sync_) {
        if (onProgress) onProgress(p);
        return;
    }
    auto alive = alive_;
    postToMain([this, alive, p] {
        if (alive->load() && onProgress) onProgress(p);
    });
}

}  // namespace moritz
