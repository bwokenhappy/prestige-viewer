#!/usr/bin/env python3
"""Open Pony pupils (#1567): the viewer's pupil math against the dev kit's own eye rig, on the kit's
own meshes, without Blender.

Reads [OP] OpenPony Mesh and Rigging.blend (scripts/tools/blend_rig_dump.py's reader): the eye
shell OBIKEye (the Shrinkwrap target), the eye white OBEye and the pupil discs OBPupil (weighted to
mFaceNoseLeft / Right). For looks left / right / up / down it poses each pupil two ways:

  kit     the rig: IKEyefocus faces IKEyecenter, the eye targets ride it +-91.93 mm sideways, the
          arm (IKEyeLeft / Right) damped-tracks its target, FaceNose* copies the arm's tail and
          shrinkwraps onto OBIKEye (nearest surface point, "Align to Normal" on the bone's Y);
  viewer  SLPoseIKMath::pupilPose (indra/newview/slposeikmath.h, compiled here) with the numbers
          of the "nose" eye set in indra/newview/app_settings/posestudio/ik_chains.xml;

and prints per eye: the pupil centre's signed distance to the shell (mm), how the disc's gap over
the eye white changed from the bind pose (min .. max over the visible disc, mm), the distance from
the kit's pupil (mm) and the angle from the kit's turn (degrees). Exit status 1 when the viewer's
centre or gap change is off by more than --tolerance mm (the kit's own gap change is -0.30 ..
+0.17 mm: its Shrinkwrap pulls the bind pupil 0.23 mm down onto the shell). The kit has no eye
limit: past --limit the viewer holds the look at the edge, so "from kit" grows there.

  scripts/tools/pupil_rig_check.py BLEND [--limit DEG] [--wide] [--tolerance MM]

In world, --capture LOG instead checks what the viewer actually put on the pupil bones: each
#EyeCapture sample (SLEyeCaptureLog) of mFaceNoseLeft / Right, its offset from rest and local
rotation as the skeleton holds them, against the shell: how far the logged position is off the
shell (mm, + outside) and how far the logged rotation is from the turn that lays the pupil flat
there (degrees). No .blend needed.

  scripts/tools/pupil_rig_check.py --capture ~/.slipstreamviewer/logs/Slipstream.log
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from blend_rig_dump import Blend  # noqa: E402

# The rig (Blender world space, metres; ik_chains.xml has the derivation).
SIDES = {
    "L": dict(joint="mFaceNoseLeft", head=(0.09209, -0.34602, 0.91801), tail=(0.1037, -0.35835, 0.91801),
              pivot=(0.06933, -0.28784, 0.9082), arm_tail=(0.06694, -0.34366, 0.9081), off=0.09193),
    "R": dict(joint="mFaceNoseRight", head=(-0.09209, -0.34602, 0.91801), tail=(-0.1037, -0.35835, 0.91801),
              pivot=(-0.06933, -0.28784, 0.9082), arm_tail=(-0.06694, -0.34366, 0.9081), off=-0.09193),
}
EYE_CENTER = np.array([0.0, -0.2702, 0.92073])
FOCUS_DISTANCE = 0.7254      # the rig's rest focus from IKEyecenter
VISIBLE_DISC = 0.022         # the part of the pupil disc the lids leave visible (metres from the head)

# Blender (x, y, z) -> SL (-y, x, z) and back, as a matrix on column vectors.
B2S = np.array([[0.0, -1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]])

DRIVER = r"""
#include "slposeikmath.h"
#include <cstdio>
using namespace SLPoseIKMath;
// Lines: "0 <29 numbers>" -> pupilPose (offset, turn axis, angle);
//        "1 <centre 3> <axes 9> <point 3>" -> shellNearest (point, normal).
int main()
{
    double v[29];
    int mode;
    while (std::scanf("%d", &mode) == 1)
    {
        const int n = mode == 0 ? 29 : 15;
        for (int i = 0; i < n; ++i)
            if (std::scanf("%lf", &v[i]) != 1)
                return 0;
        if (mode == 1)
        {
            V3 axes[3];
            for (int k = 0; k < 3; ++k)
                axes[k] = V3(v[3 + 3 * k], v[4 + 3 * k], v[5 + 3 * k]);
            V3 point, normal;
            shellNearest(V3(v[0], v[1], v[2]), axes, V3(), V3(v[12], v[13], v[14]), point, normal);
            std::printf("%.12g %.12g %.12g %.12g %.12g %.12g\n", point.x, point.y, point.z, normal.x, normal.y, normal.z);
            std::fflush(stdout);
            continue;
        }
        PupilBone p;
        p.mModel = PupilBone::SHELL;
        p.mPivot = V3(v[0], v[1], v[2]);
        p.mArm = v[3];
        p.mAimOffset = V3(v[4], v[5], v[6]);
        p.mAimCenter = V3(v[7], v[8], v[9]);
        p.mShellCenter = V3(v[10], v[11], v[12]);
        for (int k = 0; k < 3; ++k)
            p.mShellAxes[k] = V3(v[13 + 3 * k], v[14 + 3 * k], v[15 + 3 * k]);
        p.mAxis = V3(v[22], v[23], v[24]);
        const PupilPose pose = pupilPose(p, V3(v[25], v[26], v[27]), V3(1, 0, 0), v[28]);
        std::printf("%.12g %.12g %.12g %.12g %.12g %.12g %.12g\n", pose.mOffset.x, pose.mOffset.y, pose.mOffset.z,
                    pose.mTurn.mAxis.x, pose.mTurn.mAxis.y, pose.mTurn.mAxis.z, pose.mTurn.mAngle);
        std::fflush(stdout);
    }
}
"""


# ---------------------------------------------------------------------------
# Meshes
# ---------------------------------------------------------------------------

def layers(b, cd):
    out = {}
    addr = b.get(cd[0], cd[1], "layers")
    lay = b.deref(addr) if addr else None
    if not lay:
        return out
    sz = b.type_len["CustomDataLayer"]
    for k in range(b.get(cd[0], cd[1], "totlayer") or 0):
        lo = lay[1] + k * sz
        t = b.get("CustomDataLayer", lo, "type")
        out[b.get("CustomDataLayer", lo, "name") or "type%d" % t] = (t, b.get("CustomDataLayer", lo, "data"))
    return out


def read_mesh(b, name):
    """World vertices, triangles, and each vertex's strongest vertex group name."""
    for obj in b.blocks_of("Object"):
        if b.idname(obj) != name:
            continue
        os_, oo = obj
        mat = np.array(b.get(os_, oo, "object_to_world") or b.get(os_, oo, "obmat")).reshape(4, 4)
        ms, mo = b.deref(b.get(os_, oo, "data"))
        nv, npoly, nloop = b.get(ms, mo, "totvert"), b.get(ms, mo, "totpoly"), b.get(ms, mo, "totloop")
        vl, ll, pl = layers(b, b.get(ms, mo, "vdata")), layers(b, b.get(ms, mo, "ldata")), layers(b, b.get(ms, mo, "pdata"))
        if "position" in vl:
            blk, inner = b.find(vl["position"][1])
            v = np.frombuffer(b.m, dtype="<f4", count=3 * nv, offset=blk[1] + inner).reshape(nv, 3).astype(float)
        else:
            blk, inner = b.find(vl["type0"][1])
            sz = b.type_len["MVert"]
            v = np.array([b.get("MVert", blk[1] + inner + i * sz, "co") for i in range(nv)], dtype=float)
        if ".corner_vert" in ll:
            blk, inner = b.find(ll[".corner_vert"][1])
            cv = np.frombuffer(b.m, dtype="<i4", count=nloop, offset=blk[1] + inner)
            blk, inner = b.find(b.get(ms, mo, "poly_offset_indices"))
            po = np.frombuffer(b.m, dtype="<i4", count=npoly + 1, offset=blk[1] + inner)
            polys = [(po[i], po[i + 1] - po[i]) for i in range(npoly)]
        else:
            blk, inner = b.find(ll["type26"][1])
            sz = b.type_len["MLoop"]
            cv = np.array([b.get("MLoop", blk[1] + inner + i * sz, "v") for i in range(nloop)])
            blk, inner = b.find([d for (t, d) in pl.values() if t == 25][0])
            sz = b.type_len["MPoly"]
            polys = [(b.get("MPoly", blk[1] + inner + i * sz, "loopstart"), b.get("MPoly", blk[1] + inner + i * sz, "totloop"))
                     for i in range(npoly)]
        tris = []
        for start, count in polys:
            idx = cv[start:start + count]
            tris += [(idx[0], idx[k], idx[k + 1]) for k in range(1, len(idx) - 1)]
        groups = [b.get(g[0], g[1], "name") for g in b.listbase(b.get(ms, mo, "vertex_group_names"))]
        best = [None] * nv
        dv = [d for (t, d) in vl.values() if t == 2]
        if dv and groups:
            blk, inner = b.find(dv[0])
            sz = b.type_len["MDeformVert"]
            for i in range(nv):
                off = blk[1] + inner + i * sz
                dw, tot = b.get("MDeformVert", off, "dw"), b.get("MDeformVert", off, "totweight")
                if dw and tot:
                    db, di = b.find(dw)
                    ws = [struct.unpack_from("<if", b.m, db[1] + di + k * 8) for k in range(tot)]
                    best[i] = groups[max(ws, key=lambda w: w[1])[0]]
        return v @ mat[:3, :3] + mat[3, :3], np.array(tris, dtype=np.int64), best
    raise SystemExit("no object %s in the .blend" % name)


