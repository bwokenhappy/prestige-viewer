/**
 * @file llvrbodyfingers.h
 * @brief #1587 VR finger tracking: your own avatar's Bento fingers (mHand<Finger>1..3, thumb
 *        included) follow your fingers. Part of the VR body layer (LLVRBodyMotion), own avatar
 *        only; others see it through the body share (#1584) under the same deadband.
 *
 * Sources per hand, best first (llvrbodyfingermath.h):
 *   hand tracking   XR_EXT_hand_tracking where the runtime offers it: Quest camera hand tracking,
 *                   and SteamVR, which gives the Valve Index controllers' per-finger curl and splay
 *                   sensing as hand joints (OpenXR has no per-finger paths on the Index profile;
 *                   this is SteamVR's skeletal input). Joint positions only, unobstructed range.
 *   the controller  a natural grip from the trigger value, the trigger / thumb rest touches
 *                   (Index, Touch) and the grip: point, fist, thumbs up, open hand.
 * Blended over the hand animation (NORMAL_BLEND at ADDITIVE_PRIORITY: a tracked finger overrides
 * it); a hand that loses both fades back to the animation over 0.3 s. They stay on in fly cam (#1584);
 * an arm SlimeVR VMC drives (no controller) keeps VMC's fingers.
 *
 * Setting: SlipstreamVRFingerTracking (persisted, on; live). Hand trackers exist only while it is
 * on in VR and the runtime offers hand tracking. Log "VRBody": each hand's finger source on change.
 */
#pragma once

#include "llpointer.h"
#include "stdtypes.h"

#include <vector>

class LLCharacter;
class LLJoint;
class LLJointState;
struct LLVRBodyMapping;
struct LLXrHeadPose;

namespace slfinger
{
struct HandPose;
}

class LLVRBodyFingers
{
public:
    // Every VR frame (LLVRBody::update, with the arms). m: the body's mapping.
    static void update(F32 dt, bool headOk, const LLVRBodyMapping& m);
    static void reset();
    static bool on();
    // The pose and its weight (fade x 1) for a hand (0 left, 1 right).
    static F32 weight(int side);
    static const slfinger::HandPose& pose(int side);
};

class LLVRBodyFingersLayer
{
public:
    void init(LLCharacter* character);
    const std::vector<LLPointer<LLJointState>>& states() const { return mStates; }
    LLJointState* stateFor(const char* jointName) const;   // (the VMC path shares them)
    void clearWeights();
    // owned[side]: another path (VMC) drives that hand: stay out. Null: none.
    void solve(F32 base, const bool* owned);
    void clear() { clearWeights(); }

private:
    static constexpr int NJ = 2 * 5 * 3;   // side x finger x segment
    LLJoint* mJ[NJ] = {};
    LLPointer<LLJointState> mS[NJ];
    std::vector<LLPointer<LLJointState>> mStates;
};
