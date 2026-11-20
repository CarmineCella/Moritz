// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_plot.cpp — plot: curves, bars, scatter plots and matrices (heat
// maps) in a patch, with a larger window to explore them. The drawing
// follows Musil's plot.h (figures with line, scatter, bars and image layers;
// zoom, pan, PNG export).
//
// plot [buffer] [width height] [mode] shows:
//   buffer name [channel]   a buffer~, followed when it changes: its channels
//                           are the rows of a matrix (one frame a sample),
//                           its sample rate gives the time axis (seconds)
//   a list                  one curve (line, bars) or x y pairs (scatter); in
//                           image mode each list is added as a column, so a
//                           live analysis scrolls by (history columns)
//   a number                added to a scrolling curve (history points)
// Modes: auto (a curve for up to 4 rows, an image above), line, bars,
// scatter (rows 0 and 1 are x and y, row 2 the colour), image (row 0 at the
// bottom). Messages: range lo hi | range auto (the values: y of a curve,
// colours of an image), xrange from to | xrange auto (the part shown: seconds
// on a time axis), rows first last (an image's y axis: what its first and
// last rows stand for, e.g. rows 30 300 for a tempogram in bpm), normalize 1 (each row of an image coloured over its
// own range: MFCCs, curves of different units), ylog 1 (a logarithmic y axis
// for curves and scatter plots: frequencies), marks t... (vertical lines; ms on a time axis, also
// the right inlet), cursor ms, title text, labels a b c (names of the rows
// of an image or of the bars; "labels chroma": C C# ... B, "labels keys": C Cm
// Db C#m ... Bm, as keyfind orders them), colors viridis | gray | types (an image of categories: each value its own
// colour, as sound types), history n, clear,
// open (the window), export file.png [w h].
// Clicking in locked mode outputs where: x (ms on a time axis, otherwise the
// index) and y (the row of an image, the value of a curve). Double-click
// opens the window: the wheel zooms (Shift: vertically), dragging moves, r
// resets, e exports a PNG.

#include "objects.h"
#include "zoom.h"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <map>

void fl_open_display();   // FL/platform.H (not included: it drags in X11's macros on Linux)

namespace moritz {
namespace {

struct Range {
    double x0 = 0, x1 = 1, y0 = 0, y1 = 1;
};

struct Rgb {
    unsigned char r, g, b;
};

Rgb viridis(double t) {
    static const unsigned char p[][3] = {{68, 1, 84}, {59, 82, 139}, {33, 145, 140}, {94, 201, 98}, {253, 231, 37}};
    t = std::clamp(t, 0.0, 1.0) * 4;
    int i = std::min(3, (int)t);
    double f = t - i;
    return {(unsigned char)(p[i][0] + f * (p[i + 1][0] - p[i][0])), (unsigned char)(p[i][1] + f * (p[i + 1][1] - p[i][1])),
            (unsigned char)(p[i][2] + f * (p[i + 1][2] - p[i][2]))};
}
Rgb palette(int k) {
    static const Rgb c[] = {{31, 119, 180}, {255, 127, 14}, {44, 160, 44}, {214, 39, 40},
                            {148, 103, 189}, {140, 86, 75}, {227, 119, 194}, {127, 127, 127}};
    return c[((k % 8) + 8) % 8];
}
// sixteen distinct colours, for categories (sound types)
Rgb category(int k) {
    static const Rgb c[] = {{31, 119, 180}, {255, 127, 14}, {44, 160, 44}, {214, 39, 40}, {148, 103, 189},
                            {140, 86, 75}, {227, 119, 194}, {188, 189, 34}, {23, 190, 207}, {127, 127, 127},
                            {174, 199, 232}, {255, 187, 120}, {152, 223, 138}, {255, 152, 150}, {197, 176, 213},
                            {219, 219, 141}};
    return c[((k % 16) + 16) % 16];
}
Rgb brighter(Rgb c) {   // the palette on the dark patch
    return {(unsigned char)std::min(255, c.r + 50), (unsigned char)std::min(255, c.g + 50), (unsigned char)std::min(255, c.b + 50)};
}
double niceStep(double range, int target) {
    if (range <= 0) return 1;
    double raw = range / target, mag = std::pow(10, std::floor(std::log10(raw))), r = raw / mag;
    return (r < 1.5 ? 1 : r < 3.5 ? 2 : r < 7.5 ? 5 : 10) * mag;
}
std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", std::fabs(v) < 1e-12 ? 0.0 : v);
    return b;
}

// What a plot shows; shared by the box and its window.
struct PlotState {
    std::vector<std::vector<float>> rows;   // rows x columns
    double xStart = 0, xStep = 1;           // x of column k: xStart + k * xStep
    bool time = false;                      // x in seconds (a buffer~)
    std::string mode = "auto", title, source;
    bool fixed = false, gray = false, types = false, normalize = false, xfixed = false, rowValues = false, ylog = false;
    double lo = 0, hi = 1, xlo = 0, xhi = 1, rowFirst = 0, rowLast = 1;
    std::vector<double> marks;              // ms on a time axis, x otherwise
    double cursor = -1;
    std::vector<std::string> labels;
    int history = 256;
    int version = 0;

