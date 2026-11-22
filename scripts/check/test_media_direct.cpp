// Host test of indra/newview/vkgfx/llvkmediadirect.h: the decisions of the direct stage for media on prims
// (which ring entry a frame writes, when an entry may be written again, which media get a ring, which texels
// are copied). Run by scripts/check/checks/media_direct.py.
#include "llvkmediadirect.h"

#include <cstdint>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace
{
using namespace llvkmediadirect;

// A ring as the prep service keeps it: an entry is registered while its frame records, gets the frame's submit
// serial, and is written again only after that serial's fence has passed.
struct Ring
{
    bool recordPending[kRingEntries] = {};
    uint64_t submitSerial[kRingEntries] = {};
    bool quarantined[kRingEntries] = {};
    void states(Entry* out) const
    {
        for (uint32_t e = 0; e < kRingEntries; ++e)
            out[e] = entryState(recordPending[e], submitSerial[e], quarantined[e]);
    }
    void retire(uint64_t completedSerial)
    {
        for (uint32_t e = 0; e < kRingEntries; ++e)
            if (submitSerial[e] && submitSerial[e] <= completedSerial) submitSerial[e] = 0;
    }
};

// Frames uploaded out of `frames` when every frame has new pixels and a frame's fence passes `inFlight`
// frames later (the frame slot comes round again).
uint32_t uploaded(uint32_t frames, uint32_t inFlight)
{
    Ring ring;
    uint32_t done = 0;
    for (uint32_t frame = 1; frame <= frames; ++frame)
    {
        if (frame > inFlight) ring.retire(frame - inFlight);   // acquire waited that slot's fence
        Entry state[kRingEntries];
        ring.states(state);
        const int pick = pickEntry(state, kRingEntries);
        if (pick < 0) continue;                                 // no free entry: this frame is skipped
        ring.recordPending[pick] = true;                        // registered with the recording
        ring.recordPending[pick] = false;                       // submit accepted
        ring.submitSerial[pick] = frame;
        ++done;
    }
    return done;
}
}

