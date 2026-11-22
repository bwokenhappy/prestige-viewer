"""Source guard: the GPU cursor pick's per-frame request is as long as any click's ray
(2026-11-08 micro-stutters: vk_performPick / pick_answerNow ~10 ms on most async clicks).

An answer covers a pick only when its request ray is at least as long as the pick's
(llcleancursorpick.cpp covers). The per-frame request was a fixed 1024 m, but the camera
tool's focus pick (Alt-click) reaches the draw distance with DisableCameraConstraints
(LLPickInfo::getPickDepth, #560: up to ~16 km at a 4 km draw distance). Every such click --
held for its pixel, its answer in -- still failed covers() on length and answered now, a
main-thread GPU wait behind the frames in flight, twice (intersectAll and getSurfaceInfo).
  SHARED-DEPTH   LLPickInfo::getPickDepth takes the camera tool's depth from
                 LLPickInfo::maxPickDepth (one rule for the click and the request).
  REQUEST-SPANS  cursorRay (the per-frame request's ray) sets q.len from maxPickDepth.
"""
import re

NAME = "pick-ray-length"
STAGE = "source"
DESCRIPTION = "the per-frame GPU pick request spans the longest click ray (no answer-now on Alt-click focus)"
GUARDS = ("~10 ms main-thread GPU waits on every Alt-click camera focus with DisableCameraConstraints "
          "(2026-11-08 pick_answerNow hitches).")

PICK = "indra/newview/llcleancursorpick.cpp"
WIN = "indra/newview/llviewerwindow.cpp"


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
    pick = _strip_comments(open(ctx.path(PICK), encoding="utf-8").read())
    win = _strip_comments(open(ctx.path(WIN), encoding="utf-8").read())
    bad = []

    gpd = _function(win, "F32 LLPickInfo::getPickDepth(")
    if not gpd or "maxPickDepth(" not in gpd:
        bad.append("SHARED-DEPTH: LLPickInfo::getPickDepth does not take its depth from LLPickInfo::maxPickDepth")
    if not _function(win, "F32 LLPickInfo::maxPickDepth("):
        bad.append("SHARED-DEPTH: LLPickInfo::maxPickDepth is gone")

    cr = _function(pick, "bool cursorRay(")
    if not cr:
        bad.append("cursorRay not found in llcleancursorpick.cpp; update this check")
    else:
        # the desktop ray's length (the VR branch returns before it)
        lens = re.findall(r"q\.len\s*=\s*([^;]+);", cr)
        if not lens or not any("maxPickDepth(" in l for l in lens):
            bad.append("REQUEST-SPANS: cursorRay's ray length is not taken from LLPickInfo::maxPickDepth "
                       "(a fixed length leaves longer clicks uncovered: answered now)")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "per-frame request spans LLPickInfo::maxPickDepth"
