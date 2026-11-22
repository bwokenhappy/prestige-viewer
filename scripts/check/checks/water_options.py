"""Source guard: the water options (#1636) stay wired end to end.

Each user-facing water switch must exist as a persisted setting, be bound in the "Water effects"
floater, be reachable from Preferences > Graphics, and be read by the code that skips the work:
  RenderVKWaterWaveIntensity  scales the sea state (hs, detail, surf) and the foam band's roughness
  RenderVKWaterRipples        gates the ripple field's allocation (no field = no step, no sampling)
  RenderVKWaterWhitecaps      gates the surf / whitecap flag of the foam
  RenderVKWaterSplashes       gates the splash droplets
"""
import re

NAME = "water-options"
STAGE = "source"
DESCRIPTION = "the water effect switches and the wave intensity are persisted, bound in the Water effects floater and read by the renderer"
GUARDS = "#1636 water options: a switch that is not read does nothing; a setting not bound has no UI; an unpersisted one forgets the user's choice."

SETTINGS = "indra/newview/app_settings/settings.xml"
FLOATER = "indra/newview/skins/default/xui/en/floater_water_options.xml"
PANEL = "indra/newview/skins/default/xui/en/panel_preferences_graphics1.xml"
REG = "indra/newview/llviewerfloaterreg.cpp"

# setting -> (type, file that must read it, text that must appear there)
NEW = {
    "RenderVKWaterWaveIntensity": ("F32", "indra/newview/slseastate.cpp", "s_wave"),
    "RenderVKWaterRipples": ("Boolean", "indra/newview/vkgfx/llcleanwaterfxvk.cpp", "s_ripples"),
    "RenderVKWaterWhitecaps": ("Boolean", "indra/newview/vkgfx/llcleanwaterfxvk.cpp", "s_caps"),
    "RenderVKWaterSplashes": ("Boolean", "indra/newview/slwaterinteraction.cpp", "s_splashes"),
}
# settings that already existed and the floater also binds
BOUND = ["RenderVKWaterWaves", "RenderVKWaterFoam", "RenderVKWaterInteraction",
         "RenderVKWeatherRipples", "RenderVKWeatherSplashes"]


def _setting_block(xml, name):
    m = re.search(r"<key>%s</key>\s*<map>(.*?)</map>" % re.escape(name), xml, re.S)
    return m.group(1) if m else None


def run(ctx, result):
    bad = []
    settings = open(ctx.path(SETTINGS), encoding="utf-8").read()
    floater = open(ctx.path(FLOATER), encoding="utf-8").read()
    panel = open(ctx.path(PANEL), encoding="utf-8").read()
    reg = open(ctx.path(REG), encoding="utf-8").read()

    for name, (typ, src, token) in NEW.items():
        block = _setting_block(settings, name)
        if block is None:
            bad.append("%s is missing from settings.xml" % name)
            continue
        if not re.search(r"<key>Persist</key>\s*<integer>1</integer>", block):
            bad.append("%s is not persisted" % name)
        if "<string>%s</string>" % typ not in block:
            bad.append("%s is not of type %s" % (name, typ))
        code = open(ctx.path(src), encoding="utf-8").read()
        if '"%s"' % name not in code or token not in code:
            bad.append("%s is no longer read in %s" % (name, src))
        if 'control_name="%s"' % name not in floater:
            bad.append("%s is not bound in floater_water_options.xml" % name)
    for name in BOUND:
        if _setting_block(settings, name) is None:
            bad.append("%s no longer exists in settings.xml" % name)
        if 'control_name="%s"' % name not in floater:
            bad.append("%s is not bound in floater_water_options.xml" % name)

    # the intensity reaches the foam: the sea state carries it, the frame info applies it
    sea = open(ctx.path("indra/newview/slseastate.cpp"), encoding="utf-8").read()
    for needle in ("mState.waveScale = wi", "* wi;", "s.waveScale"):
        if needle not in sea:
            bad.append("slseastate.cpp no longer applies the wave intensity (%s)" % needle)

    # the ripples switch must free the field, not just hide it
    fx = open(ctx.path("indra/newview/vkgfx/llcleanwaterfxvk.cpp"), encoding="utf-8").read()
    if not re.search(r"!s_ripples\s*\?\s*0u", fx):
        bad.append("RenderVKWaterRipples no longer sizes the ripple field to 0 when off")
    if not re.search(r"TIER_MEDIUM\s*&&\s*s_caps", fx):
        bad.append("RenderVKWaterWhitecaps no longer gates the surf / whitecap flag")

    if 'name="water_options"' not in floater or 'LLFloaterReg::add("water_options"' not in reg:
        bad.append("the water_options floater is not registered")
    if 'parameter="water_options"' not in panel:
        bad.append("Preferences > Graphics has no button that opens the Water effects floater")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "%d new water settings wired; the floater binds %d more" % (len(NEW), len(BOUND))
