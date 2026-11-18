// scientific.h — linear algebra, statistics and machine learning, C++ half.
// The Musil half is scientific.mu. Ported from Musil 1.
//
// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
//
// A matrix is a list of row vectors: (list (vec 1 2) (vec 3 4)) is 2x2.
// (getidx M i) is row i, (getidx (getidx M i) j) the element at i,j.
// Only what needs an element loop in C++ is here: products, transpose,
// elimination (det, rank, inv, solve), the symmetric eigensolver, the running
// median, k-means and KNN. Everything that composes vector operations
// (statistics, covariance, PCA, regression, the
// helper layers) is written in Musil in scientific.mu at the same speed.

#pragma once
#include "core.h"
#include "scientific/algorithms.h"
#include "scientific/KNN.h"
#include "scientific/nmf.h"

namespace musil {

// --- dense row-major matrix used inside this file ---
struct mat {
    size_t r = 0, c = 0; std::vector<double> d;
    mat() {}
    mat(size_t rows, size_t cols, double v = 0) : r(rows), c(cols), d(rows * cols, v) {}
    double& operator()(size_t i, size_t j) { return d[i * c + j]; }
    double  operator()(size_t i, size_t j) const { return d[i * c + j]; }
};
// (list of equal-length vectors) -> mat, with the checks every function needs
inline mat to_mat(Interp& i, const vptr& v) {
    vlist& rows = i.list(v);
    if (rows.empty()) i.bad("empty matrix");
    size_t c = i.num(rows[0]).size();
    if (c == 0) i.bad("zero-length rows");
    mat m(rows.size(), c);
    for (size_t k = 0; k < rows.size(); k++) {
        const varr& row = i.num(rows[k]);
        if (row.size() != c) i.bad("ragged matrix: row " + std::to_string(k) + " has " + std::to_string(row.size()) + " columns, expected " + std::to_string(c));
        for (size_t j = 0; j < c; j++) m(k, j) = row[j];
    }
    return m;
}
inline vptr from_mat(const mat& m) {
    vlist rows; rows.reserve(m.r);
    for (size_t k = 0; k < m.r; k++) { varr row(m.c); for (size_t j = 0; j < m.c; j++) row[j] = m(k, j); rows.push_back(v_arr(std::move(row))); }
    return v_list(std::move(rows));
}
inline vptr from_vec(const std::vector<double>& v) { varr a(v.size()); for (size_t k = 0; k < v.size(); k++) a[k] = v[k]; return v_arr(std::move(a)); }
inline void square(Interp& i, const mat& m) { if (m.r != m.c) i.bad("matrix must be square (" + std::to_string(m.r) + "x" + std::to_string(m.c) + ")"); }

// --- products ---
// (mat-mul A B ...) matrix product, left to right
inline vptr sci_mat_mul(vlist& a, Interp& i) {
    mat acc = to_mat(i, a[0]);
    for (size_t k = 1; k < a.size(); k++) {
        mat b = to_mat(i, a[k]);
        if (acc.c != b.r) i.bad("nonconformant: " + std::to_string(acc.r) + "x" + std::to_string(acc.c) + " times " + std::to_string(b.r) + "x" + std::to_string(b.c));
        mat out(acc.r, b.c);
        for (size_t x = 0; x < acc.r; x++) for (size_t y = 0; y < acc.c; y++) { double v = acc(x, y); if (v == 0) continue; for (size_t z = 0; z < b.c; z++) out(x, z) += v * b(y, z); }
        acc = std::move(out);
    }
    return from_mat(acc);
}
// (transpose M)
inline vptr sci_transpose(vlist& a, Interp& i) {
    mat A = to_mat(i, a[0]); mat T(A.c, A.r);
    for (size_t r = 0; r < A.r; r++) for (size_t c = 0; c < A.c; c++) T(c, r) = A(r, c);
    return from_mat(T);
}

// --- decompositions by Gaussian elimination with partial pivoting ---
// The tolerances are relative to the matrix's scale (its largest element), so a matrix and the same matrix
// times 1e-12 have the same rank, determinant (to the factor) and inverse (times 1e12).
inline double max_abs(const mat& m, size_t cols) { double s = 0; for (size_t r = 0; r < m.r; r++) for (size_t c = 0; c < cols && c < m.c; c++) s = std::max(s, std::fabs(m(r, c))); return s; }
// Reduce M in place to row echelon form; a pivot below rel times the scale of the first `cols` columns (all by
// default) counts as zero; returns (rank, determinant-if-square, singular?)
struct elim_result { size_t rank; double det; bool singular; };
inline elim_result eliminate(mat& m, double rel = 1e-12, size_t cols = (size_t)-1) {
    size_t r = 0; double det = 1; bool sq = m.r == m.c;
    double eps = rel * max_abs(m, cols);
    for (size_t c = 0; c < m.c && c < cols && r < m.r; c++) {
        size_t piv = r; double best = std::fabs(m(r, c));
        for (size_t k = r + 1; k < m.r; k++) if (std::fabs(m(k, c)) > best) { best = std::fabs(m(k, c)); piv = k; }
        if (best <= eps) { det = 0; continue; }
        if (piv != r) { for (size_t j = 0; j < m.c; j++) std::swap(m(r, j), m(piv, j)); det = -det; }
        det *= m(r, c);
        for (size_t k = r + 1; k < m.r; k++) { double f = m(k, c) / m(r, c); if (f == 0) continue; for (size_t j = c; j < m.c; j++) m(k, j) -= f * m(r, j); }
        r++;
    }
    return { r, sq ? det : 0.0, !sq || r < m.r };
}
// (det M)
// (nmf V k iterations) => (list W H): V (r x c, non-negative) ~ W H with W r x k and H k x c, both
//   non-negative, by Lee & Seung's multiplicative updates for the KL divergence (the usual choice for
//   magnitude spectrograms; scientific/nmf.h, the factorisation of Moritz's nmf box). Columns of W are the
//   parts (spectral shapes, of unit sum), rows of H their activations over time (the scale is in H)
inline void nmf_check(Interp& i, const mat& V) { for (double v : V.d) if (!(v >= 0)) i.bad("V must be non-negative"); }
inline vptr sci_nmf(vlist& a, Interp& i) {
    mat V = to_mat(i, a[0]); nmf_check(i, V);
    long k = i.index(a[1]), it = i.index(a[2]);
    if (k < 1) i.bad("k must be >= 1"); if (it < 0) i.bad("iterations must be >= 0");
    std::vector<double> v(std::begin(V.d), std::end(V.d)), W(V.r * (size_t)k), H((size_t)k * V.c);
    nmf_init(W, (unsigned)i.rng()); nmf_init(H, (unsigned)i.rng());
    nmf_kl(v, (int)V.r, (int)V.c, (int)k, (int)it, W, H, false, [](int) { return true; });
    mat Wm(V.r, (size_t)k), Hm((size_t)k, V.c);
    std::copy(W.begin(), W.end(), std::begin(Wm.d)); std::copy(H.begin(), H.end(), std::begin(Hm.d));
    return v_list({ from_mat(Wm), from_mat(Hm) });
}
// (nmf-with-parts V W iterations) => H: the activations of known parts W (its columns), V ~ W H with W
//   fixed: supervised NMF, when the parts were learnt from examples of each source
inline vptr sci_nmf_with_parts(vlist& a, Interp& i) {
    mat V = to_mat(i, a[0]), Wm = to_mat(i, a[1]); nmf_check(i, V);
    long it = i.index(a[2]);
    if (Wm.r != V.r) i.bad("W must have as many rows as V"); if (it < 0) i.bad("iterations must be >= 0");
    size_t k = Wm.c;
    std::vector<double> v(std::begin(V.d), std::end(V.d)), W(std::begin(Wm.d), std::end(Wm.d)), H(k * V.c);
    nmf_init(H, (unsigned)i.rng());
    nmf_kl(v, (int)V.r, (int)V.c, (int)k, (int)it, W, H, true, [](int) { return true; });
    mat Hm(k, V.c); std::copy(H.begin(), H.end(), std::begin(Hm.d));
    return from_mat(Hm);
}
inline vptr sci_det(vlist& a, Interp& i) { mat m = to_mat(i, a[0]); square(i, m); return v_num(eliminate(m).det); }
// (rank M)
inline vptr sci_rank(vlist& a, Interp& i) { mat m = to_mat(i, a[0]); return v_num((double)eliminate(m, 1e-10).rank); }
// (inv M) by Gauss-Jordan on [M | I]
inline vptr sci_inv(vlist& a, Interp& i) {
    mat m = to_mat(i, a[0]); square(i, m); size_t n = m.r;
    double eps = 1e-12 * max_abs(m, n);
    mat aug(n, 2 * n);
    for (size_t r = 0; r < n; r++) { for (size_t c = 0; c < n; c++) aug(r, c) = m(r, c); aug(r, n + r) = 1; }
    for (size_t c = 0; c < n; c++) {
        size_t piv = c; double best = std::fabs(aug(c, c));
        for (size_t k = c + 1; k < n; k++) if (std::fabs(aug(k, c)) > best) { best = std::fabs(aug(k, c)); piv = k; }
        if (best <= eps) i.bad("matrix is singular");
        if (piv != c) for (size_t j = 0; j < 2 * n; j++) std::swap(aug(c, j), aug(piv, j));
        double p = aug(c, c); for (size_t j = 0; j < 2 * n; j++) aug(c, j) /= p;
        for (size_t k = 0; k < n; k++) { if (k == c) continue; double f = aug(k, c); if (f == 0) continue; for (size_t j = 0; j < 2 * n; j++) aug(k, j) -= f * aug(c, j); }
    }
    mat out(n, n); for (size_t r = 0; r < n; r++) for (size_t c = 0; c < n; c++) out(r, c) = aug(r, n + c);
    return from_mat(out);
}
// (solve A b) => x with A x = b
inline vptr sci_solve(vlist& a, Interp& i) {
    mat A = to_mat(i, a[0]); square(i, A); const varr& b = i.num(a[1]); size_t n = A.r;
    if (b.size() != n) i.bad("b must have " + std::to_string(n) + " elements, got " + std::to_string(b.size()));
    mat aug(n, n + 1);
    for (size_t r = 0; r < n; r++) { for (size_t c = 0; c < n; c++) aug(r, c) = A(r, c); aug(r, n) = b[r]; }
    elim_result e = eliminate(aug, 1e-12, n);                 // the pivots of A, against A's scale (not b's)
    if (e.rank < n) i.bad("matrix is singular");
    varr x(0.0, n);
    for (size_t k = n; k-- > 0;) { double s = aug(k, n); for (size_t j = k + 1; j < n; j++) s -= aug(k, j) * x[j]; x[k] = s / aug(k, k); }
    return v_arr(std::move(x));
}

// --- matching pursuit ---
// least squares on the normal equations G w = b (G p x p, symmetric positive semi-definite): false if G is singular
inline bool ls_solve(const mat& G, const std::vector<double>& b, const std::vector<size_t>& idx, std::vector<double>& w) {
    size_t p = idx.size(); mat A(p, p + 1);
    for (size_t u = 0; u < p; u++) { for (size_t v = 0; v < p; v++) A(u, v) = G(idx[u], idx[v]); A(u, p) = b[idx[u]]; }
    elim_result e = eliminate(A, 1e-12, p);
    if (e.rank < p) return false;
    w.assign(p, 0.0);
    for (size_t k = p; k-- > 0;) { double s = A(k, p); for (size_t j = k + 1; j < p; j++) s -= A(k, j) * w[j]; w[k] = s / A(k, k); }
    return true;
}
// Non-negative least squares (Lawson & Hanson 1974, on the normal equations): the w >= 0 that minimises
// |x - sum w_u d_u|^2, given G (the atoms' inner products) and b (their inner products with x). The active set
// grows by the variable whose gradient is largest; a step that would make a weight negative stops at the boundary
// and frees that variable
inline std::vector<double> nnls(const mat& G, const std::vector<double>& b) {
    size_t p = b.size(); std::vector<double> w(p, 0.0); std::vector<char> in(p, 0);
    double bmax = 0; for (double v : b) bmax = std::max(bmax, std::fabs(v));
    const double tol = 1e-12 * bmax;                                   // a gradient below this is 0
    for (size_t outer = 0; outer < 3 * p + 3; outer++) {
        std::vector<double> grad(p);                                  // b - G w: the gradient of the fit
        for (size_t u = 0; u < p; u++) { double s = b[u]; for (size_t v = 0; v < p; v++) s -= G(u, v) * w[v]; grad[u] = s; }
        long j = -1; double best = tol;
        for (size_t u = 0; u < p; u++) if (!in[u] && grad[u] > best) { best = grad[u]; j = (long)u; }
        if (j < 0) break;                                             // the Kuhn-Tucker conditions hold
        in[(size_t)j] = 1;
        for (size_t inner = 0; inner < 3 * p + 3; inner++) {
            std::vector<size_t> idx; for (size_t u = 0; u < p; u++) if (in[u]) idx.push_back(u);
            std::vector<double> s;
            if (!ls_solve(G, b, idx, s)) { in[(size_t)j] = 0; return w; }   // dependent atoms: keep what we have
            bool feasible = true; for (double v : s) if (v <= 0) feasible = false;
            if (feasible) { std::fill(w.begin(), w.end(), 0.0); for (size_t k = 0; k < idx.size(); k++) w[idx[k]] = s[k]; break; }
            double alpha = 1;                                          // the step to the first weight that reaches 0
            for (size_t k = 0; k < idx.size(); k++) if (s[k] <= 0) { double d = w[idx[k]] - s[k]; if (d > 0) alpha = std::min(alpha, w[idx[k]] / d); }
            for (size_t k = 0; k < idx.size(); k++) w[idx[k]] += alpha * (s[k] - w[idx[k]]);
            double wmax = 0; for (size_t k = 0; k < idx.size(); k++) wmax = std::max(wmax, std::fabs(w[idx[k]]));
            for (size_t k = 0; k < idx.size(); k++) if (w[idx[k]] <= 1e-14 * wmax) { w[idx[k]] = 0; in[idx[k]] = 0; }
        }
    }
    return w;
}
// (mp x D iterations threshold options) => a record: atoms (the indices of the chosen rows of D, in the order first
//   chosen), weights, residual, error (the residual's norm over x's), and approx (the reconstruction). x a vector, D a
//   dictionary (a matrix: one atom per row, the same length as x). Greedy: the atom most correlated with the
//   residual is taken with its projection weight and subtracted, until iterations are done, the error is below
//   threshold, or no atom helps. Plain matching pursuit may take an atom again (its weight grows: it is listed once);
//   orthogonal pursuit takes each atom once and refits every weight by least squares after each pick.
//   options: a record (keys symbols or strings) with 'nonneg (1: only atoms with a positive projection, weights kept
//   positive; the choice for magnitude spectra; with 'orthogonal the refit is a non-negative least squares,
//   Lawson-Hanson) and 'orthogonal (1: orthogonal matching pursuit)
inline vptr sci_mp(vlist& a, Interp& i) {
    const varr& xv = i.num(a[0]); mat D = to_mat(i, a[1]); long iters = (long)i.scalar(a[2]); double threshold = i.scalar(a[3]);
    bool nonneg = false, orth = false;
    if (a.size() > 4 && a[4]->t == Value::LIST) for (auto& kv : a[4]->l) if (kv->t == Value::LIST && kv->l.size() == 2) { std::string k = str_of(kv->l[0]); bool v = kv->l[1]->t == Value::NUM && kv->l[1]->num[0] != 0; if (k == "nonneg") nonneg = v; if (k == "orthogonal") orth = v; }
    size_t n = xv.size(), m = D.r; if (D.c != n) i.bad("mp: the atoms must have the length of the signal");
    std::vector<double> x(std::begin(xv), std::end(xv)), r = x, norms(m, 0.0);
    for (size_t k = 0; k < m; k++) { double s = 0; for (size_t j = 0; j < n; j++) s += D(k, j) * D(k, j); norms[k] = std::sqrt(s); }
    double xnorm = 0; for (double v : x) xnorm += v * v; xnorm = std::sqrt(xnorm); if (xnorm <= 0) xnorm = 1;
    std::vector<size_t> atoms; std::vector<double> weights;
    for (long it = 0; it < iters; it++) {
        double rnorm = 0; for (double v : r) rnorm += v * v; rnorm = std::sqrt(rnorm); if (rnorm / xnorm < threshold) break;
        long best = -1; double bestc = 0;
        for (size_t k = 0; k < m; k++) {
            if (norms[k] <= 0) continue;
            if (orth && std::find(atoms.begin(), atoms.end(), k) != atoms.end()) continue;   // orthogonal: each atom once
            double dot = 0; for (size_t j = 0; j < n; j++) dot += r[j] * D(k, j); double c = dot / norms[k];
            if (nonneg ? c > bestc : std::fabs(c) > std::fabs(bestc)) { bestc = c; best = (long)k; }
        }
        if (best < 0 || bestc == 0) break;
        double w = bestc / norms[(size_t)best];
        auto seen = std::find(atoms.begin(), atoms.end(), (size_t)best);
        if (seen != atoms.end()) weights[(size_t)(seen - atoms.begin())] += w;   // plain pursuit: the atom again
        else { atoms.push_back((size_t)best); weights.push_back(w); }
        if (orth) {                                            // refit every weight on the chosen atoms
            size_t p = atoms.size(); mat G(p, p); std::vector<double> bx(p);
            for (size_t u = 0; u < p; u++) { for (size_t v = 0; v < p; v++) { double s = 0; for (size_t j = 0; j < n; j++) s += D(atoms[u], j) * D(atoms[v], j); G(u, v) = s; } double s = 0; for (size_t j = 0; j < n; j++) s += D(atoms[u], j) * x[j]; bx[u] = s; }
            if (nonneg) weights = nnls(G, bx);
            else { std::vector<size_t> idx(p); for (size_t u = 0; u < p; u++) idx[u] = u; std::vector<double> wv; if (ls_solve(G, bx, idx, wv)) weights = wv; }
            r = x; for (size_t u = 0; u < p; u++) for (size_t j = 0; j < n; j++) r[j] -= weights[u] * D(atoms[u], j);
        } else { for (size_t j = 0; j < n; j++) r[j] -= w * D((size_t)best, j); }
    }
    varr res(n), approx(n); for (size_t j = 0; j < n; j++) { res[j] = r[j]; approx[j] = x[j] - r[j]; }
    double rnorm = 0; for (double v : r) rnorm += v * v; rnorm = std::sqrt(rnorm);
    vlist al, wl; for (size_t k = 0; k < atoms.size(); k++) { al.push_back(v_num((double)atoms[k])); wl.push_back(v_num(weights[k])); }
    return v_list({ v_list({ v_sym("atoms"), v_list(std::move(al)) }), v_list({ v_sym("weights"), v_list(std::move(wl)) }), v_list({ v_sym("residual"), v_arr(std::move(res)) }), v_list({ v_sym("error"), v_num(rnorm / xnorm) }), v_list({ v_sym("approx"), v_arr(std::move(approx)) }) });
}

// --- statistics ---
// (median-filter v order) the running median of width order
inline vptr sci_median_filter(vlist& a, Interp& i) {
    const varr& v = i.num(a[0]); long order = i.index(a[1]);
    if (order < 1) i.bad("order must be >= 1");
    long n = (long)v.size(), half = order / 2; varr out(n);
    for (long k = 0; k < n; k++) {
        long lo = std::max(0L, k - half), hi = std::min(n - 1, k + half);
        std::vector<double> w(std::begin(v) + lo, std::begin(v) + hi + 1);
        out[k] = median<double>(w.data(), (int)w.size());
    }
    return v_arr(std::move(out));
}
// (eig-sym M) => (list eigenvalues eigenvectors) of a symmetric matrix by Jacobi rotations;
// eigenvalues descending, eigenvectors as rows in the same order, each with its largest component positive.
inline vptr sci_eig_sym(vlist& a, Interp& i) {
    mat A = to_mat(i, a[0]); square(i, A); size_t n = A.r;
    double scale = max_abs(A, n), total = 0;                   // tolerances relative to the matrix's scale
    for (double v : A.d) total += v * v;
    for (size_t p = 0; p < n; p++) for (size_t q = p + 1; q < n; q++)
        if (std::fabs(A(p, q) - A(q, p)) > 1e-9 * scale) i.bad("matrix is not symmetric");
    mat V(n, n); for (size_t k = 0; k < n; k++) V(k, k) = 1;
    for (int sweep = 0; sweep < 100; sweep++) {
        double off = 0; for (size_t p = 0; p < n; p++) for (size_t q = p + 1; q < n; q++) off += A(p, q) * A(p, q);
        if (off <= 1e-30 * total) break;                       // the off-diagonal part under 1e-15 of the matrix's norm
        for (size_t p = 0; p < n; p++) for (size_t q = p + 1; q < n; q++) {
            if (std::fabs(A(p, q)) < 1e-300) continue;
            double theta = (A(q, q) - A(p, p)) / (2 * A(p, q));
            double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(1 + theta * theta));
            double c = 1 / std::sqrt(1 + t * t), sn = t * c;
            for (size_t k = 0; k < n; k++) { double akp = A(k, p), akq = A(k, q); A(k, p) = c * akp - sn * akq; A(k, q) = sn * akp + c * akq; }
            for (size_t k = 0; k < n; k++) { double apk = A(p, k), aqk = A(q, k); A(p, k) = c * apk - sn * aqk; A(q, k) = sn * apk + c * aqk; }
            for (size_t k = 0; k < n; k++) { double vkp = V(k, p), vkq = V(k, q); V(k, p) = c * vkp - sn * vkq; V(k, q) = sn * vkp + c * vkq; }
        }
    }
    std::vector<size_t> idx(n); for (size_t k = 0; k < n; k++) idx[k] = k;
    std::sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return A(x, x) > A(y, y); });
    varr vals(n); mat vecs(n, n);
    for (size_t r = 0; r < n; r++) {
        size_t k = idx[r]; vals[r] = A(k, k);
        size_t big = 0; for (size_t c = 0; c < n; c++) if (std::fabs(V(c, k)) > std::fabs(V(big, k))) big = c;
        double sgn = V(big, k) < 0 ? -1 : 1;
        for (size_t c = 0; c < n; c++) vecs(r, c) = sgn * V(c, k);
    }
    return v_list({ v_arr(std::move(vals)), from_mat(vecs) });
}

