"""Regression guard for the forward RT reflections' noise: the blend / water layers' traced reflection used raw.

Pass 14 (clean_deferred_light.comp, RenderVKRTReflectionsForward) traces one mirror ray per 2 x 2 block through the
jittered camera. The opaque reflections have a resolve (pass 13: lattice gather, lobe filter, rtrTemporal); the
forward ones had none. Each frame's block rays went to the forward FS raw, read from the nearest half-resolution
texel. On a normal-mapped layer (the owner's pool "water" prim, 2026-11-08) the jitter moves each block's ray to
another normal every frame, so the reflection jumped between parts of the room: heavy grain, "held back / not filling
in the picture". DLSS RR treats blend as TRANSP and only upscales it.

Now pass 14 resolves its own rays (indra/vkclean/shaders/clean_rt_fwd_temporal.h, shared with the fixture): the
same-layer 3 x 3 of the workgroup's tile, the reprojected history (surface or virtual image, depth-validated) with
its own luminance moment, a clamp box and an anti-lag test on the larger of the spatial and temporal spread, an
8-frame EMA. The FSs upsample it per pixel (rtFwdIn: the 2 x 2 texels, bilinear x the same-layer depth test).

  * the fixture (scripts/check/test_rt_fwd_temporal.cpp, the header as-is): an 8 x 8 tile of block rays reflecting a
    room with a bright window / a small bright glint in a rippled layer -- the resolved texels' frame-to-frame RMS
    <= 0.4 x the raw rays' and their mean within 3 % (no darkening); a switched light in the reflection followed
    within 4 frames; no stale history on a disocclusion; a steady reflection held exactly;
  * wiring: pass 14 resolves (shared tile, rftReproject, rftResolve, the history / moment regions by the push
    parity), the host allocates the five regions and pushes the parity + the previous-camera bit, both RT FORWARD
    barriers take the history's RAW, rtFwdIn upsamples with the depth test.
  * (the flicker, same report) the blend layer's own occlusion ray (clean_surface_light.glsl blendHitOcc) follows the
    layer's smooth normal s.Ng, not the normal-mapped one: a binary far-field test along the normal map's R flipped
    from pixel to pixel at the map's frequency and moved with the TAA jitter every frame.
On the tree before this change the header does not exist: the check fails.

THE WINDOW FLICKER (owner video, 2026-11-09: soft round blobs on window glass brightening / darkening frame to frame,
the size of a 0.25 m radiance-cache cell at the glass):
  * the resolve passed REGION-WIDE noise (every ray of the 3 x 3 agreeing: the hits' cache ambient) almost unfiltered:
    its moment was blended at the anti-lag weight and centred on the resolved colour, so one reset zeroed the spread
    and every next frame reset again (the fixture's COHERENT case: 60 % of 5 % noise, 95 % of 20 % came through).
    Now the resolve takes pass 13's rule (clean_rt_refl_temporal.h rrtResolve: per-channel log variance kept across
    resets) on the own ray mixed with its 3 x 3 -- COHERENT <= 0.4 x the raw frame-to-frame change, unbiased, a real
    step under that noise (COHERENT CHANGE) followed in 4 frames, and a light switched off (CHANGE OFF) too;
  * the glass's own GI read the cache cells raw (giFwdCache -> giCacheForward, no history: a blend fragment has no GI
    pixel), carrying each cell's short-window noise. Now pass 14 resolves the front layer's cache read over frames
    (rows [5 h, 10 h) of uRtFwd, the layer's depth in |a| of the reflection rows) and the FS takes it (rtFwdGiIn).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "rt-fwd-temporal"
STAGE = "source"
DESCRIPTION = "the forward RT reflections (blend / water layers) are resolved and accumulated, not used raw"
GUARDS = ("Grain / 'dancing' on reflective alpha-blend layers (a water prim, rippled glass): pass 14's one jittered "
          "mirror ray per 2 x 2 block reached the forward FS raw, with no resolve or history; and the blend layer's "
          "occlusion ray along the normal-mapped R flickering per pixel per frame. Soft blob flicker on window glass "
          "(2026-11-09): region-wide noise through the resolve's collapsing moment, and the glass's raw per-frame "
          "radiance-cache read.")

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
    i = d.find("#elif CLEAN_DEFER_PASS == 14")
    p14 = d[i:d.find("#elif CLEAN_DEFER_PASS == 15", i)] if i >= 0 else ""
    if not p14:
        missing.append("clean_deferred_light.comp has no pass 14 section")
    else:
        for want, why in (
                ('#include "clean_rt_fwd_temporal.h"', "pass 14 does not include clean_rt_fwd_temporal.h"),
                ("shared vec4 s_rft[64];", "pass 14 keeps no shared tile of its rays (the resolve's 3 x 3)"),
                ("barrier();", "pass 14 has no barrier between its rays and the resolve"),
                ("rftReproject(", "pass 14 does not reproject a history"),
                ("rftResolve(outv.rgb, mu, sg, n, hist, hm.rgb, hm.a, hok, rmom.rgb, rmom.a, wR)",
                 "pass 14 does not resolve its rays with their temporal variance / length (rftResolve)"),
                ("hsz.y * int(1u + par)", "pass 14 does not write the history region of this frame's parity"),
                ("hsz.y * int(3u + par)", "pass 14 does not write the moment region of this frame's parity"),
                ("hsz.y * int(3u + (par ^ 1u))", "pass 14 does not read last frame's moment region"),
                ("imageSize(uRtFwd).y >= RFT_REGIONS * hsz.y", "pass 14 does not test for all RFT_REGIONS regions"),
                # THE LAYER'S GI (the window flicker)
                ("giCacheForward(ls.P, nf, linfo.cam_pos.xyz, wf, inSph)",
                 "pass 14 does not read the radiance cache at the front blend layer (THE LAYER'S GI)"),
                ("res.a = -layerZ;", "pass 14 does not mark a blend layer without a reflection with its depth (-z)"),
                ("rfgReproject(rfgP, hsz, prevRefl, gq)", "pass 14 does not reproject the layer GI's history"),
                ("rftResolve(giv.rgb, mu, sg, gn, gh.rgb, gm.rgb, gm.a, ghok, gmom.rgb, gmom.a, wG)",
                 "pass 14 does not resolve the layer GI over frames"),
                ("hsz.y * 5)), gres", "pass 14 does not write the resolved layer GI (rows [5 h, 6 h))"),
                ("hsz.y * int(6u + par))), gres", "pass 14 does not write the layer GI's history of this parity"),
                ("hsz.y * int(8u + par))), min(gmom, vec4(60000.0))",
                 "pass 14 does not store the layer GI's variance / length"),
                ("hsz.y * int(3u + par))), min(rmom, vec4(60000.0))",
                 "pass 14 does not store the reflection's variance / length")):
            if want not in p14:
                missing.append(why)
        if re.search(r"if \(hp\.x >= hsz\.x \|\| hp\.y >= hsz\.y\) return;", p14):
            missing.append("pass 14 returns before the resolve's barrier (non-uniform barrier)")
    if not re.search(r"layout\(set = 3, binding = 12, rgba16f\) uniform image2DArray uRtFwd;", d):
        missing.append("pass 14's uRtFwd is not read-write (the history lives in it)")
    hdr = read(_SH + "clean_rt_fwd_temporal.h")
    if not re.search(r"#define RFT_REGIONS\s+15\b", hdr):
        missing.append("clean_rt_fwd_temporal.h does not define the image's 15 regions (RFT_REGIONS)")
    res = _body(hdr, "RFT_FN vec3 rftResolve(")
    if "rrtResolve(" not in res or '#include "clean_rt_refl_temporal.h"' not in hdr:
        missing.append("rftResolve does not take the opaque reflections' temporal rule (clean_rt_refl_temporal.h "
                       "rrtResolve: per-channel log variance kept across resets) -- a moment blended at the anti-lag "
                       "weight lets region-wide noise through, an absolute spread ghosts a light switched off")
    dc = read(_SH + "clean_draw_common.glsl")
    fin = _body(dc, "vec4 rtFwdIn(vec3 P)")
    if "for (int k = 0; k < 4; ++k)" not in fin or "sz.y / RFT_REGIONS" not in fin:
        missing.append("rtFwdIn does not upsample the resolved region (the 2 x 2 same-layer texels of rows [0, h / 10))")
    gin = _body(dc, "bool rtFwdGiIn(vec3 P, out vec3 Ewf, out float wf)")
    if not gin or "5 * hsz.y" not in gin or "abs(texelFetch(uRtFwd, ivec3(t, int(CLEAN_VIEW)), 0).a)" not in gin:
        missing.append("clean_draw_common.glsl has no rtFwdGiIn reading the resolved layer GI (rows [5 h, 6 h), depth |a|)")
    sl = read(_SH + "clean_surface_light.glsl")
    occ = _body(sl, "float blendHitOcc(")
    if not re.search(r"const vec3 R = reflect\(-mainViewDirection\(P, pc\.u_cam_time\.xyz\), s\.Ng\);", occ):
        missing.append("blendHitOcc's ray follows the normal-mapped normal (per-pixel, per-frame flicker): want s.Ng")
    gfc = _body(sl, "void giFwdCache(SurfaceLit s, vec3 P)")
    i_res, i_raw = gfc.find("rtFwdGiIn(P, Ewf, wf)"), gfc.find("GI_FWD_CACHE_READ(s, P, g_gi_fwd_wf, g_gi_fwd_E)")
    if i_res < 0 or i_raw < 0 or i_res > i_raw:
        missing.append("giFwdCache does not take pass 14's resolved layer GI before its own raw cache read (the blobs)")
    cpp = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if not re.search(r"mDsRtFwd = screen\(fw, full \? fh \* 15u : 1u,", cpp):
        missing.append("the host does not allocate the forward reflections' fifteen regions (RFT_REGIONS: resolved, "
                       "history x 2, variance x 2, layer GI, its history x 2, its variance x 2, the layer's light, "
                       "its history x 2, its variance x 2)")
    # (bits 4-19 the frame counter, rtPushFrameBits: the hits' stratified light picks)
    if not re.search(r"rp\.rtr2\[3\] = \(float\)\(\(mRtReflParity & 1u\) \| \(mTemporalOn \? 2u : 0u\)", cpp):
        missing.append("rtReflPush does not hand pass 14 the history parity + the previous-camera bit (rtr2.w)")
    fwd_bar = cpp[cpp.find("// RT FORWARD writes mDsRtFwd"):][:900]
    vq = read("indra/newview/vkgfx/llcleanvisqueuevk.cpp")
    for name, src in (("recordRtReflect", fwd_bar), ("the M6 level-1 barrier", vq)):
        if not re.search(r"fb\.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT \| VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;",
                         src):
            missing.append("%s: the RT FORWARD barrier does not wait for last frame's history writes (RAW)" % name)
    return missing


def run(ctx, result):
    hdr = ctx.path(_SH + "clean_rt_fwd_temporal.h")
    if not os.path.isfile(hdr):
        result.fail(_SH + "clean_rt_fwd_temporal.h is missing: the forward RT reflections reach the blend / water "
                    "layers raw (one jittered ray per 2 x 2 block, no resolve, no history)")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the resolve fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_rt_fwd_temporal.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rt_fwd_temporal")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    steady = [l.split(": ", 1)[1] for l in r.stdout.splitlines() if l.startswith("STEADY")]
    coherent = [l.split(": ", 1)[1] for l in r.stdout.splitlines() if l.startswith("COHERENT (20")]
    result.summary = "resolve: " + "; ".join(steady[:2] + coherent[:1]) + " + wiring"
