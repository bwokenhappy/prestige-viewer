"""Regression guard for the light-drop fade: a lit patch whose light is blocked must go dark within a few frames.

The owner opened a door in a dark room (a lamp in the next room): the bright patch on the floor appeared at once;
shut the door and the patch faded over 2-4 s, keeping its exact sharp shape. The patch is DIRECT local light:
ReSTIR DI / stochastic local-light shadows in RADIANCE mode (RenderVKReSTIRRadiance, default on), whose denoiser
(clean_dn_temporal.comp, the pass-15 instance) keeps its own 32-frame history even under DLSS RR (only the ratio
mode's signal goes to RR raw). Why removing light was slow and adding it fast:
  * DN_F_RADIANCE floors the history clamp's sigma at the HISTORY's own per-sample spread. A lit history's spread
    is the bright samples' (one-pick radiance: about its own size), so after the drop the box above the black 3x3
    still reached the old light: no clamp, no cut, a plain 1 / 32 EMA. A dark history's spread is ~0: light
    ARRIVING closes the box on the new value and the clamp cuts the history at once.
  * The caster test that resets a history on a real visibility change (RenderVKShadowCasterHistory 2) only ran
    where a MOVING caster was met; a door that has stopped (or shuts in one step) is no mover.
The fix (clean_dn_temporal.comp THE LIGHT DROP): the same evidence test also runs without a mover, darker only,
for a drop of most of the texel's light (dnDropShape), confirmed two frames running (the pending bits); and the
reset texels are marked (THE DROP MARK) for DLSS RR's responsivity guide (clean_rr_guides.comp rrLightDrop).

THE DEAD ZONE (the owner's retest of that fix: no change at all): the evidence is a ratio over every candidate
light's UNSHADOWED light, so with more lamps behind the walls the door lamp's patch has a prior of 0.3-0.6 -- the
old absolute drop of 0.7 never held, and that is where the clamp fails. The drop is now relative (dnDropShape:
<= DN_DROP_REL of a prior >= DN_DROP_MIN_PRIOR), tested over a wider footprint (the 3x3 + a stride-2 ring,
dnDropRingOffset) once the 3x3 gates it; (a) shuts from p0 = 1.0 / 0.6 / 0.45 / 0.4.

THE RADIANCE DROP (the owner's second retest: RenderVKDenoiseHistory 1 removes the fade, 17a92ad9a8 changed
nothing -- the evidence test does not fire on that floor): the radiance itself, a footprint's raw sum against its
own histories, with a variance bound and a lit-texel count (dnRadDrop); no evidence used.

The fixture compiles the production helpers (dnCaster*, dnDropShape, dnRad*, dnDropMarkCarry, htAlpha,
htAdvance, the DN_CASTER_ / DN_DROP_ / DN_RDROP_ constants) into C++ and replays the temporal pass's composition
on a 20 x 20 texel floor. Each texel's raw radiance is two picks: a FRESH one (the lamp with probability pf) and
its block's REUSED reservoir (blk x blk texels share it; the lamp with probability q; visibility reuse drops a
blocked pick the next frame, a lamp pick is kept, redrawn 1 / 4 of the frames), each an exponential RIS weight
over its probability (heavy tailed); the evidence (when the floor has it) the fresh pick's 0 / 1 visibility; the
history clamp floored at the history's spread. Modes: 0 no drop test, 1 the evidence test alone (17a92ad9a8),
2 both.
  (a) SHUT: vis 1 -> 0 (and -> 0.15: a faint light remains); the patch must be within 5 % of its new level in
      <= 6 frames at 60 / 30 / 144 FPS, and >= 90 % of its texels carry the RR drop mark on some frame. Mode 0
      must take > 30 frames (the bound means something); on the owner-like floors (pf < 0.3, with and WITHOUT
      evidence) so must mode 1 -- the case 17a92ad9a8 missed.
  (b) OPEN: reported (every composition follows arriving light by itself).
  (c) STATIC: 13 floors (pf 0.05 .. 1, q 0.1 .. 1, blocks 1 .. 6, with / without evidence), 20000 frames at
      60 / 144 FPS: no drop reset, no drop mark, the display bit-identical to mode 0. A partial dimming
      (vis 1 -> 0.5, a real change) may reset a few texels once, during the change: <= 15 %.
The grid wraps (an interior floor: every 3x3 full); a silhouette's 4-6 same-surface texels (DN_DROP_MIN_N) keep
the old path.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "light-drop-fade"
STAGE = "source"
DESCRIPTION = "a lit local-light patch whose light is blocked goes dark in a few frames; steady light keeps its history"
GUARDS = ("The 2-4 s sharp-edged fade of a lamp's patch after a door shuts (the radiance denoiser's clamp box held "
          "open by the bright history's own spread, the caster test running only beside movers), and the opposite "
          "failure: a drop detector that resets settled noisy light.")


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
using std::abs; using std::sqrt; using std::exp; using std::log;
static float min(float a, float b) { return std::min(a, b); }
static float max(float a, float b) { return std::max(a, b); }
static float clamp(float x, float a, float b) { return std::clamp(x, a, b); }
static float mix(float a, float b, float t) { return a + (b - a) * t; }
static float testStep = 1.0f;
static float htStep() { return testStep; }
HELPERS

static const int N = 20;
static const float GAMMA = 1.5f, MAXD = 32.0f;   // RenderVKDenoiseClamp, RenderVKDenoiseHistory
static uint rng = 0x9E3779B9u;
static float rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float(rng >> 8) + 0.5f) * (1.0f / 16777216.0f); }
static void ring(int k, int& dx, int& dy) { const int j = k < 12 ? k : k + 1; dx = 2 * (j % 5 - 2); dy = 2 * (j / 5 - 2); }

// The floor's light: the lamp is picked by a texel's FRESH pick with probability pf (the other candidate lights
// are behind walls: their picks are blocked) and by its block's REUSED reservoir with probability q (blk x blk
// texels share one reservoir -- spatial reuse; it keeps the lamp from frame to frame, redrawn with probability
// 1 / 4, and a blocked light's pick only one frame -- visibility reuse; blk 1 = an independent pick per texel,
// redrawn every frame). A lamp pick is visible with
// probability vis (1 lit, 0 the door shut, 0.15 most of the lamp blocked); its RIS weight is exponential (heavy
// tailed) over its pick probability, so the raw mean is vis. ev: the fresh pick's 0 / 1 visibility is the
// evidence (pdnEvidenceWeight); off: the texels carry none (a lone reused pick).
struct Scene { float pf, q; int blk; bool ev; };
struct Px { float h = 0, hv = 0, len = 0, vis = -1; uint pend = 0; bool init = false; };
struct Raw { float r, e; };
struct Stats { int drops = 0, marks = 0; long gates = 0; };
static std::vector<int> resv;   // per block: the reused reservoir holds the lamp
// mode 0: no drop test (before 0085081dc1); 1: the evidence drop test only (17a92ad9a8); 2: + THE RADIANCE DROP
static Stats frame(std::vector<Px>& g, const Scene& s, float vis, int mode, std::vector<int>* marked)
{
    const int nb = (N + s.blk - 1) / s.blk;
    if (int(resv.size()) != nb * nb) { resv.assign(nb * nb, 0); for (int& v : resv) v = rnd() < s.q ? 1 : 0; }
    // (RS_F_VIS_REUSE) a reservoir holding a blocked light is re-stored with W 0 and replaced the next frame; one
    // holding the visible lamp keeps it (redrawn with probability 1 / 4); blk 1: redrawn every frame
    for (int& v : resv) if (s.blk == 1 || v == 0 || rnd() < 0.25f) v = rnd() < s.q ? 1 : 0;
    std::vector<Raw> raw(N * N);
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        Raw& q = raw[y * N + x];
        const bool rl = resv[(y / s.blk) * nb + x / s.blk] != 0, fl = rnd() < s.pf;
        const float rv = rl && rnd() < vis ? 1.0f : 0.0f, fv = fl && rnd() < vis ? 1.0f : 0.0f;
        const float w1 = -log(rnd()), w2 = -log(rnd());
        q.r = 0.5f * (rv * w1 / max(s.q, 1e-3f) + fv * w2 / max(s.pf, 1e-3f));
        q.e = s.ev ? fv : -1.0f;
    }
    Stats st;
    std::vector<Px> out = g;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const Px& P = g[y * N + x];
        Px& o = out[y * N + x];
        const Raw& c = raw[y * N + x];
        float s1 = 0, s2 = 0, vs = 0, vs2 = 0, n = 0, vn = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            const int qx = (x + dx + N) % N, qy = (y + dy + N) % N;   // (wrapped: an interior floor, every 3x3 full)
            const Raw& q = raw[qy * N + qx];
            s1 += q.r; s2 += q.r * q.r; n += 1;
            if (q.e >= 0.0f) { vs += q.e; vs2 += q.e * q.e; vn += 1; }
        }
        const float mD = s1 / n, sigD0 = sqrt(max(s2 / n - mD * mD, 0.0f));
        const float visMean = vs / max(vn, 1.0f), visSigma = sqrt(max(vs2 / max(vn, 1.0f) - visMean * visMean, 0.0f));
        if (!P.init) { o.h = c.r; o.hv = 0; o.len = htStep(); o.vis = vn > 0 ? dnCasterEvidence(c.e, visMean, vn) : -1.0f; o.init = true; continue; }
        const float prior = P.vis, ld = P.len;
        // ---- the evidence drop (testV false: no mover) ----
        const bool dropShape = mode >= 1 && vn > 0 && dnDropShape(visMean, prior, vn, DN_DROP_MIN_N);
        float ds = vs, ds2 = vs2, dn = vn;
        if (dropShape)
            for (int k = 0; k < DN_DROP_RING; ++k)
            {
                int ox, oy; ring(k, ox, oy);
                const float e = raw[((y + oy + N) % N) * N + (x + ox + N) % N].e;
                if (e < 0.0f) continue;
                ds += e; ds2 += e * e; dn += 1;
            }
        const float dMean = ds / max(dn, 1.0f), dSigma = sqrt(max(ds2 / max(dn, 1.0f) - dMean * dMean, 0.0f));
        const bool testDrop = dropShape && dnDropShape(dMean, prior, dn, DN_DROP_MIN_NW);
        const float noiseD = dnCasterSamplingNoise(prior, dMean, dn, dn, ld);
        const float curD = dnCasterCurrent(c.e, dMean, false);
        const uint pendEv = testDrop ? dnCasterPending(curD, dMean, prior, dSigma, noiseD) : 0u;
        // ---- THE RADIANCE DROP: the 3x3 gate, then the footprint's raw against its own histories ----
        const bool radGate = mode >= 2 && n >= DN_DROP_MIN_N && dnRadDropShape(mD, P.h);
        float rr = 0, rh = 0, rv = 0, rn = 0, rl = 0;
        if (radGate)
        {
            rr = c.r; rh = P.h; rv = P.hv; rn = 1; rl = dnRadLit(c.r, P.h) ? 1.0f : 0.0f;
            for (int k = 0; k < DN_DROP_RING; ++k)
            {
                int ox, oy; ring(k, ox, oy); ox = ox / 2 * DN_RDROP_STRIDE; oy = oy / 2 * DN_RDROP_STRIDE;   // (dnRadRingOffset)
                const int i = ((y + oy + N) % N) * N + (x + ox + N) % N;
                if (g[i].len < 1.0f) continue;
                rr += raw[i].r; rh += g[i].h; rv += g[i].hv; rn += 1; rl += dnRadLit(raw[i].r, g[i].h) ? 1.0f : 0.0f;
            }
        }
        const bool radDrop = radGate && dnRadDrop(rr, rh, rv, rn, rl);
        const uint pendV = (radDrop || pendEv == 1u) ? 1u : pendEv;
        const bool changed = (testDrop || radDrop) && dnCasterConfirmed(pendV, P.pend);
        float lenD = P.len, hD = P.h;
        const float hvD = P.hv;
        if (changed) { lenD = 0.0f; hD = c.r; }
        // ---- the clamp (DN_F_RADIANCE: sigma floored at the history's spread) ----
        const float sig = max(sigD0, sqrt(max(hvD, 0.0f)));
        const float lo = max(mD - GAMMA * sig, 0.0f), hi = mD + GAMMA * sig;
        const float hc = clamp(hD, lo, hi);
        const float cut = abs(hc - hD) / max(hD + mD, 1e-4f);
        if (cut > 0.05f) lenD = min(lenD, mix(8.0f, 1.0f, clamp((cut - 0.05f) * 3.0f, 0.0f, 1.0f)));
        hD = hc;
        // ---- length, accumulation, variance ----
        float maxD = MAXD;
        const bool follow = dnCasterFollow(0u, ld);   // (the mover test's pending only: testV is false here)
        if (dnCasterShouldCap(2u, false, changed || follow)) maxD = dnCasterHistoryCap(maxD, htStep());
        const float nD = htAdvance(lenD, maxD);
        const float aD = lenD > 0.0f ? htAlpha(1.0f / max(nD, 1.0f)) : 1.0f;
        o.h = mix(hD, c.r, aD);
        const float aV = htAlpha(1.0f / max(nD, 1.0f));
        float var = (1.0f - aV) * (hvD + aV * (c.r - hD) * (c.r - hD));
        if (nD < 4.0f) var = max(var, sigD0 * sigD0);
        o.hv = var;
        o.len = nD;
        o.vis = vn > 0 ? dnCasterVisibilityAcc(prior, dnCasterEvidence(c.e, visMean, vn), changed ? 1.0f : dnCasterEvidenceAlpha(ld)) : prior;
        const bool dropNow = changed && (dropShape || radGate);
        const bool mark = dropNow || dnDropMarkCarry(P.pend, nD, pendV);
        o.pend = ((changed ? 0u : pendV) | (mark ? DN_DROP_MARK : 0u));
        if (dropNow) ++st.drops;
        if (radGate) ++st.gates;   // (THE RADIANCE DROP's ring loads ran)
        if (mark) { ++st.marks; if (marked) (*marked)[y * N + x] = 1; }
    }
    g.swap(out);
    return st;
}
static float display(const std::vector<Px>& g) { double s = 0; for (const Px& p : g) s += p.h; return float(s / g.size()); }

int main()
{
    int bad = 0;
    // (a) SHUT, (b) OPEN. The first three: the earlier fixture's floors (independent picks, the lamp's share p0 of
    // both; THE DEAD ZONE's 0.6 / 0.45 / 0.4). The last two: THE RADIANCE DROP's floors -- the lamp under 0.3 of the
    // fresh picks, the reused reservoirs mostly the lamp (visibility reuse), with and WITHOUT evidence: the evidence
    // test cannot fire there (mode 1 must stay slow: the case is the owner's retest).
    struct Case { Scene s; float rest; bool newCase; };
    const Case cases[] = {
        {{1.0f, 1.0f, 1, true}, 0.0f, false}, {{1.0f, 1.0f, 1, true}, 0.15f, false},
        {{0.6f, 0.6f, 1, true}, 0.0f, false}, {{0.45f, 0.45f, 1, true}, 0.0f, false}, {{0.4f, 0.4f, 1, true}, 0.0f, false},
        {{0.15f, 0.9f, 4, true}, 0.0f, true}, {{0.15f, 0.9f, 4, false}, 0.0f, true}, {{0.25f, 0.6f, 3, false}, 0.0f, true},
    };
    for (float fps : {60.0f, 30.0f, 144.0f})
        for (const Case& cs : cases)
            for (int mode = 2; mode >= 0; --mode)
            {
                if (mode == 1 && !cs.newCase) continue;
                testStep = 60.0f / fps;
                rng = 0x1234567u + uint(fps) + uint(cs.rest * 100.0f) + uint(cs.s.pf * 1000.0f) + uint(cs.s.q * 7000.0f)
                    + (cs.s.ev ? 0u : 99u);
                resv.clear();
                std::vector<Px> g(N * N);
                for (int f = 0; f < int(10.0f * fps); ++f) frame(g, cs.s, 1.0f, mode, nullptr);
                const float H0 = display(g), H1 = cs.rest;   // (the raw mean: vis)
                std::vector<int> marked(N * N, 0);
                int t = 0;
                const int maxF = int(fps * 10.0f);
                for (; t < maxF; ++t)
                {
                    frame(g, cs.s, cs.rest, mode, &marked);
                    if (abs(display(g) - H1) < 0.05f * (H0 - H1)) break;
                }
                int mk = 0; for (int v : marked) mk += v;
                std::printf("(a) shut pf %.2f q %.2f blk %d %s -> %.2f, %g FPS, %s: within 5%% after %d frames = %.3f s; %d %% RR-marked\n",
                            cs.s.pf, cs.s.q, cs.s.blk, cs.s.ev ? "evidence" : "NO evidence", cs.rest, fps,
                            mode == 2 ? "drop tests" : mode == 1 ? "evidence test only (17a92ad9a8)" : "no drop test",
                            t + 1, (t + 1) / fps, 100 * mk / (N * N));
                if (mode == 2 && t + 1 > 6) { std::printf("FAIL (a): the patch takes %d frames to go (> 6)\n", t + 1); bad = 1; }
                if (mode == 2 && mk < (N * N * 9) / 10) { std::printf("FAIL (a): only %d of %d texels marked for DLSS RR\n", mk, N * N); bad = 1; }
                if (mode == 0 && t + 1 <= 30) { std::printf("FAIL (a): the old composition fades in %d frames: the case tests nothing\n", t + 1); bad = 1; }
                if (mode == 1 && cs.newCase && cs.s.pf < 0.3f && t + 1 <= 30)
                { std::printf("FAIL (a): the evidence test alone handles this floor: it is not the owner's case\n"); bad = 1; }
                if (mode == 2 && cs.rest == 0.0f)
                {
                    int u = 0;
                    for (; u < maxF; ++u) { frame(g, cs.s, 1.0f, mode, nullptr); if (display(g) > 0.95f * H0) break; }
                    std::printf("(b) open, %g FPS: within 5%% after %d frames\n", fps, u + 1);
                }
            }
    testStep = 1.0f;
    // (c) STATIC (and a partial dimming): the drop tests must never fire, the display identical to no drop test
    const Scene statics[] = {
        {0.1f, 0.1f, 1, true}, {0.3f, 0.3f, 1, true}, {0.35f, 0.35f, 1, true}, {0.45f, 0.45f, 1, true}, {0.6f, 0.6f, 1, true},
        {0.8f, 0.8f, 1, true}, {1.0f, 1.0f, 1, true},
        {0.15f, 0.9f, 4, false}, {0.15f, 0.5f, 4, false}, {0.15f, 0.2f, 4, false}, {0.05f, 0.3f, 6, false},
        {0.3f, 0.3f, 4, true}, {0.1f, 0.1f, 2, false},
    };
    for (float fps : {60.0f, 144.0f})
        for (int si = -1; si < int(sizeof(statics) / sizeof(statics[0])); ++si)
        {
            const Scene s = si >= 0 ? statics[si] : Scene{1.0f, 1.0f, 1, true};
            testStep = 60.0f / fps;
            Stats tot;
            double diff = 0;
            std::vector<Px> gn(N * N), go(N * N);
            const uint seed = 0xBADC0DEu + uint(si + 1) * 7919u + uint(fps);
            const int F = 20000;
            std::vector<int> rn, ro;
            for (int f = 0; f < F; ++f)
            {
                // si < 0: lit, then half the light blocked at frame 2000 (a partial dimming -- not a drop)
                const float vf = si >= 0 ? 1.0f : (f < 2000 ? 1.0f : 0.5f);
                resv.swap(rn); rng = seed + uint(f) * 2654435761u; const Stats st = frame(gn, s, vf, 2, nullptr); resv.swap(rn);
                resv.swap(ro); rng = seed + uint(f) * 2654435761u; frame(go, s, vf, 0, nullptr); resv.swap(ro);
                tot.drops += st.drops; tot.marks += st.marks; tot.gates += st.gates;
                for (int i = 0; i < N * N; ++i) diff = std::max(diff, double(abs(gn[i].h - go[i].h)));
            }
            if (si >= 0)
                std::printf("(c) static pf %.2f q %.2f blk %d %s, %g FPS, %d frames: drop resets %d, RR marks %d, max display difference %.2g; radiance ring loads on %.2f %% of texel-frames\n",
                            s.pf, s.q, s.blk, s.ev ? "evidence" : "no evidence", fps, F, tot.drops, tot.marks, diff, 100.0 * double(tot.gates) / (double(F) * N * N));
            else
                std::printf("(c) partial dimming 1 -> 0.5, %g FPS: drop resets %d, RR marks %d, max display difference %.2g\n",
                            fps, tot.drops, tot.marks, diff);
            if (si >= 0 && (tot.drops || tot.marks || diff > 0.0)) { std::printf("FAIL (c): a drop test fires on steady light\n"); bad = 1; }
            if (si < 0 && tot.drops > N * N * 3 / 20) { std::printf("FAIL (c): a drop test fires on a partial dimming (%d texels)\n", tot.drops); bad = 1; }
        }
    return bad;
}
"""


