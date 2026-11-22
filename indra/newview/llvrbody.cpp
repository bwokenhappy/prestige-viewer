/**
 * @file llvrbody.cpp
 * @brief VR full-body tracking (#1565). See llvrbody.h; the math is llvrbodymath.h.
 *
 * Spaces: "body space" is the VR room in SL axes (X forward, Y left, Z up), metres, z = height
 * above the floor (OpenXR app space turned: SL = (-z, -x, y), less the floor's y). Calibration
 * offsets and smoothing live there. Each frame the bones go to agent space: across, from the
 * recentre reference turned by the rig onto LLVRCamera's anchor (never relative to the headset:
 * a head sway must not slide a standing foot); up, from the avatar's feet; both scaled by the
 * avatar's eye height over yours (your 1.6 m body drives a 2 m avatar's 2 m body). In view
 * height mode 0 the camera moves 1:1 while the body is scaled, so off the room's centre the
 * camera sits a few cm from the avatar's head (mode 2 matches them).
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbody.h"

#include "llagent.h"
#include "llcharacter.h"
#include "llformat.h"
#include "lljoint.h"
#include "lljointstate.h"
#include "llmotion.h"
#include "llsd.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llvrbodyarmmath.h"
#include "llvrbodyarms.h"
#include "llvrbodyfingers.h"
#include "llvrbodymapping.h"
#include "llvrbodymath.h"
#include "llvrbodyshare.h"
#include "llvrbodysource.h"
#include "llvrcamera.h"
#include "vkgfx/llxrsession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace fb = slfbt;

bool LLVRBody::sAvailable = false;
bool LLVRBody::sCalibrating = false;
bool LLVRBody::sHoldAnchor = false;
bool LLVRBody::sPoseHeld = false;
bool LLVRBody::sUpright = false;
F32 LLVRBody::sLiveBlend = 1.f;
LLVRBody::Targets LLVRBody::sTargets;

namespace
{
using Clock = std::chrono::steady_clock;

const LLUUID ANIM_VR_BODY("0e5e3083-06a8-4114-b54e-acd3b8fdb406");   // local only, never sent
constexpr F32 CALIBRATION_COUNTDOWN = 3.f;   // seconds to get into the T-pose after the button
constexpr F32 LOSS_LOG_INTERVAL = 5.f;       // at most one lost / back line per bone per this

fb::V3 toV(const LLVector3& v) { return fb::V3(v.mV[VX], v.mV[VY], v.mV[VZ]); }
LLVector3 toLL(const fb::V3& v) { return LLVector3(v.x, v.y, v.z); }
fb::Q toQ(const LLQuaternion& q) { return fb::Q(q.mQ[VX], q.mQ[VY], q.mQ[VZ], q.mQ[VW]); }
LLQuaternion toLLQ(const fb::Q& q) { return LLQuaternion(q.x, q.y, q.z, q.w); }

// OpenXR app space -> body space (see the file comment; llvrbodymath.h).
using fb::xrPosToBody;
using fb::xrRotToBody;

// ---- The avatar's skeleton -------------------------------------------------------------------
struct Joints
{
    LLJoint* pelvis = nullptr;
    LLJoint* hip[2] = {}, *knee[2] = {}, *ankle[2] = {};
    LLJoint* torso = nullptr, *chest = nullptr, *neck = nullptr, *head = nullptr;
    // #1592: the joints the body mapping assigns to the roles.
    void lookup(LLCharacter* ch)
    {
        namespace sb = slbmap;
        pelvis = LLVRBodyMap::jointFor(ch, sb::R_PELVIS);
        hip[0] = LLVRBodyMap::jointFor(ch, sb::R_L_THIGH);
        knee[0] = LLVRBodyMap::jointFor(ch, sb::R_L_SHIN);
        ankle[0] = LLVRBodyMap::jointFor(ch, sb::R_L_FOOT);
        hip[1] = LLVRBodyMap::jointFor(ch, sb::R_R_THIGH);
        knee[1] = LLVRBodyMap::jointFor(ch, sb::R_R_SHIN);
        ankle[1] = LLVRBodyMap::jointFor(ch, sb::R_R_FOOT);
        torso = LLVRBodyMap::jointFor(ch, sb::R_SPINE);
        chest = LLVRBodyMap::jointFor(ch, sb::R_CHEST);
        neck = LLVRBodyMap::jointFor(ch, sb::R_NECK);
        head = LLVRBodyMap::jointFor(ch, sb::R_HEAD);
    }
};

struct Skeleton
{
    fb::LegRest leg[2];        // 0 left, 1 right
    fb::SpineRest spine;
    fb::V3 chestInTorso;       // the chest in the torso's frame (through any Bento spine joints)
    fb::V3 pelvisToNeck;
    fb::V3 neckToEye;          // the VR camera's eye point from the neck (LLVRCamera's anchor)
    F32 pelvisToFoot = 0.f;
    F32 eyeH = 0.f;            // the eye point above the soles, standing
    fb::Proportions pr;
};

// A child's offset in its parent's frame (scaled as LLXformMatrix::update does).
fb::V3 offsetIn(LLJoint* child)
{
    LLVector3 p = child->getPosition();
    LLJoint* parent = child->getParent();
    if (parent && parent->getXform()->getScaleChildOffset()) p.scaleVec(parent->getScale());
    return toV(p);
}

// From `ancestor` down to `desc` in the rest pose (every rotation identity: the offsets add).
// #1592: with frames, in the virtual skeleton the solver sees (each offset turned by its
// parent's retarget frame: the upright pony's spine stands up, its forelegs reach as arms).
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

bool measure(LLCharacter* ch, const Joints& j, Skeleton& s, const slbmap::Frames* f)
{
    if (!ch || !j.pelvis || !j.torso || !j.chest || !j.neck || !j.head) return false;
    bool ok = chain(j.pelvis, j.torso, s.spine.torso, f) && chain(j.torso, j.neck, s.spine.neck, f) &&
              chain(j.torso, j.chest, s.chestInTorso, f);
    fb::V3 neckHead;
    ok = ok && chain(j.neck, j.head, neckHead, f);
    for (int i = 0; i < 2 && ok; ++i)
    {
        ok = j.hip[i] && j.knee[i] && j.ankle[i] && chain(j.pelvis, j.hip[i], s.leg[i].hip, f) &&
             chain(j.hip[i], j.knee[i], s.leg[i].knee, f) && chain(j.knee[i], j.ankle[i], s.leg[i].ankle, f);
    }
    if (!ok) return false;
    s.pelvisToNeck = s.spine.torso + s.spine.neck;
    // The VR camera's eye point (LLVRBodyMap::eyeFromHead: the avatar's own eyes; LLVRCamera::apply
    // anchors on the same point).
    fb::V3 eye = toV(LLVRBodyMap::eyeFromHead(ch, j.head));
    if (f && !f->identity) eye = slbmap::virtualOffset(toQ(LLVRBodyMap::frameOf(*f, j.head)), eye);
    s.neckToEye = neckHead + eye;
    LLVOAvatar* av = dynamic_cast<LLVOAvatar*>(ch);
    s.pelvisToFoot = av ? av->getPelvisToFoot() : 0.f;
    if (s.pelvisToFoot <= 0.05f) s.pelvisToFoot = -(s.leg[0].hip.z + s.leg[0].knee.z + s.leg[0].ankle.z);
    else if (f && f->upright)
    {
        // #1592 standing up: the hind legs hang as they rest, so the foot joint stays as high above
        // the floor as on four legs; the pelvis is where the upright legs put it.
        fb::V3 a;
        if (chain(j.pelvis, j.ankle[0], a))
            s.pelvisToFoot = (s.pelvisToFoot + a.z) - (s.leg[0].hip.z + s.leg[0].knee.z + s.leg[0].ankle.z);
    }
    s.eyeH = s.pelvisToFoot + s.pelvisToNeck.z + s.neckToEye.z;
    if (!(s.eyeH > 0.3f && s.eyeH < 10.f)) return false;
    const F32 inv = 1.f / s.eyeH;
    const fb::LegRest& l = s.leg[0];
    s.pr.knee = (s.pelvisToFoot + l.hip.z + l.knee.z) * inv;
    s.pr.ankle = (s.pelvisToFoot + l.hip.z + l.knee.z + l.ankle.z) * inv;
    s.pr.pelvis = s.pelvisToFoot * inv;
    s.pr.chest = (s.pelvisToFoot + s.spine.torso.z + s.chestInTorso.z) * inv;
    s.pr.eyeForward = (s.pelvisToNeck.x + s.neckToEye.x) * inv;
    return true;
}

// #1592: our avatar's mapped joints and skeleton, measured as the solver sees it this frame.
bool measureAvatar(LLCharacter* av, Skeleton& s)
{
    if (!av) return false;
    Joints j;
    j.lookup(av);
    slbmap::Frames f;
    LLVRBodyMap::frames(av, f, nullptr, LLVRBody::upright());
    return measure(av, j, s, &f);
}

// ---- The layer: a motion on our own avatar ----------------------------------------------------
class LLVRBodyMotion : public LLMotion
{
public:
    LLVRBodyMotion(const LLUUID& id) : LLMotion(id) {}
    static LLMotion* create(const LLUUID& id) { return new LLVRBodyMotion(id); }

    bool getLoop() override { return true; }
    F32 getDuration() override { return 0.f; }
    F32 getEaseInDuration() override { return 0.f; }    // (LLVRBody fades each limb itself)
    F32 getEaseOutDuration() override { return 0.f; }
    // Over every animation (an animation's priority is clamped below ADDITIVE_PRIORITY), as a
    // regular blend: a weight below 1 mixes the animation back in (FSPosingMotion does the same).
    LLJoint::JointPriority getPriority() override { return LLJoint::ADDITIVE_PRIORITY; }
    LLMotionBlendType getBlendType() override { return NORMAL_BLEND; }
    F32 getMinPixelArea() override { return 0.f; }   // our own avatar: never LOD'd out
    LLMotionInitStatus onInitialize(LLCharacter* character) override;
    bool onActivate() override { return true; }
    bool onUpdate(F32 time, U8* joint_mask) override;
    void onDeactivate() override
    {
        LLVRBody::noteSolved(LLQuaternion::DEFAULT, 0.f, false);
        mArms.clear();
        mFingers.clear();   // #1587
        LLVRBodyShare::layerStopped();   // #1584
    }

private:
    enum J { PELVIS, SPINE1, SPINE2, TORSO, SPINE3, SPINE4, CHEST, HIP_L, KNEE_L, ANKLE_L, HIP_R, KNEE_R, ANKLE_R, NJ };
    LLCharacter* mChar = nullptr;
    Joints mJoints;
    LLJoint* mJ[NJ] = {};
    LLPointer<LLJointState> mS[NJ];
    LLVRBodyArmsLayer mArms;   // #1584: collars, arms, neck, head
    LLVRBodyFingersLayer mFingers;   // #1587: the Bento fingers
    // #1582 the VMC skeleton's joints (slvmc::SJoint order; the ones above and the arms layer's
    // shared -- one joint state per joint in a motion -- neck / head none).
    LLJoint* mFJ[slvmc::S_COUNT] = {};
    LLPointer<LLJointState> mFS[slvmc::S_COUNT];
    // #1592 the body mapping: this frame's retarget frames, each joint's role and its parent's
    // (LLVRBodyMap::roleOf at init: joint names only change with a new motion).
    slbmap::Frames mF;
    int mRole[NJ] = {}, mParentRole[NJ] = {};
    int mFRole[slvmc::S_COUNT] = {}, mFParentRole[slvmc::S_COUNT] = {};
    std::vector<int> mAllRole;
    std::vector<char> mAllOwn;
    fb::Q frameR(int role) const { return role >= 0 ? mF.R[role] : fb::Q(); }
    // A solved local in the virtual skeleton onto the avatar's joint.
    void set(J j, const fb::Q& local, F32 w)
    {
        if (!mS[j]) return;
        mS[j]->setRotation(toLLQ(slbmap::actualLocal(frameR(mParentRole[j]), frameR(mRole[j]), local)));
        mS[j]->setWeight(w);
    }
    void finishMapped();
    void solveLayer();
    void reportShare();
    // #1584 fly cam C: every state once (the arms layer's and VMC's share some), and the pose held
    // when the freeze began, to ease back from.
    struct Snap
    {
        LLQuaternion rot;
        LLVector3 pos;
        F32 weight = 0.f;
    };
    std::vector<LLPointer<LLJointState>> mAll;
    std::vector<Snap> mSnap;
    bool mHaveSnap = false;
    void snapshot();
    void easeFromSnapshot(F32 a);
    bool solveFull(const LLVRBody::Targets& t, F32 base, const fb::V3& rootP, const fb::Q& rootQ, LLJoint* root,
                   const Skeleton& sk, const fb::V3& neckT);
};

LLMotion::LLMotionInitStatus LLVRBodyMotion::onInitialize(LLCharacter* character)
{
    // Never STATUS_FAILURE (the motion registry is shared: a failure marks the id bad for every
    // character). A skeleton this cannot drive gets a motion that does nothing.
    if (!character) return STATUS_SUCCESS;
    mChar = character;
    mJoints.lookup(character);
    // #1592: the mapped joints (the Bento spine joints by name: they sit between the mapped ones).
    namespace sb = slbmap;
    static const int kRoles[NJ] = {sb::R_PELVIS, -1, -1, sb::R_SPINE, -1, -1, sb::R_CHEST,
                                   sb::R_L_THIGH, sb::R_L_SHIN, sb::R_L_FOOT, sb::R_R_THIGH, sb::R_R_SHIN, sb::R_R_FOOT};
    static const char* const kSpine[NJ] = {nullptr, "mSpine1", "mSpine2", nullptr, "mSpine3", "mSpine4", nullptr,
                                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < NJ; ++i)
    {
        mJ[i] = kRoles[i] >= 0 ? LLVRBodyMap::jointFor(character, kRoles[i]) : character->getJoint(kSpine[i]);
        mRole[i] = mJ[i] ? LLVRBodyMap::roleOf(mJ[i]) : -1;
        mParentRole[i] = mJ[i] ? LLVRBodyMap::roleOf(mJ[i]->getParent()) : -1;
        if (!mJ[i]) continue;
        mS[i] = new LLJointState;
        mS[i]->setJoint(mJ[i]);
        mS[i]->setUsage(i == PELVIS ? (LLJointState::POS | LLJointState::ROT) : LLJointState::ROT);
        mS[i]->setWeight(0.f);
        addJointState(mS[i]);
    }
    mArms.init(character);
    for (const LLPointer<LLJointState>& s : mArms.states()) addJointState(s);
    mFingers.init(character);
    for (const LLPointer<LLJointState>& s : mFingers.states()) addJointState(s);
    auto keep = [&](const LLPointer<LLJointState>& st) {
        if (st.notNull() && std::find(mAll.begin(), mAll.end(), st) == mAll.end()) mAll.push_back(st);
    };
    for (const LLPointer<LLJointState>& st : mS) keep(st);
    for (const LLPointer<LLJointState>& st : mArms.states()) keep(st);
    for (const LLPointer<LLJointState>& st : mFingers.states()) keep(st);
    for (int j = 0; j < slvmc::S_COUNT; ++j)
    {
        if (j == slvmc::S_NECK || j == slvmc::S_HEAD) continue;   // (the headset's: #1121 / #1584)
        const char* name = slvmc::sjointInfo(j).name;
        // #1592: a finger joint the mapping makes a hoof (the Open Pony's mHandRing1) is the arms
        // layer's: VMC's finger data never bends it.
        if (LLJoint* hj = character->getJoint(name); hj && slbmap::isHoof(LLVRBodyMap::roleOf(hj))) continue;
        for (int i = 0; i < NJ; ++i)
            if (mJ[i] && mJ[i]->getName() == name)
            {
                mFJ[j] = mJ[i];
                mFS[j] = mS[i];
            }
        if (!mFS[j]) mFS[j] = mArms.stateFor(name);      // #1584: collars / arms
        if (!mFS[j]) mFS[j] = mFingers.stateFor(name);   // #1587: fingers
        if (mFS[j])
        {
            mFJ[j] = mFS[j]->getJoint();
            continue;
        }
        mFJ[j] = character->getJoint(name);
        if (!mFJ[j]) continue;
        mFS[j] = new LLJointState;
        mFS[j]->setJoint(mFJ[j]);
        mFS[j]->setUsage(LLJointState::ROT);
        mFS[j]->setWeight(0.f);
        addJointState(mFS[j]);
    }
    for (const LLPointer<LLJointState>& st : mFS) keep(st);
    for (int j = 0; j < slvmc::S_COUNT; ++j)
    {
        mFRole[j] = mFJ[j] ? LLVRBodyMap::roleOf(mFJ[j]) : -1;
        mFParentRole[j] = mFJ[j] ? LLVRBodyMap::roleOf(mFJ[j]->getParent()) : -1;
    }
    for (const LLPointer<LLJointState>& st : mAll)
    {
        bool own = false;
        mAllRole.push_back(LLVRBodyMap::roleOf(const_cast<LLJoint*>(st->getJoint()), &own));
        mAllOwn.push_back(own ? 1 : 0);
    }
    return STATUS_SUCCESS;
}

bool LLVRBodyMotion::onUpdate(F32 /*time*/, U8* /*joint_mask*/)
{
    // #1584 fly cam C: the states keep the pose they had (nothing solved, nothing zeroed).
    if (LLVRBody::poseHeld())
    {
        if (!mHaveSnap) snapshot();
        reportShare();
        return true;
    }
    solveLayer();
    finishMapped();   // #1592
    const F32 a = LLVRBody::liveBlend();
    if (mHaveSnap && a < 1.f) easeFromSnapshot(a);
    else mHaveSnap = false;
    reportShare();
    return true;
}

