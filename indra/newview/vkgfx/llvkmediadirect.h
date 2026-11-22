/**
 * @file llvkmediadirect.h
 * @brief Decisions of the direct stage for media on prims (no Vulkan, no viewer types).
 *
 *        A live media texture (video, web page) has new pixels about every frame. Through the
 *        preparation tickets a frame costs a snapshot, a worker pass and a later frame's copy,
 *        and the ticket holds its media slot until its descriptor bank retires. The direct
 *        stage gives each live media a small ring of persistently mapped stage buffers, leased
 *        from the texture prep service: the main thread copies the changed texels into a free
 *        entry and the same frame records the copy into the bound image.
 *
 *        This header holds the rules, so they can be tested without a device:
 *          - which ring entry a frame may write, and when an entry may be written again (only
 *            after the fence of the frame that copied from it);
 *          - how many rings the byte budget admits;
 *          - which media frames go direct and which keep using the tickets;
 *          - which texels are copied (the dirty rectangle, clamped to the image).
 *        scripts/check/test_media_direct.cpp tests them; scripts/check/checks/media_direct.py
 *        runs that and checks the service and the renderer decide through this header.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLVKMEDIADIRECT_H
#define LL_LLVKMEDIADIRECT_H

#include <cstdint>

namespace llvkmediadirect
{
// Entries per media. An entry written in frame N is free again when N's frame slot is acquired
// next (its fence waited). With the renderer's two frames in flight that is frame N + 2, so two
// entries serve every frame; a third frame in flight would cost every third frame, not a wait.
constexpr uint32_t kRingEntries = 2;
// All rings together. An entry holds one RGBA8 frame: 1920 x 1080 is 7.9 MiB, its ring 15.8 MiB,
// so four HD media fit. Media beyond the budget keep using the tickets.
constexpr uint64_t kBudgetBytes = 64ull << 20;
// A refused lease (budget, allocation, no ticket slot) is asked again after this many frames.
constexpr uint32_t kLeaseRetryFrames = 64;
// A ring that uploaded nothing for this many frames is returned (paused or stopped media).
constexpr uint32_t kIdleReturnFrames = 600;

// ---- ring entries ----
enum class Entry : uint8_t
{
    Free,      // may be written and recorded this frame
    InFlight,  // a recorded or submitted frame copies from it: not before that frame's fence
    Pinned     // its submit's completion is unknown (device loss, failed proof): never again
};

// The service's view of an entry: registered with the frame being recorded, the serial of the
// accepted submit that copies from it (0: none, or its fence has passed), quarantined.
inline Entry entryState(bool recordPending, uint64_t submitSerial, bool quarantined)
{
    if (quarantined) return Entry::Pinned;
    return recordPending || submitSerial ? Entry::InFlight : Entry::Free;
}

// The entry this frame writes, or -1: every entry is busy, the frame is skipped for this media
// (the next frame copies the latest pixels). Never a wait.
inline int pickEntry(const Entry* ring, uint32_t count)
{
    for (uint32_t e = 0; e < count; ++e)
        if (ring[e] == Entry::Free) return (int)e;
    return -1;
}

// No entry will ever be free again: return the ring and use the tickets.
inline bool ringDead(const Entry* ring, uint32_t count)
{
    for (uint32_t e = 0; e < count; ++e)
        if (ring[e] != Entry::Pinned) return false;
    return true;
}

// ---- budget ----
// Bytes of one ring entry (one RGBA8 frame); 0: no such frame.
inline uint64_t frameBytes(uint32_t width, uint32_t height)
{
    if (!width || !height) return 0;
    const uint64_t texels = (uint64_t)width * height;
    return texels > kBudgetBytes / 4u ? 0 : texels * 4u;
}

// Whether a ring of `entries` buffers of entryBytes fits beside the usedBytes already leased.
inline bool leaseFits(uint64_t usedBytes, uint64_t entryBytes, uint32_t entries = kRingEntries,
                      uint64_t budget = kBudgetBytes)
{
    if (!entryBytes || !entries || usedBytes > budget) return false;
    return entryBytes <= (budget - usedBytes) / entries;
}

// ---- direct or tickets ----
enum class Route : uint8_t { Direct, Tickets };

struct Target
{
    bool resident = false;      // the media slot owns an image (the first frame's ticket creates it)
    bool sameSize = false;      // the new frame has the image's size (a resize replaces the image)
    bool rgba8Image = false;    // the image takes RGBA8 bytes as they are
    bool rgba8Source = false;   // the CPU frame is 4-channel RGBA bytes (no conversion pass)
    bool ticketPending = false; // a ticket of this slot has not reached an accepted submit yet:
                                // its older frame must not land after a direct one
};

inline Route route(const Target& t)
{
    return t.resident && t.sameSize && t.rgba8Image && t.rgba8Source && !t.ticketPending
        ? Route::Direct : Route::Tickets;
}

// ---- which texels ----
struct Region
{
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool full = true;    // the whole frame (the image's previous content is not needed)
    bool empty = false;  // nothing to copy
    uint64_t bytes = 0;  // tightly packed RGBA8 rows of the region
};

// The texels to copy for a width x height image. haveRect: (x, y, w, h) is the union of the
// changes since the frame the image shows; without it the whole frame is copied. The rectangle
// is clamped to the image; one that starts outside it is not trusted (whole frame).
inline Region region(bool haveRect, int64_t x, int64_t y, int64_t w, int64_t h,
                     uint32_t width, uint32_t height)
{
    Region r;
    const uint64_t all = frameBytes(width, height);
    if (!all) { r.full = false; r.empty = true; return r; }
    r.w = width; r.h = height; r.bytes = all;
    if (!haveRect) return r;
    if (w <= 0 || h <= 0) { r = Region{}; r.full = false; r.empty = true; return r; }
    if (x >= (int64_t)width || y >= (int64_t)height || x <= -w || y <= -h) return r;
    const int64_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int64_t x1 = w > (int64_t)width - x ? (int64_t)width : x + w;
    const int64_t y1 = h > (int64_t)height - y ? (int64_t)height : y + h;
    if (x0 == 0 && y0 == 0 && x1 == (int64_t)width && y1 == (int64_t)height) return r;
    r.full = false;
    r.x = (uint32_t)x0; r.y = (uint32_t)y0;
    r.w = (uint32_t)(x1 - x0); r.h = (uint32_t)(y1 - y0);
    r.bytes = (uint64_t)r.w * r.h * 4u;
    return r;
}

// ---- lease timing (frame counters wrap) ----
inline bool leaseDue(uint32_t frame, uint32_t refusedFrame, bool refused)
{
    return !refused || frame - refusedFrame >= kLeaseRetryFrames;
}

inline bool idle(uint32_t frame, uint32_t lastUploadFrame)
{
    return frame - lastUploadFrame >= kIdleReturnFrames;
}
}

#endif // LL_LLVKMEDIADIRECT_H