def _wiring(read):
    """The temporal / RR wiring the replay composes but cannot see: [] when it holds."""
    missing = []
    t = read("indra/vkclean/shaders/clean_dn_temporal.comp")
    need = {
        "const bool testDrop = !testV && dropShape\n": "the drop test does not run without a mover",
        "dnDropShape(visMean, receiverHistory.visibility, visN, DN_DROP_MIN_N)": "the drop test does not take the drop's size",
        "dnDropShape(dropMean, receiverHistory.visibility, dropN, DN_DROP_MIN_NW)":
            "the drop test does not take the wide footprint (THE DEAD ZONE)",
        "const ivec2 q = m + dnDropRingOffset(k);": "the gated texels do not read the ring's evidence",
        "return 2 * ivec2(j % 5 - 2, j / 5 - 2);": "dnDropRingOffset changed -- update this fixture's ring",
        "dnCasterFollow(pendMover, receiverHistory.ld)": "a merely pending drop caps the history",
        "|| ((testDrop || radDrop) && dnCasterConfirmed(pendV, receiverHistory.pending));":
            "the drop tests are not confirmed over two frames (or not wired into the reset)",
        "const uint pendEv = testDrop ? dnCasterPending(curDrop, dropMean, receiverHistory.visibility, dropSigma, noiseDrop) : 0u;":
            "the drop test keeps no pending deviation over the wide footprint",
        "const uint pendV = (radDrop || pendEv == 1u) ? 1u : testV ? pendMover : pendEv;":
            "a drop does not set the pending deviation",
        "&& dnRadDropShape(pdnLum(mD), pdnLum(receiverHistory.d));": "THE RADIANCE DROP is not gated by the 3x3",
        "const ivec2 q = m + dnRadRingOffset(k);": "THE RADIANCE DROP does not read its ring",
        "return DN_RDROP_STRIDE * ivec2(j % 5 - 2, j / 5 - 2);": "dnRadRingOffset changed -- update this fixture's ring",
        "pdnUnpackHist(imageLoad(uDnHist, ivec3(hq, DN_L(int(pl)))), qhd, qhs, qvd, qvs);":
            "THE RADIANCE DROP does not compare against the taps' own histories",
        "rdLit += dnRadLit(qr, qh) ? 1.0 : 0.0;": "THE RADIANCE DROP does not count lit taps",
        "const bool radDrop = radGate && dnRadDrop(rdRaw, rdHist, rdVar, rdN, rdLit);": "THE RADIANCE DROP is not decided",
        "const bool dropNow = (casterChangeD || casterChangeS) && (dropShape || radGate);": "a real drop is not marked",
        "if (dnDebug() == PDN_DBG_DROP)": "the light-drop debug view (RenderVKDenoiseDebug 13) is gone",
        "dnDropMarkCarry(receiverHistory.pending, nD, pendV)": "the drop mark is not carried for a few frames",
        "| (dropMark ? DN_DROP_MARK << PDN_META_PEND_SHIFT : 0u);": "the drop mark is not stored in the pending bits",
        "imageStore(uDnMot, DN_MOT(m), uvec4(mo.xyz, dropMark ? (mo.w | PDN_MOT_LIGHT_DROP) : (mo.w & ~PDN_MOT_LIGHT_DROP)));":
            "the drop mark does not reach MOT w (DLSS RR)",
    }
    for k, v in need.items():
        if k not in t:
            missing.append("clean_dn_temporal.comp: " + v)
    if "#define PDN_MOT_LIGHT_DROP 0x100000u" not in read("indra/vkclean/shaders/clean_pdn_common.glsl"):
        missing.append("clean_pdn_common.glsl: PDN_MOT_LIGHT_DROP moved -- update clean_rr_guides.comp and this check")
    rg = read("indra/vkclean/shaders/clean_rr_guides.comp")
    if "const uint RR_PDN_LIGHT_DROP = 0x100000u;" not in rg or "rrLightDrop(q) ? 1.0 : 0.0" not in rg:
        missing.append("clean_rr_guides.comp: a light drop does not raise DLSS RR's responsivity")
    dr = read("indra/newview/vkgfx/llcleandlssrrvk.cpp")
    if "img(P_GUIDES, 24, t.pdnMot ? t.pdnMot : mGiMotPh.view, G, U);" not in dr or "gp.pdn[2] = 1u;" not in dr \
            or "{23,U},{24,U}}, 25, 0}" not in dr:
        missing.append("llcleandlssrrvk.cpp: the guides pass does not bind / flag the local-light MOT image")
    rv = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if "f.lightDrop = rr && f.w.t.pdnMot && mPdnDropNow" not in rv or "mPdn->motView2D() : VK_NULL_HANDLE;" not in rv \
            or "mPdnDropNow = false;" not in rv:
        missing.append("llcleanrenderervk.cpp: DLSS RR is not handed the local-light MOT image")
    ps = read("indra/newview/vkgfx/llcleanprojstochvk.cpp")
    if "mPdnDropNow = dp.radiance && !dp.raw && dp.casterHistoryPolicy >= 2u" not in ps:   # (3: 2 + the mover record)
        missing.append("llcleanprojstochvk.cpp: the frame does not say when the drop marks are this frame's")
    # the light-drop debug view (RenderVKDenoiseDebug 13): the host passes it, the a-trous transports it
    # (the clamp may pass later views too -- 14: RenderVKShadowMoverMask 2's mover-exact set)
    mclamp = re.search(r"mPdnDebug = dbg <= (\d+)u \? dbg : 0u;", ps)
    if not mclamp or int(mclamp.group(1)) < 13:
        missing.append("llcleanprojstochvk.cpp: RenderVKDenoiseDebug 13 (the light-drop view) is clamped away")
    if "#define PDN_DBG_DROP 13u" not in read("indra/vkclean/shaders/clean_pdn_common.glsl") \
            or "dbg == PDN_DBG_SKIN_RECOVERY || dbg == PDN_DBG_DROP" not in read("indra/vkclean/shaders/clean_dn_atrous.comp"):
        missing.append("clean_dn_atrous.comp / clean_pdn_common.glsl: the light-drop view is not passed through unfiltered")
    return missing


