"""Regression guard for the opaque RT reflections' "dance": the resolve's history thrown away on noise.

Pass 13 (clean_deferred_light.comp, RenderVKRTReflectionsTemporal) accumulates each mirror / near-mirror receiver's
resolved reflection over frames (rtrTemporal). The old rule clamped the history to the gather's mean +- 1.25 sigma and
went back to this frame once it sat 0.25 sigma outside. sigma was the gather's SPATIAL spread only, and a mirror's
gather is mostly its own ray (the neighbours weighed down by the plane / lobe / hit-distance tests) while that ray's
hit shading is noisy: the box sat on this frame and the anti-lag fired in about half the frames. Reset and held pixels
alternated -- the shimmer the owner reported (2026-11-09), worse while the camera moved, where also:
  * a receiver with any normal-map / curvature spread (aa > 0.02) took the SURFACE's motion only, so its reflection
    swam with the camera instead of following its virtual image;
  * the history was fetched at the nearest previous texel: snapped to the pixel grid on every camera move (a crawl);
  * under DLSS RR our full-length history stacked with the network's.

Now (indra/vkclean/shaders/clean_rt_refl_temporal.h, shared with the fixture):
  * the fixture (scripts/check/test_rt_refl_temporal.cpp, the header as-is, against the old rule reproduced): a
    mirror pixel's frame-to-frame RMS <= 0.6 x the old rule's with resets in <= 5 % of the frames and <= 3 % bias, for
    a gather with neighbours and one that is just the own ray, under grey and under two-colour light noise; a hit's
    single shadow ray (two-valued 0.02 / 0.12, 0.002 / 0 + 10 % x 0.3, sparse 0.1 + 2 % x 3) converged within 3 % of
    its mean at the RR cap and 8 (2026-11-09: the log-symmetric clamp settled at the geometric mean, shadows in
    reflections 3-17 % too dark; now a linear box below the log anti-lag); a fresh
    own-ray-only history resets at most once in its first 8 frames; a 5 x change in the reflection (a light on, then
    off -- the ghost case of a spread kept in absolute terms) followed within 4 / 8 frames; a running mean after a
    reset capped at the history length (the RR cap included); a steady reflection held exactly; and the hit's
    light picks (clean_hit_nee.h, the production helpers): the STRATIFIED draw the RT reflection kernels run
    (hnStratInit / hnAddS) takes each light w / W exactly over its uniform, is unbiased, never noisier than the
    hashed draw (hnAdd) at the RR cap, and at least 20 % less noisy for 2-4 still lights at 2-4 rays (2026-11-09:
    the picks were independent every frame -- the crawl that remained at 4 shadow rays);
  * wiring: pass 13 includes the header and resolves through rrtResolve; the history is bilinear and depth-validated
    (rtrHistFetch) for both candidates, the virtual image tried below RTR_STOCH_ROUGH whatever the normal spread and
    chosen against the surface's by agreement with this frame; the gather's effective sample count (w2B) reaches it;
    the variance / length layers (2 + parity) are read and written; the history length shortens under RR (rtt_opts4
    bit 2); the host allocates, clears and barriers four history layers and sets bit 2 from mRrRaw.
On the tree before this change the header does not exist: the check fails.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "rt-refl-temporal"
STAGE = "source"
DESCRIPTION = "the opaque RT reflections' history keeps converging (variance anti-lag, virtual image, bilinear)"
GUARDS = ("Shimmer / 'dancing' of RT reflections on opaque mirrors and near-mirrors (pass 13): the history clamped to a "
          "spatial-only box and reset on the own ray's shading noise in about half the frames; bumpy mirrors "
          "reprojected by the surface's motion; a nearest-texel history fetch; our history stacked with DLSS RR's.")

_SH = "indra/vkclean/shaders/"


def _body(text, head):
    i = text.find(head)
    if i < 0:
        return ""
    j = text.find("\n}\n", i)
    return text[i:j] if j > 0 else text[i:]


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    d = read(_SH + "clean_deferred_light.comp")
    i = d.find("#elif CLEAN_DEFER_PASS == 13\n// ---- RT RESOLVE")
    p13 = d[i:d.find("#elif CLEAN_DEFER_PASS == 14", i)] if i >= 0 else ""
    if not p13:
        missing.append("clean_deferred_light.comp has no pass 13 section")
        return missing
    if '#include "clean_rt_refl_temporal.h"' not in p13:
        missing.append("pass 13 does not include clean_rt_refl_temporal.h")
    tmp = _body(p13, "vec3 rtrTemporal(")
    if not tmp:
        missing.append("pass 13 has no rtrTemporal")
    else:
        for want, why in (
                ("rrtResolve(res, mu, sigma + 1e-4, nEff, h, aux.xyz, aux.w, ok, rtrHistFrames(rough), hv, len)",
                 "rtrTemporal does not resolve through rrtResolve (the moment rule) with the effective count"),
                ("okV = rtrHistFetch(", "the virtual-image candidate is not fetched bilinear + depth-validated"),
                ("rtrHistFetch(fp, pw, prev, hS, iS, wS)", "the surface candidate is not fetched bilinear + depth-validated"),
                ("length(hV - mu) <= length(hS - mu)", "the two candidates are not chosen by agreement with this frame"),
                ("2u + prev)", "rtrTemporal does not read last frame's variance / length layer"),
                ("RTH_TEXEL(px, 2u + par)", "rtrTemporal does not write this frame's variance / length layer")):
            if want not in tmp:
                missing.append(why)
        if "imageLoad(uRtHist, ivec3(hp," in tmp or "imageLoad(uRtHist, RTH_TEXEL(hp," in tmp:
            missing.append("rtrTemporal still fetches a nearest history texel")
    fetch = _body(p13, "bool rtrHistFetch(")
    if "abs(t.a - ew) < 0.02 * ew + 0.05" not in fetch:
        missing.append("rtrHistFetch does not depth-validate each bilinear tap")
    frames = _body(p13, "float rtrHistFrames(")
    if "(rtt_opts4 & 4u) != 0u ? min(n, RTR_HIST_RR) : n" not in frames:
        missing.append("rtrHistFrames does not shorten the history while DLSS RR accumulates (rtt_opts4 bit 2)")
    if "const bool tSurf = rough > RTR_STOCH_ROUGH;" not in p13 or re.search(r"tSurf = aa > 0\.02", p13):
        missing.append("the virtual image is skipped on receivers with a normal spread (aa): bumpy mirrors swim")
    if len(re.findall(r"w2B \+= w[bg] \* w[bg];", p13)) < 4:
        missing.append("the glossy gather's squared weights (w2B: the effective sample count) are not summed at "
                       "every tap (3 x 3, lattice, disc, coarse lattices)")
    if "const float nEff = w2B > 1e-12 ? wB * wB / w2B : 1.0;" not in p13:
        missing.append("pass 13 does not derive the effective sample count")

    # the hit's stratified light picks: the reflection kernels define the hook, the grid's picks take their xi, and
    # both follow the pixel + the frame counter (never the hit point)
    if "#define RT_SHADE_STOCH_XI rtrPixelXi(" not in d:
        missing.append("the RT reflection hits do not stratify their light picks (RT_SHADE_STOCH_XI): independent "
                       "draws every frame -- the crawl")
    xi = _body(d, "float rtrPixelXi(")
    if "g_frag_xy" not in xi or "RTR_XI_FRAME" not in xi:
        missing.append("rtrPixelXi does not follow the pixel (g_frag_xy) and the frame (RTR_XI_FRAME)")
    if not re.search(r"#ifdef RTR_HAVE_TILES\nuint rtrFrameNo\(\);\n#define RTR_XI_FRAME \(rtrFrameNo\(\) & 0xFFFFu\)\n"
                     r"#else\n#define RTR_XI_FRAME rtrPushFrameNo\(\)\n#endif", d):
        missing.append("RTR_XI_FRAME is not the tile header's counter (pass 11) else the push's (mirrors, pre-trace, "
                       "forward): a kernel without a counter holds its sequence (each pixel converges to its own bias)")
    if "uint rtrPushFrameNo() { return (uint(max(g_draw_push.rtr2.w, 0.0) + 0.5) >> 4u) & 0xFFFFu; }" not in d:
        missing.append("rtrPushFrameNo does not read the push's rtr2.w bits 4-19")
    if "regirHitLights(P, N, seed, xi, pl, pw, cinfo)" not in _body(d, "uint rtrRegir("):
        missing.append("the light grid's hit picks (rtrRegir) are not stratified (regirHitLights' xi)")
    # (population 1 may stride too, RS_F_XI_STRAT #1615: only population 0's branch is pinned here)
    if "const float xi = pop == 0u ? regirCellXi(cell, rg.cfg.w)" not in read(_SH + "clean_regir_build.comp"):
        missing.append("the light grid build's population 0 (the reflection / GI cells) does not stratify its xi over "
                       "the frames (regirCellXi)")

    cpp = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if len(re.findall(r"mDsRtHist = mCtx->createImageLayers\([^;]*?, 4u \* mViewCount, 1,\s*VK_FORMAT_R16G16B16A16_SFLOAT", cpp)) < 2:
        missing.append("the host does not allocate four history layers (history + variance, x 2 parities) -- "
                       "full size and placeholder")
    if "const VkImageSubresourceRange hr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};" not in cpp:
        missing.append("the host does not clear all four history layers")
    if "b[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};   // (#1608)" not in cpp:
        missing.append("the host's first-use barrier does not cover the four history layers")
    if not re.search(r"b\[2\]\.image = mDsRtHist\.image; b\[2\]\.subresourceRange = \{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS\};",
                     cpp):
        missing.append("the per-frame history barrier (RAW + WAR) does not cover the four layers")
    if "static uint32_t rtPushFrameBits() { return ((uint32_t)gFrameCount & 0xFFFFu) << 4; }" not in cpp \
            or len(re.findall(r"rp\.rtr2\[3\] = \(float\)\([^;]*\| rtPushFrameBits\(\)\);", cpp)) < 2:
        missing.append("the RT push (rtReflPush and the RT mirror's) does not carry the frame counter in rtr2.w bits "
                       "4-19 (rtPushFrameBits)")
    if not re.search(r"param3\[15\] = [^;]*\(mRrRaw \? 4u : 0u\);", cpp):
        missing.append("rtt_opts4 bit 2 (DLSS RR accumulates) is not set from mRrRaw")
    return missing


def run(ctx, result):
    hdr = ctx.path(_SH + "clean_rt_refl_temporal.h")
    if not os.path.isfile(hdr):
        result.fail(_SH + "clean_rt_refl_temporal.h is missing: the opaque RT reflections' history is clamped to a "
                    "spatial-only box and reset on noise (the 'dance')")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the temporal fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_rt_refl_temporal.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rt_refl_temporal")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    mirror = [l.split("): ", 1)[1].split(";")[0] for l in r.stdout.splitlines() if l.startswith("MIRROR")]
    strat = [l for l in r.stdout.splitlines() if l.startswith("STRAT (mirror, 8 lights)")]
    result.summary = "mirror: " + "; ".join(mirror) + (("; " + strat[0]) if strat else "") + " + wiring"
