// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_maple.cpp — matching pursuit, after the author's Maple (maple.h):
// atoms (a named dictionary) and pursuit (decomposition, resynthesis,
// hybridisation).
//
// A book (a decomposition) is stored in a buffer~, one atom a frame, at the
// sound's sample rate, with 8 channels:
//   0 time (s, the atom's centre)   1 frequency (Hz)   2 level (dB)
//   3 dictionary atom (-1: gabor)   4 length (samples)  5 amplitude
//   6 phase                         7 start (samples)
// so plot in scatter mode draws it as an atom map: time, frequency, level.

#include "objects_maple.h"
#include "offline.h"

#include <filesystem>
#include <fstream>

namespace moritz {

class AtomsBox;
AtomsBox* findAtoms(Patch* p, const std::string& name);

namespace {

using DictPtr = std::shared_ptr<const maple::Dictionary>;

std::vector<std::string> soundFiles(const std::string& folder) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(folder, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".aif" || ext == ".aiff")
            out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

using maple::CutOptions;
using maple::cutSound;
using maple::resampleTo;

}  // namespace

// ---------------------------------------------------------------- atoms
// atoms name: a dictionary for pursuit, shared by name.
//   gabor [min max] [fmax]       Gaussian-windowed cosines, window lengths
//                                from min to max samples (powers of 2,
//                                256 and 8192), frequencies up to fmax (16000)
//   gammatone [min max] [fmax]   the same with a gammatone envelope (sharp
//                                attack, slow decay): good for attacks
//   frames folder [ms] [hop]     every sound file of a folder cut into
//                                Hann-windowed frames (100 ms, hop 100)
//   onsets folder [threshold] [max ms]   cut at the onsets (2; 1000 ms at most)
//   files folder [max ms]        every file whole (2000 ms at most)
//   add buffer [frames ms hop | onsets threshold | whole]   from a buffer~
//   get k buffer                 writes atom k into a buffer~ (to listen)
//   clear, write file, read file, info
// Outlets: progress, bang when ready, number of atoms.
class AtomsBox : public OfflineBox {
public:
    explicit AtomsBox(const Message& a) : name(argstr(a, 0)) {
        setPorts({P::Control}, {P::Control, P::Control, P::Control});
        dict_ = std::make_shared<maple::Dictionary>();
    }
    DictPtr dict() const { return dict_; }
    std::string name;