class Surface:
    """A triangle mesh: closest point, face and smooth (vertex-interpolated) normals."""

    def __init__(self, v, t):
        a, b, c = v[t[:, 0]], v[t[:, 1]], v[t[:, 2]]
        n = np.cross(b - a, c - a)
        area = np.linalg.norm(n, axis=1)
        ok = area > 1e-14
        self.t, self.a, self.b, self.c = t[ok], a[ok], b[ok], c[ok]
        self.fn = n[ok] / area[ok][:, None]
        vn = np.zeros_like(v)
        for k in range(3):
            np.add.at(vn, self.t[:, k], n[ok])
        self.vn = vn / np.maximum(np.linalg.norm(vn, axis=1), 1e-30)[:, None]
        self.cent = (self.a + self.b + self.c) / 3.0

    def closest(self, p):
        idx = np.argpartition(((self.cent - p) ** 2).sum(1), 63)[:64]
        best = None
        for i in idx:
            q, bc = closest_on_triangle(p, self.a[i], self.b[i], self.c[i])
            d = ((q - p) ** 2).sum()
            if best is None or d < best[0]:
                best = (d, q, bc, i)
        _, q, bc, i = best
        tv = self.t[i]
        n = bc[0] * self.vn[tv[0]] + bc[1] * self.vn[tv[1]] + bc[2] * self.vn[tv[2]]
        return q, n / np.linalg.norm(n), self.fn[i]

    def signed(self, p, inside):
        """Signed distance (outward +) from p, `inside` a point within the closed shape."""
        q, _, fn = self.closest(p)
        if np.dot(fn, q - inside) < 0:
            fn = -fn
        return float(np.dot(p - q, fn))


