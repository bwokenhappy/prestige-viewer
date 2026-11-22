/**
 * @file llvrbodyarms.cpp
 * @brief VR arms and neck (#1584). See llvrbodyarms.h; the math is llvrbodyarmmath.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodyarms.h"

#include "llcharacter.h"
#include "llformat.h"
#include "lljoint.h"
#include "lljointstate.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llvrbodyarmmath.h"
#include "llvrbodymapping.h"
#include "llvrbodysource.h"
#include "vkgfx/llxrsession.h"

#include <chrono>
#include <cstring>

namespace fb = slfbt;

LLVRBodyArms::Targets LLVRBodyArms::sTargets;
F32 LLVRBodyArms::sHeadWeight = 0.f;

namespace
{
using Clock = std::chrono::steady_clock;
constexpr F32 LOSS_LOG_INTERVAL = 5.f;   // at most one lost / back line per hand per this

fb::V3 toV(const LLVector3& v) { return fb::V3(v.mV[VX], v.mV[VY], v.mV[VZ]); }
LLVector3 toLL(const fb::V3& v) { return LLVector3(v.x, v.y, v.z); }
fb::Q toQ(const LLQuaternion& q) { return fb::Q(q.mQ[VX], q.mQ[VY], q.mQ[VZ], q.mQ[VW]); }
LLQuaternion toLLQ(const fb::Q& q) { return LLQuaternion(q.x, q.y, q.z, q.w); }

// The grip offset per controller (the interaction profile path). First guesses from the
// controllers' shapes: untested in a headset -- tune from testers' reports (the log names the
// one chosen).
struct GripProfile
{
    const char* match;      // a substring of the profile path ("" = the default)
    const char* name;
    fb::GripOffset off;
};
const GripProfile& gripProfile(const std::string& path)
{
    static const GripProfile k[] = {
        {"valve/index_controller", "Valve Index", {0.35f, 0.085f, 0.035f, 0.f}},
        {"touch", "Touch", {0.45f, 0.080f, 0.035f, 0.f}},   // oculus/touch_controller, meta/touch_*
        {"htc/vive_controller", "Vive wand", {0.25f, 0.090f, 0.040f, 0.f}},
        {"", "generic", {0.35f, 0.085f, 0.035f, 0.f}},
    };
    for (const GripProfile& p : k)
        if (!*p.match || path.find(p.match) != std::string::npos) return p;
    return k[3];
}

struct HandState
{
    F32 fade = 0.f;            // the controller drives the arm
    F32 elbowFade = 0.f;       // an elbow role steers it
    bool present = false;      // for the lost / back log
    bool seen = false;
    Clock::time_point lastLog{};
    U32 suppressed = 0;
    std::string profile;       // the profile the offset was logged for
    fb::GripOffset off;
};
struct ArmsState
{
    HandState hand[2];
    F32 neckFade = 0.f;
    bool loggedOn = false;
    U32 profileGen = UINT32_MAX;
};
ArmsState sA;

const char* sideName(int h) { return h ? "right" : "left"; }

void notePresence(int h, bool present)
{
    HandState& hs = sA.hand[h];
    if (present == hs.present) return;
    hs.present = present;
    if (!hs.seen)
    {
        hs.seen = present;   // (the first appearance is the profile line, not "back")
        return;
    }
    const Clock::time_point now = Clock::now();
    if (hs.lastLog != Clock::time_point{} && std::chrono::duration<F32>(now - hs.lastLog).count() < LOSS_LOG_INTERVAL)
    {
        ++hs.suppressed;
        return;
    }
    LL_INFOS("VRBody") << sideName(h) << " controller " << (present ? "back: the arm follows it again" : "lost: the arm fades back to the animation")
                       << (hs.suppressed ? llformat(" (%u more changes in the last %.0f s)", hs.suppressed, LOSS_LOG_INTERVAL) : std::string())
                       << LL_ENDL;
    hs.lastLog = now;
    hs.suppressed = 0;
}
} // namespace

void LLVRBodyArms::reset()
{
    sTargets = Targets{};
    sHeadWeight = 0.f;
    const bool logged = sA.loggedOn;
    sA = ArmsState{};
    if (logged) LL_INFOS("VRBody") << "arms / neck off (VR stopped)" << LL_ENDL;
}

void LLVRBodyArms::update(F32 dt, const LLXrHeadPose& hp, bool headOk, const LLVRBodyFrame* body, const LLVRBodyMapping& m)
{
    static LLCachedControl<bool> s_arms(gSavedSettings, "SlipstreamVRArmTracking", true);
    static LLCachedControl<F32> s_w_arms(gSavedSettings, "SlipstreamVRBodyWeightArms", 1.f);
    static LLCachedControl<F32> s_w_neck(gSavedSettings, "SlipstreamVRBodyWeightNeck", 1.f);
    LLXrSession& xr = LLXrSession::instance();
    LLVOAvatarSelf* av = isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
    // Sitting stops the body (and the share) as before; the arms and neck with it.
    const bool usable = m.ok && headOk && av && !av->isSitting();
    Targets& t = sTargets;
    const fb::Q turnQ = fb::qyaw(m.turn);
    auto toAgent = [&](const fb::V3& b) { return toLL(fb::roomToAgent(b, toV(m.anchor), m.refX, m.refY, m.turn, m.sv, m.footZ)); };

    // ---- Hands: the controllers' grips.
    LLXrInputState in;
    xr.inputState(in);
    const U32 gen = xr.profileGeneration();
    const bool profilesChanged = gen != sA.profileGen;
    sA.profileGen = gen;
    const F32 wArmsAll = s_arms ? llclamp((F32)s_w_arms, 0.f, 1.f) : 0.f;
    // #1592: an arm whose roles all stay with the animation (or have no joint) is not driven.
    const slbmap::Profile& map = LLVRBodyMap::profile();
    for (int h = 0; h < 2; ++h)
    {
        const int first = h ? slbmap::R_R_COLLAR : slbmap::R_L_COLLAR;
        bool mapped = false;
        for (int r = first; r <= first + 3; ++r) mapped = mapped || map.drives(r);
        const F32 wArms = mapped ? wArmsAll : 0.f;
        HandState& hs = sA.hand[h];
        Hand& ht = t.hand[h];
        const float side = h ? -1.f : 1.f;
        const LLXrHandState& hand = in.hand[h];
        const bool present = hand.active && hand.grip.valid;
        if (s_arms) notePresence(h, present);
        if (present && (profilesChanged || hs.profile.empty()))
        {
            const std::string path = xr.handProfile((uint32_t)h);
            if (path != hs.profile || hs.profile.empty())
            {
                const GripProfile& gp = gripProfile(path);
                hs.profile = path.empty() ? std::string("(none)") : path;
                hs.off = gp.off;
                LL_INFOS("VRBody") << sideName(h) << " controller: " << hs.profile << " -> grip offset '" << gp.name
                                   << llformat("' (finger pitch %.0f deg, wrist %.1f cm back, %.1f cm up)", gp.off.pitch * RAD_TO_DEG,
                                               gp.off.back * 100.f, gp.off.lift * 100.f)
                                   << LL_ENDL;
            }
        }
        // The controller, else a wrist tracker (OpenXR left / right_wrist: the hand as the animation
        // turns it), else a tracked hand (#1587). Fly cam keeps them all (slfbt::armInput).
        const uint32_t wr = h ? LLXR_TRK_RIGHT_WRIST : LLXR_TRK_LEFT_WRIST;
        const LLXrHandJoints* hjAll = xr.handJoints((uint32_t)h);
        const fb::ArmInput src = fb::armInput(usable, wArms > 0.f, present, body && body->poses.role[wr].valid,
                                              hjAll && hjAll->active && hjAll->valid[1]);
        const bool ctrl = src == fb::AI_CONTROLLER;
        const bool wristTrk = src == fb::AI_WRIST_TRACKER;
        const bool handTrk = src == fb::AI_HAND;
        const LLXrHandJoints* hj = handTrk ? hjAll : nullptr;
        const bool drive = ctrl || wristTrk || handTrk;
        ht.handFree = wristTrk;
        if (wristTrk) ht.wrist = ht.grip = toAgent(fb::xrPosToBody(body->poses.role[wr].position, m.floorY));
        if (handTrk)
        {
            // XR_EXT_hand_tracking's wrist joint: -Z toward the fingers, +Y out of the back of the
            // hand; in body space those are +X and +Z, so the SL hand (fingers side * Y, back +Z,
            // thumb X) is that frame turned: X_hand -> -side Y, Y_hand -> side X, Z_hand -> Z.
            const fb::Q jq = fb::xrRotToBody(hj->orientation[1]);
            const fb::Q basis = fb::qfromBasis(fb::V3(0.f, -side, 0.f), fb::V3(side, 0.f, 0.f), fb::V3(0.f, 0.f, 1.f));
            ht.wrist = toAgent(fb::xrPosToBody(hj->position[1], m.floorY));
            ht.grip = hj->valid[0] ? toAgent(fb::xrPosToBody(hj->position[0], m.floorY)) : ht.wrist;   // (the palm)
            ht.rot = toLLQ(fb::qnormalized(fb::qmul(turnQ, fb::qmul(jq, basis))));
        }
        if (ctrl)
        {
            const fb::V3 gp = fb::xrPosToBody(hand.grip.position, m.floorY);
            const fb::Q gq = fb::xrRotToBody(hand.grip.orientation);
            fb::V3 wp;
            fb::Q hr;
            fb::gripToHand(gp, gq, side, hs.off, 1.f, wp, hr);   // (room metres: toAgent scales)
            ht.wrist = toAgent(wp);
            ht.grip = toAgent(gp);
            ht.rot = toLLQ(fb::qnormalized(fb::qmul(turnQ, hr)));
        }
        ht.driven = drive;
        hs.fade = fb::fadeStep(hs.fade, drive, dt);
        ht.weight = hs.fade * wArms;

        // The elbow role (OpenXR / SlimeVR elbow trackers, headset elbow joints): the pole.
        const uint32_t er = h ? LLXR_TRK_RIGHT_ELBOW : LLXR_TRK_LEFT_ELBOW;
        const bool elbow = usable && body && body->poses.role[er].valid;
        if (elbow) ht.elbow = toAgent(fb::xrPosToBody(body->poses.role[er].position, m.floorY));
        hs.elbowFade = fb::fadeStep(hs.elbowFade, elbow, dt);
        ht.elbowWeight = hs.elbowFade;
    }

    // ---- Neck: the headset (released in fly cam: m.headWeight eases to 0).
    const F32 wNeck = (map.drives(slbmap::R_NECK) || map.drives(slbmap::R_HEAD)) ? llclamp((F32)s_w_neck, 0.f, 1.f) : 0.f;   // #1592
    const bool neck = usable && wNeck > 0.f;
    if (neck)
    {
        const fb::Q hq = fb::qnormalized(fb::qmul(turnQ, fb::xrRotToBody(hp.orientation)));
        t.headRot = toLLQ(hq);
        // The head joint under the eye point (the avatar's eyes from its head joint, as
        // LLVRCamera::apply anchors the camera: LLVRBodyMap::eyeFromHead).
        const fb::V3 eye = toV(toAgent(fb::xrPosToBody(hp.position, m.floorY)));
        LLJoint* headJ = av ? LLVRBodyMap::jointFor(av, slbmap::R_HEAD) : nullptr;
        t.headPos = toLL(eye - fb::qrot(hq, toV(LLVRBodyMap::eyeFromHead(av, headJ ? headJ : av->mHeadp))));
    }
    sA.neckFade = fb::fadeStep(sA.neckFade, neck, dt);
    // #1592 (arms only on a quadruped: the hands reach from the avatar's own shoulders)
    t.eyeOk = usable;
    if (usable) t.eye = toAgent(fb::xrPosToBody(hp.position, m.floorY));
    t.avatarEyeH = m.avatarEyeH;
    t.neckWeight = sA.neckFade * wNeck * llclamp(m.headWeight, 0.f, 1.f);

    t.on = t.neckWeight > 0.f;
    for (const Hand& ht : t.hand) t.on = t.on || ht.weight > 0.f;
    const F32 wArms = wArmsAll;
    if (t.on != sA.loggedOn)
    {
        sA.loggedOn = t.on;
        LL_INFOS("VRBody") << (t.on ? "arms / neck on: " : "arms / neck off")
                           << (t.on ? llformat("arms %s (weight %.2f), neck weight %.2f", s_arms ? "on the controllers" : "off", wArms, wNeck)
                                    : std::string(" (setting off, sitting, no controllers, or the session stopped)"))
                           << LL_ENDL;
    }
}

// ---- The layer ----------------------------------------------------------------------------------
namespace
{
// A child's offset in its parent's frame (scaled as LLXformMatrix::update does).
fb::V3 offsetIn(LLJoint* child)
{
    LLVector3 p = child->getPosition();
    LLJoint* parent = child->getParent();
    if (parent && parent->getXform()->getScaleChildOffset()) p.scaleVec(parent->getScale());
    return toV(p);
}
// From `ancestor` down to `desc` in the rest pose, in the virtual skeleton (#1592: each offset
// turned by its parent's retarget frame; f nullptr: as it is).
bool chain(LLJoint* ancestor, LLJoint* desc, fb::V3& out, const slbmap::Frames* f = nullptr)
{
    out = fb::V3();
    for (LLJoint* j = desc; j; j = j->getParent())
    {
        if (j == ancestor) return true;
        fb::V3 o = offsetIn(j);
        if (f && !f->identity && j->getParent()) o = slbmap::virtualOffset(toQ(LLVRBodyMap::frameOf(*f, j->getParent())), o);
        out = out + o;
    }
    return false;
}
} // namespace

void LLVRBodyArmsLayer::init(LLCharacter* character)
{
    if (!character) return;
    // #1592: the joints the body mapping assigns to these roles.
    static const int kRoles[NJ] = {slbmap::R_CHEST,       slbmap::R_L_COLLAR,  slbmap::R_L_UPPER_ARM, slbmap::R_L_FOREARM,
                                   slbmap::R_L_HAND,      slbmap::R_R_COLLAR,  slbmap::R_R_UPPER_ARM, slbmap::R_R_FOREARM,
                                   slbmap::R_R_HAND,      slbmap::R_NECK,      slbmap::R_HEAD,        slbmap::R_L_HOOF,
                                   slbmap::R_R_HOOF};
    for (int i = 0; i < NJ; ++i)
    {
        mJ[i] = LLVRBodyMap::jointFor(character, kRoles[i]);
        mRole[i] = mJ[i] ? LLVRBodyMap::roleOf(mJ[i]) : -1;
        mParentRole[i] = mJ[i] ? LLVRBodyMap::roleOf(mJ[i]->getParent()) : -1;
        if (!mJ[i] || i == CHEST) continue;   // (the chest is read, never driven here)
        mS[i] = new LLJointState;
        mS[i]->setJoint(mJ[i]);
        mS[i]->setUsage(LLJointState::ROT);
        mS[i]->setWeight(0.f);
        mStates.push_back(mS[i]);
    }
}

void LLVRBodyArmsLayer::set(J j, const LLQuaternion& virtualLocal, F32 w)
{
    if (!mS[j]) return;
    // #1592: the virtual skeleton's local onto the avatar's joint.
    LLQuaternion q = toLLQ(slbmap::actualLocal(frameR(mParentRole[j]), frameR(mRole[j]), toQ(virtualLocal)));
    q.normalize();
    if (!q.isFinite()) return;
    mS[j]->setRotation(q);
    mS[j]->setWeight(w);
}

LLJointState* LLVRBodyArmsLayer::stateFor(const char* jointName) const
{
    for (int i = 0; i < NJ; ++i)
        if (mS[i] && mJ[i] && mJ[i]->getName() == jointName) return mS[i].get();
    return nullptr;
}

void LLVRBodyArmsLayer::clearWeights()
{
    for (LLPointer<LLJointState>& s : mStates) s->setWeight(0.f);
}

void LLVRBodyArmsLayer::clear()
{
    clearWeights();
    LLVRBodyArms::noteSolved(0.f);
}

void LLVRBodyArmsLayer::solve(F32 base, const LLVector3* chestPos, const LLQuaternion* chestRot, const Guide* guide)
{
    const LLVRBodyArms::Targets& t = LLVRBodyArms::targets();
    if (!t.on || base <= 0.f || !mJ[CHEST])
    {
        LLVRBodyArms::noteSolved(0.f);
        return;
    }
    const fb::V3 cP = toV(chestPos ? *chestPos : mJ[CHEST]->getWorldPosition());
    // (#1592: the drawn chest into the virtual skeleton; the body layer's is already)
    const fb::Q cQ = chestRot ? fb::qnormalized(toQ(*chestRot)) : slbmap::virtualWorld(toQ(mJ[CHEST]->getWorldRotation()), frameR(mRole[CHEST]));
    const slbmap::Profile& map = LLVRBodyMap::profile();
    const slbmap::Frames* vf = mFrames;

    // ---- Arms.
    static const fb::ArmParams kParams;
    for (int h = 0; h < 2; ++h)
    {
        const LLVRBodyArms::Hand& ht = t.hand[h];
        const J collarJ = h ? COLLAR_R : COLLAR_L, shoulderJ = h ? SHOULDER_R : SHOULDER_L;
        const J elbowJ = h ? ELBOW_R : ELBOW_L, wristJ = h ? WRIST_R : WRIST_L, hoofJ = h ? HOOF_R : HOOF_L;
        const Guide* g = guide ? &guide[h] : nullptr;
        if (ht.weight <= 0.f || (g && g->owned))
        {
            mTwist[h] = 0.f;   // (the next time it drives, from the untwisted forearm)
            continue;
        }
        if (!mJ[collarJ] || !mJ[shoulderJ] || !mJ[elbowJ] || !mJ[wristJ]) continue;
        fb::ArmRest r;
        r.side = h ? -1.f : 1.f;
        if (!chain(mJ[CHEST], mJ[collarJ], r.collar, vf) || !chain(mJ[collarJ], mJ[shoulderJ], r.shoulder, vf) ||
            !chain(mJ[shoulderJ], mJ[elbowJ], r.elbow, vf) || !chain(mJ[elbowJ], mJ[wristJ], r.wrist, vf))
            continue;
        // #1592 a hoof past the hand (the Open Pony's mHandRing1 under the cannon): the grip is
        // where the hoof joint goes and the hoof turns as your hand (solveHoofArm).
        fb::V3 cannon;
        // (A wrist tracker gives no hand rotation: its point is the wrist's, as before.)
        const bool hoof = !ht.handFree && mJ[hoofJ] && mS[hoofJ] && map.drives(slbmap::hoofOf(h)) &&
                          chain(mJ[wristJ], mJ[hoofJ], cannon, vf);
        const fb::Q handQ = toQ(ht.rot);
        // A retargeted limb's collar stays with the chest (vf->armRetarget): no turn of its own.
        const bool holdCollar = vf && vf->armRetarget[h];
        const fb::V3 collarP = cP + fb::qrot(cQ, r.collar);
        // #1592 a limb not built like your arm: the controller from your shoulder, scaled by the
        // limb's reach over yours, from the avatar's own shoulder (the collar as it rests).
        fb::V3 endT = toV(hoof ? ht.grip : ht.wrist), elbowHint = toV(ht.elbow);
        if (map.reachRebase && t.eyeOk && t.avatarEyeH > 0.1f)
        {
            const fb::V3 limbShoulder = collarP + fb::qrot(cQ, r.shoulder);
            const fb::V3 yours = slbmap::humanShoulder(toV(t.eye), fb::yawOf(cQ), t.avatarEyeH, r.side);
            const F32 k = slbmap::reachScale(r.reach() + (hoof ? fb::length(cannon) : 0.f), t.avatarEyeH);
            endT = slbmap::remapReach(endT, yours, limbShoulder, k);
            elbowHint = slbmap::remapReach(elbowHint, yours, limbShoulder, k);
        }
        // On the controller. Pole: the VMC upper arm's elbow, else the elbow role, else natural
        // (blended by their weights); collar: VMC's (blended), else collarTurn.
        const F32 gw = g ? llclamp(g->weight, 0.f, 1.f) : 0.f;
        // The cannon's rest local under the forearm, in the virtual skeleton (the actual one is
        // identity: virtual = R_forearm * conj(R_cannon)).
        const fb::Q cannonRest = hoof ? fb::qnormalized(fb::qmul(frameR(mParentRole[wristJ]), fb::qconj(frameR(mRole[wristJ])))) : fb::Q();
        const F32 limbReach = hoof ? fb::length(r.elbow) + fb::length(r.wrist + fb::qrot(cannonRest, cannon)) : r.reach();
        fb::Q collarL = holdCollar ? fb::Q() : fb::collarTurn(collarP, cQ, r, endT, kParams);
        if (gw > 0.f && !holdCollar) collarL = fb::qslerp(collarL, fb::qmul(fb::qconj(cQ), toQ(g->collar)), gw);
        const fb::V3 shoulderP = collarP + fb::qrot(fb::qmul(cQ, collarL), r.shoulder);
        fb::V3 pole = holdCollar ? fb::forelegPole(shoulderP, endT, cQ, limbReach) : fb::elbowPole(shoulderP, endT, cQ, r.side, limbReach);
        // (An elbow tracker sits on the arm: on a straight arm its offset across the shoulder ->
        // wrist line is noise, so stablePole falls back to the natural pole there.)
        if (ht.elbowWeight > 0.f)
            pole = fb::lerp(pole, fb::stablePole(shoulderP, endT, elbowHint, pole), llclamp(ht.elbowWeight, 0.f, 1.f));
        if (gw > 0.f) pole = fb::lerp(pole, shoulderP + fb::qrot(toQ(g->upperArm), r.elbow), gw);
        const F32 bw = base * llclamp(ht.weight, 0.f, 1.f);
        if (hoof)
        {
            static const fb::HoofParams kHoof;
            const fb::HoofPose hp = fb::solveHoofArm(cP, cQ, r, cannon, cannonRest, endT, handQ, &pole, kParams, kHoof, &collarL, &mTwist[h]);
            const fb::ArmPose& p = hp.arm;
            if (!p.ok) continue;
            set(collarJ, toLLQ(fb::qmul(fb::qconj(cQ), p.collarWorld)), bw);
            set(shoulderJ, toLLQ(fb::qmul(fb::qconj(p.collarWorld), p.shoulderWorld)), bw);
            set(elbowJ, toLLQ(fb::qmul(fb::qconj(p.shoulderWorld), p.elbowWorld)), bw);
            set(wristJ, toLLQ(fb::qmul(fb::qconj(p.elbowWorld), p.wristWorld)), bw);
            set(hoofJ, toLLQ(fb::qmul(fb::qconj(p.wristWorld), hp.hoofWorld)), bw);   // (the controller's turn)
            continue;
        }
        fb::ArmPose p = fb::solveArm(cP, cQ, r, endT, handQ, &pole, kParams, &collarL, &mTwist[h]);
        if (!p.ok) continue;
        if (ht.handFree)
        {
            // A wrist tracker: no hand rotation -- the forearm untwisted, the wrist straight.
            const fb::V3 axis = fb::normalized(r.wrist, fb::V3(0.f, r.side, 0.f));
            p.elbowWorld = fb::qnormalized(fb::qmul(p.elbowWorld, fb::qaxis(axis, -p.twist * kParams.twistShare)));
            p.wristWorld = p.elbowWorld;
        }
        set(collarJ, toLLQ(fb::qmul(fb::qconj(cQ), p.collarWorld)), bw);
        set(shoulderJ, toLLQ(fb::qmul(fb::qconj(p.collarWorld), p.shoulderWorld)), bw);
        set(elbowJ, toLLQ(fb::qmul(fb::qconj(p.shoulderWorld), p.elbowWorld)), bw);
        set(wristJ, toLLQ(fb::qmul(fb::qconj(p.elbowWorld), p.wristWorld)), bw);
    }

    // ---- Neck.
    F32 headW = 0.f;
    fb::V3 restNeck, restHead;
    if (t.neckWeight > 0.f && mJ[NECK] && mJ[HEAD] && chain(mJ[CHEST], mJ[NECK], restNeck, vf) && chain(mJ[NECK], mJ[HEAD], restHead, vf))
    {
        fb::NeckRest r;
        r.neck = restNeck;
        r.head = restHead;
        static const fb::NeckParams kNeck;
        const fb::V3 neckP = cP + fb::qrot(cQ, r.neck);
        const fb::V3 headP = toV(t.headPos);
        // (#1592: a quadruped's head is not where the headset is: no lean toward it)
        const fb::NeckPose np = fb::splitNeck(cQ, toQ(t.headRot), neckP, map.neckLean ? &headP : nullptr, r, kNeck);
        headW = base * llclamp(t.neckWeight, 0.f, 1.f);
        set(NECK, toLLQ(fb::qmul(fb::qconj(cQ), np.neckWorld)), headW);
        set(HEAD, toLLQ(fb::qmul(fb::qconj(np.neckWorld), np.headWorld)), headW);
    }
    LLVRBodyArms::noteSolved(headW);
}
