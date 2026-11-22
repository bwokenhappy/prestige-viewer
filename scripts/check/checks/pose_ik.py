"""Regression guard for Pose Studio's IK geometry (#530, #1566).

  * compiles scripts/check/test_pose_ik.cpp against indra/newview/slposeikmath.h with the host
    C++ compiler and runs it: two-bone limbs keep their lengths, reach or stretch straight and
    never flip the knee / elbow while the goal moves (the bend plane captured at mouse down is
    held); a placed pole wins; FABRIK chains (tail, the neck + head + nose reach) keep their
    links and reach; the gaze turn goes to the eyes first, then the head, then the neck; an eye
    aims from its own rest axis within its cone; "Auto" eye bones pick by the worn rigging;
    pupil eyes (#1567, the Open Pony) slide and turn like the dev kit's eye rig;
  * reads the viewer source for what the fixture cannot see: the limb solver uses that math
    (bendSide / twoBone) with the old one only behind SLPoseStudioIKLegacyLimb, and the settings
    the floater binds exist.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "pose-ik"
STAGE = "source"
DESCRIPTION = "Pose Studio IK geometry: stable limb bend, FABRIK chains, gaze shares (#1566)"
GUARDS = ("Knees / elbows flipping or twisting while an IK handle is dragged; a chain that stretches "
          "a bone or misses a reachable goal; the gaze turning the neck before the eyes; the limb "
          "solver drifting off the tested math.")

SETTINGS = ("SLPoseStudioIKLegacyLimb", "SLPoseStudioIKFollowRoot", "SLPoseStudioFKBones", "SLPoseStudioBoneAlone",
            "SLPoseStudioDialFrame", "SLPoseStudioIKEyeBones", "SLPoseStudioFKAllBento", "SLLookAtEyeRange",
            "SLLookAtMoveEyes", "SLEyeCaptureLog")


def read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def run_fixture(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: pose IK fixtures not run")
        return False
    src = ctx.path("scripts/check/test_pose_ik.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_pose_ik")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return False
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return False
    return True


def check_viewer(ctx, result):
    src = read(ctx, "indra/newview/slposeik.cpp")
    if '#include "slposeikmath.h"' not in src + read(ctx, "indra/newview/slposeik.h"):
        result.fail("slposeik.{h,cpp}: must use slposeikmath.h (the tested geometry)")
    m = re.search(r"void SLPoseIK::solveTwoBone\(.*?\n\}\n", src, re.S)
    if not m:
        result.fail("slposeik.cpp: no SLPoseIK::solveTwoBone")
    else:
        body = m.group(0)
        if "SLPoseIKMath::bendSide(" not in body or "SLPoseIKMath::twoBone(" not in body:
            result.fail("slposeik.cpp: solveTwoBone must place the knee with SLPoseIKMath::bendSide / twoBone")
        if "legacyLimb()" not in body:
            result.fail("slposeik.cpp: the old limb solve must stay reachable behind SLPoseStudioIKLegacyLimb")
    m = re.search(r"void SLPoseIK::buildBones\(.*?\n\}\n", src, re.S)
    if not m or "riggedJoints(" not in m.group(0) or "isRiggedTo()" in m.group(0):
        result.fail("slposeik.cpp: buildBones must ask SLPoseStudio::riggedJoints (skin joint lists count; "
                    "CPU vertex weights alone miss GPU-skinned meshes)")
    if "SLPoseStudio::instance().riggedJoints(" not in read(ctx, "indra/newview/slposelookat.cpp"):
        result.fail("slposelookat.cpp: the look-at Auto must use SLPoseStudio::riggedJoints like Pose Studio")
    lookat = read(ctx, "indra/newview/slposelookat.cpp")
    for needle, why in (("SLPoseIKMath::headFollow(", "the live look-at must lead with the eyes (headFollow)"),
                        ("SLPoseIKMath::eyeTurn(", "the look-at's eye bones must aim at the target point (eyeTurn)"),
                        ("SLPoseIKMath::gazeDistance(", "the look-at's eyes must converge at the target's distance"),
                        ("SLPoseIKMath::pupilPose(", "pupil eyes (Open Pony) must slide by the tested pupil model"),
                        ("SLPosePupilMotion::onUpdate(", "pupil offsets need their own position motion (an additive one accumulates)"),
                        ("getPupilRotation(", "the pupil motion must turn the pupils with their slide (else they cut into the eye)"),
                        ("drive.mMoveEyes", "\"Move eyes\" off must keep the look-at off the eye bones")):
        if needle not in lookat:
            result.fail("slposelookat.cpp: " + why)
    m = re.search(r"void SLPoseIK::aimEyes\(.*?\n\}\n", src, re.S)
    if not m or "SLPoseIKMath::pupilPose(" not in m.group(0) or "pose.mTurn" not in m.group(0):
        result.fail("slposeik.cpp: Pose Studio's eye IK must slide and turn pupil sets with SLPoseIKMath::pupilPose")
    if not m or "pupilRest(" not in m.group(0) or "getJointTargetTransform(" not in m.group(0):
        result.fail("slposeik.cpp: Pose Studio's pupils must be measured from the true rest (pupilRest) and held at "
                    "absolute targets (the poser's base can carry the live look-at's offset)")
    if "mPositions" not in read(ctx, "indra/newview/slposecreator.cpp"):
        result.fail("slposecreator.cpp: .anim export must write position keys for bones Pose Studio moved (pupils)")
    floater = read(ctx, "indra/newview/slfloaterposestudio.cpp")
    for fn in ("onResetAll", "onResetBone"):
        m = re.search(r"void SLFloaterPoseStudio::%s\(\)\n\{.*?\n\}\n" % fn, floater, re.S)
        if not m or "SLPoseIK::instance().onBonesReset(" not in m.group(0):
            result.fail("slfloaterposestudio.cpp: %s must reset the IK handles too (SLPoseIK::onBonesReset)" % fn)
    m = re.search(r"void SLPoseIK::onBonesReset\(.*?\n\}\n", src, re.S)
    if not m or "reset();" not in m.group(0):
        result.fail("slposeik.cpp: Reset All must drop every IK target, pin and cached solver state (reset())")
    settings = read(ctx, "indra/newview/app_settings/settings.xml")
    for key in SETTINGS:
        if "<key>%s</key>" % key not in settings:
            result.fail("settings.xml: no %s" % key)


def run(ctx, result):
    for rel in ("indra/newview/slposeikmath.h", "scripts/check/test_pose_ik.cpp", "indra/newview/slposeik.cpp"):
        if not os.path.exists(ctx.path(rel)):
            result.fail("%s is missing (Pose Studio IK)" % rel)
            result.summary = "pose IK missing"
            return
    ran = run_fixture(ctx, result)
    check_viewer(ctx, result)
    result.summary = "fixtures + source rules" if ran else "source rules (no compiler)"
