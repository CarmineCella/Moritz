// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// textview.cpp — a patch as text: a second view on the same patch
//
// The window shows the patch in its file format and stays in sync both ways.
// Editing the drawing rewrites the text; editing the text changes the drawing
// once typing pauses and the text reads as a patch (Patch::applyText keeps
// the boxes whose line did not change, so their sound goes on). Text edits
// go into the patch's undo history like any other edit.

#include "gui.h"

#include <FL/Fl_Text_Editor.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace moritz {

namespace {

const Fl_Text_Display::Style_Table_Entry kStyles[] = {
    {fl_rgb_color(214, 216, 222), FL_COURIER, 13, 0, 0},          // A plain
    {fl_rgb_color(120, 160, 120), FL_COURIER_ITALIC, 13, 0, 0},   // B ; comment
    {fl_rgb_color(226, 170, 110), FL_COURIER, 13, 0, 0},          // C "string"
    {fl_rgb_color(120, 170, 240), FL_COURIER_BOLD, 13, 0, 0},     // D obj, connect, patch, pos
    {fl_rgb_color(130, 132, 140), FL_COURIER, 13, 0, 0},          // E parentheses
};

std::string highlight(const std::string& t) {
    std::string st(t.size(), 'A');
    bool inStr = false, comment = false;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '\n') { comment = false; continue; }
        if (comment) { st[i] = 'B'; continue; }
        if (inStr) {
            st[i] = 'C';
            if (c == '\\') { if (i + 1 < t.size()) st[++i] = 'C'; }
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == ';') { comment = true; st[i] = 'B'; }
        else if (c == '"') { inStr = true; st[i] = 'C'; }
        else if (c == '(' || c == ')') {
            st[i] = 'E';
            if (c == '(')                                  // the keyword after "("
                for (const char* kw : {"obj ", "connect ", "patch", "pos "})
                    if (t.compare(i + 1, std::char_traits<char>::length(kw), kw) == 0) {
                        size_t n = std::char_traits<char>::length(kw);
                        if (kw[n - 1] == ' ') --n;
                        for (size_t k = 0; k < n; ++k) st[i + 1 + k] = 'D';
                        i += n;
                        break;
                    }
        }
    }
    return st;
}

}  // namespace

PatchTextWindow::PatchTextWindow(MainWindow* doc, Patch* p, const std::string& title, bool readOnly)
    : Fl_Double_Window(560, 640), patch(p), doc_(doc), readOnly_(readOnly) {
    copy_label((title + " — code").c_str());
    text_ = new Fl_Text_Buffer();
    style_ = new Fl_Text_Buffer();
    editor_ = new Fl_Text_Editor(0, 0, 560, 640 - 22);
    editor_->buffer(text_);
    editor_->highlight_data(style_, kStyles, (int)(sizeof kStyles / sizeof kStyles[0]), 'A', nullptr, nullptr);
    editor_->box(FL_FLAT_BOX);
    editor_->color(fl_rgb_color(24, 25, 28));
    editor_->selection_color(fl_rgb_color(52, 72, 110));
    editor_->cursor_color(fl_rgb_color(226, 228, 234));
    editor_->linenumber_width(40);
    editor_->linenumber_bgcolor(fl_rgb_color(32, 33, 37));
    editor_->linenumber_fgcolor(fl_rgb_color(110, 112, 120));
    editor_->linenumber_size(11);
    editor_->textfont(FL_COURIER);
    editor_->textsize(13);
    if (readOnly_) editor_->deactivate();
    status_ = new Fl_Box(0, 640 - 22, 560, 22);
    status_->box(FL_FLAT_BOX);
    status_->color(fl_rgb_color(30, 31, 35));
    status_->labelsize(11);
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    resizable(editor_);
    end();
    callback([](Fl_Widget* w, void*) {
        if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
        auto* tw = static_cast<PatchTextWindow*>(w);
        tw->applyNow();
        tw->hide();
    });
    refresh(true);
    text_->add_modify_callback(modified, this);
    Fl::add_timeout(0.25, poll, this);
}

PatchTextWindow::~PatchTextWindow() {
    Fl::remove_timeout(poll, this);
    Fl::remove_timeout(applyLater, this);
    text_->remove_modify_callback(modified, this);
    editor_->buffer(nullptr);
    delete text_;
    delete style_;
}

void PatchTextWindow::detach() {
    patch = nullptr;
    doc_ = nullptr;
    Fl::remove_timeout(applyLater, this);
}

void PatchTextWindow::setStatus(const std::string& s, bool error) {
    status_->labelcolor(error ? fl_rgb_color(230, 120, 110) : fl_rgb_color(140, 142, 150));
    status_->copy_label(("  " + s).c_str());
    status_->redraw();
}

// The patch changed (or the window opened): show its text, unless the user is
// in the middle of an edit that has not reached the patch yet.
void PatchTextWindow::refresh(bool force) {
    if (!patch || (pending_ && !force)) return;
    std::string t = patch->serialize();
    if (t == shown_ && !force) return;
    int top = editor_->get_absolute_top_line_number(), cur = editor_->insert_position();
    syncing_ = true;
    text_->text(t.c_str());
    style_->text(highlight(t).c_str());
    syncing_ = false;
    shown_ = t;
    editor_->insert_position(std::min(cur, text_->length()));
    editor_->scroll(top, 0);
    setStatus(readOnly_ ? "help patch: read only" : "in sync with the drawing · edit here or there");
}

void PatchTextWindow::modified(int, int nIns, int nDel, int, const char*, void* d) {
    auto* w = static_cast<PatchTextWindow*>(d);
    if (w->syncing_ || (nIns == 0 && nDel == 0)) return;
    char* t = w->text_->text();
    w->style_->text(highlight(t).c_str());
    std::free(t);
    w->editor_->redisplay_range(0, w->text_->length());
    w->pending_ = true;
    w->setStatus("editing…");
    Fl::remove_timeout(applyLater, w);
    Fl::add_timeout(0.7, applyLater, w);    // apply once typing pauses
}

void PatchTextWindow::applyLater(void* d) { static_cast<PatchTextWindow*>(d)->applyNow(); }

void PatchTextWindow::poll(void* d) {
    auto* w = static_cast<PatchTextWindow*>(d);
    if (w->shown()) w->refresh(false);
    Fl::repeat_timeout(0.25, poll, d);
}

void PatchTextWindow::applyNow() {
    Fl::remove_timeout(applyLater, this);
    if (!pending_ || !patch || readOnly_) return;
    char* raw = text_->text();
    std::string t = raw;
    std::free(raw);
    if (t == shown_) { pending_ = false; setStatus("in sync with the drawing · edit here or there"); return; }
    if (doc_) doc_->pushUndo();
    std::string err;
    if (!patch->applyText(t, err)) {
        if (doc_) doc_->dropUndo();
        setStatus("not applied: " + err, true);   // stays pending until it reads as a patch
        return;
    }
    if (patch->owner) patch->rebuild();          // a subpatcher's inlets follow positions
    if (doc_) doc_->markDirty();
    shown_ = patch->serialize();
    pending_ = false;
    if (err.empty()) setStatus("applied · in sync with the drawing");
    else setStatus("applied, with warnings: " + err, true);
}

}  // namespace moritz
