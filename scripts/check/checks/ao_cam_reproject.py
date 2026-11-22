"""Regression guard for camera-only AO temporal reprojection (#843; RenderVKAOCameraReproject;
indra/newview/vkgfx/llaocamreproject.h).

Compiles scripts/check/test_ao_cam_reproject.cpp with the host C++ compiler and runs it: with the
setting off the AO accumulator keeps the motion path exactly as before; with it on, the motion path
is dropped ONLY when the accumulator is its sole reader -- the upscaler (and frame generation), motion
blur, the RT shadow / radiance denoisers, the RT passes that track moving casters or reuse along the
motion vectors, the motion debug view, an FX motion input or an armed live snapshot each keep it.

Source guard: ensureTemporal fills every reader into llaocam::Inputs and decides the path through
llaocam::motionPath / cameraOnly; the previous-camera readers (the latch, the frame info's history,
the AO's validity, the camera-cut flags, VRS) ask mCamHistOn, so camera-only frames keep the reset
verdict; the setting is a live, unsaved A/B (Persist 0, default off).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "ao-cam-reproject"
STAGE = "source"
DESCRIPTION = ("camera-only AO reprojection drops the motion path only when the AO accumulator is its "
               "sole reader; every other motion reader keeps it")
GUARDS = ("the motion vectors / previous rows silently vanishing under FSR / DLSS, motion blur, the RT "
          "denoisers, GI or an FX motion input because the AO stopped forcing them; camera-only AO "
          "losing its camera-cut / teleport reset")

REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"

READERS = ("mi.upscaler", "mi.debugMotion", "mi.motionBlur", "mi.rtMaskTemporal", "mi.localDenoiser",
           "mi.rtMotion", "mi.fxMotion", "mi.liveSnapshot", "mi.aoTemporal", "mi.stereo", "mi.setting")


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


def _source_guard(ctx, result):
    ren = _strip_comments(open(ctx.path(REN), encoding="utf-8").read())
    fn = _function(ren, "void LLCleanRendererVK::ensureTemporal(uint32_t w, uint32_t h)")
    if fn is None:
        result.fail("LLCleanRendererVK::ensureTemporal not found; update this check")
    else:
        for field in READERS:
            if not re.search(re.escape(field) + r"\s*=", fn):
                result.fail("ensureTemporal no longer fills %s (llaocam::Inputs)" % field)
        for call in ("llaocam::motionPath(mi)", "llaocam::cameraOnly(mi)"):
            if call not in fn:
                result.fail("ensureTemporal no longer decides through %s" % call)
        if not re.search(r"if\s*\(\s*!wantMotion\b", fn):
            result.fail("ensureTemporal's off branch is no longer keyed on llaocam::motionPath (wantMotion)")
        if "mi.rtMotion" in fn and "rtMotionConsumerActive()" not in fn:
            result.fail("mi.rtMotion must come from rtMotionConsumerActive()")
        if "mCamHistOn = aoCamOnly" not in fn:
            result.fail("ensureTemporal's off branch no longer keeps the camera tracked for camera-only frames")
    rc = _function(ren, "bool LLCleanRendererVK::rtConsumerActive() const")
    if rc is None or "rtMotionConsumerActive()" not in rc:
        result.fail("rtConsumerActive no longer includes rtMotionConsumerActive (the two drift)")
    rm = _function(ren, "bool LLCleanRendererVK::rtMotionConsumerActive() const")
    if rm is None:
        result.fail("rtMotionConsumerActive not found")
    else:
        for need in ("mGiModeSetting", "sunRtWanted()", "rtReflWanted()", "rtaoWanted()", "mProjMode == 2u"):
            if need not in rm:
                result.fail("rtMotionConsumerActive no longer covers %s" % need)
    # The previous camera's readers ask mCamHistOn (camera-only frames keep the reset verdict).
    for pat, why in ((r"const bool hist = mCamHistOn && mTaaPrevValid && !mTemporalReset", "the frame info's history"),
                     (r"if \(mCamHistOn\)\s*\{\s*std::memcpy\(mTaaPrevVP", "the previous-camera latch"),
                     (r"mAoTemporalNow = !mOffscreen && mCamHistOn", "aoTemporalBegin"),
                     (r"!mTaaResolveReset && mCamHistOn;", "the AO history validity"),
                     (r"p\.flags\[0\] = \(mPrevValid && !\(mCamHistOn && mTaaResolveReset\)\)", "VRS history")):
        if not re.search(pat, ren):
            result.fail("%s no longer asks mCamHistOn" % why)
    if len(re.findall(r"cameraCut = [^;]*mCamHistOn", ren)) < 5:
        result.fail("a camera-cut flag (fog / god rays / clouds / weather) no longer asks mCamHistOn")

    settings = open(ctx.path(SETTINGS), encoding="utf-8").read()
    m = re.search(r"<key>RenderVKAOCameraReproject</key>\s*<map>(.*?)</map>", settings, re.S)
    if not m:
        result.fail("RenderVKAOCameraReproject missing from settings.xml")
    else:
        body = m.group(1)
        if not re.search(r"<key>Persist</key>\s*<integer>0</integer>", body):
            result.fail("RenderVKAOCameraReproject is a live A/B: Persist 0")
        if not re.search(r"<key>Type</key>\s*<string>Boolean</string>", body):
            result.fail("RenderVKAOCameraReproject must be Boolean")


def run(ctx, result):
    _source_guard(ctx, result)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: camera-only AO fixtures not run")
        result.summary = "source guard only (no compiler)"
        return
    src = ctx.path("scripts/check/test_ao_cam_reproject.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_ao_cam_reproject")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "AO-only drops the motion path; each of 8 other readers keeps it; camera readers ask mCamHistOn"
