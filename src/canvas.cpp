// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// canvas.cpp — the patch editor: drawing boxes and cables, editing,
// completion, and running interface objects in locked mode.

#include "gui.h"
#include "objects.h"

#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <map>

namespace moritz {


namespace {

double segDist(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy;
    double t = L > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / L, 0.0, 1.0) : 0.0;
    double cx = ax + t * dx - px, cy = ay + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

void flashTimer(void* data) { static_cast<Canvas*>(data)->redraw(); }

// Message box text as shown, like Max: a line break after each ";", so
// "; cf 1200; q 8" reads ";" / "cf 1200;" / "q 8". Only the display
// changes; the box keeps its text on one line.
std::vector<std::string> messageLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (auto& tok : splitTokens(text)) {
        if (tok == ";" || tok == ",") {
            cur += tok;
            if (tok == ";") { lines.push_back(cur); cur.clear(); }
            continue;
        }
        if (!cur.empty()) cur += ' ';
        cur += tok;
    }
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    return lines;
}

int lineHeight() {
    fl_font(kFont, kFontSize);
    return fl_height();
}

std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> v(1);
    for (char c : t) {
        if (c == '\n') v.emplace_back();
        else v.back() += c;
    }
    return v;
}

int widestLine(const std::vector<std::string>& lines) {
    fl_font(kFont, kFontSize);
    int w = 0;
    for (auto& l : lines) w = std::max(w, (int)fl_width(l.c_str()));
    return w;
}

// Text field for box editing. While the completion popup is open it takes
// Up/Down (choose), Tab (complete, keep typing) and Enter (complete and
// create); every other key edits the text and refreshes the suggestions.
class CompletingInput : public Fl_Input {
public:
    CompletingInput(int x, int y, int w, int h, Canvas* c) : Fl_Input(x, y, w, h), canvas_(c) {}
    int handle(int e) override {
        if (e == FL_KEYBOARD && input_type() == FL_MULTILINE_INPUT &&
            (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) && Fl::event_state(FL_SHIFT)) {
            replace(insert_position(), mark(), "\n", 1);   // Shift+Return: new line
            canvas_->fitEditor();
            return 1;
        }
        if (e == FL_KEYBOARD && canvas_->hasSuggestions()) {
            switch (Fl::event_key()) {
                case FL_Down: canvas_->moveSuggestion(1); return 1;
                case FL_Up: canvas_->moveSuggestion(-1); return 1;
                case FL_Tab: canvas_->acceptSuggestion(false); return 1;
                case FL_Escape: canvas_->clearSuggestions(); return 1;
                case FL_Enter:
                case FL_KP_Enter:
                    if (canvas_->acceptSuggestion(true)) return 1;
                    break;
            }
        }
        int r = Fl_Input::handle(e);
        if (e == FL_KEYBOARD || e == FL_PASTE) {
            canvas_->updateSuggestions();
            canvas_->fitEditor();
        }
        return r;
    }

private:
    Canvas* canvas_;
};
}  // namespace

Canvas::Canvas(int x, int y, int w, int h, MainWindow* win, Patch* patch)
    : Fl_Group(x, y, w, h), win_(win), patch_(patch) {
    box(FL_FLAT_BOX);
    editor_ = new CompletingInput(x, y, 120, kBoxH, this);
    editor_->textfont(kFont);
    editor_->textsize(kFontSize);
    editor_->box(FL_FLAT_BOX);
    editor_->color(kEditBg);
    editor_->textcolor(kText);
    editor_->cursor_color(kAccent);
    editor_->selection_color(fl_rgb_color(60, 100, 170));
    editor_->when(FL_WHEN_ENTER_KEY_ALWAYS | FL_WHEN_RELEASE_ALWAYS);
    editor_->callback(editCb, this);
    editor_->hide();
    end();
}

void Canvas::layoutBox(Box* b) const {
    fl_font(kFont, kFontSize);
    std::string t = b->label();
    int tw = (int)fl_width(t.c_str());
    int ports = std::max(b->numIns(), b->numOuts());
    b->h = kBoxH;
    if (b->kind == BoxKind::UI) b->uiSize(b->w, b->h);
    else if (b->kind == BoxKind::Number) b->w = std::max(56, tw + 24);
    else if (b->kind == BoxKind::Message) {
        auto lines = messageLines(t);
        b->w = std::max(36, widestLine(lines) + 20);
        b->h = kBoxH + ((int)lines.size() - 1) * lineHeight();
    }
    else if (b->kind == BoxKind::Comment) b->w = std::max(40, tw + 12);
    else b->w = std::max({tw + 16, ports * (kPortW + 8) + 2 * kPortInset, 40});
}

void Canvas::portPos(Box* b, bool out, int i, int& px, int& py) const {
    int n = out ? b->numOuts() : b->numIns();
    int span = b->w - kPortW - 2 * kPortInset;
    px = b->x + kPortInset + (n <= 1 ? 0 : i * span / (n - 1));
    py = out ? b->y + b->h : b->y;
}

