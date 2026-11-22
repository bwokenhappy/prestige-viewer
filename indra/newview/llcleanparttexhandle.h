/**
 * @file llcleanparttexhandle.h
 * @brief Which bindless texture a GPU particle emitter hands its particles this frame.
 *
 * LLCleanRendererVK::prepareParticles writes each emitter's texture index into the emitter table;
 * clean_particle_sim.comp copies it onto every particle the emitter owns, every frame. So the
 * handle of ONE frame is what EVERY live particle of the system samples on that frame.
 *
 * Bindless slot 0 is the built-in 1x1 white, and a slot whose image has not been committed yet
 * (a fresh slot waiting for its upload ticket; a released one) is written as that white by
 * writeTextureDescriptors. Handing such a slot out for a few frames turned a whole loaded system
 * into a stack of white quads (the tint of a flame is white; only the texture's alpha shapes it).
 *
 * THE RULE:
 *   0. An emitter with no texture (null id) gets the white slot, as before: LL draws a particle
 *      whose image is missing with its default (white) image -- such systems must keep drawing.
 *   1. The resolved slot, when it has a committed image (it becomes the emitter slot's LAST GOOD
 *      handle).
 *   2. Else the last good handle, when it was for this same texture and that bindless slot still
 *      holds it with a committed image (a transient: a re-resolve, an upload replacing a slot).
 *   3. Else, while the resolved slot has waited fewer than kPartTexStuckFrames frames for its
 *      image: kPartTexNone -- not drawn for that short wait (never a white flash).
 *   4. A resolved slot that waits longer is handed out as it is (whatever its descriptor shows):
 *      a texture whose upload never lands must not make a system vanish that drew before; the
 *      renderer logs it (VKParticleTex "stuck") so the stuck upload itself gets fixed.
 *   5. No resolved slot at all: kPartTexNone.
 *
 * Header-only and LL-free: shared by vkgfx/llcleanrenderervk.cpp and the CPU fixture
 * scripts/check/test_part_tex_handle.cpp (scripts/check/checks/part_tex_handle.py).
 */
#ifndef LL_LLCLEANPARTTEXHANDLE_H
#define LL_LLCLEANPARTTEXHANDLE_H

#include <cstdint>

// "No texture": the particle FS discards it (clean_particle.frag), the sim keeps the particle's last one.
constexpr uint32_t kPartTexNone = 0xFFFFFFFFu;
// Bindless slot 0: the built-in 1x1 white (LL's default image for a particle with no texture).
constexpr uint32_t kPartTexWhite = 0u;
// Frames a resolved slot may wait for its first committed image before it is handed out anyway
// (an upload lands in a few frames; ~1 s at 60 fps is no longer a transient).
constexpr uint32_t kPartTexStuckFrames = 60u;

// An emitter slot's last good handle: the bindless slot, what that slot held then (its texture
// key), which texture the emitter wanted (a 64-bit fold of the texture id), and how many frames
// the current texture's resolved slot has waited for a committed image.
struct CleanPartTexLast
{
    uint32_t idx = kPartTexNone;
    uint64_t slot_key = 0;
    uint64_t tex = 0;
    uint64_t wait_tex = 0;
    uint32_t wait = 0;
};

// Why an emitter got its handle (the renderer's VKParticleTex counters / log).
enum class PartTexWhy : uint8_t
{
    Resolved,    // 1: its own committed slot
    NoTexture,   // 0: null texture -> the white default
    Held,        // 2: the last good slot of the same texture
    Waiting,     // 3: the resolved slot has no committed image yet -> not drawn
    Stuck,       // 4: the resolved slot waited past kPartTexStuckFrames -> handed out as is
    Unresolved,  // 5: no slot -> not drawn
};

// 64-bit fold of a 16-byte texture id (identity only: which texture a last good handle was for).
// Never 0 (0 means "no texture").
inline uint64_t cleanPartTexId(const uint8_t id[16])
{
    uint64_t a = 0, b = 0;
    for (int i = 0; i < 8; ++i) { a = (a << 8) | id[i]; b = (b << 8) | id[8 + i]; }
    const uint64_t r = a ^ (b * 0x9E3779B97F4A7C15ull);
    return r ? r : 1ull;
}

// One emitter's handle this frame (THE RULE above).
//   resolved: the texture map's slot for the emitter's texture (kPartTexNone: none this pass)
//   tex:      cleanPartTexId of the emitter's texture (0: null texture)
//   last:     the emitter slot's state (updated)
//   ready(idx, key_out): true when bindless slot idx has a committed image; key_out = its texture
//             key (what it holds). key_out == 0 means the slot holds nothing.
template <class Ready>
inline uint32_t cleanPartTexHandle(uint32_t resolved, uint64_t tex, CleanPartTexLast& last, Ready ready,
                                   PartTexWhy* why = nullptr)
{
    auto out = [&](uint32_t h, PartTexWhy w) { if (why) *why = w; return h; };
    if (tex == 0) return out(kPartTexWhite, PartTexWhy::NoTexture);
    if (last.wait_tex != tex) { last.wait_tex = tex; last.wait = 0; }
    uint64_t key = 0;
    const bool have_slot = resolved != kPartTexNone && resolved != kPartTexWhite;
    if (have_slot && ready(resolved, key) && key != 0)
    {
        last.idx = resolved; last.slot_key = key; last.tex = tex; last.wait = 0;
        return out(resolved, PartTexWhy::Resolved);
    }
    if (have_slot && last.wait < 0xFFFFFFFFu) ++last.wait;
    if (last.idx != kPartTexNone && last.tex == tex)
    {
        uint64_t held = 0;
        if (ready(last.idx, held) && held != 0 && held == last.slot_key) return out(last.idx, PartTexWhy::Held);
    }
    if (!have_slot) return out(kPartTexNone, PartTexWhy::Unresolved);
    if (last.wait <= kPartTexStuckFrames) return out(kPartTexNone, PartTexWhy::Waiting);
    return out(resolved, PartTexWhy::Stuck);
}

#endif // LL_LLCLEANPARTTEXHANDLE_H
