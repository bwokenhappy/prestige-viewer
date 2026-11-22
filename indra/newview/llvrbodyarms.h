/**
 * @file llvrbodyarms.h
 * @brief VR arms and neck (#1584, part of #1565): your own avatar's arms follow the controllers and
 *        its neck follows the headset. LOCAL ONLY (others see it through the body share, #1584's
 *        llvrbodyshare.h, in Slipstream viewers with the helper).
 *
 * Input (LLVRBodyArms::update, every VR frame from LLVRBody::update, also with no body source):
 * the controllers' grip poses (LLXrSession::inputState), each hand's interaction profile (the grip
 * offset: Index, Touch, Vive wand, else generic), the headset, and from the body source's frame
 * the elbow roles (OpenXR left / right_elbow trackers, SlimeVR's elbow trackers, headset body
 * tracking's elbow joints) as elbow poles. SlimeVR's VMC skeleton (#1582, LLVRBody::Targets
 * full*) guides an arm whose hand is on a controller: its collar and upper arm give the collar's
 * turn and the elbow's direction, the hand stays pinned to the controller. An arm without a
 * tracked controller is VMC's (LLVRBodyMotion::solveFull): this layer stays out of it.
 *
 * Mapping: as the body's (slfbt::roomToAgent): from the recentre reference onto LLVRCamera's
 * anchor, turned by the rig, scaled by the avatar's eye height over yours -- so arm reach follows
 * the calibration's body scale (a short avatar reaches as far, relative to itself, as you do). The
 * LLVRHands stand-ins are drawn unscaled at the controllers: with a scale other than 1 the avatar's
 * wrists sit a little off them.
 *
 * Solve (LLVRBodyArmsLayer, inside the body layer LLVRBodyMotion: NORMAL_BLEND at
 * ADDITIVE_PRIORITY, our own avatar only), from the chest (the body layer's when it drives the
 * spine, else the drawn one): collar (a share of reach and raise), two-bone shoulder / elbow onto
 * the wrist target (never stretched), forearm roll shared between mElbow and mWrist (the SL
 * skeleton has no forearm twist joints), the wrist turned as the controller. Neck: the headset's
 * rotation relative to the chest split between mNeck and mHead, the neck leaning toward the
 * head's position. While the neck layer drives the head, SLPoseLookAt's VR head turn (#1121)
 * steps back by the same weight (headWeight), so the turn is never applied twice.
 *
 * #1592 a foreleg as an arm (the Open Pony, doc/open_pony_rig.md): the controller's grip is the
 * hoof joint's place and its rotation the hoof's (your hand at the wrist); the forearm and cannon
 * twist a forearm's share of the roll (slfbt::solveHoofArm). With the profile's reachRebase the grip is
 * taken from your shoulder (estimated under the eye) and scaled by the foreleg's reach over yours,
 * from the pony's own shoulder; a retargeted limb's collar stays with the chest.
 *
 * A lost controller fades its arm back to the animation over 0.3 s (and back in when it returns).
 * Settings (live, persisted): SlipstreamVRArmTracking (on), SlipstreamVRBodyWeightArms,
 * SlipstreamVRBodyWeightNeck (0..1; neck 0 = the #1121 look-at turns the head as before).
 *
 * Cost: the desktop never calls in; the layer's solve returns at once when nothing is on.
 * Log tag "VRBody": arms on / off, each hand's controller profile and grip offset, lost / back.
 */
#pragma once

#include "llpointer.h"
#include "llquaternion.h"
#include "llvrbodymapmath.h"
#include "v3math.h"

#include <vector>

class LLCharacter;
class LLJoint;
class LLJointState;
struct LLVRBodyFrame;
struct LLXrHeadPose;

// This frame's room -> agent mapping (LLVRBody makes it; slfbt::roomToAgent's parameters). In
// fly cam it is the last one from before (the avatar stays where it was left).
struct LLVRBodyMapping
{
    bool ok = false;
    LLVector3 anchor;
    F32 refX = 0.f, refY = 0.f, turn = 0.f;
    F32 sv = 1.f;          // the avatar's eye height over yours
    F32 footZ = 0.f;       // the avatar's soles (agent z)
    F32 floorY = 0.f;      // the floor's app-space y
    F32 headWeight = 1.f;  // 0..1: the headset turns the head (fly cam eases it to 0)
    bool fly = false;      // fly cam (the arms stay on the controllers; only the head is released)
    F32 avatarEyeH = 0.f;  // #1592 the avatar's eye height (its mapped skeleton; 0: unknown)
};

