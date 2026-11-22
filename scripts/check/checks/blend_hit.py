"""Regression guard for the blend layers' OWN light (RenderVKBlendHitLighting): no borrowed or unshadowed light.

An alpha-blended layer -- glass, a decal, makeup, hair -- has no pixel history and no denoiser, the case of a
ray-traced reflection hit, so it is shaded the way a hit is (clean_draw_common.glsl blendHitLocal: the hits' light
walk clean_rt_hit_lights.glsl rtHitLocalLights over the froxel cell, the 2 strongest lights exact + 2 sampled
picks, a shadow ray each; clean_surface_light.glsl: its own occlusion ray for the GI fallback and the environment).
Before it, the forward walk lit every point light (and, by default, every ray-traced projector) UNSHADOWED, and
the borrowing fixes took the opaque surface's lamp record (#1440) or GI occlusion record instead -- light that is
not the layer's own. Those records are deleted (the blend-borrowing check keeps them out).

The check runs the SHADERS' OWN code: for every forward lit FS variant regen_world_spv.py builds with ray queries
(clean_draw.frag's, clean_oit_resolve.frag -DOIT_LIT's), glslc -E with that variant's flags expands every hook, the
layer's light walk and GI (rtHitLocalLights, giFwdOn, giFwdCache, blendHitEnv, giForwardRefl + the constants they
name) are cut out, turned into C++ and compiled into scripts/check/test_blend_hit.cpp: a sealed room lights the
layer with nothing (from the froxel cell and the cluster list), the room's own lamp is the mean, the strongest
lights are exact, the noise is below plain NEE with the same rays, the budget holds, the glare stays, and the GI
fallback is dark in a sealed room and kept outdoors. Plus the wiring the fixture cannot see: the setting (A/B,
Persist 0), the host's fwd.w bit and the lit FS's use of the walk. On the code before it the blend FS compiles no rtHitLocalLights: the check fails.

The dark circle and the grain (owner, 2026-11-08): the layer's light rays (rtrProjRaySetup / rtrProjRay, cut too)
skipped the own-fixture rule, so a lamp inside its own opaque bulb prim lit no layer point in its reach, and the glare
of every light was added unshadowed -- a blocked shiny lamp left the layer unlit and nearly opaque: a dark disc over
the lamp's reach (DISC, VISGLARE, BELOW). And 2 exact + 2 picks re-drew the picks every frame wherever a fourth light
reached the layer (STABLE: 1, 2, 4, 8 lights must be noise-free). Both fail on 276cdd2b31.

The cloud mosaic (#1499): the GI fallback's ambient took the probes x the occlusion ray along R, and any blocker within
4 km zeroed it -- a cloud's underside seen from below (R toward the distant ground) was black wherever the radiance
cache had no cell, next to cells that had. The diffuse fallback now counts an opaque blocker past the GI ray length as
the probes' far field (blendHitDiffuseVis, the GI miss rule; cut too): FAR, RAMP, TRANSLUCENT and MOSAIC, while a near
wall stays SEALED. FAR / RAMP / MOSAIC fail on the old rule (blendHitDiffuseVis returning the plain visibility).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

NAME = "blend-hit"
STAGE = "source"
DESCRIPTION = "the blend layers' own light: hit-style walk + own GI occlusion"
GUARDS = ("Alpha-blend layers lit by lamps through walls: the forward walk's unshadowed point lights while "
          "RenderVKBlendHitLighting is on, a noisy or unbounded shadow-ray budget, the sealed-room glass lit by the "
          "probes / sky, a far-blocked layer (a cloud's underside over distant ground, #1499) unlit off the GI "
          "cache's cover: a light / dark mosaic.")

_FNS = ("vec3 rtHitLocalLights(", "bool giFwdOn(", "void giFwdCache(", "vec3 blendHitEnv(", "float blendHitDiffuseVis(",
        "SurfaceRefl giForwardRefl(",
        "bool rtrProjRaySetup(", "vec3 rtrProjRay(")
# (cut when the variant has it: the forward own-fixture size limit; a tree without it runs with the fixture's stub,
#  so its DISC case fails on the light rays themselves)
_OPT_FNS = ("float blendHitFixMax(",)
_CONSTS = re.compile(r"^const (?:uint|float|int) (?:SURF_\w+|BLEND_HIT_\w+)\b[^;]*;$", re.M)
_LIT = re.compile(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+)(?![\w.])")


def _to_cpp(fn):
    """The preprocessed GLSL as C++ for the fixture's shims."""
    fn = re.sub(r"\[\[(?:dont_)?unroll\]\]", "", fn)
    fn = re.sub(r"\b(?:out|inout)\s+(\w+)\s+(\w+)", r"\1& \2", fn)
    fn = re.sub(r"\.(?:rgb|xyz)\b(?!\s*\()", ".xyz()", fn)
    fn = re.sub(r"\.xy\b(?!\s*\()", ".xy()", fn)
    fn = re.sub(r"\.r\b", ".x", fn)
    fn = re.sub(r"\.g\b", ".y", fn)
    fn = re.sub(r"\.b\b", ".z", fn)
    return _LIT.sub(lambda m: m.group(1) + "f", fn)


