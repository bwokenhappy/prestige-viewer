"""Regression guard for #1131: system (non-mesh) avatar bodies never rendered on Vulkan.

LLCleanSysAvatar keeps ONE global skin palette for every avatar's system body, recorded from each
part's joint render data (LL's LLAvatarJointMesh::setupJoint order). setupJoint puts an ANCESTOR entry
in front of a skin joint whose parent isn't the previous entry, and for the upper body, lower body and
skirt the first one is mRoot (mPelvis' parent) -- which is no row of the avatar's joint table
(av->getJoint: mSkeleton starts at mPelvis). buildSkin failed the WHOLE skin on any entry it could not
resolve, so once a body part had been captured no avatar's system body (not even eyes or hair) drew.

The rule: an unused palette entry never fails the skin. Only an entry a drawn part's vertex weights
(or a drawn rigid part) reference has to resolve -- else no skin, never a wrong pose -- and it resolves
from that part's own mesh; any other unresolvable entry gets row 0 with an identity (it skins nothing).
The skin is rebuilt when the drawn (part, LOD) set changes (a detached BOM body un-hides parts).

Checked: the source wiring of that rule + the diagnostic, and the shipped meshes' weights (no vertex
of a mesh whose first entry is mRoot references entry 0).
"""
import re
import struct

NAME = "sysavatar-palette"
STAGE = "source"
DESCRIPTION = "system avatar body: an unused palette entry (mRoot) never fails the skin (#1131)"
GUARDS = ("Every system avatar body (and its eyes / hair) not drawn at all because one global palette "
          "entry no drawn vertex uses (mRoot, setupJoint's ancestor entry before mPelvis) has no "
          "skeleton row; a part un-hidden by a detached BOM body waiting on a skin built without it.")

CPP = "indra/newview/llcleansysavatar.cpp"
HDR = "indra/newview/llcleansysavatar.h"
MESHES = ("avatar_upper_body", "avatar_lower_body", "avatar_skirt")


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _body(text, head):
    i = text.find(head)
    if i < 0:
        return None
    j = text.find("\n}", i)
    return text[i:j + 2] if j > 0 else None


def _weights(path):
    """Vertex weights of a Linden Binary Mesh (LLPolyMeshSharedData::loadMesh layout)."""
    with open(path, "rb") as f:
        d = f.read()
    if not d.startswith(b"Linden Binary Mesh 1.0"):
        return None
    has_weights, has_detail = d[24], d[25]
    o = 24 + 2 + 12 + 12 + 1 + 12
    nv = struct.unpack_from("<H", d, o)[0]
    o += 2 + nv * 12 * 3 + nv * 8 + (nv * 8 if has_detail else 0)
    if not has_weights:
        return []
    return struct.unpack_from("<%df" % nv, d, o)


def run(ctx, result):
    cpp = _strip_comments(open(ctx.path(CPP), encoding="utf-8").read())
    hdr = _strip_comments(open(ctx.path(HDR), encoding="utf-8").read())
    bad = []

    build = _body(cpp, "bool LLCleanSysAvatar::buildSkin(")
    if build is None:
        bad.append("llcleansysavatar.cpp: buildSkin not found (renamed?): the rule is unchecked")
    else:
        if "const std::vector<PalUse>& use" not in build:
            bad.append("buildSkin no longer takes the drawn parts' palette use: any entry can fail the skin")
        # every 'return false' after the entry loop starts must sit inside the 'if (u.need)' branch
        loop = build.find("for (U32 e = 0; e < n; ++e)")
        need = build.find("if (u.need)", loop)
        rets = [m.start() for m in re.finditer(r"return false;", build)]
        if loop < 0 or need < 0:
            bad.append("buildSkin: the per-entry 'if (u.need)' gate is gone (an unused entry fails the skin)")
        else:
            end = build.find("s_jn[e] = row;", need)
            for r in rets:
                if loop < r < end and not need < r:
                    bad.append("buildSkin: a 'return false' inside the entry loop outside the 'if (u.need)' branch")
                    break
        if not re.search(r"std::memcpy\(ib, IDENT, sizeof\(IDENT\)\);\s*row = 0;", build):
            bad.append("buildSkin: an unused unresolvable entry no longer gets row 0 + identity")
        if not re.search(r"LL_WARNS\(\"VKSysAvatar\"\)[^;]*#1131 avatar[^;]*no system body skin", build, re.S) \
                or "st.skin_fail_gen != mPalGen" not in build:
            bad.append("buildSkin: the once-per-palette-gen 'no system body skin' diagnostic is missing")

    pal = _body(cpp, "bool LLCleanSysAvatar::ensurePalette(")
    if pal is None or "mJrdUsed[part][ref_lod].swap(used)" not in pal or "getWeights()" not in pal:
        bad.append("ensurePalette: the per-jrd 'used by a vertex weight' table (mJrdUsed) is not built")
    use = _body(cpp, "void LLCleanSysAvatar::notePalUse(")
    if use is None or "mJrdUsed[part][r]" not in use or "mRigidPal[part][lod]" not in use:
        bad.append("notePalUse: drawn parts no longer mark the entries they reference (skinned + rigid)")
    tick = _body(cpp, "void LLCleanSysAvatar::tick(")
    if tick is None or "st.skin_need_sig != need_sig" not in tick \
            or not re.search(r"if \(want\[p\]\.on\) notePalUse\(av, p, want\[p\]\.lod, s_use\);", tick) \
            or "if (buildSkin(av, st, s_use)) st.skin_need_sig = need_sig;" not in tick:
        bad.append("tick: the skin is not rebuilt when the drawn (part, LOD) set changes (BOM detach)")
    if "U64 skin_need_sig" not in hdr or "U32 skin_fail_gen" not in hdr:
        bad.append("llcleansysavatar.h: AvState lost skin_need_sig / skin_fail_gen")

    # The data fact the stand-in relies on: no shipped body / skirt vertex references entry 0 (mRoot).
    for m in MESHES:
        try:
            w = _weights(ctx.path("indra/newview/character/%s.llm" % m))
        except (OSError, struct.error) as e:
            bad.append("%s.llm: unreadable (%s)" % (m, e))
            continue
        if not w:
            bad.append("%s.llm: no weights parsed (format changed?)" % m)
        elif min(w) < 1.0:
            bad.append("%s.llm: a vertex weight < 1.0 references jrd entry 0 (mRoot): the body now needs an "
                       "mRoot row (its skin will fail, see the 'no system body skin' log line)" % m)

    if bad:
        result.fail("\n".join(bad))
        return
    result.summary = "unused palette entries (mRoot) never fail the skin; rebuilt on drawn-set change"
