"""Source guard: the seated third-person camera frames the avatar, never the linkset root (#1528).

#95 pivoted the TPS camera on the ROOT prim of the sat-on linkset (getTPSVehiclePivot). The
root of a linkset can be anywhere: a house whose root sits below the floor, an elevator whose
car is a child prim. Seated there, the camera centred deep below the avatar. The camera's
pivot is the avatar itself, seated or not.
  NO-ROOT-PIVOT   llagentcamera.cpp / sltpscamera.cpp take no position from the linkset root:
                  no getRootEdit(), no getTPSVehiclePivot, and every gAgentAvatarp->getRoot()
                  result is only used for flagCameraDecoupled() or as the TPS collision skip.
  AVATAR-PIVOT    getTPSCameraOrigin, getShoulderFocus and getShoulderPosition pivot on
                  getAvatarRootPosition(); the avatar-focus fallback of
                  calcFocusPositionTargetGlobal is gAgent.getPositionGlobal().
"""
import re

NAME = "seated-camera"
STAGE = "source"
DESCRIPTION = "the seated third-person camera pivots on the avatar, not on the linkset root"
GUARDS = ("#1528: seated on a child prim of a linkset (elevator car), the TPS camera centred on the "
          "linkset root, which can be far below the avatar.")

CAM = "indra/newview/llagentcamera.cpp"
TPS = "indra/newview/sltpscamera.cpp"

# variables holding gAgentAvatarp->getRoot(), and the only uses each may have
ROOT_VARS = {
    "avatarp_root": (r"if\s*\(\s*avatarp_root\s*\)", r"avatarp_root->flagCameraDecoupled\(\)"),
    "root_object": (r"root_object->flagCameraDecoupled\(\)",),
    "skip_root": (r"SSCombatRay::cast\([^;]*\bskip_root\b",),
}


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function(text, signature):
    """The body of the function whose definition starts with signature (brace matched)."""
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


def _no_root_pivot(name, text, bad):
    if "getRootEdit(" in text:
        bad.append("NO-ROOT-PIVOT: %s calls getRootEdit()" % name)
    if "getTPSVehiclePivot" in text:
        bad.append("NO-ROOT-PIVOT: %s uses getTPSVehiclePivot (the linkset root as camera pivot)" % name)
    for m in re.finditer(r"getRoot\(\)", text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        line = text[line_start:text.find("\n", m.start())]
        decl = re.search(r"\b([A-Za-z_]\w*)\s*=[^=;]*getRoot\(\)", line)
        var = decl.group(1) if decl else None
        if var not in ROOT_VARS:
            bad.append("NO-ROOT-PIVOT: %s: unexpected getRoot() use: %s" % (name, line.strip()))
            continue
        allowed = ROOT_VARS[var]
        rest = text[m.end():]
        for use in re.finditer(r"\b%s\b" % var, rest):
            ok = False
            for pattern in allowed:
                for a in re.finditer(pattern, rest):
                    if a.start() <= use.start() < a.end():
                        ok = True
                        break
                if ok:
                    break
            if not ok:
                ul0 = rest.rfind("\n", 0, use.start()) + 1
                bad.append("NO-ROOT-PIVOT: %s: the linkset root (%s) is used for more than the camera-decoupled "
                           "flag / collision skip: %s" % (name, var, rest[ul0:rest.find("\n", use.start())].strip()))


def run(ctx, result):
    cam = _strip_comments(open(ctx.path(CAM), encoding="utf-8").read())
    tps = _strip_comments(open(ctx.path(TPS), encoding="utf-8").read())
    bad = []

    _no_root_pivot(CAM, cam, bad)
    _no_root_pivot(TPS, tps, bad)

    origin = _function(cam, "LLVector3 LLAgentCamera::getTPSCameraOrigin(")
    if not origin or "getAvatarRootPosition()" not in origin:
        bad.append("AVATAR-PIVOT: getTPSCameraOrigin does not pivot on getAvatarRootPosition()")
    for sig in ("LLVector3 SLTPSCamera::getShoulderFocus(", "LLVector3 SLTPSCamera::getShoulderPosition("):
        body = _function(tps, sig)
        if not body or "gAgentCamera.getAvatarRootPosition()" not in body:
            bad.append("AVATAR-PIVOT: %s) does not pivot on getAvatarRootPosition()" % sig)
    focus = _function(cam, "LLVector3d LLAgentCamera::calcFocusPositionTargetGlobal(")
    if not focus or not re.search(r"return gAgent\.getPositionGlobal\(\) \+ calcThirdPersonFocusOffset\(\);\s*\}\s*\}\s*$",
                                  focus):
        bad.append("AVATAR-PIVOT: calcFocusPositionTargetGlobal's avatar focus is not "
                   "gAgent.getPositionGlobal() + calcThirdPersonFocusOffset()")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "TPS / third-person camera pivots on the avatar; linkset root only for decoupled flag / collision skip"
