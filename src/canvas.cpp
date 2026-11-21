// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// canvas.cpp — the patch editor: drawing boxes and cables, editing,
// completion, and running interface objects in locked mode.

#include "gui.h"
#include "zoom.h"
#include "objects.h"
#include "roll.h"
#include "settings.h"
#include "score_window.h"
#include "timeline.h"
#include "transport.h"

#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <climits>
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

// A cable from an outlet at (ax, ay) to an inlet at (bx, by), as points: a cubic Bézier that leaves the
// outlet downwards and enters the inlet from above, bending more the further apart they are, and
// looping out when the inlet is above the outlet; or the straight segment, as the settings say.
const std::vector<std::pair<double, double>>& cablePath(double ax, double ay, double bx, double by) {
    static std::vector<std::pair<double, double>> pts;
    pts.clear();
    if (!settings().curvedCables) {
        pts = {{ax, ay}, {bx, by}};
        return pts;
    }
    double dx = bx - ax, dy = by - ay;
    double bend = dy >= 0 ? std::clamp(dy * 0.45 + std::fabs(dx) * 0.12, 10.0, 140.0)
                          : std::clamp(-dy * 0.2 + std::fabs(dx) * 0.08 + 24.0, 24.0, 110.0);
    double c1x = ax, c1y = ay + bend, c2x = bx, c2y = by - bend;
    int n = std::clamp((int)((std::fabs(dx) + std::fabs(dy) + 2 * bend) / 8), 8, 80);
    for (int i = 0; i <= n; ++i) {
        double t = (double)i / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        pts.push_back({a * ax + b * c1x + c * c2x + d * bx, a * ay + b * c1y + c * c2y + d * by});
    }
    return pts;
}

double cableDist(double px, double py, double ax, double ay, double bx, double by) {
    const auto& pts = cablePath(ax, ay, bx, by);
    double best = 1e30;
    for (size_t k = 1; k < pts.size(); ++k)
        best = std::min(best, segDist(px, py, pts[k - 1].first, pts[k - 1].second, pts[k].first, pts[k].second));
    return best;
}

