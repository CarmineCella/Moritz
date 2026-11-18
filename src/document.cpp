// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// document.cpp — a patch file being edited (document.h)

#include "document.h"

#include "fileio.h"

namespace moritz {

namespace {
constexpr size_t kUndoSteps = 500;
}

// ---- the file

bool Document::load(const std::string& path, std::string& err) {
    std::string src;
    if (!fileio::read(path, src)) { err = "can't read " + path; return false; }
    std::string oldDir = patch.dir;
    patch.dir = dirOf(path);
    if (!patch.load(src, err)) {
        patch.dir = oldDir;
        return false;
    }
    path_ = fileio::absolute(path);
    suggested.clear();
    dirty_ = false;
    absDirty_.clear();
    clearHistory();
    changed();
    return true;
}

bool Document::loadText(const std::string& src, const std::string& dir, std::string& err) {
    std::string oldDir = patch.dir;
    patch.dir = dir;
    if (!patch.load(src, err)) {
        patch.dir = oldDir;
        return false;
    }
    path_.clear();
    dirty_ = true;                     // not saved yet
    absDirty_.clear();
    clearHistory();
    changed();
    return true;
}

bool Document::save(std::string& err) {
    if (path_.empty()) { err = "the patch has no file yet"; return false; }
    if (!fileio::writeAtomic(path_, patch.serialize(), &err)) return false;
    dirty_ = false;
    changed();
    return true;
}

bool Document::saveTo(const std::string& path, std::string& err) {
    if (!fileio::writeAtomic(path, patch.serialize(), &err)) return false;   // path and folder stay as they were
    path_ = fileio::absolute(path);
    patch.dir = dirOf(path_);
    suggested.clear();
    dirty_ = false;
    changed();
    return true;
}

bool Document::revert(std::string& err) {
    if (path_.empty()) { err = "the patch has no file"; return false; }
    std::string p = path_;
    return load(p, err);
}

// ---- what is unsaved

Patch* Document::abstractionOf(Patch* p) const {
    for (Patch* q = p; q && q != &patch; q = q->parent)
        if (q->isAbstraction && !q->file.empty()) return q;
    return nullptr;
}

void Document::markDirty(Patch* p) {
    if (Patch* a = abstractionOf(p)) absDirty_.insert(a);
    else dirty_ = true;
    changed();
}

bool Document::saveAbstraction(Patch* a, std::string& err) {
    if (!a || a->file.empty()) { err = "not an abstraction"; return false; }
    if (!fileio::writeAtomic(a->file, a->serialize(), &err)) return false;
    absDirty_.erase(a);
    changed();
    return true;
}

void Document::forget(Patch* p) {
    if (absDirty_.erase(p)) changed();
}

// ---- undo

std::string Document::where(Patch* p) const {
    std::string r;
    for (; p && p != &patch && p->owner; p = p->parent) r = p->owner->id + (r.empty() ? "" : "/" + r);
    return r;
}

Patch* Document::resolve(const std::string& where) const {
    Patch* p = &patch;
    size_t start = 0;
    while (p && start < where.size()) {
        size_t end = where.find('/', start);
        std::string id = where.substr(start, end == std::string::npos ? std::string::npos : end - start);
        Box* b = p->find(id);
        p = b ? b->inner() : nullptr;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return p;
}

void Document::pushUndo(Patch* p) {
    if (!p) p = &patch;
    undo_.push_back({where(p), p->serialize()});
    if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
    redo_.clear();
}

void Document::dropUndo() {
    if (!undo_.empty()) undo_.pop_back();
}

bool Document::restore(std::vector<Step>& from, std::vector<Step>& to, std::string& msg, Patch** restored) {
    if (restored) *restored = nullptr;
    while (!from.empty()) {
        Step s = std::move(from.back());
        from.pop_back();
        Patch* p = resolve(s.where);
        if (!p) {                           // the subpatcher it was made in is gone: the next step
            msg = "skipped an edit of " + s.where + ", which is gone";
            continue;
        }
        to.push_back({s.where, p->serialize()});
        std::string err;
        if (!p->applyText(s.text, err)) {   // can't happen with a text the patch wrote; nothing changed
            to.pop_back();
            msg = "can't restore: " + err;
            return false;
        }
        if (p->owner) p->rebuild();         // a subpatcher's inlets and outlets follow positions
        markDirty(p);
        if (restored) *restored = p;
        return true;
    }
    return false;
}

bool Document::undo(std::string& msg, Patch** restored) {
    msg.clear();
    if (!restore(undo_, redo_, msg, restored)) {
        if (msg.empty()) msg = "nothing to undo";
        return false;
    }
    if (msg.empty()) msg = "undo";
    return true;
}

bool Document::redo(std::string& msg, Patch** restored) {
    msg.clear();
    if (!restore(redo_, undo_, msg, restored)) {
        if (msg.empty()) msg = "nothing to redo";
        return false;
    }
    if (msg.empty()) msg = "redo";
    return true;
}

}  // namespace moritz
