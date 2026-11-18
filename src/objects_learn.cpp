// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_learn.cpp — machine learning on lists of numbers, after Musil's
// scientific library: knn (k nearest neighbours: regression and
// classification), kmeans (clustering), pca (principal components), zscore
// (standardisation) and linreg (linear regression).
//
// They all learn from examples given as messages:
//     add x1 x2 ...            a point (kmeans, pca, zscore)
//     add x1 x2 ... -> y1 ...  an input and its output (knn, linreg); the
//                              output may be words (labels) for knn
// `clear` forgets them, `write file` and `read file` save and load them as
// text (one example per line, as above without "add"), `print` shows how
// many there are. A list in the left inlet asks the model.

#include "objects.h"
#include "scientific/algorithms.h"

#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace moritz {
namespace {

// ---------------------------------------------------------------- examples
class LearnBox : public Box {
public:
    LearnBox(int outs) { setPorts({P::Control}, std::vector<Port>((size_t)outs, P::Control)); }
    void message(int, const Message& m) override {
        if (m[0].is("add")) {
            std::vector<float> x;
            Message y;
            bool out = false;
            for (size_t k = 1; k < m.size(); ++k) {
                if (m[k].is("->")) { out = true; continue; }
                if (out) y.push_back(m[k]);
                else if (m[k].isFloat()) x.push_back(m[k].f);
            }
            if (x.empty()) return;
            if (!X_.empty() && x.size() != X_[0].size()) {
                patch->print(cls + ": an example of " + std::to_string(x.size()) + " values; the others have " +
                             std::to_string(X_[0].size()));
                return;
            }
            X_.push_back(x);
            Y_.push_back(y);
            changed();
        } else if (m[0].is("clear")) {
            X_.clear();
            Y_.clear();
            changed();
        } else if (m[0].is("print")) {
            patch->print(cls + ": " + std::to_string(X_.size()) + " examples of " +
                         std::to_string(X_.empty() ? 0 : X_[0].size()) + " values" + summary());
        } else if (m[0].is("write") && m.size() > 1) {
            std::ofstream f(patch->resolvePath(m[1].str()));
            for (size_t i = 0; i < X_.size(); ++i) {
                for (float v : X_[i]) f << formatFloat(v) << ' ';
                if (!Y_[i].empty()) f << "-> " << messageToString(Y_[i]);
                f << '\n';
            }
            if (!f) patch->print(cls + ": can't write " + m[1].str());
        } else if (m[0].is("read") && m.size() > 1) {
            std::string src;
            if (!readFile(patch->resolvePath(m[1].str()), src)) { patch->print(cls + ": can't read " + m[1].str()); return; }
            X_.clear();
            Y_.clear();
            std::istringstream in(src);
            std::string line;
            while (std::getline(in, line)) {
                Message l = parseMessage(line);
                if (l.empty()) continue;
                l.insert(l.begin(), Atom("add"));
                message(0, l);
            }
        } else if (m[0].isFloat()) {
            std::vector<float> x;
            for (auto& a : m)
                if (a.isFloat()) x.push_back(a.f);
            query(x);
        } else {
            other(m);
        }
    }

protected:
    virtual void changed() {}
    virtual std::string summary() const { return ""; }
    virtual void query(const std::vector<float>& x) = 0;
    virtual void other(const Message& m) {}
    size_t dims() const { return X_.empty() ? 0 : X_[0].size(); }
    bool ready(const std::vector<float>& x) {
        if (X_.empty()) { patch->print(cls + ": no examples yet (add ...)"); return false; }
        if (x.size() != dims()) { patch->print(cls + ": expects " + std::to_string(dims()) + " values"); return false; }
        return true;
    }
    // mean and standard deviation of each input dimension
    void stats(std::vector<float>& mean, std::vector<float>& sd) const {
        size_t d = dims(), n = X_.size();
        mean.assign(d, 0.f);
        sd.assign(d, 1.f);
        if (n == 0) return;
        for (auto& x : X_)
            for (size_t j = 0; j < d; ++j) mean[j] += x[j] / n;
        for (size_t j = 0; j < d; ++j) {
            double v = 0;
            for (auto& x : X_) v += (double)(x[j] - mean[j]) * (x[j] - mean[j]);
            sd[j] = n > 1 ? (float)std::sqrt(v / n) : 1.f;          // as Musil's zscore: the population's
            if (sd[j] < 1e-9f) sd[j] = 1.f;
        }
    }
    static Message listOf(const std::vector<float>& v) {
        Message m;
        for (float x : v) m.push_back(Atom(x));
        return m;
    }
    std::vector<std::vector<float>> X_;
    std::vector<Message> Y_;
};

float dist2(const std::vector<float>& a, const std::vector<float>& b, const std::vector<float>* scale = nullptr) {
    float s = 0.f;
    for (size_t j = 0; j < a.size() && j < b.size(); ++j) {
        float d = a[j] - b[j];
        if (scale) d /= (*scale)[j];
        s += d * d;
    }
    return s;
}

// ---------------------------------------------------------------- knn
// knn [k]: the k nearest examples (3) to the list received. Regression: the
// mean of their outputs, or with `weighted 1` a mean weighted by inverse
// distance, which glides smoothly between the examples (as Wekinator). With
// `classify 1`, or with words as outputs, the most frequent output among
// them. Inputs are compared after standardisation (`normalize 0` to
// compare raw values). Outlets: the prediction, the nearest example's index.
class KnnBox : public LearnBox {
public:
    explicit KnnBox(const Message& a) : LearnBox(2) { k_ = std::clamp((int)argf(a, 0, 3.f), 1, 1000); }

private:
    void other(const Message& m) override {
        if (m[0].is("k")) k_ = std::clamp((int)argf(m, 1, 3.f), 1, 1000);
        else if (m[0].is("weighted")) weighted_ = argf(m, 1, 1.f) != 0.f;
        else if (m[0].is("classify")) classify_ = argf(m, 1, 1.f) != 0.f;
        else if (m[0].is("normalize")) normalize_ = argf(m, 1, 1.f) != 0.f;
    }
    void query(const std::vector<float>& x) override {
        if (!ready(x)) return;
        std::vector<float> mean, sd;
        stats(mean, sd);
        std::vector<std::pair<float, size_t>> d;
        for (size_t i = 0; i < X_.size(); ++i) d.push_back({dist2(x, X_[i], normalize_ ? &sd : nullptr), i});
        size_t k = std::min((size_t)k_, d.size());
        std::partial_sort(d.begin(), d.begin() + (long)k, d.end());
        outletFloat(1, (float)d[0].second);
        bool words = false;
        for (size_t i = 0; i < k; ++i)
            for (auto& a : Y_[d[i].second]) words = words || a.isSymbol();
        if (classify_ || words) {                                   // the most frequent output
            std::map<std::string, std::pair<int, size_t>> votes;   // text -> (count, first rank)
            for (size_t i = 0; i < k; ++i) {
                auto& v = votes[messageToString(Y_[d[i].second])];
                if (v.first++ == 0) v.second = i;
            }
            auto best = votes.begin();
            for (auto it = votes.begin(); it != votes.end(); ++it)
                if (it->second.first > best->second.first ||
                    (it->second.first == best->second.first && it->second.second < best->second.second))
                    best = it;
            outlet(0, Y_[d[best->second.second].second]);
            return;
        }
        size_t outDims = Y_[d[0].second].size();
        if (outDims == 0) return;
        std::vector<float> y(outDims, 0.f);
        double wsum = 0;
        for (size_t i = 0; i < k; ++i) {
            double w = weighted_ ? 1.0 / (std::sqrt(d[i].first) + 1e-6) : 1.0;
            if (weighted_ && d[i].first == 0.f) { y.assign(outDims, 0.f); wsum = 0; for (size_t j = 0; j < outDims; ++j) y[j] = argf(Y_[d[i].second], j, 0.f); wsum = 1; break; }
            for (size_t j = 0; j < outDims; ++j) y[j] += (float)(w * argf(Y_[d[i].second], j, 0.f));
            wsum += w;
        }
        for (auto& v : y) v = (float)(v / wsum);
        outlet(0, listOf(y));
    }
    int k_;
    bool weighted_ = false, classify_ = false, normalize_ = true;
};

// ---------------------------------------------------------------- kmeans
// kmeans: groups the points added into k clusters. `train k [iterations]`
// (k-means++ starts, then Lloyd's iterations, 100 at most; the best of 10
// starts, the one with the tightest clusters, is kept; `seed n`). A list
// then gives the index of its cluster (0..k-1) and that cluster's centroid;
// `centroid i` outputs a centroid. Points are standardised first
// (`normalize 0` to cluster raw values). `train` prints the cluster sizes.
class KmeansBox : public LearnBox {
public:
    KmeansBox() : LearnBox(2) {}

private:
    void changed() override { centroids_.clear(); }
    std::string summary() const override {
        return centroids_.empty() ? ", not trained" : ", " + std::to_string(centroids_.size()) + " clusters";
    }
    void other(const Message& m) override {
        if (m[0].is("train")) train(std::clamp((int)argf(m, 1, 2.f), 1, 1000), std::clamp((int)argf(m, 2, 100.f), 1, 10000));
        else if (m[0].is("seed")) seed_ = (unsigned)argf(m, 1, 1.f);
        else if (m[0].is("normalize")) normalize_ = argf(m, 1, 1.f) != 0.f;
        else if (m[0].is("centroid") && !centroids_.empty()) {
            int i = std::clamp((int)argf(m, 1, 0.f), 0, (int)centroids_.size() - 1);
            outlet(1, listOf(original(centroids_[(size_t)i])));
        }
    }
    std::vector<float> scaled(const std::vector<float>& x) const {
        std::vector<float> s(x);
        for (size_t j = 0; j < s.size(); ++j) s[j] = (s[j] - mean_[j]) / sd_[j];
        return s;
    }
    std::vector<float> original(const std::vector<float>& c) const {
        std::vector<float> s(c);
        for (size_t j = 0; j < s.size(); ++j) s[j] = s[j] * sd_[j] + mean_[j];
        return s;
    }
    void train(int k, int iters) {
        if (X_.empty()) { patch->print("kmeans: no points yet (add ...)"); return; }
        stats(mean_, sd_);
        if (!normalize_) { mean_.assign(dims(), 0.f); sd_.assign(dims(), 1.f); }
        std::vector<std::vector<float>> P;
        for (auto& x : X_) P.push_back(scaled(x));
        k = std::min(k, (int)P.size());
        std::vector<std::vector<double>> Pd;
        for (auto& p : P) Pd.emplace_back(p.begin(), p.end());
        std::vector<int> bestLabel;
        std::vector<std::vector<double>> C;
        ::kmeans_pp(Pd, k, iters, seed_, 10, bestLabel, C);   // Musil's kmeans (scientific/algorithms.h)
        std::vector<std::vector<float>> bestC;
        for (auto& c : C) bestC.emplace_back(c.begin(), c.end());
        centroids_ = bestC;
        std::vector<int> n((size_t)k, 0);
        for (int l : bestLabel) ++n[(size_t)l];
        std::string sizes;
        for (int c = 0; c < k; ++c) sizes += (c ? " " : "") + std::to_string(n[(size_t)c]);
        patch->print("kmeans: " + std::to_string(k) + " clusters of " + sizes + " points");
    }
    int nearest(const std::vector<float>& p) const {
        int best = 0;
        float bd = 1e30f;
        for (size_t c = 0; c < centroids_.size(); ++c) {
            float d = dist2(p, centroids_[c]);
            if (d < bd) { bd = d; best = (int)c; }
        }
        return best;
    }
    void query(const std::vector<float>& x) override {
        if (!ready(x)) return;
        if (centroids_.empty()) { patch->print("kmeans: not trained (train k)"); return; }
        int c = nearest(scaled(x));
        outlet(1, listOf(original(centroids_[(size_t)c])));
        outletFloat(0, (float)c);
    }
    std::vector<std::vector<float>> centroids_;
    std::vector<float> mean_, sd_;
    unsigned seed_ = 1;
    bool normalize_ = true;
};

// symmetric eigen-decomposition by Jacobi rotations (Musil's eig-sym):
// eigenvalues, and eigenvectors as the columns of V
void eigSym(std::vector<std::vector<double>> A, std::vector<double>& val, std::vector<std::vector<double>>& V) {
    size_t n = A.size();
    V.assign(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i) V[i][i] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j) off += A[i][j] * A[i][j];
        if (off < 1e-20) break;
        for (size_t p = 0; p < n; ++p)
            for (size_t q = p + 1; q < n; ++q) {
                if (std::fabs(A[p][q]) < 1e-300) continue;
                double th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
                double t = (th >= 0 ? 1 : -1) / (std::fabs(th) + std::sqrt(th * th + 1));
                double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (size_t k = 0; k < n; ++k) {
                    double akp = A[k][p], akq = A[k][q];
                    A[k][p] = c * akp - s * akq;
                    A[k][q] = s * akp + c * akq;
                }
                for (size_t k = 0; k < n; ++k) {
                    double apk = A[p][k], aqk = A[q][k];
                    A[p][k] = c * apk - s * aqk;
                    A[q][k] = s * apk + c * aqk;
                }
                for (size_t k = 0; k < n; ++k) {
                    double vkp = V[k][p], vkq = V[k][q];
                    V[k][p] = c * vkp - s * vkq;
                    V[k][q] = s * vkp + c * vkq;
                }
            }
    }
    val.resize(n);
    for (size_t i = 0; i < n; ++i) val[i] = A[i][i];
}