void Canvas::drawBox(Box* b) {
    int X = x() + b->x, Y = y() + b->y, W = b->w, H = b->h;
    bool flash = b->flashUntil > now();

    if (b->kind == BoxKind::Comment) {               // comments: just text
        if (b->selected || !locked_) {
            fl_color(b->selected ? kSelect : kGrid);
            fl_line_style(FL_DOT, 1);
            fl_rounded_rect(X, Y, W, H, 4);
            fl_line_style(0);
        }
        if (editing_ != b) {
            fl_font(kFont, kFontSize);
            fl_color(kDim);
            fl_draw(b->label().c_str(), X + 6, Y + H / 2 + fl_height() / 2 - fl_descent());
        }
        return;
    }

    // message boxes are pills, everything else a softly rounded box
    int r = b->kind == BoxKind::Message ? std::min(H, kBoxH) / 2 : kRadius;
    Fl_Color fill = kBoxFill;
    if (b->kind == BoxKind::Message) fill = flash ? kFlash : kMsgFill;
    else if (b->kind == BoxKind::Number || b->kind == BoxKind::UI) fill = kNumFill;
    else if (b->kind == BoxKind::Broken) fill = kBroken;
    fl_color(fill);
    fl_rounded_rectf(X, Y, W, H, r);

    if (b->kind == BoxKind::UI) {
        fl_push_clip(X, Y, W, H);
        b->drawUI(X, Y);
        fl_pop_clip();
    }

    // border
    if (b->selected) {
        fl_color(kSelect);
        fl_line_style(FL_SOLID, 2);
    } else if (b->kind == BoxKind::Broken) {
        fl_color(fl_rgb_color(230, 80, 80));
        fl_line_style(FL_DASH, 1);
    } else {
        fl_color(b->isSignal() ? fl_color_average(kSignal, kBorder, 0.25f) : kBorder);
        fl_line_style(FL_SOLID, 1);
    }
    fl_rounded_rect(X, Y, W, H, r);
    fl_line_style(0);

    // text
    if (editing_ != b && b->kind != BoxKind::UI) {
        fl_font(kFont, kFontSize);
        int tx = X + (b->kind == BoxKind::Message ? 10 : 8);
        std::string t = b->label();
        if (b->kind == BoxKind::Number) {
            fl_color(kAccent);
            fl_polygon(X + 6, Y + 7, X + 11, Y + H / 2, X + 6, Y + H - 7);
            tx = X + 16;
            if (typingBox_ == b && !typed_.empty()) t = typed_ + "_";
        }
        fl_color(kText);
        fl_push_clip(X + 2, Y + 1, W - 4, H - 2);
        if (b->kind == BoxKind::Message) {
            int base = Y + kBoxH / 2 + fl_height() / 2 - fl_descent(), lh = fl_height();
            for (auto& line : messageLines(t)) {
                fl_draw(line.c_str(), tx, base);
                base += lh;
            }
        } else {
            fl_draw(t.c_str(), tx, Y + H / 2 + fl_height() / 2 - fl_descent());
        }
        fl_pop_clip();
    }

    // ports: small tabs on the top (inlets) and bottom (outlets) edges
    for (int k = 0; k < b->numIns(); ++k) {
        int px, py;
        portPos(b, false, k, px, py);
        fl_color(b->ins[k] == Port::Control ? kCtlPort : kSignal);
        fl_rounded_rectf(x() + px, y() + py, kPortW, kPortH + 1, 1);
    }
    for (int k = 0; k < b->numOuts(); ++k) {
        int px, py;
        portPos(b, true, k, px, py);
        fl_color(b->outs[k] == Port::Control ? kCtlPort : kSignal);
        fl_rounded_rectf(x() + px, y() + py - kPortH - 1, kPortW, kPortH + 1, 1);
    }
}

// ---- completion popup

void Canvas::updateSuggestions() {
    sugg_.clear();
    suggSel_ = 0;
    if (editing_ && editing_->kind != BoxKind::Message && editing_->kind != BoxKind::Comment) {
        std::string t = editor_->value();
        if (!t.empty() && t.find(' ') == std::string::npos) {
            std::vector<std::string> exact, prefix, inner;
            static const std::vector<std::string> names = [] {   // objects and library
                auto v = allClassNames();
                for (auto& l : libraryNames()) v.push_back(l);
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end()), v.end());
                return v;
            }();
            for (auto& c : names) {
                if (c == t) exact.push_back(c);
                else if (c.compare(0, t.size(), t) == 0) prefix.push_back(c);
                else if (c.find(t) != std::string::npos) inner.push_back(c);
            }
            for (auto* v : {&exact, &prefix, &inner})
                for (auto& c : *v)
                    if ((int)sugg_.size() < kSuggMax) sugg_.push_back(c);
            // nothing left to complete
            if (sugg_.size() == 1 && sugg_[0] == t) sugg_.clear();
        }
    }
    redraw();
}

