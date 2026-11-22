/**
 * @file llcleanchangespheres_test.cpp
 * @brief GI change-sphere lists (llcleanchangespheres.h): hard and soft changes never merge into each
 *        other, merges stay within their bound, a full list lets the least relevant soft change go and
 *        never a hard one.
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

#include "../llcleanchangespheres.h"

#include <array>
#include <cmath>
#include <string>

#include "../test/lltut.h"

namespace
{
    using namespace LLCleanChangeSpheres;

    constexpr uint32_t kCap = 64u;

    Sphere soft(float x, float y, float z, float r) { const float c[3] = {x, y, z}; return make(c, -r); }
    Sphere hard(float x, float y, float z, float r) { const float c[3] = {x, y, z}; return make(c, r); }

    bool holds(const Sphere& a, const Sphere& b)   // a contains b (a small tolerance)
    {
        return enclosingRadius(a, b) <= radius(a) + 1e-3f;
    }

    struct List
    {
        std::array<Sphere, kCap> v{};
        uint32_t n = 0u;
        uint32_t budget = 16u;
        uint32_t add(const Sphere& s, Outcome& how, const float* focus = nullptr)
        {
            return LLCleanChangeSpheres::add(v.data(), n, kCap, s, budget, focus, how);
        }
        bool covered(const Sphere& s, bool want_soft) const   // some sphere of that kind holds s
        {
            for (uint32_t i = 0; i < n; ++i)
                if (isSoft(v[i]) == want_soft && holds(v[i], s)) return true;
            return false;
        }
        float largest(bool want_soft) const
        {
            float r = 0.f;
            for (uint32_t i = 0; i < n; ++i)
                if (isSoft(v[i]) == want_soft) r = std::max(r, radius(v[i]));
            return r;
        }
        bool withinBound() const   // every sphere within kMergeGrowth of the largest change it holds
        {
            for (uint32_t i = 0; i < n; ++i)
                if (radius(v[i]) > kMergeGrowth * v[i].src + 1e-3f) return false;
            return true;
        }
    };

    // 64 scattered soft changes (a 13 m emitter sphere every 100 m): the list full, no two within the bound
    void fillScatteredSoft(List& l)
    {
        Outcome how;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                l.add(soft(100.f * i, 100.f * j, 0.f, 13.f), how);
    }
}

namespace tut
{
    struct changespheres_data {};
    typedef test_group<changespheres_data> changespheres_test;
    typedef changespheres_test::object changespheres_object;
    tut::changespheres_test changespheres("LLCleanChangeSpheres");

    // THE FLICKER: a small hard change inside a huge soft sphere that is the list's last entry. The old
    // coalescing grew the soft sphere around it and made all of it hard.
    template<> template<>
    void changespheres_object::test<1>()
    {
        List l;
        Outcome how;
        l.add(soft(0.f, 0.f, 0.f, 900.f), how);
        l.add(hard(10.f, 0.f, 0.f, 0.1f), how);
        ensure_equals("two spheres", l.n, 2u);
        ensure("the soft sphere stays soft", isSoft(l.v[0]) && radius(l.v[0]) == 900.f);
        ensure("the hard change keeps its own size", !isSoft(l.v[1]) && radius(l.v[1]) < 0.11f);
        // and the other order: a soft change right beside a hard one stays soft
        List m;
        m.add(hard(0.f, 0.f, 0.f, 5.f), how);
        m.add(soft(0.1f, 0.f, 0.f, 5.f), how);
        ensure_equals("kinds kept apart", m.n, 2u);
        ensure("soft beside hard stays soft", isSoft(m.v[1]));
        ensure_equals("the hard sphere not grown", radius(m.v[0]), 5.f);
    }

    // Full of soft changes: a hard change takes a slot (the least relevant soft change goes) and is
    // never merged into a soft sphere -- the hard regions stay as small as the changes.
    template<> template<>
    void changespheres_object::test<2>()
    {
        List l;
        fillScatteredSoft(l);
        ensure_equals("full", l.n, kCap);
        Outcome how;
        const Sphere h = hard(350.f, 350.f, 0.f, 2.f);   // inside no soft sphere, between four of them
        const float focus[3] = {0.f, 0.f, 0.f};
        l.add(h, how, focus);
        ensure("a hard change takes a soft one's slot", how == Outcome::Replaced);
        ensure("the hard change is covered by a hard sphere", l.covered(h, false));
        ensure("hard regions as small as the change", l.largest(false) <= 2.f + 1e-3f);
        ensure("the farthest soft change went", !l.covered(soft(700.f, 700.f, 0.f, 13.f), true));
        ensure("the nearest soft change stays", l.covered(soft(0.f, 0.f, 0.f, 13.f), true));
    }

    // Scattered soft changes past the capacity: no sphere grows past the bound (the old merge grew one
    // sphere across all of them), and with a viewer the changes near it are the ones kept.
    template<> template<>
    void changespheres_object::test<3>()
    {
        List l;
        const float focus[3] = {0.f, 0.f, 0.f};
        Outcome how;
        uint32_t dropped = 0;
        for (int i = 0; i < 20; ++i)
            for (int j = 0; j < 20; ++j)
            {
                l.add(soft(50.f * i, 50.f * j, 0.f, 13.f), how, focus);
                if (how == Outcome::Dropped || how == Outcome::Replaced) ++dropped;
            }
        ensure_equals("full", l.n, kCap);
        ensure("every sphere within the bound", l.withinBound());
        ensure("no sphere grows across the field", l.largest(true) <= kMergeGrowth * 13.f + 1e-3f);
        ensure("changes past the capacity let go", dropped > 0u);
        ensure("the change at the viewer kept", l.covered(soft(0.f, 0.f, 0.f, 13.f), true));
        ensure("its neighbour kept", l.covered(soft(50.f, 0.f, 0.f, 13.f), true));
        ensure("the farthest change let go", !l.covered(soft(950.f, 950.f, 0.f, 13.f), true));
    }

    // Chains: soft changes one beside the next (a moving emitter's trail) merge, but a merged sphere
    // never grows past the bound of the largest change it holds.
    template<> template<>
    void changespheres_object::test<4>()
    {
        List l;
        fillScatteredSoft(l);
        Outcome how;
        for (int k = 0; k < 200; ++k)
            l.add(soft(5.f + 0.5f * k, 3.f, 0.f, 13.f), how);
        ensure("every sphere within the bound", l.withinBound());
        ensure("no chain past the bound", l.largest(true) <= kMergeGrowth * 13.f + 1e-3f);
        ensure_equals("still full, never past it", l.n, kCap);
    }

    // A soft change inside a hard sphere of a full list adds nothing; a hard change is never let go:
    // with every slot hard it grows the hard sphere that grows least (covered, the old rule).
    template<> template<>
    void changespheres_object::test<5>()
    {
        List l;
        Outcome how;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                l.add(hard(100.f * i, 100.f * j, 0.f, 13.f), how);
        ensure_equals("full of hard spheres", l.n, kCap);
        const std::array<Sphere, kCap> before = l.v;
        l.add(soft(101.f, 0.f, 0.f, 3.f), how);
        ensure("a soft change a hard sphere holds is covered", how == Outcome::Covered);
        bool same = true;
        for (uint32_t i = 0; i < kCap; ++i)
            same = same && before[i].w == l.v[i].w && before[i].c[0] == l.v[i].c[0];
        ensure("nothing changed", same);
        l.add(soft(5000.f, 0.f, 0.f, 3.f), how);
        ensure("a soft change with no room and no hard sphere around it goes", how == Outcome::Dropped);
        const Sphere far = hard(5000.f, 0.f, 0.f, 1.f);
        l.add(far, how);
        ensure("a hard change is never let go", how != Outcome::Dropped && how != Outcome::Replaced);
        ensure("it stays covered by a hard sphere", l.covered(far, false));
        uint32_t soft_n = 0;
        for (uint32_t i = 0; i < l.n; ++i) soft_n += isSoft(l.v[i]) ? 1u : 0u;
        ensure_equals("no soft sphere appeared", soft_n, 0u);
    }

    // Coalescing: a change that grows the last sphere by at most the slack joins it (same kind only);
    // merges keep the larger source radius.
    template<> template<>
    void changespheres_object::test<6>()
    {
        List l;
        Outcome how;
        l.add(soft(0.f, 0.f, 0.f, 10.f), how);
        l.add(soft(0.2f, 0.f, 0.f, 10.f), how);
        ensure_equals("coalesced", l.n, 1u);
        ensure("still soft", isSoft(l.v[0]));
        ensure("grew by at most the slack", radius(l.v[0]) <= 10.f + kCoalesceSlack);
        l.add(soft(0.f, 0.f, 0.f, 2.f), how);
        ensure_equals("a change inside joins it", l.n, 1u);
        ensure_equals("the source radius is the larger", l.v[0].src, 10.f);
    }
}
