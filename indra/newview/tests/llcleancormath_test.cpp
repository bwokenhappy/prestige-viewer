/**
 * @file llcleancormath_test.cpp
 * @brief #1581 centre-of-rotation skinning maths (llcleancormath.h): the twisted cylinder (CoR
 *        keeps the radius where LBS collapses), rigid motion = LBS, scale-only palettes = LBS
 *        (uniform, non-uniform, singular), the polar split, and the CoR precompute fixture.
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
 */

#include "linden_common.h"

#include "../llcleancormath.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "../test/lltut.h"

namespace
{
    using namespace LLCleanCoR;

    constexpr float kPi = 3.14159265358979f;

    // An open cylinder along +x, radius `radius`, x in [0, len]: rings of `seg` vertices every
    // `dx`. Two bones: joint 0 (weight 1 - s), joint 1 (weight s), s a smoothstep over
    // [x0, x1]. Normals point away from the axis.
    struct Cylinder
    {
        std::vector<float> pos, nrm;
        std::vector<Influence> infl;
        std::vector<uint32_t> idx;
        uint32_t rings = 0, seg = 0;
    };

    Cylinder makeCylinder(float len, float dx, uint32_t seg, float radius, float x0, float x1)
    {
        Cylinder c;
        c.seg = seg;
        c.rings = (uint32_t)std::lround(len / dx) + 1;
        for (uint32_t r = 0; r < c.rings; ++r)
        {
            const float x = (float)r * dx;
            float s = std::min(std::max((x - x0) / (x1 - x0), 0.f), 1.f);
            s = s * s * (3.f - 2.f * s);
            for (uint32_t k = 0; k < seg; ++k)
            {
                const float a = 2.f * kPi * (float)k / (float)seg;
                c.pos.insert(c.pos.end(), {x, radius * std::cos(a), radius * std::sin(a)});
                c.nrm.insert(c.nrm.end(), {0.f, std::cos(a), std::sin(a)});
                Influence in;
                in.j[0] = 0; in.j[1] = 1; in.j[2] = 0; in.j[3] = 0;
                in.w[0] = 1.f - s; in.w[1] = s; in.w[2] = 0.f; in.w[3] = 0.f;
                c.infl.push_back(in);
            }
        }
        for (uint32_t r = 0; r + 1 < c.rings; ++r)
            for (uint32_t k = 0; k < seg; ++k)
            {
                const uint32_t a = r * seg + k, b = r * seg + (k + 1) % seg;
                const uint32_t cc = a + seg, d = b + seg;
                c.idx.insert(c.idx.end(), {a, b, d, a, d, cc});
            }
        return c;
    }

    Bone boneIdentity()
    {
        Bone b;
        const float a[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(b.a, a, sizeof(a));
        b.t[0] = b.t[1] = b.t[2] = 0.f;
        return b;
    }
    // rotation by `ang` about axis (unit) -> column-major 3x3
    void rotation(const float axis[3], float ang, float m[9])
    {
        const float s = std::sin(0.5f * ang);
        const float q[4] = {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(0.5f * ang)};
        quatToMat(q, m);
    }
    Bone boneRotX(float ang)
    {
        Bone b = boneIdentity();
        const float ax[3] = {1.f, 0.f, 0.f};
        rotation(ax, ang, b.a);
        return b;
    }

    float radiusOf(const float p[3]) { return std::sqrt(p[1] * p[1] + p[2] * p[2]); }

    std::vector<float> corOf(const Cylinder& c, Stats& st)
    {
        std::vector<float> cor(c.pos.size());
        const uint32_t nv = (uint32_t)c.infl.size();
        computeCoR(c.pos.data(), c.infl.data(), nv, c.idx.data(), (uint32_t)c.idx.size(), c.infl.data(), c.pos.data(),
                   nv, cor.data(), st);
        return cor;
    }

    float maxDiff3(const float* a, const float* b)
    {
        return std::max(std::fabs(a[0] - b[0]), std::max(std::fabs(a[1] - b[1]), std::fabs(a[2] - b[2])));
    }

    // CoR against LBS over every cylinder vertex with these bones; returns the largest position /
    // normal difference.
    void compareAll(const Cylinder& c, const std::vector<float>& cor, const Bone* bones, float& dpos, float& dnrm,
                    bool& finite)
    {
        Decomp dec[2] = {decompose(bones[0].a), decompose(bones[1].a)};
        dpos = dnrm = 0.f;
        finite = true;
        for (size_t v = 0; v < c.infl.size(); ++v)
        {
            float pl[3], nl[3], pc[3], nc[3];
            skinLBS(bones, c.infl[v], &c.pos[v * 3], &c.nrm[v * 3], pl, nl);
            skinCoR(bones, dec, c.infl[v], &cor[v * 3], &c.pos[v * 3], &c.nrm[v * 3], pc, nc);
            for (int k = 0; k < 3; ++k) finite = finite && std::isfinite(pc[k]) && std::isfinite(nc[k]);
            dpos = std::max(dpos, maxDiff3(pl, pc));
            dnrm = std::max(dnrm, maxDiff3(nl, nc));
        }
    }
}

namespace tut
{
    struct cormath_data {};
    typedef test_group<cormath_data> cormath_test;
    typedef cormath_test::object cormath_object;
    tut::cormath_test cormath("LLCleanCoR");