void LLVRBodyMotion::snapshot()
{
    mSnap.resize(mAll.size());
    for (size_t i = 0; i < mAll.size(); ++i)
    {
        mSnap[i].rot = mAll[i]->getRotation();
        mSnap[i].pos = mAll[i]->getPosition();
        mSnap[i].weight = mAll[i]->getWeight();
    }
    mHaveSnap = true;
}

// #1592: every joint state takes its role's state (tracked / animation / off) and part weight;
// an upright profile holds its stance where tracking has nothing.
void LLVRBodyMotion::finishMapped()
{
    const F32 base = getPose()->getWeight();
    const bool pelvisSolved = mS[PELVIS].notNull() && mS[PELVIS]->getWeight() > 0.f;
    for (size_t i = 0; i < mAll.size() && i < mAllRole.size(); ++i)
        if (mAllRole[i] >= 0) LLVRBodyMap::finish(mAll[i].get(), mAllRole[i], mAllOwn[i] != 0, mF, base);
    // A held pelvis stays where it is drawn (its position is not part of the stance).
    if (!pelvisSolved && mS[PELVIS].notNull() && mS[PELVIS]->getWeight() > 0.f && mJ[PELVIS])
        mS[PELVIS]->setPosition(mJ[PELVIS]->getPosition());
}

