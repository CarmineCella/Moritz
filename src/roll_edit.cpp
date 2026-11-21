// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// roll_edit.cpp — the roll's edit intents (roll_edit.h), and the target of a score box with its undo.

#include "roll_edit.h"

#include <FL/Fl.H>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace moritz {

namespace tr = musil::score::transform;

// ---------------------------------------------------------------- transformations
bool applyTransform(sc::Score& s, const Ids& ids, const ScoreTransform& t) {
    switch (t.kind) {
    case ScoreTransform::Transpose: return t.a != 0 && tr::transpose(s, t.a, ids) > 0;
    case ScoreTransform::Invert: {
        double axis = t.a;
        if (std::isnan(axis) && !tr::pitchMiddle(s, ids, axis)) return false;
        return tr::invert(s, axis, ids) > 0;
    }
    case ScoreTransform::Retrograde: return tr::retrograde(s, ids) > 0;
    case ScoreTransform::Augment: return t.a != 1 && tr::augment(s, t.a, ids) > 0;
    case ScoreTransform::Quantize: return tr::quantize(s, t.a, t.b, ids) > 0;
    case ScoreTransform::Humanize: return tr::humanize(s, t.a, t.b, t.seed, ids) > 0;
    case ScoreTransform::Thin: return !tr::thin(s, t.a, t.seed, ids).empty();
    case ScoreTransform::Density: return !tr::density(s, t.curve, t.seed, ids).empty();
    case ScoreTransform::Shift: return t.a != 0 && tr::shift(s, t.a, ids) > 0;
    }
    return false;
}

// ---------------------------------------------------------------- the intents, on apply()
namespace {
std::vector<sc::Event*> picked(sc::Score& s, const Ids& ids) {
    std::vector<sc::Event*> out;
    std::unordered_set<uint64_t> want(ids.begin(), ids.end());
    for (auto& e : s.events()) if (want.count(e->id)) out.push_back(e.get());
    return out;
}
bool pitched(const sc::Event& e) { return e.kind == sc::Kind::Note && e.variant != "chord"; }
double minDur(const sc::Event& e) { return e.kind == sc::Kind::Message ? 0.0 : 0.01; }
}  // namespace

bool RollEditTarget::moveEvents(const ScorePath& path, const Ids& ids, double dt, double dpitch) {
    if (ids.empty() || (dt == 0 && dpitch == 0)) return false;
    return apply(path, "move", [&](sc::Score& s) {
        auto evs = picked(s, ids);
        for (auto* e : evs) {
            e->at = std::max(0.0, e->at + dt);
            if (pitched(*e)) e->pitch += dpitch;
            else if (e->kind == sc::Kind::Score) e->transpose += dpitch;
        }
        s.touch();
        return !evs.empty();
    });
}

bool RollEditTarget::resizeEvents(const ScorePath& path, const Ids& ids, double ddur) {
    if (ids.empty() || ddur == 0) return false;
    return apply(path, "resize", [&](sc::Score& s) {
        bool any = false;
        for (auto* e : picked(s, ids)) {
            if (e->kind == sc::Kind::Message) continue;
            e->dur = std::max(minDur(*e), e->dur + ddur);
            any = true;
        }
        s.touch();
        return any;
    });
}

Ids RollEditTarget::copyEvents(const ScorePath& path, const Ids& ids, double dt, double dpitch) {
    Ids made;
    if (ids.empty()) return made;
    apply(path, "copy", [&](sc::Score& s) {
        std::vector<std::shared_ptr<sc::Event>> copies;
        for (auto* e : picked(s, ids)) {
            auto c = std::make_shared<sc::Event>(*e);
            c->id = 0;
            c->at = std::max(0.0, c->at + dt);
            if (pitched(*c)) c->pitch += dpitch;
            else if (c->kind == sc::Kind::Score) c->transpose += dpitch;
            copies.push_back(c);
        }
        for (auto& c : copies) made.push_back(s.insert(c));
        return !copies.empty();
    });
    return made;
}

uint64_t RollEditTarget::insertEvent(const ScorePath& path, const sc::Event& ev) {
    uint64_t id = 0;
    apply(path, "add", [&](sc::Score& s) {
        auto c = std::make_shared<sc::Event>(ev);
        c->id = 0;
        id = s.insert(c);
        return true;
    });
    return id;
}

bool RollEditTarget::removeEvents(const ScorePath& path, const Ids& ids) {
    if (ids.empty()) return false;
    return apply(path, "delete", [&](sc::Score& s) {
        bool any = false;
        for (uint64_t id : ids) any = s.remove(id) != nullptr || any;
        return any;
    });
}

