// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// offline.h — what the offline objects share (objects_offline.cpp and
// objects_mir.cpp): copying channels out of a buffer~, writing results into
// one, and OfflineBox, the shape of an object that works as a background job.
#pragma once

#include "jobs.h"
#include "objects.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace moritz {

inline std::vector<float> channelOf(const SampleBuffer& b, int c) {
    std::vector<float> x((size_t)b.frames);
    for (long f = 0; f < b.frames; ++f) x[(size_t)f] = b.at(f, c);
    return x;
}
inline std::vector<float> monoOf(const SampleBuffer& b) {
    std::vector<float> x((size_t)b.frames, 0.f);
    for (long f = 0; f < b.frames; ++f) {
        float s = 0.f;
        for (int c = 0; c < b.channels; ++c) s += b.at(f, c);
        x[(size_t)f] = s / b.channels;
    }
    return x;
}

// Writes channels into the buffer~ name (GUI thread); false if there is none.
inline bool storeBuffer(Patch* patch, const std::string& name, const std::vector<std::vector<float>>& chans, double sr) {
    BufferBox* b = findBuffer(patch, name);
    if (!b) { patch->print("no buffer~ " + name); return false; }
    SampleBuffer out;
    out.channels = std::max(1, (int)chans.size());
    out.frames = chans.empty() ? 0 : (long)chans[0].size();
    out.sr = sr;
    out.data.assign((size_t)out.frames * out.channels, 0.f);
    for (int c = 0; c < (int)chans.size(); ++c)
        for (long f = 0; f < out.frames && f < (long)chans[(size_t)c].size(); ++f)
            out.data[(size_t)f * out.channels + c] = chans[(size_t)c][(size_t)f];
    b->replace(std::move(out));         // swapped in under the lock; nothing compiles again
    return true;
}

// The common shape: outlets progress and done, `stop`, a job.
class OfflineBox : public Box {
public:
    OfflineBox() {
        job_.onProgress = [this](float p) { outletFloat(0, p); };
    }

protected:
    void run(std::function<void(Job&)> work, std::function<void()> done) {
        outletFloat(0, 0.f);
        job_.start(std::move(work), [this, done = std::move(done)] {
            done();
            outletFloat(0, 1.f);
            outletBang(1);
            finished();
        });
    }
    virtual void finished() {}      // after the outlets: render starts its next render
    bool busy() {
        if (job_.running()) { patch->print(cls + ": still working (stop cancels)"); return true; }
        return false;
    }
    Job job_;
};

}  // namespace moritz
