"""Regression guard for crawling noise in mixed local light beside moving casters (radiance mode).

RenderVKShadowCasterHistory 2 resets a texel's local-light history when its visibility evidence (MOT w, the
producer's num / den) and its same-surface 3x3 mean both leave the history's visibility. That evidence is ONE
or TWO RIS picks: 0 or 1 per pick wherever one light reaches the receiver and another is blocked. The reused
reservoir pick is shared by the spatial-reuse neighbourhood, so the 3x3 mean flipped with it, and a 0.1
tolerance read every flip as a moving caster: on every moving-caster-tagged texel (an animating avatar and
its shadows) the history was cut to two frames and the binary picks crawled through. The fix:
  * the evidence counts only spatially independent samples (pdnEvidenceWeight; MOT bit 19 marks exact lists);
  * a change must exceed the 3x3 mean's sampling noise (dnCasterSamplingNoise: Bhatia-Davis p (1 - p),
    pooled with a young prior) -- K = 3.5 in one frame, or 2.2 in the same direction in two consecutive frames
    (dnCasterPending / dnCasterConfirmed: two pending bits in W's low mantissa);
  * the prior accumulates the 3x3 mean at 10 bits (the 5-bit code lost every 32-frame step: it drifted);
  * a young history still deviating keeps the two-frame cap (dnCasterFollow).

The fixture compiles those production GLSL helpers (clean_dn_temporal.comp, clean_pdn_common.glsl,
clean_history_time.glsl) into C++ and replays the temporal policy on a 24 x 24 texel receiver lit by two equal
point lights + a faint wall-blocked third (3 candidates > K = 2: the RIS picks, not the exact list):
  (a) one light visible, one blocked, every texel moving-caster tagged (an animating receiver), at 60, 30 and
      144 Hz: <= 0.1 reset per texel-second, mean = the visible light alone, and at 60 Hz a frame-to-frame rms
      <= 2 x a detector-free converged history's;
  (b) a lit floor; a caster's shadow edge sweeps in blocking one light, then away (0.5 and 2 texels / frame);
  (c) the mixed floor of (a); an edge sweeps in blocking the visible light (-> 0), then away.
(b) / (c) bound the bias behind the moving edge (the 40a5446bba trailing) over 64 runs -- interior (full 3x3)
<= 0.2, the grid's edge rows (6 same-surface neighbours: a silhouette) <= 0.6 -- and a no-detector control
must trail, so the bound means something. Old code (no pdnEvidenceWeight / dnCasterSamplingNoise) replays with
its own evidence and test, and fails (a): 55 % resets, rms 42 x.

(a) - (c) tag every texel on every frame. The producer tags a texel only on the frames one of ITS rays met the
moving caster, and the history carried that tag one frame: a texel a moving shadow had LEFT was tested once, too
early to confirm (the two-frame rule) -- the owner's ghost trail behind a moving avatar's shadow, which policy 1
removed by capping every tagged texel (and crawled the mixed light). (d) / (e) replay the full composition with the
real tags, the radiance clamp (sigma floored at the history's spread), the light-drop test and THE MOVER CARRY
(dnCasterCarry / dnCasterDeviating: the tag carried DN_CASTER_CARRY frames, held while the 3x3 still deviates):
  (d) a 12-texel shadow band crosses the floor at 0.25 - 2 texels / frame, over a lit floor (blocking one light of
      two) and over the mixed floor (blocking the visible light): the bias behind it (trailing) and inside it
      (leading) <= 0.2 of the change. Old composition: trailing 0.74 / 0.61 / 0.33 at 0.5 / 1 / 2 texels / frame.
  (e) the idle-animating avatar (tags coming and going with the picks): (a)'s reset and rms bounds; policy 1's rms
      reported (40 x the converged history's: why policy 1 is no default).
Policy 2 alone left two failures, statistical by nature (fixed by policy 3, THE MOVER RECORD, the default):
  (f) CAMERA FOLLOW: the band stays on screen, the floor scrolls under it at 0.25-0.75 texel / frame (fractional,
      bilinear reprojection). Policy 2 lags 0.25-0.33 of the change (trailing on the lit floor, leading on the
      mixed one); policy 1 0.03-0.05. Mechanism: every frame the bilinear fetch of the texel just outside the band
      takes ~25 % of its in-band neighbour; that leak convects outward and the prior is blurred the same way, so no
      3x3 ever deviates beyond the noise bound (trace: prior 0.70-0.88 vs a unanimous 3x3 of 1.0, 1.7-2 sigma).
  (g) REVEAL SPREAD: the std across a revealed column's rows (one truth) in its first 6 frames, relative to the step.
      Lit floor: policy 2 0.19-0.29 vs policy 1 0.13-0.14 -- the light returns texel by texel (each 3x3 crosses its
      bound at its own frame). Mixed floor: policy 2 0.28-0.41, policy 1 ~0.40 (policy 1's own 2-frame noise).
  Tried and rejected for policy 2 (all in this fixture): a 24-tap evidence ring on deviating texels (false resets
  2-4x, (e) fails), a radiance RISE footprint mirroring the drop test (~5 of 25 taps carry signal; (e) fails or it
  never fires), nearest-tap prior (no transport below half a texel per frame), dropping stale bilinear taps by
  evidence (no effect), soft / flat history caps on 1.5-2 sigma deviations ((e) rms 2.1-5 x), a 3x3 "no mover hit"
  likelihood test (any threshold under ~12 nats fires on idle coincidences, (e) 0.14 resets / texel-s; above, (f)
  0.12-0.14: the leak is in the fetch, not the test).
POLICY 3 (clean_dn_temporal.comp THE MOVER RECORD): the producer reports WHICH light a moving caster blocked (MOT w
bits 21-29, pdnMotReport*); each texel's history carries a record of it (the HIST age word y: the light, its
mover-blocked share, the last definite covered / uncovered observation, the colour's mover-shadowed share, the reveal
evidence). THE GATE keeps the bilinear taps on this texel's side of a mover edge (the leak stops at its source);
THE REVEAL resets (seeded from the 3x3 texels no mover blocked) when the record's light reaches a texel and its
covered neighbours (likelihood ratio 1 / (1 - blocked) per sample, 9 nats). An idle umbra never reports its light
visible: no false reveal by construction. Bounds (policy 3):
  (d) as policy 2; (f) max(trailing, leading) <= 0.08 (0.002-0.069); (g) lit floor <= 1.6 x policy 1 (1.57 / 1.35 /
      1.09 / 0.93 at 0.25 / 0.5 / 1 / 2 texels / frame), mixed floor <= 1.05 x policy 1 (0.90-0.97);
  (e) (a)'s bounds; (e2) the idle avatar's PENUMBRA (B mover-blocked on half its samples, else visible: hair breathing
      in an area light's soft shadow): the reveal <= 0.05 / texel-second, the rms bound;
  (h) a STATIC mover edge: camera still -- rmse within 5 % of policy 2 and no extra resets; panning over the static
      scene at 0.25 / 0.75 texel / frame -- rmse <= policy 2's (0.23 vs 0.28). At an exact half-texel pan every frame
      the heaviest tap is a coin toss: 0.29 vs 0.28, reported.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "caster-noise"
STAGE = "source"
DESCRIPTION = "mixed local light beside moving casters: no pick-noise history resets, no trailing edge, no smear " \
              "under camera follow, reveals fill in as regions"
GUARDS = ("Crawling noise on animating avatars / moving-caster shadows where one local light is visible and "
          "another blocked: the caster-change detector reading one-pick visibility noise as a moving caster; "
          "and the opposite failures, a moving caster's shadow trailing behind it (also with the camera following "
          "the avatar) and a receding shadow's light returning texel by texel.")


def _function(src, name):
    m = re.search(r"(?:bool|float|uint)\s+" + name + r"\([^)]*\)\s*\{", src)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[m.start():i + 1]
        i += 1
    return None


_FIXTURE = r"""
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using uint = unsigned int;
using std::abs; using std::isinf; using std::sqrt; using std::exp; using std::log;
static float min(float a, float b) { return std::min(a, b); }
static float max(float a, float b) { return std::max(a, b); }
static uint max(uint a, uint b) { return std::max(a, b); }
static uint min(uint a, uint b) { return std::min(a, b); }
static float clamp(float x, float a, float b) { return std::clamp(x, a, b); }
static float mix(float a, float b, float t) { return a + (b - a) * t; }
static float testStep = 1.0f;   // (frame interval / the 60 Hz reference: the (a) replay's cadence)
static float htStep() { return testStep; }
HELPERS
// The temporal pass's caster test as clean_dn_temporal.comp main() composes the helpers (edgeD / the
// pending bits / the restart); the OLD composition when the source has none of them.
static bool detect(float own, float mean, float prior, float sig, float n, float len, uint prevPend, uint& pend, bool& follow)
{
    follow = false;
#ifdef NEW_POLICY
    const float noise = dnCasterSamplingNoise(prior, mean, n, n, len);
    const float cur = dnCasterCurrent(own, mean, false);   // (every texel's evidence a stochastic pick)
    pend = dnCasterPending(cur, mean, prior, sig, noise);
    const bool changed = dnCasterVisibilityChange(cur, mean, prior, sig, noise) || dnCasterConfirmed(pend, prevPend);
    follow = dnCasterFollow(pend, len);
    if (changed) pend = 0u;
    return changed;
#else
    (void)n; (void)len; (void)prevPend; pend = 0u;
    return dnCasterVisibilityChange(own, mean, prior, sig);
#endif
}
static float restart(float own, float mean, float n)
{
#ifdef NEW_POLICY
    return dnCasterEvidence(own, mean, n);
#else
    (void)mean; (void)n; return own;
#endif
}
static float evidence(float own, float mean, float n) { return restart(own, mean, n); }
static float evidenceWeight(bool reused, uint ke, uint j)
{
#ifdef NEW_POLICY
    return pdnEvidenceWeight(reused, ke, j);
#else
    (void)reused; (void)ke; (void)j; return 1.0f;   // (the old evidence: every sample's num / den)
#endif
}

