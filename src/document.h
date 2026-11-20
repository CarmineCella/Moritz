// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// document.h — a patch file being edited: the patch, its file, what is unsaved, and the undo history
//
// No interface here (document_window.cpp shows a Document). The patch belongs to the application's host
// patch (an extra of it); the document keeps:
//   * its file, set only when a load or a save succeeded;
//   * what is unsaved: the patch itself, and each abstraction inside it that was edited in its window
//     (an abstraction is saved in its own file, not with the patch);
//   * the undo history: before an edit, the text of the patch that is edited (the top patch, a
//     subpatcher or an abstraction, found again by its path of box ids). Undo puts that text back with
//     Patch::applyText, which keeps every box whose line did not change, with its state (a buffer~'s
//     sound, a code box's interpreter, an open editor) and its sound going on.
// Saving writes a temporary file and renames it (fileio.h): a failed save leaves the old file whole.
#pragma once

#include "moritz.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace moritz {

class Document {
public:
    explicit Document(Patch& p) : patch(p) {}
    Patch& patch;

    // ---- the file
    const std::string& path() const { return path_; }   // "" until loaded or saved
    std::string suggested;          // Save As proposes this name (patches imported from Max)
    bool load(const std::string& path, std::string& err);   // true with warnings in err
    // A help patch: shown as a document of its own, to play with, edit, and copy from, as in Max. Its file
    // is never written: the document has no path (Save asks where, proposing suggested), and it counts as
    // edited only after an edit (an undo step), not when it is played with.
    bool loadHelp(const std::string& path, std::string& err);
    const std::string& helpFile() const { return helpFile_; }   // the help patch shown ("": none, or saved as a patch)
    // a patch converted from elsewhere: no file yet, unsaved; relative paths start in dir
    bool loadText(const std::string& src, const std::string& dir, std::string& err);
    bool save(std::string& err);                             // to path()
    bool saveTo(const std::string& path, std::string& err);  // Save As: path and folder change once saved
    bool revert(std::string& err);                           // the file as it is on the disk

    // ---- what is unsaved
    bool dirty() const { return dirty_; }                    // the patch itself
    bool anyDirty() const { return dirty_ || !absDirty_.empty(); }
    bool isEmptyUntitled() const { return path_.empty() && !anyDirty() && patch.boxes.empty(); }
    // p was edited: the abstraction p is in (or is) has unsaved changes, or else the patch
    void markDirty(Patch* p);
    // the abstraction instance p is in, or p itself when it is one (nullptr: none)
    Patch* abstractionOf(Patch* p) const;
    std::vector<Patch*> dirtyAbstractions() const { return {absDirty_.begin(), absDirty_.end()}; }
    bool abstractionDirty(Patch* a) const { return absDirty_.count(a) > 0; }
    bool saveAbstraction(Patch* a, std::string& err);       // into its file
    void forget(Patch* p);                                   // p is being destroyed

    // ---- undo
    void pushUndo(Patch* p);        // before editing p
    void dropUndo();                // the last pushUndo changed nothing
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // the last edit undone (redone); restored: the patch put back; msg says what happened
    bool undo(std::string& msg, Patch** restored = nullptr);
    bool redo(std::string& msg, Patch** restored = nullptr);
    void clearHistory() { undo_.clear(); redo_.clear(); }

    // grows with every change made through the document (edits, undo, loads): views compare it
    uint64_t generation() const { return generation_; }
    void touch() { ++generation_; }
    // the file or what is unsaved changed (the window's title follows)
    std::function<void()> onChanged;

    // the patch at a path of box ids from the top ("p1/fm2"; "" is the top), or nullptr
    Patch* resolve(const std::string& where) const;
    std::string where(Patch* p) const;

private:
    struct Step {
        std::string where;          // the patch edited, as a path of box ids
        std::string text;           // its text before the edit
    };
    bool restore(std::vector<Step>& from, std::vector<Step>& to, std::string& msg, Patch** restored);
    void changed() {
        ++generation_;
        if (onChanged) onChanged();
    }
    std::string path_;
    std::string helpFile_;
    bool dirty_ = false;
    std::set<Patch*> absDirty_;
    std::vector<Step> undo_, redo_;
    uint64_t generation_ = 0;
};

}  // namespace moritz