// Leaving fly cam C: from the held pose to this frame's live one (a 0 -> 1).
void LLVRBodyMotion::easeFromSnapshot(F32 a)
{
    a = llclamp(a, 0.f, 1.f);
    for (size_t i = 0; i < mAll.size() && i < mSnap.size(); ++i)
    {
        LLJointState* st = mAll[i].get();
        const Snap& sn = mSnap[i];
        const F32 lw = st->getWeight();
        if (sn.weight <= 0.f && lw <= 0.f) continue;
        // A bone the live pose does not drive keeps the held rotation while its weight fades.
        const LLQuaternion live = lw > 0.f ? st->getRotation() : sn.rot;
        st->setRotation(slerp(a, sn.rot, live));
        if (st->getUsage() & LLJointState::POS)
            st->setPosition(lerp(sn.pos, lw > 0.f ? st->getPosition() : sn.pos, a));
        st->setWeight(sn.weight + (lw - sn.weight) * a);
    }
}

// #1584: the bones this layer drives this frame, for the body share (others' viewers).
void LLVRBodyMotion::reportShare()
{
    if (!LLVRBodyShare::wanted()) return;
    LLVRBodyShare::beginLayer();
    for (const LLPointer<LLJointState>& s : mS) LLVRBodyShare::addState(s.get());
    for (const LLPointer<LLJointState>& s : mFS) LLVRBodyShare::addState(s.get());   // (shared ones again: harmless)
    for (const LLPointer<LLJointState>& s : mArms.states()) LLVRBodyShare::addState(s.get());
    for (const LLPointer<LLJointState>& s : mFingers.states()) LLVRBodyShare::addState(s.get());
    LLVRBodyShare::endLayer();
}

void LLVRBodyMotion::solveLayer()
{
    for (LLPointer<LLJointState>& s : mS)
        if (s) s->setWeight(0.f);
    for (LLPointer<LLJointState>& s : mFS)
        if (s) s->setWeight(0.f);
    mArms.clearWeights();
    mFingers.clearWeights();
    const F32 base = getPose()->getWeight();
    const LLVRBody::Targets& t = LLVRBody::targets();
    LLVRBodyMap::frames(mChar, mF, nullptr, LLVRBody::upright());   // #1592
    mArms.setFrames(&mF);
    Skeleton sk;
    LLJoint* root = mJ[PELVIS] ? mJ[PELVIS]->getParent() : nullptr;
    if (!t.on || !root || base <= 0.f || !measure(mChar, mJoints, sk, &mF))
    {
        LLVRBody::noteSolved(LLQuaternion::DEFAULT, 0.f, false);
        mArms.solve(base, nullptr, nullptr, nullptr);   // #1584: arms / neck from the drawn chest
        mFingers.solve(base, nullptr);                  // #1587
        return;
    }
    // #1584 x #1582: VMC's arm guides the arm IK while that hand is on a controller; without one
    // VMC drives the arm itself (solveFull) and the IK stays out.
    LLVRBodyArmsLayer::Guide guide[2];
    if (t.full)
    {
        for (int side = 0; side < 2; ++side)
        {
            const int collarJ = side ? slvmc::S_R_COLLAR : slvmc::S_L_COLLAR;
            const int upperJ = side ? slvmc::S_R_SHOULDER : slvmc::S_L_SHOULDER;
            guide[side].owned = t.fullArm[side];
            if (!t.fullArm[side] && t.fullHave[collarJ] && t.fullHave[upperJ])
            {
                guide[side].weight = llclamp(t.fullWeight, 0.f, 1.f);
                guide[side].collar = t.fullRot[collarJ];
                guide[side].upperArm = t.fullRot[upperJ];
            }
        }
    }
    // The root (mRoot: no parent, its local transform is its world one, current this frame).
    const fb::V3 rootP = toV(root->getParent() ? root->getWorldPosition() : root->getPosition());
    const fb::Q rootQ = toQ(root->getParent() ? root->getWorldRotation() : root->getRotation());
    const fb::Q rootInv = fb::qconj(rootQ);
    const fb::V3 fwdX(1.f, 0.f, 0.f);

    // The neck under this frame's eye point.
    const fb::V3 eye = toV(t.eye);
    const fb::V3 neckT = eye - fb::qrot(fb::qyaw(t.headYaw), sk.neckToEye);
    if (t.full && solveFull(t, base, rootP, rootQ, root, sk, neckT))   // #1582 VMC skeleton
    {
        const LLQuaternion chestQ = t.fullRot[slvmc::S_CHEST];
        mArms.solve(base, nullptr, t.fullHave[slvmc::S_CHEST] ? &chestQ : nullptr, guide);
        const bool vmcHands[2] = {guide[0].owned, guide[1].owned};   // (VMC drives those fingers)
        mFingers.solve(base, vmcHands);
        return;
    }

    // ---- Hips: the waist tracker, else under the neck facing the body (the avatar's facing,
    // turned toward the feet's), blended by the waist tracker's fade.
    fb::V3 bodyFwd = fb::qrot(rootQ, fwdX);
    bodyFwd.z = 0.f;
    bodyFwd = fb::normalized(bodyFwd);
    for (int b : {fb::B_LFOOT, fb::B_RFOOT})
    {
        if (!t.have[b]) continue;
        fb::V3 f = fb::qrot(toQ(t.rot[b]), fwdX);
        f.z = 0.f;
        bodyFwd = bodyFwd + fb::normalized(f, fb::V3()) * t.weight[b];
    }
    const F32 bodyYaw = std::atan2(bodyFwd.y, bodyFwd.x);
    fb::V3 pP = fb::estimatePelvis(neckT, bodyYaw, sk.pelvisToNeck);
    fb::Q pQ = fb::qyaw(bodyYaw);
    if (t.have[fb::B_WAIST] && t.waistBlend > 0.f)
    {
        const F32 a = llclamp(t.waistBlend, 0.f, 1.f);
        pP = fb::lerp(pP, toV(t.pos[fb::B_WAIST]), a);
        pQ = fb::qslerp(pQ, toQ(t.rot[fb::B_WAIST]), a);
    }
    const F32 wH = llclamp(t.hipsWeight, 0.f, 1.f);
    if (mS[PELVIS] && wH > 0.f)
    {
        fb::V3 lp = fb::qrot(rootInv, pP - rootP);
        if (root->getXform()->getScaleChildOffset())
        {
            const LLVector3& sc = root->getScale();
            if (sc.mV[VX] > 1e-4f && sc.mV[VY] > 1e-4f && sc.mV[VZ] > 1e-4f)
                lp = fb::V3(lp.x / sc.mV[VX], lp.y / sc.mV[VY], lp.z / sc.mV[VZ]);
        }
        set(PELVIS, fb::qmul(rootInv, pQ), base * wH);
        mS[PELVIS]->setPosition(toLL(lp));
    }
    // The hips the spine and legs hang from: ours when we fully own them, else where they
    // were drawn last frame (the animation's, or our blend with it).
    fb::V3 eP = pP;
    fb::Q eQ = pQ;
    if (wH < 0.999f)
    {
        eP = toV(mJ[PELVIS]->getWorldPosition());
        eQ = slbmap::virtualWorld(toQ(mJ[PELVIS]->getWorldRotation()), frameR(mRole[PELVIS]));   // #1592
    }

    // ---- Spine: toward the chest tracker, else bent toward the neck and twisted toward the
    // head. The Bento spine joints are held straight so the chain is exactly the solved one.
    const F32 wS = llclamp(t.spineWeight, 0.f, 1.f);
    fb::Q chestW = eQ;
    fb::V3 chestP;
    bool chestSolved = false;   // #1584: the arms / neck hang from this chest
    if (wS > 0.f && mS[TORSO] && mS[CHEST])
    {
        fb::Q torsoW;
        if (t.have[fb::B_CHEST] && t.weight[fb::B_CHEST] > 0.f)
        {
            fb::SpinePose sp = fb::solveSpine(eP, eQ, sk.spine, neckT, t.headYaw, 0.5f);
            chestW = fb::qslerp(sp.chestWorld, toQ(t.rot[fb::B_CHEST]), llclamp(t.weight[fb::B_CHEST], 0.f, 1.f));
            torsoW = fb::qslerp(eQ, chestW, 0.5f);
        }
        else
        {
            const fb::SpinePose sp = fb::solveSpine(eP, eQ, sk.spine, neckT, t.headYaw, 0.5f);
            torsoW = sp.torsoWorld;
            chestW = sp.chestWorld;
        }
        for (J j : {SPINE1, SPINE2, SPINE3, SPINE4}) set(j, fb::Q(), base * wS);
        set(TORSO, fb::qmul(fb::qconj(eQ), torsoW), base * wS);
        set(CHEST, fb::qmul(fb::qconj(torsoW), chestW), base * wS);
        chestP = eP + fb::qrot(eQ, sk.spine.torso) + fb::qrot(torsoW, sk.chestInTorso);
        chestSolved = wS >= 0.999f;   // (a partial weight mixes the animation in: read the drawn one)
    }
    LLVRBody::noteSolved(toLLQ(chestW), base * wS, wH > 0.f || wS > 0.f);

    // ---- Legs: two-bone IK onto the feet, the knees toward the knee trackers (else forward:
    // between the foot's and the hips' facing, half a metre in front of the leg).
    for (int side = 0; side < 2; ++side)
    {
        const int footB = side ? fb::B_RFOOT : fb::B_LFOOT;
        const int kneeB = side ? fb::B_RKNEE : fb::B_LKNEE;
        const J hipJ = side ? HIP_R : HIP_L, kneeJ = side ? KNEE_R : KNEE_L, ankleJ = side ? ANKLE_R : ANKLE_L;
        const F32 w = llclamp(t.weight[footB], 0.f, 1.f);
        if (!t.have[footB] || w <= 0.f || !mS[hipJ] || !mS[kneeJ] || !mS[ankleJ]) continue;
        const fb::V3 target = toV(t.pos[footB]);
        const fb::Q footQ = toQ(t.rot[footB]);
        const fb::V3 hipP = eP + fb::qrot(eQ, sk.leg[side].hip);
        // Forward (between the foot's and the hips' facing, half a metre in front of the leg);
        // a knee / thigh tracker steers it once the knee is really bent (stablePole: on a straight
        // leg the tracker lies on the hip -> ankle line, and its noise would flip the knee).
        fb::V3 f = fb::qrot(footQ, fwdX) + fb::qrot(eQ, fwdX);
        f.z = 0.f;
        fb::V3 pole = (hipP + target) * 0.5f + fb::normalized(f, fb::qrot(eQ, fwdX)) * 0.5f;
        if (t.have[kneeB] && t.weight[kneeB] > 0.f)
            pole = fb::lerp(pole, fb::stablePole(hipP, target, toV(t.pos[kneeB]), pole), llclamp(t.weight[kneeB], 0.f, 1.f));
        const fb::LegPose lp = fb::solveLeg(eP, eQ, sk.leg[side], target, pole);
        if (!lp.ok) continue;
        set(hipJ, fb::qmul(fb::qconj(eQ), lp.hipWorld), base * w);
        set(kneeJ, fb::qmul(fb::qconj(lp.hipWorld), lp.kneeWorld), base * w);
        set(ankleJ, fb::qmul(fb::qconj(lp.kneeWorld), footQ), base * w);
    }
    // #1584: arms and neck from this frame's chest when the spine is fully ours.
    const LLVector3 cP = toLL(chestP);
    const LLQuaternion cQ = toLLQ(chestW);
    mArms.solve(base, chestSolved ? &cP : nullptr, chestSolved ? &cQ : nullptr, t.full ? guide : nullptr);
    const bool vmcHands[2] = {guide[0].owned, guide[1].owned};
    mFingers.solve(base, vmcHands);   // #1587
    return;
}

