// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// alloc_tests.cpp — the allocation guard over every object: every help patch, example and
// tutorial runs with device-like buffers of odd sizes (blocks shorter than 64 too, as WASAPI and
// PulseAudio give); nothing may be allocated or freed inside Patch::process.

#include "rig.h"

#include "signals/ambisonics.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace moritz;
using etest::Rig;

namespace {

std::vector<std::string> patchFiles() {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    for (const char* dir : {"help", "examples", "tutorials", "tests/cross"}) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".moritz") out.push_back(it->path().generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// ambienc~ computes its gains on the audio thread without allocating: the same as Musil's sh_gains
TEST(live_spherical_harmonics_match_musil) {
    double worst = 0;
    for (int order = 0; order <= 5; ++order)
        for (double az = -180; az <= 180; az += 17.5)
            for (double el = -90; el <= 90; el += 12.5) {
                float g[36];
                shGainsLive(order, az, el, g);
                auto ref = musil::dsp::sh_gains(order, az, el);
                for (size_t k = 0; k < ref.size(); ++k) worst = std::max(worst, std::fabs(ref[k] - (double)g[k]));
            }
    CHECK(worst < 1e-5);
}

TEST(nothing_allocates_on_the_audio_thread) {
    if (!etest::allocGuardActive()) {
        std::printf("  (skipped: the sanitizer's runtime owns operator new)\n");
        return;
    }
    bool sync = jobsSynchronous;
    jobsSynchronous = true;                   // as moritz --render: background jobs finish at once
    const int sizes[] = {64, 17, 100, 512, 1, 33, 256, 63};
    int files = 0;
    long total = 0;
    for (auto& f : patchFiles()) {
        std::string src;
        if (!readFile(f, src)) continue;
        Rig r("");
        r.p.dir = dirOf(f);
        std::string err;
        if (!r.p.load(src, err)) continue;
        r.p.loadbangAll();
        r.ch = std::clamp(r.p.maxOutputChannel(), 2, 64);
        r.run(4096);                          // first sounds, buffers read: then watch
        long before = etest::audioAllocations();
        for (int k = 0; k < 48; ++k) r.run(sizes[k % 8], sizes[k % 8]);
        r.run(22050, 300);
        long n = etest::audioAllocations() - before;
        if (n > 0) std::printf("  %s: %ld allocations or frees on the audio thread\n", f.c_str(), n);
        total += n;
        ++files;
    }
    jobsSynchronous = sync;
    std::printf("  %d patches\n", files);
    CHECK(files > 100);
    CHECK(total == 0);
}