def assemble(read):
    """The fixture C++ source from the production helpers; (None, name) when one is missing."""
    temporal = read("indra/vkclean/shaders/clean_dn_temporal.comp")
    htime = read("indra/vkclean/shaders/clean_history_time.glsl")
    names = [(htime, n) for n in ("htAlpha", "htAdvance")]
    names += [(temporal, n) for n in ("dnCasterChange", "dnCasterEvidenceAlpha", "dnCasterSamplingNoise",
                                      "dnCasterVisibilityChange", "dnCasterPending", "dnCasterConfirmed",
                                      "dnCasterFollow", "dnCasterCurrent", "dnCasterEvidence", "dnDropShape",
                                      "dnDropMarkCarry", "dnRadDropShape", "dnRadDropSignificant", "dnRadLit", "dnRadDrop", "dnCasterVisibilityAcc", "dnCasterShouldCap",
                                      "dnCasterHistoryCap")]
    parts = re.findall(r"#define DN_(?:CASTER|DROP|RDROP)_\w+ [^\n]+", temporal)
    for src, n in names:
        fn = _function(src, n)
        if fn is None:
            return None, n
        parts.append("static " + fn)
    parts = [re.sub(r"//[^\n]*", "", p) for p in parts]
    helpers = re.sub(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?)(?![\w.f])", r"\1f", "\n".join(parts))
    return _FIXTURE.replace("HELPERS", helpers), None


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    source, name = assemble(read)
    if source is None:
        result.fail("production helper %s not found (moved or renamed?): the fixture covers nothing" % name)
        return
    missing = _wiring(read)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.fail("\n".join(missing)) if missing else result.warn("no C++ compiler: the replay not run")
        return
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "light_drop_fade.cpp")
        with open(src, "w") as o:
            o.write(source)
        exe = os.path.join(tmp, "t")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-w", src, "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("the fixture does not compile against the production helpers:\n" + c.stderr[:3000])
            return
        r = subprocess.run([exe], capture_output=True, text=True)
    out = r.stdout.strip()
    if r.returncode or missing:
        result.fail("\n".join(([out] if out else []) + missing))
        return
    lines = out.splitlines()
    result.summary = lines[0] if lines else "ok"