    size_t cols() const { return rows.empty() ? 0 : rows[0].size(); }
    std::string kind() const {
        if (mode != "auto") return mode;
        if (types) return "image";                       // categories are drawn as bands
        return rows.size() <= 4 ? "line" : "image";
    }
    double xOf(double col) const { return xStart + col * xStep; }
    // y as drawn: log2 of the value on a logarithmic axis (curves and scatter plots)
    double yOf(double v) const { return ylog ? std::log2(std::max(v, 1e-6)) : v; }
    // what row r (or a position between rows) stands for on the y axis
    double rowValue(double r) const {
        if (!rowValues || rows.size() < 2) return r;
        return rowFirst + r * (rowLast - rowFirst) / (double)(rows.size() - 1);
    }
    double markX(double v) const { return time ? v / 1000.0 : v; }
    // min and max of the values (of rows 0..1 for scatter: x and y)
    void stats(double& mn, double& mx, int row = -1) const {
        mn = 1e300; mx = -1e300;
        for (size_t r = 0; r < rows.size(); ++r) {
            if (row >= 0 && (int)r != row) continue;
            for (float v : rows[r])
                if (std::isfinite(v)) { mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); }
        }
        if (mn > mx) { mn = 0; mx = 1; }
        if (mn == mx) { mn -= .5; mx += .5; }
    }
    void valueRange(double& mn, double& mx) const {
        if (fixed) { mn = lo; mx = hi; return; }
        if (cacheVersion_ != version) {
            stats(cacheLo_, cacheHi_);
            cacheVersion_ = version;
        }
        mn = cacheLo_; mx = cacheHi_;
    }
    Range dataRange() const {
        Range r = autoRange();
        if (xfixed) { r.x0 = xlo; r.x1 = xhi; }
        return r;
    }
    Range autoRange() const {
        Range r;
        size_t n = cols();
        std::string k = kind();
        double lo, hi;
        if (k == "scatter") {
            stats(r.x0, r.x1, 0);
            stats(r.y0, r.y1, 1);
            r.y0 = yOf(r.y0); r.y1 = yOf(r.y1);
            if (r.y1 <= r.y0) r.y1 = r.y0 + 1;
            double dx = (r.x1 - r.x0) * .05, dy = (r.y1 - r.y0) * .05;
            r.x0 -= dx; r.x1 += dx; r.y0 -= dy; r.y1 += dy;
            return r;
        }
        if (k == "image") {
            r.x0 = xOf(0); r.x1 = xOf((double)std::max<size_t>(n, 1));
            r.y0 = 0; r.y1 = (double)std::max<size_t>(rows.size(), 1);
            return r;
        }
        valueRange(lo, hi);
        if (k == "bars") {
            r.x0 = xOf(-.5); r.x1 = xOf(std::max(0.5, n - .5));
            if (lo >= 0) lo = 0;                          // bars rise from 0 (or from the bottom, all negative: dB)
        } else {
            r.x0 = xOf(0); r.x1 = xOf(n > 1 ? (double)(n - 1) : 1.0);
        }
        if (ylog && k != "bars") { lo = yOf(lo); hi = yOf(hi); if (hi <= lo) hi = lo + 1; }
        double m = fixed ? 0 : (hi - lo) * .05;
        r.y0 = lo - m; r.y1 = hi + m;
        return r;
    }

private:
    mutable int cacheVersion_ = -1;
    mutable double cacheLo_ = 0, cacheHi_ = 1;
};

// An image of the matrix, kept until something changes.
struct ImageCache {
    std::vector<unsigned char> rgb;
    int version = -1, w = 0, h = 0;
    Range view;
    double lo = 0, hi = 0;
    bool gray = false, types = false, normalize = false;
};

