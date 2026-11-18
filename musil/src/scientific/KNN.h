// KNN.h

#ifndef KNN_H
#define KNN_H

#include <valarray>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

// ---------------------------------------------------------
// Observation<T>: feature vector + class label
// ---------------------------------------------------------
template <typename T>
struct Observation {
    std::valarray<T> attributes;
    std::string      classlabel;
};

// ---------------------------------------------------------
// KNN<T>: simple k-nearest-neighbor classifier
//   - owns the Observation<T>* it is given
//   - k: number of neighbors
//   - n_features: dimensionality of the feature space
// ---------------------------------------------------------
template <typename T>
class KNN {
public:
    KNN(int k, int n_features)
        : m_k(k), m_n_features(n_features) {
        if (m_k <= 0) {
            throw std::invalid_argument("[KNN] k must be > 0");
        }
        if (m_n_features <= 0) {
            throw std::invalid_argument("[KNN] n_features must be > 0");
        }
    }

    ~KNN() {
        for (auto* o : m_observations) {
            delete o;
        }
    }

    void addObservation(Observation<T>* o) {
        if (!o) {
            throw std::invalid_argument("[KNN] null observation");
        }
        if (static_cast<int>(o->attributes.size()) != m_n_features) {
            throw std::runtime_error("[KNN] observation dimension mismatch");
        }
        m_observations.push_back(o);
    }

    std::string classify(const Observation<T>& query) const {
        if (m_observations.empty()) {
            throw std::runtime_error("[KNN] no observations in model");
        }
        if (static_cast<int>(query.attributes.size()) != m_n_features) {
            throw std::runtime_error("[KNN] query dimension mismatch");
        }

        const int k_eff = std::min(m_k, static_cast<int>(m_observations.size()));

        // (distance^2, index) pairs
        std::vector<std::pair<T, int>> dists;
        dists.reserve(m_observations.size());

        for (std::size_t i = 0; i < m_observations.size(); ++i) {
            const auto* obs = m_observations[i];
            if (obs->attributes.size() != query.attributes.size()) {
                throw std::runtime_error("[KNN] stored observation with wrong size");
            }

            T acc = 0;
            for (std::size_t j = 0; j < query.attributes.size(); ++j) {
                T d = query.attributes[j] - obs->attributes[j];
                acc += d * d;
            }
            dists.emplace_back(acc, static_cast<int>(i));
        }

        // the k nearest, ordered by distance and then by insertion order: the same neighbours on every system
        auto closer = [](const std::pair<T, int>& a, const std::pair<T, int>& b) {
            return a.first < b.first || (a.first == b.first && a.second < b.second);
        };
        std::partial_sort(dists.begin(), dists.begin() + k_eff, dists.end(), closer);

        // Vote among the k nearest; a tie goes to the label whose nearest member is nearest
        std::vector<std::pair<std::string, int>> votes;           // label, count, in order of first appearance
        for (int i = 0; i < k_eff; ++i) {
            const std::string& label = m_observations[dists[i].second]->classlabel;
            auto it = std::find_if(votes.begin(), votes.end(), [&](const auto& v) { return v.first == label; });
            if (it == votes.end()) votes.push_back({label, 1});
            else ++it->second;
        }
        std::string best_label;
        int best_count = -1;
        for (const auto& kv : votes) {
            if (kv.second > best_count) {
                best_count = kv.second;
                best_label = kv.first;
            }
        }

        return best_label;
    }

private:
    int m_k;
    int m_n_features;
    std::vector<Observation<T>*> m_observations;
};

#endif // KNN_H

// eof