// the segment (ax, ay)-(bx, by) passes through the rectangle x0..x1, y0..y1 (Liang-Barsky)
bool segCrossesRect(double ax, double ay, double bx, double by, double x0, double y0, double x1, double y1) {
    double t0 = 0, t1 = 1, dx = bx - ax, dy = by - ay;
    const double p[4] = {-dx, dx, -dy, dy}, q[4] = {ax - x0, x1 - ax, ay - y0, y1 - ay};
    for (int k = 0; k < 4; ++k) {
        if (p[k] == 0) {
            if (q[k] < 0) return false;
            continue;
        }
        double t = q[k] / p[k];
        if (p[k] < 0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    return true;
}

bool sameCable(const Connection& a, const Connection& b) {
    return a.from == b.from && a.outlet == b.outlet && a.to == b.to && a.inlet == b.inlet;
}

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
    zd::font(kFont, kFontSize);
    return zd::height();
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
    zd::font(kFont, kFontSize);
    int w = 0;
    for (auto& l : lines) w = std::max(w, (int)zd::width(l.c_str()));
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

Canvas::Canvas(int x, int y, int w, int h, PatchEditor* editor, Patch* patch)
    : Fl_Group(x, y, w, h), ed_(editor ? editor : &DetachedEditor::instance()), patch_(patch) {
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
    hbar_ = new Fl_Scrollbar(x, y + h - kScrollW, w, kScrollW);
    hbar_->type(FL_HORIZONTAL);
    vbar_ = new Fl_Scrollbar(x + w - kScrollW, y, kScrollW, h);
    for (Fl_Scrollbar* b : {hbar_, vbar_}) {
        b->color(fl_rgb_color(34, 35, 40));
        b->selection_color(fl_rgb_color(90, 94, 104));
        b->box(FL_FLAT_BOX);
        b->slider(FL_FLAT_BOX);
        b->hide();
        b->callback([](Fl_Widget* w, void* d) {
            auto* c = static_cast<Canvas*>(d);
            auto* sb = static_cast<Fl_Scrollbar*>(w);
            if (sb->type() == FL_HORIZONTAL) c->scrollTo(sb->value(), c->scrollY());
            else c->scrollTo(c->scrollX(), sb->value());
        }, this);
    }
    end();
    editor_->hide();      // last: hiding may send an event to the canvas at once (see handle)
}

Canvas::~Canvas() {
    // FLTK keeps a timeout until it fires: none may find this canvas gone
    Fl::remove_timeout(flashTimer, this);
    Fl::remove_timeout(tipTimer, this);
}

void Canvas::detach() {
    Fl::remove_timeout(flashTimer, this);
    Fl::remove_timeout(tipTimer, this);
    editor_->hide();
    patch_ = nullptr;
    editing_ = dragBox_ = typingBox_ = tipBox_ = nullptr;
    drag_ = Drag::None;
    moveFrom_.clear();
    spliceCable_ = -1;
    sugg_.clear();
    layouts_.clear();
}

Box* Canvas::alive(Box* b) const {
    if (!b || !patch_) return nullptr;
    for (auto& x : patch_->boxes)
        if (x.get() == b) return b;
    return nullptr;
}

// Flashing and animated boxes: only their areas are drawn again (the rest of the canvas is kept).
void Canvas::flashTimer(void* data) {
    auto* c = static_cast<Canvas*>(data);
    if (!c->patch_) return;
    double t = now();
    bool any = false;
    for (auto& b : c->patch_->boxes)
        if (b->animated() || b->flashUntil > t - 0.2) {     // a flash just over: once more, without it
            c->damageBox(b.get());
            any = true;
        }
    if (!any) c->redraw();
}

void Canvas::damageBox(Box* b) {
    int X = screenX(b->x) - 4, Y = screenY(b->y) - 4;
    int W = (int)std::ceil(b->w * zoom_) + 9, H = (int)std::ceil(b->h * zoom_) + 9;
    damage(FL_DAMAGE_USER1, X, Y, W, H);
}

void layoutPatch(Patch* p) {
    for (auto& b : p->boxes) layoutBox(b.get());
}

void Canvas::layout(Box* b) {
    if (b->kind == BoxKind::UI) {                  // its own size, which may follow its state
        b->h = kBoxH;
        b->uiSize(b->w, b->h);
        return;
    }
    Layout& l = layouts_[b];
    std::string t = b->label();
    int ports = b->numIns() * 1024 + b->numOuts();
    if (l.kind == (int)b->kind && l.ports == ports && l.zoom == zd::zoom() && l.settings == settingsGeneration() &&
        l.label == t) {
        b->w = l.w;
        b->h = l.h;
        return;
    }
    layoutBox(b);
    l = Layout{std::move(t), (int)b->kind, ports, b->w, b->h, zd::zoom(), settingsGeneration()};
}

void layoutBox(Box* b) {
    zd::font(kFont, kFontSize);
    std::string t = b->label();
    int tw = (int)zd::width(t.c_str());
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

// the buffer~ a box shows or plays: its first argument, when a buffer~ has that name
std::string Canvas::bufferOf(Box* b) const {
    if (b->kind == BoxKind::Comment || b->kind == BoxKind::Message || b->args.empty() || !b->args[0].isSymbol()) return "";
    if (b->cls == "buffer~") return b->args[0].s;
    return findBuffer(patch_, b->args[0].s) ? b->args[0].s : "";
}

void Canvas::drawBox(Box* b) {
    int X = ox() + b->x, Y = oy() + b->y, W = b->w, H = b->h;
    bool flash = b->flashUntil > now();

    if (b->kind == BoxKind::Comment) {               // comments: just text
        if (b->selected || !locked_) {
            fl_color(b->selected ? kSelect : kGrid);
            zd::line_style(FL_DOT, 1);
            zd::rounded_rect(X, Y, W, H, 4);
            zd::line_style(0);
        }
        if (editing_ != b) {
            zd::font(kFont, kFontSize);
            fl_color(kDim);
            zd::draw(b->label().c_str(), X + 6, Y + H / 2 + zd::height() / 2 - zd::descent());
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
    zd::rounded_rectf(X, Y, W, H, r);

    if (b->kind == BoxKind::UI) {
        zd::push_clip(X, Y, W, H);
        b->drawUI(X, Y);
        zd::pop_clip();
        if (!locked_ && b->uiResizable()) {               // the corner it is resized by
            fl_color(kDim);
            for (int k = 0; k < 3; ++k) zd::line(X + W - 2 - 3 * k, Y + H - 7, X + W - 2, Y + H - 7 - 3 * k);
        }
    }

    // border
    if (b->selected) {
        fl_color(kSelect);
        zd::line_style(FL_SOLID, 2);
    } else if (b->kind == BoxKind::Broken) {
        fl_color(fl_rgb_color(230, 80, 80));
        zd::line_style(FL_DASH, 1);
    } else {
        fl_color(b->isSignal() ? fl_color_average(kSignal, kBorder, 0.25f) : kBorder);
        zd::line_style(FL_SOLID, 1);
    }
    zd::rounded_rect(X, Y, W, H, r);
    zd::line_style(0);

    // text
    if (editing_ != b && b->kind != BoxKind::UI) {
        zd::font(kFont, kFontSize);
        int tx = X + (b->kind == BoxKind::Message ? 10 : 8);
        std::string t = b->label();
        if (b->kind == BoxKind::Number) {
            fl_color(kAccent);
            zd::polygon(X + 6, Y + 7, X + 11, Y + H / 2, X + 6, Y + H - 7);
            tx = X + 16;
            if (typingBox_ == b && !typed_.empty()) t = typed_ + "_";
        }
        fl_color(kText);
        zd::push_clip(X + 2, Y + 1, W - 4, H - 2);
        if (b->kind == BoxKind::Message) {
            int base = Y + kBoxH / 2 + zd::height() / 2 - zd::descent(), lh = zd::height();
            for (auto& line : messageLines(t)) {
                zd::draw(line.c_str(), tx, base);
                base += lh;
            }
        } else {
            zd::draw(t.c_str(), tx, Y + H / 2 + zd::height() / 2 - zd::descent());
        }
        zd::pop_clip();
    }

    // ports: small tabs on the top (inlets) and bottom (outlets) edges
    for (int k = 0; k < b->numIns(); ++k) {
        int px, py;
        portPos(b, false, k, px, py);
        fl_color(b->ins[k] == Port::Control ? kCtlPort : kSignal);
        zd::rounded_rectf(ox() + px, oy() + py, kPortW, kPortH + 1, 1);
    }
    for (int k = 0; k < b->numOuts(); ++k) {
        int px, py;
        portPos(b, true, k, px, py);
        fl_color(b->outs[k] == Port::Control ? kCtlPort : kSignal);
        zd::rounded_rectf(ox() + px, oy() + py - kPortH - 1, kPortW, kPortH + 1, 1);
    }
}

// ---- completion popup

void Canvas::updateSuggestions() {
    sugg_.clear();
    suggSel_ = 0;
    if (alive(editing_) && editing_->kind != BoxKind::Message && editing_->kind != BoxKind::Comment) {
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
    zd::font(kFont, kFontSize);
    int nameW = 0;
    for (auto& s : sugg_) nameW = std::max(nameW, (int)zd::width(s.c_str()));
    int descW = 0;
    for (auto& s : sugg_) descW = std::max(descW, (int)zd::width(describeClass(s)));
    suggW_ = std::max(editor_->w(), nameW + descW + 40);
    int H = (int)sugg_.size() * kSuggRowH + 6;
    suggX_ = editor_->x();
    suggY_ = editor_->y() + editor_->h() + 3;
    if (suggY_ + H > y() + h()) suggY_ = editor_->y() - H - 3;   // flip above near the bottom

    fl_color(fl_rgb_color(10, 10, 12));                              // soft shadow
    zd::rounded_rectf(suggX_ + 2, suggY_ + 3, suggW_, H, 6);
    fl_color(kPopup);
    zd::rounded_rectf(suggX_, suggY_, suggW_, H, 6);
    fl_color(kBorder);
    zd::rounded_rect(suggX_, suggY_, suggW_, H, 6);
    for (int k = 0; k < (int)sugg_.size(); ++k) {
        int ry = suggY_ + 3 + k * kSuggRowH;
        if (k == suggSel_) {
            fl_color(fl_rgb_color(52, 92, 160));
            zd::rounded_rectf(suggX_ + 3, ry, suggW_ - 6, kSuggRowH, 4);
        }
        int base = ry + kSuggRowH / 2 + zd::height() / 2 - zd::descent();
        fl_color(kText);
        zd::draw(sugg_[k].c_str(), suggX_ + 10, base);
        fl_color(k == suggSel_ ? fl_rgb_color(200, 215, 240) : kDim);
        zd::draw(describeClass(sugg_[k]), suggX_ + 26 + nameW, base);
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
    if (!c->tipBox_ || !c->patch_) return;
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
    if (!tipShown_ || !alive(tipBox_)) return;
    if (tipPort_ >= (tipOut_ ? tipBox_->numOuts() : tipBox_->numIns())) return;
    std::string help = portHelp(tipBox_, tipOut_, tipPort_);
    std::string title = help.substr(0, help.find('\n'));
    std::string body = help.find('\n') == std::string::npos ? "" : help.substr(help.find('\n') + 1);

    int px, py;
    portPos(tipBox_, tipOut_, tipPort_, px, py);
    px += ox();
    py += oy();
    fl_color(kAccent);                                  // ring around the port
    zd::line_style(FL_SOLID, 2);
    zd::rounded_rect(px - 3, (tipOut_ ? py - kPortH - 4 : py - 3), kPortW + 6, kPortH + 7, 3);
    zd::line_style(0);

    zd::font(FL_HELVETICA_BOLD, 11);
    int tw = (int)zd::width(title.c_str());
    zd::font(kFont, 12);
    int bw = (int)zd::width(body.c_str());
    int W = std::max(tw, bw) + 20, H = body.empty() ? 24 : 42;
    int X = std::clamp(px - 12, x() + 4, x() + w() - W - 4);
    int Y = tipOut_ ? py + 8 : py - H - 8;
    if (Y < y() + 4) Y = py + 12;
    if (Y + H > y() + h() - 4) Y = py - H - 12;
    fl_color(fl_rgb_color(10, 10, 12));
    zd::rounded_rectf(X + 2, Y + 3, W, H, 6);
    fl_color(kPopup);
    zd::rounded_rectf(X, Y, W, H, 6);
    fl_color(kBorder);
    zd::rounded_rect(X, Y, W, H, 6);
    zd::font(FL_HELVETICA_BOLD, 11);
    fl_color(tipBox_->outs.empty() && tipOut_ ? kDim : kAccent);
    zd::draw(title.c_str(), X + 10, Y + 16);
    if (!body.empty()) {
        zd::font(kFont, 12);
        fl_color(kText);
        zd::draw(body.c_str(), X + 10, Y + 34);
    }
}

// ---- option-drag duplication

void Canvas::duplicateSelection(int dx, int dy) {
    auto sel = selectedBoxes();
    if (sel.empty() || readOnly()) return;
    ed_->pushUndo(patch_);
    std::vector<Box*> added;
    std::string err;
    // through the clipboard format, so subpatchers are copied with their contents
    if (!patch_->merge(patch_->serializeBoxes(sel), dx, dy, added, err)) { ed_->dropUndo(); return; }
    clearSelection();
    for (Box* b : added) b->selected = true;
    if (patch_->owner) patch_->rebuild();       // inlets/outlets follow positions
    ed_->markDirty(patch_);
    redraw();
}

void Canvas::copySelection() {
    if (editing_) { editor_->copy(1); return; }
    auto sel = selectedBoxes();
    if (sel.empty()) return;
    std::string t = patch_->serializeBoxes(sel);
    if (batchMode()) batchClipboard() = t;
    else Fl::copy(t.c_str(), (int)t.size(), 1);
    ed_->setStatus("copied " + std::to_string(sel.size()) + (sel.size() == 1 ? " box" : " boxes"));
}

void Canvas::cutSelection() {
    if (editing_) { editor_->copy(1); editor_->cut(); return; }
    if (readOnly() || selectedBoxes().empty()) return;
    copySelection();
    deleteSelection();
}

void Canvas::paste() {
    if (editing_) { Fl::paste(*editor_, 1); return; }
    if (readOnly()) return;
    if (batchMode()) { pasteText(batchClipboard()); return; }
    Fl::paste(*this, 1, Fl::clipboard_plain_text);   // arrives as FL_PASTE
}

// Pastes boxes copied from any patch window (or a .moritz text from another
// program) with their top-left corner at the mouse.
void Canvas::pasteText(const std::string& text) {
    size_t k = text.find_first_not_of(" \t\r\n");
    while (k != std::string::npos && text[k] == ';') {          // skip comment lines
        k = text.find('\n', k);
        if (k != std::string::npos) k = text.find_first_not_of(" \t\r\n", k);
    }
    if (readOnly() || !patch_) return;
    if (k == std::string::npos || text.compare(k, 6, "(patch") != 0) {
        ed_->setStatus("nothing to paste: the clipboard holds no Moritz boxes");
        return;
    }
    ed_->pushUndo(patch_);
    std::vector<Box*> added;
    std::string err;
    if (!patch_->merge(text.substr(k), 0, 0, added, err) || added.empty()) {
        ed_->dropUndo();
        ed_->print("paste: " + (err.empty() ? std::string("no boxes") : err));
        return;
    }
    int minX = added[0]->x, minY = added[0]->y;
    for (Box* b : added) { minX = std::min(minX, b->x); minY = std::min(minY, b->y); }
    int tx = std::max(5, mouseX_), ty = std::max(5, mouseY_);
    clearSelection();
    for (Box* b : added) {
        b->x += tx - minX;
        b->y += ty - minY;
        b->selected = true;
    }
    if (patch_->owner) patch_->rebuild();
    if (!err.empty()) ed_->print("paste: " + err);
    ed_->markDirty(patch_);
    redraw();
}

void Canvas::draw() {
    if (!patch_) {                                  // detached: the patch is gone
        draw_box();
        return;
    }
    Patch& p = *patch_;
    zd::push_clip(x(), y(), w(), h());
    // what is to be drawn: all of it, or (an animated box) a part; in patch units, with a margin
    int cx = x(), cy = y(), cw = w(), ch = h();
    fl_clip_box(x(), y(), w(), h(), cx, cy, cw, ch);
    const int vx0 = patchX(cx) - 2, vy0 = patchY(cy) - 2, vx1 = patchX(cx + cw) + 2, vy1 = patchY(cy + ch) + 2;
    auto visible = [&](int bx0, int by0, int bx1, int by1) { return bx1 >= vx0 && bx0 <= vx1 && by1 >= vy0 && by0 <= vy1; };
    fl_color(locked_ ? kBgLocked : kBgEdit);
    zd::rectf(x(), y(), w(), h());
    {
    zd::Scope zoomed(x(), y(), zoom_);           // the patch, zoomed around the canvas's corner
    if (!locked_ && zoom_ >= 0.5) {              // the grid, where it is drawn
        fl_color(kGrid);
        int gx0 = std::max(sx_, vx0), gy0 = std::max(sy_, vy0);
        int gx1 = std::min(sx_ + viewW(), vx1), gy1 = std::min(sy_ + viewH(), vy1);
        int g0x = (int)std::floor(gx0 / 15.0) * 15, g0y = (int)std::floor(gy0 / 15.0) * 15;
        for (int gx = g0x; gx < gx1; gx += 15)
            for (int gy = g0y; gy < gy1; gy += 15)
                zd::point(ox() + gx, oy() + gy);
    }

    for (auto& b : p.boxes) layout(b.get());
    if (layouts_.size() > 2 * p.boxes.size() + 64) {     // forget the boxes that are gone
        std::unordered_map<const Box*, Layout> kept;
        for (auto& b : p.boxes)
            if (auto it = layouts_.find(b.get()); it != layouts_.end()) kept.emplace(it->first, std::move(it->second));
        layouts_.swap(kept);
    }

    // cables (a curve stays between its ends across, and bends at most 140 up or down)
    bool endDragged = drag_ == Drag::CableEnd && moved_;
    int lit = drag_ == Drag::Move ? spliceCable_ : -1;           // where a box dropped with Shift goes
    for (int k = 0; k < (int)p.conns.size(); ++k) {
        const Connection& c = p.conns[k];
        if (endDragged && sameCable(c, dragConn_)) continue;      // drawn where its end is being taken
        int ax, ay, bx, by;
        cableEnds(c, ax, ay, bx, by);
        if (!visible(std::min(ax, bx) - 8, std::min(ay, by) - 150, std::max(ax, bx) + 8, std::max(ay, by) + 150))
            continue;
        ax += ox(); bx += ox();
        ay += oy(); by += oy();
        bool sig = c.from->outs[c.outlet] != Port::Control;   // signal and tap links
        const auto& path = cablePath(ax, ay, bx, by);
        if (k == lit) {
            fl_color(kAccent);
            zd::line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 4);
            zd::polyline(path);
        } else if (sig) {
            fl_color(c.selected ? kSelect : kSignal);
            zd::line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 3);
            zd::polyline(path);
            fl_color(c.selected ? fl_rgb_color(30, 70, 140) : kSignalDim);
            zd::line_style(FL_DASH, 1);
            zd::polyline(path);
        } else {
            fl_color(c.selected ? kSelect : kCable);
            zd::line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, c.selected ? 2 : 1);
            zd::polyline(path);
        }
        zd::line_style(0);
    }

    bool flashing = false, animating = false;
    double t = now();
    for (auto& b : p.boxes) {
        if (!visible(b->x - 4, b->y - 4, b->x + b->w + 4, b->y + b->h + 4)) continue;
        drawBox(b.get());
        if (b->flashUntil > t) flashing = true;
        if (b->animated()) animating = true;
    }
    if ((flashing || animating) && !Fl::has_timeout(flashTimer, this))
        Fl::add_timeout(animating ? 1.0 / 30.0 : 0.05, flashTimer, this);

    if (drag_ == Drag::Cable && alive(dragBox_)) {
        int ax, ay;
        portPos(dragBox_, true, dragPort_, ax, ay);
        fl_color(kSelect);
        zd::line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
        zd::polyline(cablePath(ox() + ax + kPortW / 2, oy() + ay, ox() + curX_, oy() + curY_));
        zd::line_style(0);
    } else if (endDragged && alive(dragConn_.from) && alive(dragConn_.to)) {
        int ax, ay, bx, by;
        cableEnds(dragConn_, ax, ay, bx, by);
        if (dragInletEnd_) { bx = curX_; by = curY_; }
        else { ax = curX_; ay = curY_; }
        fl_color(kSelect);
        zd::line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
        zd::polyline(cablePath(ox() + ax, oy() + ay, ox() + bx, oy() + by));
        zd::line_style(0);
    } else if (drag_ == Drag::Rubber) {
        fl_color(kSelect);
        zd::line_style(FL_DOT, 1);
        int rx = std::min(pressX_, curX_), ry = std::min(pressY_, curY_);
        zd::rect(ox() + rx, oy() + ry, std::abs(curX_ - pressX_), std::abs(curY_ - pressY_));
        zd::line_style(0);
    }

    drawPortTip();
    }
    if (editor_->visible()) {
        draw_child(*editor_);
        fl_color(kAccent);                       // outline: the box being typed into
        zd::rounded_rect(editor_->x() - 1, editor_->y() - 1, editor_->w() + 2, editor_->h() + 2, 3);
    }
    drawSuggestions();
    updateScrollbars();
    if (hbar_->visible()) draw_child(*hbar_);
    if (vbar_->visible()) draw_child(*vbar_);
    zd::pop_clip();
}

// ---- scrolling: the patch can be larger than the window

void Canvas::contentBounds(int& x0, int& y0, int& x1, int& y1) const {
    x0 = 0; y0 = 0; x1 = 0; y1 = 0;
    if (!patch_) return;
    for (auto& b : patch_->boxes) {
        x0 = std::min(x0, b->x - 20);
        y0 = std::min(y0, b->y - 20);
        x1 = std::max(x1, b->x + b->w + 40);
        y1 = std::max(y1, b->y + b->h + 40);
    }
}

void Canvas::updateScrollbars() {
    if (!scrollable) { hbar_->hide(); vbar_->hide(); return; }
    int x0, y0, x1, y1;
    contentBounds(x0, y0, x1, y1);
    // the range covers the patch and where the view is now (so it never jumps)
    x0 = std::min(x0, sx_); y0 = std::min(y0, sy_);
    x1 = std::max(x1, sx_ + viewW()); y1 = std::max(y1, sy_ + viewH());
    bool needH = x1 - x0 > viewW(), needV = y1 - y0 > viewH();
    const int S = kScrollW;
    int vw = (int)((w() - (needV ? S : 0)) / zoom_), vh = (int)((h() - (needH ? S : 0)) / zoom_);
    needH = x1 - x0 > vw; needV = y1 - y0 > vh;
    if (needH) {
        hbar_->resize(x(), y() + h() - S, w() - (needV ? S : 0), S);
        hbar_->value(sx_, vw, x0, x1 - x0);
        hbar_->linesize(30);
        hbar_->show();
    } else {
        hbar_->hide();
    }
    if (needV) {
        vbar_->resize(x() + w() - S, y(), S, h() - (needH ? S : 0));
        vbar_->value(sy_, vh, y0, y1 - y0);
        vbar_->linesize(30);
        vbar_->show();
    } else {
        vbar_->hide();
    }
}

void Canvas::scrollTo(int sx, int sy) {
    int x0, y0, x1, y1;
    contentBounds(x0, y0, x1, y1);
    // within the patch (and back to the origin), never past it
    sx = std::clamp(sx, std::min(x0, 0), std::max(x1 - viewW(), 0));
    sy = std::clamp(sy, std::min(y0, 0), std::max(y1 - viewH(), 0));
    if (sx == sx_ && sy == sy_) return;
    sx_ = sx;
    sy_ = sy;
    if (alive(editing_)) editor_->Fl_Widget::position(screenX(editing_->x), screenY(editing_->y));
    redraw();
}

// the zoom ladder, as in Max
static const double kZooms[] = {0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};

void Canvas::setZoom(double z) {
    z = std::clamp(z, kZooms[0], kZooms[std::size(kZooms) - 1]);
    if (std::fabs(z - zoom_) < 1e-6) return;
    commitEdit();
    // keep the patch point at the centre of the view where it is
    double cx = sx_ + w() / (2 * zoom_), cy = sy_ + h() / (2 * zoom_);
    zoom_ = z;
    sx_ = 0;
    sy_ = 0;
    scrollTo((int)std::lround(cx - w() / (2 * zoom_)), (int)std::lround(cy - h() / (2 * zoom_)));
    redraw();
    if (onZoom) onZoom();
}

void Canvas::zoomBy(int steps) {
    if (steps == 0) { setZoom(1.0); return; }
    size_t n = std::size(kZooms), k = 0;
    if (steps > 0) {
        while (k < n && kZooms[k] <= zoom_ + 1e-6) ++k;
        setZoom(kZooms[std::min(k, n - 1)]);
    } else {
        k = n;
        while (k > 0 && kZooms[k - 1] >= zoom_ - 1e-6) --k;
        setZoom(kZooms[k > 0 ? k - 1 : 0]);
    }
}

bool Canvas::uiUndo(bool redo) {
    Box* b = locked_ ? alive(typingBox_) : nullptr;
    if (!b || b->kind != BoxKind::UI || !b->uiUndo(redo)) return false;
    redraw();
    return true;
}

bool Canvas::inResizeGrip(Box* b, int mx, int my) const {
    // above the outlets' strip, at the right edge
    return mx >= b->x + b->w - 9 && mx <= b->x + b->w + 2 && my >= b->y + b->h - 17 && my < b->y + b->h - 5;
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
        int ax, ay, bx, by;
        cableEnds(conns[k], ax, ay, bx, by);
        if (cableDist(mx, my, ax, ay, bx, by) < 4.0) {
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

int Canvas::outletAt(Box* b, int mx) const {
    int best = -1, bestD = 1 << 30;
    for (int k = 0; k < b->numOuts(); ++k) {
        int px, py;
        portPos(b, true, k, px, py);
        int d = std::abs(mx - (px + kPortW / 2));
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

void Canvas::cableEnds(const Connection& c, int& ax, int& ay, int& bx, int& by) const {
    portPos(c.from, true, c.outlet, ax, ay);
    portPos(c.to, false, c.inlet, bx, by);
    ax += kPortW / 2;
    bx += kPortW / 2;
}

// The end of a cable dragged and dropped at (mx, my): on a box, the cable goes to its nearest inlet (or
// comes from its nearest outlet) instead, one undo step; anywhere else it stays as it was.
void Canvas::moveCableEnd(int mx, int my) {
    Hit h = hitTest(mx, my);
    if (!h.box) return;
    Patch& p = *patch_;
    int k = -1;
    for (int i = 0; i < (int)p.conns.size() && k < 0; ++i)
        if (sameCable(p.conns[i], dragConn_)) k = i;
    if (k < 0) return;
    Connection n = dragConn_;
    if (dragInletEnd_) { n.to = h.box; n.inlet = inletAt(h.box, mx); }
    else { n.from = h.box; n.outlet = outletAt(h.box, mx); }
    if (sameCable(n, dragConn_)) return;
    std::string err;
    ed_->pushUndo(patch_);
    if (p.reconnect(k, n.from, n.outlet, n.to, n.inlet, &err)) {
        p.conns[k].selected = true;
        ed_->markDirty(patch_);
    } else {
        ed_->dropUndo();
        ed_->print("can't connect: " + err);
    }
}

int Canvas::cableUnder(Box* b) const {
    if (!b || b->numIns() < 1 || b->numOuts() < 1) return -1;
    const auto& conns = patch_->conns;
    double cx = b->x + b->w / 2.0, cy = b->y + b->h / 2.0, bestD = 1e30;
    int best = -1;
    for (int k = 0; k < (int)conns.size(); ++k) {
        const Connection& c = conns[k];
        if (c.from == b || c.to == b) continue;
        int ax, ay, bx, by;
        cableEnds(c, ax, ay, bx, by);
        const auto& pts = cablePath(ax, ay, bx, by);
        bool crosses = false;
        for (size_t i = 1; i < pts.size() && !crosses; ++i)
            crosses = segCrossesRect(pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second, b->x, b->y,
                                     b->x + b->w, b->y + b->h);
        if (!crosses) continue;
        // only a cable the box can take: A -> its left inlet, its left outlet -> B
        std::vector<Connection> rest = conns;
        rest.erase(rest.begin() + k);
        if (!canConnect(rest, c.from, c.outlet, b, 0, nullptr) || !canConnect(rest, b, 0, c.to, c.inlet, nullptr))
            continue;
        double d = cableDist(cx, cy, ax, ay, bx, by);
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

void Canvas::clearSelection() {
    if (!patch_) return;
    for (auto& b : patch_->boxes) b->selected = false;
    for (auto& c : patch_->conns) c.selected = false;
}

std::vector<Box*> Canvas::selectedBoxes() const {
    std::vector<Box*> s;
    if (!patch_) return s;
    for (auto& b : patch_->boxes)
        if (b->selected) s.push_back(b.get());
    return s;
}

void Canvas::selectAll() {
    if (locked_ || !patch_) return;
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
    moveFrom_.clear();
    spliceCable_ = -1;
    redraw();
}

void Canvas::setLocked(bool l) {
    if (readOnly() && !l) return;
    hidePortTip();
    commitEdit();
    locked_ = l;
    clearSelection();
    typingBox_ = nullptr;
    typed_.clear();
    redraw();
    ed_->lockChanged();
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
    if (!alive(editing_)) return;
    Box* b = editing_;
    int W = std::max(b->w, 160), H = kBoxH;
    if (editor_->input_type() == FL_MULTILINE_INPUT) {
        auto lines = splitLines(editor_->value());
        W = std::max(W, widestLine(lines) + 24);
        H = kBoxH + ((int)lines.size() - 1) * lineHeight();
    }
    int ex = screenX(b->x), ey = screenY(b->y), ew = (int)std::lround(W * zoom_), eh = (int)std::lround(H * zoom_);
    if (ex != editor_->x() || ey != editor_->y() || ew != editor_->w() || eh != editor_->h()) {
        editor_->textsize(std::max(6, (int)std::lround(kFontSize * zoom_)));
        editor_->resize(ex, ey, ew, eh);
        redraw();
    }
}

void Canvas::editCb(Fl_Widget*, void* data) { static_cast<Canvas*>(data)->commitEdit(); }

void Canvas::commitEdit() {
    if (!editing_ || !patch_) return;
    Box* b = alive(editing_);
    editing_ = nullptr;
    sugg_.clear();
    std::string t = editor_->value();
    editor_->hide();
    if (!b) {                                       // the box went while it was being typed into
        redraw();
        return;
    }
    std::replace(t.begin(), t.end(), '\n', ' ');   // message lines are display only
    // trim
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    Patch& p = *patch_;

    if (b->kind == BoxKind::Comment && t.empty() && editingNew_) {
        p.removeBoxes({b});
        ed_->dropUndo();
    } else if (b->kind == BoxKind::Message || b->kind == BoxKind::Comment) {
        // message and comment boxes keep their text verbatim
        if (splitTokens(t) != splitTokens(b->label()) ||
            (b->kind == BoxKind::Comment && t != b->label())) {
            if (!editingNew_) ed_->pushUndo(patch_);
            p.replaceBox(b, p.create(b->cls, {Atom(t)}));
            ed_->markDirty(patch_);
        }
    } else if (t.empty()) {
        if (editingNew_) {
            p.removeBoxes({b});
            ed_->dropUndo();
        }
    } else if (editingNew_ || t != b->label()) {
        if (!editingNew_) ed_->pushUndo(patch_);
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
        if (nb->kind == BoxKind::Broken) ed_->print("unknown object: " + cls + " (no class and no " + cls + ".moritz next to the patch)");
        else if (nb->inner()) nb->loadbang();       // a new abstraction starts like a loaded one
        ed_->markDirty(patch_);
    }
    Fl::focus(this);
    redraw();
}

void Canvas::cancelEdit() {
    if (!editing_ || !patch_) return;
    Box* b = alive(editing_);
    editing_ = nullptr;
    sugg_.clear();
    editor_->hide();
    if (b && editingNew_ && b->kind != BoxKind::Message) {
        patch_->removeBoxes({b});
        ed_->dropUndo();
    }
    Fl::focus(this);
    redraw();
}

void Canvas::newBoxAt(const std::string& cls, int bx, int by) {
    if (!patch_) return;
    commitEdit();
    if (readOnly()) {
        ed_->setStatus("this patch can't be edited");
        return;
    }
    if (locked_) setLocked(false);
    clearSelection();
    ed_->pushUndo(patch_);
    Patch& p = *patch_;
    Box* b = p.addBox(makeBox(cls, {}, patch_), std::max(0, bx), std::max(0, by));
    layout(b);
    ed_->markDirty(patch_);
    if (b->kind == BoxKind::Number || b->kind == BoxKind::UI) {
        b->selected = true;                 // ready to use, nothing to type
        redraw();
    } else {
        beginEdit(b, true);
    }
}

void Canvas::newBoxAtMouse(const std::string& cls) { newBoxAt(cls, mouseX_, mouseY_); }

void Canvas::deleteSelection() {
    if (!patch_) return;
    Patch& p = *patch_;
    std::vector<int> cables;
    for (int k = 0; k < (int)p.conns.size(); ++k)
        if (p.conns[k].selected) cables.push_back(k);
    auto bs = selectedBoxes();
    if ((cables.empty() && bs.empty()) || readOnly()) return;
    ed_->pushUndo(patch_);
    if (!cables.empty()) p.removeConnections(cables);
    if (!bs.empty()) p.removeBoxes(bs);
    dragBox_ = typingBox_ = nullptr;
    ed_->markDirty(patch_);
    redraw();
}

// Each move places the boxes at where they were when the drag began plus the whole way the mouse went
// since: nothing adds up from one event to the next (a box stopped at the patch's edge while the mouse
// went on), so the box stays under the mouse however fast and far it moves.
void Canvas::beginMove() {
    moveFrom_.clear();
    for (Box* b : selectedBoxes()) moveFrom_[b] = {b->x, b->y};
}

void Canvas::numberDrag(int dy) {
    auto nb = dynamic_cast<NumberBox*>(alive(dragBox_));
    if (!nb) return;
    double v = dragStartValue_ + dy * dragStep_;
    if (dragStep_ < 1.0) v = std::round(v / dragStep_) * dragStep_;
    nb->set(v, true);
}

int Canvas::handle(int e) {
    if (!patch_) return 0;                          // detached: the patch is gone
    // the scrollbars get their own clicks
    // (hiding the box editor in the constructor can make FLTK send FL_ENTER at once, where the mouse is over the new
    // canvas, before the scrollbars exist: X11 did so in the arranger's drawer)
    if ((e == FL_PUSH || e == FL_MOVE || e == FL_ENTER) && hbar_ && vbar_ &&
        ((hbar_->visible() && Fl::event_inside(hbar_)) || (vbar_->visible() && Fl::event_inside(vbar_))))
        return Fl_Group::handle(e);
    if (e == FL_MOUSEWHEEL) {                    // wheel and trackpad scrolling
        scrollTo(sx_ + Fl::event_dx() * 16, sy_ + Fl::event_dy() * 16);
        return 1;
    }
    switch (e) {
        case FL_PASTE:
            if (editing_ || readOnly()) return 0;
            pasteText(std::string(Fl::event_text(), (size_t)Fl::event_length()));
            return 1;
        case FL_LEAVE:
            hidePortTip();
            return 1;
        case FL_ENTER:
        case FL_FOCUS:
        case FL_UNFOCUS:
            return 1;
        case FL_MOVE:
            mouseX_ = patchX(Fl::event_x());
            mouseY_ = patchY(Fl::event_y());
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
            int mx = patchX(Fl::event_x()), my = patchY(Fl::event_y());
            pressX_ = curX_ = mouseX_ = mx;
            pressY_ = curY_ = mouseY_ = my;
            moved_ = false;
            Hit h = hitTest(mx, my);
            bool shift = Fl::event_state(FL_SHIFT);

            // right-click (or Ctrl-click) on a box: its menu; Option-click in
            // locked mode: its help, as in Max
            bool menuClick = Fl::event_button() == FL_RIGHT_MOUSE ||
                             (Fl::event_button() == FL_LEFT_MOUSE && Fl::event_state(FL_CTRL));
            if (h.box && (menuClick || (locked_ && Fl::event_state(FL_ALT)))) {
                Box* b = h.box;
                if (!menuClick) { App::get().openHelp(b->cls); return 1; }
                std::string helpLabel = "Help for " + b->cls;
                for (size_t k = 0; k < helpLabel.size(); ++k)     // menu labels treat / and & specially
                    if (helpLabel[k] == '/' || helpLabel[k] == '&') helpLabel.insert(k++, 1, '\\');
                // Help, Open (a subpatcher), Edit buffer~ (when the first
                // argument names one), then the box's own items
                std::vector<std::string> own = b->menuItems();
                std::string buf = bufferOf(b);
                if (b->cls == "buffer~" || isScoreBox(b)) buf.clear();   // its own menu has Edit...; a score's name
                std::string showLabel = "Edit buffer~ " + buf + "...";
                for (size_t k = 0; k < showLabel.size(); ++k)
                    if (showLabel[k] == '/' || showLabel[k] == '&') showLabel.insert(k++, 1, '\\');
                bool score = isScoreBox(b);                       // a score box: Open (its window, in the view used last)
                std::string openLabel = "Open";
                if (score) {
                    std::string v = scoreViewName((ScoreView)std::clamp(scoreBoxView(b), 0, 3));
                    v[0] = (char)std::toupper((unsigned char)v[0]);
                    openLabel = "Open " + v;
                    if (v == "Piano roll") openLabel = "Open Piano Roll";
                }
                std::vector<Fl_Menu_Item> items(7 + own.size());
                for (auto& it : items) it = Fl_Menu_Item{};
                size_t n = 0;
                items[n++].label(helpLabel.c_str());
                size_t refAt = n;
                items[n++].label("Reference");
                items[n].label(openLabel.c_str());
                if (!b->inner() && !score) items[n].deactivate();
                size_t openAt = n++, showAt = items.size();
                if (!buf.empty()) { showAt = n; items[n++].label(showLabel.c_str()); }
                size_t ownAt = n;
                if (!own.empty()) items[n - 1].flags |= FL_MENU_DIVIDER;
                for (auto& o : own) items[n++].label(o.c_str());
                items.resize(n + 1);
                const Fl_Menu_Item* r = items.data()->popup(Fl::event_x(), Fl::event_y());
                if (!r) return 1;
                size_t pick = (size_t)(r - items.data());
                if (pick == 0) App::get().openHelp(b->cls);
                else if (pick == refAt) App::get().showReference(b->cls);
                else if (pick == openAt && score) openScoreBox(b);
                else if (pick == openAt) ed_->openSubpatch(b);
                else if (pick == showAt) openBufferEditor(patch_, buf);
                else if (pick >= ownAt && pick < ownAt + own.size()) b->menuAction((int)(pick - ownAt));
                redraw();
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
                        zd::font(kFont, kFontSize);
                        int dotX = nb->x + 16 + (int)zd::width(t.substr(0, dot).c_str());
                        if (dot != std::string::npos && mx > dotX) dragStep_ = 0.01;
                    }
                } else if (h.box && Fl::event_clicks() && h.box->cls != "plot" && !bufferOf(h.box).empty()) {
                    Fl::event_clicks(0);                            // double-click: the buffer~ in the sound editor
                    openBufferEditor(patch_, bufferOf(h.box));
                    return 1;
                } else if (h.box && h.box->kind == BoxKind::UI) {
                    // playing with an interface box is not an edit: no undo step (what it saves marks
                    // the patch edited)
                    drag_ = Drag::UI;
                    dragBox_ = h.box;
                    typingBox_ = h.box;                             // its keys next (uiKey)
                    uiBefore_ = messageToString(h.box->saveArgs());
                    h.box->uiMouse(FL_PUSH, mx - h.box->x, my - h.box->y);
                } else if (h.box && h.box->cls == "loadbang" && Fl::event_clicks()) {
                    h.box->loadbang();
                } else if (h.box && h.box->cls == "code" && Fl::event_clicks()) {
                    Fl::event_clicks(0);                            // double-click: the script
                    h.box->message(0, {Atom("open")});
                    return 1;
                } else if (h.box && h.box->inner() && Fl::event_clicks()) {
                    Fl::event_clicks(0);
                    drag_ = Drag::None;
                    ed_->openSubpatch(h.box);                       // like Max: double-click opens it
                    return 1;
                }
                redraw();
                return 1;
            }

            if (Fl::event_clicks()) {                       // double-click
                // Shift+double-click (and Cmd+double-click on macOS) opens a subpatcher or a script in
                // edit mode; a plain double-click edits the box's text. (Ctrl+click is the box's menu.)
#ifdef __APPLE__
                bool opens = Fl::event_state(FL_COMMAND | FL_SHIFT) != 0;
#else
                bool opens = Fl::event_state(FL_SHIFT) != 0;
#endif
                if (h.box && h.box->inner() && opens && !isScoreBox(h.box)) {
                    Fl::event_clicks(0);
                    ed_->openSubpatch(h.box);
                    return 1;
                }
                if (h.box && isScoreBox(h.box) && opens) {
                    Fl::event_clicks(0);
                    openScoreBox(h.box);                            // the score's window, in edit mode too
                    return 1;
                }
                if (h.box && h.box->cls == "code" && opens) {
                    Fl::event_clicks(0);
                    h.box->message(0, {Atom("open")});             // the script, in edit mode too
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
            if (h.box && h.box->uiResizable() && inResizeGrip(h.box, mx, my)) {   // its corner: its size
                if (!shift) { clearSelection(); h.box->selected = true; }
                drag_ = Drag::Resize;
                dragBox_ = h.box;
                resizeW0_ = h.box->w;
                resizeH0_ = h.box->h;
                redraw();
                return 1;
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
                        duplicateSelection(0, 0);
                        drag_ = Drag::Move;
                        moved_ = true;                      // undo already recorded
                        beginMove();
                        break;
                    }
                    if (shift) h.box->selected = !h.box->selected;
                    else if (!h.box->selected) { clearSelection(); h.box->selected = true; }
                    if (h.box->selected) {
                        drag_ = Drag::Move;
                        beginMove();
                    }
                    break;
                case Hit::Cable: {
                    if (!shift) clearSelection();
                    Connection& c = patch_->conns[h.cable];
                    c.selected = true;
                    // near one of its ends: that end can be dragged to another port, as in Max
                    int ax, ay, bx, by;
                    cableEnds(c, ax, ay, bx, by);
                    double dOut = std::hypot(mx - ax, my - ay), dIn = std::hypot(mx - bx, my - by);
                    if (std::min(dOut, dIn) <= kCableEndGrab) {
                        drag_ = Drag::CableEnd;
                        dragConn_ = c;
                        dragInletEnd_ = dIn <= dOut;
                    }
                    break;
                }
                default:
                    if (!shift) clearSelection();
                    drag_ = Drag::Rubber;
            }
            redraw();
            return 1;
        }

        case FL_DRAG: {
            int mx = patchX(Fl::event_x()), my = patchY(Fl::event_y());
            curX_ = mouseX_ = mx;
            curY_ = mouseY_ = my;
            if (drag_ == Drag::Move) {
                // the whole way from the press (beginMove), the group kept at x, y >= 0 as one
                std::vector<std::pair<Box*, std::pair<int, int>>> moving;
                int minX = INT_MAX, minY = INT_MAX;
                for (auto& b : patch_->boxes)
                    if (auto it = moveFrom_.find(b.get()); it != moveFrom_.end() && b->selected) {
                        moving.push_back({b.get(), it->second});
                        minX = std::min(minX, it->second.first);
                        minY = std::min(minY, it->second.second);
                    }
                int dx = std::max(mx - pressX_, std::min(0, -minX)), dy = std::max(my - pressY_, std::min(0, -minY));
                bool changes = false;
                for (auto& [b, from] : moving) changes = changes || b->x != from.first + dx || b->y != from.second + dy;
                if (changes) {
                    if (!moved_) { ed_->pushUndo(patch_); moved_ = true; }
                    for (auto& [b, from] : moving) {
                        b->x = from.first + dx;
                        b->y = from.second + dy;
                    }
                    ed_->markDirty(patch_);
                }
                // one box with Shift held: the cable it would go into, lit
                spliceCable_ = moving.size() == 1 && Fl::event_state(FL_SHIFT) ? cableUnder(moving[0].first) : -1;
            } else if (drag_ == Drag::CableEnd) {
                if (std::abs(mx - pressX_) + std::abs(my - pressY_) > 2) moved_ = true;
            } else if (drag_ == Drag::Number) {
                if (my != pressY_) { moved_ = true; typed_.clear(); }
                numberDrag(pressY_ - my);
            } else if (drag_ == Drag::UI && (dragBox_ = alive(dragBox_))) {
                dragBox_->uiMouse(FL_DRAG, mx - dragBox_->x, my - dragBox_->y);
            } else if (drag_ == Drag::Resize && (dragBox_ = alive(dragBox_))) {
                int nw = std::max(40, resizeW0_ + mx - pressX_), nh = std::max(kBoxH, resizeH0_ + my - pressY_);
                if (nw != dragBox_->w || nh != dragBox_->h) {
                    if (!moved_) { ed_->pushUndo(patch_); moved_ = true; }
                    dragBox_->uiResize(nw, nh);
                    layout(dragBox_);
                    ed_->markDirty(patch_);
                }
            }
            redraw();
            return 1;
        }

        case FL_RELEASE: {
            int mx = patchX(Fl::event_x()), my = patchY(Fl::event_y());
            dragBox_ = alive(dragBox_);
            if (drag_ == Drag::UI && dragBox_) {
                dragBox_->uiMouse(FL_RELEASE, mx - dragBox_->x, my - dragBox_->y);
                if (alive(dragBox_) && messageToString(dragBox_->saveArgs()) != uiBefore_) ed_->markDirty(patch_);
            }
            if (drag_ == Drag::Cable && dragBox_) {
                Hit h = hitTest(mx, my);
                if (h.box && h.box != dragBox_) {
                    int in = inletAt(h.box, mx);
                    std::string err;
                    Patch& p = *patch_;
                    if (canConnect(p.conns, dragBox_, dragPort_, h.box, in, &err)) {
                        ed_->pushUndo(patch_);
                        if (p.connect(dragBox_, dragPort_, h.box, in, &err)) ed_->markDirty(patch_);
                        else { ed_->dropUndo(); ed_->print("can't connect: " + err); }
                    } else if (err != "already connected") {
                        ed_->print("can't connect: " + err);
                    }
                }
            } else if (drag_ == Drag::CableEnd && moved_ && alive(dragConn_.from) && alive(dragConn_.to)) {
                moveCableEnd(mx, my);
            } else if (drag_ == Drag::Rubber) {
                int x0 = std::min(pressX_, mx), x1 = std::max(pressX_, mx);
                int y0 = std::min(pressY_, my), y1 = std::max(pressY_, my);
                for (auto& b : patch_->boxes)
                    if (b->x < x1 && b->x + b->w > x0 && b->y < y1 && b->y + b->h > y0)
                        b->selected = true;
            }
            if (drag_ == Drag::Move && moved_ && Fl::event_state(FL_SHIFT)) {
                // dropped on a cable with Shift held: the box goes into it (the move's undo step holds both)
                auto sel = selectedBoxes();
                int k = sel.size() == 1 && moveFrom_.count(sel[0]) ? cableUnder(sel[0]) : -1;
                std::string err;
                if (k >= 0) {
                    if (patch_->insertIntoCable(k, sel[0], &err)) ed_->markDirty(patch_);
                    else ed_->print("can't put the box into the cable: " + err);
                }
            }
            if (drag_ == Drag::Move && moved_ && patch_->owner)
                patch_->rebuild();                  // inlets/outlets are ordered by position
            spliceCable_ = -1;
            drag_ = Drag::None;
            moveFrom_.clear();
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
                if (Box* ub = alive(typingBox_); ub && ub->kind == BoxKind::UI) {   // an interface box clicked last
                    if (!ub->uiKey(key)) return 0;
                    redraw();
                    return 1;
                }
                auto nb = dynamic_cast<NumberBox*>(alive(typingBox_));
                if (!nb) { typingBox_ = nullptr; typed_.clear(); return 0; }
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
