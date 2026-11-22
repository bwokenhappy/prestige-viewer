"""Fail when a god rays control stops going through the shared availability predicate.

The renderer skips the god rays pass while volumetric fog is on or the physical sky's camera medium
is ready, so the setting does nothing under the default settings. Preferences > Graphics > Advanced,
Photo Studio > Scene and the Camera mode rail all grey their god rays controls (with a tooltip that
says why) through SLGodRays::availableFromSettings() in indra/newview/slgodraysavail.h, so the three
cannot drift from each other or from the renderer.
"""
import re

NAME = "godrays-grey"
STAGE = "source"
DESCRIPTION = ("the three god rays UI places (Preferences, Photo Studio, Camera mode rail) grey out through "
               "the one shared predicate")
GUARDS = ("a god rays control that looks live while volumetric fog or the physical sky has replaced the "
          "pass, or one of the three places drifting from the others")

NEWVIEW = "indra/newview/"
HEADER = NEWVIEW + "slgodraysavail.h"
RENDERER = NEWVIEW + "vkgfx/llcleanrenderervk.cpp"
PREFS_CPP = NEWVIEW + "llfloaterpreferencesgraphicsadvanced.cpp"
PREFS_XML = NEWVIEW + "skins/default/xui/en/floater_preferences_graphics_advanced.xml"
STUDIO_CPP = NEWVIEW + "slpanelphotostudiosection.cpp"
STUDIO_XML = NEWVIEW + "skins/default/xui/en/panel_photo_studio_scene.xml"
RAIL_CPP = NEWVIEW + "slcamerarail.cpp"

PREFS_NAMES = ("GodRaysText", "GodRays", "GodRaysIntensity")
STUDIO_NAMES = ("godrays_label", "godrays_combo", "godrays_rst", "godrays_intensity", "godrays_intensity_rst")
PREDICATE = "SLGodRays::availableFromSettings"


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
    header = _read(ctx, HEADER, result)
    for setting in ("RenderVKVolumetricFog", "RenderVKPhysicalSky"):
        if setting not in _function(header, "availableFromSettings"):
            result.fail("%s: availableFromSettings() no longer reads %s" % (HEADER, setting))

    # The predicate mirrors this gate; if the renderer's gate changes, revisit the predicate.
    renderer = _read(ctx, RENDERER, result)
    if not re.search(r"!mVolFogNow\s*&&\s*!mCameraMediumReady", renderer):
        result.fail("%s: the god rays gate (!mVolFogNow && !mCameraMediumReady) changed; update "
                    "SLGodRays::availableFromSettings() and this check to match" % RENDERER)

    prefs = _read(ctx, PREFS_CPP, result)
    prefs_xml = _read(ctx, PREFS_XML, result)
    if PREDICATE not in prefs:
        result.fail("%s: does not use %s" % (PREFS_CPP, PREDICATE))
    for name in PREFS_NAMES:
        if '"%s"' % name not in prefs:
            result.fail("%s: does not grey %s" % (PREFS_CPP, name))
        if 'name="%s"' % name not in prefs_xml:
            result.fail("%s: control %s is gone" % (PREFS_XML, name))
    for setting in ("RenderVKVolumetricFog", "RenderVKPhysicalSky"):
        if '"%s"' % setting not in prefs:
            result.fail("%s: does not refresh when %s changes" % (PREFS_CPP, setting))

    studio = _read(ctx, STUDIO_CPP, result)
    studio_xml = _read(ctx, STUDIO_XML, result)
    if PREDICATE not in _function(studio, "SLPanelPhotoStudioSection::refreshGodRays"):
        result.fail("%s: refreshGodRays() does not use %s" % (STUDIO_CPP, PREDICATE))
    for name in STUDIO_NAMES:
        if '"%s"' % name not in studio:
            result.fail("%s: does not grey %s" % (STUDIO_CPP, name))
        if 'name="%s"' % name not in studio_xml:
            result.fail("%s: control %s is gone" % (STUDIO_XML, name))

    rail = _read(ctx, RAIL_CPP, result)
    row = re.search(r'choice\("God rays".*?scene\.rows\.push_back', rail, re.S)
    if not row or PREDICATE not in row.group(0):
        result.fail("%s: the \"God rays\" row is not enabled through %s" % (RAIL_CPP, PREDICATE))

    result.summary = "3 places use %s" % PREDICATE