// Draws the plot into (X, Y, W, H). full: the window's look (light, axes,
// labels); otherwise the patch's (dark, compact).
void drawPlot(const PlotState& s, ImageCache& cache, int X, int Y, int W, int H, const Range& v, bool full) {
    const Fl_Color bg = full ? fl_rgb_color(250, 250, 250) : fl_rgb_color(22, 24, 28);
    const Fl_Color grid = full ? fl_rgb_color(228, 228, 228) : fl_rgb_color(40, 44, 52);
    const Fl_Color text = full ? fl_rgb_color(70, 70, 70) : fl_rgb_color(110, 114, 124);
    std::string k = s.kind();
    bool image = k == "image";
    int top = full ? (s.title.empty() ? 14 : 32) : 0, bottom = full ? 40 : 0;
    int left = full ? 62 : 0, right = full ? (image ? 70 : 18) : 0;
    fl_color(bg);
    zd::rectf(X, Y, W, H);
    int px = X + left, py = Y + top, pw = W - left - right, ph = H - top - bottom;
    if (pw < 8 || ph < 8) return;
    auto sx = [&](double x) { return px + (x - v.x0) / (v.x1 - v.x0) * pw; };
    auto sy = [&](double y) { return py + ph - (y - v.y0) / (v.y1 - v.y0) * ph; };
    if (s.rows.empty() || s.cols() == 0) return;
    zd::push_clip(px, py, pw, ph);
    double lo, hi;
    s.valueRange(lo, hi);
    if (hi <= lo) hi = lo + 1;
    size_t n = s.cols(), R = s.rows.size();

    // grid
    double stx = niceStep(v.x1 - v.x0, full ? 8 : 4), sty = niceStep(v.y1 - v.y0, full ? 6 : 3);
    fl_color(grid);
    if (!image) {
        for (double t = std::ceil(v.x0 / stx) * stx; t <= v.x1; t += stx) zd::yxline((int)sx(t), py, py + ph);
        for (double t = std::ceil(v.y0 / sty) * sty; t <= v.y1; t += sty) zd::xyline(px, (int)sy(t), px + pw);
    }

    // marks: behind curves and bars, over images; many are dimmed
    auto drawMarks = [&]() {
        int shown = 0;
        for (double m : s.marks) {
            double x = s.markX(m);
            if (x >= v.x0 && x <= v.x1) ++shown;
        }
        Fl_Color mark = full ? fl_rgb_color(214, 39, 40) : fl_rgb_color(240, 110, 110);
        if (shown > pw / 6) mark = fl_color_average(mark, bg, shown > pw / 2 ? .25f : .5f);
        fl_color(mark);
        for (double m : s.marks) {
            double x = s.markX(m);
            if (x >= v.x0 && x <= v.x1) zd::yxline((int)sx(x), py, py + ph);
        }
    };
    if (!image) drawMarks();
    if (n > 0 && R > 0 && image) {
        bool fresh = cache.version != s.version || cache.w != pw || cache.h != ph || cache.lo != lo || cache.hi != hi ||
                     cache.gray != s.gray || cache.types != s.types || cache.normalize != s.normalize || cache.view.x0 != v.x0 || cache.view.x1 != v.x1 || cache.view.y0 != v.y0 ||
                     cache.view.y1 != v.y1;
        if (fresh) {
            cache.rgb.assign((size_t)pw * ph * 3, 0);
            // the columns and rows under every pixel: their mean shows (up to 16 x 16 of them)
            std::vector<long> c0((size_t)pw), c1((size_t)pw), r0((size_t)ph), r1((size_t)ph);
            for (int i = 0; i < pw; ++i) {
                double a = (v.x0 + (v.x1 - v.x0) * i / pw - s.xStart) / s.xStep;
                double b = (v.x0 + (v.x1 - v.x0) * (i + 1) / pw - s.xStart) / s.xStep;
                c0[(size_t)i] = (long)std::floor(a);
                c1[(size_t)i] = std::max(c0[(size_t)i] + 1, (long)std::ceil(b));
            }
            for (int j = 0; j < ph; ++j) {               // j = 0 at the top
                double a = v.y1 - (v.y1 - v.y0) * (j + 1) / ph, b = v.y1 - (v.y1 - v.y0) * j / ph;
                r0[(size_t)j] = (long)std::floor(a);
                r1[(size_t)j] = std::max(r0[(size_t)j] + 1, (long)std::ceil(b));
            }
            // each row's own range, when the rows are normalised
            std::vector<double> rlo(R, lo), rhi(R, hi);
            if (s.normalize)
                for (size_t r = 0; r < R; ++r) {
                    s.stats(rlo[r], rhi[r], (int)r);
                    if (rhi[r] <= rlo[r]) rhi[r] = rlo[r] + 1;
                }
            for (int j = 0; j < ph; ++j)
                for (int i = 0; i < pw; ++i) {
                    if (s.types) {                                   // categories: the cell under the pixel's centre
                        long r = (long)std::floor(v.y1 - (v.y1 - v.y0) * (j + .5) / ph);
                        long c = (long)std::floor((v.x0 + (v.x1 - v.x0) * (i + .5) / pw - s.xStart) / s.xStep);
                        unsigned char* p = &cache.rgb[((size_t)j * pw + i) * 3];
                        Rgb col = full ? Rgb{250, 250, 250} : Rgb{22, 24, 28};
                        if (r >= 0 && r < (long)R && c >= 0 && c < (long)n) col = category((int)std::lround(s.rows[(size_t)r][(size_t)c]));
                        p[0] = col.r; p[1] = col.g; p[2] = col.b;
                        continue;
                    }
                    double sum = 0;
                    long cnt = 0;
                    long ra = std::max(0L, r0[(size_t)j]), rb = std::min((long)R, r1[(size_t)j]);
                    long ca = std::max(0L, c0[(size_t)i]), cb = std::min((long)n, c1[(size_t)i]);
                    long cstep = std::max(1L, (cb - ca) / 16), rstep = std::max(1L, (rb - ra) / 16);
                    for (long r = ra; r < rb; r += rstep) {
                        const auto& row = s.rows[(size_t)r];
                        double a = rlo[(size_t)r], d = rhi[(size_t)r] - rlo[(size_t)r];
                        for (long c = ca; c < cb; c += cstep) { sum += (row[(size_t)c] - a) / d; ++cnt; }
                    }
                    unsigned char* p = &cache.rgb[((size_t)j * pw + i) * 3];
                    Rgb c;
                    if (!cnt) c = full ? Rgb{250, 250, 250} : Rgb{22, 24, 28};
                    else {
                        double t = sum / cnt;
                        if (s.gray) { auto g = (unsigned char)(std::clamp(t, 0.0, 1.0) * 255); c = {g, g, g}; }
                        else c = viridis(t);
                    }
                    p[0] = c.r; p[1] = c.g; p[2] = c.b;
                }
            cache.version = s.version; cache.w = pw; cache.h = ph; cache.lo = lo; cache.hi = hi;
            cache.gray = s.gray; cache.types = s.types; cache.normalize = s.normalize; cache.view = v;
        }
        zd::draw_image(cache.rgb.data(), px, py, pw, ph, 3);
    } else if (n > 0 && R > 0 && k == "scatter") {
        if (R >= 2) {
            // row 2 colours the points: classes (small integers) or a value (viridis)
            bool classes = true;
            double cmin = 1e300, cmax = -1e300;
            if (R > 2)
                for (float v : s.rows[2]) {
                    if (v != std::floor(v) || v < 0 || v > 15) classes = false;
                    cmin = std::min(cmin, (double)v);
                    cmax = std::max(cmax, (double)v);
                }
            if (s.fixed && R > 2 && !classes) { cmin = s.lo; cmax = s.hi; }
            int d = n > 3000 ? 2 : n > 500 ? 4 : 6;
            // the weakest first, so the strongest stay on top
            std::vector<size_t> order(n);
            for (size_t c = 0; c < n; ++c) order[c] = c;
            if (R > 2 && !classes)
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return s.rows[2][a] < s.rows[2][b]; });
            for (size_t c : order) {
                Rgb col;
                if (R > 2 && !classes) col = viridis(cmax > cmin ? (s.rows[2][c] - cmin) / (cmax - cmin) : 1.0);
                else {
                    col = palette(R > 2 ? (int)s.rows[2][c] : 0);
                    if (!full) col = brighter(col);
                }
                fl_color(col.r, col.g, col.b);
                int x = (int)sx(s.rows[0][c]), y = (int)sy(s.yOf(s.rows[1][c]));
                if (d <= 2) zd::rectf(x - 1, y - 1, 2, 2);
                else zd::pie(x - d / 2, y - d / 2, d, d, 0, 360);
            }
        }
    } else if (n > 0 && R > 0 && k == "bars") {
        double bw = std::max(1.0, std::fabs(sx(s.xOf(1)) - sx(s.xOf(0))) * .8);
        Rgb col = full ? palette(0) : brighter(palette(0));
        fl_color(col.r, col.g, col.b);
        for (size_t c = 0; c < n; ++c) {
            double val = s.rows[0][c];
            double base = (v.y0 <= 0 && v.y1 >= 0) ? 0.0 : v.y0;
            int y0 = (int)sy(base), y1 = (int)sy(val), x = (int)(sx(s.xOf((double)c)) - bw / 2);
            zd::rectf(x, std::min(y0, y1), (int)bw, std::max(1, std::abs(y1 - y0)));
        }
    } else if (n > 0 && R > 0) {                                  // lines
        zd::line_style(FL_SOLID, full ? 2 : 1);
        for (size_t r = 0; r < R; ++r) {
            Rgb col = palette((int)r);
            if (!full) col = R == 1 ? Rgb{236, 196, 70} : brighter(col);
            fl_color(col.r, col.g, col.b);
            const auto& row = s.rows[r];
            double perPixel = (v.x1 - v.x0) / s.xStep / pw;   // columns a pixel
            if (perPixel <= 2) {
                long a = std::max(0L, (long)std::floor((v.x0 - s.xStart) / s.xStep) - 1);
                long b = std::min((long)n - 1, (long)std::ceil((v.x1 - s.xStart) / s.xStep) + 1);
                for (long c = a; c < b; ++c)
                    zd::line((int)sx(s.xOf((double)c)), (int)sy(s.yOf(row[(size_t)c])), (int)sx(s.xOf((double)c + 1)),
                            (int)sy(s.yOf(row[(size_t)c + 1])));
                if (a == b && b >= 0) zd::point((int)sx(s.xOf((double)a)), (int)sy(s.yOf(row[(size_t)a])));
            } else {                                              // min and max under every pixel
                int prevLo = 0, prevHi = 0;
                bool have = false;
                for (int i = 0; i < pw; ++i) {
                    long a = (long)std::floor((v.x0 + (v.x1 - v.x0) * i / pw - s.xStart) / s.xStep);
                    long b = (long)std::ceil((v.x0 + (v.x1 - v.x0) * (i + 1) / pw - s.xStart) / s.xStep);
                    a = std::max(0L, a); b = std::min((long)n, b);
                    if (a >= b) { have = false; continue; }
                    long stride = std::max(1L, (b - a) / 256);
                    double mn = 1e300, mx = -1e300;
                    for (long c = a; c < b; c += stride) { mn = std::min(mn, (double)row[(size_t)c]); mx = std::max(mx, (double)row[(size_t)c]); }
                    int yl = (int)sy(s.yOf(mn)), yh = (int)sy(s.yOf(mx));
                    if (have) {                                   // join with the previous column
                        if (yh > prevLo) yh = prevLo;
                        if (yl < prevHi) yl = prevHi;
                    }
                    zd::yxline(px + i, std::min(yl, yh), std::max(yl, yh));
                    prevLo = std::max(yl, yh); prevHi = std::min(yl, yh);
                    have = true;
                }
            }
        }
        zd::line_style(0);
    }

    if (image) drawMarks();
    if (s.cursor >= 0) {
        double x = s.markX(s.cursor);
        fl_color(full ? FL_BLACK : kAccent);
        if (x >= v.x0 && x <= v.x1) zd::yxline((int)sx(x), py, py + ph);
    }
    zd::pop_clip();

    // labels and ticks
    zd::font(kFont, full ? 11 : 9);
    fl_color(text);
    auto rowLabel = [&](size_t r) { return r < s.labels.size() ? s.labels[r] : num((double)r); };
    if (full) {
        fl_color(fl_rgb_color(150, 150, 150));
        zd::rect(px, py, pw, ph);
        fl_color(text);
        if (!s.title.empty()) {
            zd::font(FL_HELVETICA_BOLD, 13);
            zd::draw(s.title.c_str(), px + (pw - (int)zd::width(s.title.c_str())) / 2, Y + 21);
            zd::font(kFont, 11);
        }
        if (k == "bars" && !s.labels.empty()) {
            for (size_t c = 0; c < n && c < s.labels.size(); ++c) {
                double x = sx(s.xOf((double)c));
                if (x < px || x > px + pw) continue;
                zd::draw(s.labels[c].c_str(), (int)x - (int)zd::width(s.labels[c].c_str()) / 2, py + ph + 15);
            }
        } else {
            for (double t = std::ceil(v.x0 / stx) * stx; t <= v.x1; t += stx) {
                std::string l = num(t);
                zd::draw(l.c_str(), (int)sx(t) - (int)zd::width(l.c_str()) / 2, py + ph + 15);
            }
        }
        if (s.time) zd::draw("seconds", px + pw / 2 - 20, py + ph + 32);
        if (image) {
            double rowsShown = v.y1 - v.y0, every = std::max(1.0, std::ceil(rowsShown * 14 / ph));
            if (!s.labels.empty() || rowsShown <= 40)
                for (double r = std::ceil(v.y0); r < v.y1; r += every) {
                    if (r < 0 || r >= (double)R) continue;
                    std::string l = rowLabel((size_t)r);
                    zd::draw(l.c_str(), px - 6 - (int)zd::width(l.c_str()), (int)sy(r + .5) + 4);
                }
            else if (s.rowValues) {                           // ticks in the rows' own values
                double a = s.rowValue(std::max(0.0, v.y0)), b = s.rowValue(std::min((double)R - 1, v.y1));
                double st = niceStep(std::fabs(b - a), 6), per = (s.rowLast - s.rowFirst) / std::max<double>(1, (double)R - 1);
                if (per != 0)
                    for (double t = std::ceil(std::min(a, b) / st) * st; t <= std::max(a, b); t += st) {
                        double r = (t - s.rowFirst) / per + .5;
                        std::string l = num(t);
                        zd::draw(l.c_str(), px - 6 - (int)zd::width(l.c_str()), (int)sy(r) + 4);
                    }
            } else
                for (double t = std::ceil(v.y0 / sty) * sty; t <= v.y1; t += sty) {
                    std::string l = num(t);
                    zd::draw(l.c_str(), px - 6 - (int)zd::width(l.c_str()), (int)sy(t) + 4);
                }
            // the colour bar (none for categories)
            int bx = px + pw + 16, bwid = 12;
            for (int j = 0; j < ph && !s.types; ++j) {
                double t = 1.0 - (double)j / ph;
                Rgb c = s.gray ? Rgb{(unsigned char)(t * 255), (unsigned char)(t * 255), (unsigned char)(t * 255)} : viridis(t);
                fl_color(c.r, c.g, c.b);
                zd::xyline(bx, py + j, bx + bwid);
            }
            fl_color(text);
            if (!s.types) {
                zd::draw(num(hi).c_str(), bx + bwid + 4, py + 9);
                zd::draw(num(lo).c_str(), bx + bwid + 4, py + ph);
            }
        } else {
            double st = s.ylog ? std::max(1.0, std::round(sty)) : sty;      // octaves on a log axis
            for (double t = std::ceil(v.y0 / st) * st; t <= v.y1; t += st) {
                std::string l = num(s.ylog ? std::pow(2.0, t) : t);
                zd::draw(l.c_str(), px - 6 - (int)zd::width(l.c_str()), (int)sy(t) + 4);
            }
        }
    } else {
        // a few values inside the frame
        if (k == "bars" && !s.labels.empty()) {
            for (size_t c = 0; c < n && c < s.labels.size(); ++c)
                zd::draw(s.labels[c].c_str(), (int)sx(s.xOf((double)c)) - (int)zd::width(s.labels[c].c_str()) / 2, py + ph - 2);
        } else if (n > 1) {
            for (double t = std::ceil(v.x0 / stx) * stx; t < v.x1 - stx * .3; t += stx) {
                if (sx(t) < px + 14) continue;
                std::string l = num(t) + (s.time ? "s" : "");
                zd::draw(l.c_str(), (int)sx(t) + 2, py + ph - 2);
            }
        }
        if (!image) {
            double st = s.ylog ? std::max(1.0, std::round(sty)) : sty;
            for (double t = std::ceil(v.y0 / st) * st; t <= v.y1; t += st)
                if (sy(t) > py + 18 && sy(t) < py + ph - 12) zd::draw(num(s.ylog ? std::pow(2.0, t) : t).c_str(), px + 2, (int)sy(t) - 2);
        }
        if (image && !s.labels.empty() && ph / std::max<size_t>(1, R) >= 8)
            for (size_t r = 0; r < R; ++r) {
                std::string l = rowLabel(r);
                zd::draw(l.c_str(), px + pw - 3 - (int)zd::width(l.c_str()), (int)sy(r + .5) + 4);
            }
        std::string name = !s.title.empty() ? s.title : s.source;
        if (!name.empty()) {
            fl_color(fl_rgb_color(200, 204, 214));
            zd::draw(name.c_str(), px + 4, py + 11);
        }
    }
}