bool RollEditTarget::setField(const ScorePath& path, const Ids& ids, const std::string& key, const sc::Datum& v) {
    if (ids.empty()) return false;
    bool isNum = v.kind == sc::Datum::Num;
    std::string word = v.kind == sc::Datum::Str || v.kind == sc::Datum::Sym ? v.s : isNum ? sc::numText(v.num) : std::string();
    if (word == "-") word.clear();
    double num = v.num;
    if (key == "pitch" && !isNum) {
        num = sc::pitchOfName(word);
        if (num < 0) { lastError = "not a pitch: " + word; return false; }
        isNum = true;
    }
    static const char* numeric[] = {"at", "dur", "pitch", "cents", "gain", "az", "el"};
    bool wantsNum = std::find_if(std::begin(numeric), std::end(numeric), [&](const char* k) { return key == k; }) != std::end(numeric);
    if (wantsNum && (!isNum || !std::isfinite(num))) { lastError = key + " wants a number"; return false; }
    if ((key == "at" || key == "dur") && num < 0) { lastError = key + " wants a number >= 0"; return false; }
    return apply(path, "set " + key, [&](sc::Score& s) {
        bool any = false;
        for (auto* e : picked(s, ids)) {
            if (key == "at") e->at = num;
            else if (key == "dur") { if (e->kind == sc::Kind::Message) continue; e->dur = num; }
            else if (key == "pitch") { if (!pitched(*e)) continue; e->pitch = num; }
            else if (key == "cents") { if (!pitched(*e)) continue; e->cents = num; }
            else if (key == "gain") e->gain = num;
            else if (key == "az") e->az = num;
            else if (key == "el") e->el = std::max(-90.0, std::min(90.0, num));
            else if (key == "dyn") e->dyn = word;
            else if (key == "tech") e->tech = word;
            else if (key == "row") e->row = word;
            else if (key == "label") e->label = word;
            else sc::setField(e->extra, key, v);
            any = true;
        }
        s.touch();
        return any;
    });
}

bool RollEditTarget::transform(const ScorePath& path, const Ids& ids, const ScoreTransform& t) {
    static const char* names[] = {"transpose", "invert", "retrograde", "augment", "quantize", "humanize", "thin", "density", "shift"};
    return apply(path, names[(int)t.kind], [&](sc::Score& s) { return applyTransform(s, ids, t); });
}

bool RollEditTarget::replaceScore(std::shared_ptr<sc::Score> s, const std::string& label) {
    if (!s) return false;
    return replaceTop(std::move(s), label);
}

// ---------------------------------------------------------------- the target of a score box
std::vector<ScoreBoxTarget*>& ScoreBoxTarget::all() {
    static std::vector<ScoreBoxTarget*> v;
    return v;
}

ScoreBoxTarget::ScoreBoxTarget(ScoreSource* src, bool localHistory) : src_(src), local_(localHistory) {
    all().push_back(this);
}

RollHost& rollHost() {
    static RollHost h;
    return h;
}

namespace {
struct Orphan {
    ScoreBoxTarget* target;
    std::function<Box*()> find;        // the box in its place now
};
std::vector<Orphan>& orphans() {
    static std::vector<Orphan> v;
    return v;
}
uint64_t& bumps() {
    static uint64_t n = 0;
    return n;
}
// from the event loop, after an undo or the code view replaced the box: the box that took its place
void settle(void*) {
    auto list = std::move(orphans());
    orphans().clear();
    for (auto& o : list) {
        auto& live = ScoreBoxTarget::all();
        if (std::find(live.begin(), live.end(), o.target) == live.end() || o.target->source() || !o.find) continue;
        if (auto* s = dynamic_cast<ScoreSource*>(o.find()); s && s->scoreInPatch()) o.target->rebind(s);
    }
}
}  // namespace

ScoreBoxTarget::~ScoreBoxTarget() {
    auto& v = all();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
    auto& o = orphans();
    o.erase(std::remove_if(o.begin(), o.end(), [this](const Orphan& x) { return x.target == this; }), o.end());
}

void scoreSourceClosed(ScoreSource* s, Patch* p, const std::string& id) {
    // inside the box's destructor, maybe while its patch is being rebuilt: only note it, settle from the event loop
    bool any = false;
    for (auto* t : ScoreBoxTarget::all()) {
        if (t->source() != s) continue;
        Orphan o{t, nullptr};
        if (p && rollHost().locate) o.find = rollHost().locate(p, id);
        t->unbind();
        orphans().push_back(o);
        any = true;
    }
    if (any && !Fl::has_timeout(settle)) Fl::add_timeout(0, settle);
}

