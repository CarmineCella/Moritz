// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_doc.cpp — a score being edited (score_doc.h): where it lives, its edits and their undo, following the box
// it is bound to, saving, playing it, rendering it.

#include "score_doc.h"
#include "transport.h"

#include "music/score_text.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>

namespace moritz {

std::function<void(Patch*)> ScoreDoc::patchEdited;
size_t ScoreDoc::undoBudget = (size_t)64 << 20;
size_t ScoreDoc::maxSteps = 1000;

namespace {

std::string baseOf(const std::string& path) {
    size_t k = path.find_last_of("/\\");
    return k == std::string::npos ? path : path.substr(k + 1);
}

size_t datumBytes(const sc::Datum& d) {
    size_t n = sizeof(sc::Datum) + d.s.capacity();
    for (auto& x : d.list) n += datumBytes(x);
    return n;
}
size_t fieldsBytes(const sc::Fields& f) {
    size_t n = 0;
    for (auto& kv : f) n += kv.first.capacity() + datumBytes(kv.second);
    return n;
}
// what an event keeps (a placed score is shared: not counted)
size_t eventBytes(const sc::Event* e) {
    if (!e) return 0;
    size_t n = sizeof(sc::Event) + e->row.capacity() + e->dyn.capacity() + e->tech.capacity() + e->source.capacity() +
               e->ref.capacity() + e->variant.capacity() + e->label.capacity() + fieldsBytes(e->extra);
    for (auto& v : e->values) n += datumBytes(v);
    return n;
}
size_t rowsBytes(const std::vector<sc::Row>& rows) {
    size_t n = 0;
    for (auto& r : rows) n += sizeof(sc::Row) + r.name.capacity() + r.clef.capacity() + fieldsBytes(r.extra);
    return n;
}

}  // namespace

size_t ScoreDoc::Edit::Op::bytes() const {
    return sizeof(Op) + eventBytes(ev.get()) + eventBytes(before.get()) + eventBytes(after.get()) + rowsBytes(rowsBefore) +
           rowsBytes(rowsAfter) + nameBefore.capacity() + nameAfter.capacity();
}

// ---------------------------------------------------------------- opening

std::vector<std::weak_ptr<ScoreDoc>>& ScoreDoc::registry() {
    static std::vector<std::weak_ptr<ScoreDoc>> r;
    r.erase(std::remove_if(r.begin(), r.end(), [](const std::weak_ptr<ScoreDoc>& w) { return w.expired(); }), r.end());
    return r;
}

ScoreDoc::ScoreDoc(std::shared_ptr<sc::Score> s, Where w) : score_(std::move(s)), where_(w) {
    seen_ = score_->version();
}

ScoreDoc::~ScoreDoc() {
    if (player_ && playerRoot_) playerRoot_->removeExtra(player_);
}

std::shared_ptr<ScoreDoc> ScoreDoc::find(const sc::Score* s) {
    for (auto& w : registry())
        if (auto d = w.lock(); d && d->score_.get() == s) return d;
    return nullptr;
}

std::shared_ptr<ScoreDoc> ScoreDoc::make(const std::string& name) {
    std::shared_ptr<ScoreDoc> d(new ScoreDoc(std::make_shared<sc::Score>(name, 44100), Where::New));
    registry().push_back(d);
    return d;
}

std::shared_ptr<ScoreDoc> ScoreDoc::open(const std::string& given, std::string& err) {
    std::string path = sc::normalPath(given);
    for (auto& w : registry())
        if (auto d = w.lock(); d && !d->path().empty() && sc::normalPath(d->path()) == path) return d;
    sc::ReadContext ctx;
    // the scores it places that are open already are those documents' scores: one score, edited in one place
    for (auto& w : registry())
        if (auto d = w.lock(); d && !d->path().empty()) ctx.loaded[sc::normalPath(d->path())] = d->score_;
    std::shared_ptr<sc::Score> s;
    try {
        s = sc::readFile(path, std::move(ctx));
    } catch (const std::exception& e) {
        err = e.what();
        return nullptr;
    }
    std::shared_ptr<ScoreDoc> d(new ScoreDoc(s, Where::File));
    d->path_ = path;
    registry().push_back(d);
    return d;
}

std::shared_ptr<ScoreDoc> ScoreDoc::ofBox(Box* box) {
    if (!box || !isScoreBox(box) || !box->patch) return nullptr;
    for (auto& w : registry())
        if (auto d = w.lock(); d && d->where_ == Where::Box && d->locate() == box) return d;
    auto s = scoreOfBox(box);
    std::shared_ptr<ScoreDoc> d(new ScoreDoc(s, Where::Box));
    Patch* p = box->patch;
    d->boxPath_.push_back(box->id);
    while (p->owner && p->parent) {
        d->boxPath_.insert(d->boxPath_.begin(), p->owner->id);
        p = p->parent;
    }
    d->top_ = p;
    d->root_ = p->root();
    d->box_ = box;
    d->hadBox_ = true;
    registry().push_back(d);
    return d;
}

std::shared_ptr<ScoreDoc> ScoreDoc::ofPlacement(const std::shared_ptr<ScoreDoc>& parent, uint64_t id) {
    if (!parent) return nullptr;
    auto e = parent->score_->find(id);
    if (!e || e->kind != sc::Kind::Score || !e->sub) return nullptr;
    if (auto d = find(e->sub.get())) return d;
    std::shared_ptr<ScoreDoc> d(new ScoreDoc(e->sub, Where::Sub));
    d->parent_ = parent;
    d->placement_ = id;
    registry().push_back(d);
    return d;
}

// ---------------------------------------------------------------- what it is

Box* ScoreDoc::locate() const {
    if (!root_ || !top_ || boxPath_.empty()) return nullptr;
    bool alive = top_ == root_;
    for (size_t k = 0; !alive && k < root_->extras.size(); ++k) alive = root_->extras[k].get() == top_;
    if (!alive) return nullptr;                  // the document holding it was closed: never followed
    Patch* q = top_;
    Box* b = nullptr;
    for (size_t k = 0; k < boxPath_.size(); ++k) {
        b = q->find(boxPath_[k]);
        if (!b) return nullptr;
        if (k + 1 < boxPath_.size() && !(q = b->inner())) return nullptr;
    }
    return b && isScoreBox(b) ? b : nullptr;
}

Box* ScoreDoc::box() const {
    if (where_ != Where::Box) return nullptr;
    box_ = locate();
    return box_;
}

std::string ScoreDoc::path() const {
    switch (where_) {
    case Where::File:
    case Where::New: return path_;
    case Where::Box: {
        Box* b = box();
        return b && !scoreBoxKeptInPatch(b) ? score_->path : std::string();
    }
    case Where::Sub: {
        auto e = parent_ ? parent_->score_->find(placement_) : nullptr;
        return e && !e->ref.empty() && e->sub == score_ ? score_->path : std::string();
    }
    }
    return "";
}

std::string ScoreDoc::folder() const {
    std::string p = path();
    if (!p.empty()) return sc::folderOf(p);
    if (where_ == Where::Box)
        if (Box* b = box()) return dirOf(b->patch->resolvePath("x"));
    if (where_ == Where::Sub && parent_) return parent_->folder();
    return "";
}

std::vector<std::shared_ptr<ScoreDoc>> ScoreDoc::chain() {
    std::vector<std::shared_ptr<ScoreDoc>> c;
    for (auto d = shared_from_this(); d && c.size() < 64; d = d->parent_) c.insert(c.begin(), d);
    return c;
}

std::string ScoreDoc::title() const {
    std::string t;
    switch (where_) {
    case Where::File:
    case Where::New: t = path_.empty() ? std::string("Untitled score") : baseOf(path_); break;
    case Where::Box: {
        Box* b = box();
        t = "score " + (score_->name.empty() ? std::string("") : score_->name);
        if (!b) t += " (not in any open patch now)";
        else if (!scoreBoxKeptInPatch(b) && !score_->path.empty()) t += " (" + baseOf(score_->path) + ")";
        break;
    }
    case Where::Sub: {
        std::string p = path();
        t = !p.empty() ? baseOf(p) : (score_->name.empty() ? std::string("score") : score_->name);
        if (placedGone_) t += " (not placed any more)";
        break;
    }
    }
    if (needsSaving()) t += " — edited";
    return t;
}

// ---------------------------------------------------------------- edits

ScoreDoc::Edit ScoreDoc::edit(const std::string& name) { return Edit(this, name); }

ScoreDoc::Edit::Edit(ScoreDoc* d, std::string name) : doc_(d), name_(std::move(name)) {
    if (d->editing_) {
        error_ = "another edit is open";
        doc_ = nullptr;
    } else {
        d->editing_ = true;
    }
}

ScoreDoc::Edit::Edit(Edit&& o) noexcept
    : doc_(o.doc_), name_(std::move(o.name_)), error_(std::move(o.error_)), ops_(std::move(o.ops_)), done_(o.done_) {
    o.doc_ = nullptr;
    o.done_ = true;
}

ScoreDoc::Edit::~Edit() {
    if (!doc_) return;
    if (!done_)
        for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {
            try { doc_->apply(*it, false); } catch (const std::exception&) {}
        }
    doc_->editing_ = false;
}

uint64_t ScoreDoc::Edit::insert(std::shared_ptr<sc::Event> e) {
    if (!doc_ || !ok()) return 0;
    if (!e) { error_ = "no event"; return 0; }
    Op op;
    op.kind = Op::Insert;
    op.ev = std::move(e);
    op.index = doc_->score_->size();
    try { doc_->apply(op, true); } catch (const std::exception& x) { error_ = x.what(); return 0; }
    ops_.push_back(std::move(op));
    return ops_.back().id;
}

bool ScoreDoc::Edit::remove(uint64_t id) {
    if (!doc_ || !ok()) return false;
    Op op;
    op.kind = Op::Remove;
    op.id = id;
    try { doc_->apply(op, true); } catch (const std::exception& x) { error_ = x.what(); return false; }
    ops_.push_back(std::move(op));
    return true;
}

bool ScoreDoc::Edit::replace(const sc::Event& now) {
    if (!doc_ || !ok()) return false;
    Op op;
    op.kind = Op::Replace;
    op.id = now.id;
    op.after = std::make_shared<sc::Event>(now);
    try { doc_->apply(op, true); } catch (const std::exception& x) { error_ = x.what(); return false; }
    ops_.push_back(std::move(op));
    return true;
}

void ScoreDoc::Edit::setRows(std::vector<sc::Row> rows) {
    if (!doc_ || !ok()) return;
    Op op;
    op.kind = Op::Rows;
    op.rowsBefore = doc_->score_->rows;
    op.rowsAfter = std::move(rows);
    doc_->apply(op, true);
    ops_.push_back(std::move(op));
}

void ScoreDoc::Edit::setHeader(const std::string& name, double bpm, int num, int den) {
    if (!doc_ || !ok()) return;
    if (!(bpm > 0) || !std::isfinite(bpm) || num < 1 || den < 1) { error_ = "a tempo above 0 and a meter of at least 1/1"; return; }
    const sc::Score& s = *doc_->score_;
    Op op;
    op.kind = Op::Header;
    op.nameBefore = s.name;
    op.bpmBefore = s.bpm;
    op.numBefore = s.meterNum;
    op.denBefore = s.meterDen;
    op.nameAfter = name;
    op.bpmAfter = bpm;
    op.numAfter = num;
    op.denAfter = den;
    doc_->apply(op, true);
    ops_.push_back(std::move(op));
}

bool ScoreDoc::Edit::commit() {
    if (!doc_) {
        return false;
    }
    ScoreDoc* d = doc_;
    if (!ok() || ops_.empty()) {
        d->message = ok() ? "nothing to do" : error_;
        for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {   // taken back now
            try { d->apply(*it, false); } catch (const std::exception&) {}
        }
        ops_.clear();
        done_ = true;
        d->editing_ = false;
        doc_ = nullptr;
        return false;
    }
    Step st;
    st.name = name_;
    st.serial = d->nextSerial_++;
    for (auto& op : ops_) st.bytes += op.bytes();
    st.ops = std::move(ops_);
    ops_.clear();
    done_ = true;
    d->editing_ = false;
    doc_ = nullptr;
    d->undo_.push_back(std::move(st));
    d->redo_.clear();
    d->trimHistory();
    d->message = name_;
    d->changed(Content | State);
    return true;
}

void ScoreDoc::apply(Edit::Op& op, bool forward) {
    sc::Score& s = *score_;
    using Op = Edit::Op;
    switch (op.kind) {
    case Op::Insert:
        if (forward) op.id = s.insertAt(op.index, op.ev);
        else if (!s.remove(op.id)) throw sc::Error("score: the event to take back is gone");
        break;
    case Op::Remove:
        if (forward) {
            auto e = s.remove(op.id, &op.index);
            if (!e) throw sc::Error("score: no event " + std::to_string(op.id));
            op.ev = e;
        } else {
            s.insertAt(op.index, op.ev);
        }
        break;
    case Op::Replace:
        if (forward) {
            auto old = s.replace(*op.after);
            if (!old) throw sc::Error("score: no event " + std::to_string(op.id));
            if (!op.before) op.before = old;
        } else {
            s.replace(*op.before);
        }
        break;
    case Op::Rows:
        s.rows = forward ? op.rowsAfter : op.rowsBefore;
        s.touch();
        break;
    case Op::Header:
        s.name = forward ? op.nameAfter : op.nameBefore;
        s.bpm = forward ? op.bpmAfter : op.bpmBefore;
        s.meterNum = forward ? op.numAfter : op.numBefore;
        s.meterDen = forward ? op.denAfter : op.denBefore;
        s.touch();
        break;
    }
}

bool ScoreDoc::undo() {
    if (undo_.empty() || editing_) { message = "nothing to undo"; return false; }
    Step st = std::move(undo_.back());
    undo_.pop_back();
    try {
        for (auto it = st.ops.rbegin(); it != st.ops.rend(); ++it) apply(*it, false);
    } catch (const std::exception& e) {          // never, as the history describes this score: start again
        message = std::string("undo: ") + e.what();
        undo_.clear();
        redo_.clear();
        changed(Content | State);
        return false;
    }
    message = "undo " + st.name;
    redo_.push_back(std::move(st));
    changed(Content | State);
    return true;
}

bool ScoreDoc::redo() {
    if (redo_.empty() || editing_) { message = "nothing to redo"; return false; }
    Step st = std::move(redo_.back());
    redo_.pop_back();
    try {
        for (auto& op : st.ops) apply(op, true);
    } catch (const std::exception& e) {
        message = std::string("redo: ") + e.what();
        undo_.clear();
        redo_.clear();
        changed(Content | State);
        return false;
    }
    message = "redo " + st.name;
    undo_.push_back(std::move(st));
    changed(Content | State);
    return true;
}

std::string ScoreDoc::undoName() const { return undo_.empty() ? "" : undo_.back().name; }
std::string ScoreDoc::redoName() const { return redo_.empty() ? "" : redo_.back().name; }

size_t ScoreDoc::historyBytes() const {
    size_t n = 0;
    for (auto& s : undo_) n += s.bytes;
    for (auto& s : redo_) n += s.bytes;
    return n;
}

void ScoreDoc::trimHistory() {
    size_t total = historyBytes();
    while (undo_.size() > 1 && (total > undoBudget || undo_.size() + redo_.size() > maxSteps)) {
        total -= undo_.front().bytes;
        if (savedSerial_ == undo_.front().serial || savedSerial_ == 0) savedSerial_ = ~uint64_t(0);   // gone: unsaved
        undo_.erase(undo_.begin());
    }
}

// ---------------------------------------------------------------- the selection

bool ScoreDoc::isSelected(uint64_t id) const { return std::find(sel_.begin(), sel_.end(), id) != sel_.end(); }

void ScoreDoc::select(std::vector<uint64_t> ids) {
    std::vector<uint64_t> keep;
    for (uint64_t id : ids)
        if (score_->find(id) && std::find(keep.begin(), keep.end(), id) == keep.end()) keep.push_back(id);
    if (keep == sel_) return;
    sel_ = std::move(keep);
    notify(Selection);
}

void ScoreDoc::toggle(uint64_t id) {
    std::vector<uint64_t> s = sel_;
    auto it = std::find(s.begin(), s.end(), id);
    if (it != s.end()) s.erase(it);
    else s.push_back(id);
    select(std::move(s));
}

// ---------------------------------------------------------------- telling and playing

void ScoreDoc::changed(unsigned what) {
    size_t before = sel_.size();
    sel_.erase(std::remove_if(sel_.begin(), sel_.end(), [this](uint64_t id) { return !score_->find(id); }), sel_.end());
    if (sel_.size() != before) what |= Selection;
    push();
    ++version_;
    notify(what);
}

void ScoreDoc::push() {
    if (where_ == Where::Box)
        if (Box* b = box()) {
            replaceScoreOfBox(b, score_);
            if (scoreBoxKeptInPatch(b) && patchEdited) patchEdited(b->patch);
        }
    if (player_)
        if (Box* p = player_->find("s")) replaceScoreOfBox(p, score_);
    seen_ = score_->version();
    if (where_ == Where::Sub && parent_ && !placedGone_) {
        auto e = parent_->score_->find(placement_);
        parent_->childChanged(e && e->ref.empty());   // embedded: the parent's text holds it
    }
}

void ScoreDoc::childChanged(bool dirtyHere) {
    if (dirtyHere) {
        Box* b = box();
        if (where_ == Where::Box && b && scoreBoxKeptInPatch(b)) {
            if (patchEdited) patchEdited(b->patch);
        } else {
            outsideEdit_ = true;
        }
    }
    score_->touch();                             // what it places changed: its time index, and who follows it
    push();
    ++version_;
    notify(Content | State);
}

void ScoreDoc::notify(unsigned what) {
    std::vector<int> tokens;
    for (auto& l : listeners_) tokens.push_back(l.token);
    for (int t : tokens)                         // a listener may unlisten another (or itself)
        for (auto& l : listeners_)
            if (l.token == t) {
                auto f = l.f;
                f(what);
                break;
            }
}

int ScoreDoc::listen(std::function<void(unsigned)> f) {
    listeners_.push_back({nextToken_, std::move(f)});
    return nextToken_++;
}

void ScoreDoc::unlisten(int token) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [token](const Listener& l) { return l.token == token; }),
                     listeners_.end());
}