class PlotBox;

// ---------------------------------------------------------------- the window
class PlotView : public Fl_Widget {
public:
    PlotView(int X, int Y, int W, int H, std::shared_ptr<PlotState> s) : Fl_Widget(X, Y, W, H), s_(std::move(s)) {}
    void reset() { zoomed_ = false; redraw(); }
    void exportPng() {
        Fl_Native_File_Chooser fc;
        fc.title("Export the plot");
        fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
        fc.filter("PNG\t*.png");
        fc.preset_file("plot.png");
        fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::USE_FILTER_EXT);
        if (fc.show() != 0) return;
        std::string path = fc.filename();
        if (path.size() < 4 || path.substr(path.size() - 4) != ".png") path += ".png";
        writePng(*s_, path, w(), h(), view());
    }
    static bool writePng(const PlotState& s, const std::string& path, int W, int H, const Range& v) {
        Fl_Image_Surface surf(W, H);
        Fl_Surface_Device::push_current(&surf);
        ImageCache c;
        drawPlot(s, c, 0, 0, W, H, v, true);
        Fl_Surface_Device::pop_current();
        Fl_RGB_Image* img = surf.image();
        int r = fl_write_png(path.c_str(), img);
        delete img;
        return r == 0;
    }
    Range view() const { return zoomed_ ? v_ : s_->dataRange(); }

