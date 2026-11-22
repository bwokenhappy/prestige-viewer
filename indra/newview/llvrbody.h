/**
 * @file llvrbody.h
 * @brief VR full-body tracking (#1565, epic #1072): body trackers drive your own avatar's hips,
 *        spine and legs. LOCAL ONLY: Second Life has no live-pose channel, so other people keep
 *        seeing your normal animations (follow-up: an opt-in channel like position sync #1417).
 *
 * Input: one body source per frame (llvrbodysource.h; SlipstreamVRBodySource: Auto = OpenXR
 * trackers, else SlimeVR, else headset body tracking): LLXrSession's tracker roles
 * (XR_HTCX_vive_tracker_interaction; SteamVR Vive / Tundra trackers, SlimeVR through its SteamVR
 * driver), SlimeVR server over OSC on any runtime (#1582, llvrbodyslime.h), or the headset's own
 * body tracking (#1583, LLVRBodyHeadset: poses that are already the bones, so no T-pose; the Body
 * button then only measures your eye height). Bones: hips (waist), chest, feet (or ankles),
 * knees (pole hints). The same layer drives the arms from the controllers and the neck / head
 * from the headset (#1584, llvrbodyarms.h; also with no body source); #1121's look-at turn
 * (SLPoseLookAt) turns the head from the chest while this layer turns it (lookFrame) and steps
 * back by the weight the neck layer applied. Fly cam (#1584): the body stays live and the head is
 * released (D), or the pose is frozen (C, SlipstreamVRFlycamFreezePose). The driven bones are
 * shared with other Slipstream viewers through Pose Studio sharing (llvrbodyshare.h).
 *
 * Calibration (wrist panel "Body", shown only while trackers are listed; a 3 s countdown, then
 * stand straight in a T-pose facing forward): each tracker's offset to its bone, your eye height
 * (the avatar is scaled to you by its eye height over yours). Roles from the runtime; by height
 * and side when the runtime's roles give neither feet nor hips (or SlipstreamVRBodyAssignByPosition).
 * Kept in SlipstreamVRBodyCalibration for the next session.
 *
 * Layer: a NORMAL_BLEND motion at ADDITIVE_PRIORITY on our own avatar only (over every
 * animation), local rotations solved top-down from the avatar's root: hips from the waist
 * tracker (or under the head when only feet are worn), spine bent / twisted toward the head
 * (or the chest tracker), two-bone leg IK onto the feet with the knees toward the knee trackers
 * (else forward), the feet turned as their trackers. Per-role weights
 * (SlipstreamVRBodyWeightHips / Legs / Spine), smoothing (SlipstreamVRBodySmoothing), and a lost
 * tracker fades its limb back to the animation over 0.3 s. Sitting, fly cam or no calibration:
 * everything fades out.
 *
 * Cost: desktop never calls in. VR without a body source: no tracker work; the arms / neck
 * (#1584) still run (a skeleton measure, the mapping and the layer while they have weight).
 *
 * Log tag "VRBody": trackers found (count, roles), calibration (roles, offsets), body layer
 * on / off, a tracker lost / back (at most one line per bone per 5 s).
 */
#pragma once

#include "llquaternion.h"
#include "llvrbodyvmc.h"
#include "v3math.h"

class LLVOAvatarSelf;

class LLVRBody
{
public:
    // LLVRInput::update, every VR frame with the session running (dt seconds).
    static void update(F32 dt);
    // The session stopped / the headset is off: fade out (next frames) and forget the countdown.
    static void reset();
    // Wrist "Body": calibrate after a short countdown (stand in a T-pose).
    static void requestCalibration();
    // A body source has a body this frame: trackers listed, or headset body tracking active (the
    // wrist shows the Body button).
    static bool available() { return sAvailable; }
    // The calibration countdown is running.
    static bool calibrating() { return sCalibrating; }
    // LLVRCamera::apply: the layer drives the hips / spine -- hold the eye anchor (else your own
    // crouch / lean would move the view twice).
    static bool holdsAnchor() { return sHoldAnchor; }
    // #1121 head follow (SLPoseLookAt, our own VR head): while this layer turns the chest, the
    // head turns from the chest. frame: the root's world rotation in, the chest's (blended by
    // the spine's weight) out. False: unchanged (one static read).
    static bool lookFrame(LLQuaternion& frame);
    // #1584 fly cam: C (SlipstreamVRFlycamFreezePose) holds the layer's last pose (poseHeld); on
    // leaving, liveBlend eases 0 -> 1 from the held pose back to live (the motion blends).
    static bool poseHeld() { return sPoseHeld; }
    static F32 liveBlend() { return sLiveBlend; }
    // #1592 the body mapping's upright profile (the Open Pony on its hind legs) is in effect: the
    // layer stands the body up (not sitting, first person or fly cam).
    static bool upright() { return sUpright; }
    // The standing eye height above the soles of the body this layer drives (LLVRCamera's held
    // anchor): while it holds the anchor or stands the body up, else false. The body maps your
    // head at the camera's reference height onto exactly this height, so the camera holding the
    // anchor there keeps the avatar's eyes at the camera.
    static bool standingEyeHeight(F32& h);

    // Agent-space targets for this frame (the motion reads them). Bones as slfbt::Body.
    enum { BONES = 6 };
    struct Targets
    {
        bool on = false;              // anything to drive
        LLVector3 eye;                // the headset's eye point
        F32 headYaw = 0.f;            // the head's heading
        bool have[BONES] = {};
        LLVector3 pos[BONES];
        LLQuaternion rot[BONES];
        F32 weight[BONES] = {};       // fade x the role's setting
        F32 waistBlend = 0.f;         // the hips from the waist tracker (1) .. estimated under the head (0)
        F32 hipsWeight = 0.f;         // the hips are driven (waist tracker, else estimated under the head)
        F32 spineWeight = 0.f;
        // #1582 a whole skeleton (SlimeVR VMC): the SL joints' agent-space world rotations in
        // slvmc::SJoint order (fullHave: sent this frame), driven directly instead of the roles.
        // Arms: fullArm[side] false while that hand's controller is tracked -- the controller (and
        // #1584's arm IK) owns the arm; fullRot still holds SlimeVR's arm (a pose to start from).
        // Neck / head are never driven here (the headset: #1121).
        bool full = false;
        F32 fullWeight = 0.f;         // fade x 1
        bool fullHave[slvmc::S_COUNT] = {};
        LLQuaternion fullRot[slvmc::S_COUNT];
        bool fullArm[2] = {};         // 0 left, 1 right
        F32 floorZ = 0.f;             // the soles' agent-space height (the body never sinks below it)
    };
    static const Targets& targets() { return sTargets; }

    // The motion's own report (its last update): the chest's world rotation and the spine's
    // weight (lookFrame), and that it drove the hips / spine (holdsAnchor).
    static void noteSolved(const LLQuaternion& chest, F32 spineWeight, bool hipsDriven);

private:
    static bool sAvailable;
    static bool sCalibrating;
    static void holdPose(LLVOAvatarSelf* av);
    static bool sHoldAnchor;
    static bool sPoseHeld;
    static bool sUpright;
    static F32 sLiveBlend;
    static Targets sTargets;
};
