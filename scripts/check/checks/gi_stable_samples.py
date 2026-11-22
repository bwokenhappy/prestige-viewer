"""Regression guard for reduced-resolution GI on small receivers: the GI sample lattice (RenderVKGIStableSamples).

Owner report 2026-11-09: at RenderVKGIScale 0.5 (worse at 0.25) avatar bodies past ~5 m swim; full-resolution GI does
not. Below full resolution GI texel m samples ONE render pixel of its cell, floor((m + 0.5 + J) r) with J the TAA /
DLSS jitter (giRenderPixel): a receiver a few texels wide is sampled at a different point every frame, and the
combine's normal / depth-weighted taps and the GI denoiser's history tests pick differently every frame.
GI_F_STABLE_SAMPLES (an A/B, default off) samples the fixed floor((m + 0.5) r) instead.

  * the fixture (scripts/check/test_gi_stable_samples.cpp, host C++ compiler): a one-scanline model of the trace, the
    GI denoiser's temporal pass and the GI_F_LAG combine under the viewer's jitter sequence -- the old lattice swims
    on a small limb at quarter extent (>= 1.5 x full resolution: the report's shape), the stable lattice cuts it by
    >= 40 % still or swaying, half extent stays bounded, the wall around the body does not take the swim, full
    resolution is unchanged;
  * source wiring (fails on the code before the option): every place that maps a GI texel to its render pixel takes
    the same phase -- giRenderPixel through giSampleJitter, the combine's same-frame upsample (deferGiFetch), the GI
    denoiser's dnJitter (DN_F_GI_STABLE, temporal ctl.y 2048 from Params.giStableSamples) -- and the host sets the
    header flag and the denoiser bit from one per-recording decision (mGiStableNow).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "gi-stable-samples"
STAGE = "source"
DESCRIPTION = ("GI below full resolution on small / moving receivers: the sample lattice (RenderVKGIStableSamples), "
               "the mover samples (RenderVKGIMoverSamples) + the swim model")
GUARDS = ("Swimming GI on distant avatars at half / quarter GI resolution (owner report 2026-11-09): the jittered "
          "GI sample lattice on receivers a few texels wide; and any texel -> pixel mapping that disagrees with the "
          "trace's when the stable lattice is on (the denoiser following another pixel's motion vector, the "
          "combine's taps offset by the jitter).")


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    common = read("indra/vkclean/shaders/clean_gi_common.glsl")
    if not re.search(r"#define GI_F_STABLE_SAMPLES 2147483648u", common):
        missing.append("clean_gi_common.glsl: no GI_F_STABLE_SAMPLES (bit 31)")
    defer = read("indra/vkclean/shaders/clean_deferred_light.comp")
    sj = defer[defer.find("vec2 giSampleJitter(int Dx)"):]
    sj = sj[:sj.find("\n}\n")]
    if not sj or "GI_F_STABLE_SAMPLES" not in sj or "ssr.taa_jitter.xy" not in sj:
        missing.append("clean_deferred_light.comp: giSampleJitter does not drop the jitter under GI_F_STABLE_SAMPLES")
    rp = defer[defer.find("ivec2 giRenderPixel(ivec2 m, ivec2 D, out vec2 r)"):]
    rp = rp[:rp.find("\n}\n")]
    if not re.search(r"const vec2 J = giSampleJitter\(D\.x\);", rp):
        missing.append("clean_deferred_light.comp: giRenderPixel does not take its phase from giSampleJitter")
    if len(re.findall(r"ssr\.taa_jitter\.xy", rp)) != 0:
        missing.append("clean_deferred_light.comp: giRenderPixel reads the jitter itself (bypassing giSampleJitter)")
    fetch = defer[defer.find("vec3 deferGiFetch(vec3 P, vec3 N, out bool ok, out float skyVis)"):]
    fetch = fetch[:fetch.find("\n}\n")]
    if not re.search(r"const vec2 j = \(ssr\.taa_params\.w > 0\.5 && \(gi\.cfg\.x & GI_F_STABLE_SAMPLES\) == 0u\) "
                     r"\? ssr\.taa_jitter\.xy : vec2\(0\.0\);", fetch):
        missing.append("clean_deferred_light.comp: deferGiFetch's upsample phase ignores GI_F_STABLE_SAMPLES")
    dn = read("indra/vkclean/shaders/clean_dn_common.glsl")
    if not re.search(r"#define DN_F_GI_STABLE 2048u", dn) or not re.search(r"vec2 dnJitter\(\) \{[^\n]*!dnGiStable\(\)", dn):
        missing.append("clean_dn_common.glsl: dnJitter does not drop the phase under DN_F_GI_STABLE (2048)")
    dncpp = read("indra/newview/vkgfx/llcleandenoisevk.cpp")
    if not re.search(r"\(p\.giStableSamples && !p\.radiance \? 2048u : 0u\)", dncpp):
        missing.append("llcleandenoisevk.cpp: Params.giStableSamples does not set temporal ctl.y 2048")
    gi = read("indra/newview/vkgfx/llcleangivk.cpp")
    if not re.search(r"GI_F_STABLE_SAMPLES = 2147483648u", gi):
        missing.append("llcleangivk.cpp: no host GI_F_STABLE_SAMPLES matching the shader's bit 31")
    if not re.search(r"mGiStableNow = cfg\.stable && !mGiVrNow;\n\s*if \(mGiStableNow\) h\.cfg\[0\] \|= GI_F_STABLE_SAMPLES;", gi):
        missing.append("llcleangivk.cpp: the header flag is not set from mGiStableNow (giWriteHeader)")
    if not re.search(r"dp\.giStableSamples = mGiStableNow;", gi):
        missing.append("llcleangivk.cpp: the GI denoiser's Params.giStableSamples is not mGiStableNow (recordGITail)")
    settings = read("indra/newview/app_settings/settings.xml")
    if "<key>RenderVKGIStableSamples</key>" not in settings:
        missing.append("settings.xml: no RenderVKGIStableSamples")
    # (RenderVKGIMoverSamples, clean_gi_movers.glsl) the mover path: the header word on both sides, the kernel built and
    # recorded after the trace stages, the consumer's read, the memory released when off, the setting
    hdr = read("indra/newview/vkgfx/llcleanrenderervk.h")
    if not re.search(r"uvec4 mov;", common) or not re.search(r"uint32_t mov\[4\];", hdr) \
            or "sizeof(GiHeader) == 5424" not in hdr:   # (gi-glass: + xcfg, stat5 .. stat7)
        missing.append("GiHeader: the mover word (mov) is not on both sides (5424 B)")
    spv = read("indra/vkclean/regen_world_spv.py")
    if not re.search(r'"kWorldDeferGiMoverSpv", \[[^\]]*"-DCLEAN_GI_TRACE", "-DCLEAN_GI_MOVER"\]', spv):
        missing.append("regen_world_spv.py: no kWorldDeferGiMoverSpv (-DCLEAN_GI_TRACE -DCLEAN_GI_MOVER)")
    if not re.search(r"#ifdef CLEAN_GI_MOVER\n(?:.*\n)*?void DEFER_MAIN\(\)\n\{\n    const vec2 R2", defer):
        missing.append("clean_deferred_light.comp: no CLEAN_GI_MOVER main")
    rf = defer[defer.find("SurfaceRefl deferReflIn(SurfaceLit s)"):]
    rf = rf[:rf.find("\n}\n")]
    if "deferGiMoverRead(" not in rf or "giMoverFetch(" not in defer:
        missing.append("clean_deferred_light.comp: deferReflIn does not read the mover GI (deferGiMoverRead -> giMoverFetch)")
    tail = gi[gi.find("void LLCleanRendererVK::recordGITail("):]
    if not re.search(r"\{\n    recordGIMovers\(cmd, p, viewOff, onAsync\);", tail):
        missing.append("llcleangivk.cpp: recordGITail does not record the mover pass first")
    if not re.search(r"else if \(mGiMov\.buffer\)\n\s*\{\n\s*retireBuffer\(mGiMov\);", gi):
        missing.append("llcleangivk.cpp: the mover buffer is not retired when the setting goes off")
    if "<key>RenderVKGIMoverSamples</key>" not in settings or "<key>RenderVKGIMoverBudget</key>" not in settings:
        missing.append("settings.xml: no RenderVKGIMoverSamples / RenderVKGIMoverBudget")
    return missing


def run(ctx, result):
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the swim model not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_gi_stable_samples.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_gi_stable_samples")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", src, "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = ("one texel->pixel phase (trace, combine, GI denoiser) + mover-sample wiring; quarter-extent swim "
                      "reproduced, the mover samples at full-resolution quality in the model")
