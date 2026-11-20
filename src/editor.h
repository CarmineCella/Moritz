// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// editor.h — what a canvas asks of the document its patch belongs to
//
// A canvas (canvas.cpp) draws a patch and turns the mouse and the keys into edits; every edit goes
// through its PatchEditor, which keeps the undo history and knows what is edited and unsaved. A
// document window is one (help patches are documents too); --render snapshots get a detached one, so a
// canvas always has an editor to call.
#pragma once

#include <string>

namespace moritz {

class Box;
class Patch;

class PatchEditor {
public:
    virtual ~PatchEditor() = default;
    // false: the patch can't be edited: always locked, nothing to undo or save
    virtual bool editable() const = 0;
    // before an edit of p: an undo step holding p as it is
    virtual void pushUndo(Patch* p) = 0;
    // the last pushUndo() changed nothing: forget it
    virtual void dropUndo() = 0;
    // p was edited: the document (or the abstraction p is in) has unsaved changes
    virtual void markDirty(Patch* p) = 0;
    // a message: the console, and the status bar of the window
    virtual void print(const std::string& s) = 0;
    virtual void setStatus(const std::string& s) = 0;
    // the canvas was locked or unlocked
    virtual void lockChanged() = 0;
    // a subpatcher, abstraction or pfft~ box asks for its window
    virtual void openSubpatch(Box* b) = 0;
};

// A canvas drawn on its own (moritz --render ... png=): editable, so edit mode can be drawn, but with
// no history and nowhere to report to.
class DetachedEditor : public PatchEditor {
public:
    bool editable() const override { return true; }
    void pushUndo(Patch*) override {}
    void dropUndo() override {}
    void markDirty(Patch*) override {}
    void print(const std::string&) override {}
    void setStatus(const std::string&) override {}
    void lockChanged() override {}
    void openSubpatch(Box*) override {}
    static DetachedEditor& instance() {
        static DetachedEditor e;
        return e;
    }
};

}  // namespace moritz