// #1582: the VMC skeleton's world rotations straight onto the joints. The pelvis goes where the
// neck under this frame's eye point hangs it (forward kinematics through the avatar's own spine
// offsets with the solved rotations), raised if an ankle would sink below the floor. False:
// nothing could be driven (the roles path runs instead).
bool LLVRBodyMotion::solveFull(const LLVRBody::Targets& t, F32 base, const fb::V3& rootP, const fb::Q& rootQ, LLJoint* root,
                               const Skeleton& sk, const fb::V3& neckT)
{
    using namespace slvmc;
    if (!mFS[S_PELVIS] || !mJoints.neck) return false;
    const F32 w = base * llclamp(t.fullWeight, 0.f, 1.f);
    if (w <= 0.f) return false;
    fb::Q W[S_COUNT];
    bool drive[S_COUNT];
    for (int j = 0; j < S_COUNT; ++j)
    {
        drive[j] = t.fullHave[j] && mFS[j].notNull();
        W[j] = toQ(t.fullRot[j]);
    }
    for (int side = 0; side < 2; ++side)
    {
        if (t.fullArm[side]) continue;
        // The controller owns this arm (and its fingers' parent): leave them to it.
        for (int j = 0; j < S_COUNT; ++j)
            if ((isArmJoint(j) || isFingerJoint(j)) && isLeftSide(j) == (side == 0)) drive[j] = false;
    }
    // The world rotation a joint hangs from: SlimeVR's where it sent one (even for a joint whose
    // state we leave to another path: the FK wants the real pose), else as drawn last frame.
    // (#1592: all in the virtual skeleton: drawn rotations and offsets through the frames.)
    auto worldOf = [&](LLJoint* jt) -> fb::Q {
        for (int j = 0; j < S_COUNT; ++j)
            if (mFJ[j] == jt && (drive[j] || t.fullHave[j])) return W[j];
        return slbmap::virtualWorld(toQ(jt->getWorldRotation()), toQ(LLVRBodyMap::frameOf(mF, jt)));
    };
    // Pelvis -> neck through the avatar's joints (Bento spine included).
    fb::V3 rel;
    for (LLJoint* jt = mJoints.neck; jt && jt != mFJ[S_PELVIS]; jt = jt->getParent())
    {
        LLJoint* parent = jt->getParent();
        if (!parent) return false;
        rel = rel + fb::qrot(worldOf(parent), slbmap::virtualOffset(toQ(LLVRBodyMap::frameOf(mF, parent)), offsetIn(jt)));
    }
    fb::V3 pelvisP = neckT - rel;
    // No ankle below the floor.
    const F32 restAnkle = sk.pelvisToFoot + sk.leg[0].hip.z + sk.leg[0].knee.z + sk.leg[0].ankle.z;
    F32 lowest = 1e9f;
    for (int side = 0; side < 2; ++side)
    {
        const int hipJ = side ? S_R_HIP : S_L_HIP, kneeJ = side ? S_R_KNEE : S_L_KNEE;
        if (!t.fullHave[hipJ] || !t.fullHave[kneeJ]) continue;
        const fb::LegRest& lr = sk.leg[side];
        const fb::V3 hp = pelvisP + fb::qrot(W[S_PELVIS], lr.hip);
        const fb::V3 kp = hp + fb::qrot(W[hipJ], lr.knee);
        const fb::V3 ap = kp + fb::qrot(W[kneeJ], lr.ankle);
        lowest = llmin(lowest, ap.z);
    }
    const F32 minAnkle = t.floorZ + llmax(restAnkle, 0.f);
    if (lowest < minAnkle) pelvisP.z += minAnkle - lowest;

    const fb::Q rootInv = fb::qconj(rootQ);
    fb::V3 lp = fb::qrot(rootInv, pelvisP - rootP);
    if (root->getXform()->getScaleChildOffset())
    {
        const LLVector3& sc = root->getScale();
        if (sc.mV[VX] > 1e-4f && sc.mV[VY] > 1e-4f && sc.mV[VZ] > 1e-4f)
            lp = fb::V3(lp.x / sc.mV[VX], lp.y / sc.mV[VY], lp.z / sc.mV[VZ]);
    }
    mFS[S_PELVIS]->setPosition(toLL(lp));
    for (int j = 0; j < S_COUNT; ++j)
    {
        if (!drive[j]) continue;
        const int p = sjointInfo(j).parent;
        // A parent we do not drive (an old skeleton without Bento spine joints): its drawn rotation.
        const fb::Q pw = p < 0 ? rootQ : (t.fullHave[p] && mFS[p].notNull() ? W[p] : (mFJ[p] ? worldOf(mFJ[p]) : fb::Q()));
        mFS[j]->setRotation(toLLQ(slbmap::actualLocal(frameR(mFParentRole[j]), frameR(mFRole[j]), fb::qmul(fb::qconj(pw), W[j]))));
        mFS[j]->setWeight(w);
    }
    LLVRBody::noteSolved(toLLQ(W[S_CHEST]), w, true);
    return true;
}

// ---- Per-session state -----------------------------------------------------------------------
struct Bone
{
    bool smoothValid = false;
    fb::V3 pos;                 // smoothed tracker pose, body space
    fb::Q rot;
    F32 fade = 0.f;
    bool present = false;       // for the lost / back log
    Clock::time_point lastLog{};
    U32 suppressed = 0;
};
struct Calibration
{
    bool valid = false;
    bool floorSpace = true;
    int source = LLVRBodySource::OPENXR;   // #1582: the body source it was taken from
    F32 floorY = 0.f;           // the floor's app-space y (0 in LOCAL_FLOOR / STAGE)
    F32 eyeH = 1.6f;            // your eye height (m) in the T-pose
    int slot[fb::B_COUNT] = {-1, -1, -1, -1, -1, -1};   // LLXrTrackerRole per bone
    fb::Offset off[fb::B_COUNT];
};
struct BodyState
{
    bool engaged = false;       // trackers seen this session: update() runs past its first test
    U32 generation = UINT32_MAX;
    bool savedTried = false;
    bool askedToCalibrate = false;
    Calibration cal;
    Bone bone[fb::B_COUNT];
    F32 countdown = 0.f;
    bool motionOn = false;
    LLQuaternion chest;
    F32 chestWeight = 0.f;
    F32 fullFade = 0.f;         // #1582 the VMC skeleton's fade in / out
    F32 uprightFade = 0.f;      // #1592 an upright profile's stance (the hips / spine held up)
    F32 standEyeH = 0.f;        // the driven body's standing eye height above the soles (the camera anchor)
    U32 mapJointGen = 0;        // #1592 the mapping's joints the motion was made for
    bool skeleton = false;      // the last frame was a whole skeleton (the Body countdown's text)
    LLVRBodySource::Kind source = LLVRBodySource::NONE;   // the body source of the last frame with a body
    fb::FlyState fly;           // #1584 fly cam mode (D live / C frozen)
    bool flyingLogged = false;  // #1584: the avatar flies (hips / legs to the animation), last logged
    LLVRBodyMapping heldMap;    // #1584: the mapping from before fly cam (the avatar stays there)
};
BodyState sB;