    // Two-bone twisted cylinder: a 120 degree twist of bone 1 about the axis. At the middle
    // (weights 0.5 / 0.5) LBS averages the two rotations and the radius collapses to
    // cos(60 deg) = 0.5 of the rest radius (the candy wrapper); CoR rotates about a centre on
    // the axis and keeps the radius everywhere.
    template<> template<>
    void cormath_object::test<1>()
    {
        const float radius = 0.1f;
        const Cylinder c = makeCylinder(2.f, 0.05f, 24, radius, 0.5f, 1.5f);
        Stats st;
        const std::vector<float> cor = corOf(c, st);
        ensure("precompute ran", !st.aborted && st.clusters > 0);
        Bone bones[2] = {boneIdentity(), boneRotX(2.f * kPi / 3.f)};
        Decomp dec[2] = {decompose(bones[0].a), decompose(bones[1].a)};
        float lbs_min = 1e9f, cor_min = 1e9f, cor_max = 0.f, lbs_mid = 0.f, cor_mid = 0.f;
        for (size_t v = 0; v < c.infl.size(); ++v)
        {
            float pl[3], nl[3], pc[3], nc[3];
            skinLBS(bones, c.infl[v], &c.pos[v * 3], &c.nrm[v * 3], pl, nl);
            skinCoR(bones, dec, c.infl[v], &cor[v * 3], &c.pos[v * 3], &c.nrm[v * 3], pc, nc);
            lbs_min = std::min(lbs_min, radiusOf(pl) / radius);
            cor_min = std::min(cor_min, radiusOf(pc) / radius);
            cor_max = std::max(cor_max, radiusOf(pc) / radius);
            if (std::fabs(c.pos[v * 3] - 1.f) < 1e-4f)
            {
                lbs_mid = radiusOf(pl) / radius;
                cor_mid = radiusOf(pc) / radius;
                // the normal stays radial: rotated with the surface
                const float nl2 = std::sqrt(nc[0] * nc[0] + nc[1] * nc[1] + nc[2] * nc[2]);
                ensure("CoR normal unit", std::fabs(nl2 - 1.f) < 1e-3f);
                ensure("CoR normal radial", std::fabs(nc[0]) < 1e-3f);
            }
        }
        ensure("LBS collapses to half the radius at the middle", std::fabs(lbs_mid - 0.5f) < 0.02f);
        ensure("LBS candy wrapper", lbs_min < 0.55f);
        ensure("CoR keeps the radius at the middle", std::fabs(cor_mid - 1.f) < 0.01f);
        ensure("CoR keeps the radius everywhere", cor_min > 0.98f && cor_max < 1.02f);
    }