int main()
{
    // ---- ring entry selection, fence-gated reuse ----
    CHECK(entryState(false, 0, false) == Entry::Free);
    CHECK(entryState(true, 0, false) == Entry::InFlight);       // registered with this frame's recording
    CHECK(entryState(false, 7, false) == Entry::InFlight);      // submitted, fence not passed
    CHECK(entryState(true, 0, true) == Entry::Pinned);          // completion unknown: never written again
    CHECK(entryState(false, 0, true) == Entry::Pinned);
    {
        const Entry a[2] = {Entry::Free, Entry::Free};
        const Entry b[2] = {Entry::InFlight, Entry::Free};
        const Entry c[2] = {Entry::InFlight, Entry::InFlight};
        const Entry d[2] = {Entry::Pinned, Entry::Free};
        const Entry e[2] = {Entry::Pinned, Entry::InFlight};
        const Entry f[2] = {Entry::Pinned, Entry::Pinned};
        CHECK(pickEntry(a, 2) == 0);
        CHECK(pickEntry(b, 2) == 1);
        CHECK(pickEntry(c, 2) == -1);                           // skip the frame, never block
        CHECK(pickEntry(d, 2) == 1);
        CHECK(pickEntry(e, 2) == -1);
        CHECK(pickEntry(f, 2) == -1);
        CHECK(pickEntry(a, 0) == -1);
        CHECK(!ringDead(a, 2) && !ringDead(c, 2) && !ringDead(e, 2));
        CHECK(ringDead(f, 2));                                  // nothing will ever come back: use tickets
        CHECK(ringDead(a, 0));
    }
    // Two frames in flight (the renderer's count): the ring serves every frame.
    CHECK(kRingEntries == 2);
    CHECK(uploaded(600, 2) == 600);
    CHECK(uploaded(600, 1) == 600);
    // A third frame in flight costs every third frame, never a wait.
    CHECK(uploaded(600, 3) == 400);

    // ---- budget ----
    const uint64_t hd = frameBytes(1920, 1080);
    CHECK(hd == 1920ull * 1080ull * 4ull);
    CHECK(frameBytes(0, 1080) == 0 && frameBytes(1920, 0) == 0);
    CHECK(frameBytes(0xffffffffu, 0xffffffffu) == 0);           // overflow is "no ring", not a small one
    {
        uint64_t used = 0;
        uint32_t tvs = 0;
        while (leaseFits(used, hd)) { used += hd * kRingEntries; ++tvs; }
        CHECK(tvs == 4);                                        // four HD TVs, the fifth uses tickets
        CHECK(used <= kBudgetBytes);
    }
    CHECK(!leaseFits(0, 0));
    CHECK(!leaseFits(0, kBudgetBytes));                         // one entry would take the whole budget
    CHECK(leaseFits(0, kBudgetBytes / kRingEntries));
    CHECK(!leaseFits(1, kBudgetBytes / kRingEntries));
    CHECK(!leaseFits(kBudgetBytes + 1, 1));                     // an overdrawn ledger admits nothing
    CHECK(!leaseFits(0, UINT64_MAX));                           // no overflow

    // ---- direct or tickets ----
    {
        Target t;
        t.resident = t.sameSize = t.rgba8Image = t.rgba8Source = true;
        CHECK(route(t) == Route::Direct);
        Target u = t; u.resident = false;                       // first frame: a ticket creates the image
        CHECK(route(u) == Route::Tickets);
        u = t; u.sameSize = false;                              // resize: the ticket's replace path
        CHECK(route(u) == Route::Tickets);
        u = t; u.rgba8Image = false;
        CHECK(route(u) == Route::Tickets);
        u = t; u.rgba8Source = false;                           // bytes need a conversion: the worker does it
        CHECK(route(u) == Route::Tickets);
        u = t; u.ticketPending = true;                          // its older frame must not land after ours
        CHECK(route(u) == Route::Tickets);
        CHECK(route(Target{}) == Route::Tickets);
    }

    // ---- which texels ----
    {
        Region r = region(false, 0, 0, 0, 0, 1920, 1080);       // no rectangle known: whole frame
        CHECK(r.full && !r.empty && r.bytes == hd && r.x == 0 && r.y == 0 && r.w == 1920 && r.h == 1080);
        r = region(true, 100, 200, 300, 50, 1920, 1080);
        CHECK(!r.full && !r.empty && r.x == 100 && r.y == 200 && r.w == 300 && r.h == 50 && r.bytes == 300ull * 50 * 4);
        r = region(true, 0, 0, 1920, 1080, 1920, 1080);         // everything changed (video): whole frame
        CHECK(r.full && r.bytes == hd);
        r = region(true, 10, 10, 0, 40, 1920, 1080);            // nothing changed
        CHECK(r.empty && r.bytes == 0);
        r = region(true, 10, 10, 40, -3, 1920, 1080);
        CHECK(r.empty && r.bytes == 0);
        r = region(true, 1900, 1070, 100, 100, 1920, 1080);     // clamped to the image
        CHECK(!r.full && r.x == 1900 && r.y == 1070 && r.w == 20 && r.h == 10 && r.bytes == 20ull * 10 * 4);
        r = region(true, -5, -7, 25, 27, 1920, 1080);
        CHECK(!r.full && r.x == 0 && r.y == 0 && r.w == 20 && r.h == 20);
        r = region(true, 5000, 10, 10, 10, 1920, 1080);         // outside the image: not trusted, whole frame
        CHECK(r.full && r.bytes == hd);
        r = region(true, -50, 10, 20, 10, 1920, 1080);
        CHECK(r.full && r.bytes == hd);
        r = region(true, INT64_MAX - 1, 0, INT64_MAX, 10, 1920, 1080);
        CHECK(r.full && r.bytes == hd);
        r = region(true, 0, 0, 10, 10, 0, 1080);                // no image: nothing to copy
        CHECK(r.empty && r.bytes == 0);
        // every region stays inside the image and inside one ring entry
        for (int64_t x = -3; x <= 9; x += 3)
            for (int64_t w = 1; w <= 12; w += 5)
            {
                r = region(true, x, x, w, w, 8, 8);
                CHECK(r.x + r.w <= 8 && r.y + r.h <= 8 && r.bytes <= frameBytes(8, 8));
                CHECK(r.empty || r.bytes == (uint64_t)r.w * r.h * 4);
            }
    }

    // ---- lease timing ----
    CHECK(leaseDue(100, 0, false));                             // never refused: ask now
    CHECK(!leaseDue(100, 100, true));
    CHECK(!leaseDue(100 + kLeaseRetryFrames - 1, 100, true));   // refused: tickets for a while, no per-frame retry
    CHECK(leaseDue(100 + kLeaseRetryFrames, 100, true));
    CHECK(leaseDue(100, 0xfffffff0u, true));                    // the frame counter wrapped
    CHECK(!leaseDue(5, 0xfffffff0u, true));
    CHECK(!idle(100, 100));
    CHECK(!idle(100 + kIdleReturnFrames - 1, 100));             // paused briefly: keep the ring
    CHECK(idle(100 + kIdleReturnFrames, 100));                  // stopped: give the bytes back
    CHECK(idle(5, 0xfffff000u));

    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