def closest_on_triangle(p, a, b, c):
    """Ericson, Real-Time Collision Detection 5.1.5: point and barycentrics."""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = ab @ ap, ac @ ap
    if d1 <= 0 and d2 <= 0:
        return a, (1.0, 0.0, 0.0)
    bp = p - b
    d3, d4 = ab @ bp, ac @ bp
    if d3 >= 0 and d4 <= d3:
        return b, (0.0, 1.0, 0.0)
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3)
        return a + v * ab, (1 - v, v, 0.0)
    cp = p - c
    d5, d6 = ab @ cp, ac @ cp
    if d6 >= 0 and d5 <= d6:
        return c, (0.0, 0.0, 1.0)
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6)
        return a + w * ac, (1 - w, 0.0, w)
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return b + w * (c - b), (0.0, 1 - w, w)
    den = 1.0 / (va + vb + vc)
    v, w = vb * den, vc * den
    return a + ab * v + ac * w, (1 - v - w, v, w)


def rotation_between(u, v):
    """The smallest rotation taking direction u to direction v (Damped Track)."""
    u, v = u / np.linalg.norm(u), v / np.linalg.norm(v)
    axis = np.cross(u, v)
    s = np.linalg.norm(axis)
    if s < 1e-12:
        return np.eye(3)
    return axis_angle(axis / s, np.arctan2(s, float(u @ v)))


def axis_angle(k, angle):
    k = np.asarray(k, dtype=float)
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + np.sin(angle) * K + (1 - np.cos(angle)) * (K @ K)


# ---------------------------------------------------------------------------
# ik_chains.xml (LLSD)
# ---------------------------------------------------------------------------

def llsd(node):
    if node.tag == "map":
        kids = list(node)
        return {kids[i].text: llsd(kids[i + 1]) for i in range(0, len(kids), 2)}
    if node.tag == "array":
        return [llsd(k) for k in node]
    if node.tag in ("real", "integer"):
        return float(node.text or 0)
    return node.text or ""


def pupil_entries():
    root = ET.parse(os.path.join(ROOT, "indra/newview/app_settings/posestudio/ik_chains.xml")).getroot()
    for chain in llsd(root[0]):
        for eye_set in chain.get("eye_sets", []):
            if eye_set.get("type") == "pupil" and eye_set.get("name") == "nose":
                return eye_set["pupil"]
    raise SystemExit("ik_chains.xml: no pupil eye set \"nose\"")