    // Pure rigid motion: every bone carries the same rotation + translation -> CoR = LBS.
    // And single-bone vertices under different bones: CoR = own position = LBS exactly.
    template<> template<>
    void cormath_object::test<2>()
    {
        const Cylinder c = makeCylinder(2.f, 0.05f, 16, 0.1f, 0.5f, 1.5f);
        Stats st;
        const std::vector<float> cor = corOf(c, st);
        const float ax[3] = {0.267261f, 0.534522f, 0.801784f};
        Bone b = boneIdentity();
        rotation(ax, 1.3f, b.a);
        b.t[0] = 3.f; b.t[1] = -1.f; b.t[2] = 0.5f;
        Bone same[2] = {b, b};
        float dp, dn;
        bool fin;
        compareAll(c, cor, same, dp, dn, fin);
        ensure("finite", fin);
        ensure("rigid: positions = LBS", dp < 1e-5f);
        ensure("rigid: normals = LBS", dn < 1e-5f);

        Bone diff[2] = {b, boneRotX(2.f)};
        diff[1].t[1] = 0.7f;
        Decomp dec[2] = {decompose(diff[0].a), decompose(diff[1].a)};
        float worst = 0.f;
        for (size_t v = 0; v < c.infl.size(); ++v)
        {
            if (c.infl[v].w[0] != 1.f && c.infl[v].w[1] != 1.f) continue;   // single-bone vertices only
            float pl[3], nl[3], pc[3], nc[3];
            skinLBS(diff, c.infl[v], &c.pos[v * 3], &c.nrm[v * 3], pl, nl);
            skinCoR(diff, dec, c.infl[v], &cor[v * 3], &c.pos[v * 3], &c.nrm[v * 3], pc, nc);
            worst = std::max(worst, std::max(maxDiff3(pl, pc), maxDiff3(nl, nc)));
        }
        ensure("single-bone vertices = LBS", worst < 1e-5f);
    }

    // Scale-only palettes (the shape sliders / fitted-mesh volumes, no rotation): CoR = LBS for
    // positions AND normals -- uniform, non-uniform, and a zero-scale (hidden) bone.
    template<> template<>
    void cormath_object::test<3>()
    {
        const Cylinder c = makeCylinder(2.f, 0.05f, 16, 0.1f, 0.5f, 1.5f);
        Stats st;
        const std::vector<float> cor = corOf(c, st);
        auto scaleBone = [](float sx, float sy, float sz, float tx, float ty, float tz)
        {
            Bone b = boneIdentity();
            b.a[0] = sx; b.a[4] = sy; b.a[8] = sz;
            b.t[0] = tx; b.t[1] = ty; b.t[2] = tz;
            return b;
        };
        const Bone cases[3][2] = {
            {scaleBone(1.3f, 1.3f, 1.3f, 0.f, 0.f, 0.f), scaleBone(0.7f, 0.7f, 0.7f, 0.1f, 0.f, 0.f)},     // uniform
            {scaleBone(1.5f, 0.6f, 1.1f, 0.f, 0.2f, 0.f), scaleBone(0.8f, 1.4f, 0.9f, -0.1f, 0.f, 0.3f)},  // non-uniform
            {scaleBone(1.f, 1.f, 1.f, 0.f, 0.f, 0.f), scaleBone(0.f, 0.f, 0.f, 0.5f, 0.5f, 0.5f)}};        // singular
        const char* names[3] = {"uniform", "non-uniform", "zero-scale"};
        for (int i = 0; i < 3; ++i)
        {
            float dp, dn;
            bool fin;
            compareAll(c, cor, cases[i], dp, dn, fin);
            ensure(std::string(names[i]) + ": finite", fin);
            ensure(std::string(names[i]) + ": positions = LBS", dp < 2e-5f);
            ensure(std::string(names[i]) + ": normals = LBS", dn < 2e-5f);
        }
        // Identical bones carrying rotation, non-uniform scale AND shear: the bone's affine.
        Bone sh = boneIdentity();
        const float a[9] = {1.2f, 0.3f, -0.1f, 0.2f, 0.7f, 0.4f, -0.3f, 0.1f, 1.6f};
        std::memcpy(sh.a, a, sizeof(a));
        sh.t[0] = 0.4f;
        const Bone same[2] = {sh, sh};
        float dp, dn;
        bool fin;
        compareAll(c, cor, same, dp, dn, fin);
        ensure("sheared identical bones: finite", fin);
        ensure("sheared identical bones: positions = LBS", dp < 2e-5f);
        ensure("sheared identical bones: normals = LBS", dn < 2e-5f);
        // A twist under non-uniform scale stays finite and keeps more volume than LBS.
        Bone tw[2] = {scaleBone(1.f, 1.5f, 0.8f, 0.f, 0.f, 0.f), boneRotX(2.f * kPi / 3.f)};
        compareAll(c, cor, tw, dp, dn, fin);
        ensure("scaled twist: finite", fin);
    }

