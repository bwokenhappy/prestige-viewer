// Fixture for indra/newview/llcleanparttexhandle.h (scripts/check/checks/part_tex_handle.py).
//
// A model of the bindless table: slot -> (committed image?, texture key). An emitter's handle
// must never flash the white slot 0 or a slot without a committed image (both read as white,
// and the sim copies the handle onto every live particle of the system) -- but nothing that
// drew before may vanish for good: a null texture keeps the white default (LL's default image),
// and a slot that never gets its image is handed out after kPartTexStuckFrames.
#include "llcleanparttexhandle.h"

#include <cstdio>
#include <vector>

namespace
{
struct Slot { bool image; uint64_t key; };
std::vector<Slot> g_tab;

bool ready(uint32_t idx, uint64_t& key)
{
    if (idx >= g_tab.size() || !g_tab[idx].image) return false;
    key = g_tab[idx].key;
    return true;
}

// Reads as white on the GPU: the white slot, or any slot with no committed image.
bool white(uint32_t h)
{
    if (h == kPartTexNone) return false;
    return h == kPartTexWhite || h >= g_tab.size() || !g_tab[h].image;
}

int g_fail = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}
}

int main()
{
    const uint8_t idA[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const uint8_t idB[16] = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    const uint64_t A = cleanPartTexId(idA), B = cleanPartTexId(idB);
    expect(A != 0 && B != 0 && A != B, "texture id fold distinguishes ids and is never 0");

    // slot 0 white (built-in, key 0), 5 = texture A resident, 7 = fresh slot for A (no image yet)
    g_tab = {{true, 0}, {false, 0}, {false, 0}, {false, 0}, {false, 0}, {true, 0xA1}, {false, 0}, {false, 0xA2}};

    CleanPartTexLast last;
    PartTexWhy why;
    // 1. resolved + committed -> that slot, remembered
    uint32_t h = cleanPartTexHandle(5u, A, last, ready, &why);
    expect(h == 5u && last.idx == 5u && why == PartTexWhy::Resolved, "a committed slot is handed out and remembered");

    // 2. null texture -> the white default (LL draws it with its default image): must keep drawing
    CleanPartTexLast none;
    h = cleanPartTexHandle(kPartTexNone, 0, none, ready, &why);
    expect(h == kPartTexWhite && why == PartTexWhy::NoTexture, "a null-texture emitter keeps drawing with the white default");

    // 3. the map lost the entry for a pass -> the last good slot
    h = cleanPartTexHandle(kPartTexNone, A, last, ready, &why);
    expect(h == 5u && why == PartTexWhy::Held, "an unresolved pass keeps the last good slot of the same texture");

    // 4. resolved to a fresh slot whose upload has not committed -> last good, not white
    h = cleanPartTexHandle(7u, A, last, ready, &why);
    expect(!white(h) && h == 5u, "a slot without a committed image is never flashed (keeps the last good one)");

    // 5. resolved to the white slot 0 -> never handed out
    h = cleanPartTexHandle(kPartTexWhite, A, last, ready);
    expect(!white(h), "the white slot 0 is never handed out for a textured emitter");

    // 6. the upload commits -> the new slot, remembered
    g_tab[7].image = true;
    h = cleanPartTexHandle(7u, A, last, ready);
    expect(h == 7u && last.idx == 7u, "the committed new slot is handed out");

    // 7. the source switched to texture B (not resolved): A's slot is not B -> not drawn
    h = cleanPartTexHandle(kPartTexNone, B, last, ready);
    expect(h == kPartTexNone, "another texture's last good slot is never borrowed");

    // 8. the last good slot was recycled for something else / released -> not borrowed, not flashed
    CleanPartTexLast l2;
    cleanPartTexHandle(5u, A, l2, ready);
    g_tab[5] = {true, 0xB0};
    h = cleanPartTexHandle(kPartTexNone, A, l2, ready);
    expect(h == kPartTexNone, "a recycled last good slot is not used");
    g_tab[5] = {false, 0};
    h = cleanPartTexHandle(5u, A, l2, ready, &why);
    expect(!white(h) && why == PartTexWhy::Waiting, "a released slot is not flashed while it waits");

    // 9. A FRESH texture whose slot never gets an image (no last good): hidden only for the short
    //    wait, then handed out as before -- a system that drew before must not vanish for good.
    g_tab = {{true, 0}, {false, 0}, {false, 0}, {false, 0}, {false, 0}, {false, 0}, {false, 0}, {false, 0xA2}};
    CleanPartTexLast l3;
    uint32_t hidden = 0, f = 0;
    for (; f < 10u * kPartTexStuckFrames; ++f)
    {
        h = cleanPartTexHandle(7u, A, l3, ready, &why);
        if (h == kPartTexNone) ++hidden;
        else break;
    }
    expect(hidden <= kPartTexStuckFrames && h == 7u && why == PartTexWhy::Stuck,
           "a slot that never commits is handed out after kPartTexStuckFrames (not hidden for good)");
    // ... and once its image lands it is simply resolved
    g_tab[7].image = true;
    h = cleanPartTexHandle(7u, A, l3, ready, &why);
    expect(h == 7u && why == PartTexWhy::Resolved && l3.wait == 0, "a stuck slot that commits is resolved");

    // 10. a transient (a few frames) on a texture with a last good slot never shows white or hides
    g_tab = {{true, 0}, {false, 0}, {false, 0}, {false, 0}, {false, 0}, {true, 0xA1}, {false, 0}, {false, 0xA2}};
    CleanPartTexLast l4;
    cleanPartTexHandle(5u, A, l4, ready);
    for (uint32_t k = 0; k < 3u * kPartTexStuckFrames; ++k)
    {
        h = cleanPartTexHandle(7u, A, l4, ready);
        if (h != 5u) { expect(false, "a waiting re-resolve keeps the last good slot for as long as it waits"); break; }
    }

    // Exhaustive: no input state flashes white within the wait window.
    for (uint32_t res : {0u, 1u, 5u, 7u, 9u, kPartTexNone})
        for (uint32_t li : {0u, 1u, 5u, 7u, 9u, kPartTexNone})
            for (int img5 = 0; img5 < 2; ++img5)
                for (int img7 = 0; img7 < 2; ++img7)
                    for (uint64_t tex : {A, B})
                        for (uint32_t w : {0u, kPartTexStuckFrames / 2u})
                        {
                            g_tab = {{true, 0}, {false, 0}, {false, 0}, {false, 0}, {false, 0},
                                     {img5 != 0, 0xA1}, {false, 0}, {img7 != 0, 0xA2}};
                            CleanPartTexLast l;
                            l.idx = li; l.slot_key = li == 5u ? 0xA1 : li == 7u ? 0xA2 : 0; l.tex = A;
                            l.wait_tex = tex; l.wait = w;
                            const uint32_t hh = cleanPartTexHandle(res, tex, l, ready);
                            if (white(hh))
                            {
                                std::printf("FAIL: white handle %u (resolved %u, last %u, img5 %d, img7 %d, wait %u)\n",
                                            hh, res, li, img5, img7, w);
                                ++g_fail;
                            }
                        }

    if (g_fail) return 1;
    std::printf("ok\n");
    return 0;
}
