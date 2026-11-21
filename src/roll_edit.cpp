// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// roll_edit.cpp — the roll's edit intents (roll_edit.h), and their target: a score document (score_doc.h).

#include "roll_edit.h"

#include "score_doc.h"

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

bool RollEditTarget::detune(const ScorePath& path, const Ids& ids, double dcents) {
    if (ids.empty() || dcents == 0 || !std::isfinite(dcents)) return false;
    return apply(path, "detune", [&](sc::Score& s) {
        bool any = false;
        for (auto* e : picked(s, ids)) {
            if (!pitched(*e)) continue;
            e->cents += dcents;
            any = true;
        }
        s.touch();
        return any;
    });
}

// ---------------------------------------------------------------- the target of a score document
namespace {
bool sameEvent(const sc::Event& a, const sc::Event& b) {
    return a.id == b.id && a.kind == b.kind && a.at == b.at && a.dur == b.dur && a.gain == b.gain && a.az == b.az && a.el == b.el &&
           a.row == b.row && a.pitch == b.pitch && a.cents == b.cents && a.dyn == b.dyn && a.tech == b.tech && a.source == b.source &&
           a.buffer == b.buffer && a.from == b.from && a.to == b.to && a.values == b.values && a.sub == b.sub && a.ref == b.ref &&
           a.stretch == b.stretch && a.transpose == b.transpose && a.variant == b.variant && a.label == b.label && a.extra == b.extra &&
           a.host == b.host;
}
bool sameRows(const std::vector<sc::Row>& a, const std::vector<sc::Row>& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k)
        if (a[k].name != b[k].name || a[k].clef != b[k].clef || a[k].extra != b[k].extra) return false;
    return true;
}
}  // namespace

ScoreDocTarget::ScoreDocTarget(std::shared_ptr<ScoreDoc> doc) : doc_(std::move(doc)) {}

ScoreDocTarget::~ScoreDocTarget() {
    if (token_) doc_->unlisten(token_);
}

void ScoreDocTarget::setListener(std::function<void(unsigned)> f) {
    if (token_) doc_->unlisten(token_);
    token_ = 0;
    listener_ = std::move(f);
    if (listener_) token_ = doc_->listen([this](unsigned what) { if (listener_) listener_(what); });
}

std::shared_ptr<const sc::Score> ScoreDocTarget::score() const {
    doc_->follow();                              // a message to the box, the box replaced or gone: seen here too
    if (!snap_ || snapVersion_ != doc_->version()) {
        snap_ = doc_->score().clone();
        snapVersion_ = doc_->version();
    }
    return snap_;
}

uint64_t ScoreDocTarget::generation() const {
    doc_->follow();
    return doc_->version();                      // an edit of a score placed inside changes its parent's too
}

std::shared_ptr<ScoreDoc> ScoreDocTarget::docAt(const ScorePath& path) const {
    std::shared_ptr<ScoreDoc> d = doc_;
    ScorePath prefix;
    for (uint64_t id : path) {
        prefix.push_back(id);
        auto e = d->score().find(id);
        if (!e || e->kind != sc::Kind::Score || !e->sub) return nullptr;
        auto& kept = subs_[prefix];
        if (!kept || kept->scorePtr() != e->sub) kept = ScoreDoc::ofPlacement(d, id);
        if (!kept) return nullptr;
        d = kept;
    }
    return d;
}

std::shared_ptr<ScoreDoc> ScoreDocTarget::focused() const {
    auto d = docAt(focus_);
    return d ? d : doc_;
}

std::vector<std::shared_ptr<ScoreDoc>> ScoreDocTarget::documents() const {
    std::vector<std::shared_ptr<ScoreDoc>> out{doc_};
    for (auto& kv : subs_)
        if (kv.second && std::find(out.begin(), out.end(), kv.second) == out.end()) out.push_back(kv.second);
    return out;
}

