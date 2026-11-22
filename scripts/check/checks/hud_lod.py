"""Source guard: HUD attachments capture and draw their FINEST level (#1676).

The VK cull draws a HUD instance at its highest RESIDENT level, so the ingest must make the
finest level resident. LL's live volume of a HUD prim keeps its creation detail (LOD 0: a
cylinder's 6-sided profile) under VK, and the LOD convergence block used to skip HUDs, so
round HUD gauges drew as hexagons.
  CONVERGE    llvovolume.cpp's LOD CONVERGENCE block runs for HUD attachments (its gate does not
              exclude isHUDAttachment()) and forces their desired level to the finest
              (NUM_LOD - 1), never the world-camera distance level.
  LL-LOD      calcLOD still gives HUD attachments cur_detail = 3 (meshes / sculpts fetch and
              tessellate at the finest level).
  CULL        clean_cull.comp draws HUD instances through the highest-resident branch, and the
              reduced-avatar LOD bias does not apply to them.
  GC          the in-use slot scan keeps every level of HUD instances (flag 8 is own-LOD).
"""
import re

NAME = "hud-lod"
STAGE = "source"
DESCRIPTION = "HUD attachments capture and draw their finest LOD, never a distance LOD"
GUARDS = "#1676: round HUD prims (sailing compasses) drawn as hexagons (LOD 0) under Vulkan."

VOL = "indra/newview/llvovolume.cpp"
CULL = "indra/vkclean/shaders/clean_cull.comp"
STORE = "indra/newview/llcleaninstancestore.cpp"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def run(ctx, result):
    vol = _strip_comments(open(ctx.path(VOL), encoding="utf-8").read())
    cull = _strip_comments(open(ctx.path(CULL), encoding="utf-8").read())
    store = _strip_comments(open(ctx.path(STORE), encoding="utf-8").read())
    bad = []

    # CONVERGE: the block that ends in noteConverge(); its gate is the `if (...) {` right
    # after the conv_waiting declaration.
    wait = vol.find("bool conv_waiting = false;")
    note = vol.find("noteConverge(", wait if wait >= 0 else 0)
    gate = re.match(r"\s*if\s*\(([^\n{]*)\)\s*\{", vol[wait + len("bool conv_waiting = false;"):]) if wait >= 0 else None
    if wait < 0 or note < 0 or gate is None:
        bad.append("CONVERGE: llvovolume.cpp's LOD CONVERGENCE block (conv_waiting ... noteConverge) not found")
    else:
        if "isHUDAttachment" in gate.group(1):
            bad.append("CONVERGE: the LOD CONVERGENCE gate excludes HUD attachments again -- their finest "
                       "level is then never captured (live level = creation LOD 0)")
        body = vol[wait:note]
        if not re.search(r"conv_hud\s*=\s*pObj->isHUDAttachment\(\)", body) \
                or not re.search(r"S32\s+desired\s*=\s*LLCleanMeshAsset::NUM_LOD\s*-\s*1\s*;", body) \
                or not re.search(r"if\s*\(\s*!conv_hud\s*\)", body):
            bad.append("CONVERGE: HUD attachments no longer converge on the finest level "
                       "(desired = NUM_LOD - 1 unless !conv_hud)")

    if not re.search(r"if\s*\(\s*isHUDAttachment\(\)\s*\)\s*\{\s*cur_detail\s*=\s*3\s*;", vol):
        bad.append("LL-LOD: calcLOD no longer forces cur_detail = 3 for HUD attachments")

    if not re.search(r"if\s*\(\s*_rigged\s*\|\|\s*_is_hud\s*\|\|\s*_attach\s*\)", cull):
        bad.append("CULL: clean_cull.comp no longer draws HUD instances at the highest resident level")
    if not re.search(r"if\s*\(\s*!_is_hud\s*&&\s*_av\s*<\s*AV_VIS_SLOTS\s*&&\s*\(av_vis\[_av\]\s*&\s*AVV_REDUCED\)", cull):
        bad.append("CULL: the reduced-avatar LOD bias is no longer kept off HUD instances")

    own = re.search(r"own_lod\s*=\s*\(inst\.flags\s*&\s*\(([^;]*)\)\)\s*!=\s*0u;", store)
    if own is None or not re.search(r"\b8u\b", own.group(1)):
        bad.append("GC: the in-use LOD scan no longer keeps every level of HUD instances (flag 8 own_lod)")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "HUD attachments converge on, keep and draw their finest LOD"
