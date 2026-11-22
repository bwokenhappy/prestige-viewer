/**
 * @file llvrbodyestimate.cpp
 * @brief #1586 VR 3-point body estimate. See llvrbodyestimate.h; the math is llvrbodyestimatemath.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodyestimate.h"

#include "llagent.h"
#include "llvoavatarself.h"
#include "llvrbodyestimatemath.h"
#include "llvrbodysource.h"
#include "vkgfx/llxrsession.h"

namespace fb = slfbt;

namespace
{
constexpr U32 GENERATION = 1;        // the role set never changes

struct EstimateState
{
    bool on = false;
    bool moving = false;
    slest::MoveGate gate;
    slest::State st;
};
EstimateState sE;

// The avatar is walking / flying / sitting (the animations own the body then). Sitting and flying
// at once; walking through the debounced gate (slest::stepMoveGate): the movement controls the
// sticks set (intent), else the avatar's horizontal speed (autopilot, pushed, sliding).
bool avatarBusy(F32 dt)
{
    if (!isAgentAvatarValid()) return true;
    if (gAgentAvatarp->isSitting() || gAgent.getFlying())
    {
        sE.gate = slest::MoveGate{};
        sE.gate.moving = true;   // (leaving a sit / flight goes through the leave debounce)
        return true;
    }
    const U32 moveFlags = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG |
                          AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG | AGENT_CONTROL_NUDGE_LEFT_POS |
                          AGENT_CONTROL_NUDGE_LEFT_NEG;
    const bool intent = (gAgent.getControlFlags() & moveFlags) != 0 || gAgent.getAutoPilot();
    LLVector3 v = gAgentAvatarp->getVelocity();
    v.mV[VZ] = 0.f;
    return slest::stepMoveGate(sE.gate, intent, v.magVec(), dt);
}
} // namespace

bool LLVRBodyEstimate::poll(bool want, const LLXrHeadPose& hp, F32 dt, F32 eyeH, F32 floorY, bool haveBodyYaw, F32 bodyYaw,
                            LLVRBodyFrame& out)
{
    if (!want || !hp.valid)
    {
        if (sE.on) LL_INFOS("VRBody") << "body estimate off" << LL_ENDL;
        sE = EstimateState{};
        return false;
    }
    if (!sE.on)
    {
        sE.on = true;
        LL_INFOS("VRBody") << "body estimate on (headset + controllers, no trackers): hips, crouch, planted feet and steps "
                              "estimated; wrist panel > Body measures your eye height"
                           << LL_ENDL;
    }
    const bool busy = avatarBusy(dt);
    if (busy != sE.moving)
    {
        sE.moving = busy;
        LL_INFOS("VRBody") << (busy ? "body estimate: the avatar walks / flies / sits, the legs and hips follow its animation"
                                    : "body estimate: standing again, feet planted")
                           << LL_ENDL;
        if (!busy) sE.st = slest::State{};   // (plant the feet anew where you stand)
    }

    static const slest::Params kParams;
    const slest::Shape shape = slest::makeShape(llclamp(eyeH, 0.8f, 2.5f));
    slest::Input in;
    in.head = fb::xrPosToBody(hp.position, floorY);
    in.headRot = fb::xrRotToBody(hp.orientation);
    in.haveBodyYaw = haveBodyYaw;
    in.bodyYaw = bodyYaw;
    LLXrInputState is;
    if (LLXrSession::instance().inputState(is))
    {
        for (int h = 0; h < 2; ++h)
        {
            const LLXrHandState& hs = is.hand[h];
            in.hand[h] = hs.active && hs.grip.valid;
            if (in.hand[h]) in.handPos[h] = fb::xrPosToBody(hs.grip.position, floorY);
        }
    }
    const slest::Output o = slest::estimate(sE.st, in, shape, dt, kParams);

    out = LLVRBodyFrame{};
    out.kind = LLVRBodySource::ESTIMATE;
    out.bonePoses = true;
    out.poses.generation = GENERATION;
    auto put = [&](uint32_t role, const fb::V3& p, const fb::Q& q, F32 conf) {
        LLXrTrackerPose& tp = out.poses.role[role];
        tp.active = true;
        ++out.poses.connected;
        out.confidence[role] = conf;
        if (busy) return;   // (the animation's while the avatar moves: the roles fade out)
        tp.valid = true;
        tp.tracked = false;   // (estimated)
        fb::bodyPosToXr(p, tp.position);
        tp.position[1] += floorY;
        fb::bodyRotToXr(q, tp.orientation);
    };
    // The hips are a guess: a little extra smoothing (LLVRBody: confidence < 1); the feet move
    // only when they step, which is already eased.
    put(LLXR_TRK_WAIST, o.hips, o.hipsRot, 0.75f);
    put(LLXR_TRK_LEFT_FOOT, o.ankle[0], fb::qyaw(o.footYaw[0]), 1.f);
    put(LLXR_TRK_RIGHT_FOOT, o.ankle[1], fb::qyaw(o.footYaw[1]), 1.f);
    return true;
}