// ---------------------------------------------------------------- pca
// pca: the principal components of the points added. `train [dims]` (2 by
// default) finds the directions of largest variance (of the standardised
// points; `normalize 0` for raw ones); a list then gives its coordinates
// along them: a way to see many descriptors on two axes. `explained`
// outputs the share of the variance each component carries (right outlet).
class PcaBox : public LearnBox {
public:
    PcaBox() : LearnBox(2) {}

private:
    void changed() override { comps_.clear(); }
    std::string summary() const override { return comps_.empty() ? ", not trained" : ", trained"; }
    void other(const Message& m) override {
        if (m[0].is("train")) train(std::max(1, (int)argf(m, 1, 2.f)));
        else if (m[0].is("normalize")) normalize_ = argf(m, 1, 1.f) != 0.f;
        else if (m[0].is("explained")) outlet(1, listOf(explained_));
    }
    void train(int want) {
        if (X_.size() < 2) { patch->print("pca: needs at least two points"); return; }
        stats(mean_, sd_);
        if (!normalize_) sd_.assign(dims(), 1.f);
        size_t d = dims(), n = X_.size();
        std::vector<std::vector<double>> C(d, std::vector<double>(d, 0.0));
        for (auto& x : X_)
            for (size_t i = 0; i < d; ++i)
                for (size_t j = 0; j < d; ++j)
                    C[i][j] += (double)(x[i] - mean_[i]) / sd_[i] * (x[j] - mean_[j]) / sd_[j] / (n - 1);
        std::vector<double> val;
        std::vector<std::vector<double>> V;
        eigSym(C, val, V);
        std::vector<size_t> order(d);
        for (size_t i = 0; i < d; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return val[a] > val[b]; });
        double total = 0;
        for (double v : val) total += std::max(0.0, v);
        comps_.clear();
        explained_.clear();
        for (size_t r = 0; r < d && (int)r < want; ++r) {
            std::vector<float> c(d);
            for (size_t i = 0; i < d; ++i) c[i] = (float)V[i][order[r]];
            comps_.push_back(c);
            explained_.push_back(total > 0 ? (float)(std::max(0.0, val[order[r]]) / total) : 0.f);
        }
        outlet(1, listOf(explained_));
    }
    void query(const std::vector<float>& x) override {
        if (!ready(x)) return;
        if (comps_.empty()) { patch->print("pca: not trained (train)"); return; }
        std::vector<float> y;
        for (auto& c : comps_) {
            float s = 0.f;
            for (size_t i = 0; i < x.size(); ++i) s += (x[i] - mean_[i]) / sd_[i] * c[i];
            y.push_back(s);
        }
        outlet(0, listOf(y));
    }
    std::vector<std::vector<float>> comps_;
    std::vector<float> mean_, sd_, explained_;
    bool normalize_ = true;
};

