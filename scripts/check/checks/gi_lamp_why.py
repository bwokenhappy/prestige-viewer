"""Wiring guard for RenderVKGIDebug 11: why a GI hit's lamp light passed its shadow ray.

The close-up GI specks on shadowed skin come from lamp light at GI hits (RenderVKGIHitPointShadows 0 removes them):
some hits' shadow rays toward a lamp come back unblocked. View 11 marks every GI texel whose fresh hit received
such light, coloured by the rule that let the ray through (indra/vkclean/shaders/clean_gi_lamp_why.h: the light's
own fixture, no-self-shadow, an emitter's own geometry, the facing rule, an alpha-test hole / translucent caster,
or nothing in the way). The reasons are collected in the shared walk (clean_vis_walk.glsl, RT_WALK_WHY) and travel
two ways: the inline rtrProjRay (fused kernel, the inline HIT-LIGHT / SHADE) and the visibility queue (the walk
kernel writes them into the result word, vqHitLocal reads them), then through the GI record to REUSE.

  * the fixture (scripts/check/test_gi_lamp_why.cpp, includes the header): every reason its own colour, nothing
    without GLW_PASSED, the priority, the bits fit the queue's result word;
  * source wiring: every pass site of the walk notes its reason, both transports carry them, the finish draws
    the view, the host lets RenderVKGIDebug reach 11 and shows it raw.
  * (the second round: the marks were blended away -- DLSS RR at Ultra Performance, the tonemap, a grey GI under
    them) views 11 / 12 mark the SPECKS themselves (view 9's outlier test) by their source -- the fresh hit's lamp
    reason, red a fresh non-lamp sample, green a reused one -- into a per-texel overlay buffer the RT debug pass
    draws NEAREST over the final image (clean_rt_debug.frag view 41), each mark held GI_DBG_HOLD frames; 12 the
    outliers alone, white on black.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "gi-lamp-why"
STAGE = "source"
DESCRIPTION = ("RenderVKGIDebug 11 / 12: the GI specks per texel over the final image (held, no filter / upscaler), "
               "coloured by their source and why a lamp ray passed")
GUARDS = "The diagnostic view for lamp light leaking to GI hits (the close-up GI specks on shadowed skin)."


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    sh = "indra/vkclean/shaders/"
    walk = read(sh + "clean_vis_walk.glsl")
    for pat, what in [
        (r"#ifdef RT_WALK_WHY\n#include \"clean_gi_lamp_why.h\"\nuint rt_walk_why = 0u;", "the walk's rt_walk_why"),
        (r"rt_fix_t = max\(max\(max\(tn\.x, tn\.y\), tn\.z\), 0\.0\);\s*RT_WHY\(GLW_FIX\);", "the fixture re-trace"),
        (r"if \(rtEmissiveNoShadow\(id\)\) \{ RT_WHY\(GLW_EMIT\); continue; \}", "the emitter skip"),
        (r"if \(rtFixtureBox\(id, rq, false, sa, sb\)\) \{ RT_WHY\(GLW_FIX\); continue; \}", "the fixture skip"),
        (r"if \(self_c == 1u\) \{ RT_WHY\(GLW_SELF\); continue; \}", "the no-self-shadow skip"),
        (r"if \(fol == RT_FOL_PASS\) \{ RT_WHY\(GLW_FACING\); continue; \}", "the facing pass"),
        (r"else RT_WHY\(fol == RT_FOL_LEAF \? GLW_FACING : GLW_ALPHA\);", "the alpha / leaf pass"),
    ]:
        if not re.search(pat, walk):
            missing.append("clean_vis_walk.glsl does not note " + what)
    ray = read(sh + "clean_rt_light_ray.glsl")
    if "uint w = rt_walk_why | GLW_PASSED;" not in ray or "g_gi_lamp_why |= w;" not in ray:
        missing.append("rtrProjRay does not collect its reasons into g_gi_lamp_why")
    gi = read(sh + "clean_gi_common.glsl")
    if not re.search(r"#define GI_DBG_LAMP\s+11u", gi):
        missing.append("clean_gi_common.glsl has no GI_DBG_LAMP 11")
    d = read(sh + "clean_deferred_light.comp")
    for s, what in [
        ("#define RT_WALK_WHY", "RT_WALK_WHY for the GI trace / the queue's walk"),
        ("#define RTR_LAMP_WHY_ON (gi.cfg.w == GI_DBG_LAMP)", "RTR_LAMP_WHY_ON"),
        ("VQ_RES_WALKED | (g_vq_why << 8u)", "the queue walk's reasons in the result word"),
        ("g_gi_lamp_why |= (r.q[vqResWord(r, db + VQ_SLOT_K0 + j)].w >> 8u) & GLW_MASK;", "vqHitLocal reading them"),
        ("gi_hit[ri + 3u] = uvec4(floatBitsToUint(ns.yz), g_gi_lamp_why, g_gi_outcome);", "SHADE / HIT-LIGHT storing them"),
        ("g_gi_fresh_why = q3.z & GLW_MASK;", "REUSE reading them"),
        ("if (pass == 0u) { g_gi_fresh_why = g_gi_lamp_why;", "the fused kernel taking its own"),
        ("if (dbg == GI_DBG_LAMP || dbg == GI_DBG_SPECK)", "the speck test for views 11 / 12"),
        ("const bool outlier = !noInfo && ls > 4.0 * meanO && ls > 1e-3;", "the outlier (speck) test"),
        (": giLampSpeckColour(outlier, src, why);", "the finish colouring view 11 by the speck's source"),
        ("GiDbgOverlay ov = GiDbgOverlay(g_draw_push.rtr3.zw);", "the finish writing the overlay buffer"),
        ("ov.w[i] = outlier ? (rgb | (GI_DBG_HOLD << 24u))", "the overlay's max-hold of a mark"),
    ]:
        if s not in d:
            missing.append("clean_deferred_light.comp: no " + what)
    if d.count("gi_hit[ri + 3u] = uvec4(floatBitsToUint(ns.yz), g_gi_lamp_why, g_gi_outcome);") != 2:
        missing.append("clean_deferred_light.comp: SHADE and HIT-LIGHT must both store the reasons")
    gi = read(sh + "clean_gi_common.glsl")
    if not re.search(r"#define GI_DBG_SPECK\s+12u", gi) or not re.search(r"#define GI_DBG_HOLD\s+\d+u", gi):
        missing.append("clean_gi_common.glsl has no GI_DBG_SPECK 12 / GI_DBG_HOLD")
    dbgfs = read(sh + "clean_rt_debug.frag")
    if "if (view == 41)" not in dbgfs or "GiDbgOverlayRef(a).w[t.y * ext.x + t.x]" not in dbgfs:
        missing.append("clean_rt_debug.frag does not draw the GI speck overlay (view 41) per texel")
    host = read("indra/newview/vkgfx/llcleangivk.cpp")
    if "c.debug = std::min<U32>(s_dbg, 13u);" not in host:   # (13: gi-glass ray outcome)
        missing.append("llcleangivk.cpp clamps RenderVKGIDebug below 12")
    if not re.search(r"kDnDbg\[14\] = \{0u, 0u, 1u, 2u, 3u, 1u, 1u, 0u, 1u, 1u, 1u, 1u, 1u, 1u\};", host):
        missing.append("llcleangivk.cpp does not show views 11 / 12 raw in the GI denoiser")
    if "rp.rtr3[2] = (uint32_t)(mGiDbgBuf.address & 0xFFFFFFFFull);" not in host:
        missing.append("recordGI does not hand the GI finish the overlay buffer (rtr3.zw)")
    rt = read("indra/newview/vkgfx/llcleanrtvk.h")
    if "static constexpr uint32_t kGiDbgOverlayView = 41u;" not in rt:
        missing.append("llcleanrtvk.h: kGiDbgOverlayView is not 41 (clean_rt_debug.frag's view)")
    rtc = read("indra/newview/vkgfx/llcleanrtvk.cpp")
    if "const VkDeviceAddress a = mR.mGiDbgBuf.buffer ? mR.mGiDbgBuf.address : 0u;" not in rtc:
        missing.append("LLCleanRTVK::recordDebug does not pass the overlay buffer to view 41")
    rend = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if not re.search(r"else if \(mGiNow && mGiDebug >= 11u && mGiDbgBuf\.buffer\)\s*\{\s*releaseTlas\([^;]*;\s*"
                     r"mRT->recordDebug\(cmd, colorView, width, height, drawPush, LLCleanRTVK::kGiDbgOverlayView\);", rend):
        missing.append("the renderer does not draw the GI speck overlay over the final image")
    return missing


def run(ctx, result):
    hdr = ctx.path("indra/vkclean/shaders/clean_gi_lamp_why.h")
    if not os.path.isfile(hdr):
        result.fail("indra/vkclean/shaders/clean_gi_lamp_why.h is missing: no RenderVKGIDebug 11 lamp-ray view")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the legend fixture not run (wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_gi_lamp_why.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_gi_lamp_why")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "view 11 legend (fixture) + walk notes / queue + inline transport / finish / host wiring"
