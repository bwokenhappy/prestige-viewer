"""Regression guard for GI fireflies where faces lie millimetres apart: hit-point rays that jumped thin geometry.

Every light ray leaving a ray-traced hit (GI hits, RT reflection hits, the forward blend layers' own light) started
1 cm along the SHADING normal turned toward the light (rtrProjRaySetup), the reflection hits' / blend layers'
occlusion ray 1 cm along the plane's normal (RT_OCC_RAY_SETUP), a GI-frame blend layer's sun ray 1 cm along its
normal -- and the GI reuse's visibility ray toward a reused sample stopped a flat max(1 cm, 0.1 % dl) short of it.
Anything thinner than that offset was jumped: an ear, a roof's trim over its shingles, two faces a few mm apart. A
lamp lit a hit it could not reach, a reused sample reconnected through a layer just in front of it -- rare bright
samples the reservoirs prefer and spread: GI fireflies along near-coincident layers.

Now (indra/vkclean/shaders/clean_rt_ray_origin.h, shared by the shaders and the fixture): one origin rule,
rtHitOrigin(P, Ng, err) -- Wachter & Binder's float-error offset along the GEOMETRIC normal on the leaving side, plus
the point's own position error (0 at a barycentric ray-traced hit, the depth error at a raster receiver, t's at a
parametric point); the light side by rtHitLightSide (the main view's / the sun's back-face self-shadow rule); the
reuse segment ends RT_REUSE_END_CLEAR (2 mm, the rigged pose skip's drift bound) / cos before the sample.

  * the fixture (scripts/check/test_hit_ray_origin.cpp, host C++ compiler, the header included as-is): a 4 mm ear
    2 mm above a hit with the lamp straight above (N . l = 1), the roof's near-coincident layers (0.5 .. 9 mm gaps:
    every hit under the shingles lit by the lamp above under the old rule, none under the new; the sun never
    leaked -- its rule never took the 1 cm), a reused sample under the sheet (the old end leaks, the new one tests
    every layer past 2 mm), a turned-away hit, and self-intersection: random region / region-corner / skybox
    (4000 m) triangles with light directions down to cos 0.02 -- the origin strictly on the leaving side, a float
    re-test against the hit triangle and its coplanar / convex neighbours never hits, and an unblocked sample's
    own triangle never stops its reuse ray;
  * source wiring: clean_rt_common.glsl includes the header (no second offsetRayOrigin), rtReceiverOrigin goes
    through rtHitOrigin, rtrProjRaySetup / rtHitSunRay take rtHitLightSide + rtHitOrigin, RT_OCC_RAY_SETUP and the
    blend layer's sun ray take rtHitOrigin, the walk's callers hand it the hit's plane (rt_hit_ng), the visibility
    queue's producer queues no ray for a self-shadowed pick, both reuse visibility rays end at rtReuseSegEnd, and no
    shader builds a ray origin from a flat `n * 0.01` again.
On the tree before this change the header does not exist: the check fails.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "hit-ray-origin"
STAGE = "source"
DESCRIPTION = ("hit-point light / occlusion / reuse rays leave by the hit's float error along its geometric normal "
               "(clean_rt_ray_origin.h): no flat 1 cm through thin layers")
GUARDS = ("GI fireflies where two faces lie a few mm apart (a roof's trim over its shingles, an ear): a hit's shadow "
          "ray starting 1 cm off the surface jumped the layer, a reused sample reconnected through a layer < 1 cm in "
          "front of it.")

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

    common = read(_SH + "clean_rt_common.glsl")
    if '#include "clean_rt_ray_origin.h"' not in common:
        missing.append("clean_rt_common.glsl does not include clean_rt_ray_origin.h")
    if re.search(r"\bvec3 offsetRayOrigin\(", common):
        missing.append("clean_rt_common.glsl defines its own offsetRayOrigin (the fixture would test another copy)")
    if "rtHitOrigin(" not in _body(common, "vec3 rtReceiverOrigin("):
        missing.append("rtReceiverOrigin does not go through rtHitOrigin (the main view's rule is the shared one)")

    ray = read(_SH + "clean_rt_light_ray.glsl")
    setup = _body(ray, "bool rtrProjRaySetup(Light L, vec3 P, vec3 Ng, float err,")
    if not setup:
        missing.append("clean_rt_light_ray.glsl has no bool rtrProjRaySetup(Light L, vec3 P, vec3 Ng, float err, ...)")
    else:
        if not re.search(r"rtHitLightSide\(Ng, l, rtBackfaceSelfShadow\(\), n\)", setup):
            missing.append("rtrProjRaySetup does not take its side from rtHitLightSide(Ng, l, rtBackfaceSelfShadow(), n)")
        if not re.search(r"o = rtHitOrigin\(P, n, err\);", setup):
            missing.append("rtrProjRaySetup does not take its origin from rtHitOrigin(P, n, err)")
    proj = _body(ray, "vec3 rtrProjRay(")
    if not re.search(r"if \(!rtrProjRaySetup\(L, P, rt_hit_ng, rt_hit_err,", proj):
        missing.append("rtrProjRay does not set its ray up from the hit's plane (rt_hit_ng / rt_hit_err) or traces a "
                       "self-shadowed hit")
    occ = ray[ray.find("#define RT_OCC_RAY_SETUP"):]
    occ = occ[:occ.find("void rtOccRaySetup")]
    if "o = rtHitOrigin(P, ng, err);" not in occ:
        missing.append("RT_OCC_RAY_SETUP does not take its origin from rtHitOrigin(P, ng, err)")

    shade = read(_SH + "clean_rt_shade.glsl")
    sun = _body(shade, "bool rtHitSunRay(")
    if "rtHitLightSide(Ng, L, rtBackfaceSelfShadow(), n)" not in sun or "Po = rtHitOrigin(" not in sun:
        missing.append("rtHitSunRay does not use rtHitLightSide + rtHitOrigin (the hit's light rays share one rule)")
    hit = _body(shade, "vec4 rtShadeHit(RtLitSurface s,")
    if not re.search(r"rt_hit_ng = s\.Ng;\s*rt_hit_err = 0\.0;\s*#endif\s*const vec3 local = rtHitLocalLights\(", hit):
        missing.append("rtShadeHit does not hand the light walk its hit's plane (rt_hit_ng = s.Ng; rt_hit_err = 0.0)")

    draw = read(_SH + "clean_draw_common.glsl")
    if "blendHitRayFrame(P, N, rt_hit_ng, rt_hit_err);" not in _body(draw, "vec3 blendHitLocal("):
        missing.append("blendHitLocal does not hand the light walk the layer's plane (blendHitRayFrame)")
    surf = read(_SH + "clean_surface_light.glsl")
    bo = _body(surf, "float blendHitOcc(")
    if "blendHitRayFrame(P, s.Ng, ng, err);" not in bo or "rtOccRaySetup(P, R, ng, err, o, Ro, bent);" not in bo:
        missing.append("blendHitOcc does not set its occlusion ray up from the layer's plane (blendHitRayFrame)")

    defer = read(_SH + "clean_deferred_light.comp")
    if not re.search(r"ray = rtrProjRaySetup\(lights\[li\], s\.P, s\.Ng, 0\.0, ko, kl, kt, fix\);", defer) \
            or not re.search(r"vqPush\(r, wj && ray, db \+ VQ_SLOT_K0 \+ j,", defer):
        missing.append("the visibility queue's producer does not set its light rays up from the hit's plane, or "
                       "queues a ray for a self-shadowed pick")
    if "RT_OCC_RAY_SETUP(P, R, ng, 0.0, o, Ro, g_rtr_occ_bent)" not in defer:
        missing.append("rtrOccRay does not pass its hit's error (0) to RT_OCC_RAY_SETUP")
    if "rtHitOrigin(Pl, ns, lt[j] * RT_DEPTH_REL_ERR)" not in defer:
        missing.append("the GI-frame blend layer's sun ray does not leave by rtHitOrigin(Pl, ns, lt x RT_DEPTH_REL_ERR)")
    if len(re.findall(r"const float tm = y\.dist > 0\.0 \? rtReuseSegEnd\(y\.xs, dl, abs\(dot\(y\.ns, dv\)\)\)", defer)) != 3:   # (+ the mover TRACE: RenderVKGIMoverSamples)
        missing.append("the GI reuse's three visibility rays (fused, REUSE, the mover TRACE) do not all end at rtReuseSegEnd")
    if "max(dl - max(0.01, dl * 1e-3), 0.0)" in defer:
        missing.append("a GI reuse visibility ray still stops a flat 1 cm short of its sample")

    # no flat-centimetre origin anywhere in the shaders
    flat = re.compile(r"(offsetRayOrigin|rtHitOrigin|rtReceiverOrigin)\([^;]*[+]\s*\w+(\.\w+)?\s*\*\s*0\.0[1-9]\b")
    for name in sorted(os.listdir(ctx.path(_SH))):
        if not name.endswith((".glsl", ".comp", ".frag", ".vert", ".h")):
            continue
        for i, line in enumerate(read(_SH + name).splitlines(), 1):
            if flat.search(line):
                missing.append("%s:%d builds a ray origin from a flat centimetre offset: %s" % (name, i, line.strip()))
    return missing


def run(ctx, result):
    hdr = ctx.path(_SH + "clean_rt_ray_origin.h")
    if not os.path.isfile(hdr):
        result.fail(_SH + "clean_rt_ray_origin.h is missing: hit-point rays leave 1 cm off the surface again (GI "
                    "fireflies through thin layers)")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the hit-ray-origin fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_hit_ray_origin.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_hit_ray_origin")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "ear / roof layers / reuse: no leak, no self-hit at grazing / skybox coordinates + wiring"
