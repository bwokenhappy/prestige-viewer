// Snapshot system-memory headroom: a snapshot is not refused just because free RAM is below the
// memory governor's reserve. Standalone (no viewer build):
//   g++ -std=c++17 -I../vkgfx snapshot_host_headroom_checks.cpp -o /tmp/shh && /tmp/shh   (in indra/newview/tests)
#include "llsnapshotbufferlimits.h"
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++fails; }
}

int main()
{
    using LLSnapshotBufferLimits::hostHeadroomBytes;
    const uint64_t MB = 1ull << 20;

    // The owner's log: avail 3444 MB, reserve 3828 MB, commit-avail 8495 MB. A 41 MB live
    // snapshot was refused with "0 MB above the reserve".
    const uint64_t room = hostHeadroomBytes(3444 * MB, 3828 * MB, true, 8495 * MB);
    check(room >= 41 * MB, "below the reserve, a 41 MB snapshot still fits");
    check(room == 1722 * MB, "below the reserve: half of what is free");

    // Comfortable: everything above the reserve, as before.
    check(hostHeadroomBytes(20000 * MB, 3828 * MB, false, 0) == 16172 * MB, "above the reserve: unchanged");
    check(hostHeadroomBytes(20000 * MB, 3828 * MB, true, 64000 * MB) == 16172 * MB, "plenty of commit: unchanged");

    // Windows commit: never more than half the commit still available.
    check(hostHeadroomBytes(20000 * MB, 3828 * MB, true, 1000 * MB) == 500 * MB, "commit nearly exhausted caps it");
    check(hostHeadroomBytes(3444 * MB, 3828 * MB, true, 0) == 0, "no commit left: nothing");

    // Nothing free: nothing.
    check(hostHeadroomBytes(0, 3828 * MB, false, 0) == 0, "no free memory: nothing");

    if (fails) return 1;
    std::printf("snapshot_host_headroom_checks: ok\n");
    return 0;
}