protected:
    void draw() override {
        Range v = view();
        drawPlot(*s_, cache_, x(), y(), w(), h() - 18, v, true);
        fl_color(fl_rgb_color(240, 240, 240));
        zd::rectf(x(), y() + h() - 18, w(), 18);
        zd::font(kFont, 11);
        fl_color(fl_rgb_color(90, 90, 90));
        std::string info = readout_.empty() ? "wheel: zoom (Shift: vertically) · drag: move · r: reset · e: export PNG" : readout_;
        zd::draw(info.c_str(), x() + 8, y() + h() - 5);
    }
    int handle(int e) override {
        switch (e) {
        case FL_ENTER: return 1;
        case FL_FOCUS: case FL_UNFOCUS: return 1;
        case FL_MOVE: updateReadout(); redraw(); return 1;
        case FL_LEAVE: readout_.clear(); redraw(); return 1;
        case FL_PUSH:
            take_focus();
            if (Fl::event_clicks()) { Fl::event_clicks(0); reset(); return 1; }
            dragX_ = Fl::event_x(); dragY_ = Fl::event_y(); dragView_ = view();
            return 1;
        case FL_DRAG: {
            Range a = dragView_;
            double dx = (Fl::event_x() - dragX_) * (a.x1 - a.x0) / plotW(), dy = (Fl::event_y() - dragY_) * (a.y1 - a.y0) / plotH();
            v_ = {a.x0 - dx, a.x1 - dx, a.y0 + dy, a.y1 + dy};
            zoomed_ = true;
            redraw();
            return 1;
        }
        case FL_MOUSEWHEEL: {
            Range a = view();
            double f = Fl::event_dy() > 0 ? 1.25 : 0.8;
            double mx, my;
            toData(Fl::event_x(), Fl::event_y(), a, mx, my);
            if (Fl::event_state(FL_SHIFT)) { a.y0 = my + (a.y0 - my) * f; a.y1 = my + (a.y1 - my) * f; }
            else { a.x0 = mx + (a.x0 - mx) * f; a.x1 = mx + (a.x1 - mx) * f; }
            v_ = a;
            zoomed_ = true;
            updateReadout();
            redraw();
            return 1;
        }
        case FL_KEYBOARD:
            if (Fl::event_key() == 'r' || Fl::event_key() == '0') { reset(); return 1; }
            if (Fl::event_key() == 'e') { exportPng(); return 1; }
            return 0;
        }
        return Fl_Widget::handle(e);
    }

