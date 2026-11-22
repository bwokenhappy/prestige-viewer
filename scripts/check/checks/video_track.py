"""Regression guard for Video Studio (#1668): the track math, the ride, the recorder's clock, and three invariants.

Compiles scripts/check/test_video_track.cpp with the host C++ compiler and runs it: straight and curved rails
(both sides, tilted), the bend handle's radius, easing (continuous, monotonic, the right cruise speed), repeat
modes, the cart's aim (along / target / blend, no roll), the gizmos' drag math, the recorder's clock (preroll,
fixed step, dropped frames, the end of a ride).

Source rules:
  LOCAL    the track is client-local: the studio's files never send to the region or touch the selection
           (no message system, no LLSelectMgr calls, no object updates).
  BOUNDED  recording memory is bounded: the recorder's limits stay within their caps, the encoder runs in record
           mode with that budget, the encoder's record mode stops (never grows or evicts silently), the PNG
           writer's queue is checked before every push, saved tracks are capped in count and size.
  REPLAY   SLInstantReplay stays default off (settings.xml and the code's cached default).
  HIDDEN   the track and gizmos are never drawn while recording (they would be in the video).
  ROUTE    the studio runs the tested math (slvtrack) and the recorder runs the tested clock.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "video-track"
STAGE = "source"
DESCRIPTION = "Video Studio: track / ride / aim / recorder clock tests; track never sent; recording bounded; replay off"
GUARDS = ("A camera track sent to the region, a recording that grows without bound, instant replay on by default, "
          "the track drawn into a video, or untested ride math.")

HEADER = "indra/newview/slvideotrack.h"
TEST = "scripts/check/test_video_track.cpp"
STUDIO = "indra/newview/slvideostudio.cpp"
VIEW = "indra/newview/slvideostudioview.cpp"
RECORDER_H = "indra/newview/slvideorecorder.h"
RECORDER = "indra/newview/slvideorecorder.cpp"
ENCODER = "indra/newview/vkgfx/llcleanreplayvk.cpp"
REPLAY = "indra/newview/llinstantreplay.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"

# Anything that reaches the region or the object selection.
SENDS = [r"gMessageSystem", r"sendReliableMessage", r"\bnewMessage(?:Fast)?\s*\(", r"sendMultipleUpdate",
         r"LLSelectMgr::getInstance\(\)\s*->", r"LLSelectMgr::instance\(\)", r"\bsendUpdate\s*\(", r"\bselectObject",
         r"LLCoreHttpUtil", r"\bgAgent\.send", r"\bsendObjectUpdate"]


def _code(ctx, rel):
    """The file without comments ("" when it is missing)."""
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return re.sub(r"//[^\n]*|/\*.*?\*/", "", f.read(), flags=re.S)
    except OSError:
        return ""


def _const(text, name):
    m = re.search(r"\b%s\s*=\s*([0-9]+)(?:ull|u|ULL|U)?(?:\s*<<\s*([0-9]+))?" % name, text)
    if not m:
        return None
    v = int(m.group(1))
    return v << int(m.group(2)) if m.group(2) else v


def source_rules(ctx):
    bad = []
    for rel in (HEADER, STUDIO, VIEW, RECORDER_H, RECORDER):
        c = _code(ctx, rel)
        if not c:
            bad.append("LOCAL: %s is missing" % rel)
            continue
        for pat in SENDS:
            if re.search(pat, c):
                bad.append("LOCAL: %s uses %s (the track and its recording never reach the region or the selection)"
                           % (rel, pat))
    rh = _code(ctx, RECORDER_H)
    secs, budget, queue = _const(rh, "kMaxSeconds"), _const(rh, "kBudgetBytes"), _const(rh, "kPngQueue")
    if secs is None or secs > 300:
        bad.append("BOUNDED: kMaxSeconds must be at most 300 (the encoder's longest; got %r)" % secs)
    if budget is None or budget > (2 << 30):
        bad.append("BOUNDED: kBudgetBytes must be at most 2 GiB (got %r)" % budget)
    if queue is None or queue > 8:
        bad.append("BOUNDED: kPngQueue must be at most 8 frames (got %r)" % queue)
    rc = _code(ctx, RECORDER)
    for need, why in (("c.record = true", "the encoder runs in record mode"),
                      ("c.budgetBytes = SLVideoRecorder::kBudgetBytes", "with the recorder's budget"),
                      ("c.seconds = SLVideoRecorder::kMaxSeconds", "and its longest recording"),
                      ("hasRoom()", "the PNG writer's queue is checked before a push"),
                      ("->full()", "a full recording stops")):
        if need not in rc:
            bad.append("BOUNDED: %s must contain %r (%s)" % (RECORDER, need, why))
    ec = _code(ctx, ENCODER)
    if not re.search(r"if \(m\.record\)\s*\{[^}]*recordFull = true;[^}]*return;", ec, flags=re.S):
        bad.append("BOUNDED: %s poll() must stop a recording at its budget / length (recordFull) and return before "
                   "the ring's eviction" % ENCODER)
    if "if (m.record && m.recordFull) return false;" not in ec:
        bad.append("BOUNDED: %s recordCapture() must take nothing once a recording is full" % ENCODER)
    sc = _code(ctx, STUDIO)
    for name in ("kMaxTracks", "kMaxTrackFile"):
        if not re.search(r"\b%s\s*=" % name, sc):
            bad.append("BOUNDED: %s must cap saved tracks with %s" % (STUDIO, name))
    try:
        with open(ctx.path(SETTINGS), encoding="utf-8") as f:
            settings = f.read()
    except OSError:
        settings = ""
    m = re.search(r"<key>SLInstantReplay</key>\s*<map>(.*?)</map>", settings, flags=re.S)
    if not m or not re.search(r"<key>Value</key>\s*<integer>0</integer>", m.group(1)):
        bad.append("REPLAY: SLInstantReplay must stay default off in settings.xml")
    if 'LLCachedControl<bool> s_on(gSavedSettings, "SLInstantReplay", false)' not in _code(ctx, REPLAY):
        bad.append("REPLAY: %s must read SLInstantReplay with a false default" % REPLAY)
    m = re.search(r"void SLVideoStudio::renderWorld\(\)\s*\{(.*?)\n\}", sc, flags=re.S)
    if not m or "if (SLVideoRecorder::busy()) return;" not in m.group(1):
        bad.append("HIDDEN: SLVideoStudio::renderWorld must draw nothing while the recorder is busy")
    for call in ("sv::position(", "sv::at(", "sv::aimDir(", "sv::cameraAxes(", "sv::radiusForBulge(", "SLVideoRecorder::rideDone("):
        if call not in sc:
            bad.append("ROUTE: %s must call %s" % (STUDIO, call))
    for call in ("slvtrack::RecClock", "clock.encoderClock(", "clock.captured(", "clock.submitted("):
        if call not in rc:
            bad.append("ROUTE: %s must use %s" % (RECORDER, call))
    return bad


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: video track tests not run")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "test_video_track")
            c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview"),
                                ctx.path(TEST), "-o", exe], capture_output=True, text=True)
            if c.returncode:
                result.fail("compile failed:\n" + c.stdout + c.stderr)
                return
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode:
                result.fail(r.stdout + r.stderr)
                return
    bad = source_rules(ctx)
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "tests pass; the track stays local, recording is bounded, replay is off by default"
