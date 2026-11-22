// vkclean -- MOVER SHADOW ROUTING (RenderVKShadowMoverMask, 2026-11-09): the layout of the MOVER BUFFER
// and the geometric tests on it. One GPU-only uint buffer (LLCleanRTVK mMoverBuf; C++ twin of every
// constant below: llcleanrtvk.h kMv*):
//   * written by the TLAS instance pass (clean_rt_instances.comp, compute set binding 29): the per-instance
//     motion hold, the instance re-class into the mover classes (clean_rt_common.glsl RT_MASK_MOVER) and
//     this frame's MOVER BOUNDS -- one sphere per avatar (joint-table slot: its skeleton's extent, its
//     rigged groups and worn attachments merged in) and one per moving object;
//   * read by the RT shadow mask's trace (clean_rt_shadow_mask.comp, mask set 3 binding 23), which only
//     traces mover rays toward a light where a mover sphere can lie between the receiver and the light
//     (the MOVER CULL there), and counts its work into the header for the host's log line.
// THE ROUTING: under the stochastic lighting path the stochastic shadow rays leave every mover out (pass
// 15: clean_deferred_light.comp PDN_OPT_MOVER), so the denoiser histories hold STATIC occlusion only; the
// mask traces mover-ONLY rays per frame (its own short-history denoiser: the moving-caster path) and the
// lighting multiplies the two (clean_shadow_mask_read.glsl MOVER VISIBILITY).
#ifndef CLEAN_RT_MOVERS_GLSL
#define CLEAN_RT_MOVERS_GLSL
// header words (zeroed by the host every frame before the instance pass, after copying the last frame's
// header to its readback):
//   [0] avatars listed  [1] object spheres  [2] overflow bits (1 avatars, 2 objects, 4 a rigged mover without
//       an avatar slot -- no bound): any bit = the mask's cull keeps every mover everywhere
//   [3] instances re-classed into the mover classes this frame (moving statics)
//   [4] mask subgroups with a receiver  [5] ... of them with a mover candidate (they trace mover rays)
//   [6] mask texels that traced a mover ray  [7] mover rays traced (sun + lights; static weight rays too)
//   [8] mask texels whose light cluster overflowed CLUSTER_CAP (their mover lights walk every light)
//   [9] mover-blocked lights there that the capped cluster list did not hold (avatar shadows recovered)
//   THE COST BREAKDOWN (2026-11-13; counted only on a STATS FRAME -- frame info moverBits bit 4, one frame per
//   VKMoverShadow line -- so the per-light atomics cost nothing on the other frames): [10] 1 = this frame counted
//   words 11-23 and the per-light block MV_LST; [11] texels that walked the local-light passes; sums over them of
//   [12] lights in range (eligible: pass 15's set) [13] ... listed (reaching with light, not turned away) [14] ... with a
//   candidate mover on the segment by the SPHERE alone (the cull before the boxes) [15] ... by sphere AND box (the
//   lights that take mover rays) [16] REST lights whose static ray the contribution cut skipped [17] REST lights whose
//   static ray the walled rule skipped (every blocked light statically dark: the ratio is exactly 1) [18] mover rays
//   [19] static rays [20] mover traces that came back blocked (Vm < 1) [21] texels that ran pass 3 [22] texels of the
//   pixel fold [23] texels whose cluster overflowed (walked the whole light table)
//   (RenderVKShadowMoverMask 2, every frame -- pass 15's SELECT, clean_deferred_light.comp pdnMoverExactPlan, through RT
//   set binding 17) [24] texels MOVER-EXACT by a light segment [25] ... held by the mover countdown alone [26] exact jobs
//   (one all-caster ray each) [27] texels that wanted the exact list but found it full (stochastic that frame)
//   [28] SELECT's cull subgroups with a receiver [29] ... with a candidate mover (mvCullBuild, MV_CULL_STAT 28)
//   (2026-11-14, WHICH MOVER: pass 15's SELECT attributes every texel that went exact by a light segment to the FIRST
//   mover its first hit segment met; per-mover counts in MV_HIT) [30] the mover with the most such texels: its count
//   << 11 | its code (an avatar: its joint-table slot, < 1024; a moving object: 1024 + its index in the frame's object
//   list) -- atomicMax over growing counts, so the final word names the top one [31] bits 0-23 such texels whose
//   mover was an OBJECT (the rest: avatars), bits 24-27 the priority threshold the overflow rule admitted up to
//   (MV_X_BINS - 1 = everything), bit 28 the list still overflowed past that threshold (a demand jump in one frame)
#define MV_HDR        32u
#define MV_AV_CAP     1024u   // = AV_SLOT_CAP (clean_av_slots.glsl): one entry per joint-table slot at most
#define MV_OBJ_CAP    1024u   // moving objects per frame (past it: overflow -- every mask texel traces)
// PER-LIGHT STATS (stats frames; light index li < MV_LST_N, MV_LST_W words each): [0] mover rays [1] static rays
// [2] mover traces blocked [3] texels where it had a candidate mover [4] info bits (1 written, 2 projector, 4 a
// ray-traced projector record, 8 its position lies inside an avatar's bounds: carried / worn) [5] range (m, float)
// [6..8] position (float) [9] disc radius (m, float) [10] static rays skipped (contribution cut + walled rule)
// [11] static-weight traces (texels) [12] ... of them that saw static light (Vs > 0: the light reaches past the walls)
// [13] its split id (Light.tex.w: the host maps it back to the light's object)
#define MV_LST_N      256u
#define MV_LST_W      14u
#define MV_LST        MV_HDR                          // [MV_LST_N * MV_LST_W] per-light stats
#define MV_AV_LIST    (MV_LST + MV_LST_N * MV_LST_W)  // [MV_AV_CAP]     listed avatar slots
#define MV_AV_MARK    (MV_AV_LIST + MV_AV_CAP)        // [MV_AV_CAP]     per slot: listed this frame (0 / 1)
#define MV_AV_SPH     (MV_AV_MARK + MV_AV_CAP)        // [MV_AV_CAP * 4] per slot: centre xyz, radius (float bits)
#define MV_AV_BOX     (MV_AV_SPH + 4u * MV_AV_CAP)    // [MV_AV_CAP * 6] per slot: ATTACHMENT box lo xyz (~mvOrd), hi xyz (mvOrd)
#define MV_AV_CLN     (MV_AV_BOX + 6u * MV_AV_CAP)    // [MV_AV_CAP]     per slot: its joint clusters this frame (0: none)
#define MV_HIT        (MV_AV_CLN + MV_AV_CAP)         // [MV_AV_CAP + MV_OBJ_CAP] per mover code: texels it made exact
#define MV_OBJ_SPH    (MV_HIT + MV_AV_CAP + MV_OBJ_CAP) // [MV_OBJ_CAP * 4] centre xyz, radius (float bits)
#define MV_ZERO_WORDS MV_OBJ_SPH                      // the per-frame part the host zeroes
// (not zeroed: written whole by the slot's first contributor every frame it is listed -- read only for listed slots,
//  up to MV_AV_CLN's count)
#define MV_AV_CL      (MV_OBJ_SPH + 4u * MV_OBJ_CAP)  // [MV_AV_CAP * MV_CL_N * MV_CL_W] per slot: joint-cluster boxes
// (not zeroed, carried across frames: pass 15's exact-list demand by priority bin, two parities -- SELECT adds this
//  frame's, reads last frame's; SHADE zeroes the one it read. clean_deferred_light.comp THE OVERFLOW RULE)
#define MV_X_HIST     (MV_AV_CL + MV_CL_N * MV_CL_W * MV_AV_CAP)   // [2 * MV_X_BINS]
#define MV_HOLD       (MV_X_HIST + 2u * MV_X_BINS)    // [instance capacity] the time it last moved (float bits)
// The motion HOLD (s): a static instance stays a mover this long after its transform last changed, so an
// object moving in steps (server updates without interpolation, a door's script) does not switch its
// shadow between the two pipelines every few frames.
#define MV_HOLD_S     0.2
// THE AVATAR BOUND (2026-11-14, replaces the one sphere + one box of joints + 0.75 m): a SPHERE about the pelvis (the
// tile cull's and the per-texel prefilter), and the FINE bound the per-texel test takes with it (mvBoxSeg):
//   * the JOINT CLUSTERS: up to MV_CL_N world boxes, the slot's real joints grouped in table order (a joint joins the
//     nearest box it lies within MV_CL_LINK of while that box's longest side stays <= MV_CL_EXT; else a new box; past
//     MV_CL_N the nearest box takes it) -- an arm, a shin, the head are their own boxes, so the space between the limbs
//     and above the head is no longer "inside the avatar". Each box is grown by MV_JOINT_PAD: a rigged mesh's vertex
//     lies within it of a joint it is weighted to (skin, rigged hair / clothing past the bones);
//   * the ATTACHMENT box (MV_AV_BOX): every worn rigid attachment's own sphere (its row's world box), nothing else.
// A caster of the avatar lies in a cluster box or in the attachment box, and in the sphere: the per-texel test is
// sphere AND (any cluster box OR the attachment box), each grown by the light's disc.
// REAL JOINTS ONLY (mvJointReal, clean_rt_instances.comp): the joint table's 216 rows hold more than the skeleton --
//   * an ABSENT joint (no LLJoint at that number: LLCleanSkinStore packs IDENTITY, clean_anim.comp writes identity for
//     an absent one; a flexi pseudo-avatar's unused chains are never written: all zero) sits at the WORLD ORIGIN;
//   * the own avatar's HUD attachment points hang under mScreen, at the world origin + HUD offsets (llvoavatarself);
// both used to be "joints" of the bound: one avatar's sphere and box reached from the region's corner to the avatar
// (its radius ~ the avatar's distance from the origin) -- every light segment in a house crossed it, every texel was
// "near a mover" (the owner's 2026-11-14 log: a ceiling light 2.4 m up "inside the avatar's padded bounds", ~860k
// candidate texels of 890k). Such rows are skipped: an identity / zero linear part, or a joint within MV_HUD_R of the
// world origin while the pelvis is farther than 4 x that.
#define MV_JOINT_PAD  0.4
#define MV_CL_N       16u
#define MV_CL_W       6u      // lo xyz, hi xyz (float bits, padded)
#define MV_CL_LINK    0.5
#define MV_CL_EXT     0.9
#define MV_HUD_R      4.0
// THE EXACT LIST'S PRIORITY (clean_deferred_light.comp THE OVERFLOW RULE): a wanting texel's bin is its distance to the
// mover its segment met, in MV_X_BIN_M steps (a held texel -- the countdown alone -- bin 0)
#define MV_X_BINS     16u
#define MV_X_BIN_M    0.5
// Ordered float bits: mvOrd is monotonic over the floats, so atomicMax(mvOrd(hi)) and atomicMax(~mvOrd(lo)) give the
// box from zeroed words (0 = never written: no box).
uint  mvOrd(float f)   { const uint u = floatBitsToUint(f); return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u); }
float mvUnord(uint o)  { return uintBitsToFloat((o & 0x80000000u) != 0u ? (o & 0x7FFFFFFFu) : ~o); }

