"""Source guard: the 3D mouse is polled every frame, flycam included (#1486, #1562).

LL's Windows viewer skipped LLViewerJoystick::updateStatus() in scanJoystick() while flycam was
on, because the Win32 window's WM_TIMER (LLViewerWindow::handleTimerEvent) polled the device
instead. That window backend is gone (SDL only); the guard stayed, so on Windows the axes froze
the moment flycam turned on: the device monitor stopped moving and flycam ignored the device.
  POLL-ALWAYS   scanJoystick() calls updateStatus() with no getOverrideCamera() / mOverrideCamera
                condition in front of it.
"""
import re

NAME = "joystick-flycam-poll"
STAGE = "source"
DESCRIPTION = "the 3D mouse is polled every frame, flycam included"
GUARDS = ("#1486 / #1562: on Windows the SpaceMouse axes froze as soon as flycam was on (polling was "
          "left to a Win32 timer that no longer exists).")

SRC = "indra/newview/llviewerjoystick.cpp"


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
    text = _strip_comments(open(ctx.path(SRC), encoding="utf-8").read())
    body = _function(text, "void LLViewerJoystick::scanJoystick(")
    if body is None:
        result.fail("POLL-ALWAYS: LLViewerJoystick::scanJoystick() not found in %s" % SRC)
        return
    at = body.find("updateStatus();")
    if at < 0:
        result.fail("POLL-ALWAYS: scanJoystick() no longer calls updateStatus()")
        return
    # the statement right before the call must not be a condition on the flycam state
    # (skipping preprocessor lines, so an `if (...)` above an `#endif` still counts)
    lines = [l.strip() for l in body[:at].split("\n") if l.strip() and not l.strip().startswith("#")]
    last = lines[-1] if lines else ""
    if re.search(r"\bif\s*\(", last) or re.search(r"getOverrideCamera|mOverrideCamera", last):
        result.fail("POLL-ALWAYS: updateStatus() in scanJoystick() is conditional again: %s" % last)
        return
    result.summary = "scanJoystick() polls the device every frame, flycam or not"