    std::vector<std::string> menuItems() override { return {"Info", "Save the dictionary..."}; }
    void menuAction(int k) override {
        if (k == 0) { message(0, {Atom("info")}); return; }
        std::string f = chooseFile(true, "Save atoms " + name, "Moritz atoms\t*.atoms", name + ".atoms");
        if (!f.empty() && !write(f)) patch->print("atoms " + name + ": can't write " + f);   // the user chose it: written there
    }
    void message(int, const Message& m) override {
        const Atom& h = m[0];
        if (h.is("stop")) { job_.cancel(); return; }
        if (h.is("gabor") || h.is("gammatone")) {
            double sr = patch->root()->sampleRate.load() > 0 ? patch->root()->sampleRate.load() : 44100;
            dict_ = std::make_shared<maple::Dictionary>(maple::parametric(h.is("gammatone"), sr, (int)argf(m, 1, 256.f),
                                                                          (int)argf(m, 2, 8192.f), argf(m, 3, 16000.f)));
            ready();
        } else if ((h.is("frames") || h.is("onsets") || h.is("files")) && m.size() > 1) {
            CutOptions c;
            c.mode = h.is("files") ? "whole" : h.s;
            if (h.is("frames")) { c.ms = argf(m, 2, 100.f); c.hop = argf(m, 3, (float)c.ms); }
            if (h.is("onsets")) { c.threshold = argf(m, 2, 2.f); c.maxMs = argf(m, 3, 1000.f); }
            if (h.is("files")) c.maxMs = argf(m, 2, 2000.f);
            build(patch->resolvePath(m[1].str()), c, false);
        } else if (h.is("add") && m.size() > 1) {
            CutOptions c;
            c.mode = m.size() > 2 ? m[2].str() : "whole";
            if (c.mode == "frames") { c.ms = argf(m, 3, 100.f); c.hop = argf(m, 4, (float)c.ms); }
            if (c.mode == "onsets") { c.threshold = argf(m, 3, 2.f); c.maxMs = argf(m, 4, 1000.f); }
            addBuffer(m[1].str(), c);
        } else if (h.is("clear")) {
            dict_ = std::make_shared<maple::Dictionary>();
            ready();
        } else if (h.is("get") && m.size() > 2) {
            int k = (int)argf(m, 1, 0.f);
            if (dict_->parametric() || k < 0 || k >= (int)dict_->atoms.size()) { patch->print("atoms " + name + ": no atom " + std::to_string(k)); return; }
            storeBuffer(patch, m[2].str(), {dict_->atoms[(size_t)k].x}, dict_->sr);
        } else if (h.is("info")) {
            std::string s = "atoms " + name + ": " + dict_->kind;
            if (dict_->parametric()) s += ", windows " + std::to_string(dict_->minScale) + " to " + std::to_string(dict_->maxScale) + " samples";
            else s += ", " + std::to_string(dict_->atoms.size()) + " atoms";
            patch->print(s);
        } else if (h.is("write") && m.size() > 1) {
            std::string path = patch->resolvePath(m[1].str()), why;
            if (!patchMayWrite(patch, path, why)) patch->print("atoms " + name + ": " + why);   // only where a patch may write
            else if (!write(path)) patch->print("atoms " + name + ": can't write " + m[1].str());
        } else if (h.is("read") && m.size() > 1) {
            read(patch->resolvePath(m[1].str()));
        }
    }

private:
    void ready() {
        outletFloat(2, (float)dict_->size());
        outletBang(1);
    }
    void build(const std::string& folder, const CutOptions& c, bool append) {
        if (busy()) return;
        auto files = soundFiles(folder);
        if (files.empty()) { patch->print("atoms " + name + ": no sound files in " + folder); return; }
        double sr = dict_->kind == "sounds" && append ? dict_->sr : 44100;
        auto out = std::make_shared<maple::Dictionary>();
        if (append) *out = *dict_;
        out->kind = "sounds";
        out->sr = sr;
        run(
            [files, c, sr, out](Job& job) {
                maple::MelDescriptor desc(sr);
                for (size_t i = 0; i < files.size(); ++i) {
                    SampleBuffer b;
                    std::string err;
                    if (!decodeAudioFile(files[i], b, err)) continue;
                    std::vector<float> x = resampleTo(monoOf(b), b.sr, sr);
                    cutSound(x, sr, c, std::filesystem::path(files[i]).filename().string(), desc, out->atoms);
                    job.progress((float)(i + 1) / files.size());
                    if (job.cancelled()) return;
                }
            },
            [this, out] {
                dict_ = out;
                if (dict_->atoms.empty()) patch->print("atoms " + name + ": no atoms (sounds too short or silent?)");
                outletFloat(2, (float)dict_->size());
            });
    }
    void addBuffer(const std::string& bname, const CutOptions& c) {
        BufferBox* b = findBuffer(patch, bname);
        if (!b || b->buf.frames <= 0) { patch->print("atoms " + name + ": no sound in buffer~ " + bname); return; }
        if (busy()) return;
        double sr = dict_->kind == "sounds" ? dict_->sr : b->buf.sr;
        auto x = std::make_shared<std::vector<float>>(resampleTo(monoOf(b->buf), b->buf.sr, sr));
        auto out = std::make_shared<maple::Dictionary>();
        if (dict_->kind == "sounds") *out = *dict_;
        out->kind = "sounds";
        out->sr = sr;
        run(
            [x, c, sr, out, bname](Job&) {
                maple::MelDescriptor desc(sr);
                cutSound(*x, sr, c, bname, desc, out->atoms);
            },
            [this, out] { dict_ = out; outletFloat(2, (float)dict_->size()); });
    }
    bool write(const std::string& path) const { return maple::saveDictionary(*dict_, path); }
    void read(const std::string& path) {
        if (busy()) return;
        auto out = std::make_shared<maple::Dictionary>();
        run(
            [path, out](Job& job) {
                auto tick = [&job](double p) { job.progress((float)p); return !job.cancelled(); };
                if (!maple::loadDictionary(path, *out, tick)) out->kind = "bad";
            },
            [this, out, path] {
                if (out->kind == "bad") { patch->print("atoms " + name + ": " + path + " is not a dictionary"); return; }
                dict_ = out;
                outletFloat(2, (float)dict_->size());
            });
    }
    std::shared_ptr<const maple::Dictionary> dict_;
};

AtomsBox* findAtoms(Patch* p, const std::string& name) {
    AtomsBox* found = nullptr;
    p->root()->forEachBox([&](Box* b) {
        if (auto a = dynamic_cast<AtomsBox*>(b))
            if (!found && a->name == name) found = a;
    });
    return found;
}

namespace {

// a book to and from a buffer~
void storeBook(Patch* patch, const std::string& name, const maple::Book& book, const maple::Dictionary& d) {
    std::vector<std::vector<float>> ch;
    for (auto& row : maple::bookRows(book, d)) ch.emplace_back(row.begin(), row.end());
    storeBuffer(patch, name, ch, book.sr);
}
bool readBook(const SampleBuffer& b, maple::Book& book) {
    if (b.channels < 8) return false;
    std::vector<std::vector<double>> ch(8, std::vector<double>((size_t)b.frames));
    for (long f = 0; f < b.frames; ++f)
        for (int c = 0; c < 8; ++c) ch[(size_t)c][(size_t)f] = b.data[(size_t)f * b.channels + c];
    book = maple::bookFromRows(ch, b.sr);
    return true;
}

// ---------------------------------------------------------------- pursuit
// pursuit: matching pursuit, after Maple, as a background job.
//   run sound dictionary book [residual]   decomposes a buffer~ into atoms of
//                     a dictionary (an atoms object): the book goes to a
//                     buffer~ (see above), the residual (what is left) too
//   synth book dictionary out   rebuilds a sound from a book (ratio,
//                     stretch, tails, keep apply)
//   cross book from to out      rebuilds a book made with dictionary `from`
//                     with the closest atoms (mel spectra) of dictionary `to`
// Gabor and gammatone: atoms n (most, 2000), snr dB (stop when the residual
// is that far under the sound, 30). Sounds: components n (atoms a segment,
// 4), candidates n (16), segments onsets | blocks, block ms (100: also the
// longest segment), threshold x (onsets, 2). Synthesis: ratio r
// (transposition), stretch s (time), tails 1/0 (sound atoms ring whole, or
// stop where they matched), keep f (the share of the atoms, strongest
// first); a result that would clip is scaled down. Outlets: progress, done,
// atoms found, signal-to-residual (dB).
class PursuitBox : public OfflineBox {
public:
    explicit PursuitBox(const Message&) { setPorts({P::Control}, {P::Control, P::Control, P::Control, P::Control}); }
    void message(int, const Message& m) override {
        const Atom& h = m[0];
        if (h.is("stop")) { job_.cancel(); return; }
        if (h.is("atoms")) maxAtoms_ = std::max(1, (int)argf(m, 1, 2000.f));
        else if (h.is("snr")) snr_ = argf(m, 1, 30.f);
        else if (h.is("components")) so_.components = std::clamp((int)argf(m, 1, 4.f), 1, 64);
        else if (h.is("candidates")) so_.candidates = std::clamp((int)argf(m, 1, 16.f), 1, 100000);
        else if (h.is("segments")) so_.onsets = !(m.size() > 1 && m[1].is("blocks"));
        else if (h.is("block")) so_.block = std::max(.005f, argf(m, 1, 100.f) / 1000.f);
        else if (h.is("threshold")) so_.threshold = std::max(0.f, argf(m, 1, 2.f));
        else if (h.is("ratio")) syn_.ratio = std::clamp(argf(m, 1, 1.f), .05f, 20.f);
        else if (h.is("stretch")) syn_.stretch = std::clamp(argf(m, 1, 1.f), .05f, 50.f);
        else if (h.is("tails")) syn_.tails = argf(m, 1, 1.f) != 0.f;
        else if (h.is("keep")) syn_.keep = std::clamp(argf(m, 1, 1.f), 0.f, 1.f);
        else if (h.is("run") && m.size() > 3) decompose(m[1].str(), m[2].str(), m[3].str(), argstr(m, 4));
        else if (h.is("synth") && m.size() > 3) synth(m[1].str(), m[2].str(), "", m[3].str());
        else if (h.is("cross") && m.size() > 4) synth(m[1].str(), m[2].str(), m[3].str(), m[4].str());
    }

private:
    DictPtr dictionary(const std::string& name) {
        AtomsBox* a = findAtoms(patch, name);
        if (!a) { patch->print("pursuit: no atoms " + name); return nullptr; }
        DictPtr d = a->dict();
        if (d->kind == "empty" || (!d->parametric() && d->atoms.empty())) { patch->print("pursuit: atoms " + name + " is empty"); return nullptr; }
        return d;
    }
    void decompose(const std::string& src, const std::string& dname, const std::string& bookName, const std::string& resName) {
        if (busy()) return;
        BufferBox* b = findBuffer(patch, src);
        if (!b || b->buf.frames <= 0) { patch->print("pursuit: no sound in buffer~ " + src); return; }
        DictPtr d = dictionary(dname);
        if (!d) return;
        double sr = b->buf.sr;
        auto x = std::make_shared<std::vector<float>>(monoOf(b->buf));
        auto book = std::make_shared<maple::Book>();
        auto res = std::make_shared<std::vector<float>>();
        int maxAtoms = maxAtoms_;
        double snr = snr_;
        maple::SoundPursuitOptions so = so_;
        run(
            [x, d, sr, book, res, maxAtoms, snr, so](Job& job) {
                auto tick = [&job](double p) { job.progress((float)p); return !job.cancelled(); };
                maple::decompose(*d, *x, sr, maxAtoms, snr, so, *book, *res, tick);
            },
            [this, d, book, res, bookName, resName] {
                storeBook(patch, bookName, *book, *d);
                if (!resName.empty() && !res->empty()) storeBuffer(patch, resName, {*res}, book->sr);
                outletFloat(3, (float)maple::snrOf(*book));
                outletFloat(2, (float)book->atoms.size());
            });
    }
    void synth(const std::string& bookName, const std::string& dname, const std::string& toName, const std::string& out) {
        if (busy()) return;
        BufferBox* bb = findBuffer(patch, bookName);
        auto book = std::make_shared<maple::Book>();
        if (!bb || !readBook(bb->buf, *book)) { patch->print("pursuit: no book in buffer~ " + bookName); return; }
        DictPtr d = dictionary(dname);
        if (!d) return;
        DictPtr to = d;
        if (!toName.empty()) {
            to = dictionary(toName);
            if (!to) return;
            if (d->parametric() || to->parametric()) { patch->print("pursuit: cross needs two dictionaries of sounds"); return; }
        }
        maple::SynthOptions o = syn_;
        auto y = std::make_shared<std::vector<float>>();
        run(
            [book, d, to, o, y](Job&) {
                if (to != d) *book = maple::crossBook(*book, *d, *to);   // each atom by the closest of the other dictionary
                *y = maple::synthesize(*book, *to, o);
            },
            [this, y, book, out] {
                float peak = 0;
                for (float v : *y) peak = std::max(peak, std::fabs(v));
                if (peak > .99f) {                                // overlapping atoms: no clipping
                    for (auto& v : *y) v *= .99f / peak;
                    char msg[96];
                    std::snprintf(msg, sizeof msg, "pursuit: %s scaled by %.1f dB (it would clip)", out.c_str(), 20 * std::log10(.99 / peak));
                    patch->print(msg);
                }
                storeBuffer(patch, out, {*y}, book->sr);
            });
    }
    int maxAtoms_ = 2000;
    double snr_ = 30;
    maple::SoundPursuitOptions so_;
    maple::SynthOptions syn_;
};

}  // namespace

std::shared_ptr<const maple::Dictionary> findDictionary(Patch* p, const std::string& name) {
    AtomsBox* a = findAtoms(p, name);
    return a ? a->dict() : nullptr;
}
bool loadBook(const SampleBuffer& b, maple::Book& book) { return readBook(b, book); }

void registerMapleObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"atoms", "Atoms and types", "a dictionary for pursuit: gabor, gammatone, or sounds cut from files: name", with<AtomsBox>()},
        {"pursuit", "Atoms and types", "matching pursuit: decompose a buffer~ into atoms, resynthesise, hybridise", with<PursuitBox>()},
    });
}

}  // namespace moritz
