#!/usr/bin/env python3
"""Summarise an #EyeCapture log (#1567): how the eye bones move under the animations that play.

Turn on SLEyeCaptureLog (Debug Settings; not saved), play the eye animations, turn it off, then:

  scripts/tools/eye_capture_summary.py ~/.slipstreamviewer/logs/Slipstream.log [--anim ID8] [--by-anim]
                                        [--moving-only] [--min-mm 0.2]

The viewer logs (SLPoseLookAt::tickEyeCapture), about 20 times a second:
  #EyeCapture rest  <bone>=x,y,z ...                        rest positions (joint override / default)
  #EyeCapture anims t=<s> <uuid>:<name>:p<priority> ...     the animations playing, when that changes
  #EyeCapture s t=<s> a=<id8>,... set=<n> <bone>=dx,dy,dz/yaw,pitch,roll ...
position offsets from rest in metres, rotation in degrees, the bone's parent frame (+X forward,
+Y left, +Z up). For each bone this prints the range (min / max / mean per axis), the dominant
translation axes (PCA: direction and spread), and for left / right pairs whether they move the
same way (a look) or mirrored (cross-eye), with the vergence offset when both move inward.
"""
import argparse
import math
import re
import sys
from collections import defaultdict

SAMPLE = re.compile(r"#EyeCapture s t=(?P<t>[-\d.]+) a=(?P<a>\S*) set=(?P<set>-?\d+)(?P<bones>.*)$")
BONE = re.compile(r"(\w+)=([-\d.eE+]+),([-\d.eE+]+),([-\d.eE+]+)/([-\d.eE+]+),([-\d.eE+]+),([-\d.eE+]+)")
ANIMS = re.compile(r"#EyeCapture anims t=(?P<t>[-\d.]+)(?P<rest>.*)$")
PAIRS = (("mFaceNoseLeft", "mFaceNoseRight"), ("mEyeLeft", "mEyeRight"), ("mFaceEyeAltLeft", "mFaceEyeAltRight"))


def read(path, anim_filter):
    samples = []
    names = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ANIMS.search(line)
            if m:
                for tok in m.group("rest").split():
                    parts = tok.split(":")
                    if len(parts) >= 2:
                        names[parts[0][:8]] = ":".join(parts[1:])
                continue
            m = SAMPLE.search(line)
            if not m:
                continue
            anims = [a for a in m.group("a").split(",") if a]
            if anim_filter and not any(a.startswith(anim_filter) for a in anims):
                continue
            bones = {}
            for b in BONE.finditer(m.group("bones")):
                vals = [float(x) for x in b.groups()[1:]]
                bones[b.group(1)] = (vals[:3], vals[3:])
            samples.append((float(m.group("t")), tuple(anims), int(m.group("set")), bones))
    return samples, names


def mean(xs):
    return sum(xs) / len(xs) if xs else 0.0


def eig_sym3(c):
    """Eigen decomposition of a symmetric 3x3 (Jacobi): [(value, vector)] largest first."""
    a = [row[:] for row in c]
    v = [[1.0 if i == j else 0.0 for j in range(3)] for i in range(3)]
    for _ in range(50):
        p, q = max(((i, j) for i in range(3) for j in range(i + 1, 3)), key=lambda ij: abs(a[ij[0]][ij[1]]))
        if abs(a[p][q]) < 1e-18:
            break
        theta = 0.5 * math.atan2(2 * a[p][q], a[q][q] - a[p][p])
        cs, sn = math.cos(theta), math.sin(theta)
        for k in range(3):
            akp, akq = a[k][p], a[k][q]
            a[k][p], a[k][q] = cs * akp - sn * akq, sn * akp + cs * akq
        for k in range(3):
            apk, aqk = a[p][k], a[q][k]
            a[p][k], a[q][k] = cs * apk - sn * aqk, sn * apk + cs * aqk
        for k in range(3):
            vkp, vkq = v[k][p], v[k][q]
            v[k][p], v[k][q] = cs * vkp - sn * vkq, sn * vkp + cs * vkq
    out = [(a[i][i], [v[0][i], v[1][i], v[2][i]]) for i in range(3)]
    return sorted(out, key=lambda e: -e[0])