void ScoreDocTarget::focus(const ScorePath& path) {
    focus_ = path;
    docAt(path);
}

std::string ScoreDocTarget::title() const {
    auto f = focused();
    return f == doc_ ? doc_->title() : doc_->title() + "  ›  " + f->title();
}

Patch* ScoreDocTarget::patch() const {
    Box* b = doc_->box();
    return b ? b->patch : nullptr;
}

bool ScoreDocTarget::dirty() const {
    for (auto& d : documents())
        if (d->needsSaving()) return true;
    return false;
}

bool ScoreDocTarget::save(std::string& err) {
    auto d = focused();
    if (!d->canSaveInPlace() && d != doc_) d = doc_;     // kept inside its parent: the parent holds it
    return d->save(err);
}

bool ScoreDocTarget::sharedSelection(const ScorePath& path, Ids& out) const {
    auto d = docAt(path);
    if (!d) return false;
    out = d->selection();
    return true;
}

void ScoreDocTarget::shareSelection(const ScorePath& path, const Ids& ids) {
    if (auto d = docAt(path)) d->select(ids);
}

// (a change from outside first: the history then describes the score as it is, or starts again)
bool ScoreDocTarget::undo() {
    doc_->follow();
    auto d = focused();
    if (!d->undo()) { lastError = d->message; return false; }
    return true;
}
bool ScoreDocTarget::redo() {
    doc_->follow();
    auto d = focused();
    if (!d->redo()) { lastError = d->message; return false; }
    return true;
}
bool ScoreDocTarget::canUndo() const {
    doc_->follow();
    return focused()->canUndo();
}
bool ScoreDocTarget::canRedo() const {
    doc_->follow();
    return focused()->canRedo();
}

bool ScoreDocTarget::commitDiff(ScoreDoc& d, const sc::Score& next, const std::string& label) {
    const sc::Score& cur = d.score();
    auto ed = d.edit(label);
    if (!ed.ok()) { lastError = ed.error(); return false; }
    std::vector<uint64_t> gone;
    for (auto& e : cur.events())
        if (!next.find(e->id)) gone.push_back(e->id);
    for (uint64_t id : gone) ed.remove(id);
    for (auto& e : next.events()) {
        auto c = cur.find(e->id);
        if (!c) ed.insert(std::make_shared<sc::Event>(*e));          // its id kept: the intent's copies are known by it
        else if (!sameEvent(*c, *e)) ed.replace(*e);
    }
    if (!sameRows(cur.rows, next.rows)) ed.setRows(next.rows);
    if (cur.name != next.name || cur.bpm != next.bpm || cur.meterNum != next.meterNum || cur.meterDen != next.meterDen)
        ed.setHeader(next.name, next.bpm, next.meterNum, next.meterDen);
    if (ed.ok() && ed.empty()) return false;
    if (!ed.commit()) {
        lastError = d.message;
        return false;
    }
    return true;
}

bool ScoreDocTarget::apply(const ScorePath& path, const std::string& label, const std::function<bool(sc::Score&)>& f) {
    lastError.clear();
    doc_->follow();                              // what the box plays now is what is edited
    auto d = docAt(path);
    if (!d) { lastError = "that score is not in the score any more"; return false; }
    auto copy = d->score().clone();
    try {
        if (!f(*copy)) return false;
    } catch (const std::exception& e) {
        lastError = e.what();
        return false;
    }
    return commitDiff(*d, *copy, label);
}

bool ScoreDocTarget::replaceTop(std::shared_ptr<sc::Score> s, const std::string& label) {
    lastError.clear();
    doc_->follow();
    return commitDiff(*doc_, *s, label);
}

std::shared_ptr<ScoreDocTarget> makeRollTarget(std::shared_ptr<ScoreDoc> doc) {
    return doc ? std::make_shared<ScoreDocTarget>(std::move(doc)) : nullptr;
}

}  // namespace moritz
