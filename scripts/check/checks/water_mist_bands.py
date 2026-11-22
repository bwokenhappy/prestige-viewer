"""Source guard: the water mist (#963) reads and integrates its thin layer per slab, never by
geometric slab progress or the slab's midpoint (zoomed-out concentric bands over water).

The camera medium grid stores, per slice, the transport to the slab's FAR end. The mist hugs the
water (a few metres), so in a long far slab all of its optical depth sits at the slab's low end:
  OPTICAL-READ   cmAt blends a point inside a slab by its OPTICAL progress (cmHeightTau over
                 [slab begin, point] / over the slab) while the mist is on -- a geometric blend
                 reads (progress) of the mist on the water and the mist under the terrain on
                 land: a sawtooth at every slice boundary.
  IN-LAYER       cmWaterMistTau samples its noise / cover in the layer (point S) and LODs the
                 noise by the chord through the layer, not by the slab's midpoint and length.
  EXACT-COLUMN   the vertical profile stays integrated exactly (cmLayerColumn).
"""
import re

NAME = "water-mist-bands"
STAGE = "source"
DESCRIPTION = "water mist slabs are read by optical progress and sampled inside the layer (no slice bands)"
GUARDS = "#963 concentric camera-centred bands over water when zoomed out (mist sawtooth per froxel slice)."

MEDIUM = "indra/vkclean/shaders/clean_medium.glsl"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function(text, signature):
    at = text.find(signature)
    if at < 0:
        return None
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def run(ctx, result):
    src = _strip_comments(open(ctx.path(MEDIUM), encoding="utf-8").read())
    bad = []

    at = _function(src, "CameraTransport cmAt(")
    if not at:
        bad.append("cmAt not found in clean_medium.glsl; update this check")
    else:
        if not re.search(r"cmHeightTau\(O,V,near0,far0\)", at) or \
           not re.search(r"cmHeightTau\(O,V,near0,clamp\(distance,near0,far0\)\)", at):
            bad.append("OPTICAL-READ: cmAt no longer measures the point's optical progress through its slab")
        if "c=slab-1.0+progress" not in at.replace(" ", ""):
            bad.append("OPTICAL-READ: cmAt's slice coordinate is not set from the optical progress")
        if "progress>=0.0?progress:" not in at.replace(" ", ""):
            bad.append("OPTICAL-READ: the first slab's scale ignores the optical progress")

    tau = _function(src, "float cmWaterMistTau(")
    if not tau:
        bad.append("cmWaterMistTau not found in clean_medium.glsl; update this check")
    else:
        flat = tau.replace(" ", "")
        if "weatherPrecipOpen(S)" not in flat or "vec3(S.xy/size,S.z/size*2.0)" not in flat:
            bad.append("IN-LAYER: the mist's cover / noise are no longer sampled inside the layer (S)")
        if "len=(b-a)*inLayer" not in flat:
            bad.append("IN-LAYER: the noise LOD is not taken from the chord through the layer")
        if flat.count("cmLayerColumn(") < 2:
            bad.append("EXACT-COLUMN: the mist / spray profiles are no longer integrated by cmLayerColumn")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "cmAt reads by optical progress; cmWaterMistTau samples inside the layer"