def summarise(samples, min_mm, moving_only, title):
    print("== %s: %d samples, %.1f s" % (title, len(samples), (samples[-1][0] - samples[0][0]) if samples else 0.0))
    bones = sorted({b for s in samples for b in s[3]})
    for bone in bones:
        pos = [s[3][bone][0] for s in samples if bone in s[3]]
        rot = [s[3][bone][1] for s in samples if bone in s[3]]
        if moving_only and all(math.sqrt(sum(x * x for x in p)) * 1000 < min_mm for p in pos):
            continue
        print("  %s (%d)" % (bone, len(pos)))
        for label, vals, unit, scale in (("pos mm", pos, "x y z", 1000.0), ("rot deg", rot, "yaw pitch roll", 1.0)):
            cols = list(zip(*vals))
            print("    %-7s %-15s min %s  max %s  mean %s" % (
                label, unit,
                " ".join("%8.2f" % (min(c) * scale) for c in cols),
                " ".join("%8.2f" % (max(c) * scale) for c in cols),
                " ".join("%8.2f" % (mean(c) * scale) for c in cols)))
        m = [mean(c) for c in zip(*pos)]
        cov = [[mean([(p[i] - m[i]) * (p[j] - m[j]) for p in pos]) for j in range(3)] for i in range(3)]
        total = sum(cov[i][i] for i in range(3))
        if total * 1e6 < (min_mm * min_mm):
            print("    translation: none to speak of")
            continue
        for k, (val, vec) in enumerate(eig_sym3(cov)):
            print("    PCA %d: dir (%+.3f %+.3f %+.3f)  sd %.2f mm  (%.0f%% of the motion)" % (
                k + 1, vec[0], vec[1], vec[2], math.sqrt(max(val, 0.0)) * 1000, 100.0 * val / total if total > 0 else 0))
    for left, right in PAIRS:
        both = [(s[3][left][0], s[3][right][0]) for s in samples if left in s[3] and right in s[3]]
        if len(both) < 4:
            continue
        ml = [mean(c) for c in zip(*[b[0] for b in both])]
        mr = [mean(c) for c in zip(*[b[1] for b in both])]
        dl = [[b[0][i] - ml[i] for i in range(3)] for b in both]
        dr = [[b[1][i] - mr[i] for i in range(3)] for b in both]
        if all(math.sqrt(sum(x * x for x in d)) * 1000 < min_mm for d in dl + dr):
            continue
        print("  %s / %s" % (left, right))
        for axis, name in ((0, "x (fwd)"), (1, "y (left)"), (2, "z (up)")):
            a = [d[axis] for d in dl]
            b = [d[axis] for d in dr]
            sa = math.sqrt(mean([x * x for x in a]))
            sb = math.sqrt(mean([x * x for x in b]))
            if sa * 1000 < min_mm and sb * 1000 < min_mm:
                continue
            corr = mean([x * y for x, y in zip(a, b)]) / (sa * sb) if sa > 0 and sb > 0 else 0.0
            how = "same direction" if corr > 0.5 else ("mirrored" if corr < -0.5 else "unrelated")
            print("    %-8s corr %+.2f (%s), sd L %.2f mm R %.2f mm" % (name, corr, how, sa * 1000, sb * 1000))
        # Vergence: both inward (left -y, right +y from their mean) at once.
        inward = [(-l[1], r[1]) for l, r in zip(dl, dr) if l[1] < 0 and r[1] > 0]
        if inward:
            v = [min(a, b) for a, b in inward]
            print("    both inward in %d samples: common inward offset mean %.2f mm, max %.2f mm (vergence)" % (
                len(inward), mean(v) * 1000, max(v) * 1000))
        print("    rest offsets (mean) L %s mm  R %s mm" % (
            " ".join("%.2f" % (x * 1000) for x in ml), " ".join("%.2f" % (x * 1000) for x in mr)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--anim", default="", help="only samples while this animation (id prefix) plays")
    ap.add_argument("--by-anim", action="store_true", help="also summarise each set of playing animations")
    ap.add_argument("--moving-only", action="store_true", help="skip bones that never leave rest")
    ap.add_argument("--min-mm", type=float, default=0.2, help="motion below this counts as none (mm)")
    args = ap.parse_args()
    samples, names = read(args.log, args.anim)
    if not samples:
        print("no #EyeCapture samples (turn SLEyeCaptureLog on while the animations play)")
        return 1
    summarise(samples, args.min_mm, args.moving_only, "all")
    if args.by_anim:
        groups = defaultdict(list)
        for s in samples:
            groups[s[1]].append(s)
        for key, group in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            label = ", ".join("%s(%s)" % (a, names.get(a, "?")) for a in key) or "(none)"
            summarise(group, args.min_mm, True, label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