def driver_params(e):
    axes = e.get("shell_axes")
    if not axes:
        r = e.get("shell_radius", 0.0)
        axes = [[r, 0, 0], [0, r, 0], [0, 0, r]]
    vals = list(e["pivot"]) + [e["arm"]] + list(e.get("aim_offset", [0, 0, 0])) + list(e.get("aim_center", [0, 0, 0]))
    vals += list(e["shell_center"]) + [x for a in axes for x in a] + list(e.get("axis", [0, 0, 0]))
    return vals


# ---------------------------------------------------------------------------
# The two rigs
# ---------------------------------------------------------------------------

def look_dir(yaw, pitch):
    """Blender: the pony faces -Y, its left (+yaw) is +X, up (+pitch) is +Z."""
    y, p = np.radians(yaw), np.radians(pitch)
    return np.array([np.sin(y) * np.cos(p), -np.cos(y) * np.cos(p), np.sin(p)])


def kit_pose(s, shell, focus):
    back = np.array([0.0, 1.0, 0.0])            # IKEyefocus's local -Z at rest: toward the centre
    to_c = EYE_CENTER - focus
    r1 = rotation_between(back, np.array([to_c[0], to_c[1], 0.0]))      # Locked Track
    r2 = rotation_between(r1 @ back, to_c)                              # Damped Track
    target = focus + (r2 @ r1) @ np.array([s["off"], 0.0, 0.0])
    arm = np.linalg.norm(s["arm_tail"] - s["pivot"])
    tip = s["pivot"] + arm * (target - s["pivot"]) / np.linalg.norm(target - s["pivot"])
    q, n, _ = shell.closest(tip)
    return q, rotation_between(s["tail"] - s["head"], n)


def ll_euler(q):
    """LLQuaternion::getEulerAngles (x, y, z, w) -> (yaw, pitch, roll) degrees, as #EyeCapture logs."""
    x, y, z, w = q
    sx = 2 * (x * w - y * z)
    sy = 2 * (y * w + x * z)
    ys = w * w - y * y
    xz = x * x - z * z
    cx = ys - xz
    cy = np.hypot(sx, cx)
    return (np.degrees(np.arctan2(2 * (z * w - x * y), ys + xz)), np.degrees(np.arctan2(sy, cy)),
            np.degrees(np.arctan2(sx, cx)))


