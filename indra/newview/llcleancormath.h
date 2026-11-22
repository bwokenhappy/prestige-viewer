/**
 * @file llcleancormath.h
 * @brief #1581 OPTIMISED CENTRE-OF-ROTATION SKINNING (Le & Hodgins, SIGGRAPH 2016, "Real-time
 *        Skeletal Skinning with Optimized Centers of Rotation"): the per-vertex centre-of-rotation
 *        precompute and a CPU reference of the GPU skinning (clean_preskin.comp, PRESKIN_COR).
 *        Header only, no viewer state: the module (llcleanrigcor.cpp) and its unit test
 *        (tests/llcleancormath_test.cpp) use it as is.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 *
 * THE CENTRE OF ROTATION (paper, eq. 4). Vertex i with bone weights w_i gets
 *     p*_i = sum_t s(w_i, w_t) a_t c_t / sum_t s(w_i, w_t) a_t
 * over the mesh triangles t (area a_t, centroid c_t, weights w_t = the mean of its corners'),
 * with the weight similarity (eq. 3, sigma = 0.1)
 *     s(w_p, w_v) = sum_{j != k} w_pj w_pk w_vj w_vk exp(-(w_pj w_vk - w_pk w_vj)^2 / sigma^2).
 * p*_i depends on w_i only (and the mesh), so vertices with equal weights share one result.
 *
 * ACCELERATION (the paper subdivides for accuracy and clusters for speed; both here):
 *   - triangles whose corner weights differ by more than kSubdivEps (L2) are split 1:4, at most
 *     kMaxDepth times (the integral then follows the weight field instead of the corners);
 *   - s() is a sum over bone PAIRS (j, k) whose term depends only on the two vectors' weights on
 *     j and k, so the integral splits exactly per pair. Per pair, the (sub)triangles carrying
 *     both bones are CLUSTERED by those two weights quantized to 1/kQuant: one record per
 *     cluster (summed area, area-weighted centroid, area-weighted pair weights);
 *   - a vertex visits only the clusters of its own pairs (<= 4 bones: <= 6 pairs);
 *   - vertices are deduplicated by their exact weights.
 * A vertex with no similar triangle (one bone only) keeps its own position: with p* = v the
 * skinning below is exactly linear blend skinning, so the fallback is the old result.
 *
 * SKINNING WITH BONE SCALE (SL palettes carry shape-slider / fitted-mesh / animated scale, and
 * shear where a scaled parent meets a rotated child). Each palette matrix A_j (its 3x3) is split
 * A_j = R_j S_j: R_j the closest rotation (Mueller et al. 2016, "A Robust Method to Extract the
 * Rotational Part of Deformations"), S_j = R_j^T A_j (general 3x3: scale and shear stay in it,
 * and R_j S_j = A_j exactly). Per vertex (weights w, CoR c, both in bind space):
 *     q   = normalize(sum_j w_j q_j)   (sign-aligned to the largest weight's quaternion)
 *     M   = R(q) * sum_j w_j S_j
 *     pos = M (v - c) + sum_j w_j (A_j c + t_j)
 *     nrm = M n                       (as the LBS path: the blended operator, no inverse transpose)
 * Rigid bones (S_j = I): the paper's R v + (LBS(c) - R c). Scale-only palettes (R_j = I): exactly
 * LBS, positions and normals. Identical bones: exactly the bone's affine. A degenerate blend
 * (|sum w q| ~ 0) keeps LBS for that vertex.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace LLCleanCoR
{
constexpr float    kSigma     = 0.1f;
constexpr uint32_t kMaxJoints = 110;      // palette lanes (clean_preskin.comp MAX_JOINTS)
constexpr uint32_t kJointKeys = 256;      // AVATAR joint ids (< 216, LLCleanSkinStore::MAX_AVATAR_JOINTS)
constexpr uint32_t kSparseCap = 16;
constexpr uint32_t kMaxDepth  = 2;        // subdivision levels (<= 16 sub-triangles per triangle)
constexpr float    kSubdivEps = 0.1f;     // split when two corners' weights differ by more (L2)
constexpr uint32_t kQuant     = 32;       // cluster key: pair weights in 1/32 steps (< sigma / 3)
constexpr float    kExpCut    = 16.f;     // similarity terms below exp(-16) of their bin are skipped
constexpr uint32_t kTab       = 64;       // per-pair inner sums tabulated on a 1/64 grid of the vertex pair weights

// One vertex's four influences as the pre-skin uses them: joints clamped to the palette,
// weights summing to 1 (all-zero -> the first influence).
struct Influence
{
    uint32_t j[4];
    float    w[4];
};

// = clean_preskin.comp loadWeights + its index clamp and zero-weight rule (10 B record, #582).
inline Influence decodeRecord(const uint8_t rec[10])
{
    Influence o;
    uint16_t q[3];
    std::memcpy(q, rec + 4, sizeof(q));
    const uint32_t qs = (uint32_t)q[0] + q[1] + q[2];
    const uint32_t q3 = qs >= 65535u ? 0u : 65535u - qs;
    const float k = 1.f / 65535.f;
    o.w[0] = (float)q[0] * k; o.w[1] = (float)q[1] * k; o.w[2] = (float)q[2] * k; o.w[3] = (float)q3 * k;
    for (int i = 0; i < 4; ++i)
        o.j[i] = std::min<uint32_t>(rec[i] & 0x7Fu, kMaxJoints - 1u);
    const float ws = o.w[0] + o.w[1] + o.w[2] + o.w[3];
    if (ws > 0.f)
        for (float& w : o.w) w *= 1.f / ws;
    else
    {
        o.w[0] = 1.f; o.w[1] = o.w[2] = o.w[3] = 0.f;
    }
    return o;
}

// ---- sparse weight vectors (sorted by joint, merged, no zeros) --------------------------
struct Sparse
{
    uint32_t n = 0;
    uint8_t  j[kSparseCap];
    float    w[kSparseCap];
};

inline void sparseAdd(Sparse& s, uint32_t joint, float w)
{
    if (!(w > 0.f)) return;
    const uint8_t jj = (uint8_t)std::min<uint32_t>(joint, kJointKeys - 1u);
    uint32_t i = 0;
    while (i < s.n && s.j[i] < jj) ++i;
    if (i < s.n && s.j[i] == jj) { s.w[i] += w; return; }
    if (s.n == kSparseCap)
    {
        // full: the smallest entry gives way (only reachable for deep blends of many bones)
        uint32_t m = 0;
        for (uint32_t k = 1; k < s.n; ++k) if (s.w[k] < s.w[m]) m = k;
        if (s.w[m] >= w) return;
        for (uint32_t k = m; k + 1 < s.n; ++k) { s.j[k] = s.j[k + 1]; s.w[k] = s.w[k + 1]; }
        --s.n;
        if (m < i) --i;
    }
    for (uint32_t k = s.n; k > i; --k) { s.j[k] = s.j[k - 1]; s.w[k] = s.w[k - 1]; }
    s.j[i] = jj; s.w[i] = w;
    ++s.n;
}

inline Sparse sparseOf(const Influence& in)
{
    Sparse s;
    for (int k = 0; k < 4; ++k) sparseAdd(s, in.j[k], in.w[k]);
    return s;
}

// a * fa + b * fb (+ c * fc)
inline Sparse sparseMix(const Sparse& a, float fa, const Sparse& b, float fb)
{
    Sparse s;
    for (uint32_t k = 0; k < a.n; ++k) sparseAdd(s, a.j[k], a.w[k] * fa);
    for (uint32_t k = 0; k < b.n; ++k) sparseAdd(s, b.j[k], b.w[k] * fb);
    return s;
}
inline Sparse sparseMix3(const Sparse& a, const Sparse& b, const Sparse& c)
{
    Sparse s;
    const float t = 1.f / 3.f;
    for (uint32_t k = 0; k < a.n; ++k) sparseAdd(s, a.j[k], a.w[k] * t);
    for (uint32_t k = 0; k < b.n; ++k) sparseAdd(s, b.j[k], b.w[k] * t);
    for (uint32_t k = 0; k < c.n; ++k) sparseAdd(s, c.j[k], c.w[k] * t);
    return s;
}

// L2 distance over the union of the supports.
inline float sparseDist(const Sparse& a, const Sparse& b)
{
    float d = 0.f;
    uint32_t i = 0, k = 0;
    while (i < a.n || k < b.n)
    {
        float x;
        if (k >= b.n || (i < a.n && a.j[i] < b.j[k])) x = a.w[i++];
        else if (i >= a.n || b.j[k] < a.j[i]) x = b.w[k++];
        else x = a.w[i++] - b.w[k++];
        d += x * x;
    }
    return std::sqrt(d);
}

// The paper's s(w_p, w_v) over UNORDERED bone pairs (half the paper's ordered sum: the factor
// cancels in the CoR quotient). Only bones both vectors carry contribute.
inline float similarity(const Sparse& a, const Sparse& b, float inv_sigma2 = 1.f / (kSigma * kSigma))
{
    float wa[kSparseCap], wb[kSparseCap];
    uint32_t m = 0, i = 0, k = 0;
    while (i < a.n && k < b.n)
    {
        if (a.j[i] < b.j[k]) ++i;
        else if (b.j[k] < a.j[i]) ++k;
        else { wa[m] = a.w[i++]; wb[m] = b.w[k++]; ++m; }
    }
    float s = 0.f;
    for (uint32_t x = 0; x < m; ++x)
        for (uint32_t y = x + 1; y < m; ++y)
        {
            const float d = wa[x] * wb[y] - wa[y] * wb[x];
            s += wa[x] * wa[y] * wb[x] * wb[y] * std::exp(-d * d * inv_sigma2);
        }
    return s;
}

// ---- the precompute ---------------------------------------------------------------------
struct Stats
{
    uint32_t tris = 0;        // non-degenerate source triangles
    uint32_t sub_tris = 0;    // after subdivision
    uint32_t clusters = 0;    // (bone pair, quantized pair weights) bins
    uint32_t unique = 0;      // distinct vertex weights evaluated
    uint32_t fallback = 0;    // of those, without a similar triangle (CoR = own position)
    uint64_t work = 0;        // similarity terms evaluated
    bool     aborted = false; // over the work cap: no result
};

namespace detail
{
// One CLUSTER of (sub)triangles for one bone pair (j, k): those whose weights on j and k
// quantize to the same 1/kQuant cell. s() is a sum over bone pairs and each pair's term
// depends only on the two weights of that pair, so the integral splits exactly per pair:
//   num_p = sum_{(j,k) in supp(p)} w_pj w_pk sum_c w_cj w_ck exp(-(w_pj w_ck - w_pk w_cj)^2 / sigma^2) a_c c_c
// and a cluster needs only its summed area, area-weighted centroid and pair weights.
struct Bin
{
    uint32_t pair = 0;      // j * kJointKeys + k (j < k)
    double   area = 0.0;
    double   wj = 0.0, wk = 0.0;               // area-weighted pair weights (sums)
    double   c[3] = {0.0, 0.0, 0.0};           // area-weighted centroid (sum)
};
struct VKey
{
    uint32_t e[kSparseCap * 2];
    uint32_t n = 0;
    bool operator==(const VKey& o) const { return n == o.n && std::memcmp(e, o.e, n * sizeof(uint32_t)) == 0; }
};
struct VKeyHash
{
    size_t operator()(const VKey& k) const
    {
        uint64_t h = 1469598103934665603ull ^ k.n;
        for (uint32_t i = 0; i < k.n; ++i) { h ^= k.e[i]; h *= 1099511628211ull; }
        return (size_t)(h ^ (h >> 29));
    }
};

struct Builder
{
    std::unordered_map<uint64_t, uint32_t> index;   // pair << 16 | qj << 8 | qk -> bin
    std::vector<Bin> bins;
    uint32_t sub_tris = 0;

    void add(const float* p0, const float* p1, const float* p2, const Sparse& s0, const Sparse& s1,
             const Sparse& s2, uint32_t depth)
    {
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
                    cz = e1[0] * e2[1] - e1[1] * e2[0];
        const double area = 0.5 * std::sqrt((double)cx * cx + (double)cy * cy + (double)cz * cz);
        if (!(area > 0.0) || !std::isfinite(area)) return;
        if (depth < kMaxDepth
            && std::max(sparseDist(s0, s1), std::max(sparseDist(s1, s2), sparseDist(s0, s2))) > kSubdivEps)
        {
            float m01[3], m12[3], m02[3];
            for (int k = 0; k < 3; ++k)
            {
                m01[k] = 0.5f * (p0[k] + p1[k]);
                m12[k] = 0.5f * (p1[k] + p2[k]);
                m02[k] = 0.5f * (p0[k] + p2[k]);
            }
            const Sparse w01 = sparseMix(s0, 0.5f, s1, 0.5f), w12 = sparseMix(s1, 0.5f, s2, 0.5f),
                         w02 = sparseMix(s0, 0.5f, s2, 0.5f);
            add(p0, m01, m02, s0, w01, w02, depth + 1);
            add(m01, p1, m12, w01, s1, w12, depth + 1);
            add(m02, m12, p2, w02, w12, s2, depth + 1);
            add(m01, m12, m02, w01, w12, w02, depth + 1);
            return;
        }
        ++sub_tris;
        const Sparse w = sparseMix3(s0, s1, s2);
        const double cen[3] = {((double)p0[0] + p1[0] + p2[0]) / 3.0, ((double)p0[1] + p1[1] + p2[1]) / 3.0,
                               ((double)p0[2] + p1[2] + p2[2]) / 3.0};
        auto q = [](float x) { return (uint32_t)std::lround(std::min(std::max(x, 0.f), 1.f) * (float)kQuant); };
        for (uint32_t x = 0; x < w.n; ++x)
            for (uint32_t y = x + 1; y < w.n; ++y)
            {
                const uint32_t pair = (uint32_t)w.j[x] * kJointKeys + w.j[y];
                const uint64_t key = ((uint64_t)pair << 16) | (q(w.w[x]) << 8) | q(w.w[y]);
                auto it = index.find(key);
                uint32_t bi;
                if (it == index.end())
                {
                    bi = (uint32_t)bins.size();
                    index.emplace(key, bi);
                    bins.emplace_back();
                    bins.back().pair = pair;
                }
                else bi = it->second;
                Bin& b = bins[bi];
                b.area += area;
                b.wj += area * w.w[x];
                b.wk += area * w.w[y];
                for (int k = 0; k < 3; ++k) b.c[k] += area * cen[k];
            }
    }
};

// A bin ready for evaluation: mean pair weights, area, mean centroid.
struct EvalBin
{
    float  wj, wk;
    double area;
    double c[3];
};
} // namespace detail

// CoR of every EVAL vertex, integrated over the GEO mesh (positions xyz, influences, triangle
// list). The two are the same slot normally; a decimated step (#561, a vertex subset of its
// source with byte-identical weights) is evaluated against its SOURCE's triangles, so every
// level of a mesh gets the same centre for the same weights. out: 3 floats per eval vertex,
// in the positions' space. Returns false (out untouched) when the work cap is hit.
inline bool computeCoR(const float* geo_pos, const Influence* geo_infl, uint32_t geo_nv, const uint32_t* idx,
                       uint32_t ni, const Influence* eval_infl, const float* eval_pos, uint32_t eval_n, float* out,
                       Stats& st, uint64_t max_work = 400000000ull)
{
    st = Stats();
    std::vector<Sparse> gs(geo_nv);
    for (uint32_t v = 0; v < geo_nv; ++v) gs[v] = sparseOf(geo_infl[v]);
    detail::Builder b;
    for (uint32_t t = 0; t + 2 < ni; t += 3)
    {
        const uint32_t a = idx[t], c1 = idx[t + 1], c2 = idx[t + 2];
        if (a >= geo_nv || c1 >= geo_nv || c2 >= geo_nv || a == c1 || c1 == c2 || a == c2) continue;
        const uint32_t before = b.sub_tris;
        b.add(&geo_pos[a * 3], &geo_pos[c1 * 3], &geo_pos[c2 * 3], gs[a], gs[c1], gs[c2], 0);
        if (b.sub_tris != before) ++st.tris;
    }
    st.sub_tris = b.sub_tris;
    st.clusters = (uint32_t)b.bins.size();
    // Bins grouped by pair: range[pair] = [begin, end) in `eb`.
    std::vector<uint32_t> order(b.bins.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return b.bins[x].pair < b.bins[y].pair; });
    std::vector<detail::EvalBin> eb(order.size());
    std::vector<uint32_t> range_lo((size_t)kJointKeys * kJointKeys, 0u), range_hi((size_t)kJointKeys * kJointKeys, 0u);
    for (uint32_t i = 0; i < (uint32_t)order.size(); ++i)
    {
        const detail::Bin& s = b.bins[order[i]];
        detail::EvalBin& e = eb[i];
        e.area = s.area;
        e.wj = (float)(s.wj / s.area);
        e.wk = (float)(s.wk / s.area);
        for (int k = 0; k < 3; ++k) e.c[k] = s.c[k] / s.area;
        if (i == 0 || b.bins[order[i - 1]].pair != s.pair) range_lo[s.pair] = i;
        range_hi[s.pair] = i + 1;
    }
    { std::unordered_map<uint64_t, uint32_t> t; b.index.swap(t); }   // (inputs no longer needed)
    { std::vector<detail::Bin> t; b.bins.swap(t); }
    const float inv_s2 = 1.f / (kSigma * kSigma);
    // A pair's inner sums  sum_c w_cj w_ck exp(-(x w_ck - y w_cj)^2 / sigma^2) a_c {c_c, 1}  are a
    // smooth function of the vertex's two pair weights (x, y) alone (its scale is sigma): they are
    // TABULATED on a 1/kTab grid, node by node as vertices need them, and interpolated. Cost:
    // (nodes used) x (clusters of the pair) instead of (distinct vertex weights) x (clusters) --
    // what lets a whole avatar's meshes be one integration domain.
    std::unordered_map<uint64_t, std::array<double, 4>> nodes;
    auto node = [&](uint32_t pair, uint32_t ix, uint32_t iy) -> const double*
    {
        const uint64_t key = ((uint64_t)pair << 16) | (ix << 8) | iy;
        auto it = nodes.find(key);
        if (it != nodes.end()) return it->second.data();
        const float x = (float)ix / (float)kTab, y = (float)iy / (float)kTab;
        std::array<double, 4> a = {0.0, 0.0, 0.0, 0.0};
        for (uint32_t i = range_lo[pair]; i < range_hi[pair]; ++i)
        {
            const detail::EvalBin& e = eb[i];
            const float d = x * e.wk - y * e.wj;
            const float ex = d * d * inv_s2;
            if (ex > kExpCut) continue;   // exp(-16) ~ 1e-7 of the bin's weight
            const double f = (double)(e.wj * e.wk * std::exp(-ex)) * e.area;
            a[0] += f * e.c[0]; a[1] += f * e.c[1]; a[2] += f * e.c[2];
            a[3] += f;
        }
        st.work += range_hi[pair] - range_lo[pair];
        return nodes.emplace(key, a).first->second.data();
    };
    std::unordered_map<detail::VKey, uint32_t, detail::VKeyHash> seen;
    seen.reserve(eval_n);
    std::vector<float> res;   // 3 per unique weight vector
    std::vector<uint32_t> map(eval_n);
    for (uint32_t v = 0; v < eval_n; ++v)
    {
        const Sparse s = sparseOf(eval_infl[v]);
        detail::VKey key;
        for (uint32_t i = 0; i < s.n; ++i)
        {
            uint32_t bits;
            std::memcpy(&bits, &s.w[i], 4);
            key.e[key.n++] = s.j[i];
            key.e[key.n++] = bits;
        }
        auto it = seen.find(key);
        if (it != seen.end()) { map[v] = it->second; continue; }
        const uint32_t u = (uint32_t)(res.size() / 3);
        seen.emplace(key, u);
        map[v] = u;
        double num[3] = {0.0, 0.0, 0.0}, den = 0.0;
        for (uint32_t x = 0; x < s.n; ++x)
            for (uint32_t y = x + 1; y < s.n; ++y)
            {
                const uint32_t pair = (uint32_t)s.j[x] * kJointKeys + s.j[y];
                if (range_lo[pair] >= range_hi[pair]) continue;
                const float px = std::min(std::max(s.w[x], 0.f), 1.f), py = std::min(std::max(s.w[y], 0.f), 1.f);
                // the pair's inner sums, bilinear between the four tabulated nodes around (px, py)
                const float gx = px * (float)kTab, gy = py * (float)kTab;
                const uint32_t ix = std::min((uint32_t)gx, kTab - 1u), iy = std::min((uint32_t)gy, kTab - 1u);
                const double fx = (double)gx - ix, fy = (double)gy - iy;
                const double* n00 = node(pair, ix, iy);
                const double* n10 = node(pair, ix + 1u, iy);
                const double* n01 = node(pair, ix, iy + 1u);
                const double* n11 = node(pair, ix + 1u, iy + 1u);
                double pn[4];
                for (int k = 0; k < 4; ++k)
                    pn[k] = (n00[k] * (1.0 - fx) + n10[k] * fx) * (1.0 - fy) + (n01[k] * (1.0 - fx) + n11[k] * fx) * fy;
                const double pw = (double)px * py;
                for (int k = 0; k < 3; ++k) num[k] += pw * pn[k];
                den += pw * pn[3];
            }
        if (st.work > max_work) { st.aborted = true; return false; }
        if (den > 1e-30 && std::isfinite(den))
        {
            res.push_back((float)(num[0] / den));
            res.push_back((float)(num[1] / den));
            res.push_back((float)(num[2] / den));
        }
        else
        {
            // one bone (or nothing similar): each vertex's OWN position = exactly LBS (header)
            res.push_back(NAN);
            res.push_back(NAN);
            res.push_back(NAN);
            ++st.fallback;
        }
    }
    st.unique = (uint32_t)(res.size() / 3);
    for (uint32_t v = 0; v < eval_n; ++v)
    {
        const float* r = &res[(size_t)map[v] * 3];
        if (!(std::isfinite(r[0]) && std::isfinite(r[1]) && std::isfinite(r[2]))) r = &eval_pos[v * 3];
        out[v * 3] = r[0]; out[v * 3 + 1] = r[1]; out[v * 3 + 2] = r[2];
    }
    return true;
}

// Brute force reference (no subdivision, no clustering, no pair index): O(V x T). Tests only.
inline void computeCoRReference(const float* pos, const Influence* infl, uint32_t nv, const uint32_t* idx, uint32_t ni,
                                float* out)
{
    for (uint32_t v = 0; v < nv; ++v)
    {
        const Sparse s = sparseOf(infl[v]);
        double num[3] = {0.0, 0.0, 0.0}, den = 0.0;
        for (uint32_t t = 0; t + 2 < ni; t += 3)
        {
            const float* p0 = &pos[idx[t] * 3];
            const float* p1 = &pos[idx[t + 1] * 3];
            const float* p2 = &pos[idx[t + 2] * 3];
            const double e1[3] = {(double)p1[0] - p0[0], (double)p1[1] - p0[1], (double)p1[2] - p0[2]};
            const double e2[3] = {(double)p2[0] - p0[0], (double)p2[1] - p0[1], (double)p2[2] - p0[2]};
            const double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
                         cz = e1[0] * e2[1] - e1[1] * e2[0];
            const double area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
            const Sparse w = sparseMix3(sparseOf(infl[idx[t]]), sparseOf(infl[idx[t + 1]]), sparseOf(infl[idx[t + 2]]));
            const double f = (double)similarity(s, w) * area;
            for (int k = 0; k < 3; ++k) num[k] += f * ((double)p0[k] + p1[k] + p2[k]) / 3.0;
            den += f;
        }
        for (int k = 0; k < 3; ++k) out[v * 3 + k] = den > 1e-30 ? (float)(num[k] / den) : pos[v * 3 + k];
    }
}

// ---- skinning reference (= clean_preskin.comp, PRESKIN_COR) ------------------------------
// 3x3 matrices column-major: m[c * 3 + r]. Quaternions (x, y, z, w).
struct Bone
{
    float a[9];   // the palette's 3x3 (rotation x scale x shear)
    float t[3];   // its translation
};
struct Decomp
{
    float q[4];   // R_j
    float s[9];   // S_j = R_j^T A_j
};

inline void quatToMat(const float q[4], float m[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1.f - 2.f * (y * y + z * z); m[1] = 2.f * (x * y + w * z);       m[2] = 2.f * (x * z - w * y);
    m[3] = 2.f * (x * y - w * z);       m[4] = 1.f - 2.f * (x * x + z * z); m[5] = 2.f * (y * z + w * x);
    m[6] = 2.f * (x * z + w * y);       m[7] = 2.f * (y * z - w * x);       m[8] = 1.f - 2.f * (x * x + y * y);
}

// m: a rotation (column-major). Element (row r, col c) = m[c * 3 + r].
inline void matToQuat(const float m[9], float q[4])
{
    const float m00 = m[0], m11 = m[4], m22 = m[8];
    const float m10 = m[1], m20 = m[2], m01 = m[3], m21 = m[5], m02 = m[6], m12 = m[7];
    const float tr = m00 + m11 + m22;
    if (tr > 0.f)
    {
        const float s = std::sqrt(tr + 1.f) * 2.f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    }
    else
    {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
}

inline void cross3(const float* a, const float* b, float* o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

constexpr int kPolarIters = 8;

// A = R S (see the header). Any A, singular and reflecting ones included: R is always a proper
// rotation (a zero-scale bone -- the SL hiding idiom -- gives R = I, S = 0).
inline Decomp decompose(const float a[9])
{
    Decomp d;
    // initial guess: Gram-Schmidt of the first two columns (a proper rotation)
    float r[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    {
        float x[3] = {a[0], a[1], a[2]}, y[3] = {a[3], a[4], a[5]};
        const float lx = std::sqrt(dot3(x, x));
        if (lx > 1e-12f)
        {
            for (float& c : x) c /= lx;
            const float d0 = dot3(y, x);
            for (int k = 0; k < 3; ++k) y[k] -= d0 * x[k];
            float ly = std::sqrt(dot3(y, y));
            if (!(ly > 1e-12f))
            {
                const float ax[3] = {std::fabs(x[0]) < 0.9f ? 1.f : 0.f, std::fabs(x[0]) < 0.9f ? 0.f : 1.f, 0.f};
                cross3(x, ax, y);
                ly = std::sqrt(dot3(y, y));
            }
            for (float& c : y) c /= ly;
            float z[3];
            cross3(x, y, z);
            for (int k = 0; k < 3; ++k) { r[k] = x[k]; r[3 + k] = y[k]; r[6 + k] = z[k]; }
        }
    }
    matToQuat(r, d.q);
    for (int it = 0; it < kPolarIters; ++it)
    {
        float rr[9];
        quatToMat(d.q, rr);
        float om[3] = {0.f, 0.f, 0.f}, c[3];
        for (int k = 0; k < 3; ++k)
        {
            cross3(&rr[k * 3], &a[k * 3], c);
            om[0] += c[0]; om[1] += c[1]; om[2] += c[2];
        }
        const float den = std::fabs(dot3(&rr[0], &a[0]) + dot3(&rr[3], &a[3]) + dot3(&rr[6], &a[6])) + 1e-9f;
        for (float& o : om) o /= den;
        const float w = std::sqrt(dot3(om, om));
        if (!(w > 1e-9f)) break;
        const float sh = std::sin(0.5f * w) / w, ch = std::cos(0.5f * w);
        const float dq[4] = {om[0] * sh, om[1] * sh, om[2] * sh, ch};
        // q = dq * q
        const float* q = d.q;
        float n[4];
        n[0] = dq[3] * q[0] + q[3] * dq[0] + (dq[1] * q[2] - dq[2] * q[1]);
        n[1] = dq[3] * q[1] + q[3] * dq[1] + (dq[2] * q[0] - dq[0] * q[2]);
        n[2] = dq[3] * q[2] + q[3] * dq[2] + (dq[0] * q[1] - dq[1] * q[0]);
        n[3] = dq[3] * q[3] - (dq[0] * q[0] + dq[1] * q[1] + dq[2] * q[2]);
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
        for (int k = 0; k < 4; ++k) d.q[k] = n[k] / l;
    }
    if (!(std::isfinite(d.q[0]) && std::isfinite(d.q[1]) && std::isfinite(d.q[2]) && std::isfinite(d.q[3])))
    {
        d.q[0] = d.q[1] = d.q[2] = 0.f; d.q[3] = 1.f;
    }
    float rr[9];
    quatToMat(d.q, rr);
    // S = R^T A: element (r, c) = dot(R column r, A column c)
    for (int c = 0; c < 3; ++c)
        for (int r2 = 0; r2 < 3; ++r2) d.s[c * 3 + r2] = dot3(&rr[r2 * 3], &a[c * 3]);
    return d;
}

inline void mulMat(const float a[9], const float b[9], float o[9])
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            o[c * 3 + r] = a[r] * b[c * 3] + a[3 + r] * b[c * 3 + 1] + a[6 + r] * b[c * 3 + 2];
}
inline void mulVec(const float m[9], const float v[3], float o[3])
{
    for (int r = 0; r < 3; ++r) o[r] = m[r] * v[0] + m[3 + r] * v[1] + m[6 + r] * v[2];
}

// = clean_preskin.comp's LBS: rot = sum w A, trans = sum w t; pos = rot v + trans, nrm = rot n.
inline void skinLBS(const Bone* bones, const Influence& in, const float v[3], const float n[3], float pos[3],
                    float nrm[3])
{
    float rot[9] = {}, tr[3] = {};
    for (int k = 0; k < 4; ++k)
    {
        const Bone& b = bones[in.j[k]];
        for (int e = 0; e < 9; ++e) rot[e] += b.a[e] * in.w[k];
        for (int e = 0; e < 3; ++e) tr[e] += b.t[e] * in.w[k];
    }
    mulVec(rot, v, pos);
    for (int e = 0; e < 3; ++e) pos[e] += tr[e];
    mulVec(rot, n, nrm);
}

// SAFEGUARD: |sum w q| (sign-aligned to the dominant bone) measures how far the vertex's bones'
// rotations agree: 1 = all equal; two bones at 180 deg, half each: 0.71; it only drops further
// with three or four bones pulling in very different directions, where normalising the blend
// makes the rotation swing fast across the region. There the CoR operator is eased linearly to
// LBS's (= clean_preskin.comp corBlend): never a hard switch between neighbouring vertices.
constexpr float kAgreeLo = 0.35f, kAgreeHi = 0.6f;
inline float agreement(float ql)
{
    float x = (ql - kAgreeLo) / (kAgreeHi - kAgreeLo);
    x = std::min(std::max(x, 0.f), 1.f);
    return x * x * (3.f - 2.f * x);
}

// = clean_preskin.comp corBlend. cor: the vertex's centre in bind space. Returns false (LBS
// kept) when the bones' rotations disagree completely (see agreement()).
inline bool skinCoR(const Bone* bones, const Decomp* dec, const Influence& in, const float cor[3], const float v[3],
                    const float n[3], float pos[3], float nrm[3])
{
    int piv = 0;
    for (int k = 1; k < 4; ++k) if (in.w[k] > in.w[piv]) piv = k;
    const float* qp = dec[in.j[piv]].q;
    float qb[4] = {}, sb[9] = {}, lc[3] = {};
    for (int k = 0; k < 4; ++k)
    {
        const Decomp& d = dec[in.j[k]];
        const float sgn = (d.q[0] * qp[0] + d.q[1] * qp[1] + d.q[2] * qp[2] + d.q[3] * qp[3]) < 0.f ? -in.w[k] : in.w[k];
        for (int e = 0; e < 4; ++e) qb[e] += d.q[e] * sgn;
        for (int e = 0; e < 9; ++e) sb[e] += d.s[e] * in.w[k];
        float ac[3];
        mulVec(bones[in.j[k]].a, cor, ac);
        for (int e = 0; e < 3; ++e) lc[e] += (ac[e] + bones[in.j[k]].t[e]) * in.w[k];
    }
    const float ql = std::sqrt(qb[0] * qb[0] + qb[1] * qb[1] + qb[2] * qb[2] + qb[3] * qb[3]);
    const float t = agreement(ql);
    if (!(t > 0.f))
    {
        skinLBS(bones, in, v, n, pos, nrm);
        return false;
    }
    for (float& e : qb) e /= ql;
    float r[9], m[9];
    quatToMat(qb, r);
    mulMat(r, sb, m);
    // = the shader's rot / trans form: rot = M, trans = LBS(c) - M c ...
    float mc[3], tr[3];
    mulVec(m, cor, mc);
    for (int e = 0; e < 3; ++e) tr[e] = lc[e] - mc[e];
    // ... eased toward LBS's rot / trans where the bones' rotations disagree (t < 1)
    if (t < 1.f)
    {
        float la[9] = {}, lt[3] = {};
        for (int k = 0; k < 4; ++k)
        {
            for (int e = 0; e < 9; ++e) la[e] += bones[in.j[k]].a[e] * in.w[k];
            for (int e = 0; e < 3; ++e) lt[e] += bones[in.j[k]].t[e] * in.w[k];
        }
        for (int e = 0; e < 9; ++e) m[e] = la[e] + (m[e] - la[e]) * t;
        for (int e = 0; e < 3; ++e) tr[e] = lt[e] + (tr[e] - lt[e]) * t;
    }
    float mv[3];
    mulVec(m, v, mv);
    for (int e = 0; e < 3; ++e) pos[e] = mv[e] + tr[e];
    mulVec(m, n, nrm);
    return true;
}
} // namespace LLCleanCoR
