"""Run the production exposure scalar solve and host policy on the CPU.

The shader tail is included as C++ with storage-image I/O replaced and
min/max renamed to avoid Windows SDK macro names in the C++ harness.
This exercises the shipped arithmetic, not a second implementation. Shader
compilation below checks the actual GLSL; neither step creates a GPU device.
"""
import os
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

NAME = "interior-exposure"
STAGE = "source"
DESCRIPTION = "bounded daytime low-key exposure, physical-night/EEP bypass, live adaptation history"
GUARDS = "Auto exposure washing out dark interiors; accidental daylight/night, EEP, bloom or toggle regressions."


def run(ctx, result):
    settings = ET.parse(ctx.path("indra/newview/app_settings/settings.xml")).getroot().find("map")
    entries = list(settings)
    option = entries[entries.index(next(e for e in entries if e.tag == "key" and e.text == "RenderVKInteriorExposure")) + 1]
    fields = list(option)
    values = {fields[i].text: fields[i+1].text for i in range(0, len(fields), 2)}
    if values.get("Persist") != "0" or values.get("Value") != "0" or values.get("Type") != "Boolean":
        result.fail("interior exposure must remain default off and nonpersistent")
    host = open(ctx.path("indra/newview/vkgfx/llcleanrenderervk.cpp"), encoding="utf-8").read()
    for wiring in ('"RenderVKInteriorExposure", false', '"RenderUseExposureSkySettings", false',
                   'ep[7] = slInteriorExposureWeight(s_interior, s_sky_range, p15[3] > 0.5f,',
                   'nl.on && nl.night > 0.f, mAtmos[10 * 4 + 2])'):
        if wiring not in host:
            result.fail("missing live policy wiring: " + wiring)
    shader = ctx.path("indra/vkclean/shaders/clean_exposure.comp")
    source = open(shader, encoding="utf-8").read()
    tail = source[source.index("    float L = s_lum[0] / 256.0;"):source.rindex("}")]
    tail = tail.replace("imageLoad(uExposure, ivec2(0)).r", "previous")
    tail = tail.replace("imageStore(uExposure, ivec2(0), vec4(s));", "return s;")
    tail = re.sub(r"\b(min|max)\(", lambda m: {"min": "glslMin(", "max": "glslMax("}[m[1]], tail)
    compiler = os.environ.get("CXX") or shutil.which("c++")
    if not compiler:
        result.fail("C++ compiler required for production exposure fixtures")
        return
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "exposure_tail.inc"), "w", encoding="utf-8") as f:
            f.write(tail)
        exe = os.path.join(tmp, "interior_exposure")
        commands = [[compiler, "-std=c++17", "-O1", "-Wall", "-Werror",
                     "-I" + tmp, "-I" + ctx.path("indra/newview"),
                     ctx.path("scripts/check/test_interior_exposure.cpp"), "-o", exe], [exe]]
        glslc = shutil.which("glslc")
        if glslc:
            commands.append([glslc, "--target-env=vulkan1.3", "-O", shader, "-o", os.path.join(tmp, "exposure.spv")])
        else:
            result.warn("glslc unavailable: GLSL compile skipped")
        for command in commands:
            run = subprocess.run(command, capture_output=True, text=True)
            if run.returncode:
                result.fail("exposure fixture/compile failed:\n" + run.stdout + run.stderr)
                return
    result.summary = "production scalar solve + host gate PASS; default off/Persist=0; GLSL compile " + ("PASS" if glslc else "skipped")