class LLVRBodyArms
{
public:
    // Every VR frame (LLVRBody::update, also with no body source). body: this frame's body-source
    // frame (its elbow roles), or nullptr; m: the body's mapping.
    static void update(F32 dt, const LLXrHeadPose& hp, bool headOk, const LLVRBodyFrame* body, const LLVRBodyMapping& m);
    // The session stopped: fade out (next frames) and forget the hands.
    static void reset();
    // Arms or neck have something to drive: the body layer must run.
    static bool on() { return sTargets.on; }
    // SLPoseLookAt: how much of our own VR head turn this layer applied (0..1) in its last solve.
    static F32 headWeight() { return sHeadWeight; }

    // Agent-space targets for this frame (the layer reads them). side 0 left, 1 right.
    struct Hand
    {
        bool driven = false;            // the controller drives the arm this frame (VMC leaves it alone)
        F32 weight = 0.f;               // fade x SlipstreamVRBodyWeightArms
        LLVector3 wrist;                // the wrist joint's target
        LLVector3 grip;                 // #1592 the point the hand holds (the controller's grip; a hoof goes here)
        LLQuaternion rot;               // the hand's world rotation
        bool handFree = false;          // a wrist tracker gives the position only: the hand as drawn
        F32 elbowWeight = 0.f;          // the elbow role's fade (0: the natural pole)
        LLVector3 elbow;                // the elbow role's position
    };
    struct Targets
    {
        bool on = false;
        Hand hand[2];
        F32 neckWeight = 0.f;           // fade x SlipstreamVRBodyWeightNeck
        LLQuaternion headRot;           // the head's world rotation (the headset)
        LLVector3 headPos;              // where the head joint should be (the headset's position)
        // #1592 arms only on a quadruped (reach from its own shoulders): the eye point (agent) and
        // the avatar's eye height.
        bool eyeOk = false;
        LLVector3 eye;
        F32 avatarEyeH = 0.f;
    };
    static const Targets& targets() { return sTargets; }
    static void noteSolved(F32 headWeight) { sHeadWeight = headWeight; }

private:
    static Targets sTargets;
    static F32 sHeadWeight;
};

// The arms / neck part of the body layer: its joint states and solve. LLVRBodyMotion owns one.
class LLVRBodyArmsLayer
{
public:
    // LLVRBodyMotion::onInitialize: looks up the joints and makes the joint states (the motion
    // adds states() to itself).
    void init(LLCharacter* character);
    const std::vector<LLPointer<LLJointState>>& states() const { return mStates; }
    // The joint state this layer holds for a joint name (the VMC path shares it: a motion has one
    // state per joint), or nullptr.
    LLJointState* stateFor(const char* jointName) const;
    // A bone-pose guide per arm (SlimeVR VMC, #1582).
    struct Guide
    {
        bool owned = false;            // another path (VMC) drives this arm: stay out
        F32 weight = 0.f;              // 0..1: how much its collar / upper arm steer the IK
        LLQuaternion collar, upperArm; // world rotations (SL bones, rest = identity)
    };
    // LLVRBodyMotion::onUpdate, at its start: no weight on this layer's states.
    void clearWeights();
    // LLVRBodyMotion::onUpdate, every frame the motion runs (after the body paths). base: the
    // motion's pose weight. chestPos / chestRot: the chest's world pose when the body layer solved
    // it this frame, else nullptr (the drawn one, last frame's). guide: two (left, right) or nullptr.
    void solve(F32 base, const LLVector3* chestPos, const LLQuaternion* chestRot, const Guide* guide);
    // The motion stops: no weight anywhere.
    void clear();
    // #1592 this frame's retarget frames (LLVRBodyMap::frames; the motion's, set before solve).
    void setFrames(const slbmap::Frames* f) { mFrames = f; }

private:
    enum J { CHEST, COLLAR_L, SHOULDER_L, ELBOW_L, WRIST_L, COLLAR_R, SHOULDER_R, ELBOW_R, WRIST_R, NECK, HEAD, HOOF_L, HOOF_R, NJ };
    LLJoint* mJ[NJ] = {};
    LLPointer<LLJointState> mS[NJ];
    std::vector<LLPointer<LLJointState>> mStates;
    // #1592 the role of each joint and of its parent (LLVRBodyMap::roleOf; -1: none).
    int mRole[NJ] = {}, mParentRole[NJ] = {};
    const slbmap::Frames* mFrames = nullptr;
    slbmap::Q frameR(int role) const { return mFrames && role >= 0 ? mFrames->R[role] : slbmap::Q(); }
    void set(J j, const LLQuaternion& local, F32 w);
    float mTwist[2] = {};   // each forearm's last roll (continuity: no 180-degree flips)
};