private:
    bool image() const { return s_->kind() == "image"; }
    int left() const { return 62; }
    int top() const { return s_->title.empty() ? 14 : 32; }
    double plotW() const { return std::max(8, w() - 62 - (image() ? 70 : 18)); }
    double plotH() const { return std::max(8, h() - 18 - top() - 40); }
    void toData(int ex, int ey, const Range& v, double& dx, double& dy) const {
        dx = v.x0 + (ex - x() - left()) / plotW() * (v.x1 - v.x0);
        dy = v.y1 - (ey - y() - top()) / plotH() * (v.y1 - v.y0);
    }
    void updateReadout() {
        Range v = view();
        double dx, dy;
        toData(Fl::event_x(), Fl::event_y(), v, dx, dy);
        const PlotState& s = *s_;
        std::string r = (s.time ? num(dx) + " s" : "x " + num(dx));
        long c = (long)std::floor((dx - s.xStart) / s.xStep + (image() ? 0 : .5));
        if (image()) {
            long row = (long)std::floor(dy);
            if (row >= 0 && row < (long)s.rows.size() && c >= 0 && c < (long)s.cols()) {
                std::string name = (size_t)row < s.labels.size() ? s.labels[(size_t)row]
                                   : s.rowValues ? num(s.rowValue((double)row)) : std::to_string(row);
                r += "   row " + name + "   value " + num(s.rows[(size_t)row][(size_t)c]);
            }
        } else if (s.kind() != "scatter" && c >= 0 && c < (long)s.cols()) {
            r += "   value";
            for (size_t k = 0; k < s.rows.size() && k < 6; ++k) r += " " + num(s.rows[k][(size_t)c]);
        } else {
            r += "   y " + num(s.ylog ? std::pow(2.0, dy) : dy);
        }
        readout_ = r;
    }
    std::shared_ptr<PlotState> s_;
    ImageCache cache_;
    bool zoomed_ = false;
    Range v_, dragView_;
    int dragX_ = 0, dragY_ = 0;
    std::string readout_;
};

class PlotWindow : public Fl_Double_Window {
public:
    PlotWindow(std::shared_ptr<PlotState> s) : Fl_Double_Window(820, 520) {
        view_ = new PlotView(0, 0, 820, 520, std::move(s));
        resizable(view_);
        end();
    }
    void refresh(const std::string& title) {
        copy_label(title.c_str());
        view_->redraw();
    }

private:
    PlotView* view_;
};