// The eye height a bone-pose source (headset body tracking) maps your body with until you
// measure it (wrist "Body"): the last one measured, else SlipstreamVREyeHeight.
F32 savedBoneEyeHeight()
{
    const LLSD sd = gSavedSettings.getLLSD("SlipstreamVRBodyCalibration");
    const F32 h = sd.isMap() && sd.has("headset_eye_h") ? (F32)sd["headset_eye_h"].asReal() : 0.f;
    if (h >= 0.8f && h <= 2.5f) return h;
    static LLCachedControl<F32> s_eye_height(gSavedSettings, "SlipstreamVREyeHeight", 1.6f);
    return llclamp((F32)s_eye_height, 0.8f, 2.5f);
}

// A bone-pose source needs no offsets: every role it has, identity, at the saved eye height.
void calibrateBonePoses(const LLXrTrackers& tr, const LLXrHeadPose& hp, F32 eyeH, const char* why)
{
    Calibration c;
    c.source = LLVRBodySource::active();
    c.floorSpace = hp.floorSpace;
    c.eyeH = eyeH;
    c.floorY = hp.floorSpace ? 0.f : hp.position[1] - eyeH;
    const uint32_t slot[fb::B_COUNT] = {LLXR_TRK_WAIST,     LLXR_TRK_CHEST,     LLXR_TRK_LEFT_FOOT,
                                        LLXR_TRK_RIGHT_FOOT, LLXR_TRK_LEFT_KNEE, LLXR_TRK_RIGHT_KNEE};
    std::string roles;
    for (int b = 0; b < fb::B_COUNT; ++b)
    {
        if (!tr.role[slot[b]].active) continue;
        c.slot[b] = (int)slot[b];
        roles += (roles.empty() ? "" : ", ") + std::string(fb::bodyName(b));
    }
    if (roles.empty()) return;
    c.valid = true;
    sB.cal = c;
    for (Bone& bn : sB.bone) bn.smoothValid = false;
    LL_INFOS("VRBody") << LLVRBodySource::kindName(LLVRBodySource::active()) << " mapped as bones (" << why << "): " << roles << llformat("; eye height %.2f m", eyeH)
                       << (c.floorSpace ? "" : " (LOCAL space: floor from the eye height)") << LL_ENDL;
}

const char* roleName(int slot) { return slot >= 0 ? llxrTrackerRoleName((uint32_t)slot) : "-"; }

void logTrackers(const LLXrTrackers& tr)
{
    std::string roles;
    U32 active = 0;
    for (uint32_t i = 0; i < LLXR_TRK_COUNT; ++i)
    {
        if (!tr.role[i].active) continue;
        ++active;
        roles += (roles.empty() ? "" : ", ") + std::string(llxrTrackerRoleName(i));
    }
    LL_INFOS("VRBody") << "body trackers (" << LLVRBodySource::kindName(LLVRBodySource::active()) << "): " << tr.connected << " listed, " << active << " bound ("
                       << (roles.empty() ? std::string("none yet") : roles) << ")"
                       << (tr.withoutRole ? ", " + std::to_string(tr.withoutRole) + " without a role (unusable)" : std::string())
                       << (sB.cal.valid ? "; calibrated" : "; not calibrated: wrist panel > Body") << LL_ENDL;
}

void saveCalibration()
{
    const Calibration& c = sB.cal;
    LLSD sd;
    sd["version"] = 1;
    sd["floor_space"] = c.floorSpace;
    sd["source"] = LLVRBodySource::kindName((LLVRBodySource::Kind)c.source);
    sd["eye_h"] = (LLSD::Real)c.eyeH;
    LLSD bones = LLSD::emptyArray();
    for (int b = 0; b < fb::B_COUNT; ++b)
    {
        if (c.slot[b] < 0) continue;
        LLSD e;
        e["bone"] = b;
        e["role"] = roleName(c.slot[b]);
        const fb::Offset& o = c.off[b];
        LLSD p = LLSD::emptyArray(), q = LLSD::emptyArray();
        for (F32 v : {o.pos.x, o.pos.y, o.pos.z}) p.append((LLSD::Real)v);
        for (F32 v : {o.rot.x, o.rot.y, o.rot.z, o.rot.w}) q.append((LLSD::Real)v);
        e["p"] = p;
        e["q"] = q;
        bones.append(e);
    }
    sd["bones"] = bones;
    const LLSD old = gSavedSettings.getLLSD("SlipstreamVRBodyCalibration");
    if (old.isMap() && old.has("headset_eye_h")) sd["headset_eye_h"] = old["headset_eye_h"];   // #1583
    gSavedSettings.setLLSD("SlipstreamVRBodyCalibration", sd);
}

// The last session's calibration, if it was taken over a floor (a LOCAL space's floor is per
// session) and its roles exist.
void loadCalibration(const LLXrHeadPose& hp)
{
    sB.savedTried = true;
    const LLSD sd = gSavedSettings.getLLSD("SlipstreamVRBodyCalibration");
    if (!sd.isMap() || sd["version"].asInteger() != 1 || !sd["floor_space"].asBoolean() || !hp.floorSpace) return;
    // #1582: the same role names, other trackers (a calibration without a source is OpenXR's).
    const LLVRBodySource::Kind src = LLVRBodySource::active();
    const std::string savedSrc = sd.has("source") ? sd["source"].asString() : LLVRBodySource::kindName(LLVRBodySource::OPENXR);
    if (savedSrc != LLVRBodySource::kindName(src)) return;
    Calibration c;
    c.source = src;
    c.floorSpace = true;
    c.eyeH = (F32)sd["eye_h"].asReal();
    if (!(c.eyeH >= 0.8f && c.eyeH <= 2.5f)) return;
    std::string roles;
    for (LLSD::array_const_iterator it = sd["bones"].beginArray(); it != sd["bones"].endArray(); ++it)
    {
        const LLSD& e = *it;
        const int b = e["bone"].asInteger();
        if (b < 0 || b >= fb::B_COUNT || e["p"].size() != 3 || e["q"].size() != 4) continue;
        int slot = -1;
        for (uint32_t i = 0; i < LLXR_TRK_COUNT; ++i)
            if (e["role"].asString() == llxrTrackerRoleName(i)) slot = (int)i;
        if (slot < 0) continue;
        c.slot[b] = slot;
        c.off[b].pos = fb::V3((F32)e["p"][0].asReal(), (F32)e["p"][1].asReal(), (F32)e["p"][2].asReal());
        c.off[b].rot = fb::qnormalized(fb::Q((F32)e["q"][0].asReal(), (F32)e["q"][1].asReal(), (F32)e["q"][2].asReal(),
                                             (F32)e["q"][3].asReal()));
        roles += (roles.empty() ? "" : ", ") + std::string(fb::bodyName(b)) + "=" + roleName(slot);
    }
    if (roles.empty()) return;
    c.valid = true;
    sB.cal = c;
    LL_INFOS("VRBody") << "using the saved calibration (" << roles << ", eye height " << c.eyeH
                       << " m); recalibrate from the wrist panel if the trackers moved" << LL_ENDL;
}

