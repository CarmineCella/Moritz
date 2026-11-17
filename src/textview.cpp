// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// textview.cpp — a patch's code view: the code editor (code.cpp) bound to the patch
//
// The window shows the patch in its file format and stays in sync both ways.
// Editing the drawing rewrites the text; editing the text changes the drawing
// once typing pauses and the text reads as a patch (Patch::applyText keeps
// the boxes whose line did not change, so their sound goes on). Text edits
// go into the patch's undo history like any other edit. Text with code in it
// (a loop, a comma) runs in the patch's context when Run file is pressed, and
// the patch it makes replaces the drawing.

#include "gui.h"
#include "langpatch.h"

#include <FL/Fl.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Text_Editor.H>

#include <algorithm>
#include <string>

#ifdef OPAQUE      // Windows (wingdi.h) defines it; the language has a value type of that name
#undef OPAQUE
#endif

namespace moritz {

PatchTextWindow::PatchTextWindow(MainWindow* doc, Patch* p, const std::string& title, bool readOnly)
    : CodeWindow(), patch(p), doc_(doc), readOnly_(readOnly), title_(title) {
    copy_label((title + " — code").c_str());
    context_->clear();
    context_->add(readOnly ? "help patch" : "this patch");
    context_->value(0);
    context_->deactivate();
    if (readOnly_) editor_->deactivate();
    refresh(true);
    refreshVariables();
    Fl::add_timeout(0.25, poll, this);
}

PatchTextWindow::~PatchTextWindow() {
    Fl::remove_timeout(poll, this);
    Fl::remove_timeout(applyLater, this);
}

void PatchTextWindow::detach() {
    patch = nullptr;
    doc_ = nullptr;
    Fl::remove_timeout(applyLater, this);
}

void PatchTextWindow::note(const std::string& s) { setStatus(s); }

std::shared_ptr<LangContext> PatchTextWindow::context() {
    if (!doc_) return sessionContext();
    return patchContext(doc_, doc_->label() ? doc_->label() : "patch");
}

// The patch changed (or the window opened): show its text, unless the user is
// in the middle of an edit that has not reached the patch yet.
void PatchTextWindow::refresh(bool force) {
    if (!patch || (pending_ && !force)) return;
    std::string t = patch->serialize();
    if (t == shown_ && !force) return;
    int top = editor_->get_absolute_top_line_number(), cur = editor_->insert_position();
    setText(t);
    shown_ = t;
    editor_->insert_position(std::min(cur, text_->length()));
    editor_->scroll(top, 0);
    note(readOnly_ ? "help patch: read only" : "in sync with the drawing · edit here or there");
}

void PatchTextWindow::textChanged() {
    if (syncing_) return;
    errorLine_ = 0;
    restyle();
    pending_ = true;
    Fl::remove_timeout(applyLater, this);
    if (patchNeedsLanguage(text())) {
        note("code: Run file (Shift-Cmd-Enter) runs it and its patch replaces the drawing");
        return;
    }
    note("editing…");
    Fl::add_timeout(0.7, applyLater, this);    // apply once typing pauses
}

void PatchTextWindow::applyLater(void* d) { static_cast<PatchTextWindow*>(d)->applyNow(); }

void PatchTextWindow::poll(void* d) {
    auto* w = static_cast<PatchTextWindow*>(d);
    if (w->shown()) w->refresh(false);
    Fl::repeat_timeout(0.25, poll, d);
}

void PatchTextWindow::applyText(const std::string& t) {
    if (!patch) return;
    if (doc_) doc_->pushUndo();
    std::string err;
    if (!patch->applyText(t, err)) {
        if (doc_) doc_->dropUndo();
        note("not applied: " + err);              // stays pending until it reads as a patch
        return;
    }
    if (patch->owner) patch->rebuild();          // a subpatcher's inlets follow positions
    if (doc_) doc_->markDirty();
    shown_ = patch->serialize();
    pending_ = false;
    if (err.empty()) note("applied · in sync with the drawing");
    else note("applied, with warnings: " + err);
}

void PatchTextWindow::applyNow() {
    Fl::remove_timeout(applyLater, this);
    if (!pending_ || !patch || readOnly_) return;
    std::string t = text();
    if (t == shown_) { pending_ = false; note("in sync with the drawing · edit here or there"); return; }
    if (patchNeedsLanguage(t)) return;            // code waits for Run file
    applyText(t);
}

void PatchTextWindow::runFile() {
    if (!patch || readOnly_) return;
    std::string t = text();
    if (!patchNeedsLanguage(t)) { pending_ = true; applyNow(); return; }
    run(t, 1, text_->count_lines(0, text_->length()) + 1, "<" + title_ + ">");
}

void PatchTextWindow::patchesMade(const std::vector<std::string>& patches, const std::string&) {
    if (!patches.empty()) applyText(patches.front());   // the patch the code makes is this patch
}

bool PatchTextWindow::save() {
    applyNow();
    return doc_ ? doc_->save() : false;
}

bool PatchTextWindow::saveAs() {
    applyNow();
    return doc_ ? doc_->saveAs() : false;
}

void PatchTextWindow::closeRequested() {
    applyNow();
    hide();
}

}  // namespace moritz