    // The polar split: R S = A, R a proper rotation -- also for reflections, shear and a
    // singular matrix.
    template<> template<>
    void cormath_object::test<4>()
    {
        const float mats[5][9] = {
            {1, 0, 0, 0, 1, 0, 0, 0, 1},
            {0.f, 1.2f, 0.f, -0.8f, 0.f, 0.f, 0.f, 0.f, 1.5f},              // rotate 90 + scale
            {1.2f, 0.3f, -0.1f, 0.2f, 0.7f, 0.4f, -0.3f, 0.1f, 1.6f},       // general
            {-1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f},                 // reflection
            {0, 0, 0, 0, 0, 0, 0, 0, 0}};                                   // zero
        for (const float* a : mats)
        {
            const Decomp d = decompose(a);
            float r[9], rs[9];
            quatToMat(d.q, r);
            mulMat(r, d.s, rs);
            float err = 0.f;
            for (int k = 0; k < 9; ++k) err = std::max(err, std::fabs(rs[k] - a[k]));
            ensure("R S = A", err < 1e-5f);
            const float ql = std::sqrt(d.q[0] * d.q[0] + d.q[1] * d.q[1] + d.q[2] * d.q[2] + d.q[3] * d.q[3]);
            ensure("unit quaternion", std::fabs(ql - 1.f) < 1e-5f);
        }
        // a pure rotation of 170 degrees is recovered exactly
        const float ax[3] = {0.f, 0.6f, 0.8f};
        float rot[9];
        rotation(ax, 170.f * kPi / 180.f, rot);
        const Decomp d = decompose(rot);
        float err = 0.f;
        for (int k = 0; k < 9; ++k) err = std::max(err, std::fabs(d.s[k] - ((k % 4) == 0 ? 1.f : 0.f)));
        ensure("rotation: S = I", err < 1e-5f);
    }