void calibrate(const LLXrTrackers& tr, const LLXrHeadPose& hp, LLVOAvatarSelf* av, bool bonePoses)
{
    Skeleton sk;
    if (!measureAvatar(av, sk))
    {
        LL_WARNS("VRBody") << "calibration: this avatar's skeleton has no hips / legs / spine to drive" << LL_ENDL;
        return;
    }
    static LLCachedControl<F32> s_eye_height(gSavedSettings, "SlipstreamVREyeHeight", 1.6f);
    static LLCachedControl<bool> s_by_pos(gSavedSettings, "SlipstreamVRBodyAssignByPosition", false);
    Calibration c;
    c.source = LLVRBodySource::active();
    c.floorSpace = hp.floorSpace;
    c.floorY = hp.floorSpace ? 0.f : hp.position[1] - llclamp((F32)s_eye_height, 0.5f, 2.5f);
    const fb::V3 hmd = xrPosToBody(hp.position, c.floorY);
    c.eyeH = hmd.z;
    if (!hp.valid || !(c.eyeH >= 0.8f && c.eyeH <= 2.5f))
    {
        LL_WARNS("VRBody") << "calibration refused: the headset is " << c.eyeH
                           << " m above the floor (stand up straight; a height between 0.8 and 2.5 m)" << LL_ENDL;
        return;
    }
    if (bonePoses)
    {
        // #1583: the poses are the bones; only your eye height is measured (kept for next time).
        calibrateBonePoses(tr, hp, c.eyeH, "measured");
        LLSD sd = gSavedSettings.getLLSD("SlipstreamVRBodyCalibration");
        if (!sd.isMap()) sd = LLSD::emptyMap();
        sd["headset_eye_h"] = (LLSD::Real)c.eyeH;
        gSavedSettings.setLLSD("SlipstreamVRBodyCalibration", sd);
        // (Your height as measured now is the view's too: the avatar's eyes at the camera.)
        if (hp.floorSpace) LLVRCamera::setReferenceHeight(c.eyeH);
        LL_INFOS("VRBody") << LLVRBodySource::kindName(LLVRBodySource::active())
                           << llformat(" calibrated: eye height %.2f m, avatar %.2f m (scale %.2f)", c.eyeH,
                                       sk.eyeH, sk.eyeH / c.eyeH)
                           << LL_ENDL;
        LLXrSession& xr = LLXrSession::instance();
        for (uint32_t h = 0; h < LLXR_HAND_COUNT; ++h) xr.haptic(h, 0.6f, 0.12f);
        return;
    }
    const fb::Q headQ = xrRotToBody(hp.orientation);
    const F32 yaw = fb::yawOf(headQ);
    // #1592: under an upright profile the foot joint is a hock far above the floor; the foot
    // tracker is calibrated onto a human ankle and LLVRBody::update lifts the target straight up.
    fb::Proportions calPr = sk.pr;
    if (LLVRBody::upright()) calPr.ankle = 0.055f;
    auto ok = [&](uint32_t r) { return tr.role[r].valid; };
    // Roles from the runtime (the first located of each bone's roles).
    auto pick = [&](uint32_t a, uint32_t b) -> int { return ok(a) ? (int)a : (ok(b) ? (int)b : -1); };
    c.slot[fb::B_WAIST] = pick(LLXR_TRK_WAIST, LLXR_TRK_WAIST);
    c.slot[fb::B_CHEST] = pick(LLXR_TRK_CHEST, LLXR_TRK_CHEST);
    c.slot[fb::B_LFOOT] = pick(LLXR_TRK_LEFT_FOOT, LLXR_TRK_LEFT_ANKLE);
    c.slot[fb::B_RFOOT] = pick(LLXR_TRK_RIGHT_FOOT, LLXR_TRK_RIGHT_ANKLE);
    c.slot[fb::B_LKNEE] = pick(LLXR_TRK_LEFT_KNEE, LLXR_TRK_LEFT_KNEE);
    c.slot[fb::B_RKNEE] = pick(LLXR_TRK_RIGHT_KNEE, LLXR_TRK_RIGHT_KNEE);
    const bool byPos = s_by_pos || (c.slot[fb::B_WAIST] < 0 && c.slot[fb::B_LFOOT] < 0 && c.slot[fb::B_RFOOT] < 0);
    if (byPos)
    {
        // Where they are: every located tracker, whatever its role.
        fb::V3 pos[LLXR_TRK_COUNT];
        int slotOf[LLXR_TRK_COUNT];
        int n = 0;
        for (uint32_t i = 0; i < LLXR_TRK_COUNT; ++i)
        {
            if (!ok(i)) continue;
            pos[n] = xrPosToBody(tr.role[i].position, c.floorY);
            slotOf[n++] = (int)i;
        }
        int out[fb::B_COUNT];
        fb::assignByPosition(pos, n, hmd, fb::qrot(headQ, fb::V3(1.f, 0.f, 0.f)), c.eyeH, calPr, out);
        for (int b = 0; b < fb::B_COUNT; ++b) c.slot[b] = out[b] >= 0 ? slotOf[out[b]] : -1;
    }
    std::string summary;
    for (int b = 0; b < fb::B_COUNT; ++b)
    {
        if (c.slot[b] < 0) continue;
        const LLXrTrackerPose& tp = tr.role[c.slot[b]];
        const fb::V3 trk = xrPosToBody(tp.position, c.floorY);
        const fb::Q trq = xrRotToBody(tp.orientation);
        fb::V3 bp;
        fb::Q bq;
        fb::tposeBone(b, trk, hmd, yaw, c.eyeH, calPr, bp, bq);
        c.off[b] = fb::calibrateOffset(trk, trq, bp, bq);
        summary += llformat("%s%s=%s (%.0f cm, %.0f deg)", summary.empty() ? "" : ", ", fb::bodyName(b), roleName(c.slot[b]),
                            fb::length(c.off[b].pos) * 100.f, fb::qangle(fb::Q(), c.off[b].rot) * RAD_TO_DEG);
    }
    if (summary.empty())
    {
        LL_WARNS("VRBody") << "calibration: no tracker could be used (" << (byPos ? "none at a foot / knee / hips / chest height"
                                                                            : "no located tracker") << ")" << LL_ENDL;
        return;
    }
    c.valid = true;
    sB.cal = c;
    for (Bone& bn : sB.bone) bn.smoothValid = false;
    saveCalibration();
    if (c.floorSpace) LLVRCamera::setReferenceHeight(c.eyeH);   // (the view's height too: userEyeHeight)
    LL_INFOS("VRBody") << "calibrated (" << (byPos ? "by position" : "by the runtime's roles") << "): " << summary
                       << llformat("; eye height %.2f m, avatar %.2f m (scale %.2f), %s", c.eyeH, sk.eyeH, sk.eyeH / c.eyeH,
                                   c.floorSpace ? "floor space" : "LOCAL space (floor from the eye height setting)")
                       << LL_ENDL;
    LLXrSession& xr = LLXrSession::instance();
    for (uint32_t h = 0; h < LLXR_HAND_COUNT; ++h) xr.haptic(h, 0.6f, 0.12f);
}

void notePresence(int b, bool present)
{
    Bone& bn = sB.bone[b];
    if (present == bn.present) return;
    bn.present = present;
    // #1586: the estimate's roles drop out on purpose while the avatar walks (it logs that once).
    if (sB.source == LLVRBodySource::ESTIMATE) return;
    const Clock::time_point now = Clock::now();
    if (bn.lastLog != Clock::time_point{} && std::chrono::duration<F32>(now - bn.lastLog).count() < LOSS_LOG_INTERVAL)
    {
        ++bn.suppressed;
        return;
    }
    LL_INFOS("VRBody") << fb::bodyName(b)
                       << (sB.source == LLVRBodySource::HEADSET ? std::string(" (headset body tracking) ")
                                                              : " tracker (" + std::string(roleName(sB.cal.slot[b])) + ") ")
                       << (present ? "back" : "lost: that limb fades back to the animation")
                       << (bn.suppressed ? llformat(" (%u more changes in the last %.0f s)", bn.suppressed, LOSS_LOG_INTERVAL)
                                         : std::string())
                       << LL_ENDL;
    bn.lastLog = now;
    bn.suppressed = 0;
}

void setMotion(bool on)
{
    if (on == sB.motionOn || !isAgentAvatarValid()) return;
    if (on)
    {
        static bool registered = false;
        if (!registered)
        {
            gAgentAvatarp->registerMotion(ANIM_VR_BODY, LLVRBodyMotion::create);   // (shared registry)
            registered = true;
        }
        gAgentAvatarp->startMotion(ANIM_VR_BODY);
        LL_INFOS("VRBody") << "body layer on (your own avatar, local only)" << LL_ENDL;
    }
    else
    {
        gAgentAvatarp->stopMotion(ANIM_VR_BODY, true);
        LL_INFOS("VRBody") << "body layer off: the animations have the body again" << LL_ENDL;
    }
    sB.motionOn = on;
}

// #1592: the mapping's joints changed: the motion's joint states are made at its initialisation,
// so drop the instance (stopped first); setMotion starts a fresh one when it is wanted again.
void remakeMotion()
{
    if (!isAgentAvatarValid()) return;
    if (sB.motionOn) setMotion(false);
    gAgentAvatarp->removeMotion(ANIM_VR_BODY);
}

void allOff()
{
    sB.fullFade = 0.f;
    for (Bone& bn : sB.bone)
    {
        bn.fade = 0.f;
        bn.smoothValid = false;
    }
    setMotion(false);
}

// #1584: your eye height for the body scale when nothing is calibrated (the arms and neck run
// without a body source): the saved one, re-read every few seconds (an LLSD read).
F32 uncalibratedEyeHeight()
{
    static F32 s_h = 0.f;
    static Clock::time_point s_read{};
    const Clock::time_point now = Clock::now();
    if (s_h <= 0.f || std::chrono::duration<F32>(now - s_read).count() > 5.f)
    {
        s_h = savedBoneEyeHeight();
        s_read = now;
    }
    return s_h;
}

// Your standing eye height the body is scaled by (the avatar's eye height over it): the VR
// camera's reference height -- the one the view maps onto the avatar's eyes -- else this
// calibration's, else the saved one. #1592 / #1584: the body used the calibration's (1.69 m in
// the owner's log) while the camera's recentres took 1.60 - 1.70 m: after a recentre the avatar's
// eyes were up to 5% of its height away from the camera.
F32 userEyeHeight()
{
    F32 h = 0.f;
    if (LLVRCamera::referenceHeight(h) && h >= 0.5f && h <= 2.5f) return h;
    return sB.cal.valid ? sB.cal.eyeH : uncalibratedEyeHeight();
}

// This frame's room -> agent mapping (the body's targets and the arms'). False: no rig mapping
// or no skeleton this frame.
bool makeMapping(const LLXrHeadPose& hp, LLVOAvatarSelf* av, const Skeleton* sk, LLVRBodyMapping& m)
{
    const Calibration& c = sB.cal;
    m = LLVRBodyMapping{};
    const F32 userEyeH = userEyeHeight();
    m.sv = sk && userEyeH > 0.5f ? sk->eyeH / userEyeH : 1.f;
    m.floorY = c.valid ? c.floorY : (hp.floorSpace ? 0.f : hp.position[1] - userEyeH);
    m.footZ = av ? av->getRenderPosition().mV[VZ] - av->getPelvisToFoot() : 0.f;
    m.fly = sB.fly.fly;
    m.headWeight = sB.fly.headLive;
    m.avatarEyeH = sk ? sk->eyeH : 0.f;   // #1592
    if (m.fly)
    {
        // #1584: fly cam moves the rig with the camera; the body stays where the avatar was left,
        // on the mapping from before (its scale / floor as now).
        m.ok = av && sk && sB.heldMap.ok;
        m.anchor = sB.heldMap.anchor;
        m.turn = sB.heldMap.turn;
        m.refX = sB.heldMap.refX;
        m.refY = sB.heldMap.refY;
        return m.ok;
    }
    m.ok = av && sk && LLVRCamera::rigMapping(m.turn, m.anchor, m.refX, m.refY);
    if (LLVRCamera::firstPerson()) sB.heldMap = m;
    return m.ok;
}

