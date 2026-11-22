"""Regression guard for the sealed-room fade: a room cut off from its light must go dark within a second.

The owner closed a bathroom door (the lamp outside stays on): the room stayed lit for 15-20 s, fading slowly. The
GI radiance cache feeds itself -- GI hits read a cell's mean as the next bounce (RenderVKGIHitAmbient 2 / 3) -- so in
a closed room the light cycles cell -> hit -> receiver -> cell, and each trip waited on
  * the reservoirs: the cache took the RESERVOIR estimate, an exponential mean of ~1 / Msum per frame
    (16 x (1 + 3 spatial taps) + 1 = 65 frames, frame-based: twice as long in seconds at 30 FPS),
  * the cell's window (RenderVKGICacheFrames 32, time-based),
times 1 / (1 - albedo) trips; the cells behind the camera (no receiver measures them) held their light until
eviction (600 frames, 10 s) and held the room up; the GI denoiser's 32 frames came on top. Old code: 16-23 s here.
The fix (clean_gi_common.glsl THE CHANGE TEST; clean_deferred_light.comp's resolve and giReuseFinish):
  * the cache takes the receivers' FRESH estimate (independent, immediate), never the reservoirs' lagged one;
  * a regional statistical change test (the bins: summed relative departures of the cells' fresh means against
    their summed sampling noise -- GI_BIN_K sigma, or GI_BIN_KS three frames running, and a material share)
    starts a CHANGED episode (GI_CELL_HOLD frames, re-armed while the region keeps moving the same way);
  * CHANGED cells take this frame's fresh mean (the loop runs a bounce a frame), their receivers cap reused
    reservoir confidence (GI_CELL_MCAP) and GI denoiser history (the light-change cap, 8), and the region's
    unmeasured cells follow the measured cells' ratio (sums) every frame of the episode.

The fixture compiles those production helpers (giCellVar, giBinTerms, giBinVerdict, giCellHold, giBinQ,
giBinRatio, htAlpha, htAdvance, the GI_CELL_ / GI_BIN_ constants) into C++ and replays a lumped room in the GPU's
order (the trace reads last resolve's cells, then the resolve): 48 measured cells (8..256 fresh samples a
frame, exponentially distributed -- heavy-tailed 1-spp rays), 48 unmeasured ones; every hit reads half its light
from each set (loop gain k = albedo x enclosure); the reservoirs and the GI denoiser as the exponential means
they are (no neighbourhood clamp: conservative).
  (a) SEALED: the direct light (the doorway's patch) goes to 0. The displayed bounce light must fall under 5 % of
      its value within N frames: N = 32 at k = 0.8 (0.53 s at 60 FPS, 1.07 s at 30), N = 56 at k = 0.9 (0.93 /
      1.87 s). Frames, because the loop runs a bounce a frame. Now: 24 / 50 frames at 60 FPS, 23 / 46 at 30.
  (a) is replayed twice: the GI denoiser on the display (a CHANGED texel's history DN_GI_CHANGED_HIST), and DLSS
      Ray Reconstruction taking the GI raw (the owner's setup), modelled as a 24-frame accumulator whose
      responsivity guide marks CHANGED pixels (clean_rr_guides.comp rrGiChanged) -- before that guide the cache was
      fast and RR's history held the room: 82-98 frames here, broken only by camera motion. Bounds 32 / 56.
  (b) STATIC: the same room lit and settled, 8..256 and 4..64 samples a cell, 60 / 30 / 144 FPS, 6000 frames:
      no CHANGED episode, no bin ratio applied, the cells' and the display's variance within 5 % of a
      detector-free control on the same samples (no extra noise).
  (c) STATIC, SPARSE BRIGHT PATHS: 90 % of the light in rare unclamped hits (p 0.002 / 0.0005 a sample), 4-256
      samples a cell -- fewer than one bright hit per cell and frame, the regime of reduced GI extent and upscaling.
      CHANGED at most 20 % of the cell-frames. Before 2026-11-14 (relative variance capped at 1, CHANGED cells voting
      with their 1-frame means) five of the six cases latched (CHANGED on 100 % of the cell-frames once started; the
      new fixture against the old helpers alone: 43-75 %): near 1-spp GI everywhere such light falls.
Old code (no giBinVerdict) replays the old composition and fails (a).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "gi-sealed-fade"
STAGE = "source"
DESCRIPTION = "a GI room sealed from its light goes dark within ~1 s; static noisy lighting gains no resets / variance"
GUARDS = ("The 15-20 s fade of a room whose door closes (the radiance cache feeding itself through the reservoirs' "
          "and its own long means, the unseen cells holding the room up), and the opposite failure: a change detector that resets settled noisy GI.")


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
static int cntLo = 8, cntHi = 256;   // (a cell's fresh samples a frame: uniform in [cntLo, cntHi])
static float htStep() { return testStep; }
HELPERS

static uint rng = 0x9E3779B9u;
static float rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float(rng >> 8) + 0.5f) * (1.0f / 16777216.0f); }
// a cell's fresh frame mean: cnt 1-spp estimates of mean f, exponentially distributed (std = mean), clamped at
// the firefly clamp (8 x here: rarely reached)
// (c) SPARSE BRIGHT PATHS: spikeShare of the light arrives as rare hits (probability spikeP a sample, f x spikeShare /
// spikeP each -- a small emitter, a lit patch through a gap) on an exponential base carrying the rest; not clamped
// (the cache's fresh feed carries only the absolute RenderVKGIFireflyClamp, which a dim room's hits stay under)
static float spikeP = 0.0f, spikeShare = 0.0f;
static float freshMean(float f, int cnt)
{
    float s = 0.0f;
    if (spikeP > 0.0f)
    {
        for (int i = 0; i < cnt; ++i)
        {
            float v = -f * (1.0f - spikeShare) * log(rnd());
            if (rnd() < spikeP) v += f * spikeShare / spikeP;
            s += v;
        }
        return s / float(cnt);
    }
    for (int i = 0; i < cnt; ++i) s += min(-f * log(rnd()), 8.0f * max(f, 1e-6f));
    return s / float(cnt);
}

static const int NM = 48, NU = 48;      // measured (on screen) / unmeasured (behind the camera) cells
static const float WIN = 32.0f, EVICT = 600.0f, HIST = 32.0f, MOVEHIST = 8.0f, MSUM = 65.0f, RR_HIST = 24.0f, UNIT = 0.04f;   // (the change unit: 1/256 of a white fullbright -- the lit room at 0.5-1 fullbright)
#ifdef NEW_POLICY
static float msumChanged() { return 1.0f + GI_CELL_MCAP * 4.0f; }   // fresh + temporal + 3 spatial, each capped
#else
static float msumChanged() { return MSUM; }
#endif
// THE DISPLAY. rrMode 0: the GI denoiser (RenderVKGIHistory; a CHANGED texel's cap: DN_GI_CHANGED_HIST, or the old
// light-change cap RenderVKGILightChangeHistory 8). rrMode 1: DLSS Ray Reconstruction takes the GI RAW (the owner's
// RenderVKDlssRawSignals bit 8, GI at the render extent) -- MODELLED as an accumulator of RR_HIST frames (an
// assumption: RR's internals are not public; camera motion breaks it, which matched the owner's report), its
// responsivity guide on a CHANGED pixel (clean_rr_guides.comp rrGiChanged) as a 1-frame history.
static int rrMode = 0;
static const int NK8 = 32, NK9 = 56;   // (a)'s bounds (frames) at k 0.8 / 0.9
static float dnChangedCap()
{
#ifdef DN_GI_CHANGED_HIST
    return DN_GI_CHANGED_HIST;
#else
    return MOVEHIST;
#endif
}
static float rrChangedCap()
{
#ifdef RR_RESPONSIVE
    return 1.0f;
#else
    return RR_HIST;
#endif
}

struct Cell { float E = 0, n = 0, age = 0, vs = 0; uint dir = 0, hold = 0; };
struct Room
{
    Cell m[NM], u[NU];
    float R[NM] = {}, H[NM] = {}, Hn[NM] = {};   // per measured cell: reservoir estimate, display history, its length
    // the room's bin (one GI_BIN_EDGE box): last frame's record -> this frame's verdict / ratio, fixed point
    // as the resolve's atomics hold it
    uint binCount = 0, binQo = 0, binQn = 0; int binSr = 0; uint binSv = 0;
};
// One frame. D: the direct-lit bounce (the doorway patch) at the receivers; k: the loop gain.
// mode 0 OLD (reservoir feed, no test), 1 NEW, 2 NEW without the detector (the static control).
// Returns the cells that started a CHANGED episode; ratioUsed counts unmeasured cells a bin ratio scaled.
// The order is the GPU's: the receivers (the trace) read last resolve's cells and holds, then the resolve.
static int frame(Room& r, float D, float k, int mode, int& ratioUsed)
{
    float cm = 0, cu = 0;
    for (const Cell& c : r.m) cm += c.E;
    for (const Cell& c : r.u) cu += c.E;
    const float hitE = 0.5f * cm / NM + 0.5f * cu / NU;   // what a GI hit reads (the cache, last resolve)
    const float f = D + k * hitE;                          // the fresh estimate's mean at every receiver
    int fired = 0;
    // the bin: last frame's record -> this frame's verdict (every cell of the room) and ratio
    uint binV = 0u;
    float ratio = 1.0f;
#ifdef NEW_POLICY
    if (mode == 1)
    {
        binV = giBinVerdict(r.binCount, float(r.binSr) / GI_BIN_RQ, float(r.binSv) / GI_BIN_VQ);
        ratio = giBinRatio(r.binCount, r.binQo, r.binQn);
    }
#endif
    r.binCount = r.binQo = r.binQn = r.binSv = 0; r.binSr = 0;
    for (int i = 0; i < NM; ++i)
    {
        const int cnt = cntLo + int(rnd() * float(cntHi - cntLo + 1));
        const float fm = freshMean(f, cnt);
        Cell& c = r.m[i];
        // the trace: the reservoirs, an exponential mean of ~1 / Msum per frame (frame-based), capped on a
        // CHANGED cell's receivers; the GI denoiser (RenderVKGIHistory; the light-change cap while CHANGED)
        const bool chg = mode == 1 && c.hold > 0u;
        r.R[i] += (fm - r.R[i]) / (chg ? msumChanged() : MSUM);
        const float cap = rrMode ? (chg ? rrChangedCap() : RR_HIST) : (chg ? dnChangedCap() : HIST);
        r.Hn[i] = htAdvance(min(r.Hn[i], cap), cap);
        r.H[i] = mix(r.H[i], r.R[i], htAlpha(1.0f / max(r.Hn[i], 1.0f)));
        // the resolve: the cell's input is the reservoir estimate (old) or the fresh one (new)
        const float in = mode == 0 ? r.R[i] : fm;
        const bool fresh = c.n <= 0.0f;
        float win = WIN;
#ifdef NEW_POLICY
        if (mode != 0)
        {
            uint hold = c.hold > 0u ? c.hold - 1u : 0u;
            const uint was = hold;
            c.dir = giCellHold(binV, hold, c.dir);
            if (hold > 0u && was == 0u) ++fired;
            c.hold = hold;
            if (c.hold > 0u) { win = min(win, GI_CELL_HWIN); c.n = min(c.n, win); }
            const float mu = c.E;
            float br, bv;
            const bool terms = !fresh && giBinTerms(in, mu, c.vs, float(cnt), c.n, UNIT, br, bv);
            c.n = htAdvance(c.n, win);
            const float a = fresh ? 1.0f : htAlpha(1.0f / max(c.n, 1.0f));
            c.vs = fresh ? 0.0f : giCellVar(c.vs, in, mu, float(cnt), a, c.hold > 0u);
            c.E = mix(c.E, in, a);
            if (terms)
            {
                ++r.binCount;
                r.binQo += giBinQ(mu, UNIT); r.binQn += giBinQ(c.E, UNIT);
                if (c.hold == 0u)   // (a CHANGED cell adds its means, no departure: the resolve's rule)
                {
                    r.binSr += int(std::round(br * GI_BIN_RQ));
                    r.binSv += uint(bv * GI_BIN_VQ + 0.5f);
                }
            }
        }
        else
#endif
        {
            c.n = htAdvance(c.n, win);
            c.E = mix(c.E, in, fresh ? 1.0f : htAlpha(1.0f / max(c.n, 1.0f)));
        }
        c.age = 0.0f;
    }
    ratioUsed = 0;
    for (Cell& c : r.u)
    {
        c.age = htAdvance(c.age, EVICT + htStep());
        if (c.age > EVICT) { c = Cell(); continue; }   // evicted: a hit there reads nothing (HitAmbient 3)
#ifdef NEW_POLICY
        if (mode == 1)
        {
            uint hold = c.hold > 0u ? c.hold - 1u : 0u;
            c.dir = giCellHold(binV, hold, c.dir);
            c.hold = hold;
            if (c.n > 0.0f && c.hold > 0u) { c.E *= ratio; ++ratioUsed; }
        }
#endif
    }
    return fired;
}
// a settled room: every cell, reservoir and history at the steady state of D (the unmeasured cells measured
// just before the camera turned: their idle age 0)
static void settle(Room& r, float D, float k, int mode)
{
    const float C = D / (1.0f - k);
    for (Cell& c : r.m) { c = Cell(); c.E = C; c.n = WIN; }
    for (Cell& c : r.u) { c = Cell(); c.E = C; c.n = WIN; }
    for (int i = 0; i < NM; ++i) { r.R[i] = C; r.H[i] = C; r.Hn[i] = HIST; }
    int dummy;
    for (int f = 0; f < 600; ++f)
    {
        frame(r, D, k, mode, dummy);
        for (Cell& c : r.u) c.age = 0.0f;   // (warm-up: the unmeasured cells stay fresh)
    }
}
static float display(const Room& r) { float s = 0; for (int i = 0; i < NM; ++i) s += r.H[i]; return s / NM; }

int main()
{
    int bad = 0;
#ifdef NEW_POLICY
    const int mode = 1;
#else
    const int mode = 0;
#endif
    // (a) SEALED
    for (int rr = 0; rr <= 1; ++rr)
    for (float fps : {60.0f, 30.0f})
        for (float k : {0.8f, 0.9f})
        {
            rrMode = rr;
            testStep = 60.0f / fps;
            rng = 0x1234567u + uint(k * 100.0f) + uint(fps);
            Room r;
            settle(r, 1.0f, k, mode);
            const float H0 = display(r);
            int t = 0, used = 0, dummy;
            const int maxF = int(fps * 60.0f);
            for (; t < maxF; ++t)
            {
                frame(r, 0.0f, k, mode, dummy);
                used += dummy;
                if (display(r) < 0.05f * H0) break;
            }
            const float secs = float(t + 1) / fps;
            std::printf("(a) sealed%s, k %.1f, %g FPS: the bounce light under 5%% after %d frames = %.2f s%s\n",
                        rr ? " (DLSS RR)" : "", k, fps, t + 1, secs, t >= maxF ? " (never, 60 s)" : "");
            // N: the frames the loop needs (it runs a bounce a frame: frame-based, so 30 FPS takes twice the time)
            const int N = k < 0.85f ? NK8 : NK9;
            if (t + 1 > N) { std::printf("FAIL (a): %d frames > %d (%.2f s at 60 FPS, %.2f s at 30)\n", t + 1, N, N / 60.0f, N / 30.0f); bad = 1; }
        }
    rrMode = 0;
#ifdef NEW_POLICY
    // (b) STATIC: no resets, no extra variance against a detector-free control on the same samples
    for (int noisy = 0; noisy <= 1; ++noisy)
    for (float fps : {60.0f, 30.0f, 144.0f})
    {
        testStep = 60.0f / fps;
        cntLo = noisy ? 4 : 8; cntHi = noisy ? 64 : 256;
        double var[2] = {0, 0}, dvar[2] = {0, 0}, fires = 0;
        int ratios = 0;
        for (int m = 1; m <= 2; ++m)
        {
            rng = 0xBADC0DEu + uint(fps);
            Room r;
            settle(r, 1.0f, 0.8f, m);
            const float C = 1.0f / (1.0f - 0.8f);
            double s2 = 0, d2 = 0, cnt = 0;
            for (int f = 0; f < 6000; ++f)
            {
                int used;
                const int fi = frame(r, 1.0f, 0.8f, m, used);
                for (Cell& c : r.u) c.age = 0.0f;   // (the camera sweeps back over them: no eviction)
                if (m == 1) { fires += fi; ratios += used; }
                for (int i = 0; i < NM; ++i)
                {
                    s2 += (r.m[i].E - C) * (r.m[i].E - C);
                    d2 += (r.H[i] - C) * (r.H[i] - C);
                    cnt += 1;
                }
            }
            var[m - 1] = s2 / cnt; dvar[m - 1] = d2 / cnt;
        }
        const double rate = fires / (NM * 6000.0) * fps;
        std::printf("(b) static, %d-%d samples a cell, %g FPS: resets %.5f / cell-second, cell variance %.3f x control, display %.3f x, "
                    "bin ratios applied %d\n", cntLo, cntHi, fps, rate, var[0] / var[1], dvar[0] / dvar[1], ratios);
        if (rate > 0.002) { std::printf("FAIL (b): the detector resets settled noisy cells (%.4f / s)\n", rate); bad = 1; }
        if (var[0] > 1.05 * var[1] || dvar[0] > 1.05 * dvar[1]) { std::printf("FAIL (b): extra variance\n"); bad = 1; }
        if (ratios != 0) { std::printf("FAIL (b): a bin ratio scaled unmeasured cells in a static room\n"); bad = 1; }
    }
    // (c) STATIC, SPARSE BRIGHT PATHS (2026-11-14, the owner's noise at reduced GI extent / under upscaling, worst
    // around emissives): fewer than ~1 bright hit per cell and frame. The old rules latched: the region went CHANGED
    // and stayed CHANGED (five of the six cases below) -- the relative variance capped at 1 made every
    // frame without the rare hit a drop, and the CHANGED cells' 1-frame means re-armed the episode themselves.
    testStep = 1.0f;
    for (float p : {0.002f, 0.0005f})
        for (int band = 0; band < 3; ++band)
        {
            static const int kLo[3] = {4, 16, 64}, kHi[3] = {16, 64, 256};
            cntLo = kLo[band]; cntHi = kHi[band];
            spikeP = p; spikeShare = 0.9f;
            rng = 0xC0FFEEu;
            Room r;
            settle(r, 1.0f, 0.8f, 1);
            double held = 0, n = 0;
            for (int f = 0; f < 3000; ++f)
            {
                int used;
                frame(r, 1.0f, 0.8f, 1, used);
                for (Cell& c : r.u) c.age = 0.0f;
                if (f >= 1000) for (const Cell& c : r.m) { held += c.hold > 0u ? 1 : 0; n += 1; }
            }
            std::printf("(c) static, sparse bright paths (p %g, 90 %% of the light), %d-%d samples a cell: CHANGED %.3f "
                        "of the cell-frames\n", p, cntLo, cntHi, held / n);
            if (held / n > 0.2) { std::printf("FAIL (c): settled light held CHANGED (%.3f)\n", held / n); bad = 1; }
        }
    spikeP = 0.0f;
#endif
    return bad;
}
"""


