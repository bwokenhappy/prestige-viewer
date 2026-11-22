"""Regression guard for #1585: the VR headless body (SlipstreamVRShowBody).

In VR the own avatar is drawn in the headset view without its head: own rigged vertices weighted at
least half to mHead or a joint under it collapse onto mHead (clean_headless_self.glsl, run by every
rigged VS variant), the system head parts and own worn flexi prims on head points (instance bit 18)
are dropped by the rigged VS, own worn rigid prims on head points by the main cull. Mirrors, probes,
shadows and RT draw the whole avatar. The mode is one push word (u_ambient_color.w = 0.5) + one cull
bit, so nothing runs outside it.

Checked: the SHIPPED skeleton (character/avatar_skeleton.xml + the attachment points of
avatar_lad.xml) under the head rule of llheadlessbody.h (mHead and its subtree; mNeck and the Neck
point not), and the wiring the unit test (tests/llheadlessbody_test.cpp) cannot see: the push word's
value on the host vs the shader tests, the cull bit on both sides, the state word (RC_WBASE) the
cull writes and the VS reads, the SkinMeta word offsets of the head record, the bit-18 flag on both
sides, binding 70 (layout, write, declaration), the setting and its checkbox, the mode gates.
"""
import re
import xml.etree.ElementTree as ET

NAME = "headless-self"
STAGE = "source"
DESCRIPTION = "VR headless body: the shipped skeleton's head set + host / shader wiring (#1585)"
GUARDS = ("A mesh head / hair / Bento face part or a head attachment left in front of the eyes in VR, "
          "the head test running (or the whole avatar vanishing) outside the mode, mirrors / shadows "
          "losing the head, or the VS reading the wrong weights / skin words after a layout change.")

SKEL = "indra/newview/character/avatar_skeleton.xml"
LAD = "indra/newview/character/avatar_lad.xml"


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _skeleton(ctx):
    """name -> parent name for every bone and collision volume, + every attachment point
    (named 'AP:<name>') under its joint -- the hierarchy LLVOAvatar builds."""
    parent = {}
    root = ET.fromstring(_read(ctx, SKEL))

    def walk(node, up):
        for ch in node:
            if ch.tag in ("bone", "collision_volume"):
                parent[ch.get("name")] = up
                if ch.tag == "bone":
                    walk(ch, ch.get("name"))
    walk(root, None)
    lad = _read(ctx, LAD)
    for m in re.finditer(r"<attachment_point\b(.*?)>", lad, re.S):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
        if "name" in attrs and "joint" in attrs:
            parent["AP:" + attrs["name"]] = attrs["joint"]
    return parent


def _head_side(parent, name):
    seen = 0
    while name is not None and seen <= len(parent):
        if name == "mHead":
            return True
        name = parent.get(name)
        seen += 1
    return False


def _skeleton_rule(ctx, bad):
    p = _skeleton(ctx)
    if "mHead" not in p or "mNeck" not in p:
        bad.append("avatar_skeleton.xml: mHead / mNeck not found -- the head rule covers nothing")
        return 0
    face = [n for n in p if n.startswith(("mFace", "mEye", "mSkull"))]
    if len(face) < 40:
        bad.append("avatar_skeleton.xml: only %d face / eye / skull bones found (Bento has ~45)" % len(face))
    for n in face + ["mHead", "HEAD"]:
        if n in p and not _head_side(p, n):
            bad.append("%s is not under mHead in the shipped skeleton: the headless body keeps it" % n)
    for n in ("mNeck", "NECK", "mChest", "mCollarLeft", "mShoulderLeft", "mWristLeft", "mPelvis",
              "mTail1", "mWing1Left", "mHindLimb1Left"):
        if n in p and _head_side(p, n):
            bad.append("%s is head-side: the headless body would lose it" % n)
    head_points = ("Skull", "Mouth", "Chin", "Left Ear", "Right Ear", "Left Eyeball", "Right Eyeball",
                   "Nose", "Jaw", "Alt Left Ear", "Alt Right Ear", "Alt Left Eye", "Alt Right Eye", "Tongue")
    for a in head_points:
        k = "AP:" + a
        if k not in p:
            bad.append("avatar_lad.xml: attachment point %s not found" % a)
        elif not _head_side(p, k):
            bad.append("attachment point %s is not under mHead: worn head prims stay in the view" % a)
    for a in ("Neck", "Chest", "Left Hand", "Spine", "Tail Tip", "Avatar Center"):
        k = "AP:" + a
        if k in p and _head_side(p, k):
            bad.append("attachment point %s is head-side: worn prims there would vanish" % a)
    return sum(1 for n in p if _head_side(p, n))


