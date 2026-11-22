#!/usr/bin/env python3
"""Pose Studio dials turn from the bone's current pose, and the ring finger moves on its own (#1459).

Source checks for the wiring that tests/slposedialmath_test.cpp cannot reach:
  - bone_axes.xml no longer gives the ring finger's first bone the foot's dial model with the
    other two ring bones as followers (they were overwritten with its rotation on every turn);
  - a dial turned on a bone the animation still poses starts from that pose: the other two
    axes are decoded from the bone's current rotation (SLPoseStudio::getShownBoneAngles), not
    set to zero, and the dials show those values;
  - the dial drags relative to the press (SLPoseDial::dragValue), Shift is fine, a click never
    jumps the value, nothing pins below the centre line, and only the small centre hub resets.

Usage:
  python3 <source>/indra/newview/tests/pose_dials_1459_checks.py --source-root <source>
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys


def check(cond: bool, what: str) -> None:
    if not cond:
        raise AssertionError(what)


def body(src: str, signature: str) -> str:
    """The text of the function whose definition starts with signature (brace matched)."""
    start = src.index(signature)
    open_at = src.index("{", start)
    depth = 0
    for i in range(open_at, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_at:i + 1]
    raise AssertionError(f"unbalanced braces after {signature}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    args = ap.parse_args()
    nv = pathlib.Path(args.source_root) / "indra/newview"

    # 1. The ring finger uses the default model like the other fingers, without followers.
    axes = (nv / "app_settings/posestudio/bone_axes.xml").read_text()
    check("<key>mHandRing1Left</key>" not in axes and "<key>mHandRing1Right</key>" not in axes,
          "bone_axes.xml still has a mHandRing1 entry (foot model, Ring2/Ring3 followers)")
    check(not re.search(r"<string>mHand\w+</string>", axes), "bone_axes.xml still makes a finger bone a follower")

    # 2. Dials start from the bone's current pose.
    studio_h = (nv / "slposestudio.h").read_text()
    studio = (nv / "slposestudio.cpp").read_text()
    check("bool getShownBoneAngles(LLVOAvatar* avatar, const joint_t& joint, SLPoseAngles& out)" in studio_h,
          "SLPoseStudio::getShownBoneAngles is not declared")
    shown = body(studio, "bool SLPoseStudio::getShownBoneAngles(")
    check("isBoneEdited(" in shown and "getBoneAngles(" in shown, "getShownBoneAngles does not keep edited bones on getBoneAngles")
    check("getRotation()" in shown and "fromQuat(" in shown,
          "getShownBoneAngles does not decode an unedited bone from the rotation it shows")

    floater = (nv / "slfloaterposestudio.cpp").read_text()
    apply_axis = body(floater, "void SLFloaterPoseStudio::applyAxis(")
    seed = body(floater, "void SLFloaterPoseStudio::seedAngles(")
    check("seedAngles(" in apply_axis and "getShownBoneAngles(" in seed,
          "applyAxis does not seed the other axes from the bone's current rotation")
    check("isBoneEdited(" in seed, "applyAxis uses remembered values for a bone that is not edited")
    check("!studio.isBoneEdited(avatar->getID(), joint->jointName()) || !studio.getBoneAngles" not in floater,
          "the floater still shows / applies dial zero for an unedited bone")
    check(floater.count("getShownBoneAngles(") >= 2, "the dials do not show the current pose of an unedited bone")

    # 3. The dial: relative drag, fine with Shift, small hub reset, no pinning, no click jump.
    dial = (nv / "slposedialctrl.cpp").read_text()
    down = body(dial, "bool SLPoseDialCtrl::handleMouseDown(")
    check("setFromUser" not in down and "valueFromPoint" not in down, "a click on the dial still sets the value")
    hover = body(dial, "bool SLPoseDialCtrl::handleHover(")
    check("SLPoseDial::dragValue(" in hover, "the dial drag is not relative (SLPoseDial::dragValue)")
    check("MASK_SHIFT" in hover, "Shift does not slow the dial drag")
    check("F_PI_BY_TWO) ? F_PI : 0.f" not in dial, "the dial still pins below the centre line")
    check("mPressedOnKnob" not in dial, "a click on the knob still resets the dial")
    up = body(dial, "bool SLPoseDialCtrl::handleMouseUp(")
    check("mChanged" in up, "releasing a click that changed nothing still commits")
    check("HUB_RADIUS" in dial and "* 0.3f" not in dial, "the hub reset zone is still 30% of the ring")

    xui = (nv / "skins/default/xui/en/floater_sl_pose_studio.xml").read_text()
    tips = re.findall(r'name="dial_[xyz]"\s+ring_color="[^"]*"\s+tool_tip="([^"]*)"', xui)
    check(len(tips) == 3, "dial tool tips not found")
    for tip in tips:
        check("knob" not in tip and "Shift" in tip and "centre" in tip, f"dial tool tip not updated: {tip}")

    # Review fixes. (1) A drag starts from one fresh decode of the live frame: the dial asks
    # the floater on press, the floater re-reads all three axes into the dials and spinners
    # and applies its drag on top of that same seed, and shows what it applied.
    dial_h = (nv / "slposedialctrl.h").read_text()
    check("setPressCallback(" in dial_h, "the dial has no press signal")
    press_at = down.find("mPressSignal(this)")
    check(press_at >= 0 and press_at < down.find("mDragValue = mValue"),
          "the dial does not let the floater refresh it before the drag takes its start value")
    check("setPressCallback(" in floater, "the floater does not refresh a dial's bone on press")
    pressed = body(floater, "void SLFloaterPoseStudio::onDialPressed(")
    check("seedAngles(" in pressed and "mDials[i]->setValue(" in pressed and "mDialSpinners[i]->setValue(" in pressed,
          "onDialPressed does not put one fresh decode on every dial and spinner")
    check("mPressSeed" in apply_axis and "seedAngles(" in apply_axis, "applyAxis does not start from the press seed")
    check("mDialSpinners[i]->setValue(angles.get(i))" in apply_axis, "applyAxis does not show the values it applied")
    # (2) Out-of-range values are kept: no clamp in setValue, drag limits from the start value,
    # the pointer drawn at the end stop.
    check("llclamp" not in body(dial, "void SLPoseDialCtrl::setValue("), "the dial still clamps a set value")
    check("SLPoseDial::dragLimits(" in down, "the drag does not take its limits from the start value")
    check("mDragLo, mDragHi" in hover, "the drag is not clamped to its own limits")
    check("SLPoseDial::shownValue(" in body(dial, "void SLPoseDialCtrl::draw("), "the pointer is not drawn at the end stop")
    # (3) A reset that changes nothing commits nothing.
    reset = body(dial, "void SLPoseDialCtrl::resetToZero(")
    check("mReleaseSignal" not in reset, "a reset still commits when the value was already zero")
    # (4) Losing the mouse mid-drag ends the drag and commits it.
    check("void onMouseCaptureLost() override;" in dial_h, "the dial does not handle losing the mouse capture")
    lost = body(dial, "void SLPoseDialCtrl::onMouseCaptureLost(")
    check("mDragging = false" in lost and "mChanged" in lost and "mReleaseSignal" in lost,
          "losing the capture does not end the drag and commit it")
    # (5) The hub's hit area stays above the value label.
    check("getLineHeight()" in body(dial, "bool SLPoseDialCtrl::onHub("), "the hub's hit area can overlap the value label")

    print("pose_dials_1459_checks: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as err:
        print(f"FAIL: {err}")
        sys.exit(1)