static const int N = 24, BLOCK = 4;
static uint rng = 0x9E3779B9u;
static float rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return float(rng >> 8) * (1.0f / 16777216.0f); }
// the history meta's evidence prior: its stored code (10 bits; old: 5) and accumulation rate (old: the colour's)
static float storePrior(float v)
{
#ifdef NEW_POLICY
    return pdnMetaVisibility(pdnMetaVisibilityCode(v));
#else
    return pdnVisibilityValue(pdnVisibilityCode(v));
#endif
}
static float evidenceAlpha(float age, float colourAlpha)
{
#ifdef NEW_POLICY
    (void)colourAlpha; return dnCasterEvidenceAlpha(age);
#else
    (void)age; return colourAlpha;
#endif
}
// the two point lights + a faint candidate behind a wall (3 candidates > K = 2: the RIS picks, not the exact list)
static const float T[3] = {1.0f, 1.0f, 0.05f};
static int pick() { const float u = rnd() * (T[0] + T[1] + T[2]); return u < T[0] ? 0 : (u < T[0] + T[1] ? 1 : 2); }

struct Px { float acc = 0, age = 0, vis = -1, ev = -1, raw = 0; uint pend = 0; bool init = false; };
// One frame of the radiance producer (K = 2: the reused reservoir, shared by its BLOCK x BLOCK spatial-reuse
// neighbourhood, + the fresh pick, per texel) and the temporal pass (every texel moving-caster tagged:
// casterAffected -- an animating receiver, an avatar's shadow). vis(x, y, light): 1 visible, 0 blocked.
template <class Vis>
static int frame(std::vector<Px>& g, Vis vis, bool detector)
{
    const float W = T[0] + T[1] + T[2];
    int res[N / BLOCK][N / BLOCK];
    for (auto& r : res) for (int& v : r) v = pick();
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        Px& p = g[y * N + x];
        const int li[2] = {res[y / BLOCK][x / BLOCK], pick()};
        float raw = 0, en = 0, ed = 0;
        for (uint j = 0; j < 2; ++j)
        {
            const float v = vis(x, y, li[j]), c = W * 0.5f;   // target x weight W / (Ke p)
            raw += c * v;
            const float ew = evidenceWeight(true, 2u, j);
            en += c * v * ew; ed += c * ew;
        }
        p.raw = raw;
        p.ev = ed > 1e-8f ? pdnVisibilityValue(pdnVisibilityCode(en / ed)) : -1.0f;
    }
    int fired = 0;
    std::vector<Px> out = g;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const Px& p = g[y * N + x];
        Px& o = out[y * N + x];
        if (!p.init) { o.acc = p.raw; o.age = 1; o.vis = storePrior(p.ev); o.init = true; continue; }
        float s = 0, s2 = 0, n = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            const int qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= N || qy >= N) continue;
            const float e = g[qy * N + qx].ev;
            if (e < 0) continue;
            s += e; s2 += e * e; n += 1;
        }
        const float mean = n > 0 ? s / n : 0, sig = n > 0 ? sqrt(max(s2 / n - mean * mean, 0.0f)) : 0;
        uint pend = 0u;
        bool follow = false;
        const bool changed = detector && n > 0 && detect(p.ev, mean, p.vis, sig, n, p.age, p.pend, pend, follow);
        float hist = p.acc, len = p.age;
        if (changed) { len = 0; hist = p.raw; ++fired; }
        const float cap = dnCasterShouldCap(detector ? 2u : 0u, true, changed || follow) ? dnCasterHistoryCap(32.0f, htStep()) : 32.0f;
        const float age = htAdvance(len, cap);
        const float a = len > 0 ? htAlpha(1.0f / max(age, 1.0f)) : 1.0f;
        o.acc = mix(hist, p.raw, a);
        o.age = age;
        o.pend = pend;
        o.vis = storePrior(dnCasterVisibilityAcc(p.vis, changed ? restart(p.ev, mean, n) : evidence(p.ev, mean, n),
                                                 changed ? 1.0f : evidenceAlpha(p.age, a)));
    }
    g.swap(out);
    return fired;
}

