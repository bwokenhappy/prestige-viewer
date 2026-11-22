"""Source guard: the mini-map object layer's per-frame slice is bounded by time and skips
invisible dots cheaply (2026-11-08 micro-stutters: ui_netmap / ui_netmap_objects 3-24 ms).

The object layer is re-plotted from every map object each refresh, sliced at
SlipstreamMinimapObjectsPerFrame (4000) objects per frame. Each object costs ~1-6 us of
cache misses (the LLViewerObject, its parent chain for the position, its region), so a
4000-object slice was 3-24 ms.
  TIME-SLICE     renderObjectsForMap ends a slice on a deadline (kMapSliceMs), checked inside
                 the loop, and returns its cursor.
  EARLY-REJECT   a dot that rounds to no texel (LLNetMap::dotVisible) is skipped before
                 getPositionGlobal.
  NO-RELOCK      LLNetMap::renderPoint does not re-take the image lock per dot (the slice
                 holds it).
"""
import re

NAME = "netmap-slice"
STAGE = "source"
DESCRIPTION = "mini-map object slice: time-bounded, no per-dot relock, invisible dots skipped before the position"
GUARDS = "3-24 ms ui_netmap_objects frames re-plotting a 4000-object slice of the mini-map (2026-11-08)."

OBJ = "indra/newview/llviewerobjectlist.cpp"
MAP = "indra/newview/llnetmap.cpp"


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
    obj = _strip_comments(open(ctx.path(OBJ), encoding="utf-8").read())
    nm = _strip_comments(open(ctx.path(MAP), encoding="utf-8").read())
    bad = []
    fn = _function(obj, "size_t LLViewerObjectList::renderObjectsForMap(")
    if not fn:
        bad.append("LLViewerObjectList::renderObjectsForMap not found; update this check")
    else:
        loop = fn.find("for (size_t oi = start; oi < end; ++oi)")
        if loop < 0:
            bad.append("renderObjectsForMap's slice loop changed; update this check")
        else:
            body = fn[loop:]
            if not re.search(r"kMapSliceMs[^;]*\)\s*\{\s*return oi;", body):
                bad.append("TIME-SLICE: the slice loop does not end on the kMapSliceMs deadline")
            vis = body.find("dotVisible(")
            posi = body.find("getPositionGlobal()")
            if vis < 0 or posi < 0 or vis > posi:
                bad.append("EARLY-REJECT: a dot too small to plot is not skipped before getPositionGlobal")
    rp = _function(nm, "void LLNetMap::renderPoint(")
    if not rp:
        bad.append("LLNetMap::renderPoint not found; update this check")
    elif "LLImageDataLock" in rp:
        bad.append("NO-RELOCK: LLNetMap::renderPoint re-takes the image lock per dot")
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "time-bounded slice, early reject before position, no per-dot relock"