def _wiring(read):
    """The resolve / finish wiring the replay composes but cannot see: [] when it holds."""
    missing = []
    c = read("indra/vkclean/shaders/clean_deferred_light.comp")
    need = {
        "binV = giBinVerdict(b0.z, float(int(b1.x)) / GI_BIN_RQ, float(b1.y) / GI_BIN_VQ);":
            "the resolve does not read its region's verdict",
        "const uint hst = giCellHold(binV, hold, cw.y & 15u);":
            "the resolve does not start / re-arm the CHANGED hold",
        "if (hold > 0u) { nWin = min(nWin, GI_CELL_HWIN); n = min(n, nWin); }":
            "a CHANGED cell keeps its long window",
        "giBinTerms(m, mu, vs, cnt, n, unit, br, bv)":
            "measured cells do not add their terms to the bin",
        "vs = fresh ? 0.0 : giCellVar(vs, m, mu, cnt, aE, hold > 0u);":
            "the resolve does not keep the cell's spread",
        "if (n > 0.0 && hold > 0u) E *= binRat;":
            "unmeasured CHANGED cells do not follow their region",
        "if (freshOk) giCacheAddWave(slot, Sf, d);":
            "the cache is not fed the fresh estimate",
        "if (cellChg) Mout = min(Mout, GI_CELL_MCAP);":
            "a CHANGED cell's receivers keep their reservoirs' long confidence",
        "if (cellChg) chgShare = 1.0;":
            "a CHANGED cell's receivers keep their GI denoiser's long history",
        "if (hold == 0u)\n                        {\n                            atomicAdd(gi_data[bi + 1u].x, uint(int(round(br * GI_BIN_RQ))));":
            "a CHANGED cell's departure votes in its bin (the latch: it re-arms its own episode, (c))",
    }
    for k, v in need.items():
        if k not in c:
            missing.append("clean_deferred_light.comp: " + v)
    if "uint(sh) | (cellChg ? GI_MOT_CHANGED : 0u)" not in c:
        missing.append("clean_deferred_light.comp: the GI MOT image does not mark CHANGED texels (GI_MOT_CHANGED)")
    t = read("indra/vkclean/shaders/clean_dn_temporal.comp")
    if "if (dnGiChangedCut() && (mo.w & 256u) != 0u) maxD = max(min(maxD, DN_GI_CHANGED_HIST), 1.0);" not in t:
        missing.append("clean_dn_temporal.comp: the GI denoiser keeps its history on CHANGED texels")
    rg = read("indra/vkclean/shaders/clean_rr_guides.comp")
    if "rrGiChanged(q) ? 1.0 : 0.0" not in rg or "(imageLoad(uGiMot, m).w & RR_GI_MOT_CHANGED) != 0u" not in rg:
        missing.append("clean_rr_guides.comp: CHANGED GI texels do not raise DLSS RR's responsivity")
    rv = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if "f.giChanged = rr && f.w.t.giMot && mGiNow && !mGiLagNow && v == 0u;" not in rv or "mGiDn->motView2D() : VK_NULL_HANDLE;" not in rv:
        missing.append("llcleanrenderervk.cpp: DLSS RR is not handed the GI MOT image")
    dr = read("indra/newview/vkgfx/llcleandlssrrvk.cpp")
    if "img(P_GUIDES, 23, t.giMot ? t.giMot : mGiMotPh.view, G, U);" not in dr or "gp.flags[0] |= 4u;" not in dr:
        missing.append("llcleandlssrrvk.cpp: the guides pass does not bind / flag the GI MOT image")
    if "dp.giChangedCut = true;" not in read("indra/newview/vkgfx/llcleangivk.cpp"):
        missing.append("llcleangivk.cpp: the GI denoiser does not take the CHANGED cut")
    h = read("indra/newview/vkgfx/llcleangivk.cpp")
    if "kGiBinBytes = 3u * (2u * 2048u) * 16u" not in h:
        missing.append("llcleangivk.cpp: the state buffer has no room for the change bins (GI_BIN_N 2048)")
    g = read("indra/vkclean/shaders/clean_gi_common.glsl")
    if "#define GI_BIN_N     2048u" not in g:
        missing.append("clean_gi_common.glsl: GI_BIN_N changed -- update kGiBinBytes (llcleangivk.cpp) and this check")
    return missing