// ---- (d) / (e): the temporal pass as main() composes it, with the producer's REAL moving-caster tags ----
// frame() above tags every texel on every frame. The producer tags a texel (MOT w bit 8) only when one of THIS
// frame's rays hit the moving caster: a texel inside a shadow of light B picks B with one of its two picks ~75 %
// of the frames, and a texel the shadow has left is tagged by nobody. The history meta carries the tag (old: bit 11,
// the previous frame only; new: dnCasterCarry). Here also the radiance clamp (DN_F_RADIANCE: the 3x3 sigma floored
// at the history's own spread), the light-drop test and the drop mark, as clean_dn_temporal.comp main().
#ifdef NEW_POLICY
// rec: THE MOVER RECORD (policy 3; clean_dn_temporal.comp dnRec*), the HIST age word y
struct Pd { float h = 0, hv = 0, len = 0, vis = -1; uint pend = 0, carry = 0, rec = 0; bool init = false; };
struct Dstat { int resets = 0, reveals = 0; };
// CAMERA FOLLOW (the owner walking his own avatar): the floor scrolls g_scroll texels per frame under a screen-fixed
// shadow; this texel's history is last frame's at x + g_scroll -- the temporal pass's bilinear same-surface taps
// (dnFetchHistory: values weighted, the pending bits the heaviest tap's, the mover carry the taps' longest).
static float g_scroll = 0.0f;
static uint g_frame = 0u;   // (the producer's frame counter: the report's alternation)
static uint rng2 = 0x2545F491u;   // (the record's dither: its own stream, so policy 3 sees policy 2's picks)
static float dither() { rng2 ^= rng2 << 13; rng2 ^= rng2 >> 17; rng2 ^= rng2 << 5; return float(rng2 >> 8) * (1.0f / 16777216.0f); }
// The history fetch. Policy 3 (MOVER): the taps' records first -- the record's light (the heaviest tap carrying one),
// state (the heaviest tap), this frame's observation, THE GATE on the content span -- then the kept taps, their blocked
// share (the minimum, 0 for another light), content and evidence weighted, as dnFetchHistory.
struct Rec { uint light = 0, state = 0, obs = 0; float blocked = 0, content = 0, evidence = 0; };
static Pd fetchD(const std::vector<Pd>& g, float hx, int y, bool mover, uint mot, Rec& r)
{
    const int x0 = int(std::floor(hx));
    const float f = hx - float(x0);
    r = Rec();
    uint keep = 0u;
    for (int k = 0; k < 2; ++k)
    {
        const int q = x0 + k;
        const float b = k ? f : 1.0f - f;
        if (q >= 0 && q < N && b > 1e-4f && g[y * N + q].init) keep |= 1u << k;
    }
#ifdef MOVER
    if (mover)
    {
        float bwL = 0, bwS = 0, lo = 1, hi = 0;
        for (int k = 0; k < 2; ++k)
        {
            if (!(keep & (1u << k))) continue;
            const float b = k ? f : 1.0f - f;
            const uint rc = g[y * N + x0 + k].rec;
            if (b > bwS) { bwS = b; r.state = dnRecState(rc); }
            if (dnRecLight(rc) != 0u && b > bwL) { bwL = b; r.light = dnRecLight(rc); }
            lo = min(lo, dnRecContent(rc)); hi = max(hi, dnRecContent(rc));
        }
        r.obs = dnMoverObs(mot, r.light, r.state);
        uint gated = 0u;
        for (int k = 0; k < 2; ++k)
            if ((keep & (1u << k)) && dnMoverTapKeep(r.obs, dnRecContent(g[y * N + x0 + k].rec), lo, hi)) gated |= 1u << k;
        if (gated) keep = gated;
        r.blocked = 1.0f;
        for (int k = 0; k < 2; ++k)
            if (keep & (1u << k))
            {
                const uint rc = g[y * N + x0 + k].rec;
                r.blocked = min(r.blocked, dnRecLight(rc) == r.light ? dnRecBlocked(rc) : 0.0f);
            }
        if (!keep) r.blocked = 0.0f;
    }
#else
    (void)mover; (void)mot;
#endif
    Pd t; float w = 0, wv = 0, best = 0;
    for (int k = 0; k < 2; ++k)
    {
        if (!(keep & (1u << k))) continue;
        const int q = x0 + k;
        const float b = k ? f : 1.0f - f;
        const Pd& p = g[y * N + q];
        t.h += b * p.h; t.hv += b * p.hv; t.len += b * p.len; w += b;
        if (p.vis >= 0) { t.vis = (wv > 0 ? t.vis : 0.0f) + b * p.vis; wv += b; }
        if (b > best) { best = b; t.pend = p.pend; }
        t.carry = std::max(t.carry, p.carry);
#ifdef MOVER
        if (mover) { r.content += b * dnRecContent(p.rec); r.evidence += b * dnRecEvidence(p.rec); }
#endif
    }
    if (w < 1e-3f) return Pd();
    t.h /= w; t.hv /= w; t.len /= w;
    if (wv > 0) t.vis /= wv;
    r.content /= w; r.evidence /= w;
    t.init = true;
    return t;
}
// One frame: the producer (K = 2: the block-shared reused reservoir + the fresh pick; MOT w bit 8 any pick
// mover-blocked; the MOVER REPORT as clean_deferred_light.comp writes it) and the temporal pass as main() composes it
// for policy 0 / 1 / 2 / 3. vis(x, y, light): 1 visible, 0 blocked; mover(x, y, light): a MOVING caster blocks it
// (called once per sample: a penumbra may answer at random).
template <class Vis, class Mover>
static Dstat frameD(std::vector<Pd>& g, Vis vis, Mover mover, uint policy)
{
    const float W = T[0] + T[1] + T[2], GAMMA = 1.5f, MAXD = 32.0f;
    const uint pol2 = policy == 3u ? 2u : policy;   // (dnCasterPolicy(): 3 runs every policy-2 rule)
    const bool mv3 = policy == 3u;
    ++g_frame;
    int res[N / BLOCK][N / BLOCK];
    for (auto& r : res) for (int& v : r) v = pick();
    std::vector<float> raw(N * N), ev(N * N);
    std::vector<int> now(N * N);
    std::vector<uint> mot(N * N, 0u);
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const int li[2] = {res[y / BLOCK][x / BLOCK], pick()};
        float r = 0, en = 0, ed = 0;
        bool hit = false, repMover = false;
        uint repLight = 0u;
        for (uint j = 0; j < 2; ++j)
        {
            const bool mb = mover(x, y, li[j]);
            const float v = mb ? 0.0f : vis(x, y, li[j]), c = W * 0.5f;
            r += c * v;
            const float ew = evidenceWeight(true, 2u, j);
            en += c * v * ew; ed += c * ew;
            hit = hit || mb;   // (rt_block_dyn: this ray met the moving caster)
            const bool reached = v > 0.0f;
            if (mb && !reached) { if (!repMover) { repLight = uint(li[j]) + 1u; repMover = true; } }
            else if (reached && !repMover && (repLight == 0u || ((g_frame + uint(x) + uint(y) + j) & 1u) != 0u))
                repLight = uint(li[j]) + 1u;
        }
        raw[y * N + x] = r;
        ev[y * N + x] = ed > 1e-8f ? pdnVisibilityValue(pdnVisibilityCode(en / ed)) : -1.0f;
        now[y * N + x] = hit ? 1 : 0;
#ifdef MOVER
        mot[y * N + x] = pdnMotReportBits(repLight, !repMover);
#else
        (void)repLight; (void)repMover;
#endif
    }
    Dstat st;
    std::vector<Pd> out = g;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const int i = y * N + x;
        Pd& o = out[i];
        const float rD = raw[i];
        float s = 0, s2 = 0, n = 0, sd = 0, sd2 = 0, seed = 0, seedN = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            const int qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= N || qy >= N) continue;
            const float e = ev[qy * N + qx], q = raw[qy * N + qx];
            sd += q; sd2 += q * q;
#ifdef MOVER
            if (!pdnMotReportMover(mot[qy * N + qx])) { seed += q; seedN += 1; }   // (the reveal's seed)