Box* ScoreDoc::player(Patch* root) {
    if (where_ == Where::Box)
        if (Box* b = box()) return b;
    if (!player_) {
        if (!root) return nullptr;
        playerRoot_ = root;
        auto p = std::make_unique<Patch>();
        p->dir = folder();
        player_ = root->addExtra(std::move(p));
        std::string err;
        player_->load("(patch (obj s score) (obj o dac~ 1 2) (connect s 0 o 0) (connect s 1 o 1))", err);
    }
    Box* s = player_->find("s");
    if (s && scoreOfBox(s) != score_) replaceScoreOfBox(s, score_);
    return s;
}

// ---------------------------------------------------------------- following

void ScoreDoc::adopt(std::shared_ptr<sc::Score> s, const std::string& why) {
    score_ = std::move(s);
    undo_.clear();
    redo_.clear();
    savedSerial_ = 0;
    outsideEdit_ = false;
    seen_ = score_->version();
    sel_.erase(std::remove_if(sel_.begin(), sel_.end(), [this](uint64_t id) { return !score_->find(id); }), sel_.end());
    if (player_)
        if (Box* p = player_->find("s")) replaceScoreOfBox(p, score_);
    message = why;
    ++version_;
    notify(Content | Selection | State);
}

bool ScoreDoc::follow() {
    if (editing_) return false;
    if (where_ == Where::Box) {
        Box* b = box();
        if (!b) {
            if (!hadBox_) return false;
            hadBox_ = false;
            message = "the score box is gone: the score is kept here";
            ++version_;
            notify(State);
            return true;
        }
        bool back = !hadBox_;
        hadBox_ = true;
        auto s = scoreOfBox(b);
        if (s && (s != score_ || s->version() != seen_)) {
            adopt(s, "the score box changed: its score now");
            return true;
        }
        if (back) {
            ++version_;
            notify(State);
            return true;
        }
        return false;
    }
    if (where_ == Where::Sub && parent_) {
        auto e = parent_->score_->find(placement_);
        if (!e || e->kind != sc::Kind::Score || e->sub != score_) {
            uint64_t other = 0;
            for (auto& x : parent_->score_->events())
                if (x->kind == sc::Kind::Score && x->sub == score_) { other = x->id; break; }
            if (other) placement_ = other;
            if (!other && !placedGone_) {
                placedGone_ = true;
                message = "this score is not placed in " + parent_->title() + " any more";
                ++version_;
                notify(State);
                return true;
            }
            if (other && placedGone_) {
                placedGone_ = false;
                ++version_;
                notify(State);
                return true;
            }
        } else if (placedGone_) {
            placedGone_ = false;
            ++version_;
            notify(State);
            return true;
        }
    }
    if (score_->version() != seen_) {           // changed in place by someone else
        adopt(score_, "the score changed elsewhere");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- saving

bool ScoreDoc::dirty() const { return topSerial() != savedSerial_ || outsideEdit_; }

bool ScoreDoc::needsSaving() const {
    if (where_ == Where::Box) {
        Box* b = box();
        if (!b || scoreBoxKeptInPatch(b)) return false;
        return dirty();
    }
    if (where_ == Where::Sub) return !path().empty() && dirty();
    return dirty();
}

bool ScoreDoc::save(std::string& err) {
    std::string p = path();
    if (p.empty()) {
        err = where_ == Where::Box ? "this score is kept in its patch: save the patch" : "it has no file yet: Save As";
        return false;
    }
    try {
        sc::writeFile(*score_, p);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    score_->path = p;
    savedSerial_ = topSerial();
    outsideEdit_ = false;
    message = "saved " + baseOf(p);
    ++version_;
    notify(State);
    return true;
}

bool ScoreDoc::saveAs(const std::string& given, std::string& err) {
    if (where_ == Where::Box || where_ == Where::Sub) {
        if (!exportTo(given, err)) return false;
        message = "a copy saved as " + baseOf(given);
        return true;
    }
    std::string p = sc::normalPath(given);
    try {
        sc::writeFile(*score_, p);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    path_ = p;
    where_ = Where::File;
    score_->path = p;
    savedSerial_ = topSerial();
    outsideEdit_ = false;
    message = "saved " + baseOf(p);
    ++version_;
    notify(State);
    return true;
}

bool ScoreDoc::exportTo(const std::string& p, std::string& err) const {
    try {
        sc::writeFile(*score_, p);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- rendering

ScoreRender::~ScoreRender() { cancel(); }

bool ScoreRender::running() const { return job_ && job_->running(); }

void ScoreRender::cancel() {
    if (job_) job_->cancel();                    // waits for the worker; its done() is dropped
    patch_.reset();
}

bool ScoreRender::start(const sc::Score& s, const std::string& folder, const std::string& wav, int channels, double sr,
                        std::function<void(bool, const std::string&)> done, std::string& err) {
    cancel();
    channels = std::clamp(channels, 1, 16);
    double seconds = s.duration();
    if (!(seconds > 0)) { err = "the score is empty"; return false; }
    if (!(sr >= 1000 && sr <= 768000)) sr = 44100;
    auto p = std::make_unique<Patch>();
    p->dir = folder;
    p->sampleRate = sr;
    p->dspRunning = true;
    p->offline = true;                           // the clock is the sample count: events land on their sample
    p->ioBufferFrames = dsp::kBlock;
    auto frame = std::make_shared<std::atomic<long>>(0);
    p->clock = [frame, sr] { return (double)frame->load() / sr; };
    auto prints = std::make_shared<std::pair<std::mutex, std::vector<std::string>>>();
    p->onPrint = [prints](const std::string& m) {
        std::lock_guard<std::mutex> lk(prints->first);
        prints->second.push_back(m);
    };
    std::string text = "(patch (obj s score " + std::to_string(channels) + ") (obj o dac~";
    for (int c = 1; c <= channels; ++c) text += " " + std::to_string(c);
    text += ")";
    for (int c = 0; c < channels; ++c) text += " (connect s " + std::to_string(c) + " o " + std::to_string(c) + ")";
    text += ")";
    std::string lerr;
    if (!p->load(text, lerr)) { err = lerr; return false; }
    Box* b = p->find("s");
    if (!b || !replaceScoreOfBox(b, s.clone())) { err = "no score box"; return false; }   // its plan made here
    transportOf(p.get())->play(p.get());
    patch_ = std::move(p);
    job_ = std::make_unique<Job>();
    job_->onProgress = [this](float f) { if (onProgress) onProgress(f); };
    Patch* ip = patch_.get();
    long total = (long)std::ceil((seconds + 0.05) * sr);
    auto result = std::make_shared<std::string>();
    job_->start(
        [ip, frame, total, channels, sr, wav, result](Job& job) {
            dsp::FlushDenormalsScope ftz;
            struct Owner {
                Patch* p;
                explicit Owner(Patch* q) : p(q) { if (!jobsSynchronous) p->setDispatchThread(std::this_thread::get_id()); }
                ~Owner() { p->setDispatchThread(std::thread::id()); }
            } owner(ip);
            const int blk = dsp::kBlock;
            SampleBuffer out;
            out.channels = channels;
            out.sr = sr;
            out.frames = total;
            out.data.assign((size_t)total * channels, 0.f);
            std::vector<float> buf((size_t)blk * channels);
            for (long f = 0; f < total; f += blk) {
                if (job.cancelled()) return;
                frame->store(f);
                ip->drainForeign();
                int n = (int)std::min<long>(blk, total - f);
                ip->runDue((double)(f + n) / sr);
                ip->process(nullptr, 0, buf.data(), channels, n);
                std::copy(buf.begin(), buf.begin() + (long)n * channels, out.data.begin() + f * channels);
                if ((f / blk) % 256 == 0) job.progress((float)f / total);
            }
            std::string e;
            if (!writeWavFile(wav, out, e, 24)) *result = e.empty() ? "cannot write " + wav : e;
        },
        [this, result, prints, wav, done] {
            patch_.reset();
            bool ok = result->empty();
            if (done) done(ok, ok ? "rendered " + wav : *result);
        });
    return true;
}

}  // namespace moritz