def check_capture(path, entries, run):
    import re
    bone = re.compile(r"(mFaceNose(?:Left|Right))=([-\d.eE+]+),([-\d.eE+]+),([-\d.eE+]+)/([-\d.eE+]+),([-\d.eE+]+),([-\d.eE+]+)")
    stats = {}
    shown = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "#EyeCapture s " not in line:
                continue
            t = re.search(r"t=([-\d.]+)", line).group(1)
            for m in bone.finditer(line):
                joint = m.group(1)
                vals = [float(x) for x in m.groups()[1:]]
                d, logged = np.array(vals[:3]), np.array(vals[3:])
                if np.linalg.norm(d) < 0.0005:
                    continue
                e = entries[joint]
                p = driver_params(e)
                out = run("1 " + " ".join("%.12g" % x for x in p[10:22] + list(d)))
                point, normal = np.array(out[:3]), np.array(out[3:6])
                depth = float(np.dot(d - point, normal)) * 1000
                a = np.array(e.get("axis", [0, 0, 0]), dtype=float)
                a /= np.linalg.norm(a)
                k = np.cross(a, normal)
                ang = np.arctan2(np.linalg.norm(k), np.dot(a, normal))
                k = k / max(np.linalg.norm(k), 1e-12)
                q = (k[0] * np.sin(ang / 2), k[1] * np.sin(ang / 2), k[2] * np.sin(ang / 2), np.cos(ang / 2))
                expected = np.array(ll_euler(q))
                diff = float(np.max(np.abs(expected - logged)))
                st = stats.setdefault(joint, [0, 0.0, 0.0])
                st[0] += 1
                st[1] = max(st[1], abs(depth))
                st[2] = max(st[2], diff)
                if shown < 40 and (abs(depth) > 0.3 or diff > 0.5):
                    shown += 1
                    print("t=%s %s offset %s mm  off shell %+.2f mm  rotation yaw/pitch/roll logged %s expected %s" % (
                        t, joint, np.round(d * 1000, 1), depth, np.round(logged, 2), np.round(expected, 2)))
    for joint, (n, depth, diff) in sorted(stats.items()):
        print("%s: %d samples off rest; worst %.2f mm off the shell, rotation up to %.2f deg from the flat turn" % (joint, n, depth, diff))
    if not stats:
        print("no mFaceNose samples off rest in %s" % path)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("blend", nargs="?")
    ap.add_argument("--capture", help="check an #EyeCapture log instead (no .blend)")
    ap.add_argument("--limit", type=float, default=45.0, help="the eye limit, degrees (Pose Studio 30; the look-at's eye range up to 45)")
    ap.add_argument("--wide", action="store_true", help="45 degree and diagonal looks")
    ap.add_argument("--tolerance", type=float, default=0.35, help="mm")
    args = ap.parse_args()

    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise SystemExit("no C++ compiler")
    tmp = tempfile.mkdtemp()
    src, exe = os.path.join(tmp, "pupil_driver.cpp"), os.path.join(tmp, "pupil_driver")
    with open(src, "w") as f:
        f.write(DRIVER)
    subprocess.run([cxx, "-std=c++17", "-O1", "-I" + os.path.join(ROOT, "indra/newview"), src, "-o", exe], check=True)
    driver = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    entries = pupil_entries()

    def run(line):
        driver.stdin.write(line + "\n")
        driver.stdin.flush()
        return [float(x) for x in driver.stdout.readline().split()]

    if args.capture:
        return check_capture(args.capture, entries, run)
    if not args.blend:
        raise SystemExit("a .blend (or --capture LOG) is needed")

    b = Blend(args.blend)
    shell = Surface(*read_mesh(b, "OBIKEye")[:2])
    white = Surface(*read_mesh(b, "OBEye")[:2])
    pv, pt, pg = read_mesh(b, "OBPupil")
    rng = np.random.default_rng(1)
    for s in SIDES.values():
        for k in ("head", "tail", "pivot", "arm_tail"):
            s[k] = np.array(s[k])
        s["inside"] = s["head"] - 0.15 * np.array([np.sign(s["off"]), -0.5, 0.0])
        disc = np.array([p for p, g in zip(pv, pg) if g == s["joint"] and np.linalg.norm(p - s["head"]) < VISIBLE_DISC])
        s["disc"] = disc[rng.choice(len(disc), size=min(120, len(disc)), replace=False)]
        s["gap0"] = np.array([white.signed(p, s["inside"]) for p in s["disc"]])
        s["params"] = driver_params(entries[s["joint"]])

    def viewer_pose(s, focus):
        to_target = B2S @ (focus - s["head"])
        v = run("0 " + " ".join("%.12g" % x for x in s["params"] + list(to_target) + [np.radians(args.limit)]))
        return s["head"] + B2S.T @ np.array(v[:3]), B2S.T @ axis_angle(v[3:6], v[6]) @ B2S

    looks = [(0, 0)] + [(a, 0) for a in (-30, -20, -10, 10, 20, 30)] + [(0, a) for a in (-30, -20, -10, 10, 20, 30)]
    if args.wide:
        looks = [(a, 0) for a in (-45, 45)] + [(0, a) for a in (-45, 45)] + [(a, c) for a in (-30, 30) for c in (-30, 30)]
    print("per eye: centre to shell mm | disc gap change over the eye white mm (min..max) | from kit mm | turn vs kit deg")
    worst, worst_turn = 0.0, 0.0
    for label, fn in (("kit", None), ("viewer", viewer_pose)):
        print("== %s" % label)
        for yaw, pitch in looks:
            focus = EYE_CENTER + FOCUS_DISTANCE * look_dir(yaw, pitch)
            line = "yaw %+3d pitch %+3d" % (yaw, pitch)
            for name, s in SIDES.items():
                pk, rk = kit_pose(s, shell, focus)
                p, r = (pk, rk) if fn is None else fn(s, focus)
                centre = shell.signed(p, s["inside"]) * 1000
                moved = p + (s["disc"] - s["head"]) @ r.T
                gap = (np.array([white.signed(q, s["inside"]) for q in moved]) - s["gap0"]) * 1000
                turn = np.degrees(np.arccos(np.clip((np.trace(r.T @ rk) - 1) / 2, -1, 1)))
                line += " | %s %+6.2f %+6.2f..%+6.2f %5.2f %4.1f" % (name, centre, gap.min(), gap.max(),
                                                                   np.linalg.norm(p - pk) * 1000, turn)
                if fn is not None:
                    worst = max(worst, abs(centre), abs(gap.min()), abs(gap.max()))
                    worst_turn = max(worst_turn, turn)
            print(line)
    print("viewer: worst %.2f mm (tolerance %.2f mm), turn up to %.2f deg from the kit" % (worst, args.tolerance, worst_turn))
    shutil.rmtree(tmp, ignore_errors=True)
    return 0 if worst <= args.tolerance else 1


if __name__ == "__main__":
    sys.exit(main())
