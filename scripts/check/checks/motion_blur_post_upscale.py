"""Pin the Vulkan motion blur (#118) after the temporal upscaler, at the display resolution.

Motion blur used to run on the render-size scene BEFORE the resolve: FSR's history held the blur (a
~10-frame settle after the camera stopped) and under DLSS it stood down entirely ("motion blur: off
while DLSS runs"). It now runs on the resolve's display-size output (mHdr) for every path -- DLSS SR / RR
/ DLAA, FSR 3.1 / 4 upscaling or Native AA, the scaled blit, none -- after the SSR copy and the no-resolve
sharpening, before the eyedropper, DoF, bloom and the tonemap (indra/newview/vkgfx/llcleanmotionblurvk.h).

Source rules:
  ORDER    recordWorld calls recordMotionBlur exactly once, after the resolve (rrTry / recordFSR /
           recordSceneUpscaleBlit), recordSSRCopy and recordSharpenNoTaa, before gpuTimerMark(cmd, 4), the
           eyedropper, DoF and bloom; with the display extent and the render extent; off in stereo.
  NO-DLSS  recordMotionBlur has no DLSS stand-down (no mDlssFrame / mRrRanLast test, no "off while DLSS
           runs" log) and blurs mHdr, never sceneHdr() (the pre-resolve render-size scene).
  INPUTS   the tile pass maps output pixels to render texels (clean_mblur_common.glsl inputTexel, the
           jitter-corrected position) and reads depth, motion and the water mask through it; the radius is
           scaled from the output height; the noise is a fixed pattern (no per-frame temporalResolve clock).
  TEXT     the RenderVKMotionBlur settings text implies no DLSS / TAA / FSR limitation.
"""
import re

NAME = "motion-blur-post-upscale"
STAGE = "source"
DESCRIPTION = ("motion blur runs after the temporal upscaler (DLSS / FSR / none) on the display-size scene, "
               "with no DLSS stand-down")
GUARDS = ("motion blur moving back in front of the upscaler (blur in FSR's history, a settle after stopping), "
          "a DLSS stand-down coming back, or the gather running at the render size after an upscale")

VK = "indra/newview/vkgfx/"
RENDERER = VK + "llcleanrenderervk.cpp"
BLUR = VK + "llcleanmotionblurvk.cpp"
SH = "indra/vkclean/shaders/"
SETTINGS = "indra/newview/app_settings/settings.xml"


def _read(ctx, rel, result):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        result.fail("%s: missing" % rel)
        return ""


def _body(code, signature):
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


def _strip_comments(code):
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


def run(ctx, result):
    renderer = _strip_comments(_read(ctx, RENDERER, result))
    world = _body(renderer, "void LLCleanRendererVK::recordWorld(")
    if not world:
        result.fail("%s: recordWorld not found" % RENDERER)
        return

    # ORDER
    calls = [m.start() for m in re.finditer(r"\brecordMotionBlur\s*\(", world)]
    if len(calls) != 1:
        result.fail("%s: recordWorld calls recordMotionBlur %d times (expected exactly once, after the resolve)"
                    % (RENDERER, len(calls)))
    else:
        at = calls[0]
        call = world[at:world.find(";", at)]
        if not re.search(r"recordMotionBlur\s*\(\s*cmd\s*,\s*width\s*,\s*height\s*,\s*rw\s*,\s*rh\b", call):
            result.fail("%s: recordMotionBlur is not given the display extent and the render extent "
                        "(cmd, width, height, rw, rh, ...)" % RENDERER)
        before = ("rrTry(", "recordFSR(cmd", "recordSceneUpscaleBlit(cmd", "recordSSRCopy(cmd", "recordSharpenNoTaa(cmd")
        for name in before:
            i = world.find(name)
            if i < 0 or i > at:
                result.fail("%s: recordMotionBlur must come after %s (the resolve / SSR copy / sharpen)"
                            % (RENDERER, name.rstrip("(")))
        after = ("gpuTimerMark(cmd, 4)", "recordScenePixel(cmd", "recordDoF(cmd", "recordBloom(cmd")
        for name in after:
            i = world.find(name)
            if i < 0 or i < at:
                result.fail("%s: recordMotionBlur must come before %s" % (RENDERER, name.rstrip("(")))
        guard = world[max(0, at - 120):at]
        if "stereoActive()" not in guard:
            result.fail("%s: recordMotionBlur is no longer gated off in stereo (#1075)" % RENDERER)

    # NO-DLSS
    blur_raw = _read(ctx, BLUR, result)
    if "off while DLSS runs" in blur_raw:
        result.fail("%s: the \"motion blur: off while DLSS runs\" stand-down is back" % BLUR)
    blur = _strip_comments(blur_raw)
    rec = _body(blur, "bool LLCleanRendererVK::recordMotionBlur(")
    if not rec:
        result.fail("%s: LLCleanRendererVK::recordMotionBlur not found" % BLUR)
    else:
        for bad in ("mDlssFrame", "mRrRanLast", "mDlssKind", "sceneHdr()"):
            if bad in rec:
                result.fail("%s: recordMotionBlur reads %s (no DLSS stand-down; blur the resolved mHdr)" % (BLUR, bad))
        if "mHdr.image" not in rec:
            result.fail("%s: recordMotionBlur does not blur mHdr (the resolve's display-size output)" % BLUR)
    if "temporalResolve" in blur or "mNoiseFrame" in blur:
        result.fail("%s: a per-frame noise clock (temporalResolve / mNoiseFrame) is back; nothing integrates it "
                    "after the resolve" % BLUR)
    tiles = _body(blur, "bool LLCleanMotionBlurVK::recordTiles(")
    if not re.search(r"max_px\s*=.*\bf\.oh\b", tiles):
        result.fail("%s: the max radius is not scaled from the output height (f.oh)" % BLUR)

    # INPUTS
    common = _read(ctx, SH + "clean_mblur_common.glsl", result)
    tile = _strip_comments(_read(ctx, SH + "clean_mblur_tile.comp", result))
    if "ivec2 inputTexel(" not in common or "mb.p5.xy" not in common:
        result.fail("clean_mblur_common.glsl: inputTexel (output uv -> the jitter-corrected render texel) is missing")
    for tex in ("uDepth", "uMotion", "uReact"):
        if not re.search(r"texelFetch\(\s*%s\s*,\s*iq\s*," % tex, tile):
            result.fail("clean_mblur_tile.comp: %s is not read at the input texel (iq = inputTexel(uv))" % tex)
    if "blurSize()" not in tile or "renderSize()" in tile:
        result.fail("clean_mblur_tile.comp: the velocity is not computed over the output (blurSize) grid")

    # TEXT
    settings = _read(ctx, SETTINGS, result)
    m = re.search(r"<key>RenderVKMotionBlur</key>\s*<map>\s*<key>Comment</key>\s*<string>(.*?)</string>", settings, re.S)
    if not m:
        result.fail("%s: RenderVKMotionBlur comment not found" % SETTINGS)
    elif re.search(r"before TAA|before the resolve|off while DLSS|not with DLSS|without DLSS", m.group(1)):
        result.fail("%s: the RenderVKMotionBlur text still implies a DLSS / pre-resolve limitation" % SETTINGS)