std::shared_ptr<const sc::Score> ScoreBoxTarget::score() const { return src_ ? src_->scoreNow() : last_; }

uint64_t ScoreBoxTarget::generation() const { return (src_ ? src_->scoreGeneration() : 0) + gone_; }

std::string ScoreBoxTarget::title() const {
    if (!src_) return "score (its box is gone)";
    std::string f = src_->scoreFile();
    return src_->scoreTitle() + (f.empty() ? std::string() : "  (" + f + ")");
}

void ScoreBoxTarget::unbind() {
    if (!src_) return;
    last_ = src_->scoreNow();
    src_ = nullptr;
    undo_.clear();
    redo_.clear();
    gone_ += ++bumps() << 32;
}

void ScoreBoxTarget::rebind(ScoreSource* s) {
    src_ = s;
    last_.reset();
    fileDirty_ = false;
    gone_ += ++bumps() << 32;
}

bool ScoreBoxTarget::save(std::string& err) {
    if (!src_) { err = "the score's box is gone"; return false; }
    if (src_->scoreFile().empty()) { err = "the score is kept in the patch: save the patch"; return false; }
    if (!src_->saveScoreFile(err)) return false;
    fileDirty_ = false;
    return true;
}

void ScoreBoxTarget::put(std::shared_ptr<const sc::Score> s) {
    Patch* p = src_->scorePatch();
    bool inPatch = src_->scoreFile().empty();
    if (inPatch && p && rollHost().pushUndo) rollHost().pushUndo(p);
    src_->setScore(std::const_pointer_cast<sc::Score>(std::move(s)));
    if (inPatch && p && rollHost().markEdited) rollHost().markEdited(p);
    else if (!inPatch) fileDirty_ = true;
}

void ScoreBoxTarget::commit(std::shared_ptr<sc::Score> next, const std::string& label) {
    if (local_) {
        undo_.push_back({src_->scoreNow(), label});
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redo_.clear();
    }
    put(std::move(next));
}

bool ScoreBoxTarget::apply(const ScorePath& path, const std::string& label, const std::function<bool(sc::Score&)>& f) {
    lastError.clear();
    if (!editable()) { lastError = "the score's box is gone"; return false; }
    auto top = std::const_pointer_cast<sc::Score>(src_->scoreNow());
    if (!top) return false;
    auto here = tr::descend(top, path);
    if (!here) { lastError = "that score is not in the score any more"; return false; }
    // a score placed from a file (anywhere, by any of its placements) is written back by its name: its edits would be
    // lost, so it is read-only here
    std::string ref;
    const sc::Score* target = nullptr;
    std::function<void(const sc::Score&, int)> find = [&](const sc::Score& s, int depth) {
        if (depth > sc::kMaxDepth || !ref.empty()) return;
        for (auto& e : s.events()) {
            if (e->kind != sc::Kind::Score || !e->sub) continue;
            if (e->sub.get() == target && !e->ref.empty()) { ref = e->ref; return; }
            find(*e->sub, depth + 1);
        }
    };
    for (size_t k = 1; k <= path.size() && ref.empty(); ++k) {     // the score edited, and those it is inside
        auto s = tr::descend(top, ScorePath(path.begin(), path.begin() + (long)k));
        target = s.get();
        find(*top, 0);
    }
    if (!ref.empty()) { lastError = "a score placed from " + ref + ": edit it in its own file"; return false; }
    auto copy = here->clone();
    try {
        if (!f(*copy)) return false;
    } catch (const std::exception& e) {
        lastError = e.what();
        return false;
    }
    std::shared_ptr<sc::Score> next = path.empty() ? copy : tr::replaceInstance(*top, here.get(), copy);
    if (!next) { lastError = "that score is not in the score any more"; return false; }
    commit(next, label);
    return true;
}

bool ScoreBoxTarget::replaceTop(std::shared_ptr<sc::Score> s, const std::string& label) {
    lastError.clear();
    if (!editable()) { lastError = "the score's box is gone"; return false; }
    commit(std::move(s), label);
    return true;
}

bool ScoreBoxTarget::undo() {
    if (!canUndo()) return false;
    Step st = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({src_->scoreNow(), st.label});
    put(st.score);
    return true;
}

bool ScoreBoxTarget::redo() {
    if (!canRedo()) return false;
    Step st = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({src_->scoreNow(), st.label});
    put(st.score);
    return true;
}

std::shared_ptr<RollEditTarget> makeRollTarget(ScoreSource* src, bool localHistory) {
    return std::make_shared<ScoreBoxTarget>(src, localHistory);
}

}  // namespace moritz