// ---------------------------------------------------------------- zscore
// zscore: `train` measures the mean and standard deviation of each value
// over the points added; a list then comes out standardised, (x - mean) /
// sd, so values of different units (Hz, dB, 0..1) become comparable.
class ZscoreBox : public LearnBox {
public:
    ZscoreBox() : LearnBox(1) {}

private:
    void changed() override { trained_ = false; }
    void other(const Message& m) override {
        if (m[0].is("train")) {
            if (X_.empty()) { patch->print("zscore: no points yet (add ...)"); return; }
            stats(mean_, sd_);
            trained_ = true;
        }
    }
    void query(const std::vector<float>& x) override {
        if (!ready(x)) return;
        if (!trained_) { patch->print("zscore: not trained (train)"); return; }
        std::vector<float> y(x);
        for (size_t j = 0; j < y.size(); ++j) y[j] = (y[j] - mean_[j]) / sd_[j];
        outlet(0, listOf(y));
    }
    std::vector<float> mean_, sd_;
    bool trained_ = false;
};

// ---------------------------------------------------------------- linreg
// linreg: linear regression from the examples added (add x... -> y...):
// `train` fits y = b0 + b1 x1 + ... by least squares, one set of weights per
// output; a list then gives the predicted outputs. `weights` outputs them
// (right outlet), intercept first, output after output.
class LinregBox : public LearnBox {
public:
    LinregBox() : LearnBox(2) {}

private:
    void changed() override { B_.clear(); }
    void other(const Message& m) override {
        if (m[0].is("train")) train();
        else if (m[0].is("weights") && !B_.empty()) {
            std::vector<float> w;
            for (auto& b : B_) w.insert(w.end(), b.begin(), b.end());
            outlet(1, listOf(w));
        }
    }
    void train() {
        if (X_.empty() || Y_[0].empty()) { patch->print("linreg: needs examples with outputs (add x ... -> y ...)"); return; }
        size_t d = dims() + 1, outs = Y_[0].size();
        // normal equations (X^T X) b = X^T y, with a tiny ridge for stability
        std::vector<std::vector<double>> A(d, std::vector<double>(d + outs, 0.0));
        for (size_t s = 0; s < X_.size(); ++s) {
            std::vector<double> row(d, 1.0);
            for (size_t j = 1; j < d; ++j) row[j] = X_[s][j - 1];
            for (size_t i = 0; i < d; ++i) {
                for (size_t j = 0; j < d; ++j) A[i][j] += row[i] * row[j];
                for (size_t o = 0; o < outs; ++o) A[i][d + o] += row[i] * argf(Y_[s], o, 0.f);
            }
        }
        for (size_t i = 0; i < d; ++i) A[i][i] += 1e-9;
        for (size_t c = 0; c < d; ++c) {                  // Gauss-Jordan with partial pivoting
            size_t piv = c;
            for (size_t r = c + 1; r < d; ++r)
                if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
            std::swap(A[c], A[piv]);
            double p = A[c][c];
            if (std::fabs(p) < 1e-300) { patch->print("linreg: the inputs are not independent"); return; }
            for (auto& v : A[c]) v /= p;
            for (size_t r = 0; r < d; ++r)
                if (r != c && A[r][c] != 0.0) {
                    double f = A[r][c];
                    for (size_t j = 0; j < d + outs; ++j) A[r][j] -= f * A[c][j];
                }
        }
        B_.assign(outs, std::vector<float>(d));
        for (size_t o = 0; o < outs; ++o)
            for (size_t i = 0; i < d; ++i) B_[o][i] = (float)A[i][d + o];
    }
    void query(const std::vector<float>& x) override {
        if (!ready(x)) return;
        if (B_.empty()) { patch->print("linreg: not trained (train)"); return; }
        std::vector<float> y;
        for (auto& b : B_) {
            float s = b[0];
            for (size_t j = 0; j < x.size(); ++j) s += b[j + 1] * x[j];
            y.push_back(s);
        }
        outlet(0, listOf(y));
    }
    std::vector<std::vector<float>> B_;
};

}  // namespace

void registerLearnObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"knn", "Learning", "k nearest neighbours: regression, classification [k]", with<KnnBox>()},
        {"kmeans", "Learning", "clustering: add points, train k, then classify", plain<KmeansBox>()},
        {"pca", "Learning", "principal components: add points, train [dims], project", plain<PcaBox>()},
        {"zscore", "Learning", "standardise lists: add points, train, then (x - mean) / sd", plain<ZscoreBox>()},
        {"linreg", "Learning", "linear regression: add x -> y, train, predict", plain<LinregBox>()},
    });
}

}  // namespace moritz