def _wiring(ctx, bad):
    h = _read(ctx, "indra/newview/vkgfx/llcleanrenderervk.h")
    r = _read(ctx, "indra/newview/vkgfx/llcleanrenderervk.cpp")
    cull = _strip_comments(_read(ctx, "indra/vkclean/shaders/clean_cull.comp"))
    vs = _strip_comments(_read(ctx, "indra/vkclean/shaders/clean_draw_rigged.vert"))
    hl = _strip_comments(_read(ctx, "indra/vkclean/shaders/clean_headless_self.glsl"))
    sv = _strip_comments(_read(ctx, "indra/vkclean/shaders/clean_skinvert.glsl"))
    store = _read(ctx, "indra/newview/llcleanskinstore.h")
    hb = _read(ctx, "indra/newview/llheadlessbody.h")
    inst = _read(ctx, "indra/newview/llcleaninstancestore.h")
    sysav = _read(ctx, "indra/newview/llcleansysavatar.h")
    st = _read(ctx, "indra/newview/app_settings/settings.xml")
    xui = _read(ctx, "indra/newview/skins/default/xui/en/panel_preferences_safety.xml")
    av = _read(ctx, "indra/newview/llvoavatar.cpp")

    # the push word: hide 1 > 0.5 (whole), headless 0.5 in (0.4, 0.6) and > 0.2 (system head), 0.25 not headless
    m = re.search(r"return hideSelfNow\(\) \? 1\.f : \(headlessSelfNow\(\) \? ([0-9.]+)f : \(mHideSelfHead \? ([0-9.]+)f : 0\.f\)\);", h)
    g = re.search(r"bool headlessPass\(float w\) \{ return w > ([0-9.]+) && w < ([0-9.]+); \}", hl)
    if not m or not g:
        bad.append("selfHideW / headlessPass not found in their expected form: the push word is unchecked")
    else:
        hw, sw, lo, hi = float(m.group(1)), float(m.group(2)), float(g.group(1)), float(g.group(2))
        if not (lo < hw < hi):
            bad.append("selfHideW's headless value %.2f is not a headless pass (%.2f, %.2f)" % (hw, lo, hi))
        if lo < 1.0 < hi or lo < sw < hi or lo < 0.0 < hi:
            bad.append("hide (1), system-head (%.2f) or none (0) would run the headless head test" % sw)
        if hw > 0.5 or not hw > 0.2:
            bad.append("the headless value %.2f must hide the system head (> 0.2) but not the whole avatar (<= 0.5)" % hw)
    if "pc.u_ambient_color.w > 0.5 && (inst.flags & 8192u) != 0u" not in vs \
            or "pc.u_ambient_color.w > 0.2 && (inst.flags & 262144u) != 0u" not in vs:
        bad.append("clean_draw_rigged.vert: the whole-avatar (> 0.5) / head-part (> 0.2) tests changed")
    if "headlessPass(pc.u_ambient_color.w) && (inst.flags & (8192u | 8u)) == 8192u && rc_ok" not in vs \
            or "headlessCollapse(inst.skin_id, rig_cache[rcb + RC_WBASE]" not in vs:
        bad.append("clean_draw_rigged.vert: the per-vertex head test is not gated on the mode + own avatar")
    if '#include "clean_headless_self.glsl"' not in vs:
        bad.append("clean_draw_rigged.vert does not include clean_headless_self.glsl")

    # the cull bit
    hm = re.search(r"kCullHeadlessSelf = (\d+)u;", h)
    cm = re.search(r"#define HEADLESS_SELF\s+(\d+)u", cull)
    if not hm or not cm or hm.group(1) != cm.group(1):
        bad.append("the cull's headless bit differs between kCullHeadlessSelf and clean_cull.comp HEADLESS_SELF")
    else:
        bit = int(cm.group(1))
        used = set()
        for n in re.findall(r"pc\.u_flags & (\d+)u", cull):
            used.add(int(n))
        for name in ("SHADOW_SPLIT_STATIC", "SHADOW_SPLIT_DYNAMIC", "SEA_FLOOR_PASS", "CULL_ORTHO", "CULL_DIAG_BIT"):
            dm = re.search(r"#define %s\s+(0x[0-9a-fA-F]+|\d+)u" % name, cull)
            if dm:
                used.add(int(dm.group(1), 0))
        if any(u & bit for u in used if u != bit):
            bad.append("HEADLESS_SELF (%d) collides with another u_flags bit of the cull" % bit)
    if "if (headlessSelfNow()) push.flags |= kCullHeadlessSelf;" not in r \
            or "push.flags = hideSelfNow() ? 1u : 0u;" not in r:
        bad.append("recordWorld does not set the cull's hide / headless bits from hideSelfNow() / headlessSelfNow()")
    # the cluster path has no per-vertex head test: under the mode it must hide the own avatar whole
    if "bool hideSelfNow() const { return mHideSelf || (mHeadlessSelf && clusterOwnsRigged()); }" not in h \
            or "if (hideSelfNow()) v.flags |= CLV_HIDE_SELF;" not in r or "si.hide_self = hideSelfNow();" not in r:
        bad.append("the cluster LOD path does not degrade the headless body to the whole hide (its head would show)")
    if not re.search(r"pc\.u_hud_pass == 0u && \(pc\.u_flags & HEADLESS_SELF\) != 0u\s*"
                     r"&& \(_flags & \(8192u \| 512u \| SELF_HEAD_FLAG \| 1u\)\) == \(8192u \| 512u \| SELF_HEAD_FLAG\)", cull):
        bad.append("clean_cull.comp: own head-point rigid attachments are not dropped in the main pass only")

    # the state word
    w1 = re.search(r"#define RC_WBASE\s+(\d+)u", cull)
    w2 = re.search(r"#define RC_WBASE\s+(\d+)u", sv)
    if not w1 or not w2 or w1.group(1) != w2.group(1):
        bad.append("RC_WBASE differs between clean_cull.comp and clean_skinvert.glsl")
    elif "rig_cache[cb + %su] = wbase;" % w1.group(1) not in cull:
        bad.append("RC_WBASE is no longer the skin cache's weight key word (rig_cache[cb + %su] = wbase)" % w1.group(1))
    if not re.search(r"if \(!rig_cached && \(pc\.u_flags & HEADLESS_SELF\) != 0u && \(flags & 8192u\) != 0u\s*"
                     r"&& cb \+ RC_WORDS <= uint\(rig_cache\.length\(\)\)\)\s*rig_cache\[cb \+ RC_WBASE\] = wbase;", cull):
        bad.append("clean_cull.comp: the per-frame RC_WBASE write is not gated (mode, own row, row bounds)")

    # the SkinMeta head words: avatar_idx, joint_count, head_lo[2] (2, 3), joint_nums[110], head_hi[2] (114, 115)
    sm = re.search(r"struct SkinMeta\s*\{(.*?)\};", store, re.S)
    if not sm:
        bad.append("LLCleanSkinStore::SkinMeta not found")
    else:
        word, off = 0, {}
        for mm in re.finditer(r"(U32|S32|F32)\s+(\w+)(?:\[(\w+)(?:\s*\*\s*(\d+))?\])?;", sm.group(1)):
            n = mm.group(3)
            cnt = 1 if n is None else (110 if n == "MAX_JOINTS" else int(n))
            if mm.group(4):
                cnt *= int(mm.group(4))
            off[mm.group(2)] = word
            word += cnt
        lo = re.search(r"const uint HL_META_LO\s*=\s*(\d+)u;", hl)
        hi = re.search(r"const uint HL_META_HI\s*=\s*(\d+)u;", hl)
        if off.get("head_lo") != 2 or off.get("head_hi") != 114 or word != 1892:
            bad.append("SkinMeta layout changed (head_lo %s, head_hi %s, %d words)" % (off.get("head_lo"), off.get("head_hi"), word))
        if not lo or not hi or int(lo.group(1)) != off.get("head_lo") or int(hi.group(1)) != off.get("head_hi"):
            bad.append("clean_headless_self.glsl reads the head words at other offsets than SkinMeta's")
    mk = re.search(r"kMaskHiBits = (0x[0-9A-Fa-f]+)u;", hb)
    sk = re.search(r"const uint HL_MASK_HI\s*=\s*(0x[0-9A-Fa-f]+)u;", hl)
    sh = re.search(r"kHeadShift\s*=\s*(\d+);", hb)
    if not mk or not sk or int(mk.group(1), 16) != int(sk.group(1), 16) or not sh or "(m.w >> %su) - 1u" % sh.group(1) not in hl:
        bad.append("the head_hi[1] packing (mask bits / head joint shift) differs between llheadlessbody.h and the shader")
    if "const uint HL_SKIN_MAX  = 109u;" not in hl:
        bad.append("clean_headless_self.glsl: the joint clamp is not the pre-skin's (MAX_JOINTS - 1 = 109)")
    if "2u * hlHeadWeight(wbase + local, m) < 65535u" not in hl or "2u * headWeight(joint, q, w) >= kWeightOne" not in hb:
        bad.append("the 0.5 head-weight threshold differs between the shader and llheadlessbody.h")

    # the bit-18 flag
    f1 = re.search(r"kSelfHeadFlag = (\d+)u;", inst)
    f2 = re.search(r"#define SELF_HEAD_FLAG\s+(\d+)u", cull)
    f3 = re.search(r"kHeadFlag = (\d+)u;", sysav)
    if not f1 or not f2 or not f3 or not (f1.group(1) == f2.group(1) == f3.group(1) == "262144"):
        bad.append("the own-head-part flag (bit 18) differs between the instance store, the cull and the system body")

    # binding 70
    if "db0[70].binding=70; db0[70].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;" not in r \
            or "ww.dstBinding=70" not in r or "db0f[70] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;" not in r:
        bad.append("draw set 0 binding 70 (the skin weights) is not in the layout / never written")
    if "layout(std430, set = 0, binding = 70) readonly buffer HeadlessWeights" not in hl:
        bad.append("clean_headless_self.glsl: binding 70 not declared")

    # the setting, its checkbox, the rule
    sm2 = re.search(r"<key>SlipstreamVRShowBody</key>\s*<map>(.*?)</map>", st, re.S)
    if not sm2 or "<string>Boolean</string>" not in sm2.group(1) \
            or not re.search(r"<key>Persist</key>\s*<integer>1</integer>", sm2.group(1)) \
            or not re.search(r"<key>Value</key>\s*<integer>1</integer>", sm2.group(1)):
        bad.append("settings.xml: SlipstreamVRShowBody (Boolean, persisted, default on) missing")
    if 'control_name="SlipstreamVRShowBody"' not in xui:
        bad.append("panel_preferences_safety.xml: the 'Show my body in VR' checkbox is missing")
    if "if (!LLVRCamera::firstPerson()) return false;" not in av:
        bad.append("LLVOAvatar::selfHeadlessInMainView is not VR-only")


def run(ctx, result):
    bad = []
    n = _skeleton_rule(ctx, bad)
    _wiring(ctx, bad)
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "%d head-side joints / points in the shipped skeleton; host and shaders agree" % n