#endif
            if (e < 0) continue;
            s += e; s2 += e * e; n += 1;
        }
        float cntD = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            const int qx = x + dx, qy = y + dy;
            if (qx >= 0 && qy >= 0 && qx < N && qy < N) cntD += 1;
        }
        const float mD = sd / cntD, sigD0 = sqrt(max(sd2 / cntD - mD * mD, 0.0f));
        const float visMean = n > 0 ? s / n : 0, visSigma = n > 0 ? sqrt(max(s2 / n - visMean * visMean, 0.0f)) : 0;
        const bool casterNow = now[i] != 0;
        Rec R;
        const Pd P = fetchD(g, float(x) + g_scroll, y, mv3, mot[i], R);
        if (!P.init)
        {
            o.h = rD; o.hv = 0; o.len = 1; o.vis = storePrior(evidence(ev[i], visMean, n)); o.init = true;
            o.carry = casterNow ? 1u : 0u;
#ifdef MOVER
            o.rec = mv3 ? dnMoverNext(false, mot[i], 0u, 0.0f, 0.0f, 0.0f, 0u, true, false, 1.0f, dither()) : 0u;
#endif
            continue;
        }
        const float prior = pol2 == 2u ? P.vis : -1.0f, ld = P.len;
        const bool casterAffected = casterNow || P.carry > 0u;
        // ---- the caster test (policy 2: edgeV) / the old colour test (policy 1) ----
        const bool testV = casterAffected && n > 0;
        const bool dropShape = pol2 == 2u && n > 0 && dnDropShape(visMean, prior, n, DN_DROP_MIN_N);
        const bool testDrop = !testV && dropShape;
        uint pendV = 0u;
        bool changed = false, edgeV = false;
        if (pol2 == 2u)
        {
            const float noiseV = dnCasterSamplingNoise(prior, visMean, n, n, ld);
            const float curV = dnCasterCurrent(ev[i], visMean, false);
            pendV = (testV || testDrop) ? dnCasterPending(curV, visMean, prior, visSigma, noiseV) : 0u;
            edgeV = (testV && (dnCasterVisibilityChange(curV, visMean, prior, visSigma, noiseV)
                               || dnCasterConfirmed(pendV, P.pend)))
                 || (testDrop && dnCasterConfirmed(pendV, P.pend));
            changed = edgeV;
        }
        else if (pol2 == 1u)
            changed = casterAffected && dnCasterChange(rD, mD, P.h, sigD0);
        // ---- policy 3: THE REVEAL (own sample of the record's light + the neighbours' transitions) ----
        float mvEv = 0.0f;
        bool reveal = false;
#ifdef MOVER
        if (mv3)
        {
            mvEv = dnMoverEvidence(mot[i], R.light, R.blocked, R.evidence, 0.0f);
            const bool open = R.light != 0u && R.blocked >= DN_MOVER_MIN_BLOCKED
                           && !(pdnMotReportLight(mot[i]) == R.light && !pdnMotReportVisible(mot[i]));
            if (open && !dnMoverRevealed(mvEv))
            {
                float sup = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                {
                    const int qx = x + dx, qy = y + dy;
                    if ((!dx && !dy) || qx < 0 || qy < 0 || qx >= N || qy >= N) continue;
                    const uint qm = mot[qy * N + qx];
                    if (!pdnMotReportVisible(qm) || pdnMotReportLight(qm) != R.light) continue;
                    // its own footprint (q + the centre's offset): every tap covered umbra of the light
                    const float hq = float(qx) + g_scroll;
                    const int h0 = int(std::floor(hq));
                    const float fq = hq - float(h0);
                    bool ok = true, seen = false;
                    for (int k = 0; k < 2 && ok; ++k)
                    {
                        if ((k ? fq : 1.0f - fq) <= 1e-4f) continue;
                        const int hx = h0 + k;
                        if (hx < 0 || hx >= N) { ok = false; continue; }
                        ok = dnMoverSupportTap(g[qy * N + hx].rec, R.light);
                        seen = true;
                    }
                    if (ok && seen) sup += 1.0f;
                }
                mvEv = dnMoverEvidence(mot[i], R.light, R.blocked, R.evidence, sup);
            }
            reveal = dnMoverRevealed(mvEv);
            changed = changed || reveal;   // (casterChangeD = edgeV || mvReveal)
            if (reveal) ++st.reveals;
        }
#endif
        float lenD = P.len, hD = P.h;
        if (changed) { lenD = 0.0f; hD = rD; ++st.resets; }
        // ---- the clamp (DN_F_RADIANCE: sigma floored at the history's spread) ----
        const float sig = max(sigD0, sqrt(max(P.hv, 0.0f)));
        const float lo = max(mD - GAMMA * sig, 0.0f), hi = mD + GAMMA * sig;
        const float hc = clamp(hD, lo, hi);
        const float cut = abs(hc - hD) / max(hD + mD, 1e-4f);
        if (cut > 0.05f) lenD = min(lenD, mix(8.0f, 1.0f, clamp((cut - 0.05f) * 3.0f, 0.0f, 1.0f)));
        hD = hc;
        // ---- length, accumulation, variance ----
        float maxD = MAXD;
        const bool follow = dnCasterFollow(testV ? pendV : 0u, ld);
        if (dnCasterShouldCap(pol2, casterAffected, changed || follow)) maxD = dnCasterHistoryCap(maxD, htStep());
        const float nD = htAdvance(lenD, maxD);
        const float aD = lenD > 0.0f ? htAlpha(1.0f / max(nD, 1.0f)) : 1.0f;
        o.h = mix(hD, rD, aD);
        // (policy 3) a reveal restarts from the 3x3 texels no mover blocked
        if (reveal && !edgeV && lenD <= 0.0f && seedN > 0) o.h = seed / seedN;
        const float aV = htAlpha(1.0f / max(nD, 1.0f));
        float var = (1.0f - aV) * (P.hv + aV * (rD - hD) * (rD - hD));
        if (nD < 4.0f) var = max(var, sigD0 * sigD0);
        o.hv = var;
        o.len = nD;
        o.vis = storePrior(dnCasterVisibilityAcc(P.vis, evidence(ev[i], visMean, n),
                                                 changed ? 1.0f : dnCasterEvidenceAlpha(ld)));
        const bool dropMark = (changed && dropShape) || dnDropMarkCarry(P.pend, nD, pendV);
        o.pend = (changed ? 0u : pendV) | (dropMark ? DN_DROP_MARK : 0u);
#ifdef CARRY
        o.carry = dnCasterCarry(pol2, casterNow, P.carry,
                                testV && !changed && pol2 == 2u && dnCasterDeviating(visMean, prior, dnCasterSamplingNoise(prior, visMean, n, n, ld)));
#else
        o.carry = casterNow ? 1u : 0u;   // (bit 11: this frame's tag, read back next frame only)
#endif
#ifdef MOVER
        o.rec = mv3 ? dnMoverNext(true, mot[i], R.light, R.blocked, R.content, mvEv, R.obs, changed, reveal, aD, dither()) : 0u;
#else
        (void)mvEv;
#endif
    }
    g.swap(out);
    return st;
}
#endif

