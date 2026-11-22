/**
 * @file llvrbodyshare.h
 * @brief #1584: other Slipstream users see your VR body. The bones the VR body layer drives on
 *        your own avatar (hips, spine, legs, collars, arms, neck, head; the pelvis' position) go
 *        out through Pose Studio's pose sharing (SLPoseSync, slposesync.h): the same P1 lines,
 *        bone indices, smallest-three rotations and 'E' ease record -- no new wire format.
 *
 * What is sent: each driven bone's local rotation as drawn (the layer blended over the animation),
 * the pelvis' position as Pose Studio's offset from the joint's default. A bone the layer stops
 * driving goes out released (the receiver plays the animation on it). One stream per avatar
 * (target "-"): receivers key by sender, so while the VR body shares, your own Pose Studio pose
 * rides the same stream -- the VR body wins on the bones it drives, the studio keeps the others.
 *
 * When (llvrbodysharemath.h): per bone a deadband with hysteresis by bone kind (start / keep: limbs
 * 4 / 2 deg, spine and hips 5 / 3 deg and 3 / 1.5 cm, neck / head 6 / 4 deg, fingers 8 / 5 deg; a
 * settle line with the final value when it stops); real movement at most one line per 0.2 s
 * (SLPoseSync's 0.25 s tick: 4 lines/s), gentle drift (every bone under 2x its keep threshold) at
 * most one per second; a byte budget (2400 B/s, burst 4800), a full
 * keyframe every 15 s, on a newcomer's request (Q1) and when the stream starts; nearly still,
 * nothing between keyframes. Each line carries an ease of about the line interval, so receivers
 * blend smoothly from line to line. Stop (X) when the layer stops (VR off, setting off, sitting,
 * every source lost) unless a studio pose remains (then a full releases the VR bones).
 *
 * Setting: SlipstreamVRBodyShare (persisted, on); also needs Pose Studio sharing (SLPoseSyncShare)
 * and the helper. Log "VRBody": share on / off, every 60 s while on: lines/s, bytes/s, bones per
 * line, keyframes, and which kinds of bone triggered the deltas; debug: each line's triggering
 * bones and how far past their threshold.
 * Cost: the desktop never reports (no VR layer); SLPoseSync's tick asks active(): a static read.
 */
#pragma once

#include "llquaternion.h"
#include "stdtypes.h"
#include "v3math.h"

class LLJointState;

class LLVRBodyShare
{
public:
    // ---- The body layer (LLVRBodyMotion::onUpdate, after it set its joint states) ----
    // The share is on: the layer reports its states (one cached-setting read).
    static bool wanted();
    static void beginLayer();
    static void addState(const LLJointState* state);   // (weight 0: not driven)
    static void endLayer();
    static void layerStopped();   // onDeactivate

    // ---- SLPoseSync (its tick) ----
    // The VR body stream is live: wanted, and the layer reported bones within the last second.
    static bool active();
    // This tick's line: 0 none, 1 delta, 2 full keyframe (fullAsked: a request / stream start /
    // SLPoseSync's own full schedule). Commits the values heldBone() then gives.
    static int plan(bool fullAsked);
    // The receivers' ease for the line plan() just chose (seconds).
    static F32 ease() { return sEase; }
    // A driven bone's value to send (Pose Studio's PoserJoints index): its local rotation and
    // (the pelvis) position offset. False: not driven (released, or the studio's).
    static bool heldBone(U32 poserIndex, LLQuaternion& rot, LLVector3& pos);
    // A line went out (stats).
    static void noteSent(size_t bytes, bool full);
    // The stream ended since the last call (SLPoseSync: release the VR bones / stop).
    static bool takeEnded();
    // Send a full keyframe at the next tick (fly cam freeze, #1584).
    static void requestFull();

private:
    static F32 sEase;
};