// #1584: fly cam's mode, every VR frame: D keeps the body live and releases the head, C holds the
// pose. Logged on entering / leaving.
void stepFlyCam(F32 dt)
{
    static LLCachedControl<bool> s_freeze(gSavedSettings, "SlipstreamVRFlycamFreezePose", false);
    const bool fly = LLVRCamera::flyCam() && sB.heldMap.ok;
    const fb::FlyEvents ev = fb::stepFly(sB.fly, fly, s_freeze, dt);
    if (ev.entered)
    {
        LL_INFOS("VRBody") << (sB.fly.freeze ? "fly cam on: your pose is frozen as it was (C, SlipstreamVRFlycamFreezePose), head included"
                                             : "fly cam on: the body keeps following your tracking where the avatar stands, the head is "
                                               "released to the animation (D); arms, fingers and the rest stay tracked")
                           << LL_ENDL;
        if (sB.fly.freeze) LLVRBodyShare::requestFull();   // (one keyframe of the held pose)
    }
    if (ev.left) LL_INFOS("VRBody") << "fly cam off: easing back to live tracking" << LL_ENDL;
}

// #1584: arms and neck, every VR frame (with or without a body source). In fly cam on the held
// mapping (only the head released: m.headWeight; the arms stay on the controllers).
void updateArms(F32 dt, const LLXrHeadPose& hp, bool headOk, const LLVRBodyFrame* body, LLVRBodyMapping m)
{
    m.ok = m.ok && (LLVRCamera::firstPerson() || m.fly);
    LLVRBodyArms::update(dt, hp, headOk, body, m);
    LLVRBodyFingers::update(dt, headOk, m);   // #1587
}
} // namespace

void LLVRBody::requestCalibration()
{
    if (!sAvailable) return;
    sCalibrating = true;
    sB.countdown = CALIBRATION_COUNTDOWN;
    LL_INFOS("VRBody") << "calibration in " << CALIBRATION_COUNTDOWN
                       << (sB.source == LLVRBodySource::HEADSET || sB.source == LLVRBodySource::ESTIMATE
                               ? " s: stand up straight (your eye height)"
                           : sB.skeleton ? " s: stand up straight, facing forward (your eye height and SlimeVR's heading)"
                                         : " s: stand up straight, face forward, arms out (T-pose)")
                       << LL_ENDL;
    LLXrSession& xr = LLXrSession::instance();
    for (uint32_t h = 0; h < LLXR_HAND_COUNT; ++h) xr.haptic(h, 0.3f, 0.05f);
}

void LLVRBody::reset()
{
    LLVRBodySource::stop();   // #1582 (closes SlimeVR's socket; no-op when nothing is open)
    LLVRBodyArms::reset();    // #1584
    LLVRBodyFingers::reset(); // #1587
    sPoseHeld = false;
    sLiveBlend = 1.f;
    sB.fly = fb::FlyState{};
    if (!sB.engaged)
    {
        setMotion(false);     // (the arms / neck alone may have run it)
        return;
    }
    sCalibrating = false;
    sHoldAnchor = false;
    sB.chestWeight = 0.f;
    sTargets = Targets{};
    allOff();
    sB.engaged = false;   // (the next running frame with trackers takes it up again)
    sAvailable = false;
}

bool LLVRBody::standingEyeHeight(F32& h)
{
    if ((sB.uprightFade <= 0.f && !sHoldAnchor) || sB.standEyeH <= 0.1f) return false;
    h = sB.standEyeH;
    return true;
}

void LLVRBody::noteSolved(const LLQuaternion& chest, F32 spineWeight, bool hipsDriven)
{
    sB.chest = chest;
    sB.chestWeight = spineWeight;
    sHoldAnchor = hipsDriven;
}

bool LLVRBody::lookFrame(LLQuaternion& frame)
{
    if (sB.chestWeight <= 0.f) return false;   // (no trackers: this is the whole cost)
    frame = toLLQ(fb::qslerp(toQ(frame), toQ(sB.chest), llclamp(sB.chestWeight, 0.f, 1.f)));
    return true;
}

// #1584 fly cam C: the layer holds its pose while the motion runs and you do not sit (sitting
// stops everything, as before); then eases back to live (liveBlend).
void LLVRBody::holdPose(LLVOAvatarSelf* av)
{
    sPoseHeld = sB.fly.frozen() && sB.motionOn && av && !av->isSitting();
    sLiveBlend = sB.fly.liveBlend;
}