void Canvas::moveSuggestion(int d) {
    if (sugg_.empty()) return;
    int n = (int)sugg_.size();
    suggSel_ = ((suggSel_ + d) % n + n) % n;
    redraw();
}

void Canvas::clearSuggestions() {
    sugg_.clear();
    redraw();
}

bool Canvas::acceptSuggestion(bool commit) {
    if (sugg_.empty() || !editing_) return false;
    std::string t = editor_->value(), s = sugg_[suggSel_];
    sugg_.clear();
    if (commit && t == s) return false;          // already complete: plain Enter
    std::string v = commit ? s : s + " ";
    editor_->value(v.c_str());
    editor_->insert_position((int)v.size());
    if (commit) commitEdit();
    else redraw();
    return true;
}

void Canvas::drawSuggestions() {
    if (!editing_ || sugg_.empty() || !editor_->visible()) return;
    fl_font(kFont, kFontSize);
    int nameW = 0;
    for (auto& s : sugg_) nameW = std::max(nameW, (int)fl_width(s.c_str()));
    int descW = 0;
    for (auto& s : sugg_) descW = std::max(descW, (int)fl_width(describeClass(s)));
    suggW_ = std::max(editor_->w(), nameW + descW + 40);
    int H = (int)sugg_.size() * kSuggRowH + 6;
    suggX_ = editor_->x();
    suggY_ = editor_->y() + editor_->h() + 3;
    if (suggY_ + H > y() + h()) suggY_ = editor_->y() - H - 3;   // flip above near the bottom

    fl_color(fl_rgb_color(10, 10, 12));                              // soft shadow
    fl_rounded_rectf(suggX_ + 2, suggY_ + 3, suggW_, H, 6);
    fl_color(kPopup);
    fl_rounded_rectf(suggX_, suggY_, suggW_, H, 6);
    fl_color(kBorder);
    fl_rounded_rect(suggX_, suggY_, suggW_, H, 6);
    for (int k = 0; k < (int)sugg_.size(); ++k) {
        int ry = suggY_ + 3 + k * kSuggRowH;
        if (k == suggSel_) {
            fl_color(fl_rgb_color(52, 92, 160));
            fl_rounded_rectf(suggX_ + 3, ry, suggW_ - 6, kSuggRowH, 4);
        }
        int base = ry + kSuggRowH / 2 + fl_height() / 2 - fl_descent();
        fl_color(kText);
        fl_draw(sugg_[k].c_str(), suggX_ + 10, base);
        fl_color(k == suggSel_ ? fl_rgb_color(200, 215, 240) : kDim);
        fl_draw(describeClass(sugg_[k]), suggX_ + 26 + nameW, base);
    }
}

int Canvas::suggestionAt(int ex, int ey) const {
    if (sugg_.empty()) return -1;
    int H = (int)sugg_.size() * kSuggRowH + 6;
    if (ex < suggX_ || ex > suggX_ + suggW_ || ey < suggY_ + 3 || ey >= suggY_ + H - 3) return -1;
    return std::min((int)sugg_.size() - 1, (ey - suggY_ - 3) / kSuggRowH);
}

// ---- port help

void Canvas::tipTimer(void* data) {
    auto* c = static_cast<Canvas*>(data);
    if (!c->tipBox_) return;
    c->tipShown_ = true;
    c->redraw();
}

void Canvas::hidePortTip() {
    Fl::remove_timeout(tipTimer, this);
    bool was = tipShown_;
    tipBox_ = nullptr;
    tipPort_ = -1;
    tipShown_ = false;
    if (was) redraw();
}

// Finds the inlet or outlet under the mouse (edit mode) and shows its help
// after a short rest.
void Canvas::updatePortTip(int mx, int my) {
    Box* box = nullptr;
    bool out = false;
    int port = -1;
    if (!locked_ && !editing_ && drag_ == Drag::None) {
        for (auto it = patch_->boxes.rbegin(); it != patch_->boxes.rend() && port < 0; ++it) {
            Box* b = it->get();
            if (mx < b->x - 3 || mx > b->x + b->w + 3) continue;
            bool top = my >= b->y - 4 && my <= b->y + 5;
            bool bottom = my >= b->y + b->h - 5 && my <= b->y + b->h + 4;
            int n = top ? b->numIns() : bottom ? b->numOuts() : 0;
            for (int k = 0; k < n; ++k) {
                int px, py;
                portPos(b, bottom, k, px, py);
                if (mx >= px - 3 && mx <= px + kPortW + 3) {
                    box = b;
                    out = bottom;
                    port = k;
                    break;
                }
            }
        }
    }
    if (box == tipBox_ && out == tipOut_ && port == tipPort_) return;
    hidePortTip();
    if (!box) return;
    tipBox_ = box;
    tipOut_ = out;
    tipPort_ = port;
    Fl::add_timeout(0.35, tipTimer, this);
}