def assemble(read):
    """(fixture C++ source, new policy?) from the production helpers; None when one is missing."""
    common = read("indra/vkclean/shaders/clean_gi_common.glsl")
    htime = read("indra/vkclean/shaders/clean_history_time.glsl")
    new = _function(common, "giBinVerdict") is not None
    names = [(htime, n) for n in ("htAlpha", "htAdvance")]
    parts = []
    if new:
        names += [(common, n) for n in ("giCellVar", "giBinTerms", "giBinVerdict", "giCellHold", "giBinQ", "giBinRatio")]
        parts += re.findall(r"#define GI_(?:CELL|BIN)_\w+ [^\n]+", common)
    for src, n in names:
        fn = _function(src, n)
        if fn is None:
            return None, n
        fn = re.sub(r"\b(?:out|inout) (\w+) (\w+)", r"\1& \2", fn)
        parts.append("static " + fn)
    dn = re.search(r"#define DN_GI_CHANGED_HIST [^\n]+", read("indra/vkclean/shaders/clean_dn_common.glsl"))
    if dn:
        parts.append(dn.group(0))
    if "rrGiChanged(q)" in read("indra/vkclean/shaders/clean_rr_guides.comp"):
        parts.append("#define RR_RESPONSIVE 1")
    parts = [re.sub(r"//[^\n]*", "", p) for p in parts]
    helpers = re.sub(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?)(?![\w.f])", r"\1f", "\n".join(parts))
    return _FIXTURE.replace("HELPERS", helpers), new


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    source, new = assemble(read)
    if source is None:
        result.fail("production helper %s not found (moved or renamed?): the fixture covers nothing" % new)
        return
    missing = _wiring(read) if new else []
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.fail("\n".join(missing)) if missing else result.warn("no C++ compiler: the replay not run")
        return
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "gi_sealed_fade.cpp")
        with open(src, "w") as o:
            o.write(source)
        exe = os.path.join(tmp, "t")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-w"] + (["-DNEW_POLICY"] if new else []) + [src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("the fixture does not compile against the production helpers:\n" + c.stderr[:3000])
            return
        r = subprocess.run([exe], capture_output=True, text=True)
    out = r.stdout.strip()
    if r.returncode or missing:
        result.fail("\n".join(([out] if out else []) + missing
                              + ([] if new else ["(old code: no giBinVerdict -- the cache feeds itself through the "
                                                 "reservoirs' and its own long means)"])))
        return
    result.summary = out.splitlines()[0] if out else "ok"
