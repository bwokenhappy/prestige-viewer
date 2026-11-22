"""Regression guard for #1518: an avatar seen already sitting rendered its sit broken (pelvis / hind legs).

LLMotionController keeps its active motions newest-START first, and at equal priority that order decides who
drives a joint (joint signature skip + LLJointStateBlender slot order; clean_anim.comp mirrors it). Since cached
animation reads moved to a worker (LLKeyframeMotion::readAssetAsync), a motion activates when its read lands, and
activateMotionInstance pushed it to the FRONT then: on load-in an avatar's animations ended up in read-completion
order, so a sit could lose its pelvis and hind legs to an equal-priority animation started before it -- until a
stand / sit restarted it. Now each start request is stamped and the motion is inserted where the stamp belongs
(indra/llcharacter/llmotionstartorder.h); a direct start is still the newest.

  * the fixture (scripts/check/test_motion_start_order.cpp, includes the header): for every start x read-completion
    order of 4 equal-priority motions (any of them synchronous) the active order equals LL's synchronous start
    order; the old push_front-at-completion rule departs from it (sanity: the bug);
  * source wiring: activateMotionInstance stamps a start requested while loading, inserts with
    llMotionStartOrderPos instead of push_front, and stopMotionInstance withdraws a loading motion's stamp.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "motion-start-order"
STAGE = "source"
DESCRIPTION = ("Motions activate in START order, not data-load order: an avatar's equal-priority animation ties "
               "resolve as in LL after an async load (#1518)")
GUARDS = ("An avatar seen already sitting on load-in renders its sit wrong (pelvis rotated, hind legs twisted) "
          "until it stands and sits again: an equal-priority animation whose read finished later took the tie.")

CONTROLLER = "indra/llcharacter/llmotioncontroller.cpp"
HEADER = "indra/llcharacter/llmotionstartorder.h"


def body(text, signature):
    i = text.find(signature)
    if i < 0:
        return ""
    j = text.find("\n}", i)
    return text[i:j + 2] if j >= 0 else ""


def _wiring(ctx):
    missing = []
    with open(ctx.path(CONTROLLER), encoding="utf-8", errors="replace") as f:
        src = f.read()
    act = body(src, "bool LLMotionController::activateMotionInstance(")
    if not act:
        return ["LLMotionController::activateMotionInstance() not found"]
    if "mActiveMotions.push_front(" in act:
        missing.append("activateMotionInstance pushes the motion to the front again (load-completion order)")
    if not re.search(r"llMotionStartOrderPos\(\s*mActiveMotions\s*,\s*seq\b", act) \
            or "mActiveMotions.insert(pos, motion)" not in act:
        missing.append("activateMotionInstance does not insert at llMotionStartOrderPos(mActiveMotions, seq, ...)")
    if not re.search(r"mPendingStartSeq\s*=\s*\+\+mStartSeqCounter", act):
        missing.append("a start requested while the motion loads is not stamped (mPendingStartSeq)")
    if not re.search(r"const U64 seq = motion->mPendingStartSeq \? motion->mPendingStartSeq : \+\+mStartSeqCounter;",
                     act):
        missing.append("activation does not take the pending start stamp (else a fresh one)")
    stop = body(src, "bool LLMotionController::stopMotionInstance(")
    if not re.search(r"isMotionLoading\(motion\)\)\s*\{[^}]*mPendingStartSeq\s*=\s*0", stop):
        missing.append("stopMotionInstance does not withdraw a loading motion's start stamp")
    return missing


def run(ctx, result):
    hdr = ctx.path(HEADER)
    if not os.path.isfile(hdr):
        result.fail(HEADER + " is missing: late-loaded motions activate in load-completion order again (#1518)")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the start-order fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_motion_start_order.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_motion_start_order")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "late-loaded motions take their start-order place (fixture) + controller wiring"