    // The precompute fixture: single-bone vertices keep their position; blended vertices of
    // the cylinder get a centre on the axis; equal weights give equal centres; the accelerated
    // precompute matches the brute-force integral on a mesh fine enough not to subdivide; the
    // 10 B record decode follows the pre-skin's rules.
    template<> template<>
    void cormath_object::test<5>()
    {
        const Cylinder c = makeCylinder(2.f, 0.02f, 16, 0.1f, 0.5f, 1.5f);
        Stats st;
        const std::vector<float> cor = corOf(c, st);
        ensure("precompute ran", !st.aborted);
        ensure("weights deduplicated", st.unique < c.infl.size() / 4);
        std::vector<float> ref(c.pos.size());
        computeCoRReference(c.pos.data(), c.infl.data(), (uint32_t)c.infl.size(), c.idx.data(),
                            (uint32_t)c.idx.size(), ref.data());
        float worst_axis = 0.f, worst_ref = 0.f, worst_single = 0.f;
        for (size_t v = 0; v < c.infl.size(); ++v)
        {
            const float* p = &cor[v * 3];
            const bool single = c.infl[v].w[0] == 1.f || c.infl[v].w[1] == 1.f;
            if (single)
                worst_single = std::max(worst_single, maxDiff3(p, &c.pos[v * 3]));
            else
            {
                worst_axis = std::max(worst_axis, radiusOf(p));
                worst_ref = std::max(worst_ref, maxDiff3(p, &ref[v * 3]));
            }
            // equal weights (the same ring) -> the same centre
            const size_t first = (v / c.seg) * c.seg;
            if (!single) ensure("equal weights, equal centre", maxDiff3(p, &cor[first * 3]) < 1e-6f);
        }
        ensure_equals("single-bone vertices keep their position", worst_single, 0.f);
        ensure("blended centres on the axis", worst_axis < 1e-4f);
        ensure("matches the brute-force integral", worst_ref < 0.02f);

        // A coarse mesh (the twist zone two rings wide) is subdivided and stays on the axis.
        const Cylinder coarse = makeCylinder(2.f, 0.5f, 8, 0.1f, 0.75f, 1.25f);
        Stats sc;
        const std::vector<float> cc = corOf(coarse, sc);
        ensure("coarse mesh subdivided", sc.sub_tris > sc.tris);
        float wa = 0.f;
        for (size_t v = 0; v < coarse.infl.size(); ++v)
            if (coarse.infl[v].w[0] != 1.f && coarse.infl[v].w[1] != 1.f) wa = std::max(wa, radiusOf(&cc[v * 3]));
        ensure("coarse centres on the axis", wa < 1e-3f);

        // Four bones per vertex over a unit grid (a lattice of 12 bones, inverse-distance weights
        // quantized through the 10 B record): the accelerated precompute stays within 2 % of the
        // grid size of the brute-force integral.
        {
            const int n = 40;
            std::vector<float> gp;
            std::vector<Influence> gi;
            std::vector<uint32_t> gx;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    const float u = (float)x / (n - 1), w = (float)y / (n - 1);
                    gp.insert(gp.end(), {u, w, 0.1f * std::sin(6.f * u) * std::cos(5.f * w)});
                    float bd[4] = {1e9f, 1e9f, 1e9f, 1e9f};
                    int bj[4] = {0, 0, 0, 0};
                    for (int b = 0; b < 12; ++b)
                    {
                        const float bx = (b % 4 + 0.5f) / 4.f, by = (b / 4 + 0.5f) / 3.f;
                        const float d = (u - bx) * (u - bx) + (w - by) * (w - by);
                        for (int k = 0; k < 4; ++k)
                            if (d < bd[k])
                            {
                                for (int m = 3; m > k; --m) { bd[m] = bd[m - 1]; bj[m] = bj[m - 1]; }
                                bd[k] = d; bj[k] = b;
                                break;
                            }
                    }
                    float wt[4], s = 0.f;
                    for (int k = 0; k < 4; ++k) { wt[k] = 1.f / (bd[k] + 1e-2f); s += wt[k]; }
                    uint8_t rec[10] = {(uint8_t)bj[0], (uint8_t)bj[1], (uint8_t)bj[2], (uint8_t)bj[3]};
                    const uint16_t q[3] = {(uint16_t)(wt[0] / s * 65535.f), (uint16_t)(wt[1] / s * 65535.f),
                                           (uint16_t)(wt[2] / s * 65535.f)};
                    std::memcpy(rec + 4, q, sizeof(q));
                    gi.push_back(decodeRecord(rec));
                }
            for (int y = 0; y + 1 < n; ++y)
                for (int x = 0; x + 1 < n; ++x)
                {
                    const uint32_t a = y * n + x, b = a + 1, c2 = a + n, d = c2 + 1;
                    gx.insert(gx.end(), {a, b, d, a, d, c2});
                }
            const uint32_t nv = (uint32_t)gi.size();
            std::vector<float> fast(gp.size()), slow(gp.size());
            Stats sg;
            ensure("grid precompute ran", computeCoR(gp.data(), gi.data(), nv, gx.data(), (uint32_t)gx.size(), gi.data(),
                                                     gp.data(), nv, fast.data(), sg));
            computeCoRReference(gp.data(), gi.data(), nv, gx.data(), (uint32_t)gx.size(), slow.data());
            float worst = 0.f;
            for (uint32_t v = 0; v < nv; ++v) worst = std::max(worst, maxDiff3(&fast[v * 3], &slow[v * 3]));
            ensure("4-bone grid matches the brute-force integral", worst < 0.02f);
        }

