// vkclean -- HIT LIGHT SAMPLING (sealed-room leak): how a ray-traced hit (a GI bounce) shadows its local
// lights with a fixed number of shadow rays. Shared by clean_rt_shade.glsl (rtHitLocalLights) and the
// CPU fixture scripts/check/test_hit_nee.cpp (scripts/check/checks/hit_nee.py), so the logic the check
// runs is the logic the GPU runs.
//
// THE LEAK IT REPLACES: the hit shadow-ray budget traced the K BRIGHTEST shadowable shares at the hit and
// added EVERY OTHER light that reached it UNSHADOWED. With K = 1 (RenderVKGIHitShadowRays' default) and a
// house full of lamps, a GI ray inside a closed room hit a wall lit by the next room's lamp at full
// strength (only the room's own brightest light was tested): light through solid walls, at any distance
// from the wall the lamp's radius reaches, carried on by the radiance cache's multi-bounce.
//
// THE ESTIMATOR (next-event estimation, Pharr / Jakob / Humphreys PBRT 14.2; Talbot et al. 2005 RIS with
// the exact target): every shadowable share s_i at the hit enters K independent weighted reservoirs
// (streaming, with replacement) with weight w_i = lum(s_i) > 0; reservoir k keeps share s_k with
// probability w_k / W, W = sum w_i. Each pick takes ONE shadow ray v_k and contributes
//     s_k x W / (K w_k) x v_k
// -- an unbiased estimate of sum s_i v_i with no unshadowed remainder: a light every ray finds blocked
// adds exactly 0. Every pick's luminance is W / K (no firefly above the total), the cost is K rays per
// hit whatever the light count, and the denoisers integrate the noise.
//
// hnAdd / hnFinish keep the budget's slot layout (score, light, share; a valid pick prefix), and hnFinish
// leaves each slot's share SCALED by W / (K w_k): the consumers that subtract share x (1 - vis) per traced
// slot from the walk's sum (the inline loop, the visibility queue's resolve vqHitLocal) need no change.
#ifndef CLEAN_HIT_NEE_H
#define CLEAN_HIT_NEE_H
#define HN_MAXK 4
#ifdef __cplusplus
// (the fixture's vec3 shim and uint come from the includer)
#define HN_FN inline
#define inout
#define HN_REF(T) T&
#else
#define HN_FN
#define HN_REF(T) inout T
#endif
// PCG hash (O'Neill; the generic one used across the shaders)
HN_FN uint hnHash(uint v)
{
    const uint st = v * 747796405u + 2891336453u;
    const uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return (w >> 22u) ^ w;
}
// the uniform [0, 1) of candidate i in reservoir k (seed: the hit and the frame)
HN_FN float hnRand(uint seed, uint i, uint k)
{
    return float(hnHash(seed ^ hnHash(i * 4u + k + 0x5BD1E995u)) >> 8u) * (1.0f / 16777216.0f);
}
HN_FN float hnLum(vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
// Empties the K slots.
HN_FN void hnInit(inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK])
{
    for (int j = 0; j < HN_MAXK; ++j) { score[j] = 0.0f; light[j] = 0u; share[j] = vec3(0.0f); }
}
// Candidate i (unique per walk step): light li with its SHADOWABLE share sh. Returns the new weight sum W.
// A share without visible light (lum <= HN_MIN_W) is no candidate: it adds nothing, shadowed or not. (The
// floor keeps W / (K w) finite: the uniform is 0 once in 2^24 draws, and 0 x W < w picks ANY w -- a share
// at the cutoff fade, ~1e-38, scaled by W / w overflowed to inf and put a NaN in the hit's light.)
#define HN_MIN_W 1e-20f
HN_FN float hnAdd(float wsum, uint K, uint seed, uint i, uint li, vec3 sh,
                  inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK])
{
    const float w = hnLum(sh);
    if (!(w > HN_MIN_W)) return wsum;
    const float W = wsum + w;
    for (int j = 0; j < HN_MAXK; ++j)
        if (uint(j) < K && hnRand(seed, i, uint(j)) * W < w)
        {
            score[j] = w; light[j] = li; share[j] = sh;
        }
    return W;
}
// After the walk: each pick's share scaled to its estimate (W / (K w_k)), its score = W / K. Returns the
// sum of the scaled shares -- what the walk adds to its total before the K rays remove the blocked parts.
HN_FN vec3 hnFinish(float wsum, uint K, inout float score[HN_MAXK], inout vec3 share[HN_MAXK])
{
    vec3 s = vec3(0.0f);
    for (int j = 0; j < HN_MAXK; ++j)
    {
        if (uint(j) >= K || !(score[j] > 0.0f)) { score[j] = 0.0f; continue; }
        share[j] = share[j] * (wsum / (float(K) * score[j]));
        score[j] = wsum / float(K);
        s = s + share[j];
    }
    return s;
}
// THE STRATIFIED DRAW (the RT reflection hits: clean_rt_hit_lights.glsl RT_SHADE_STOCH_XI; the light grid's hit picks,
// clean_regir.glsl regirHitLights). hnAdd draws a fresh hash per candidate and reservoir, every frame: a pixel's picks
// are independent from frame to frame, and the reflections' short history (4 frames under DLSS RR) averages a handful of
// independent picks -- the crawl. Here each reservoir runs on ONE uniform u per frame, rescaled at every candidate
// into the next step's uniform: take candidate i iff u < p = w_i / W_i, then u <- 1 - u / p (taken) or
// (u - p) / (1 - p) (passed). Given the decisions so far u stays uniform on [0, 1), so every decision is the exact
// reservoir step and each frame's estimate is the same unbiased one-sample estimator (W / (K w) per pick). But the
// pick is now a FIXED function of u: inverse-CDF sampling of the candidates (contiguous for the first three in
// arrival order; the taken branch's map runs downward, so a new candidate's slices of two neighbouring regions meet;
// a later one's region is split in a few pieces). The caller makes u a low-discrepancy sequence: each pixel's own
// rotation + the frame index along a Kronecker sequence (consecutive frames' picks cover the light CDF in strata),
// and the K reservoirs (k + u) / K (K strata of the CDF within the frame). Precision: each rescale multiplies u's
// rounding error by 1 / p (taken) or 1 / (1 - p); past HN_STRAT_AMP (u known to ~2^-14) a reservoir falls back to
// hnRand for the rest of the walk -- still exact (an independent uniform per step), only no longer stratified.
#define HN_STRAT_AMP 1024.0f
#define HN_U_MAX 0.99999994f
// One stratified decision on (u, amp) against p in (0, 1]: true = take the candidate; u is rescaled for the next step.
HN_FN bool hnStratTake(HN_REF(float) u, HN_REF(float) amp, float p)
{
    const bool take = u < p;
    const float q = take ? p : 1.0f - p;
    const float d = (take ? u : u - p) / q;
    const float x = take ? 1.0f - d : d;
    u = x > 0.0f ? (x < HN_U_MAX ? x : HN_U_MAX) : 0.0f;
    amp = amp / q;
    return take;
}
// The K reservoirs' uniforms from the frame's stratified xi in [0, 1): (k + xi) / K -- reservoir k in stratum k, at
// the frame's point within it, so each stratum walks the caller's sequence itself whatever K is. (xi + k / K mod 1
// put the same points in the strata, but each one stepped by K x the sequence's step: K = 2 with a step of 0.54
// moved 0.08 of a stratum per frame -- four frames all but one sample.)
HN_FN void hnStratInit(float xi, uint K, inout float u[HN_MAXK], inout float amp[HN_MAXK])
{
    const float kk = float(K > 0u ? K : 1u);
    for (int j = 0; j < HN_MAXK; ++j)
    {
        const float x = (float(j) + xi) / kk;
        u[j] = x > 0.0f ? (x < HN_U_MAX ? x : HN_U_MAX) : 0.0f;
        amp[j] = 1.0f;
    }
}
// hnAdd on the stratified uniforms (hnStratInit before the walk). seed / i: the fallback's hash (as hnAdd's).
HN_FN float hnAddS(float wsum, uint K, inout float u[HN_MAXK], inout float amp[HN_MAXK], uint seed, uint i, uint li, vec3 sh,
                   inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK])
{
    const float w = hnLum(sh);
    if (!(w > HN_MIN_W)) return wsum;
    const float W = wsum + w;
    const float p = w / W;
    for (int j = 0; j < HN_MAXK; ++j)
    {
        if (uint(j) >= K) continue;
        const bool take = amp[j] < HN_STRAT_AMP ? hnStratTake(u[j], amp[j], p) : hnRand(seed, i, uint(j)) * W < w;
        if (take)
        {
            score[j] = w; light[j] = li; share[j] = sh;
        }
    }
    return W;
}
// Fixed total K-ray budget for reflections: retain up to K exact shares. If more arrive,
// retain the strongest exactly and sample the remaining shares with K-1 rays. No light is
// left unshadowed, duplicated, or discarded. K=1 uses the original sampler at the call site.
HN_FN float hnBudgetAdd(float W, uint K, uint seed, uint id, uint li, vec3 sh,
                        inout float es[HN_MAXK], inout uint el[HN_MAXK], inout vec3 ev[HN_MAXK],
                        inout float u[HN_MAXK], inout float amp[HN_MAXK],
                        inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK])
{
    float w = hnLum(sh);
    if (!(w > HN_MIN_W)) return W;
    for (int j = 0; j < HN_MAXK; ++j)
        if (uint(j) < K && w > es[j])
        {
            const float sw = es[j]; const uint sl = el[j]; const vec3 sv = ev[j];
            es[j] = w; el[j] = li; ev[j] = sh;
            w = sw; li = sl; sh = sv;
        }
    return w > HN_MIN_W ? hnAddS(W, K-1u, u, amp, seed, id, li, sh, score, light, share) : W;
}
HN_FN vec3 hnBudgetFinish(float W, uint K, uint seed,
                         inout float es[HN_MAXK], inout uint el[HN_MAXK], inout vec3 ev[HN_MAXK],
                         inout float u[HN_MAXK], inout float amp[HN_MAXK],
                         inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK])
{
    if (!(W > 0.0f))
    {
        vec3 sum = vec3(0.0f);
        for (int j = 0; j < HN_MAXK; ++j)
        {
            score[j] = es[j]; light[j] = el[j]; share[j] = ev[j];
            if (uint(j) < K) sum = sum + ev[j];
        }
        return sum;
    }
    for (int j = 1; j < HN_MAXK; ++j)
        if (uint(j) < K)
            W = hnAddS(W, K-1u, u, amp, seed, uint(j)+0x10000u, el[j], ev[j], score, light, share);
    const vec3 sum = hnFinish(W, K-1u, score, share) + ev[0];
    for (int j = HN_MAXK-1; j > 0; --j)
    {
        score[j] = score[j-1]; light[j] = light[j-1]; share[j] = share[j-1];
    }
    score[0] = es[0]; light[0] = el[0]; share[0] = ev[0];
    return sum;
}
// EXACT SLOTS (the forward alpha-blend layers' light, clean_draw.frag CLEAN_BLEND_HIT): a blended layer has no
// pixel history and no denoiser, so the estimator's noise above shows as it is. Most of a receiver's light comes
// from one or two lamps; the M STRONGEST shadowable shares therefore take a shadow ray each and enter the sum
// exactly (no estimator noise for the lights that dominate), and only the REST are candidates of the K picks.
// Unbiased: every share is either exact (its own ray) or a reservoir candidate (W / (K w) above), never both,
// never neither -- hnExactOffer hands back the one share that leaves the exact set at each step (the offered one
// when it is not among the M strongest so far, else the one it displaced) and the caller adds THAT to the
// reservoirs; reservoir sampling does not depend on the order the candidates arrive in. Cost: M + K shadow rays.
// THE STRATIFIED TAIL (hnExactDemote): the walk offers every share to M + K exact slots. When no more than M + K
// shares reach the receiver, all of them are exact, and the K picks stay empty. That is every receiver a typical
// window sees (1 to 8 lights), so it is NOISE-FREE there. When more shares arrive, the K weakest exact slots join
// the reservoirs after the walk: the M strongest stay exact and K picks sample the rest. Either way at most M + K
// shadow rays, and a receiver with fewer lights traces one ray per light. The plain M exact + K picks rule was exact
// only up to M + 1 lights; with M = K = 2, a fourth light in reach re-drew the picks every frame (the grain in the
// circles of the lights' reach).
// PAYLOAD g: each slot also carries a scalar that goes with its share (the alpha glare its light adds, clean_draw
// glare): the exact slots keep it with the share, and hnAddG / hnFinishG scale it by the same W / (K w) as the share,
// so the ray that removes a share's blocked part removes its glare's too.
#define HN_MAXM 8
// Empties the exact slots.
HN_FN void hnExactInit(inout float escore[HN_MAXM], inout uint elight[HN_MAXM], inout vec3 eshare[HN_MAXM],
                       inout uint eid[HN_MAXM], inout float eg[HN_MAXM])
{
    for (int j = 0; j < HN_MAXM; ++j) { escore[j] = 0.0f; elight[j] = 0u; eshare[j] = vec3(0.0f); eid[j] = 0u; eg[j] = 0.0f; }
}
// Offers candidate (id, li, sh, g) to the first E exact slots (descending by luminance). Returns true with the share
// that leaves the exact set in (id, li, sh, g) -- the caller hands it to hnAddG -- or false when nothing is left over
// (an empty slot took it, or a share without visible light: no candidate at all, as in hnAdd).
HN_FN bool hnExactOffer(uint E, inout float escore[HN_MAXM], inout uint elight[HN_MAXM], inout vec3 eshare[HN_MAXM],
                        inout uint eid[HN_MAXM], inout float eg[HN_MAXM],
                        HN_REF(uint) id, HN_REF(uint) li, HN_REF(vec3) sh, HN_REF(float) g)
{
    float sc = hnLum(sh);
    if (!(sc > HN_MIN_W)) return false;
    for (int j = 0; j < HN_MAXM; ++j)
        if (uint(j) < E && sc > escore[j])
        {
            const float ts = escore[j]; const uint ti = eid[j]; const uint tl = elight[j]; const vec3 tv = eshare[j];
            const float tg = eg[j];
            escore[j] = sc; eid[j] = id; elight[j] = li; eshare[j] = sh; eg[j] = g;
            sc = ts; id = ti; li = tl; sh = tv; g = tg;
        }
    return sc > 0.0f;
}
// hnAdd with the payload: reservoir j keeps g with the share it keeps.
HN_FN float hnAddG(float wsum, uint K, uint seed, uint i, uint li, vec3 sh, float g,
                   inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK], inout float sg[HN_MAXK])
{
    const float w = hnLum(sh);
    if (!(w > HN_MIN_W)) return wsum;
    const float W = wsum + w;
    for (int j = 0; j < HN_MAXK; ++j)
        if (uint(j) < K && hnRand(seed, i, uint(j)) * W < w)
        {
            score[j] = w; light[j] = li; share[j] = sh; sg[j] = g;
        }
    return W;
}
// hnFinish with the payload: each pick's g scaled as its share (W / (K w_k)).
HN_FN vec3 hnFinishG(float wsum, uint K, inout float score[HN_MAXK], inout vec3 share[HN_MAXK], inout float sg[HN_MAXK])
{
    for (int j = 0; j < HN_MAXK; ++j)
        sg[j] = (uint(j) < K && score[j] > 0.0f) ? sg[j] * (wsum / (float(K) * score[j])) : 0.0f;
    return hnFinish(wsum, K, score, share);
}
// After the walk (the stratified tail, above): when the walk left any share to the reservoirs (wsum > 0, more than
// E = M + K shares), the exact slots M .. E - 1 (the weakest) join them. Returns the new weight sum; the exact set
// is then the first M slots. Without a tail nothing moves, and all E slots stay exact.
HN_FN float hnExactDemote(float wsum, uint M, uint E, uint K, uint seed,
                          inout float escore[HN_MAXM], inout uint elight[HN_MAXM], inout vec3 eshare[HN_MAXM],
                          inout uint eid[HN_MAXM], inout float eg[HN_MAXM],
                          inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK], inout float sg[HN_MAXK])
{
    if (!(wsum > 0.0f)) return wsum;
    for (int j = 0; j < HN_MAXM; ++j)
        if (uint(j) >= M && uint(j) < E && escore[j] > 0.0f)
        {
            wsum = hnAddG(wsum, K, seed, eid[j], elight[j], eshare[j], eg[j], score, light, share, sg);
            escore[j] = 0.0f;
        }
    return wsum;
}
// After the walk: the sum of the exact shares (each removed again by its own ray where blocked).
HN_FN vec3 hnExactSum(uint M, float escore[HN_MAXM], vec3 eshare[HN_MAXM])
{
    vec3 s = vec3(0.0f);
    for (int j = 0; j < HN_MAXM; ++j)
        if (uint(j) < M && escore[j] > 0.0f) s = s + eshare[j];
    return s;
}
#undef HN_REF
#ifdef __cplusplus
#undef inout
#endif
#endif
