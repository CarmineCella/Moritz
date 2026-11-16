// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_soundtypes.cpp — sound types, after the author's SoundTypes
// (soundtypes.h): soundtypes (analysis into types and levels, shared by
// name), typegen (new sequences, offline or step by step) and typematch
// (a sound rebuilt with the types of another).

#include "objects_maple.h"
#include "offline.h"
#include "soundtypes.h"

#include <fstream>

namespace moritz {

class SoundtypesBox;
SoundtypesBox* findTypes(Patch* p, const std::string& name);

using ModelPtr = std::shared_ptr<const soundtypes::Model>;

// ---------------------------------------------------------------- soundtypes
// soundtypes name: a sound analysed into types, at one or more levels.
//   analyse sound                a buffer~: its frames (MFCCs, centroid,
//                                level) clustered into types
//   analyse book dictionary      the atoms of a pursuit book instead: the
//                                units are rebuilt from their atoms
//   types k1 [k2 k3]             types per level (12 6 4)
//   levels n                     1 to 3 (2): level 2 groups the units of
//                                level 1 into gestures, cut where the next
//                                type is surprising, then clusters them
//   threshold z                  how surprising (1: the mean plus 1 sd)
//   smooth n, size n, hop n, seed n
//   labels buffer                the type of every frame, a channel a level:
//                                plot it with "colors types"
//   transitions level buffer     the transition probabilities (a matrix)
//   play level type              start and end (ms) of one of its units
//   write file                   the units as text, level start end type
//                                (tokens for later models), info
// Outlets: progress, done, units per level (a list), start end (ms) for play.
class SoundtypesBox : public OfflineBox {
public:
    explicit SoundtypesBox(const Message& a) : name(argstr(a, 0)) {
        setPorts({P::Control}, {P::Control, P::Control, P::Control, P::Control});
    }
    ModelPtr model() const { return model_; }
    std::string name;