void Canvas::drawPortTip() {
    if (!tipShown_ || !tipBox_) return;
    bool alive = false;
    for (auto& b : patch_->boxes) alive = alive || b.get() == tipBox_;
    if (!alive || tipPort_ >= (tipOut_ ? tipBox_->numOuts() : tipBox_->numIns())) return;
    std::string help = portHelp(tipBox_, tipOut_, tipPort_);
    std::string title = help.substr(0, help.find('\n'));
    std::string body = help.find('\n') == std::string::npos ? "" : help.substr(help.find('\n') + 1);

    int px, py;
    portPos(tipBox_, tipOut_, tipPort_, px, py);
    px += x();
    py += y();
    fl_color(kAccent);                                  // ring around the port
    fl_line_style(FL_SOLID, 2);
    fl_rounded_rect(px - 3, (tipOut_ ? py - kPortH - 4 : py - 3), kPortW + 6, kPortH + 7, 3);
    fl_line_style(0);

    fl_font(FL_HELVETICA_BOLD, 11);
    int tw = (int)fl_width(title.c_str());
    fl_font(kFont, 12);
    int bw = (int)fl_width(body.c_str());
    int W = std::max(tw, bw) + 20, H = body.empty() ? 24 : 42;
    int X = std::clamp(px - 12, x() + 4, x() + w() - W - 4);
    int Y = tipOut_ ? py + 8 : py - H - 8;
    if (Y < y() + 4) Y = py + 12;
    if (Y + H > y() + h() - 4) Y = py - H - 12;
    fl_color(fl_rgb_color(10, 10, 12));
    fl_rounded_rectf(X + 2, Y + 3, W, H, 6);
    fl_color(kPopup);
    fl_rounded_rectf(X, Y, W, H, 6);
    fl_color(kBorder);
    fl_rounded_rect(X, Y, W, H, 6);
    fl_font(FL_HELVETICA_BOLD, 11);
    fl_color(tipBox_->outs.empty() && tipOut_ ? kDim : kAccent);
    fl_draw(title.c_str(), X + 10, Y + 16);
    if (!body.empty()) {
        fl_font(kFont, 12);
        fl_color(kText);
        fl_draw(body.c_str(), X + 10, Y + 34);
    }
}

// ---- option-drag duplication

void Canvas::duplicateSelection() {
    auto sel = selectedBoxes();
    if (sel.empty()) return;
    win_->pushUndo();
    Patch& p = *patch_;
    std::map<Box*, Box*> copy;
    for (Box* b : sel) copy[b] = p.addBox(p.create(b->cls, b->keepRaw ? b->rawArgs : b->saveArgs()), b->x, b->y);
    auto conns = p.conns;               // connect() appends to p.conns
    for (auto& c : conns)
        if (copy.count(c.from) && copy.count(c.to))
            p.connect(copy[c.from], c.outlet, copy[c.to], c.inlet);
    clearSelection();
    for (auto& kv : copy) kv.second->selected = true;
    win_->markDirty();
}

void Canvas::draw() {
    Patch& p = *patch_;
    fl_push_clip(x(), y(), w(), h());
    fl_color(locked_ ? kBgLocked : kBgEdit);
    fl_rectf(x(), y(), w(), h());
    if (!locked_) {
        fl_color(kGrid);
        for (int gx = 15; gx < w(); gx += 15)
            for (int gy = 15; gy < h(); gy += 15) fl_point(x() + gx, y() + gy);
    }

    for (auto& b : p.boxes) layoutBox(b.get());

    // cables
    for (auto& c : p.conns) {
        int ax, ay, bx, by;
        portPos(c.from, true, c.outlet, ax, ay);
        portPos(c.to, false, c.inlet, bx, by);
        ax += x() + kPortW / 2; bx += x() + kPortW / 2;
        ay += y(); by += y();
        bool sig = c.from->outs[c.outlet] != Port::Control;   // signal and tap links
        if (sig) {
            fl_color(c.selected ? kSelect : kSignal);
            fl_line_style(FL_SOLID | FL_CAP_ROUND, 3);
            fl_line(ax, ay, bx, by);
            fl_color(c.selected ? fl_rgb_color(30, 70, 140) : kSignalDim);
            fl_line_style(FL_DASH, 1);
            fl_line(ax, ay, bx, by);
        } else {
            fl_color(c.selected ? kSelect : kCable);
            fl_line_style(FL_SOLID | FL_CAP_ROUND, c.selected ? 2 : 1);
            fl_line(ax, ay, bx, by);
        }
        fl_line_style(0);
    }

    bool flashing = false, animating = false;
    for (auto& b : p.boxes) {
        drawBox(b.get());
        if (b->flashUntil > now()) flashing = true;
        if (b->animated()) animating = true;
    }
    if ((flashing || animating) && !Fl::has_timeout(flashTimer, this))
        Fl::add_timeout(animating ? 1.0 / 30.0 : 0.05, flashTimer, this);

    if (drag_ == Drag::Cable && dragBox_) {
        int ax, ay;
        portPos(dragBox_, true, dragPort_, ax, ay);
        fl_color(kSelect);
        fl_line_style(FL_SOLID, 2);
        fl_line(x() + ax + kPortW / 2, y() + ay, x() + curX_, y() + curY_);
        fl_line_style(0);
    } else if (drag_ == Drag::Rubber) {
        fl_color(kSelect);
        fl_line_style(FL_DOT, 1);
        int rx = std::min(pressX_, curX_), ry = std::min(pressY_, curY_);
        fl_rect(x() + rx, y() + ry, std::abs(curX_ - pressX_), std::abs(curY_ - pressY_));
        fl_line_style(0);
    }

    if (editor_->visible()) {
        draw_child(*editor_);
        fl_color(kAccent);                       // outline: the box being typed into
        fl_rounded_rect(editor_->x() - 1, editor_->y() - 1, editor_->w() + 2, editor_->h() + 2, 3);
    }
    drawSuggestions();
    drawPortTip();
    fl_pop_clip();
}

