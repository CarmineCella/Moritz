// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// zoom.h — drawing with a zoom: the FLTK drawing calls used by the canvas and
// by the interface boxes, through a transform. Coordinates are given as if
// unzoomed; a point p is drawn at origin + (p - origin) * zoom, sizes and
// fonts are multiplied by zoom, and text measurements are returned in
// unzoomed units, so that layout does not depend on the zoom. Outside a
// zoomed canvas the transform is the identity and every call is FLTK's own.
#pragma once

#include <FL/Fl_RGB_Image.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>

namespace zd {

struct Transform {
    double ox = 0, oy = 0, z = 1;
};
inline Transform& current() {
    static Transform t;
    return t;
}
inline double zoom() { return current().z; }
inline int X(double x) { const Transform& t = current(); return (int)std::lround(t.ox + (x - t.ox) * t.z); }
inline int Y(double y) { const Transform& t = current(); return (int)std::lround(t.oy + (y - t.oy) * t.z); }
inline int S(double s) { return (int)std::lround(s * current().z); }
// a size from two transformed ends (no gaps between neighbouring rectangles)
inline int W(double x, double w) { return X(x + w) - X(x); }
inline int H(double y, double h) { return Y(y + h) - Y(y); }

// sets the transform for a scope, and restores the previous one
class Scope {
public:
    Scope(double ox, double oy, double z) : saved_(current()) { current() = Transform{ox, oy, z}; }
    ~Scope() { current() = saved_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Transform saved_;
};

// ---- fonts: the font size in unzoomed units, measurements unzoomed
struct FontState {
    Fl_Font face = FL_HELVETICA;
    Fl_Fontsize size = 12;
};
inline FontState& font() {
    static FontState f;
    return f;
}
inline void font(Fl_Font face, Fl_Fontsize size) {
    font() = FontState{face, size};
    fl_font(face, std::max(1, S(size)));
}
template <class F>
inline double unzoomed(F measure) {
    if (current().z == 1) return measure();
    fl_font(font().face, font().size);
    double v = measure();
    fl_font(font().face, std::max(1, S(font().size)));
    return v;
}
inline double width(const char* s) { return unzoomed([&] { return fl_width(s); }); }
inline double width(const char* s, int n) { return unzoomed([&] { return fl_width(s, n); }); }
inline int height() { return (int)unzoomed([] { return (double)fl_height(); }); }
inline int descent() { return (int)unzoomed([] { return (double)fl_descent(); }); }

// ---- lines
inline void line_style(int style, int w = 0, char* dashes = nullptr) {
    double z = current().z;
    int ww = w == 0 ? (z >= 1.75 ? (int)std::lround(z) : 0) : std::max(1, (int)std::lround(w * z));
    fl_line_style(style, ww, dashes);
}
inline void line(double x, double y, double x1, double y1) { fl_line(X(x), Y(y), X(x1), Y(y1)); }
// a line through points (pairs of x, y), in the current line style
template <class Points>
inline void polyline(const Points& pts) {
    const Transform& t = current();
    fl_begin_line();
    for (auto& p : pts) fl_vertex(t.ox + (p.first - t.ox) * t.z, t.oy + (p.second - t.oy) * t.z);
    fl_end_line();
}
inline void line(double x, double y, double x1, double y1, double x2, double y2) {
    fl_line(X(x), Y(y), X(x1), Y(y1), X(x2), Y(y2));
}
inline void xyline(double x, double y, double x1) { fl_xyline(X(x), Y(y), X(x1)); }
inline void yxline(double x, double y, double y1) { fl_yxline(X(x), Y(y), Y(y1)); }
inline void point(double x, double y) {
    if (current().z == 1) { fl_point((int)x, (int)y); return; }
    int s = std::max(1, S(1));
    fl_rectf(X(x), Y(y), s, s);
}
inline void begin_line() { fl_begin_line(); }
inline void end_line() { fl_end_line(); }
inline void vertex(double x, double y) {
    const Transform& t = current();
    fl_vertex(t.ox + (x - t.ox) * t.z, t.oy + (y - t.oy) * t.z);
}

// ---- shapes
inline void rectf(double x, double y, double w, double h) { fl_rectf(X(x), Y(y), W(x, w), H(y, h)); }
inline void rect(double x, double y, double w, double h) { fl_rect(X(x), Y(y), W(x, w), H(y, h)); }
inline void rounded_rectf(double x, double y, double w, double h, double r) {
    fl_rounded_rectf(X(x), Y(y), W(x, w), H(y, h), std::max(0, S(r)));
}
inline void rounded_rect(double x, double y, double w, double h, double r) {
    fl_rounded_rect(X(x), Y(y), W(x, w), H(y, h), std::max(0, S(r)));
}
inline void polygon(double x0, double y0, double x1, double y1, double x2, double y2) {
    fl_polygon(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2));
}
inline void pie(double x, double y, double w, double h, double a1, double a2) {
    fl_pie(X(x), Y(y), W(x, w), H(y, h), a1, a2);
}
inline void arc(double x, double y, double w, double h, double a1, double a2) {
    fl_arc(X(x), Y(y), W(x, w), H(y, h), a1, a2);
}
inline void push_clip(double x, double y, double w, double h) { fl_push_clip(X(x), Y(y), W(x, w), H(y, h)); }
inline void pop_clip() { fl_pop_clip(); }

// ---- text
inline void draw(const char* s, double x, double y) { fl_draw(s, X(x), Y(y)); }
inline void draw(const char* s, int n, double x, double y) { fl_draw(s, n, X(x), Y(y)); }
inline void draw(const char* s, double x, double y, double w, double h, Fl_Align a) {
    fl_draw(s, X(x), Y(y), W(x, w), H(y, h), a);
}

// ---- images: an RGB buffer of w x h pixels, drawn at w x h unzoomed units
inline void draw_image(const unsigned char* buf, double x, double y, int w, int h, int d = 3, int ld = 0) {
    if (current().z == 1) { fl_draw_image(buf, (int)x, (int)y, w, h, d, ld); return; }
    Fl_RGB_Image img(buf, w, h, d, ld);
    img.scale(W(x, w), H(y, h), 0, 1);
    img.draw(X(x), Y(y));
}

}  // namespace zd