// --- machine learning ---
// (kmeans M k) => (list labels centroids): labels is a vector of cluster indices, centroids a k x d matrix.
//   k-means++ starts and Lloyd's iterations, ten starts, the tightest kept (the same as Moritz's kmeans box)
inline vptr sci_kmeans(vlist& a, Interp& i) {
    mat X = to_mat(i, a[0]); long k = i.index(a[1]);
    if (k < 1 || (size_t)k > X.r) i.bad("k must be between 1 and the number of rows");
    std::vector<std::vector<double>> P(X.r, std::vector<double>(X.c));
    for (size_t r = 0; r < X.r; r++) for (size_t c = 0; c < X.c; c++) P[r][c] = X.d[r * X.c + c];
    std::vector<int> labels; std::vector<std::vector<double>> cent;
    try { kmeans_pp(P, (int)k, 100, 1, 10, labels, cent); } catch (std::exception& e) { i.bad(e.what()); }
    mat C(k, X.c); for (long r = 0; r < k; r++) for (size_t c = 0; c < X.c; c++) C.d[r * X.c + c] = cent[(size_t)r][c];
    varr lv(X.r); for (size_t r = 0; r < X.r; r++) lv[r] = labels[r];
    return v_list({ v_arr(std::move(lv)), from_mat(C) });
}
// (knn training k queries) => list of predicted labels.
// training: list of (list features label); queries: list of feature vectors.
inline vptr sci_knn(vlist& a, Interp& i) {
    vlist& train = i.list(a[0]); long k = i.index(a[1]); vlist& queries = i.list(a[2]);
    if (train.empty()) i.bad("empty training set");
    if (k < 1) i.bad("k must be >= 1");
    vlist& first = i.list(train[0]);
    if (first.size() != 2) i.bad("each training sample is (list features label)");
    int features = (int)i.num(first[0]).size();
    KNN<double> model((int)k, features);
    try {
        for (auto& s : train) {
            vlist& pair = i.list(s);
            if (pair.size() != 2) i.bad("each training sample is (list features label)");
            auto* o = new Observation<double>();
            o->attributes = i.num(pair[0]); o->classlabel = str_of(pair[1]);
            model.addObservation(o);
        }
    } catch (std::exception& e) { i.bad(e.what()); }
    vlist out;
    for (auto& q : queries) {
        Observation<double> qo; qo.attributes = i.num(q);
        if ((int)qo.attributes.size() != features) i.bad("query has " + std::to_string(qo.attributes.size()) + " features, expected " + std::to_string(features));
        try { out.push_back(v_str(model.classify(qo))); } catch (std::exception& e) { i.bad(e.what()); }
    }
    return v_list(std::move(out));
}

inline void add_scientific(Interp& i) {
    const int N = -1;
    i.def("mat-mul", sci_mat_mul, 2, N); i.def("transpose", sci_transpose, 1, 1);
    i.def("det", sci_det, 1, 1); i.def("rank", sci_rank, 1, 1); i.def("inv", sci_inv, 1, 1); i.def("solve", sci_solve, 2, 2);
    i.def("eig-sym", sci_eig_sym, 1, 1); i.def("median-filter", sci_median_filter, 2, 2);
    i.def("kmeans", sci_kmeans, 2, 2); i.def("nmf", sci_nmf, 3, 3); i.def("nmf-with-parts", sci_nmf_with_parts, 3, 3); i.def("knn", sci_knn, 3, 3); i.def("mp", sci_mp, 4, 5);
}

} // namespace musil