void LLVRBody::update(F32 dt)
{
    // The body source (llvrbodysource.h; SlipstreamVRBodySource) fills this frame's poses.
    LLXrSession& xr = LLXrSession::instance();
    LLXrHeadPose hp;
    const bool headOk = xr.headPose(hp) && hp.valid;
    // #1592 the body mapping (per avatar; Auto re-detects every few seconds). New joint names need
    // a new motion (its joint states are made at its initialisation).
    LLVRBodyMap::update(dt);
    if (LLVRBodyMap::jointGeneration() != sB.mapJointGen)
    {
        sB.mapJointGen = LLVRBodyMap::jointGeneration();
        remakeMotion();
    }
    {
        LLVOAvatarSelf* self = isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
        sUpright = LLVRBodyMap::profile().upright && self && !self->isSitting() && (LLVRCamera::firstPerson() || sB.fly.fly);
    }
    LLVRBodyFrame fr;
    {
        // #1586: the estimate's body scale and floor (your standing eye height as the body maps it).
        const F32 eyeH = userEyeHeight();
        LLVRBodySource::setBodyScale(eyeH, sB.cal.valid ? sB.cal.floorY : (hp.floorSpace ? 0.f : hp.position[1] - eyeH));
        // #1584: the one body heading in VR is the avatar's own facing (LLVRCamera::bodyFollowHead
        // turns it only past SlipstreamVRBodyTurnLimit; walking turns it to the head): in the room,
        // through the last first-person mapping (agent = room turned by m.turn).
        const LLVector3 at = gAgent.getAtAxis();
        const bool yawOk = sB.heldMap.ok && (at.mV[VX] * at.mV[VX] + at.mV[VY] * at.mV[VY]) > 0.01f;
        LLVRBodySource::setBodyYaw(yawOk, yawOk ? std::atan2(at.mV[VY], at.mV[VX]) - sB.heldMap.turn : 0.f);
    }
    const bool have = LLVRBodySource::poll(hp, dt, fr);
    stepFlyCam(dt);   // #1584
    if (!sB.engaged && !have && !sUpright)
    {
        // VR without a body source: the arms / neck only (controllers and headset, #1584).
        sAvailable = false;
        LLVOAvatarSelf* av = isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
        Skeleton sk;
        const bool skOk = measureAvatar(av, sk);
        LLVRBodyMapping m;
        makeMapping(hp, av, skOk ? &sk : nullptr, m);
        updateArms(dt, hp, headOk, nullptr, m);
        holdPose(av);
        setMotion(LLVRBodyArms::on() || LLVRBodyFingers::on() || sPoseHeld);
        return;
    }
    const LLXrTrackers& tr = fr.poses;
    if (have && fr.kind != sB.source)
    {
        // Another source (Auto: trackers / SlimeVR came or went): its calibration, not the last one's.
        if (sB.source != LLVRBodySource::NONE)
            LL_INFOS("VRBody") << "body source changed: " << LLVRBodySource::kindName(sB.source) << " -> "
                               << LLVRBodySource::kindName(fr.kind) << LL_ENDL;
        sB.source = fr.kind;
        sB.cal = Calibration{};
        sB.savedTried = sB.askedToCalibrate = false;
        sB.generation = UINT32_MAX;
        for (Bone& bn : sB.bone) bn.smoothValid = false;
    }
    sB.engaged = true;
    sAvailable = have;
    if (have) sB.skeleton = fr.bonePoses && fr.kind == LLVRBodySource::SLIMEVR;
    if (have && tr.generation != sB.generation)
    {
        sB.generation = tr.generation;
        if (fr.bonePoses)
        {
            if (headOk) calibrateBonePoses(tr, hp, sB.cal.valid ? sB.cal.eyeH : savedBoneEyeHeight(), "joint set");
            else sB.generation = UINT32_MAX;   // (again once the head is located)
        }
        else
        {
            if (!sB.savedTried && !sB.cal.valid && headOk) loadCalibration(hp);
            logTrackers(tr);
        }
    }

    static LLCachedControl<bool> s_on(gSavedSettings, "SlipstreamVRBodyTracking", true);
    static LLCachedControl<F32> s_w_hips(gSavedSettings, "SlipstreamVRBodyWeightHips", 1.f);
    static LLCachedControl<F32> s_w_legs(gSavedSettings, "SlipstreamVRBodyWeightLegs", 1.f);
    static LLCachedControl<F32> s_w_spine(gSavedSettings, "SlipstreamVRBodyWeightSpine", 1.f);
    static LLCachedControl<F32> s_smooth(gSavedSettings, "SlipstreamVRBodySmoothing", 0.03f);

    LLVOAvatarSelf* av = isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;

    // The countdown (the wrist's Body button), then the T-pose.
    if (sCalibrating)
    {
        sB.countdown -= dt;
        if (!have || !headOk || !av)
        {
            sCalibrating = false;
            LL_INFOS("VRBody") << "calibration cancelled (trackers or headset gone)" << LL_ENDL;
        }
        else if (sB.countdown <= 0.f)
        {
            sCalibrating = false;
            // #1582: the source aligns its space first (SlimeVR: trackers or VMC skeleton) and
            // refreshes the poses, then the T-pose (or, for bone poses, the eye height) on them.
            const bool aligned = LLVRBodySource::calibrate(hp, fr);
            if (aligned || fr.bonePoses) calibrate(tr, hp, av, fr.bonePoses);
        }
    }
    if (have && !sB.cal.valid && !sB.askedToCalibrate)
    {
        sB.askedToCalibrate = true;
        LL_INFOS("VRBody") << "trackers found but not calibrated: wrist panel > Body, then stand in a T-pose" << LL_ENDL;
    }

    Skeleton sk;
    const bool skOk = measureAvatar(av, sk);
    // This frame's mapping (body space -> agent space).
    LLVRBodyMapping m;
    const bool mapOk = makeMapping(hp, av, skOk ? &sk : nullptr, m);
    // (#1584 fly cam: still live on the held mapping; C holds the pose in the motion instead.)
    const bool usable = have && s_on && headOk && mapOk && skOk && sB.cal.valid && !av->isSitting() &&
                        (LLVRCamera::firstPerson() || sB.fly.fly);
    // #1592 an upright profile (the pony on its hind legs) stands up with or without a body source:
    // the hips under the head, the spine up, whatever tracking adds on top.
    const bool upright = sUpright && s_on && headOk && mapOk && skOk && !gAgent.getFlying();   // (#1584: flying, the fly animation)
    sB.uprightFade = fb::fadeStep(sB.uprightFade, upright, dt);
    sB.standEyeH = skOk ? sk.eyeH : 0.f;

    Targets& t = sTargets;
    const Calibration& c = sB.cal;
    const F32 floorY = m.floorY;
    const fb::V3 hmd = xrPosToBody(hp.position, floorY);
    const fb::Q turnQ = fb::qyaw(m.turn);
    // (Not only while usable: a limb that stops being driven -- sitting, fly cam -- fades out
    // over the same mapping.)
    const bool eyeOk = headOk && mapOk && skOk;
    // From the recentre reference, never from the headset (a head sway must not move a foot).
    auto toAgent = [&](const fb::V3& b) { return toLL(fb::roomToAgent(b, toV(m.anchor), m.refX, m.refY, m.turn, m.sv, m.footZ)); };
    if (eyeOk)
    {
        t.eye = toAgent(hmd);
        t.headYaw = fb::yawOf(fb::qmul(turnQ, xrRotToBody(hp.orientation)));
    }

    const F32 tau = llclamp((F32)s_smooth, 0.f, 1.f);
    // #1592: a part the body mapping leaves to the animation is not solved at all (else the hips
    // would still hold the camera anchor and hand SLPoseLookAt a chest nobody drives). drives():
    // tracked or off (an off pelvis under an upright profile is still placed under the head).
    namespace sb = slbmap;
    const sb::Profile& map = LLVRBodyMap::profile();
    const F32 hipsOn = map.drives(sb::R_PELVIS) ? 1.f : 0.f;
    const F32 spineOn = (map.drives(sb::R_SPINE) || map.drives(sb::R_CHEST)) ? 1.f : 0.f;
    const F32 legOn[2] = {(map.drives(sb::R_L_THIGH) || map.drives(sb::R_L_SHIN) || map.drives(sb::R_L_FOOT)) ? 1.f : 0.f,
                          (map.drives(sb::R_R_THIGH) || map.drives(sb::R_R_SHIN) || map.drives(sb::R_R_FOOT)) ? 1.f : 0.f};
    const F32 spineW = llclamp((F32)s_w_spine, 0.f, 1.f) * spineOn;
    const F32 roleW[fb::B_COUNT] = {llclamp((F32)s_w_hips, 0.f, 1.f) * hipsOn, spineW, llclamp((F32)s_w_legs, 0.f, 1.f) * legOn[0],
                                    llclamp((F32)s_w_legs, 0.f, 1.f) * legOn[1], 1.f, 1.f};
    // #1584: flying, the hips and legs are the fly animation's whatever the source
    // (slfbt::roleTrackedInFlight: tracked feet have no floor in the air).
    const bool flying = gAgent.getFlying();
    if (flying != sB.flyingLogged)
    {
        sB.flyingLogged = flying;
        if (sB.engaged)
            LL_INFOS("VRBody") << (flying ? "flying: the hips and legs follow the fly animation (the spine, arms and head stay tracked)"
                                          : "landed: the hips and legs follow your tracking again")
                               << LL_ENDL;
    }
    for (int b = 0; b < fb::B_COUNT; ++b)
    {
        Bone& bn = sB.bone[b];
        const int slot = c.valid ? c.slot[b] : -1;
        const bool located = have && slot >= 0 && tr.role[slot].valid;
        if (slot >= 0 && !fr.skeleton) notePresence(b, located);   // (a skeleton's roles are never located)
        const bool sample = usable && eyeOk && located && (!flying || fb::roleTrackedInFlight(b));
        if (sample)
        {
            const LLXrTrackerPose& tp = tr.role[slot];
            const fb::V3 p = xrPosToBody(tp.position, floorY);
            const fb::Q q = xrRotToBody(tp.orientation);
            if (!bn.smoothValid)
            {
                bn.pos = p;
                bn.rot = q;
                bn.smoothValid = true;
            }
            else
            {
                // (A source's low confidence -- generated legs -- smooths up to 4x more.)
                const F32 conf = llclamp(fr.confidence[slot], 0.f, 1.f);
                fb::smoothPose(bn.pos, bn.rot, p, q, dt, tau * (1.f + 3.f * (1.f - conf)));
            }
            fb::V3 bp;
            fb::Q bq;
            fb::applyOffset(bn.pos, bn.rot, c.off[b], bp, bq);
            // #1592: an upright quadruped's foot joint (the hock) rests far above your ankle: the
            // target is your ankle lifted by the difference, straight up (a calibration under an
            // upright profile maps the foot tracker onto a human ankle, so the tracker's offset
            // carries no long lever that a tilted foot would swing).
            if (sUpright && (b == fb::B_LFOOT || b == fb::B_RFOOT)) bp.z += llmax(0.f, sk.pr.ankle - 0.055f) * userEyeHeight();
            t.pos[b] = toAgent(bp);
            t.rot[b] = toLLQ(fb::qnormalized(fb::qmul(turnQ, bq)));
        }
        bn.fade = fb::fadeStep(bn.fade, sample, dt);
        t.have[b] = bn.fade > 0.f && bn.smoothValid;   // (fading out: the last pose)
        t.weight[b] = t.have[b] ? bn.fade * roleW[b] : 0.f;
        if (!t.have[b]) bn.smoothValid = bn.smoothValid && sample;
    }
    const F32 waist = sB.bone[fb::B_WAIST].fade;
    const F32 feet = llmax(sB.bone[fb::B_LFOOT].fade, sB.bone[fb::B_RFOOT].fade);
    const F32 any = llmax(llmax(waist, feet), sB.bone[fb::B_CHEST].fade);
    t.waistBlend = waist;
    t.hipsWeight = llmax(waist, feet) * roleW[fb::B_WAIST];
    t.spineWeight = any * spineW;
    if (sB.uprightFade > 0.f)   // #1592
    {
        t.hipsWeight = llmax(t.hipsWeight, sB.uprightFade * roleW[fb::B_WAIST]);
        t.spineWeight = llmax(t.spineWeight, sB.uprightFade * spineW);
    }
    // #1582 VMC: a whole skeleton drives the SL joints directly (llvrbodyvmc.h; the motion places
    // it under the headset with the avatar's own bone lengths).
    {
        const bool skel = usable && eyeOk && fr.skeleton && hipsOn > 0.f;   // (#1592: the VMC body needs the hips)
        if (skel)
        {
            bool sh[slvmc::S_COUNT];
            fb::Q sw[slvmc::S_COUNT];
            slvmc::toSecondLife(fr.skelHave, fr.skelRot, sh, sw);
            for (int sj = 0; sj < slvmc::S_COUNT; ++sj)
            {
                // (Flying: VMC's legs too go to the fly animation.)
                t.fullHave[sj] = sh[sj] && !(flying && sj >= slvmc::S_L_HIP && sj <= slvmc::S_R_FOOT);
                if (sh[sj]) t.fullRot[sj] = toLLQ(fb::qnormalized(fb::qmul(turnQ, sw[sj])));
            }
            // An arm the #1584 arm IK drives (its controller tracked, arm tracking on, not in fly
            // cam) stays with the controller; otherwise it is VMC's (last frame's IK state).
            for (int side = 0; side < 2; ++side) t.fullArm[side] = !LLVRBodyArms::targets().hand[side].driven;
        }
        sB.fullFade = fb::fadeStep(sB.fullFade, skel && t.fullHave[slvmc::S_PELVIS], dt);
        t.full = sB.fullFade > 0.f && t.fullHave[slvmc::S_PELVIS];
        t.fullWeight = t.full ? sB.fullFade : 0.f;
        t.floorZ = m.footZ;
    }
    t.on = eyeOk && (t.full || t.hipsWeight > 0.f || t.spineWeight > 0.f || t.weight[fb::B_LFOOT] > 0.f ||
                     t.weight[fb::B_RFOOT] > 0.f);
    if (!t.on)
    {
        // Nothing to drive (sitting, fly cam, setting off, every tracker faded out, ...).
        t.hipsWeight = t.spineWeight = 0.f;
        sHoldAnchor = false;
        sB.chestWeight = 0.f;
    }
    updateArms(dt, hp, headOk, have ? &fr : nullptr, m);   // #1584 (elbow roles from the frame)
    holdPose(av);
    setMotion(t.on || LLVRBodyArms::on() || LLVRBodyFingers::on() || sPoseHeld);
    if (!have && !t.on && !sCalibrating)
    {
        sB.engaged = false;   // the trackers are gone and everything faded: idle again
        sAvailable = false;
    }
}
