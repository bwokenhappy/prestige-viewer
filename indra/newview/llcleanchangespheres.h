/**
 * @file llcleanchangespheres.h
 * @brief GI change spheres (header only): the bounded lists of regions where a frame's GI history is
 *        WRONG (hard) or only STALE (soft), and the policy that keeps a list within its capacity.
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

#ifndef LL_LLCLEANCHANGESPHERES_H
#define LL_LLCLEANCHANGESPHERES_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// The GI header carries up to 64 change spheres (clean_gi_common.glsl gi.sph: xyz centre, w radius);
// the instance store's emitter deltas and the ingest's geometry changes feed them. Two kinds, by the
// sign of w:
//   HARD (w >= 0): geometry changed there -- the history inside may be WRONG (a surface gone, an
//   occluder moved): the reservoirs, the GI denoiser's history and the cache cells inside restart.
//   SOFT (w < 0, radius -w): an emitter's light changed -- the history is only STALE and adapts within a
//   few frames. Its radius is also its STRENGTH (giSoftCap: about (R / 2 d)^2 at d from the centre).
// A list holds more changes than slots by merging them, and a merge must not change what a sphere
// MEANS:
//   - the kinds never merge: a hard change inside a merged soft sphere made all of it hard -- a soft
//     sphere spanning the region (scattered moving emitters) turned into one hard sphere hundreds of
//     metres wide, and every pixel lost its GI history for that frame (one-frame-latency GI: a frame of
//     probe-only light, a flicker every second or so);
//   - a merge stays within kMergeGrowth of the largest change it holds (src): growing by chained
//     merges overstated a soft sphere's strength by its size squared and spread hard regions;
//   - past that, the least relevant SOFT sphere is let go (a soft region without its sphere adapts at
//     the normal pace): the one that matters least to the viewer at `focus`. A hard sphere is never
//     let go; only when every slot is hard does it grow the hard sphere that grows least.
namespace LLCleanChangeSpheres
{
    struct Sphere
    {
        float c[3];   // centre (agent frame)
        float w;      // radius; negative: a soft sphere of radius -w
        float src;    // the radius of the largest single change it holds (merges keep the larger)
    };

    inline Sphere make(const float centre[3], float w)
    {
        return Sphere{{centre[0], centre[1], centre[2]}, w, std::fabs(w)};
    }

    // Stable partition into a GPU header, without changing the CPU merge queue.
    // Caller provides n rows of output. Returns the prefix length; a radius of -0 is hard.
    inline uint32_t writeHardFirst(const Sphere* src, uint32_t n, float (*dst)[4])
    {
        uint32_t out = 0, hard = 0;
        for (uint32_t kind = 0; kind < 2u; ++kind)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                if ((src[i].w < 0.f) != (kind != 0u)) continue;
                for (uint32_t k = 0; k < 3u; ++k) dst[out][k] = src[i].c[k];
                dst[out][3] = src[i].w;
                ++out;
            }
            if (kind == 0u) hard = out;
        }
        return hard;
    }

    inline bool isSoft(const Sphere& s) { return s.w < 0.f; }
    inline float radius(const Sphere& s) { return std::fabs(s.w); }

    // The radius of the smallest sphere holding a and b.
    inline float enclosingRadius(const Sphere& a, const Sphere& b)
    {
        const float dx = b.c[0] - a.c[0], dy = b.c[1] - a.c[1], dz = b.c[2] - a.c[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float ra = radius(a), rb = radius(b);
        if (d + rb <= ra) return ra;
        if (d + ra <= rb) return rb;
        return 0.5f * (d + ra + rb);
    }

    // a grown to hold b (the same kind: a keeps its own).
    inline void enclose(Sphere& a, const Sphere& b)
    {
        const float dx = b.c[0] - a.c[0], dy = b.c[1] - a.c[1], dz = b.c[2] - a.c[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float ra = radius(a), rb = radius(b);
        const float sgn = isSoft(a) ? -1.f : 1.f;
        a.src = std::max(a.src, b.src);
        if (d + rb <= ra) return;
        if (d + ra <= rb) { std::copy_n(b.c, 3, a.c); a.w = sgn * rb; return; }
        const float r = 0.5f * (d + ra + rb);
        const float t = (r - ra) / std::max(d, 1e-6f);
        a.c[0] += dx * t; a.c[1] += dy * t; a.c[2] += dz * t;
        a.w = sgn * r;
    }

    // How much a sphere matters to a viewer at focus: 1 with the viewer inside, about r / d beyond.
    // No focus: its radius (the larger change).
    inline float relevance(const Sphere& s, const float* focus)
    {
        const float r = radius(s);
        if (!focus) return r;
        const float dx = s.c[0] - focus[0], dy = s.c[1] - focus[1], dz = s.c[2] - focus[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        return r / std::max(d, std::max(r, 1e-3f));
    }

    // Coalescing into the last sphere: a face's neighbours, a mover's old + new pose -- nothing that
    // grows the region (25 % of the radius chained a frame's run of events far past any of them).
    constexpr float kCoalesceSlack = 0.25f;
    // A merge's radius against the largest change it would hold: at most twice (a soft sphere's
    // strength overstated at most 4 x).
    constexpr float kMergeGrowth = 2.f;
    constexpr uint32_t kNone = 0xFFFFFFFFu;
    // "None yet" for the searches' growth / relevance: the largest finite float -- the build's fast-math
    // options make an infinity undefined (comparisons with it may be folded away).
    constexpr float kFar = std::numeric_limits<float>::max();

    enum class Outcome : uint8_t
    {
        Appended,    // a free slot
        Coalesced,   // into a sphere, which grew at most by the slack
        Merged,      // a merge grew a sphere past s's own radius (into s's host, or a pair to make room)
        Covered,     // soft, inside a hard sphere already: nothing to add
        Replaced,    // took the slot of the least relevant soft sphere (that change let go)
        Dropped,     // soft, itself the least relevant: let go
        Forced       // hard, every slot hard and none could take it within the bound: the least growth
    };

    // One sphere into the list v[0..n) of at most cap. pair_budget: the O(cap^2) closest-pair searches
    // left (each costs one); focus: the viewer (agent frame) or null. Returns the slot this call
    // filled or grew (kNone: Dropped).
    inline uint32_t add(Sphere* v, uint32_t& n, uint32_t cap, const Sphere& s, uint32_t& pair_budget,
                        const float* focus, Outcome& out)
    {
        const float rs = radius(s);
        const bool soft = isSoft(s);
        if (n != 0u)
        {
            Sphere& last = v[n - 1u];
            if (isSoft(last) == soft)
            {
                const float r = enclosingRadius(last, s);
                if (r <= std::max(radius(last), rs) + kCoalesceSlack)
                {
                    out = r > rs + kCoalesceSlack ? Outcome::Merged : Outcome::Coalesced;
                    enclose(last, s);
                    return n - 1u;
                }
            }
        }
        if (n < cap)
        {
            v[n] = s;
            out = Outcome::Appended;
            return n++;
        }
        // Full. The host that grows least within the bound (a soft s a hard sphere holds is covered).
        uint32_t host = kNone;
        float host_grow = kFar;
        for (uint32_t i = 0u; i < cap; ++i)
        {
            const float rh = radius(v[i]);
            const float r = enclosingRadius(v[i], s);
            if (isSoft(v[i]) != soft)
            {
                if (soft && r <= rh) { out = Outcome::Covered; return i; }
                continue;
            }
            if (r > kMergeGrowth * std::max(v[i].src, s.src)) continue;
            if (r - rh < host_grow) { host_grow = r - rh; host = i; }
        }
        // Or the closest pair within the bound, s then taking the freed slot: soft pairs, and for a hard
        // s hard pairs too (a soft s never grows a hard region to make room for itself).
        if (host_grow > 0.f && pair_budget != 0u)
        {
            --pair_budget;
            uint32_t pi = kNone, pj = kNone;
            float pair_grow = kFar;
            for (uint32_t i = 0u; i < cap; ++i)
            {
                if (!isSoft(v[i]) && soft) continue;
                for (uint32_t j = i + 1u; j < cap; ++j)
                {
                    if (isSoft(v[j]) != isSoft(v[i])) continue;
                    const float r = enclosingRadius(v[i], v[j]);
                    if (r > kMergeGrowth * std::max(v[i].src, v[j].src)) continue;
                    const float g = r - std::max(radius(v[i]), radius(v[j]));
                    if (g < pair_grow) { pair_grow = g; pi = i; pj = j; }
                }
            }
            if (pi != kNone && pair_grow < host_grow)
            {
                enclose(v[pi], v[pj]);
                v[pj] = s;
                out = pair_grow > 0.f ? Outcome::Merged : Outcome::Coalesced;
                return pi;
            }
        }
        if (host != kNone)
        {
            enclose(v[host], s);
            out = host_grow > kCoalesceSlack ? Outcome::Merged : Outcome::Coalesced;
            return host;
        }
        // No merge within the bound: the least relevant soft sphere goes (s itself when it is that one).
        uint32_t weakest = kNone;
        float weak = soft ? relevance(s, focus) : kFar;
        for (uint32_t i = 0u; i < cap; ++i)
        {
            if (!isSoft(v[i])) continue;
            const float r = relevance(v[i], focus);
            if (r < weak) { weak = r; weakest = i; }
        }
        if (weakest != kNone)
        {
            v[weakest] = s;
            out = Outcome::Replaced;
            return weakest;
        }
        if (soft)
        {
            out = Outcome::Dropped;
            return kNone;
        }
        // A hard s and every slot hard: it must stay covered -- the hard sphere that grows least.
        host = 0u;
        host_grow = kFar;
        for (uint32_t i = 0u; i < cap; ++i)
        {
            const float g = enclosingRadius(v[i], s) - radius(v[i]);
            if (g < host_grow) { host_grow = g; host = i; }
        }
        enclose(v[host], s);
        out = Outcome::Forced;
        return host;
    }
}

#endif // LL_LLCLEANCHANGESPHERES_H
