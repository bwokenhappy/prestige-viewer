"""Source guard: a mesh face EMPTY at a level draws nothing there (#1387).

Impostor meshes swap their content between faces across LODs: face 0 the full model at
High / Medium and a speck at Low, face 1 the reverse (a far box). A face that decoded empty
at the drawn level was treated as "not loaded" and drew another level's geometry, so the far
box was drawn around the near model.
  CPU-EMPTY   llvovolume.cpp's structural-gap fallback asks faceEmptyAt() before borrowing
              groupBestSlot(), and publishes emptyFaceSlot() for an empty level.
  GPU-EMPTY   clean_cull.comp's static branch returns (draws nothing) for slotEmptyLevel()
              before its residency fallback.
"""
import re

NAME = "lod-empty-face"
STAGE = "source"
DESCRIPTION = "a mesh face empty at the drawn LOD draws nothing, not another LOD's geometry"
GUARDS = "#1387: an impostor mesh's low-LOD box drawn around its high-LOD model."

VOL = "indra/newview/llvovolume.cpp"
CULL = "indra/vkclean/shaders/clean_cull.comp"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def run(ctx, result):
    vol = _strip_comments(open(ctx.path(VOL), encoding="utf-8").read())
    cull = _strip_comments(open(ctx.path(CULL), encoding="utf-8").read())
    bad = []

    fb = vol.find("CONV_FALLBACK;")
    empty = vol.find("faceEmptyAt(")
    if fb < 0 or empty < 0 or empty > fb or "emptyFaceSlot()" not in vol[empty:fb]:
        bad.append("CPU-EMPTY: llvovolume.cpp's structural-gap fallback no longer publishes emptyFaceSlot() "
                   "for a face that decoded empty (faceEmptyAt) before borrowing groupBestSlot")

    sel = cull.find("slot_id = lod_groups[group_id].slot[_level];")
    resid = cull.find("if (!slotOk(slot_id)) slot_id = LOD_SENTINEL;", sel if sel >= 0 else 0)
    if sel < 0 or resid < 0 or not re.search(r"if\s*\(\s*slotEmptyLevel\(slot_id\)\s*\)\s*\{[^}]*return 0xFFu;",
                                             cull[sel:resid]):
        bad.append("GPU-EMPTY: clean_cull.comp's static LOD select no longer draws nothing for an "
                   "empty-by-content level (slotEmptyLevel) before the residency fallback")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "empty-by-content LOD levels draw nothing (CPU publishes the empty slot, cull honours it)"