def _body(text, i):
    """The function starting at i, through its matching closing brace (one-line bodies too)."""
    j = text.find("{", i)
    if j < 0:
        return None
    depth = 0
    for k in range(j, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[i:k + 1] + "\n"
    return None


def _cut(text):
    """The constants + the functions; None when the variant has no blend-layer walk."""
    if "vec3 rtHitLocalLights(" not in text or "vec3 blendHitEnv(" not in text:
        return None
    parts = _CONSTS.findall(text)
    for fn in _OPT_FNS + _FNS:   # (callees first)
        i = text.find("\n" + fn)
        if i < 0:
            if fn in _OPT_FNS:
                continue
            return None
        body = _body(text, i + 1)
        if body is None:
            return None
        # (the shader's light ray under its own name: the fixture's rtrProjRay counts the walk's rays, then calls it)
        parts.append(body.replace("vec3 rtrProjRay(", "vec3 rtrProjRayReal(", 1))
    return "\n".join(parts)


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    st = read("indra/newview/app_settings/settings.xml")
    m = re.search(r"<key>RenderVKBlendHitLighting</key>\s*<map>(.*?)</map>", st, re.S)
    if not m:
        missing.append("settings.xml has no RenderVKBlendHitLighting (the A/B toggle)")
    elif not (re.search(r"<key>Persist</key>\s*<integer>0</integer>", m.group(1))
              and re.search(r"<key>Type</key>\s*<string>Boolean</string>", m.group(1))
              and re.search(r"<key>Value</key>\s*<integer>1</integer>", m.group(1))):
        missing.append("RenderVKBlendHitLighting is not a Boolean, default 1, Persist 0 (an A/B toggle never persists)")
    r = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if not re.search(r"mLightInfoCPU\.fwd\[3\] = \(\(blendHit && mLitRt\) \? 1u : 0u\)[^;]*\(fixtureMm << 8\)[^;]*;", r):
        missing.append("the light info's fwd.w is not RenderVKBlendHitLighting on a ray-query lit frame (bit 0) + the "
                       "own-fixture size in mm (bits 8..)")
    if not re.search(r'LLCachedControl<F32> s_fixture\(gSavedSettings, "RenderVKReSTIRFixtureSize"', r):
        missing.append("fwd.w's fixture size is not RenderVKReSTIRFixtureSize (pass 15's / the GI hits' limit)")
    dc = read("indra/vkclean/shaders/clean_draw_common.glsl")
    if not re.search(r"#ifdef CLEAN_BLEND_HIT\s*\n(?://[^\n]*\n)*#define RT_WALK_FIXTURE\s*\n#endif\s*\n#include \"clean_rt_common.glsl\"", dc):
        missing.append("the blend layers' FSs do not compile the walk's own-fixture rule (RT_WALK_FIXTURE before "
                       "clean_rt_common.glsl): their light rays stop at the lamp's own bulb / shade")
    s = read("indra/vkclean/shaders/clean_surface_light.glsl")
    if not re.search(r"vec3 local = \(blendHitOn\(\) && !bf\) \? blendHitLocal\(", s):
        missing.append("lightSurface does not light a blended layer through blendHitLocal")
    # (RenderVKBlendFrontLight) the front layer's unshadowed walk is never left unshadowed: x pass 14's resolved ratio
    if not re.search(r"if \(bf\) \{ local \*= bfv\.g; glare \*= bfv\.g; \}", s):
        missing.append("the front blend layer's local light is not shadowed by THE LAYER'S LIGHT ratio (bfv.g): "
                       "lamps through walls on the front layer")
    o = read("indra/vkclean/shaders/clean_oit_resolve.frag")
    if re.search(r"^\s*#define CLEAN_BLEND_HIT\b", o, re.M):
        missing.append("the WBOIT weighted-mean surface must not launch per-layer hit-lighting rays")
    return missing


def _variants(regen):
    out = []
    for e in regen.SHADERS:
        flags = e[3] if len(e) > 3 else ()
        if "-DCLEAN_PROJ_SHADOW_RAYQUERY" not in flags:
            continue
        if e[0] == "clean_draw.frag" and not any(f in flags for f in ("-DCLEAN_CAPTURE", "-DCLEAN_GBUFFER",
                                                                       "-DCLEAN_OIT_GBUF", "-DCLEAN_BASIC")):
            out.append(e)
        # OIT resolves a weighted mean, not a physical layer; intentionally no hit walk.
    return out


def run(ctx, result):
    missing = _wiring(ctx)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    glslc = os.environ.get("GLSLC") or shutil.which("glslc")
    if not cxx or not glslc:
        if missing:
            result.fail("\n".join(missing))
            return
        result.warn("no C++ compiler / glslc: the layers' walk not run (wiring checked)")
        result.summary = "wiring only"
        return
    vk = ctx.path("indra/vkclean")
    sys.path.insert(0, vk)
    try:
        import regen_world_spv as regen
    finally:
        sys.path.pop(0)
    entries = _variants(regen)
    names = [e[2] for e in entries]
    jobs = [regen.variant(e) for e in entries]

    def pre(job):
        path, stage, flags = job
        r = subprocess.run([glslc, "-E", "-fshader-stage=" + stage, "--target-env=vulkan1.3",
                            *[f for f in flags if f.startswith(("-D", "-I"))], path], capture_output=True, text=True)
        return _cut(r.stdout) if r.returncode == 0 else ("ERR", r.stderr)

    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        cut = list(ex.map(pre, jobs))
    walks = {}
    for n, c in zip(names, cut):
        if isinstance(c, tuple):
            result.fail("\n".join(missing + ["glslc -E failed for %s:\n%s" % (n, c[1][:2000])]))
            return
        if c is None:
            missing.append("%s does not light its blended layers through the hit walk (rtHitLocalLights / blendHitEnv)" % n)
            continue
        walks.setdefault(c, []).append(n)
    for want in ("kWorldFragBlendRtSpv",):
        if want not in names:
            missing.append("regen_world_spv.py builds no %s (the blend layers' ray-query FS)" % want)
    if not walks:
        result.fail("\n".join(missing + ["no forward variant carries the blend layers' own light"]))
        return
    src = ctx.path("scripts/check/test_blend_hit.cpp")
    inc = ctx.path("indra/vkclean/shaders")
    failures = []
    notes = []

    def build_run(item):
        k, (fn, ns) = item
        with tempfile.TemporaryDirectory() as tmp:
            f = os.path.join(tmp, "walk.inc")
            with open(f, "w") as o:
                o.write(_to_cpp(fn))
            exe = os.path.join(tmp, "t")
            defs = ["-I" + inc, '-DBH_FN_FILE="%s"' % f]
            if "g_rt_fwd_hit" in fn:
                defs.append("-DBH_RT_FWD")
            if "float blendHitFixMax(" in fn:
                defs.append("-DBH_HAS_FIXMAX")
            c = subprocess.run([cxx, "-std=c++17", "-O2", "-w", *defs, src, "-o", exe], capture_output=True, text=True)
            if c.returncode:
                return ns, "the fixture does not compile against this walk (a new hook needs a shim?):\n" + c.stderr[:3000], ""
            r = subprocess.run([exe], capture_output=True, text=True)
            return ns, (r.stdout + r.stderr) if r.returncode else None, r.stdout

    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        for ns, err, out in ex.map(build_run, enumerate(walks.items())):
            if err:
                failures.append("%s:\n%s" % (", ".join(ns), err.strip()))
            else:
                if not notes:
                    notes = [l for l in out.splitlines() if l.startswith("NOISE-TAIL")]
    if failures or missing:
        result.fail("\n".join(missing + failures))
        return
    result.summary = ("%d distinct walks over %d variants: sealed 0, own lamp, exact key/fill, budget, no dark disc, "
                      "1-8 lights noise-free, GI sealed / far / no mosaic%s" % (len(walks), sum(len(v) for v in walks.values()),
                                                       ("; " + "; ".join(n.replace("NOISE-TAIL ", "") for n in notes))
                                                       if notes else ""))
