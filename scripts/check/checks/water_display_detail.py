"""Regression guard for #1637: the water stays sharp under an upscaler and its foam stays on the water.

Two water-specific faults, both in indra/vkclean/shaders/clean_water.frag + clean_water_fx.glsl:

  * BLUR. Every other surface samples its textures with taaLodBias (clean_frame_info.glsl): the render scale's
    log2(render / display), so FSR / DLSS get the DISPLAY resolution's mip and rebuild its detail from the jittered
    frames. The water sampled its normal maps, its wave-slope filter and its foam pattern per RENDER pixel: at Ultra
    Performance (1/3) the waves were 1.6 mips blurrier than everything around them and the foam lace collapsed to
    a flat white 3x nearer. waterPixelScale() (render pixel in display pixels, gated like taaLodBias) now feeds the
    normal maps' bias, seaSurface's footprint and the foam's pattern derivatives.
  * WHITE OBJECTS. The contact foam band is measured in depth below the surface. Right for a shore; on a steep
    face (piles, hulls, legs) every pixel of its submerged top part counted as "at the contact", so the face was
    painted white in its own shape. The FS reads the slope of the surface behind the water (screen derivatives of
    the reconstructed position) and waterFxFoam narrows the band on steep faces to a waterline.
"""
import re

NAME = "water-display-detail"
STAGE = "source"
DESCRIPTION = ("the water samples its detail at the display resolution under an upscaler, and its contact foam "
               "narrows to a waterline on steep faces")
GUARDS = ("Blurry water under FSR / DLSS at low render scales and foam painting submerged piles, hulls and legs "
          "white (#1637)")

WATER = "indra/vkclean/shaders/clean_water.frag"
FX = "indra/vkclean/shaders/clean_water_fx.glsl"


def _read(ctx, rel, result):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        result.fail("%s: missing" % rel)
        return ""


def _function(code, signature):
    at = code.find(signature)
    if at < 0:
        return ""
    open_at = code.find("{", at)
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at:i + 1]
    return ""


def run(ctx, result):
    water = _read(ctx, WATER, result)
    fx = _read(ctx, FX, result)
    if not water or not fx:
        return

    scale = _function(water, "float waterPixelScale()")
    if not scale:
        result.fail("%s: waterPixelScale() is gone" % WATER)
    else:
        if "taa_scale.y" not in scale or "taa_params.w" not in scale:
            result.fail("%s: waterPixelScale() no longer follows the render scale behind the jitter gate "
                        "(ssr.taa_scale.y / ssr.taa_params.w, as taaLodBias)" % WATER)
        if not re.search(r"#ifdef\s+WATER_OPAQUE\s+return\s+1\.0", scale):
            result.fail("%s: waterPixelScale() must stay 1 in the WATER_OPAQUE capture build" % WATER)

    tex = _function(water, "vec3 waterNormalTex(")
    if not re.search(r"texture\([^;]*,\s*uv\s*,\s*lodBias\s*\)", tex):
        result.fail("%s: waterNormalTex() samples the normal maps without the display-mip bias" % WATER)
    main = _function(water, "void main()")
    if len(re.findall(r"waterNormalTex\([^)]*texBias\)", main)) != 6:
        result.fail("%s: every normal-map sample (2 maps x 3 layers) must take texBias = log2(waterPixelScale())"
                    % WATER)
    if not re.search(r"texBias\s*=\s*log2\(\s*pxScale\s*\)", main):
        result.fail("%s: texBias is not log2 of the render pixel's display scale" % WATER)
    if not re.search(r"seaSurface\(v_uv,[^;]*fwidth\(v_uv\)\)\s*\*\s*pxScale", main):
        result.fail("%s: the waves' slope filter footprint is not a display pixel" % WATER)
    if not re.search(r"fxdx\s*=\s*dFdx\(P\.xy\)\s*\*\s*pxScale", main) or \
            not re.search(r"fxdy\s*=\s*dFdy\(P\.xy\)\s*\*\s*pxScale", main):
        result.fail("%s: the foam pattern derivatives are not a display pixel's" % WATER)

    # The contact's slope: taken before the under-water early return (derivatives in uniform control flow),
    # and handed to the foam.
    steep_at = main.find("fxSteep = 1.0 - smoothstep(")
    under_at = main.find("if (under)")
    if steep_at < 0:
        result.fail("%s: the contact slope (fxSteep) is no longer read from the scene behind the water" % WATER)
    elif under_at >= 0 and steep_at > under_at:
        result.fail("%s: the contact slope's derivatives must be taken before the under-water early return" % WATER)
    if "waterFxFoam(P, fxDepth, fxSteep," not in main:
        result.fail("%s: the foam no longer gets the contact slope" % WATER)

    foam = _function(fx, "float waterFxFoam(")
    if "float waterFxFoam(vec3 P, float depthV, float steep," not in fx:
        result.fail("%s: waterFxFoam() lost its steep parameter" % FX)
    if not re.search(r"We\s*=.*mix\(\s*1\.0\s*,\s*0\.2\s*,\s*clamp\(\s*steep", foam):
        result.fail("%s: the contact band no longer narrows on steep faces (piles, hulls and legs turn white)" % FX)

    # Refraction rejection: both the above-water and the under-water distorted lookups test the sampled
    # point's depth along the view and its side of the water (P.z and the flat level), else undistorted.
    ok = _function(water, "bool waterRefractOk(")
    if not ok or "dist - 0.05" not in ok or "flatZ" not in ok:
        result.fail("%s: waterRefractOk() no longer tests the sample's depth and its side of the water" % WATER)
    if len(re.findall(r"waterRefractOk\([^;]*,\s*true\)", main)) != 1:
        result.fail("%s: the under-water refraction lookup lost its rejection (displaced / doubled objects)" % WATER)
    if len(re.findall(r"waterRefractOk\([^;]*,\s*false\)", main)) != 1:
        result.fail("%s: the above-water refraction lookup lost its rejection (displaced / doubled objects)" % WATER)

    result.summary = "display-mip water detail, steep-contact waterline, refraction rejection in place"