        // Work cap: nothing written, aborted.
        std::vector<float> none(c.pos.size(), -7.f);
        Stats sa;
        const bool ok = computeCoR(c.pos.data(), c.infl.data(), (uint32_t)c.infl.size(), c.idx.data(),
                                   (uint32_t)c.idx.size(), c.infl.data(), c.pos.data(), (uint32_t)c.infl.size(),
                                   none.data(), sa, 10);
        ensure("work cap aborts", !ok && sa.aborted && none[0] == -7.f);

        // Record decode: weights normalised, influence 3 implied, weld bit masked, joints clamped,
        // all-zero -> the first influence.
        const uint8_t rec[10] = {3, 120, 5, 0x80 | 9, 0x00, 0x80, 0x00, 0x40, 0x00, 0x00};
        const Influence in = decodeRecord(rec);
        ensure_equals("joint clamp", in.j[1], kMaxJoints - 1u);
        ensure_equals("weld bit masked", in.j[3], 9u);
        ensure("weights sum 1", std::fabs(in.w[0] + in.w[1] + in.w[2] + in.w[3] - 1.f) < 1e-6f);
        const uint8_t zero[10] = {4, 5, 6, 7, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x00};   // 65535 + 1: q3 = 0, sum > 0
        const Influence z = decodeRecord(zero);
        ensure("saturated weights normalised", std::fabs(z.w[0] + z.w[2] - 1.f) < 1e-6f && z.w[3] == 0.f);
    }