// the channels of a sample buffer as the rows of a plot (time in seconds)
void loadBuffer(PlotState& st, const SampleBuffer& sb, int channel) {
    auto& R = st.rows;
    int c0 = 0, c1 = sb.channels;
    if (channel >= 0) { c0 = std::min(channel, sb.channels - 1); c1 = c0 + 1; }
    R.assign((size_t)std::max(0, c1 - c0), std::vector<float>((size_t)std::max(0L, sb.frames)));
    for (long f = 0; f < sb.frames; ++f)
        for (int c = c0; c < c1; ++c) R[(size_t)(c - c0)][(size_t)f] = sb.data[(size_t)f * sb.channels + c];
    st.time = sb.sr > 1.5;                         // a rate of 1: a plain matrix (no time axis)
    st.xStart = 0;
    st.xStep = st.time ? 1.0 / sb.sr : 1.0;
    ++st.version;
}

// ---------------------------------------------------------------- plot
class PlotBox : public Box {
public:
    explicit PlotBox(const Message& a) : s_(std::make_shared<PlotState>()) {
        kind = BoxKind::UI;
        setPorts({P::Control, P::Control}, {P::Control, P::Control});
        size_t k = 0;
        std::vector<float> nums;
        for (; k < a.size(); ++k) {
            if (a[k].isFloat()) nums.push_back(a[k].f);
            else if (isMode(a[k].s)) s_->mode = a[k].s;
            else bufName_ = argName_ = a[k].s;               // a buffer~ to show
        }
        w_ = std::clamp((int)(nums.size() > 0 ? nums[0] : 300.f), 60, 3000);
        h_ = std::clamp((int)(nums.size() > 1 ? nums[1] : 140.f), 40, 3000);
    }
    ~PlotBox() override {
        if (win_) { win_->hide(); Fl::delete_widget(win_); }
    }
    void uiSize(int& w, int& h) const override { w = w_; h = h_; }
    Message saveArgs() const override {
        Message m;
        if (!argName_.empty()) m.push_back(Atom(argName_));
        m.push_back(Atom((float)w_));
        m.push_back(Atom((float)h_));
        if (s_->mode != "auto") m.push_back(Atom(s_->mode));
        return m;
    }
    void resolve() override {
        if (bufName_.empty()) return;
        BufferBox* b = findBuffer(patch, bufName_);
        if (b != b_) { b_ = b; seen_ = -1; }
    }
    void message(int inlet, const Message& m) override {
        if (inlet == 1) {                                     // marks
            s_->marks.clear();
            for (auto& x : m)
                if (x.isFloat()) s_->marks.push_back(x.f);
            changed();
            return;
        }
        if (m[0].isFloat()) { add(m); return; }
        const std::string& w = m[0].s;
        if (w == "buffer" && m.size() > 1) {
            bufName_ = m[1].str();
            channel_ = m.size() > 2 && m[2].isFloat() ? (int)m[2].f : -1;
            b_ = findBuffer(patch, bufName_);
            seen_ = -1;
            if (!b_) patch->print("plot: no buffer~ " + bufName_);
            load();
        } else if (w == "mode" && m.size() > 1 && isMode(m[1].str())) {
            s_->mode = m[1].str();
            changed();
        } else if (w == "range") {
            if (m.size() > 2 && m[1].isFloat() && m[2].isFloat() && m[2].f > m[1].f) { s_->fixed = true; s_->lo = m[1].f; s_->hi = m[2].f; }
            else s_->fixed = false;
            changed();
        } else if (w == "rows") {
            if (m.size() > 2 && m[1].isFloat() && m[2].isFloat()) { s_->rowValues = true; s_->rowFirst = m[1].f; s_->rowLast = m[2].f; }
            else s_->rowValues = false;
            changed();
        } else if (w == "xrange") {
            if (m.size() > 2 && m[1].isFloat() && m[2].isFloat() && m[2].f > m[1].f) { s_->xfixed = true; s_->xlo = m[1].f; s_->xhi = m[2].f; }
            else s_->xfixed = false;
            changed();
        } else if (w == "marks") {
            s_->marks.clear();
            for (size_t k = 1; k < m.size(); ++k)
                if (m[k].isFloat()) s_->marks.push_back(m[k].f);
            changed();
        } else if (w == "cursor") {
            s_->cursor = argf(m, 1, -1.f);
            changed();
        } else if (w == "title") {
            s_->title = messageToString(tailOf(m));
            changed();
        } else if (w == "labels") {
            s_->labels.clear();
            static const char* const pcs[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            if (m.size() == 2 && m[1].is("chroma"))
                for (auto n : pcs) s_->labels.push_back(n);
            else if (m.size() == 2 && m[1].is("keys")) {
                static const char* const keys[] = {"C", "Cm", "Db", "C#m", "D", "Dm", "Eb", "Ebm", "E", "Em", "F", "Fm",
                                                   "F#", "F#m", "G", "Gm", "Ab", "G#m", "A", "Am", "Bb", "Bbm", "B", "Bm"};
                for (auto n : keys) s_->labels.push_back(n);
            }
            else
                for (size_t k = 1; k < m.size(); ++k) s_->labels.push_back(m[k].str());
            changed();
        } else if (w == "ylog") {
            s_->ylog = argf(m, 1, 1.f) != 0.f;
            changed();
        } else if (w == "normalize") {
            s_->normalize = argf(m, 1, 1.f) != 0.f;
            changed();
        } else if (w == "colors" && m.size() > 1) {
            s_->gray = m[1].is("gray");
            s_->types = m[1].is("types");
            changed();
        } else if (w == "history" && m.size() > 1) {
            s_->history = std::clamp((int)argf(m, 1, 256.f), 2, 100000);
        } else if (w == "clear") {
            bufName_.clear();
            b_ = nullptr;
            s_->rows.clear();
            s_->marks.clear();
            s_->cursor = -1;
            s_->time = false;
            s_->source.clear();
            changed();
        } else if (w == "open") {
            open();
        } else if (w == "export" && m.size() > 1) {
            int W = std::clamp((int)argf(m, 2, 900.f), 100, 8000), H = std::clamp((int)argf(m, 3, 500.f), 100, 8000);
            reload();
            fl_open_display();
            std::string path = patch->resolvePath(m[1].str());
            if (!PlotView::writePng(*s_, path, W, H, s_->dataRange())) patch->print("plot: can't write " + path);
        } else if (w == "list" || w == "set") {
            add(tailOf(m));
        }
    }
    std::vector<std::string> menuItems() override { return {"Open window", "Export PNG..."}; }
    void menuAction(int k) override {
        if (k == 0) { open(); return; }
        std::string f = chooseFile(true, "Export the plot", "PNG\t*.png", "plot.png");
        if (!f.empty()) message(0, {Atom("export"), Atom(f)});
    }
    void uiMouse(int e, int mx, int my) override {
        if (e == FL_PUSH && Fl::event_clicks()) { Fl::event_clicks(0); open(); return; }
        if (e != FL_PUSH && e != FL_DRAG) return;
        reload();
        Range v = s_->dataRange();
        double fx = std::clamp((mx - 3.0) / (w - 6), 0.0, 1.0), fy = std::clamp((my - 3.0) / (h - 6), 0.0, 1.0);
        double x = v.x0 + fx * (v.x1 - v.x0), y = v.y1 - fy * (v.y1 - v.y0);
        if (s_->kind() == "image") y = s_->rowValue(std::floor(y));
        else if (s_->kind() != "scatter") {                    // the curve's value there
            long c = std::lround((x - s_->xStart) / s_->xStep);
            if (c >= 0 && c < (long)s_->cols() && !s_->rows.empty()) y = s_->rows[0][(size_t)c];
        }
        if (s_->ylog && s_->kind() != "image") y = std::pow(2.0, y);
        outletFloat(1, (float)y);
        outletFloat(0, (float)(s_->time ? x * 1000.0 : x));
    }
    void drawUI(int X, int Y) override {
        reload();
        drawPlot(*s_, cache_, X + 3, Y + 3, w - 6, h - 6, s_->dataRange(), false);
        if (s_->rows.empty()) {
            zd::font(kFont, 9);
            fl_color(kDim);
            std::string t = !bufName_.empty() ? (b_ ? bufName_ + ": empty" : "no buffer~ " + bufName_) : "plot";
            zd::draw(t.c_str(), X + 8, Y + h / 2 + 3);
        }
    }

private:
    static bool isMode(const std::string& s) {
        return s == "auto" || s == "line" || s == "bars" || s == "scatter" || s == "image";
    }
    // data arriving in the inlet: a number or a list
    void add(const Message& m) {
        if (!bufName_.empty()) { bufName_.clear(); b_ = nullptr; s_->rows.clear(); }
        s_->time = false;
        s_->source.clear();
        s_->xStart = 0;
        s_->xStep = 1;
        auto& R = s_->rows;
        if (m.size() == 1) {                                  // a scrolling curve
            if (R.size() != 1) R.assign(1, {});
            R[0].push_back(m[0].f);
            if ((int)R[0].size() > s_->history) R[0].erase(R[0].begin(), R[0].end() - s_->history);
        } else if (s_->kind() == "image" && s_->mode == "image") {   // a scrolling matrix
            if (!R.empty() && R.size() != m.size()) R.clear();
            if (R.empty()) R.assign(m.size(), {});
            for (size_t k = 0; k < m.size(); ++k) R[k].push_back(m[k].isFloat() ? m[k].f : 0.f);
            if ((int)R[0].size() > s_->history)
                for (auto& r : R) r.erase(r.begin(), r.end() - s_->history);
        } else if (s_->kind() == "scatter") {                 // x y pairs
            R.assign(2, {});
            for (size_t k = 0; k + 1 < m.size(); k += 2) {
                R[0].push_back(m[k].isFloat() ? m[k].f : 0.f);
                R[1].push_back(m[k + 1].isFloat() ? m[k + 1].f : 0.f);
            }
        } else {
            R.assign(1, {});
            for (auto& x : m) R[0].push_back(x.isFloat() ? x.f : 0.f);
        }
        changed();
    }
    void reload() {
        if (b_ && b_->version != seen_) load();
    }
    void load() {
        s_->rows.clear();
        s_->source = bufName_;
        if (!b_) { changed(); return; }
        seen_ = b_->version;
        loadBuffer(*s_, b_->buf, channel_);
        changed();
    }
    void changed() {
        ++s_->version;
        if (patch) patch->redraw();
        if (win_ && win_->shown()) win_->refresh(windowTitle());
    }
    std::string windowTitle() const {
        std::string t = !s_->title.empty() ? s_->title : !bufName_.empty() ? bufName_ : "plot";
        return t;
    }
    void open() {
        if (!win_) win_ = new PlotWindow(s_);
        reload();
        win_->refresh(windowTitle());
        win_->show();
    }
    std::shared_ptr<PlotState> s_;
    ImageCache cache_;
    PlotWindow* win_ = nullptr;
    std::string bufName_, argName_;
    BufferBox* b_ = nullptr;
    int channel_ = -1, seen_ = -1, w_, h_;
};

}  // namespace

void registerPlotObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"plot", "Interface", "curves, bars, scatter and matrices from a buffer~ or lists: [w h] [mode]", with<PlotBox>()},
    });
}

}  // namespace moritz