int main()
{
    int bad = 0;
    // (a) A visible, B blocked, C blocked: the truth is A alone (1). Detector vs no detector (the control's
    // frame-to-frame rms is what a converged 32-frame history of these picks gives).
    for (float fps : {60.0f, 30.0f, 144.0f})
    {
    testStep = 60.0f / fps;
    double rmsC = 0;
    for (int det = 0; det <= 1; ++det)
    {
        std::vector<Px> g(N * N);
        auto mixed = [](int, int, int l) { return l == 0 ? 1.0f : 0.0f; };
        for (int f = 0; f < 128; ++f) frame(g, mixed, det != 0);
        double fires = 0, sum = 0, d2 = 0, cnt = 0, pv = 0, pv2 = 0;
        std::vector<float> prev(N * N);
        for (int i = 0; i < N * N; ++i) prev[i] = g[i].acc;
        for (int f = 0; f < 2048; ++f)
        {
            fires += frame(g, mixed, det != 0);
            for (int i = 0; i < N * N; ++i)
            {
                sum += g[i].acc; d2 += (g[i].acc - prev[i]) * (g[i].acc - prev[i]); prev[i] = g[i].acc; cnt += 1;
                pv += g[i].vis; pv2 += g[i].vis * g[i].vis;
            }
        }
        const double rate = fires / cnt, mean = sum / cnt, rms = sqrt(d2 / cnt), pm = pv / cnt, ps = sqrt(max(float(pv2 / cnt - pm * pm), 0.0f));
        if (!det) { rmsC = rms; continue; }
        std::printf("(a) %g Hz: one light visible, one blocked, moving-caster tagged: resets %.5f / texel-frame, mean %.3f "
                    "(the visible light alone: 1), frame-to-frame rms %.4f (no detector %.4f); evidence prior %.3f +- %.3f\n",
                    fps, rate, mean, rms, rmsC, pm, ps);
        // (resets per texel-SECOND: the per-frame test sees the same evidence at any rate, so its false rate
        // per second grows with the frame rate -- bounded at 0.1 / s, 144 Hz included)
        if (rate * fps > 0.1) { std::printf("FAIL (a): pick noise resets the history (%.3f / texel-second > 0.1)\n", rate * fps); bad = 1; }
        if (abs(mean - 1.0) > 0.03) { std::printf("FAIL (a): mean %.3f != the visible light alone (1)\n", mean); bad = 1; }
        if (fps == 60.0f && rms > 2.0 * rmsC) { std::printf("FAIL (a): frame-to-frame rms %.4f > 2 x converged %.4f (crawling)\n", rms, rmsC); bad = 1; }
    }
    }
    testStep = 1.0f;
    // (b) / (c): a caster's shadow edge sweeps in from x = 0 (b: blocking light B on a lit floor, c: blocking the
    // visible light A on the mixed floor of (a)), then leaves the same way, at 0.5 and 2 texels per frame. The
    // lag: the signed mean error (the bias toward the old state) of the texels whose 3x3 the edge left >= 2
    // frames ago, relative to the change. INTERIOR = full 3x3 (the pass / fail number); EDGE ROWS = 6
    // same-surface neighbours (a silhouette's / thin surface's lower test power), reported and bounded looser.
    for (int scen = 0; scen < 2; ++scen)
        for (float speed : {0.5f, 2.0f})
            for (int det = 1; det >= 0; --det)
            {
                const int blocked = scen == 0 ? 1 : 0, F = int(float(N + 4) / speed) + 1, RUNS = 64;
                rng = 0x9E3779B9u + uint(scen * 4 + int(speed));   // (each case its own fixed seed)
                const float before = scen == 0 ? 2.0f : 1.0f, after = scen == 0 ? 1.0f : 0.0f;
                // per (phase, frame): summed error and count over RUNS independent runs (a single run's young
                // histories make the bias of a few columns a noisy number)
                std::vector<double> eI(2 * F, 0.0), nI(2 * F, 0.0), eE(2 * F, 0.0), nE(2 * F, 0.0);
                for (int run = 0; run < RUNS; ++run)
                {
                    std::vector<Px> g(N * N);
                    for (int f = 0; f < 192; ++f)
                        frame(g, [&](int, int, int l) { return scen == 0 ? (l < 2 ? 1.0f : 0.0f) : (l == 0 ? 1.0f : 0.0f); }, det != 0);
                    for (int phase = 0; phase < 2; ++phase)
                        for (int f = 0; f < F; ++f)
                        {
                            const float edge = speed * float(f + 1);   // texels x < edge: shadowed (phase 0) / freed (1)
                            auto vis = [&](int x, int, int l) {
                                if (l == 2 || (scen == 1 && l == 1)) return 0.0f;
                                const bool in = phase == 0 ? float(x) < edge : float(x) >= edge;
                                return (in && l == blocked) ? 0.0f : 1.0f;
                            };
                            frame(g, vis, det != 0);
                            const int k = phase * F + f;
                            for (int y = 0; y < N; ++y) for (int x = 1; x < N - 1; ++x)
                            {
                                if (float(x) + 1.0f + 2.0f * speed >= edge) continue;   // the 3x3 left >= 2 frames ago
                                const float e = g[y * N + x].acc - (phase == 0 ? after : before);
                                if (y == 0 || y == N - 1) { eE[k] += e; nE[k] += 1; } else { eI[k] += e; nI[k] += 1; }
                            }
                        }
                }
                double worstI = 0, worstE = 0;
                for (int k = 0; k < 2 * F; ++k)
                {
                    if (nI[k] > 0) worstI = std::max(worstI, abs(eI[k]) / nI[k] / abs(before - after));
                    if (nE[k] > 0) worstE = std::max(worstE, abs(eE[k]) / nE[k] / abs(before - after));
                }
                const char c = scen == 0 ? 'b' : 'c';
                std::printf("(%c) %s, %.1f texel / frame: worst relative bias behind the edge: interior %.3f, edge rows %.3f\n",
                            c, det ? "detector" : "no detector (control: must trail)", speed, worstI, worstE);
                if (det && worstI > 0.2) { std::printf("FAIL (%c): the moving caster's shadow trails (interior)\n", c); bad = 1; }
                if (det && worstE > 0.6) { std::printf("FAIL (%c): the moving caster's shadow trails (edge rows)\n", c); bad = 1; }
                if (!det && worstI < 0.3) { std::printf("FAIL (%c): the control does not trail: the lag case tests nothing\n", c); bad = 1; }
            }
    // (d) THE MOVING-EDGE TRAIL, real tags: a BAND_W-texel shadow band (an avatar's) crosses the floor in +x at
    // 0.25 / 0.5 / 1 / 2 texels per frame. (d1) a floor lit by A and B, the band blocks B (evidence ~1 -> ~0.5 and
    // back); (d2) the mixed floor of (a) (A visible, B behind a wall), the band blocks A (evidence ~0.5 -> 0 and
    // back). Only the rays that meet the band's caster tag their texel. TRAILING: the texels the band left (their
    // 3x3 freed >= 3 frames ago) must be back within 0.2 of the change (the owner's ghost trail behind a moving
    // shadow); LEADING: the texels inside the band (3x3 covered >= 3 frames ago), the same bound. Policy 1 (the broad
    // cap) and 0 (none) are reported for reference; 0 must lag on one side, so the bound means something. Interior
    // rows / columns only. Before THE MOVER CARRY: (d1) trailing 0.74 / 0.61 / 0.33 at 0.5 / 1 / 2 texels per frame.
    // (g) REVEAL SPREAD on the same runs: a revealed column's rows share one truth; in its first 6 frames the std of
    // the displayed history across them, relative to the step -- light filling in as points (each texel at its own
    // frame) is a large spread, a region filling in together a small one.
#ifdef NEW_POLICY
#ifdef MOVER
    const std::vector<uint> pols = {1u, 2u, 3u, 0u};
#else
    const std::vector<uint> pols = {1u, 2u, 0u};
#endif
    const int BAND_W = 12;
    for (int scen = 0; scen < 2; ++scen)
        for (float speed : {0.25f, 0.5f, 1.0f, 2.0f})
        {
            double spread1 = 0;   // (policy 1's reveal spread: the reference)
            for (uint pol : pols)
            {
                const int F = int(float(N + BAND_W + 8) / speed) + 1, RUNS = 32;
                rng = 0xC0FFEEu + uint(scen * 16) + uint(speed * 4.0f);
                const int moverLight = scen == 0 ? 1 : 0;
                const float lit = scen == 0 ? 2.0f : 1.0f, shad = scen == 0 ? 1.0f : 0.0f;
                std::vector<double> eT(F, 0.0), nT(F, 0.0), eL(F, 0.0), nL(F, 0.0);
                double spS = 0, spN = 0;
                for (int run = 0; run < RUNS; ++run)
                {
                    std::vector<Pd> g(N * N);
                    auto base = [&](int, int, int l) { return l == 2 || (scen == 1 && l == 1) ? 0.0f : 1.0f; };
                    auto none = [](int, int, int) { return false; };
                    for (int f = 0; f < 192; ++f) frameD(g, base, none, pol);
                    std::vector<int> colIn(N, 0), colExit(N, -1);
                    for (int f = 0; f < F; ++f)
                    {
                        const float lead = speed * float(f + 1) - 4.0f, trail = lead - float(BAND_W);   // band: [trail, lead)
                        auto inBand = [&](int x) { return float(x) >= trail && float(x) < lead; };
                        auto mover = [&](int x, int, int l) { return l == moverLight && inBand(x); };
                        auto vis = [&](int x, int y, int l) { return mover(x, y, l) ? 0.0f : base(x, y, l); };
                        frameD(g, vis, mover, pol);
                        for (int x = 1; x < N - 1; ++x)
                        {
                            if (inBand(x)) colIn[x] = 1;
                            else if (colIn[x] && colExit[x] < 0) colExit[x] = f;
                            if (colExit[x] >= 0 && f - colExit[x] < 6)
                            {
                                double a = 0, a2 = 0;
                                for (int y = 1; y < N - 1; ++y) { const double h = g[y * N + x].h; a += h; a2 += h * h; }
                                const double m = a / (N - 2);
                                spS += sqrt(std::max(a2 / (N - 2) - m * m, 0.0)) / (lit - shad); spN += 1;
                            }
                        }
                        for (int y = 1; y < N - 1; ++y) for (int x = 1; x < N - 1; ++x)
                        {
                            const float h = g[y * N + x].h;
                            if (float(x) + 1.0f + 3.0f * speed < trail) { eT[f] += h - lit; nT[f] += 1; }
                            else if (float(x) - 1.0f >= trail && float(x) + 1.0f + 3.0f * speed < lead) { eL[f] += h - shad; nL[f] += 1; }
                        }
                    }
                }
                const double spread = spN > 0 ? spS / spN : 0.0;
                if (pol == 1u) spread1 = spread;
                double wT = 0, wL = 0;
                for (int f = 0; f < F; ++f)
                {
                    if (nT[f] > 0) wT = std::max(wT, abs(eT[f]) / nT[f] / (lit - shad));
                    if (nL[f] > 0) wL = std::max(wL, abs(eL[f]) / nL[f] / (lit - shad));
                }
                std::printf("(d%d) policy %u, %.2f texel / frame: worst relative bias, trailing (the band left) %.3f, "
                            "leading (inside the band) %.3f; (g) reveal spread %.3f (%.2f x policy 1)\n", scen + 1, pol, speed,
                            wT, wL, spread, spread1 > 0 ? spread / spread1 : 0.0);
                if (pol == 2u || pol == 3u)
                {
                    if (wT > 0.2) { std::printf("FAIL (d%d): policy %u: the moving shadow leaves a trail behind it (%.3f > 0.2)\n", scen + 1, pol, wT); bad = 1; }
                    if (wL > 0.2) { std::printf("FAIL (d%d): policy %u: the moving shadow arrives late (%.3f > 0.2)\n", scen + 1, pol, wL); bad = 1; }
                }
                // (g) policy 3 (the default): the revealed light fills in as a region -- on the lit floor within 1.6 x
                // policy 1's spread (whose two-frame cap resets every row the frame the band leaves), on the mixed
                // floor no worse than policy 1 (5 %). Policy 2 is reported: 0.19-0.29 on the lit floor (1.5-2.1 x).
                if (pol == 3u && scen == 0 && spread > 1.6 * spread1)
                { std::printf("FAIL (g1): the revealed light fills in point by point (spread %.3f > 1.6 x policy 1's %.3f)\n", spread, spread1); bad = 1; }
                if (pol == 3u && scen == 1 && spread > 1.05 * spread1)
                { std::printf("FAIL (g2): the revealed light fills in point by point (spread %.3f > policy 1's %.3f)\n", spread, spread1); bad = 1; }
                if (pol == 0u && std::max(wT, wL) < 0.3) { std::printf("FAIL (d%d): the no-detector control does not trail: the case tests nothing\n", scen + 1); bad = 1; }
            }
        }
    // (f) CAMERA FOLLOW: the shadow band [10, 20) stays put on screen while the floor scrolls under it (the owner
    // walking his avatar, the camera behind it) at 0.25 / 0.5 / 0.75 texels per frame -- fractional reprojection, a
    // bilinear history every frame. Same lights as (d1) / (d2); the bias of the floor that left the band >= 3 frames
    // ago (trailing) and of the band's interior (leading), averaged over 300 settled frames x 8 runs. Policy 3 (THE
    // GATE: the taps on this texel's side of the mover edge): <= 0.08. Policy 2 lags 0.25-0.33 (reported), policy 1
    // 0.03-0.05, no detector 0.25-0.76 (must lag).
    for (int scen = 0; scen < 2; ++scen)
        for (float speed : {0.25f, 0.5f, 0.75f})
            for (uint pol : pols)
            {
                rng = 0xF011u + uint(scen * 16) + uint(speed * 4.0f);
                const int moverLight = scen == 0 ? 1 : 0;
                const float lit = scen == 0 ? 2.0f : 1.0f, shad = scen == 0 ? 1.0f : 0.0f, A = 10.0f, B = 20.0f;
                double eT = 0, nT = 0, eL = 0, nL = 0;
                for (int run = 0; run < 8; ++run)
                {
                    std::vector<Pd> g(N * N);
                    auto base = [&](int, int, int l) { return l == 2 || (scen == 1 && l == 1) ? 0.0f : 1.0f; };
                    auto mover = [&](int x, int, int l) { return l == moverLight && float(x) >= A && float(x) < B; };
                    auto vis = [&](int x, int y, int l) { return mover(x, y, l) ? 0.0f : base(x, y, l); };
                    g_scroll = 0.0f;
                    for (int f = 0; f < 192; ++f) frameD(g, vis, mover, pol);
                    g_scroll = speed;
                    for (int f = 0; f < 348; ++f)
                    {
                        frameD(g, vis, mover, pol);
                        if (f < 48) continue;
                        for (int y = 1; y < N - 1; ++y) for (int x = 1; x < N - 1; ++x)
                        {
                            const float h = g[y * N + x].h;
                            if (float(x) + 1.0f + 3.0f * speed < A) { eT += h - lit; nT += 1; }
                            else if (float(x) - 1.0f >= A && float(x) + 1.0f + 3.0f * speed < B) { eL += h - shad; nL += 1; }
                        }
                    }
                }
                g_scroll = 0.0f;
                const double wT = abs(eT) / max(float(nT), 1.0f) / (lit - shad), wL = abs(eL) / max(float(nL), 1.0f) / (lit - shad);
                std::printf("(f%d) camera follow, policy %u, %.2f texel / frame: relative bias, trailing %.3f, leading %.3f\n",
                            scen + 1, pol, speed, wT, wL);
                if (pol == 3u && std::max(wT, wL) > 0.08)
                { std::printf("FAIL (f%d): under camera motion the shadow lags (trailing %.3f, leading %.3f > 0.08)\n", scen + 1, wT, wL); bad = 1; }
                if (pol == 0u && std::max(wT, wL) < 0.3) { std::printf("FAIL (f%d): the no-detector control does not lag: the case tests nothing\n", scen + 1); bad = 1; }
            }
    // (e) THE IDLE-ANIMATING AVATAR under the full composition: the mixed light of (a), every texel tagged on the
    // frames its picks meet the (static-in-place, animating) caster -- light B, blocked -- so tags come and go with
    // the picks, as on an avatar. Policies 2 and 3 must keep (a)'s bounds; policy 1 (the broad cap) is reported: its
    // noise is what the owner sees as crawling. (e2) its PENUMBRA: B mover-blocked on half its samples, else visible
    // (an area light's soft shadow, hair breathing in it) -- policy 3's reveal must not fire on the visible half
    // (<= 0.05 / texel-second) and the rms bound holds.
    for (int pen = 0; pen < 2; ++pen)
        for (float fps : {60.0f, 144.0f})
        {
            testStep = 60.0f / fps;
            double rms[4] = {0, 0, 0, 0};
            int rs[4] = {0, 0, 0, 0}, rv3 = 0;
            double cnt = 0;
            for (uint pol : pols)
            {
                rng = 0xA11CEu + uint(fps) + uint(pen * 7);
                std::vector<Pd> g(N * N);
                auto mixed = [&](int, int, int l) { return l == 0 ? 1.0f : (pen && l == 1 ? 1.0f : 0.0f); };
                auto tagB = [&](int, int, int l) { return l == 1 && (!pen || rnd() < 0.5f); };
                for (int f = 0; f < 128; ++f) frameD(g, mixed, tagB, pol);
                std::vector<float> prev(N * N);
                for (int i = 0; i < N * N; ++i) prev[i] = g[i].h;
                double d2 = 0; cnt = 0;
                for (int f = 0; f < 2048; ++f)
                {
                    const Dstat d = frameD(g, mixed, tagB, pol);
                    rs[pol] += d.resets;
                    if (pol == 3u) rv3 += d.reveals;
                    for (int i = 0; i < N * N; ++i) { d2 += (g[i].h - prev[i]) * (g[i].h - prev[i]); prev[i] = g[i].h; cnt += 1; }
                }
                rms[pol] = sqrt(d2 / cnt);
            }
            const char* tag = pen ? "(e2)" : "(e)";
            std::printf("%s %g Hz idle-animating%s, real tags: frame-to-frame rms policy 3 %.4f, policy 2 %.4f, policy 1 %.4f, "
                        "none %.4f; resets / texel-second policy 3 %.4f (reveals %.4f), policy 2 %.4f\n", tag, fps,
                        pen ? " (penumbra)" : "", rms[3], rms[2], rms[1], rms[0], rs[3] / cnt * fps, rv3 / cnt * fps,
                        rs[2] / cnt * fps);
            for (uint pol : {2u, 3u})
            {
#ifndef MOVER
                if (pol == 3u) continue;
#endif
                if (rs[pol] / cnt * fps > 0.1 + (pen ? 0.05 : 0.0))
                { std::printf("FAIL %s: policy %u: pick noise resets the history (%.3f / texel-second)\n", tag, pol, rs[pol] / cnt * fps); bad = 1; }
                if (fps == 60.0f && rms[pol] > 2.0 * rms[0])
                { std::printf("FAIL %s: policy %u: frame-to-frame rms %.4f > 2 x converged %.4f (crawling)\n", tag, pol, rms[pol], rms[0]); bad = 1; }
            }
            if (rv3 / cnt * fps > 0.05) { std::printf("FAIL %s: the mover record's reveal fires on steady light (%.4f / texel-second)\n", tag, rv3 / cnt * fps); bad = 1; }
        }
    testStep = 1.0f;
#ifdef MOVER
    // (h) A STATIC SHADOW EDGE of an idle-animating mover (a band blocks B on the lit floor, real tags), the camera
    // still (pan 0) or panning over the static scene (the band fixed on the floor, the floor scrolling 0.25 / 0.5 /
    // 0.75 texel / frame): the rmse against the truth on the edge strips and the resets. Policy 3 (THE GATE takes the
    // taps on this texel's side of the edge) must not do worse than policy 2 there: still, within 5 % (no reveal
    // fires: the outside texels were never covered); panning at 0.25 / 0.75, at most policy 2's rmse. Pan 0.5 is an
    // exact half-texel tie every frame (the heaviest tap is a coin toss): reported.
    for (float pan : {0.0f, 0.25f, 0.5f, 0.75f})
    {
        double rm[4] = {0, 0, 0, 0}, rr[4] = {0, 0, 0, 0};
        for (uint pol : {2u, 3u})
        {
            rng = 0xED6Eu;
            double e2s = 0, cnt = 0; long rs = 0, fr = 0;
            float off = 0.0f;   // (the floor's offset in screen texels: the band moves with it)
            auto inBand = [&](int x) { const float u = float(x) + off; return u >= 8.0f && u < 16.0f; };
            auto base = [&](int, int, int l) { return l == 2 ? 0.0f : 1.0f; };
            auto mover = [&](int x, int, int l) { return l == 1 && inBand(x); };
            auto vis = [&](int x, int y, int l) { return mover(x, y, l) ? 0.0f : base(x, y, l); };
            for (int seg = 0; seg < (pan > 0.0f ? 48 : 1); ++seg)
            {
                std::vector<Pd> g(N * N);
                off = 0.0f; g_scroll = 0.0f;
                for (int f = 0; f < 160; ++f) frameD(g, vis, mover, pol);
                g_scroll = pan;
                const int F = pan > 0.0f ? int(6.0f / pan) : 1024;
                for (int f = 0; f < F; ++f)
                {
                    off = pan * float(f + 1);
                    rs += frameD(g, vis, mover, pol).resets; ++fr;
                    for (int y = 1; y < N - 1; ++y) for (int x = 1; x < N - 1; ++x)
                    {
                        const float u = float(x) + off;
                        if (!((u >= 6.0f && u < 10.0f) || (u >= 14.0f && u < 18.0f))) continue;
                        const float e = g[y * N + x].h - (inBand(x) ? 1.0f : 2.0f);
                        e2s += e * e; cnt += 1;
                    }
                }
            }
            g_scroll = 0.0f;
            rm[pol] = sqrt(e2s / cnt); rr[pol] = rs / (double(fr) * N * N);
        }
        std::printf("(h) static mover edge, pan %.2f texel / frame: edge-strip rmse policy 3 %.4f, policy 2 %.4f; "
                    "resets / texel-frame %.5f, %.5f\n", pan, rm[3], rm[2], rr[3], rr[2]);
        if (pan == 0.0f && (rm[3] > 1.05 * rm[2] || rr[3] > 1.5 * rr[2] + 1e-5))
        { std::printf("FAIL (h): policy 3 disturbs a static mover edge\n"); bad = 1; }
        if ((pan == 0.25f || pan == 0.75f) && rm[3] > rm[2])
        { std::printf("FAIL (h): panning over a static mover edge, policy 3's rmse %.4f > policy 2's %.4f\n", rm[3], rm[2]); bad = 1; }
    }
#endif
#endif
    return bad;
}
"""


def _wiring(read):
    """The producer / temporal wiring the replay composes but cannot see: [] when it holds."""
    missing = []
    c = read("indra/vkclean/shaders/clean_deferred_light.comp")
    t = read("indra/vkclean/shaders/clean_dn_temporal.comp")
    if not re.search(r"const float ew = wgt \* pdnEvidenceWeight\(reused, sel\.Ke, si - s0\);", c) \
            or "evN += dt * vis * ew; evD += dt * ew;" not in c or "pdnSampleVisibility(evN, evD) << 9u" not in c:
        missing.append("clean_deferred_light.comp: MOT w's evidence is not the independent samples' (pdnEvidenceWeight)")
    # (RenderVKShadowMoverMask 2: a mover-exact texel's sum over every light is deterministic evidence too -- sel.allL)
    if not re.search(r"\(sel\.exact(?: \|\| sel\.allL)? \? PDN_MOT_EVIDENCE_EXACT : 0u\)", c):
        missing.append("clean_deferred_light.comp: MOT w does not mark exact (deterministic) evidence")
    if "visStoch += qExact ? 0.0 : 1.0" not in t \
            or "dnCasterSamplingNoise(receiverHistory.visibility, visMean, visStoch, visN, receiverHistory.ld)" not in t:
        missing.append("clean_dn_temporal.comp: the caster test does not take the 3x3's sampling noise")
    if not re.search(r"\|\| dnCasterConfirmed\(pendV, receiverHistory\.pending\)\)", t) \
            or "((casterChangeD ? 0u : pendV) << PDN_META_PEND_SHIFT)" not in t:
        missing.append("clean_dn_temporal.comp: the pending deviation is not stored / confirmed across frames")
    if "dnCasterEvidence(ownVisibility, visMean, visStoch)," not in t or "pdnMetaVisibilityCode(visibility)" not in t \
            or "dnCasterEvidenceAlpha(receiverHistory.ld)" not in t:
        missing.append("clean_dn_temporal.comp: the 10-bit evidence prior does not accumulate dnCasterEvidence")
    if "casterChangeD || follow" not in t or "dnCasterPolicy() == 2u ? edgeV" not in t:
        missing.append("clean_dn_temporal.comp: the caster test's result does not reach both channels' cap")
    if _function(t, "dnCasterCarry") is None \
            or "t.caster = max(t.caster, float(pdnMetaCarry(g.meta)));" not in t \
            or "t.caster = float(pdnMetaCarry(bmeta));" not in t \
            or "casterNow || (have && h.caster > 0.0)" not in t \
            or "testV && !casterChangeD && dnCasterDeviating(visMean, receiverHistory.visibility, noiseV));" not in t \
            or "| pdnMetaCarryBits(carry) |" not in t:
        missing.append("clean_dn_temporal.comp: the mover carry (dnCasterCarry) is not read / held / stored")
    p = read("indra/vkclean/shaders/clean_pdn_common.glsl")
    if "& 0xFFF0u) << 16u)" not in p or "unpackHalf2x16((w >> 16u) & 0xFFF0u)" not in p:
        missing.append("clean_pdn_common.glsl: W's packing overlaps the mover carry's bits 18-19")
    if _function(t, "dnMoverNext") is not None:
        # THE MOVER RECORD: the producer's report, the gated fetch, the reveal routed through casterChangeD, its seed,
        # the record stored in the age word y (and kept by the sun channel's rewrite), the policy plumbing
        if "if (rvMover && !reached)" not in c or "| pdnMotReportBits(repLight, !repMover) : 0u)" not in c:
            missing.append("clean_deferred_light.comp: the MOVER REPORT (MOT w bits 21-29) is not written")
        if "dnMoverTapKeep(t.mvObs, dnRecContent(recs[k]), lo, hi)" not in t \
                or "dnFetchHistory(hc, par ^ 1u, pw, g.n, 0.85, 0.04, h, hw, true, mo.w, true)" not in t \
                or "length(hv - hc) <= 0.5, mo.w, false)" not in t:
            missing.append("clean_dn_temporal.comp: THE GATE is not applied to (only) the receiver's history fetch")
        if "dnCasterPolicy() == 2u ? edgeV || mvReveal" not in t \
                or "dnMoverSupport(m, par, hc - vec2(m), light, mvSame)" not in t \
                or "accD = mvSeedD / mvSeedND" not in t:
            missing.append("clean_dn_temporal.comp: THE REVEAL does not reset (casterChangeD) / take support / seed")
        if "uvec4(dnPackAges(nD * HT_REF_MS, nS * HT_REF_MS), mvRec, 0u, 0u)" not in t \
                or "uvec4(localWord, floatBitsToUint(slen * HT_REF_MS)" not in t \
                or "receiverHistory.mvContent, mvEvidence, receiverHistory.mvObs, casterChangeD, mvReveal, aD," not in t:
            missing.append("clean_dn_temporal.comp: the mover record is not stored in / kept in the age word")
        if "const bool radSelfMoving = dnMoverRecordOn() && (currentRiggedReceiver || surfMoved);" not in t \
                or "nnD >= DN_DROP_MIN_N && !radSelfMoving" not in t:
            missing.append("clean_dn_temporal.comp: policy 3 runs THE RADIANCE DROP on self-moving (rigged) receivers")
        d = read("indra/vkclean/shaders/clean_dn_common.glsl")
        if "uint dnCasterPolicy() { return min(dnCasterPolicyRaw(), 2u); }" not in d or "r.rec = aw.y;" not in d:
            missing.append("clean_dn_common.glsl: policy 3 does not run policy 2's rules / the record is not read")
        h = read("indra/newview/vkgfx/llcleandenoisevk.cpp")
        s = read("indra/newview/vkgfx/llcleanprojstochvk.cpp")
        if "std::min(p.casterHistoryPolicy, 3u) << 7u" not in h or "llclamp((S32)s_casterPolicy, 0, 3)" not in s:
            missing.append("host: RenderVKShadowCasterHistory 3 does not reach the temporal pass")
    return missing


def assemble(read):
    """(fixture C++ source, new policy?) from the production helpers; None when one is missing."""
    temporal = read("indra/vkclean/shaders/clean_dn_temporal.comp")
    common = read("indra/vkclean/shaders/clean_pdn_common.glsl")
    htime = read("indra/vkclean/shaders/clean_history_time.glsl")
    new = _function(temporal, "dnCasterSamplingNoise") is not None and _function(common, "pdnEvidenceWeight") is not None
    names = [(temporal, n) for n in ("dnCasterChange", "dnCasterVisibilityChange", "dnCasterVisibilityAcc",
                                     "dnCasterShouldCap", "dnCasterHistoryCap")]
    names += [(common, n) for n in ("pdnVisibilityCode", "pdnVisibilityValue")]
    names += [(htime, n) for n in ("htAlpha", "htAdvance")]
    if new:
        names += [(temporal, n) for n in ("dnCasterEvidenceAlpha", "dnCasterSamplingNoise", "dnCasterPending",
                                          "dnCasterConfirmed", "dnCasterFollow", "dnCasterCurrent", "dnCasterEvidence")]
        names += [(common, n) for n in ("pdnEvidenceWeight", "pdnMetaVisibilityCode", "pdnMetaVisibility")]
        names += [(temporal, n) for n in ("dnDropShape", "dnDropMarkCarry")]
    carry = new and _function(temporal, "dnCasterCarry") is not None
    if carry:
        names += [(temporal, "dnCasterDeviating"), (temporal, "dnCasterCarry")]
    mover = carry and _function(temporal, "dnMoverNext") is not None
    if mover:
        names += [(common, n) for n in ("pdnMotReportBits", "pdnMotReportLight", "pdnMotReportVisible", "pdnMotReportMover")]
        names += [(temporal, n) for n in ("dnRecLight", "dnRecBlocked", "dnRecState", "dnRecContent", "dnRecEvidence",
                                          "dnRecPack", "dnMoverObs", "dnMoverTapKeep", "dnMoverStep", "dnMoverEvidence",
                                          "dnMoverRevealed", "dnMoverSupportTap", "dnMoverNext")]
    parts = re.findall(r"#define DN_(?:CASTER|DROP|MOVER)_\w+ [^\n]+", temporal)
    parts += re.findall(r"#define PDN_MOT_REPORT_\w+ [^\n]+", common)
    for src, n in names:
        fn = _function(src, n)
        if fn is None:
            return None, n
        parts.append("static " + fn)
    parts = [re.sub(r"//[^\n]*", "", p) for p in parts]
    helpers = re.sub(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?)(?![\w.f])", r"\1f", "\n".join(parts))
    return _FIXTURE.replace("HELPERS", helpers), (new, carry, mover)


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    source, flags = assemble(read)
    if source is None:
        result.fail("production helper %s not found (moved or renamed?): the fixture covers nothing" % flags)
        return
    new, carry, mover = flags
    missing = _wiring(read) if new else []
    if new and not mover:
        missing.append("clean_dn_temporal.comp: THE MOVER RECORD (dnMoverNext, policy 3) is gone: (f) / (g) are unbounded")
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.fail("\n".join(missing)) if missing else result.warn("no C++ compiler: the replay not run")
        return
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "caster_noise.cpp")
        with open(src, "w") as o:
            o.write(source)
        exe = os.path.join(tmp, "t")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-w"] + (["-DNEW_POLICY"] if new else [])
                           + (["-DCARRY"] if carry else []) + (["-DMOVER"] if mover else []) + [src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("the fixture does not compile against the production helpers:\n" + c.stderr[:3000])
            return
        r = subprocess.run([exe], capture_output=True, text=True)
    out = r.stdout.strip()
    if r.returncode or missing:
        result.fail("\n".join(([out] if out else []) + missing
                              + ([] if new else ["(old policy: no pdnEvidenceWeight / dnCasterSamplingNoise)"])))
        return
    result.summary = out.splitlines()[0] if out else "ok"