    // #1581 owner report: shirt buttons fell off the shirt. A 1 cm button 3 mm off a torso tube,
    // weights copied from the shirt under it. Centres integrated over the button ALONE (its own
    // centroid) drift it off the shirt by centimetres under a 90 degree twist; integrated over the
    // UNION of the meshes (LLCleanRigCoR's per-avatar domain) the button keeps its offset exactly.
    template<> template<>
    void cormath_object::test<6>()
    {
        // (the torso along x: makeCylinder's axis)
        Cylinder shirt = makeCylinder(0.5f, 0.02f, 32, 0.15f, 0.1f, 0.4f);
        const float bx = 0.26f;   // a ring (0.02 steps): the probe below is a shirt vertex
        const size_t ring = (size_t)std::lround(bx / 0.02f);
        const Influence bw = shirt.infl[ring * shirt.seg];
        Cylinder button;
        for (int k = 0; k < 4; ++k)
        {
            const float dx = (k & 1) ? 0.005f : -0.005f, dz = (k & 2) ? 0.005f : -0.005f;
            button.pos.insert(button.pos.end(), {bx + dx, 0.153f, dz});
            button.nrm.insert(button.nrm.end(), {0.f, 1.f, 0.f});
            button.infl.push_back(bw);
        }
        button.idx = {0, 1, 3, 0, 3, 2};
        Cylinder uni = shirt;   // shirt + button
        const uint32_t base = (uint32_t)shirt.infl.size();
        uni.pos.insert(uni.pos.end(), button.pos.begin(), button.pos.end());
        uni.nrm.insert(uni.nrm.end(), button.nrm.begin(), button.nrm.end());
        uni.infl.insert(uni.infl.end(), button.infl.begin(), button.infl.end());
        for (uint32_t i : button.idx) uni.idx.push_back(base + i);
        Stats st;
        std::vector<float> c_uni(uni.pos.size()), c_own(button.pos.size()), c_shirt(shirt.pos.size());
        computeCoR(uni.pos.data(), uni.infl.data(), (uint32_t)uni.infl.size(), uni.idx.data(), (uint32_t)uni.idx.size(),
                   uni.infl.data(), uni.pos.data(), (uint32_t)uni.infl.size(), c_uni.data(), st);
        computeCoR(button.pos.data(), button.infl.data(), 4, button.idx.data(), 6, button.infl.data(), button.pos.data(), 4,
                   c_own.data(), st);
        computeCoR(shirt.pos.data(), shirt.infl.data(), (uint32_t)shirt.infl.size(), shirt.idx.data(),
                   (uint32_t)shirt.idx.size(), shirt.infl.data(), shirt.pos.data(), (uint32_t)shirt.infl.size(),
                   c_shirt.data(), st);
        Bone bones[2] = {boneIdentity(), boneRotX(kPi / 2.f)};   // bone 1 twists 90 degrees about the torso axis
        Decomp dec[2] = {decompose(bones[0].a), decompose(bones[1].a)};
        const size_t probe = ring * shirt.seg;   // the shirt vertex at angle 0 under the button
        auto drift = [&](const float* cb, const float* cs)
        {
            float worst = 0.f;
            float sp[3], sn[3];
            skinCoR(bones, dec, shirt.infl[probe], cs, &shirt.pos[probe * 3], &shirt.nrm[probe * 3], sp, sn);
            for (int v = 0; v < 4; ++v)
            {
                float bp[3], bn[3];
                skinCoR(bones, dec, bw, cb + v * 3, &button.pos[v * 3], &button.nrm[v * 3], bp, bn);
                float d = 0.f, d0 = 0.f;
                for (int k = 0; k < 3; ++k)
                {
                    d += (bp[k] - sp[k]) * (bp[k] - sp[k]);
                    d0 += (button.pos[v * 3 + k] - shirt.pos[probe * 3 + k]) * (button.pos[v * 3 + k] - shirt.pos[probe * 3 + k]);
                }
                worst = std::max(worst, std::fabs(std::sqrt(d) - std::sqrt(d0)));
            }
            return worst;
        };
        const float per_mesh = drift(c_own.data(), &c_shirt[probe * 3]);
        const float unified = drift(&c_uni[base * 3], &c_uni[probe * 3]);
        ensure("per-mesh centres: the button leaves the shirt", per_mesh > 0.01f);
        ensure("union centres: the button stays on the shirt", unified < 1e-4f);
    }

    // The agreement safeguard: three bones rotating in strongly different directions -- the
    // blend eases to LBS continuously (finite, and no jump between nearby weights).
    template<> template<>
    void cormath_object::test<7>()
    {
        Bone bones[3] = {boneIdentity(), boneIdentity(), boneIdentity()};
        const float axes[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
        for (int b = 0; b < 3; ++b) rotation(axes[b], 2.9f, bones[b].a);
        Decomp dec[3] = {decompose(bones[0].a), decompose(bones[1].a), decompose(bones[2].a)};
        ensure("agreement: full above 0.6", agreement(0.7f) == 1.f && agreement(0.3f) == 0.f);
        const float v[3] = {0.1f, 0.2f, 0.3f}, n[3] = {0.f, 0.f, 1.f}, c[3] = {0.f, 0.f, 0.f};
        float prev[3] = {};
        float worst_jump = 0.f;
        for (int i = 0; i <= 100; ++i)
        {
            const float s = (float)i / 100.f;
            Influence in{};
            in.j[0] = 0; in.j[1] = 1; in.j[2] = 2;
            in.w[0] = (1.f - s) * 0.5f; in.w[1] = 0.25f + 0.25f * s; in.w[2] = 1.f - in.w[0] - in.w[1];
            float p[3], nn[3];
            skinCoR(bones, dec, in, c, v, n, p, nn);
            for (int k = 0; k < 3; ++k) ensure("finite", std::isfinite(p[k]));
            if (i) worst_jump = std::max(worst_jump, maxDiff3(p, prev));
            std::memcpy(prev, p, sizeof(prev));
        }
        ensure("continuous across the disagreement zone", worst_jump < 0.02f);
    }
}