Canvas::Hit Canvas::hitTest(int mx, int my) const {
    Hit h;
    auto& boxes = patch_->boxes;
    for (auto it = boxes.rbegin(); it != boxes.rend(); ++it) {
        Box* b = it->get();
        if (mx < b->x - 2 || mx > b->x + b->w + 2 || my < b->y - 2 || my > b->y + b->h + 2)
            continue;
        h.box = b;
        h.what = Hit::Body;
        if (my >= b->y + b->h - 5) {
            for (int k = 0; k < b->numOuts(); ++k) {
                int px, py;
                portPos(b, true, k, px, py);
                if (mx >= px - 3 && mx <= px + kPortW + 3) {
                    h.what = Hit::Outlet;
                    h.port = k;
                    break;
                }
            }
        }
        return h;
    }
    auto& conns = patch_->conns;
    for (int k = (int)conns.size() - 1; k >= 0; --k) {
        auto& c = conns[k];
        int ax, ay, bx, by;
        portPos(c.from, true, c.outlet, ax, ay);
        portPos(c.to, false, c.inlet, bx, by);
        if (segDist(mx, my, ax + kPortW / 2, ay, bx + kPortW / 2, by) < 4.0) {
            h.what = Hit::Cable;
            h.cable = k;
            return h;
        }
    }
    return h;
}