    void message(int, const Message& m) override {
        const Atom& h = m[0];
        if (h.is("stop")) { job_.cancel(); return; }
        if (h.is("types")) {
            opt_.k.clear();
            for (size_t i = 1; i < m.size() && i <= 3; ++i)
                if (m[i].isFloat()) opt_.k.push_back(std::clamp((int)m[i].f, 2, 64));
            if (opt_.k.empty()) opt_.k = {12, 6, 4};
        } else if (h.is("levels")) opt_.levels = std::clamp((int)argf(m, 1, 2.f), 1, 3);
        else if (h.is("threshold")) opt_.threshold = argf(m, 1, 1.f);
        else if (h.is("smooth")) opt_.smooth = std::clamp((int)argf(m, 1, 5.f), 1, 101) | 1;
        else if (h.is("seed")) opt_.seed = (unsigned)argf(m, 1, 1.f);
        else if (h.is("size")) { int n = 1; while (n < (int)argf(m, 1, 2048.f)) n <<= 1; size_ = std::clamp(n, 256, 16384); }
        else if (h.is("hop")) hop_ = std::clamp((int)argf(m, 1, 512.f), 32, 8192);
        else if (h.is("analyse") && m.size() > 1) analyse(m[1].str(), argstr(m, 2));
        else if (h.is("labels") && m.size() > 1) {
            if (!ready()) return;
            storeBuffer(patch, m[1].str(), soundtypes::typeStrips(*model_), model_->sr / model_->hop);
        } else if (h.is("transitions") && m.size() > 2) {
            if (!ready()) return;
            int L = std::clamp((int)argf(m, 1, 1.f), 1, (int)model_->levels.size()) - 1;
            storeBuffer(patch, m[2].str(), soundtypes::transitions(model_->levels[(size_t)L]), 1.0);
        } else if (h.is("play") && m.size() > 2) {
            if (!ready()) return;
            int L = std::clamp((int)argf(m, 1, 1.f), 1, (int)model_->levels.size()) - 1, t = (int)argf(m, 2, 0.f);
            std::vector<const soundtypes::Unit*> in;
            for (auto& u : model_->levels[(size_t)L].units) if (u.type == t) in.push_back(&u);
            if (in.empty()) return;
            const soundtypes::Unit* u = in[(size_t)(rng_() % in.size())];
            outlet(3, {Atom((float)(u->start * 1000.0 / model_->sr)), Atom((float)(u->end * 1000.0 / model_->sr))});
        } else if (h.is("write") && m.size() > 1) {
            if (!ready()) return;
            std::ofstream f(patch->resolvePath(m[1].str()));
            f << "; level start end type (seconds)\n";
            for (size_t L = 0; L < model_->levels.size(); ++L)
                for (auto& u : model_->levels[L].units)
                    f << L + 1 << " " << u.start / model_->sr << " " << u.end / model_->sr << " " << u.type << "\n";
            if (!f) patch->print("soundtypes " + name + ": can't write " + m[1].str());
        } else if (h.is("info")) {
            if (!ready()) return;
            std::string s = "soundtypes " + name + ":";
            for (size_t L = 0; L < model_->levels.size(); ++L)
                s += " level " + std::to_string(L + 1) + " " + std::to_string(model_->levels[L].units.size()) + " units of " +
                     std::to_string(model_->levels[L].k) + " types;";
            patch->print(s);
        }
    }

private:
    bool ready() {
        if (model_) return true;
        patch->print("soundtypes " + name + ": nothing analysed yet");
        return false;
    }
    void analyse(const std::string& src, const std::string& dictName) {
        if (busy()) return;
        BufferBox* b = findBuffer(patch, src);
        auto m = std::make_shared<soundtypes::Model>();
        m->size = size_;
        m->hop = hop_;
        if (!dictName.empty()) {                                   // a book and its dictionary
            auto book = std::make_shared<maple::Book>();
            if (!b || !loadBook(b->buf, *book)) { patch->print("soundtypes " + name + ": no book in buffer~ " + src); return; }
            auto d = findDictionary(patch, dictName);
            if (!d) { patch->print("soundtypes " + name + ": no atoms " + dictName); return; }
            m->book = book;
            m->dict = d;
            m->sr = book->sr;
        } else {
            if (!b || b->buf.frames <= 0) { patch->print("soundtypes " + name + ": no sound in buffer~ " + src); return; }
            m->sound = monoOf(b->buf);
            m->sr = b->buf.sr;
        }
        soundtypes::Options o = opt_;
        run(
            [m, o](Job& job) {
                auto tick = [&job](double p) { job.progress((float)p); return !job.cancelled(); };
                if (m->book) m->frames = soundtypes::bookFeatures(*m->book, m->hop);
                else m->frames = soundtypes::soundFeatures(m->sound, m->sr, m->size, m->hop,
                                                           [&](double p) { return tick(.6 * p); });
                if (m->frames.empty() || !soundtypes::analyse(*m, o, tick)) m->levels.clear();
            },
            [this, m] {
                if (m->levels.empty()) { patch->print("soundtypes " + name + ": the analysis found nothing"); return; }
                model_ = m;
                Message counts;
                for (auto& L : m->levels) counts.push_back(Atom((float)L.units.size()));
                outlet(2, counts);
            });
    }
    ModelPtr model_;
    soundtypes::Options opt_;
    int size_ = 2048, hop_ = 512;
    std::mt19937 rng_{7};
};

SoundtypesBox* findTypes(Patch* p, const std::string& name) {
    SoundtypesBox* found = nullptr;
    p->root()->forEachBox([&](Box* b) {
        if (auto s = dynamic_cast<SoundtypesBox*>(b))
            if (!found && s->name == name) found = s;
    });
    return found;
}

namespace {

// a result that would clip is scaled down (atoms ringing across units add up)
void limitPeak(Patch* patch, const std::string& who, const std::string& name, std::vector<float>& y) {
    float peak = 0;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    if (peak <= .99f) return;
    for (auto& v : y) v *= .99f / peak;
    char msg[128];
    std::snprintf(msg, sizeof msg, "%s: %s scaled by %.1f dB (it would clip)", who.c_str(), name.c_str(), 20 * std::log10(.99 / peak));
    patch->print(msg);
}

ModelPtr modelOf(Patch* patch, const std::string& who, const std::string& name) {
    SoundtypesBox* s = findTypes(patch, name);
    if (!s) { patch->print(who + ": no soundtypes " + name); return nullptr; }
    if (!s->model()) { patch->print(who + ": soundtypes " + name + " has nothing analysed yet"); return nullptr; }
    return s->model();
}

// ---------------------------------------------------------------- typegen
// typegen [model]: new sequences of the types of a soundtypes model.
//   classic n                 SoundTypes' n-grams (a context of n-1 types;
//                             a dead end starts again elsewhere)
//   markov [order] [count]    variable order: the longest context (up to
//                             order, 3) seen at least count times (2)
//   oracle [continuity] [context]   a factor oracle: follow the original
//                             with probability continuity (0.8), otherwise
//                             jump to a place where the last context (at
//                             least context units, 1) repeats
//   level n                   generate units of level n (1)
//   generate seconds out [labels]   writes a new sound (and its types, for
//                             plot) into buffer~s
//   next                      live: the next unit of an endless sequence:
//                             its type, then start end (ms) in the original
//   reset, model name, seed n, xfade ms (crossfades between units, 10)
// Outlets: progress, done, type (next), start end (next).
class TypegenBox : public OfflineBox {
public:
    explicit TypegenBox(const Message& a) : model_(argstr(a, 0)) {
        setPorts({P::Control}, {P::Control, P::Control, P::Control, P::Control});
    }
    void message(int, const Message& m) override {
        const Atom& h = m[0];
        if (h.is("stop")) { job_.cancel(); return; }
        if (h.is("model") && m.size() > 1) { model_ = m[1].str(); live_.clear(); }
        else if (h.is("classic")) { go_.method = soundtypes::Method::Classic; go_.order = std::clamp((int)argf(m, 1, 2.f), 1, 16); live_.clear(); }
        else if (h.is("markov")) {
            go_.method = soundtypes::Method::Markov;
            go_.order = std::clamp((int)argf(m, 1, 3.f), 1, 16);
            go_.minCount = std::max(1, (int)argf(m, 2, 2.f));
            live_.clear();
        } else if (h.is("oracle")) {
            go_.method = soundtypes::Method::Oracle;
            go_.continuity = std::clamp(argf(m, 1, .8f), 0.f, 1.f);
            go_.context = std::max(0, (int)argf(m, 2, 1.f));
            live_.clear();
        } else if (h.is("level")) { level_ = std::clamp((int)argf(m, 1, 1.f), 1, 3) - 1; live_.clear(); }
        else if (h.is("seed")) { go_.seed = (unsigned)argf(m, 1, 1.f); live_.clear(); }
        else if (h.is("xfade")) xfade_ = std::clamp(argf(m, 1, 10.f), 0.5f, 500.f);
        else if (h.is("reset")) live_.clear();
        else if (h.is("generate") && m.size() > 2) generate(argf(m, 1, 10.f), m[2].str(), argstr(m, 3));
        else if (h.is("next")) next();
    }

private:
    void generate(double seconds, const std::string& out, const std::string& labels) {
        if (busy()) return;
        ModelPtr md = modelOf(patch, "typegen", model_);
        if (!md) return;
        int level = std::min(level_, (int)md->levels.size() - 1);
        soundtypes::GenOptions go = go_;
        double xf = xfade_;
        auto y = std::make_shared<std::vector<float>>();
        auto strips = std::make_shared<std::vector<std::vector<float>>>();
        run(
            [md, level, seconds, go, xf, y, strips](Job&) {
                std::vector<int> seq = soundtypes::generate(*md, level, std::max(.1, seconds), go);
                std::vector<soundtypes::Piece> pieces;
                for (int u : seq) {
                    soundtypes::Piece p;
                    p.src0 = md->levels[(size_t)level].units[(size_t)u].start;
                    p.src1 = md->levels[(size_t)level].units[(size_t)u].end;
                    pieces.push_back(p);
                }
                *y = soundtypes::join(*md, pieces, xf);
                *strips = soundtypes::generatedStrips(*md, level, seq);
            },
            [this, md, y, strips, out, labels] {
                limitPeak(patch, "typegen", out, *y);
                storeBuffer(patch, out, {*y}, md->sr);
                if (!labels.empty()) storeBuffer(patch, labels, *strips, md->sr / md->hop);
            });
    }
    void next() {
        ModelPtr md = modelOf(patch, "typegen", model_);
        if (!md) return;
        int level = std::min(level_, (int)md->levels.size() - 1);
        if (live_.empty() || pos_ >= live_.size() || md != liveModel_) {
            soundtypes::GenOptions go = go_;
            go.seed = go_.seed + (unsigned)(++round_);
            double total = (double)md->length() / md->sr;
            live_ = soundtypes::generate(*md, level, std::max(60.0, 4 * total), go);
            liveModel_ = md;
            pos_ = 0;
        }
        if (live_.empty()) return;
        const soundtypes::Unit& u = md->levels[(size_t)level].units[(size_t)live_[pos_++]];
        outlet(3, {Atom((float)(u.start * 1000.0 / md->sr)), Atom((float)(u.end * 1000.0 / md->sr))});
        outletFloat(2, (float)u.type);
    }
    std::string model_;
    soundtypes::GenOptions go_;
    int level_ = 0;
    double xfade_ = 10;
    std::vector<int> live_;
    ModelPtr liveModel_;
    size_t pos_ = 0;
    int round_ = 0;
};

// ---------------------------------------------------------------- typematch
// typematch: SoundTypes' rematch: every unit of sound A replaced by a unit of
// the closest type of sound B (the instance closest in duration), at any
// level: units at level 1, gestures at level 2.
//   run A B out [level] [labels]   A and B are soundtypes models; out a buffer~
//   level n                   the level when run doesn't give one (1)
//   fit 1/0                   1 (default): at A's times and durations (B's
//                             sound cut or continued); 0: one after the
//                             other at B's own durations
//   xfade ms                  crossfades (10)
// Outlets: progress, done.
class TypematchBox : public OfflineBox {
public:
    explicit TypematchBox(const Message&) { setPorts({P::Control}, {P::Control, P::Control}); }
    void message(int, const Message& m) override {
        const Atom& h = m[0];
        if (h.is("stop")) { job_.cancel(); return; }
        if (h.is("fit")) fit_ = argf(m, 1, 1.f) != 0.f;
        else if (h.is("xfade")) xfade_ = std::clamp(argf(m, 1, 10.f), .5f, 500.f);
        else if (h.is("level")) level_ = std::clamp((int)argf(m, 1, 1.f), 1, 3);
        else if (h.is("run") && m.size() > 3) {
            bool hasLevel = m.size() > 4 && m[4].isFloat();
            run3(m[1].str(), m[2].str(), m[3].str(), hasLevel ? (int)m[4].f : level_, argstr(m, hasLevel ? 5 : 4));
        }
    }

private:
    void run3(const std::string& a, const std::string& b, const std::string& out, int level, const std::string& labels) {
        if (busy()) return;
        ModelPtr A = modelOf(patch, "typematch", a), B = modelOf(patch, "typematch", b);
        if (!A || !B) return;
        if (A->frames.empty() || B->frames.empty() || A->frames[0].size() != B->frames[0].size()) {
            patch->print("typematch: " + a + " and " + b + " must be analysed the same way (both sounds, or both books)");
            return;
        }
        int L = std::clamp(level, 1, (int)std::min(A->levels.size(), B->levels.size())) - 1;
        bool fit = fit_;
        double xf = xfade_;
        auto y = std::make_shared<std::vector<float>>();
        auto types = std::make_shared<std::vector<int>>();
        run(
            [A, B, L, fit, xf, y, types](Job&) {
                auto pieces = soundtypes::match(*A, *B, L, fit, types.get());
                *y = soundtypes::join(*B, pieces, xf);
            },
            [this, A, B, L, y, types, out, labels] {
                limitPeak(patch, "typematch", out, *y);
                storeBuffer(patch, out, {*y}, B->sr);
                if (!labels.empty()) {                     // B's types over A's units, for plot
                    const auto& la = A->levels[(size_t)L];
                    std::vector<float> r;
                    for (size_t i = 0; i < la.units.size() && i < types->size(); ++i)
                        for (long f = la.units[i].start / A->hop; f < (la.units[i].end + A->hop - 1) / A->hop; ++f) r.push_back((float)(*types)[i]);
                    storeBuffer(patch, labels, {r}, A->sr / A->hop);
                }
            });
    }
    bool fit_ = true;
    double xfade_ = 10;
    int level_ = 1;
};

}  // namespace

void registerSoundtypesObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"soundtypes", "Atoms and types", "a sound analysed into types and levels (SoundTypes): name", with<SoundtypesBox>()},
        {"typegen", "Atoms and types", "new sequences of sound types: classic, markov, oracle: [model]", with<TypegenBox>()},
        {"typematch", "Atoms and types", "a sound rebuilt with the types of another, at any level", with<TypematchBox>()},
    });
}

}  // namespace moritz
