"""Regression guard for the media volume controls (#1610): the master Media slider and mute, a media's
own volume and its falloff with distance must reach the sound of web media (CEF) on every platform.

Chromium plays a page's sound from its audio service, a utility process the CEF browser process
(SLPlugin) launches -- not from SLPlugin itself. On Windows dullahan keeps the audio service in process
only with force_wave_audio, which #1538 turned off, so the volume catcher's sessions-of-this-pid match
found nothing and no media volume control did anything. The catchers now match the plugin's process
tree (the plugin and its descendants), per plugin, so each media keeps its own volume.

Tests (scripts/check/test_media_volume.cpp, built on the host):
  * volume_catcher_util.h isInProcessTree: the plugin, its audio-service child, a zygote grandchild
    match; another media's plugin tree, the viewer, unrelated apps, pid 0, cycles and over-deep chains
    do not; the rate-limited log the plugin sends to the viewer (per-key interval, held-back count,
    bounded queue and key table);
  * llmediavolume.h llmedia_effective_volume: master x media volume, mutes, MediaRollOffMin / Max,
    attenuation falling with distance and never above 1, "only audible" media.
Wiring the tests cannot see:
  * windows_volume_catcher.cpp matches sessions through isInProcessTree (not the plugin's pid alone);
  * the Linux backends match through isOurStreamPid (the process tree); PipeWire sets soft volumes,
    never the stream's master SPA_PROP_volume (WirePlumber stores and restores that per stream key:
    one muted TV would mute every later CEF stream), and re-applies them shortly after a stream is
    attached (WirePlumber's channel-volume restore at creation turns soft volumes off);
  * the Linux backend choice falls back to PulseAudio when PipeWire does not connect;
  * LLViewerMediaImpl::updateVolume uses llmedia_effective_volume;
  * the plugin sends its volume_catcher_log lines and the viewer logs them (tag MediaVolume): the
    plugin's own LL_INFOS reach no log file.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "media-volume"
STAGE = "source"
DESCRIPTION = "media volume: per-plugin process-tree stream matching, volume combine, catcher log (tests)"
GUARDS = ("Media volume slider, mute, a TV's own volume or its distance falloff doing nothing (the catcher "
          "matching no stream, #1610), one media's volume applied to another's, or volume problems that leave "
          "no trace in Slipstream.log.")

UTIL = "indra/media_plugins/cef/volume_catcher_util.h"
VOLUME = "indra/newview/llmediavolume.h"
TEST = "scripts/check/test_media_volume.cpp"
WIN = "indra/media_plugins/cef/windows_volume_catcher.cpp"
LINUX = "indra/media_plugins/cef/linux/volume_catcher_linux.cpp"
PIPEWIRE = "indra/media_plugins/cef/linux/volume_catcher_pipewire.cpp"
PULSE = "indra/media_plugins/cef/linux/volume_catcher_pulseaudio.cpp"
PLUGIN = "indra/media_plugins/cef/media_plugin_cef.cpp"
CLASSMEDIA = "indra/llplugin/llpluginclassmedia.cpp"
VIEWERMEDIA = "indra/newview/llviewermedia.cpp"


def _code(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function_body(code, signature):
    at = code.find(signature)
    if at < 0:
        return None
    open_at = code.find("{", at)
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at:i + 1]
    return None


def _wiring(ctx):
    problems = []
    win = _code(ctx, WIN)
    if "isInProcessTree(" not in win:
        problems.append("%s: sessions are not matched through isInProcessTree (Chromium's audio service is a "
                        "child process)" % WIN)
    if re.search(r"GetProcessId\(&pid\)\)\s*&&\s*pid\s*==\s*self", win):
        problems.append("%s: a session is matched by the plugin's own pid only" % WIN)

    linux = _code(ctx, LINUX)
    if "isInProcessTree(" not in linux:
        problems.append("%s: isOurStreamPid does not walk the process tree" % LINUX)
    ensure = _function_body(linux, "void VolumeCatcher::ensureImpl(")
    if ensure is None or "mCore" not in ensure:
        problems.append("%s: ensureImpl keeps PipeWire without a connection (no PulseAudio fallback)" % LINUX)
    for rel in (PIPEWIRE, PULSE):
        code = _code(ctx, rel)
        if "isOurStreamPid(" not in code:
            problems.append("%s: streams are not matched through isOurStreamPid" % rel)
    pw = _code(ctx, PIPEWIRE)
    if re.search(r"\bSPA_PROP_volume\b", pw) or "SPA_PROP_softVolumes" not in pw:
        problems.append("%s: set soft volumes, never SPA_PROP_volume (WirePlumber persists a stream's volume per "
                        "stream key: it would leak to every later CEF stream)" % PIPEWIRE)
    pump = _function_body(pw, "void VolumeCatcherPipeWire::pump(")
    if pump is None or "updateVolume()" not in pump:
        problems.append("%s: pump does not re-apply a newly attached stream's volume (WirePlumber's restore at "
                        "creation can land after ours)" % PIPEWIRE)

    vm = _code(ctx, VIEWERMEDIA)
    upd = _function_body(vm, "void LLViewerMediaImpl::updateVolume(")
    if upd is None or "llmedia_effective_volume(" not in upd:
        problems.append("%s: updateVolume does not use llmedia_effective_volume (tested combine)" % VIEWERMEDIA)

    plugin = _code(ctx, PLUGIN)
    if '"volume_catcher_log"' not in plugin or "logQueue().drain()" not in plugin:
        problems.append("%s: the volume catcher's log lines are not sent to the viewer" % PLUGIN)
    cm = _code(ctx, CLASSMEDIA)
    if not re.search(r'message_name == "volume_catcher_log"[^}]*LL_INFOS\("MediaVolume"\)', cm, re.S):
        problems.append("%s: the viewer does not log the plugin's volume_catcher_log lines" % CLASSMEDIA)
    return problems


def run(ctx, result):
    missing = [p for p in (UTIL, VOLUME, TEST, WIN, LINUX, PIPEWIRE, PULSE, PLUGIN, CLASSMEDIA, VIEWERMEDIA)
               if not os.path.exists(ctx.path(p))]
    if missing:
        result.fail("missing: " + ", ".join(missing))
        result.summary = "files missing"
        return
    problems = _wiring(ctx)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        for p in problems:
            result.fail(p)
        result.warn("no C++ compiler: media volume tests not run (wiring checked)")
        result.summary = "wiring only"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_media_volume")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I" + ctx.path("indra/media_plugins/cef"), "-I" + ctx.path("indra/newview"),
                            ctx.path(TEST), "-o", exe], capture_output=True, text=True)
        if c.returncode:
            problems.append("compile failed:\n" + c.stdout + c.stderr)
        else:
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            if r.returncode:
                problems.append(r.stdout + r.stderr)
    for p in problems:
        result.fail(p)
    result.summary = ("%d problem(s)" % len(problems) if problems
                      else "process-tree matching, volume combine and catcher log tests pass; wiring present")