int Canvas::inletAt(Box* b, int mx) const {
    int best = -1, bestD = 1 << 30;
    for (int k = 0; k < b->numIns(); ++k) {
        int px, py;
        portPos(b, false, k, px, py);
        int d = std::abs(mx - (px + kPortW / 2));
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

void Canvas::clearSelection() {
    for (auto& b : patch_->boxes) b->selected = false;
    for (auto& c : patch_->conns) c.selected = false;
}

std::vector<Box*> Canvas::selectedBoxes() const {
    std::vector<Box*> s;
    for (auto& b : patch_->boxes)
        if (b->selected) s.push_back(b.get());
    return s;
}

void Canvas::selectAll() {
    if (locked_) return;
    for (auto& b : patch_->boxes) b->selected = true;
    redraw();
}

void Canvas::reset() {
    hidePortTip();
    if (editor_->visible()) editor_->hide();
    editing_ = nullptr;
    sugg_.clear();
    dragBox_ = nullptr;
    typingBox_ = nullptr;
    typed_.clear();
    drag_ = Drag::None;
    redraw();
}

void Canvas::setLocked(bool l) {
    if (readOnly && !l) return;
    hidePortTip();
    commitEdit();
    locked_ = l;
    clearSelection();
    typingBox_ = nullptr;
    typed_.clear();
    redraw();
    if (win_) win_->refreshStatus();      // none when drawing a --render snapshot
}

void Canvas::beginEdit(Box* b, bool isNew) {
    editing_ = b;
    editingNew_ = isNew;
    sugg_.clear();
    std::string t = b->label();
    bool content = b->kind == BoxKind::Message || b->kind == BoxKind::Comment;
    if (isNew && !content && !t.empty()) t += " ";
    bool multi = b->kind == BoxKind::Message;   // edited as shown, one line per ";"
    if (multi) {
        auto lines = messageLines(t);
        t.clear();
        for (size_t k = 0; k < lines.size(); ++k) t += (k ? "\n" : "") + lines[k];
    }
    editor_->type(multi ? FL_MULTILINE_INPUT : FL_NORMAL_INPUT);
    editor_->value(t.c_str());
    fitEditor();
    editor_->show();
    editor_->take_focus();
    editor_->insert_position((int)t.size());
    redraw();
}

void Canvas::fitEditor() {
    if (!editing_) return;
    Box* b = editing_;
    int W = std::max(b->w, 160), H = kBoxH;
    if (editor_->input_type() == FL_MULTILINE_INPUT) {
        auto lines = splitLines(editor_->value());
        W = std::max(W, widestLine(lines) + 24);
        H = kBoxH + ((int)lines.size() - 1) * lineHeight();
    }
    if (W != editor_->w() || H != editor_->h()) {
        editor_->resize(x() + b->x, y() + b->y, W, H);
        redraw();
    }
}

void Canvas::editCb(Fl_Widget*, void* data) { static_cast<Canvas*>(data)->commitEdit(); }

void Canvas::commitEdit() {
    if (!editing_) return;
    Box* b = editing_;
    editing_ = nullptr;
    sugg_.clear();
    std::string t = editor_->value();
    editor_->hide();
    std::replace(t.begin(), t.end(), '\n', ' ');   // message lines are display only
    // trim
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    Patch& p = *patch_;

    if (b->kind == BoxKind::Comment && t.empty() && editingNew_) {
        p.removeBoxes({b});
        win_->dropUndo();
    } else if (b->kind == BoxKind::Message || b->kind == BoxKind::Comment) {
        // message and comment boxes keep their text verbatim
        if (splitTokens(t) != splitTokens(b->label()) ||
            (b->kind == BoxKind::Comment && t != b->label())) {
            if (!editingNew_) win_->pushUndo();
            p.replaceBox(b, p.create(b->cls, {Atom(t)}));
            win_->markDirty();
        }
    } else if (t.empty()) {
        if (editingNew_) {
            p.removeBoxes({b});
            win_->dropUndo();
        }
    } else if (editingNew_ || t != b->label()) {
        if (!editingNew_) win_->pushUndo();
        Message m = parseMessage(t);
        std::string cls = m[0].str();
        auto made = p.create(cls, Message(m.begin() + 1, m.end()));
        if (b->savesInner() && made->cls == "p") {     // renaming a p keeps its contents
            std::vector<SExpr> form;
            std::string err;
            std::vector<std::string> warnings;
            if (parseSExprs(b->inner()->serialize(), form, err) && form.size() == 1)
                made->loadInner(form[0], warnings);
        }
        Box* nb = p.replaceBox(b, std::move(made));
        if (nb->kind == BoxKind::Broken) win_->print("unknown object: " + cls + " (no class and no " + cls + ".moritz next to the patch)");
        else if (nb->inner()) nb->loadbang();       // a new abstraction starts like a loaded one
        win_->markDirty();
    }
    Fl::focus(this);
    redraw();
}

void Canvas::cancelEdit() {
    if (!editing_) return;
    Box* b = editing_;
    editing_ = nullptr;
    sugg_.clear();
    editor_->hide();
    if (editingNew_ && b->kind != BoxKind::Message) {
        patch_->removeBoxes({b});
        win_->dropUndo();
    }
    Fl::focus(this);
    redraw();
}

void Canvas::newBoxAt(const std::string& cls, int bx, int by) {
    commitEdit();
    if (locked_) setLocked(false);
    clearSelection();
    win_->pushUndo();
    Patch& p = *patch_;
    Box* b = p.addBox(makeBox(cls, {}, patch_), std::max(0, bx), std::max(0, by));
    layoutBox(b);
    win_->markDirty();
    if (b->kind == BoxKind::Number || b->kind == BoxKind::UI) {
        b->selected = true;                 // ready to use, nothing to type
        redraw();
    } else {
        beginEdit(b, true);
    }
}

void Canvas::newBoxAtMouse(const std::string& cls) { newBoxAt(cls, mouseX_, mouseY_); }

void Canvas::deleteSelection() {
    Patch& p = *patch_;
    std::vector<int> cables;
    for (int k = 0; k < (int)p.conns.size(); ++k)
        if (p.conns[k].selected) cables.push_back(k);
    auto bs = selectedBoxes();
    if (cables.empty() && bs.empty()) return;
    win_->pushUndo();
    if (!cables.empty()) p.removeConnections(cables);
    if (!bs.empty()) p.removeBoxes(bs);
    dragBox_ = typingBox_ = nullptr;
    win_->markDirty();
    redraw();
}

void Canvas::numberDrag(int dy) {
    auto nb = dynamic_cast<NumberBox*>(dragBox_);
    if (!nb) return;
    double v = dragStartValue_ + dy * dragStep_;
    if (dragStep_ < 1.0) v = std::round(v / dragStep_) * dragStep_;
    nb->set(v, true);
}

int Canvas::handle(int e) {
    switch (e) {
        case FL_LEAVE:
            hidePortTip();
            return 1;
        case FL_ENTER:
        case FL_FOCUS:
        case FL_UNFOCUS:
            return 1;
        case FL_MOVE:
            mouseX_ = Fl::event_x() - x();
            mouseY_ = Fl::event_y() - y();
            updatePortTip(mouseX_, mouseY_);
            return 1;

        case FL_PUSH: {
            hidePortTip();
            if (editor_->visible() && Fl::event_inside(editor_)) return Fl_Group::handle(e);
            int pick = editing_ ? suggestionAt(Fl::event_x(), Fl::event_y()) : -1;
            if (pick >= 0) {                                // click on a completion
                suggSel_ = pick;
                acceptSuggestion(false);
                editor_->take_focus();
                return 1;
            }
            commitEdit();
            Fl::focus(this);
            int mx = Fl::event_x() - x(), my = Fl::event_y() - y();
            pressX_ = curX_ = mouseX_ = mx;
            pressY_ = curY_ = mouseY_ = my;
            moved_ = false;
            Hit h = hitTest(mx, my);
            bool shift = Fl::event_state(FL_SHIFT);

            // right-click (or Ctrl-click) on a box: its menu; Option-click in
            // locked mode: its help, as in Max
            bool menuClick = Fl::event_button() == FL_RIGHT_MOUSE ||
                             (Fl::event_button() == FL_LEFT_MOUSE && Fl::event_state(FL_CTRL));
            if (h.box && win_ && (menuClick || (locked_ && Fl::event_state(FL_ALT)))) {
                Box* b = h.box;
                if (!menuClick) { win_->openHelp(b->cls); return 1; }
                std::string helpLabel = "Help for " + b->cls;
                for (size_t k = 0; k < helpLabel.size(); ++k)     // menu labels treat / and & specially
                    if (helpLabel[k] == '/' || helpLabel[k] == '&') helpLabel.insert(k++, 1, '\\');
                Fl_Menu_Item items[3] = {};
                items[0].label(helpLabel.c_str());
                items[1].label("Open");
                if (!b->inner()) items[1].deactivate();
                const Fl_Menu_Item* r = items->popup(Fl::event_x(), Fl::event_y());
                if (r == &items[0]) win_->openHelp(b->cls);
                else if (r == &items[1]) win_->openSubpatch(b);
                return 1;
            }

            if (locked_) {
                typingBox_ = nullptr;
                typed_.clear();
                if (h.box && h.box->kind == BoxKind::Message) {
                    static_cast<MessageBox*>(h.box)->trigger({});
                } else if (h.box && h.box->kind == BoxKind::Number) {
                    auto nb = static_cast<NumberBox*>(h.box);
                    drag_ = Drag::Number;
                    dragBox_ = nb;
                    typingBox_ = nb;
                    dragStartValue_ = nb->value();
                    dragStep_ = 1.0;
                    if (nb->isFloat()) {
                        std::string t = nb->text();
                        size_t dot = t.find('.');
                        fl_font(kFont, kFontSize);
                        int dotX = nb->x + 16 + (int)fl_width(t.substr(0, dot).c_str());
                        if (dot != std::string::npos && mx > dotX) dragStep_ = 0.01;
                    }
                } else if (h.box && h.box->kind == BoxKind::UI) {
                    drag_ = Drag::UI;
                    dragBox_ = h.box;
                    h.box->uiMouse(FL_PUSH, mx - h.box->x, my - h.box->y);
                } else if (h.box && h.box->cls == "loadbang" && Fl::event_clicks()) {
                    h.box->loadbang();
                } else if (h.box && h.box->inner() && Fl::event_clicks()) {
                    Fl::event_clicks(0);
                    drag_ = Drag::None;
                    win_->openSubpatch(h.box);     // like Max: double-click opens it
                    return 1;
                }
                redraw();
                return 1;
            }

            if (Fl::event_clicks()) {                       // double-click
                if (h.box && h.box->inner() && Fl::event_state(FL_COMMAND)) {
                    Fl::event_clicks(0);
                    win_->openSubpatch(h.box);              // Cmd+double-click opens in edit mode
                    return 1;
                }
                if (h.box && h.what == Hit::Body && h.box->kind != BoxKind::Number) {
                    Fl::event_clicks(0);
                    beginEdit(h.box, false);
                    return 1;
                }
                if (h.what == Hit::None) {
                    Fl::event_clicks(0);
                    newBoxAt("", mx, my - kBoxH / 2);
                    return 1;
                }
            }
            switch (h.what) {
                case Hit::Outlet:
                    drag_ = Drag::Cable;
                    dragBox_ = h.box;
                    dragPort_ = h.port;
                    break;
                case Hit::Body:
                    if (Fl::event_state(FL_ALT)) {          // option-drag copies
                        if (!h.box->selected) { clearSelection(); h.box->selected = true; }
                        duplicateSelection();
                        drag_ = Drag::Move;
                        moved_ = true;                      // undo already recorded
                        break;
                    }
                    if (shift) h.box->selected = !h.box->selected;
                    else if (!h.box->selected) { clearSelection(); h.box->selected = true; }
                    if (h.box->selected) drag_ = Drag::Move;
                    break;
                case Hit::Cable:
                    if (!shift) clearSelection();
                    patch_->conns[h.cable].selected = true;
                    break;
                default:
                    if (!shift) clearSelection();
                    drag_ = Drag::Rubber;
            }
            redraw();
            return 1;
        }

        case FL_DRAG: {
            int mx = Fl::event_x() - x(), my = Fl::event_y() - y();
            int dx = mx - curX_, dy = my - curY_;
            curX_ = mouseX_ = mx;
            curY_ = mouseY_ = my;
            if (drag_ == Drag::Move && (dx || dy)) {
                if (!moved_) { win_->pushUndo(); moved_ = true; }
                for (Box* b : selectedBoxes()) {
                    b->x = std::max(0, b->x + dx);
                    b->y = std::max(0, b->y + dy);
                }
                win_->markDirty();
            } else if (drag_ == Drag::Number) {
                if (my != pressY_) { moved_ = true; typed_.clear(); }
                numberDrag(pressY_ - my);
            } else if (drag_ == Drag::UI && dragBox_) {
                dragBox_->uiMouse(FL_DRAG, mx - dragBox_->x, my - dragBox_->y);
            }
            redraw();
            return 1;
        }

        case FL_RELEASE: {
            int mx = Fl::event_x() - x(), my = Fl::event_y() - y();
            if (drag_ == Drag::UI && dragBox_)
                dragBox_->uiMouse(FL_RELEASE, mx - dragBox_->x, my - dragBox_->y);
            if (drag_ == Drag::Cable && dragBox_) {
                Hit h = hitTest(mx, my);
                if (h.box && h.box != dragBox_) {
                    int in = inletAt(h.box, mx);
                    std::string err;
                    Patch& p = *patch_;
                    if (canConnect(p.conns, dragBox_, dragPort_, h.box, in, &err)) {
                        win_->pushUndo();
                        if (p.connect(dragBox_, dragPort_, h.box, in, &err)) win_->markDirty();
                        else { win_->dropUndo(); win_->print("can't connect: " + err); }
                    } else if (err != "already connected") {
                        win_->print("can't connect: " + err);
                    }
                }
            } else if (drag_ == Drag::Rubber) {
                int x0 = std::min(pressX_, mx), x1 = std::max(pressX_, mx);
                int y0 = std::min(pressY_, my), y1 = std::max(pressY_, my);
                for (auto& b : patch_->boxes)
                    if (b->x < x1 && b->x + b->w > x0 && b->y < y1 && b->y + b->h > y0)
                        b->selected = true;
            }
            if (drag_ == Drag::Move && moved_ && patch_->owner)
                patch_->rebuild();                  // inlets/outlets are ordered by position
            drag_ = Drag::None;
            if (moved_) typingBox_ = nullptr;
            dragBox_ = nullptr;
            redraw();
            return 1;
        }

        case FL_KEYBOARD:
        case FL_SHORTCUT: {
            if (editing_) return Fl_Group::handle(e);
            if (Fl::event_state(FL_COMMAND | FL_CTRL | FL_ALT)) return 0;
            int key = Fl::event_key();
            const char* txt = Fl::event_text();
            char ch = (txt && txt[0] && !txt[1]) ? txt[0] : 0;

            if (locked_) {
                if (!typingBox_) return 0;
                auto nb = static_cast<NumberBox*>(typingBox_);
                if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
                    typed_ += ch;
                } else if (key == FL_BackSpace && !typed_.empty()) {
                    typed_.pop_back();
                } else if ((key == FL_Enter || key == FL_KP_Enter) && !typed_.empty()) {
                    nb->set(std::atof(typed_.c_str()), true);
                    typed_.clear();
                } else if (key == FL_Escape) {
                    typed_.clear();
                } else {
                    return 0;
                }
                redraw();
                return 1;
            }

            if (key == FL_BackSpace || key == FL_Delete) { deleteSelection(); return 1; }
            if (e == FL_SHORTCUT && Fl::focus() != this) return 0;
            switch (ch) {                                   // Max's patcher shortcuts
                case 'n': newBoxAtMouse(""); return 1;
                case 'm': newBoxAtMouse("message"); return 1;
                case 'f': newBoxAtMouse("flonum"); return 1;
                case 'i': newBoxAtMouse("number"); return 1;
                case 'b': newBoxAtMouse("button"); return 1;
                case 't': newBoxAtMouse("toggle"); return 1;
                case 'c': newBoxAtMouse("comment"); return 1;
            }
            return 0;
        }
    }
    return Fl_Group::handle(e);
}


}  // namespace moritz