// Does sphere (c, r) meet the segment a -> b (inflated by pad)?
bool mvSegHit(vec3 a, vec3 b, vec3 c, float r)
{
    const vec3 ab = b - a;
    const float l2 = dot(ab, ab);
    const float t = l2 > 1e-12 ? clamp(dot(c - a, ab) / l2, 0.0, 1.0) : 0.0;
    const vec3 d = a + ab * t - c;
    return dot(d, d) <= r * r;
}
// Does the segment a -> b meet box [lo, hi] grown by r on every side? (the slab test; a superset of "passes within r
// of the box": a ray from a to any point within r of b runs within r of the segment)
bool mvSegHitBox(vec3 a, vec3 b, vec3 lo, vec3 hi, float r)
{
    lo -= vec3(r); hi += vec3(r);
    const vec3 d = b - a;
    float t0 = 0.0, t1 = 1.0;
    for (int k = 0; k < 3; ++k)
    {
        if (abs(d[k]) < 1e-9)
        {
            if (a[k] < lo[k] || a[k] > hi[k]) return false;
            continue;
        }
        const float inv = 1.0 / d[k];
        float ta = (lo[k] - a[k]) * inv, tb = (hi[k] - a[k]) * inv;
        if (ta > tb) { const float t = ta; ta = tb; tb = t; }
        t0 = max(t0, ta); t1 = min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}
// Does sphere (c, r) meet the ray a + s L, s in [0, tmax]?
bool mvRayHit(vec3 a, vec3 L, float tmax, vec3 c, float r)
{
    const float s = clamp(dot(c - a, L), 0.0, tmax);
    const vec3 d = a + L * s - c;
    return dot(d, d) <= r * r;
}
#ifdef MV_CULL_BODIES
// ---- THE SHARED MOVER CULL (RenderVKShadowMoverMask; moved here from clean_rt_shadow_mask.comp 2026-11-13 so the
//      mask's cull and pass 15's MOVER-EXACT selection -- clean_deferred_light.comp, mode 2 -- run ONE body) ----
// The includer declares the mover buffer as `mv[]` (any set / binding), Light lights[] + ProjShadow pshadow[] and the
// frame info (moverPointRadius / moverProjRadius) before including this file with MV_CULL_BODIES, and enables the
// subgroup basic / arithmetic / ballot / vote extensions. MV_CULL_STAT: the header word pair mvCullBuild counts its
// subgroups into ([MV_CULL_STAT] with a receiver, [+ 1] with a candidate): the mask's words 4 / 5 by default.
#ifndef MV_CULL_STAT
#define MV_CULL_STAT 4u
#endif
uint mvAvCount()  { return uint(mv.length()) > MV_HOLD ? min(mv[0], MV_AV_CAP) : 0u; }
uint mvObjCount() { return uint(mv.length()) > MV_HOLD ? min(mv[1], MV_OBJ_CAP) : 0u; }
// Mover m of this frame's list (avatars first): centre xyz, radius w (<= 0: none).
vec4 mvSphere(uint m)
{
    const uint na = mvAvCount();
    uint b;
    if (m < na) b = MV_AV_SPH + 4u * min(mv[MV_AV_LIST + m], MV_AV_CAP - 1u);
    else        b = MV_OBJ_SPH + 4u * min(m - na, MV_OBJ_CAP - 1u);
    const vec4 s = uintBitsToFloat(uvec4(mv[b], mv[b + 1u], mv[b + 2u], mv[b + 3u]));
    return (any(isnan(s)) || any(isinf(s))) ? vec4(0.0, 0.0, 0.0, -1.0) : s;
}
// A light's disc radius (m), the one pass 15's static rays sample it with (clean_deferred_light.comp
// pdnProjSample; frame info moverLightBits): a ray-traced projector record's soft-shadow radius, a point light's
// RenderVKReSTIRPointRadius disc, a record-less projector's RenderVKRTShadowSoftness. The rays aim anywhere on it.
float mvLightRadius(Light L)
{
    if (L.proj_origin.w < 0.5) return moverPointRadius();
    if (L.tex.z == 0u || L.tex.z - 1u >= uint(pshadow.length())) return moverProjRadius();
    return max(pshadow[L.tex.z - 1u].texel.x, 0.0);
}
// THE MOVER CULL. One test per SUBGROUP (its texels: a compact tile of the 8 x 8 workgroup), cooperative
// and barrier-free (texels that returned early -- sky, outside the mask -- are simply inactive): the
// subgroup's receivers' bounding sphere against every mover sphere -- for the sun / moon the sphere swept
// along the light, for the local lights the sphere grown by the farthest light any of its texels takes --
// keeps up to MV_CAND candidates per kind (more: every mover counts -- mv_*_all). Then per texel and per
// light, a ray / segment test against the candidates alone decides whether that light's mover rays run.
#define MV_CAND 8
uint g_mv_sun[MV_CAND];
uint g_mv_loc[MV_CAND];
uint g_mv_nsun = 0u, g_mv_nloc = 0u;
bool g_mv_sun_all = false, g_mv_loc_all = false;
// The subgroup lane holding the k-th set bit of ballot b (low lanes first): its rank among active lanes.
uint mvRankBelow(uvec4 act, uint lane)
{
    uint r = 0u;
    [[unroll]] for (uint w = 0u; w < 4u; ++w)
    {
        const uint lo = w * 32u;
        if (lane <= lo) break;
        const uint bits = lane >= lo + 32u ? act[w] : (act[w] & ((1u << (lane - lo)) - 1u));
        r += uint(bitCount(bits));
    }
    return r;
}
void mvCullBuild(vec3 P, vec3 L, bool sunWant, float sunFar, bool locWant, float reach)
{
    g_mv_nsun = g_mv_nloc = 0u;
    g_mv_sun_all = g_mv_loc_all = false;
    if (!(uint(mv.length()) > MV_HOLD)) return;   // (no mover buffer bound: nothing moves)
    // (every active lane of the subgroup runs what follows: the wants are the subgroup's)
    sunWant = subgroupAny(sunWant);
    locWant = subgroupAny(locWant);
    const uint n = mvAvCount() + mvObjCount();
    const bool want = sunWant || locWant;
    if (subgroupElect()) atomicAdd(mv[MV_CULL_STAT], 1u);
    if (mv[2] != 0u)   // (a bounds list overflowed: every mover counts)
    {
        g_mv_sun_all = sunWant; g_mv_loc_all = locWant;
        if (want && subgroupElect()) atomicAdd(mv[MV_CULL_STAT + 1u], 1u);
        return;
    }
    if (n == 0u || !want) return;
    const vec3 tmn = subgroupMin(P), tmx = subgroupMax(P);
    const vec3 tc = 0.5 * (tmn + tmx);
    const float tr = 0.5 * length(tmx - tmn) + 0.05;
    const float rch = subgroupMax(reach);
    const uvec4 act = subgroupBallot(true);
    const uint nact = subgroupBallotBitCount(act);
    const uint rank = subgroupBallotExclusiveBitCount(act);
    for (uint base = 0u; base < n; base += nact)
    {
        const uint m = base + rank;
        bool hs = false, hl = false;
        if (m < n)
        {
            const vec4 s = mvSphere(m);
            if (s.w > 0.0)
            {
                hs = sunWant && mvRayHit(tc, L, sunFar, s.xyz, s.w + tr);
                hl = locWant && length(s.xyz - tc) <= s.w + tr + rch;
            }
        }
        const uvec4 bs = subgroupBallot(hs), bl = subgroupBallot(hl);
        [[unroll]] for (uint w = 0u; w < 4u; ++w)
        {
            uint b = bs[w];
            while (b != 0u)
            {
                const uint lane = w * 32u + uint(findLSB(b));
                b &= b - 1u;
                if (g_mv_nsun < uint(MV_CAND)) g_mv_sun[g_mv_nsun++] = base + mvRankBelow(act, lane);
                else g_mv_sun_all = true;
            }
            b = bl[w];
            while (b != 0u)
            {
                const uint lane = w * 32u + uint(findLSB(b));
                b &= b - 1u;
                if (g_mv_nloc < uint(MV_CAND)) g_mv_loc[g_mv_nloc++] = base + mvRankBelow(act, lane);
                else g_mv_loc_all = true;
            }
        }
    }
    if ((g_mv_nsun + g_mv_nloc > 0u || g_mv_sun_all || g_mv_loc_all) && subgroupElect()) atomicAdd(mv[MV_CULL_STAT + 1u], 1u);
}
// Avatar mover m's ATTACHMENT box (THE AVATAR BOUND; m < mvAvCount()). False: none written (an object, or an avatar
// without a worn rigid attachment this frame).
bool mvAvBox(uint m, out vec3 lo, out vec3 hi)
{
    lo = vec3(0.0); hi = vec3(0.0);
    if (m >= mvAvCount()) return false;
    const uint x = MV_AV_BOX + 6u * min(mv[MV_AV_LIST + m], MV_AV_CAP - 1u);
    for (uint k = 0u; k < 3u; ++k)
    {
        const uint a = mv[x + k], b = mv[x + 3u + k];
        if (a == 0u || b == 0u) return false;
        lo[k] = mvUnord(~a); hi[k] = mvUnord(b);
    }
    return !(any(isnan(lo)) || any(isnan(hi)) || any(isinf(lo)) || any(isinf(hi)));
}
// Avatar mover m's joint clusters this frame (THE AVATAR BOUND): their count (0: none written -- the sphere alone)
// and cluster k's padded world box.
uint mvAvClusters(uint m)
{
    if (m >= mvAvCount()) return 0u;
    return min(mv[MV_AV_CLN + min(mv[MV_AV_LIST + m], MV_AV_CAP - 1u)], MV_CL_N);
}
void mvAvCluster(uint m, uint k, out vec3 lo, out vec3 hi)
{
    const uint x = MV_AV_CL + (min(mv[MV_AV_LIST + m], MV_AV_CAP - 1u) * MV_CL_N + min(k, MV_CL_N - 1u)) * MV_CL_W;
    lo = uintBitsToFloat(uvec3(mv[x], mv[x + 1u], mv[x + 2u]));
    hi = uintBitsToFloat(uvec3(mv[x + 3u], mv[x + 4u], mv[x + 5u]));
}
// (stats frames: the per-texel cull counted once more by the SPHERE alone -- the "before the boxes" figure; never a
//  selection: the rays always take sphere AND the fine bound)
bool g_mv_sphere_only = false;
// The FINE half of the per-texel test (the sphere half is the caller's): an avatar's joint-cluster boxes or its
// attachment box, grown by rl; an object (or an avatar without clusters this frame): yes -- its sphere alone.
bool mvBoxSeg(uint m, vec3 P, vec3 X, float rl)
{
    if (g_mv_sphere_only) return true;
    const uint nc = mvAvClusters(m);
    if (nc == 0u) return true;
    for (uint k = 0u; k < nc; ++k)
    {
        vec3 clo, chi;
        mvAvCluster(m, k, clo, chi);
        if (mvSegHitBox(P, X, clo, chi, rl)) return true;
    }
    vec3 lo, hi;
    return mvAvBox(m, lo, hi) && mvSegHitBox(P, X, lo, hi, rl);
}
// Does avatar mover m's bound hold point X (a light carried or touched by it: every segment to X meets it)?
bool mvAvHolds(uint m, vec3 X)
{
    const vec4 s = mvSphere(m);
    if (!(s.w > 0.0) || length(X - s.xyz) > s.w) return false;
    const uint nc = mvAvClusters(m);
    if (nc == 0u) return true;
    for (uint k = 0u; k < nc; ++k)
    {
        vec3 clo, chi;
        mvAvCluster(m, k, clo, chi);
        if (all(greaterThanEqual(X, clo)) && all(lessThanEqual(X, chi))) return true;
    }
    vec3 lo, hi;
    return mvAvBox(m, lo, hi) && all(greaterThanEqual(X, lo)) && all(lessThanEqual(X, hi));
}
// Mover m's code in the per-mover counts (MV_HIT, header word 30): an avatar its joint-table slot, an object 1024 + its
// index in the frame's object list.
uint mvMoverCode(uint m)
{
    const uint na = mvAvCount();
    return m < na ? min(mv[MV_AV_LIST + m], MV_AV_CAP - 1u) : MV_AV_CAP + min(m - na, MV_OBJ_CAP - 1u);
}
#define MV_SEG_NONE 0xFFFFFFFFu
#define MV_SEG_LIST 0xFFFFFFFEu   // (a bounds-list overflow: no mover to name, every segment a yes)
// rl: the light's disc radius (a soft light's rays aim anywhere on it: the sphere grown by it covers the cone
// from P to the disc -- conservative, never a lost blocker in the penumbra). An avatar must ALSO meet its fine bound
// grown by rl (mvBoxSeg; both bound the caster, so a blocked ray meets both -- exact, the same image).
// The FIRST mover (the frame list's order) whose bounds the segment meets: its index, else MV_SEG_NONE / MV_SEG_LIST.
// THE OWN FIXTURE (g_mv_fix_max > 0: the caller's light takes the own-fixture rule, clean_vis_walk.glsl rtFixNext --
// RenderVKReSTIRFixtureSize): a MOVING OBJECT whose sphere holds the light's centre X and is no larger than a fixture
// (radius <= the box edge x sqrt(3) / 2) is that light's own shade / bulb / flame -- a flickering or llTargetOmega
// flame the re-class makes a mover. The light's rays pass over it, so it is never "a mover on the segment": without
// this every texel the light reached went exact (2026-11-13: red discs around houses with no avatar in them).
// The mask (mode 1) leaves it 0.
float g_mv_fix_max = 0.0;
bool mvOwnFixture(uint m, vec4 s, vec3 X)
{
    return g_mv_fix_max > 0.0 && m >= mvAvCount() && s.w <= 0.866 * g_mv_fix_max
        && dot(X - s.xyz, X - s.xyz) <= s.w * s.w;
}
uint mvSegHitFirst(vec3 P, vec3 X, float rl)
{
    // TILE OVERFLOW (more than MV_CAND candidate movers in this subgroup's tile -- a worn animesh's separately moving
    // prims, a crowd): the per-texel test runs against EVERY mover instead of answering "yes" for every light. The
    // blanket yes made every light a "light with a mover on its way" in that tile only, so the radiance packing's
    // top 4 there were simply the 4 strongest lights -- a mover-blocked fifth lost its shadow in the overflowing tiles
    // and kept it in their neighbours: tile-shaped, straight-edged patches (2026-11-11, the owner's animesh "boxes").
    // (A bounds-list overflow, mv[2]: the list itself is incomplete -- every light stays a yes.)
    if (g_mv_loc_all)
    {
        if (mv[2] != 0u) return MV_SEG_LIST;
        const uint n = mvAvCount() + mvObjCount();
        for (uint m = 0u; m < n; ++m)
        {
            const vec4 s = mvSphere(m);
            if (s.w > 0.0 && !mvOwnFixture(m, s, X) && mvSegHit(P, X, s.xyz, s.w + rl) && mvBoxSeg(m, P, X, rl)) return m;
        }
        return MV_SEG_NONE;
    }
    for (uint k = 0u; k < g_mv_nloc; ++k)
    {
        const uint m = g_mv_loc[k];
        const vec4 s = mvSphere(m);
        if (!mvOwnFixture(m, s, X) && mvSegHit(P, X, s.xyz, s.w + rl) && mvBoxSeg(m, P, X, rl)) return m;
    }
    return MV_SEG_NONE;
}
bool mvSegHitAny(vec3 P, vec3 X, float rl) { return mvSegHitFirst(P, X, rl) != MV_SEG_NONE; }
#endif   // MV_CULL_BODIES
#endif
