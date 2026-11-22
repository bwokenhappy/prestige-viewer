"""Regression guard for choppy audio from media on prims (web pages, video on in-world TVs).

The CEF plugin plays a page's sound itself, inside SLPlugin. It used to force Chromium onto the old
waveOut output (force_wave_audio), only because the volume catcher set the TV's volume with
waveOutSetVolume, which reaches waveOut streams alone. waveOut keeps a few short buffers queued, so a
brief stall in the plugin process emptied them and the sound broke up; a live stream also fell
further behind with every gap. Chromium now uses its normal (WASAPI) output, and the Windows volume
catcher sets the volume of the audio sessions of this plugin's process tree (ISimpleAudioVolume),
caught again on pump() so a stream that starts later gets the volume too. Without force_wave_audio
dullahan no longer disables AudioServiceOutOfProcess, so the sessions belong to Chromium's audio
service, a child of SLPlugin: matching the plugin's own pid alone found none (#1610; the
media-volume check guards the process-tree match).

  * media_plugin_cef.cpp leaves Chromium on its normal audio output (force_wave_audio = false);
  * windows_volume_catcher.cpp sets volume through the audio sessions of this process and its
    descendants (GetProcessId vs GetCurrentProcessId's tree, ISimpleAudioVolume::SetMasterVolume),
    never waveOutSetVolume, and pump() refreshes the sessions.
"""
import re

NAME = "media-audio"
STAGE = "source"
DESCRIPTION = "media audio: Chromium's normal output, volume through this process's audio sessions"
GUARDS = ("Choppy sound from media on prims (and a live stream drifting behind) because Chromium was forced "
          "onto waveOut for the volume control; a TV volume slider that stops working.")

PLUGIN = "indra/media_plugins/cef/media_plugin_cef.cpp"
CATCHER = "indra/media_plugins/cef/windows_volume_catcher.cpp"


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


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


def _code(text):
    # comments out, so a mention in a comment never satisfies or breaks a rule
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def run(ctx, result):
    problems = []
    plugin = _code(_read(ctx, PLUGIN))
    if re.search(r"force_wave_audio\s*=\s*true", plugin):
        problems.append("%s: force_wave_audio = true puts Chromium on waveOut (choppy sound)" % PLUGIN)
    if not re.search(r"force_wave_audio\s*=\s*false", plugin):
        problems.append("%s: set force_wave_audio = false explicitly (dullahan's default is not ours to rely on)" % PLUGIN)

    catcher = _code(_read(ctx, CATCHER))
    if "waveOutSetVolume" in catcher:
        problems.append("%s: waveOutSetVolume reaches waveOut streams only" % CATCHER)
    for needle, why in (("IAudioSessionManager2", "enumerate the audio sessions"),
                        ("GetProcessId", "pick this process's sessions"),
                        ("GetCurrentProcessId", "compare against this process"),
                        ("SetMasterVolume", "set each session's volume")):
        if needle not in catcher:
            problems.append("%s: missing %s (%s)" % (CATCHER, needle, why))
    pump = _function_body(catcher, "void VolumeCatcher::pump(")
    if pump is None or not re.search(r"pimpl\s*->\s*\w+\s*\(", pump):
        problems.append("%s: VolumeCatcher::pump must refresh the sessions (a stream that starts later)" % CATCHER)

    for p in problems:
        result.fail(p)
    result.summary = "%d problem(s)" % len(problems) if problems else "normal audio output, per-process session volume"
