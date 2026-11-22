/**
 * @file llvrbodyfingers.cpp
 * @brief #1587 VR finger tracking. See llvrbodyfingers.h; the math is llvrbodyfingermath.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodyfingers.h"

#include "llcharacter.h"
#include "lljoint.h"
#include "lljointstate.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llvrbodyarms.h"
#include "llvrbodyfingermath.h"
#include "llvrbodymapping.h"
#include "llvrbodymath.h"
#include "vkgfx/llxrsession.h"

namespace fb = slfbt;
namespace sf = slfinger;

namespace
{
constexpr F32 JOINTS_TAU = 0.04f;       // smoothing of hand-tracked poses (seconds)
constexpr F32 CONTROLLER_TAU = 0.07f;   // ... of controller poses (touches are on / off)

enum Source { SRC_NONE = 0, SRC_JOINTS, SRC_CONTROLLER };
const char* sourceName(int s)
{
    return s == SRC_JOINTS ? "hand tracking (XR_EXT_hand_tracking: camera hands, or the Index controllers' finger sensing)"
         : s == SRC_CONTROLLER ? "the controller (trigger, grip and touch: point, fist, thumbs up, open)"
                               : "none (the hand animation)";
}

struct HandState
{
    sf::HandPose pose;
    F32 fade = 0.f;
    int source = SRC_NONE;
    bool smoothValid = false;
};
struct FingerState
{
    HandState hand[2];
};
FingerState sF;
const sf::Limits kLimits;
} // namespace

void LLVRBodyFingers::reset()
{
    sF = FingerState{};
    LLXrSession::instance().requestHandTracking(false);
}

bool LLVRBodyFingers::on()
{
    return sF.hand[0].fade > 0.f || sF.hand[1].fade > 0.f;
}

F32 LLVRBodyFingers::weight(int side)
{
    return (side == 0 || side == 1) ? sF.hand[side].fade : 0.f;
}

const sf::HandPose& LLVRBodyFingers::pose(int side)
{
    return sF.hand[side == 1 ? 1 : 0].pose;
}

void LLVRBodyFingers::update(F32 dt, bool headOk, const LLVRBodyMapping& m)
{
    static LLCachedControl<bool> s_fingers(gSavedSettings, "SlipstreamVRFingerTracking", true);
    LLXrSession& xr = LLXrSession::instance();
    LLVOAvatarSelf* av = isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
    const bool usable = s_fingers && m.ok && headOk && av && !av->isSitting();   // (fly cam too: #1584)
    // Hand trackers only while the fingers are on in VR and the runtime tracks hands.
    const bool want = s_fingers && m.ok && xr.handTrackingOffered();
    xr.requestHandTracking(want);   // (cheap when nothing changes)

    LLXrInputState in;
    const bool inOk = xr.inputState(in);
    for (int h = 0; h < 2; ++h)
    {
        HandState& hs = sF.hand[h];
        int src = SRC_NONE;
        sf::HandPose target;
        if (usable)
        {
            const LLXrHandJoints* hj = xr.handJoints((uint32_t)h);
            if (hj && hj->active)
            {
                fb::V3 j[sf::JOINTS];
                bool valid[sf::JOINTS];
                for (int i = 0; i < sf::JOINTS; ++i)
                {
                    j[i] = fb::V3(hj->position[i][0], hj->position[i][1], hj->position[i][2]);   // (OpenXR axes: a proper rotation of SL's)
                    valid[i] = hj->valid[i];
                }
                if (sf::fromJoints(j, valid, h ? -1.f : 1.f, kLimits, target)) src = SRC_JOINTS;
            }
            const LLXrHandState& hand = in.hand[h];
            if (src == SRC_NONE && inOk && hand.active && hand.grip.valid)
            {
                sf::ControllerInput ci;
                ci.trigger = hand.selectValue >= 0.f ? hand.selectValue : (hand.select ? 1.f : 0.f);
                ci.triggerTouch = hand.triggerTouch;
                ci.triggerTouchBound = hand.triggerTouchBound;
                ci.squeeze = hand.squeezeValue;
                ci.thumbTouch = hand.thumbTouch;
                ci.thumbTouchBound = hand.thumbTouchBound;
                target = sf::fromController(ci, kLimits);
                src = SRC_CONTROLLER;
            }
        }
        if (src != hs.source && (src != SRC_NONE || hs.source != SRC_NONE))
        {
            LL_INFOS("VRBody") << (h ? "right" : "left") << " fingers: " << sourceName(src) << LL_ENDL;
            hs.source = src;
        }
        if (src != SRC_NONE)
        {
            if (!hs.smoothValid || hs.fade <= 0.f) hs.pose = target;
            else sf::smoothPose(hs.pose, target, dt, src == SRC_JOINTS ? JOINTS_TAU : CONTROLLER_TAU);
            hs.smoothValid = true;
        }
        hs.fade = fb::fadeStep(hs.fade, src != SRC_NONE, dt);   // (fading out: the last pose)
        if (hs.fade <= 0.f) hs.smoothValid = false;
    }
}

// ---- The layer --------------------------------------------------------------------------------
namespace
{
const char* const kFinger[5] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
int slot(int side, int f, int k) { return (side * 5 + f) * 3 + k; }
fb::V3 offsetIn(LLJoint* child)
{
    LLVector3 p = child->getPosition();
    LLJoint* parent = child->getParent();
    if (parent && parent->getXform()->getScaleChildOffset()) p.scaleVec(parent->getScale());
    return fb::V3(p.mV[VX], p.mV[VY], p.mV[VZ]);
}
} // namespace

void LLVRBodyFingersLayer::init(LLCharacter* character)
{
    if (!character) return;
    for (int side = 0; side < 2; ++side)
        for (int f = 0; f < 5; ++f)
            for (int k = 0; k < 3; ++k)
            {
                const std::string name = std::string("mHand") + kFinger[f] + std::to_string(k + 1) + (side ? "Right" : "Left");
                const int i = slot(side, f, k);
                mJ[i] = character->getJoint(name);
                if (!mJ[i]) continue;   // (a skeleton without Bento hands: nothing to drive)
                // #1592: a finger bone the body mapping gives a role of its own (the Open Pony's
                // hoof on mHandRing1) is that role's: the arms layer holds it, never a finger pose.
                bool own = false;
                const int role = LLVRBodyMap::roleOf(mJ[i], &own);
                if (own && role >= 0 && !slbmap::isFingers(role))
                {
                    mJ[i] = nullptr;
                    continue;
                }
                mS[i] = new LLJointState;
                mS[i]->setJoint(mJ[i]);
                mS[i]->setUsage(LLJointState::ROT);
                mS[i]->setWeight(0.f);
                mStates.push_back(mS[i]);
            }
}

LLJointState* LLVRBodyFingersLayer::stateFor(const char* jointName) const
{
    for (int i = 0; i < NJ; ++i)
        if (mS[i] && mJ[i] && mJ[i]->getName() == jointName) return mS[i].get();
    return nullptr;
}

void LLVRBodyFingersLayer::clearWeights()
{
    for (LLPointer<LLJointState>& s : mStates) s->setWeight(0.f);
}

void LLVRBodyFingersLayer::solve(F32 base, const bool* owned)
{
    if (base <= 0.f || !LLVRBodyFingers::on()) return;
    for (int side = 0; side < 2; ++side)
    {
        const F32 w = base * llclamp(LLVRBodyFingers::weight(side), 0.f, 1.f);
        if (w <= 0.f || (owned && owned[side])) continue;
        const sf::HandPose& p = LLVRBodyFingers::pose(side);
        const float sd = side ? -1.f : 1.f;
        for (int f = 0; f < 5; ++f)
            for (int k = 0; k < 3; ++k)
            {
                const int i = slot(side, f, k);
                if (!mS[i]) continue;
                // The bone's direction: toward its child; the last one continues its own offset.
                const fb::V3 dir = k < 2 && mJ[i + 1] ? offsetIn(mJ[i + 1]) : offsetIn(mJ[i]);
                const fb::Q q = sf::boneLocal(f, k, dir, sd, p);
                LLQuaternion lq(q.x, q.y, q.z, q.w);
                if (!lq.isFinite()) continue;
                mS[i]->setRotation(lq);
                mS[i]->setWeight(w);
            }
    }
}
