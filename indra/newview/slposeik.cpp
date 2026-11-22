/**
 * @file slposeik.cpp
 * @brief Pose Studio IK handles (#530). See slposeik.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slposeik.h"

#include "llagentcamera.h"
#include "llavatarjoint.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "lljoint.h"
#include "llkeyboard.h"
#include "llrender.h"
#include "m3math.h"
#include "llsdserialize.h"
#include "lltool.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "slfloaterposestudio.h"
#include "slposelookat.h"
#include "slposepupil.h"

#include <cmath>
#include <functional>

namespace
{
constexpr F32 MIN_PICK_PX = 9.f;            // pick radius floor, UI pixels (follows UI scale)
constexpr F32 DRAG_ROTATE_RAD_PER_PX = 0.01f;
constexpr F32 AXIS_LOCK_PX = 4.f;
constexpr S32 TWOBONE_ITERATIONS = 4;
constexpr S32 CCD_ITERATIONS = 10;
constexpr S32 CCD_REFINE_ITERATIONS = 4;    // after a limited multi-bone solve
constexpr F32 CCD_MAX_STEP_RAD = 25.f * DEG_TO_RAD; // per joint per pass: no flailing
constexpr S32 FABRIK_ITERATIONS = 24;
constexpr F32 REACHED_M = 0.0005f;
constexpr F32 HOLD_EPSILON_M = 0.0005f;
constexpr F32 HOLD_EPSILON_DOT = 0.99999f;
constexpr F32 BONE_CHANGED_DOT = 0.99995f;
constexpr F32 LIMIT_REPRODUCED_DOT = 0.99999f; // ~0.5 degrees: the dials show this rotation
constexpr S32 LIMIT_BISECTIONS = 10;
constexpr F32 LOOKAT_DISTANCE = 0.35f;      // metres in front of the head (scaled with the avatar)
constexpr F32 BASE_HANDLE_LENGTH = 0.075f;  // metres (scaled with the avatar and the setting)
constexpr S32 RING_SEGMENTS = 48;
// Rotate rings, after LLManipRotate: screen-size radius, a generous pick band, edge-on
// below 85 degrees from the view (decided once at mouse down).
constexpr F32 RING_RADIUS_PX = 80.f;
constexpr F32 RING_WIDTH_PX = 2.5f;         // half width of the drawn band
constexpr F32 RING_PICK_PX = 10.f;
const F32 RING_EDGE_ON_DOT = cosf(85.f * DEG_TO_RAD);
constexpr F32 RING_SNAP_DEG = 15.f;         // Shift while turning a ring
// Translate gizmo, after LLManipTranslate: screen-size arrows and plane squares (UI pixels).
constexpr F32 MOVE_ARROW_START_PX = 14.f;
constexpr F32 MOVE_ARROW_LENGTH_PX = 72.f;
constexpr F32 MOVE_HEAD_LENGTH_PX = 14.f;
constexpr F32 MOVE_HEAD_RADIUS_PX = 5.f;
constexpr F32 MOVE_SHAFT_PX = 1.5f;         // half width
constexpr F32 MOVE_PICK_PX = 8.f;
constexpr F32 MOVE_PLANE_LO_PX = 20.f;
constexpr F32 MOVE_PLANE_HI_PX = 38.f;
constexpr F32 MOVE_ARROW_MAX_DOT = 0.985f;  // an arrow pointing at the camera is hidden
constexpr F32 MOVE_PLANE_MIN_DOT = 0.12f;   // an edge-on square is hidden
constexpr F32 POLE_MIN_BEND_DEG = 10.f;     // a set pole keeps at least this much bend to point with
constexpr F32 POLE_DISTANCE = 0.5f;         // pole diamond: this many limb lengths in front of the joint
constexpr F32 TIPS_POLL_SECONDS = 0.5f;     // rigging-key poll for the handle points
constexpr F32 TIP_EXTREMITY = 0.75f;        // handle point: this far from a rigged box's center to its far face
constexpr F32 ROOT_HANDLE_DROP = 0.3f;      // root diamond: metres under the hips (scaled), clear of the hips box

constexpr const char* FLOATER_NAME = "sl_pose_studio";

bool nearlySameRotation(const LLQuaternion& a, const LLQuaternion& b, F32 min_dot)
{
    return llabs(dot(a, b)) >= min_dot;
}

LLVector3 perpendicular(const LLVector3& v)
{
    LLVector3 p = (llabs(v.mV[VX]) < 0.9f) ? (v % LLVector3::x_axis) : (v % LLVector3::y_axis);
    p.normVec();
    return p;
}

// The rotation is one the bone's dials show, every dial inside its range.
bool withinLimits(const SLBoneModel& model, const LLQuaternion& local, const SLPoseAngles& reference)
{
    LLQuaternion target(local);
    target.normalize();
    const SLPoseAngles dial = model.fromQuat(target, reference);
    if (llabs(dot(model.toQuat(dial), target)) < LIMIT_REPRODUCED_DOT)
        return false;
    for (S32 axis = 0; axis < 3; ++axis)
    {
        const F32 v = dial.get(axis);
        if (v < model.mMin.mV[axis] - 0.01f || v > model.mMax.mV[axis] + 0.01f)
            return false;
    }
    return true;
}

LLVector3 readVector(const LLSD& sd, const LLVector3& fallback)
{
    if (!sd.isArray() || sd.size() < 3)
        return fallback;
    return LLVector3((F32)sd[0].asReal(), (F32)sd[1].asReal(), (F32)sd[2].asReal());
}
}

// Holds the mouse while a handle is dragged: hover and mouse-up come here through the
// focus manager's mouse capture, whatever tool is current.
class SLPoseIKTool : public LLTool
{
public:
    SLPoseIKTool() : LLTool("SLPoseIK") {}

    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        if (!hasMouseCapture())
            return false;
        SLPoseIK::instance().onDragHover(x, y, mask);
        gViewerWindow->setCursor(UI_CURSOR_TOOLTRANSLATE);
        return true;
    }

    bool handleMouseUp(S32 x, S32 y, MASK mask) override
    {
        if (hasMouseCapture())
        {
            SLPoseIK::instance().onDragHover(x, y, mask);
            setMouseCapture(false);     // ends the drag in onMouseCaptureLost
        }
        return true;
    }

    bool handleMouseDown(S32, S32, MASK) override { return true; }
    bool handleRightMouseDown(S32, S32, MASK) override { return true; }
    bool handleRightMouseUp(S32, S32, MASK) override { return true; }
    bool clipMouseWhenDown() override { return false; }

    void onMouseCaptureLost() override
    {
        if (SLPoseIK::instanceExists())
            SLPoseIK::instance().onDragEnd(false);
    }
};

SLPoseIK::SLPoseIK()
{
    loadChains();
    mTool = new SLPoseIKTool();
    SLPoseStudio& studio = SLPoseStudio::instance();
    mEditedConnection = studio.setEditedCallback([this](const LLUUID& id, bool committed) { onStudioEdited(id, committed); });
    mPosingConnection = studio.setPosingCallback([this](const LLUUID& id, bool posing) { onStudioPosing(id, posing); });
}

SLPoseIK::~SLPoseIK()
{
    if (mTool && mTool->hasMouseCapture())
        gFocusMgr.setMouseCapture(nullptr);
}

// ---------------------------------------------------------------------------
// Chains
// ---------------------------------------------------------------------------

void SLPoseIK::loadChains()
{
    mChains.clear();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "posestudio", "ik_chains.xml");
    llifstream infile(path.c_str());
    LLSD data;
    if (!infile.is_open() || LLSDSerialize::fromXML(data, infile) == LLSDParser::PARSE_FAILURE || !data.isArray())
    {
        LL_WARNS("PoseStudio") << "Cannot read " << path << "; no IK handles" << LL_ENDL;
        return;
    }

    for (const LLSD& entry : llsd::inArray(data))
    {
        Chain chain;
        chain.mName = entry["name"].asString();
        const std::string solver = entry["solver"].asString();
        if (solver == "twobone")
            chain.mSolver = SOLVER_TWOBONE;
        else if (solver == "lookat")
            chain.mSolver = SOLVER_LOOKAT;
        else if (solver == "pelvis")
            chain.mSolver = SOLVER_PELVIS;
        else if (solver == "reach")
            chain.mSolver = SOLVER_REACH;
        else if (solver == "gaze")
            chain.mSolver = SOLVER_GAZE;
        else if (solver == "hips")
            chain.mSolver = SOLVER_HIPS;
        else
            chain.mSolver = SOLVER_CCD;
        for (const LLSD& joint : llsd::inArray(entry["joints"]))
            chain.mJoints.push_back(joint.asString());
        for (const LLSD& tip : llsd::inArray(entry["tip"]))
        {
            if (SLPoseStudio::instance().findJoint(tip.asString()))
                chain.mTip.push_back(tip.asString());
            else
                LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": unknown tip joint " << tip.asString() << LL_ENDL;
        }
        chain.mHandBranches = llmax(0, entry["hand_branches"].asInteger());
        if (entry.has("hand_depth"))
            chain.mHandDepth = llmax(0, entry["hand_depth"].asInteger());
        chain.mHandSkip = entry["hand_skip"].asString();
        chain.mMirror = entry["mirror"].asString();
        chain.mBento = entry["bento"].asBoolean();
        chain.mGroup = entry["group"].asString();
        chain.mRigged = entry["rigged"].asBoolean();
        if (entry.has("color"))
        {
            const LLVector3 rgb = readVector(entry["color"], LLVector3(0.4f, 0.8f, 1.f));
            chain.mColor = LLColor4(rgb.mV[VX], rgb.mV[VY], rgb.mV[VZ], 1.f);
        }
        chain.mForward = readVector(entry["forward"], chain.mForward);
        chain.mForward.normVec();
        chain.mBend = readVector(entry["bend"], chain.mBend);
        chain.mBend.normVec();
        for (const LLSD& share : llsd::inArray(entry["share"]))
            chain.mShare.push_back(llclamp((F32)share.asReal(), 0.f, 1.f));
        chain.mExtend = llclamp(entry["extend"].asInteger(), 0, 4);
        if (entry.has("max_bend"))
            chain.mMaxBend = llclamp((F32)entry["max_bend"].asReal(), 10.f, 179.f);
        chain.mBox = entry["shape"].asString() == "box";
        chain.mPlanted = entry["planted"].asBoolean();
        static const char* const EYE_SET_NAMES[] = { "eyes", "alt", "nose", "both" };
        for (const LLSD& set : llsd::inArray(entry["eye_sets"]))
        {
            S32 pick = -1;
            for (S32 k = 0; k <= SLPoseIKMath::EYE_SET_BOTH; ++k)
            {
                if (set["name"].asString() == EYE_SET_NAMES[k])
                    pick = k;
            }
            if (pick < 0)
            {
                LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": unknown eye set " << set["name"].asString() << LL_ENDL;
                continue;
            }
            Chain::EyeSet& eyes = chain.mEyeSets[pick];
            for (const LLSD& eye : llsd::inArray(set["joints"]))
            {
                if (SLPoseStudio::instance().findJoint(eye.asString()))
                    eyes.mJoints.push_back(eye.asString());
                else
                    LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": eye " << eye.asString() << " is not a Pose Studio bone" << LL_ENDL;
            }
            eyes.mForward = readVector(set["forward"], eyes.mForward);
            eyes.mForward.normVec();
            // #1567: a pupil set slides its bones (one mapping per joint kept above).
            SLPosePupil::readSet(set, eyes.mJoints, eyes.mPupil);
        }
        for (const LLSD& limit : llsd::inArray(entry["limits"]))
            chain.mLimits.push_back(llclamp((F32)limit.asReal(), 0.f, 180.f));
        if (entry.has("tip_length"))
            chain.mTipLength = llclamp((F32)entry["tip_length"].asReal(), 0.01f, 1.f);

        const bool single = chain.mSolver == SOLVER_PELVIS || chain.mSolver == SOLVER_HIPS;
        const size_t needed = (chain.mSolver == SOLVER_TWOBONE) ? 3 : (single ? 1 : 2);
        if (chain.mJoints.size() < needed)
        {
            LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": wrong joint count, skipped" << LL_ENDL;
            continue;
        }
        bool known = true;
        for (const std::string& joint : chain.mJoints)
            known = known && SLPoseStudio::instance().findJoint(joint) != nullptr;
        if (!known)
        {
            LL_WARNS("PoseStudio") << "IK chain " << chain.mName << ": a joint Pose Studio cannot pose, skipped" << LL_ENDL;
            continue;
        }
        mChains.push_back(chain);
    }
    mStates.assign(mChains.size(), ChainState());
    LL_INFOS("PoseStudio") << "Loaded " << mChains.size() << " IK chains" << LL_ENDL;
}

bool SLPoseIK::chainUsable(LLVOAvatar* avatar, S32 chain) const
{
    if (!avatar || chain < 0 || chain >= (S32)mChains.size())
        return false;
    const Chain& c = mChains[chain];
    static LLCachedControl<bool> bento(gSavedSettings, "SLPoseStudioIKBentoChains", false);
    if (c.mGroup == "wings")
    {
        static LLCachedControl<bool> wings(gSavedSettings, "SLPoseStudioIKWings", true);
        if (!wings && !bento)
            return false;
    }
    else if (c.mBento && !bento)
    {
        return false;
    }
    // Only where the outfit has it: no wing / tail handles on an avatar without them.
    if (c.mRigged && (!mTipsValid || mTipsAvatar != avatar->getID() || chain >= (S32)mTips.size() || !mTips[chain].mBound))
        return false;
    for (const std::string& joint : c.mJoints)
    {
        if (!avatar->getJoint(joint))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

LLVOAvatar* SLPoseIK::getActiveAvatar() const
{
    // IK handles or the FK bones view (#1566) on.
    if ((!ikShown() && !fkShown()) || mChains.empty())
        return nullptr;
    if (!LLFloaterReg::instanceVisible(FLOATER_NAME))
        return nullptr;
    if (gAgentCamera.cameraMouselook())
        return nullptr;
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLVOAvatar* avatar = studio.getTarget();
    // Only ever the studio's own target, posed by us, and only while we may pose it:
    // never an avatar that merely shows a synced pose.
    if (!avatar || !studio.isPosing(avatar) || !studio.canPose(avatar) || !studio.isPosedByStudio(avatar->getID()))
        return nullptr;
    return avatar;
}

bool SLPoseIK::isActive() const
{
    return getActiveAvatar() != nullptr;
}

void SLPoseIK::syncAvatar(LLVOAvatar* avatar)
{
    if (avatar && avatar->getID() != mAvatarID)
    {
        reset();
        mAvatarID = avatar->getID();
    }
}

void SLPoseIK::reset()
{
    if (mDrag.mActive && mTool && mTool->hasMouseCapture())
        gFocusMgr.setMouseCapture(nullptr);   // ends the drag (onDragEnd)
    mDrag = DragState();
    mStates.assign(mChains.size(), ChainState());
    mUndo.clear();
    mRedo.clear();
    mHoverChain = -1;
    mHoverPole = false;
    mHoverRing = -1;
    mSelectedChain = -1;
    mAvatarID.setNull();
    mTipsValid = false;     // re-derived (and logged) the next time the handles show
}

void SLPoseIK::onBonesReset(LLVOAvatar* avatar, const std::string& joint)
{
    if (!avatar)
        return;
    if (joint.empty() || avatar->getID() != mAvatarID)
    {
        // Reset All: as when Pose Studio first opened -- no pins, no placed targets, poles,
        // look-at point, carried-target anchors, drag bend planes or undo; every handle snaps
        // onto the reset skeleton the next time the handles show.
        reset();
        return;
    }
    // One bone (and its mirror partner in mirror mode, which the studio reset too): the
    // chains it is part of start over; chains below it are carried as usual.
    SLPoseStudio& studio = SLPoseStudio::instance();
    std::string mirror;
    if (studio.getMirror())
    {
        if (const SLPoseStudio::joint_t* pj = studio.findJoint(joint))
            mirror = pj->mirrorJointName();
    }
    auto hit = [&](const std::vector<std::string>& names)
    {
        for (const std::string& name : names)
        {
            if (name == joint || (!mirror.empty() && name == mirror))
                return true;
        }
        return false;
    };
    for (S32 i = 0; i < (S32)mChains.size() && i < (S32)mStates.size(); ++i)
    {
        bool part = hit(mChains[i].mJoints) || hit(chainJoints(i));
        for (const Chain::EyeSet& eyes : mChains[i].mEyeSets)
            part = part || hit(eyes.mJoints);
        if (i < (S32)mTips.size() && !mTips[i].mJoint.empty())
            part = part || hit(std::vector<std::string>(1, mTips[i].mJoint));
        if (!part)
            continue;
        mStates[i] = ChainState();      // snaps onto its bones as they are now
        if (chainUsable(avatar, i))
            snapTarget(avatar, i);
    }
}

F32 SLPoseIK::handleScale(LLVOAvatar* avatar) const
{
    static LLCachedControl<F32> size(gSavedSettings, "SLPoseStudioIKHandleSize", 1.f);
    F32 body = avatar ? avatar->mBodySize.mV[VZ] : 0.f;
    if (body <= 0.1f)
        body = 1.9f;
    return llclamp(body / 1.9f, 0.25f, 4.f) * llclamp((F32)size, 0.25f, 4.f);
}

// ---------------------------------------------------------------------------
// Forward kinematics on the targets
// ---------------------------------------------------------------------------

SLPoseIK::FkMemoScope::FkMemoScope(const SLPoseIK& ik)
    : mIK(ik)
{
    if (mIK.mFkMemoDepth++ == 0)
        mIK.mFkMemo.clear();
}

SLPoseIK::FkMemoScope::~FkMemoScope()
{
    if (--mIK.mFkMemoDepth == 0)
        mIK.mFkMemo.clear();
}

const SLPoseStudio::joint_t* SLPoseIK::poserJoint(const std::string& name) const
{
    auto it = mPoserJointByName.find(name);
    if (it == mPoserJointByName.end())
        it = mPoserJointByName.emplace(name, SLPoseStudio::instance().findJoint(name)).first;
    return it->second;
}

bool SLPoseIK::fkWorld(LLVOAvatar* avatar, LLJoint* joint, LLVector3& pos, LLQuaternion& rot) const
{
    if (!avatar || !joint)
        return false;
    if (mFkMemoDepth > 0)
    {
        auto it = mFkMemo.find(joint);
        if (it != mFkMemo.end())
        {
            pos = it->second.mPos;
            rot = it->second.mRot;
            return true;
        }
    }
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLJoint* parent = joint->getParent();
    const SLPoseStudio::joint_t* poser_joint = poserJoint(joint->getName());
    if (!parent || !poser_joint)
    {
        // Above the posed skeleton (the avatar root): where it is now.
        pos = joint->getWorldPosition();
        rot = joint->getWorldRotation();
        if (mFkMemoDepth > 0)
            mFkMemo[joint] = FkMemoEntry{ pos, rot };
        return true;
    }

    LLVector3 parent_pos;
    LLQuaternion parent_rot;
    fkWorld(avatar, parent, parent_pos, parent_rot);

    FSPoserAnimator& animator = studio.getAnimator();
    LLQuaternion local_rot;
    LLVector3 local_pos, local_scale;
    if (!animator.isPosingAvatarJoint(avatar, *poser_joint)
        || !animator.getJointTargetTransform(avatar, *poser_joint, local_rot, local_pos, local_scale))
    {
        // Not held by the pose: the animation plays on it.
        local_rot = joint->getRotation();
        local_pos = joint->getPosition();
    }
    else
    {
        // Easing in (an eased edit, undo, pose load): the rotation it is going to.
        LLQuaternion blend_rot;
        LLVector3 blend_pos, blend_scale;
        if (studio.getBlendTarget(avatar, *poser_joint, blend_rot, blend_pos, blend_scale))
            local_rot = blend_rot;
    }
    // LLXformMatrix::update, on the targets.
    if (parent->getXform()->getScaleChildOffset())
        local_pos.scaleVec(parent->getScale());
    pos = parent_pos + local_pos * parent_rot;
    rot = local_rot * parent_rot;
    rot.normalize();
    if (mFkMemoDepth > 0)
        mFkMemo[joint] = FkMemoEntry{ pos, rot };
    return true;
}

bool SLPoseIK::fkJoint(LLVOAvatar* avatar, const std::string& name, LLVector3& pos, LLQuaternion& rot) const
{
    return avatar && fkWorld(avatar, avatar->getJoint(name), pos, rot);
}

bool SLPoseIK::fkParent(LLVOAvatar* avatar, const std::string& name, LLVector3& pos, LLQuaternion& rot) const
{
    LLJoint* joint = avatar ? avatar->getJoint(name) : nullptr;
    return joint && joint->getParent() && fkWorld(avatar, joint->getParent(), pos, rot);
}

void SLPoseIK::updateTips(LLVOAvatar* avatar)
{
    // Never mid-drag: the solve depends on the handle's offset staying put.
    if (!avatar || mDrag.mActive)
        return;
    const bool same_avatar = mTipsValid && mTipsAvatar == avatar->getID();
    if (same_avatar && mTipsTimer.getElapsedTimeF32() < TIPS_POLL_SECONDS)
        return;
    mTipsTimer.reset();
    // The avatar's rigging info: per joint, whether a worn rigged mesh weights vertices to it
    // (weight > 0.2) and the box of those vertices in the joint's space. Keyed by the worn
    // meshes and their LODs, so this is cheap while the outfit is unchanged.
    avatar->updateRiggingInfo();
    SLPoseStudio& studio = SLPoseStudio::instance();
    size_t key = avatar->mLastRiggingInfoKey;
    boost::hash_combine(key, studio.riggedJointsKey(avatar));
    if (same_avatar && mTipsKey == key)
        return;

    const std::vector<TipInfo> previous = same_avatar ? mTips : std::vector<TipInfo>();
    std::vector<bool> tip_changed(mChains.size(), false);
    mTipsValid = true;
    mTipsAvatar = avatar->getID();
    mTipsKey = key;
    mTips.assign(mChains.size(), TipInfo());

    const LLJointRiggingInfoTab& tab = avatar->mJointRiggingInfoTab;
    // CPU vertex weights with their boxes: where a handle point can be found.
    auto rigged = [&tab](const LLJoint* joint)
    {
        const S32 num = joint->getJointNum();
        return num >= 0 && num < tab.size() && tab[num].isRiggedTo();
    };
    // A worn rigged mesh uses the joint (skin joint list for GPU-skinned meshes too): what
    // gates chains and picks the eye bones, the same answer the FK bones and look-at use.
    const std::vector<bool>& used_joints = studio.riggedJoints(avatar);
    auto uses = [&](const LLJoint* joint)
    {
        const S32 num = joint->getJointNum();
        return rigged(joint) || (num >= 0 && num < (S32)used_joints.size() && used_joints[num]);
    };
    auto is_volume = [](LLJoint* joint) { return dynamic_cast<LLAvatarJointCollisionVolume*>(joint) != nullptr; };
    // Skeleton children only: bones Pose Studio knows and collision volumes (fitted mesh),
    // never attachment points.
    auto children = [&](LLJoint* joint)
    {
        std::vector<LLJoint*> out;
        for (LLJoint* child : joint->mChildren)
        {
            if (child && (is_volume(child) || studio.findJoint(child->getName())))
                out.push_back(child);
        }
        return out;
    };
    std::function<bool(LLJoint*)> subtree_rigged = [&](LLJoint* joint) -> bool
    {
        if (uses(joint))
            return true;
        for (LLJoint* child : children(joint))
        {
            if (subtree_rigged(child))
                return true;
        }
        return false;
    };

    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        const Chain& c = mChains[i];
        TipInfo& tip = mTips[i];
        LLJoint* root = avatar->getJoint(c.mJoints.front());
        LLJoint* end = avatar->getJoint(c.mJoints.back());
        if (!root || !end)
            continue;
        for (const std::string& name : c.mJoints)
        {
            LLJoint* joint = avatar->getJoint(name);
            tip.mBound = tip.mBound || (joint && uses(joint));
        }
        tip.mBound = tip.mBound || subtree_rigged(end);
        if (c.mSolver == SOLVER_GAZE)
        {
            // "Auto" eye bones: the pair the worn rigged meshes weight (alt eyes on a Bento
            // mesh head, the nose left / right bones on the Open Pony rig).
            auto set_rigged = [&](SLPoseIKMath::EyeSetPick pick)
            {
                for (const std::string& name : c.mEyeSets[pick].mJoints)
                {
                    LLJoint* joint = avatar->getJoint(name);
                    if (joint && uses(joint))
                        return true;
                }
                return false;
            };
            const bool eyes = set_rigged(SLPoseIKMath::EYE_SET_EYES);
            const bool alt = set_rigged(SLPoseIKMath::EYE_SET_ALT);
            const bool nose = set_rigged(SLPoseIKMath::EYE_SET_NOSE);
            tip.mAutoEyes = SLPoseIKMath::autoEyeSet(eyes, alt, nose);
            if (previous.size() != mTips.size() || previous[i].mAutoEyes != tip.mAutoEyes)
            {
                std::string turns;
                for (const std::string& name : c.mEyeSets[tip.mAutoEyes].mJoints)
                    turns += (turns.empty() ? "" : ", ") + name;
                LL_INFOS("PoseStudio") << "IK chain " << c.mName << ": Auto eye bones " << (turns.empty() ? "(none)" : turns)
                                       << " (worn rigged mesh weights: eyes " << eyes << ", alt eyes " << alt
                                       << ", nose left / right " << nose << ")" << LL_ENDL;
            }
        }
        if (c.mSolver == SOLVER_REACH)
        {
            // The nose: the first tip joint the skeleton has (the Bento nose, which a mesh
            // head's joint positions move onto its own nose / muzzle), never a rigged-region
            // search (that finds the back of the head or the ears).
            for (const std::string& name : c.mTip)
            {
                if (avatar->getJoint(name))
                {
                    tip.mJoint = name;
                    break;
                }
            }
            continue;
        }
        if (c.mSolver == SOLVER_LOOKAT || c.mSolver == SOLVER_PELVIS || c.mSolver == SOLVER_GAZE
            || c.mSolver == SOLVER_HIPS || c.mBox)
            continue;

        // A hand (several rigged fingers): stay near the knuckles, not on a fingertip.
        S32 branches = 0;
        for (LLJoint* child : children(end))
        {
            if (!is_volume(child) && subtree_rigged(child))
                ++branches;
        }
        const bool hand = c.mHandBranches > 0 && branches >= c.mHandBranches;
        const S32 max_depth = hand ? c.mHandDepth : S32_MAX;

        // The farthest rigged region at / below the end joint, measured from the chain's
        // root: its box (rendered space, joint-position overrides and scale included) is
        // pushed toward its far face along the root -> box direction, so a hoof's handle
        // sits at the hoof, a toe's at the toes. A collision volume's region belongs to
        // its bone.
        const LLVector3 root_pos = root->getWorldPosition();
        LLJoint* best_joint = nullptr;
        LLVector3 best_point;
        F32 best_reach = -1.f;
        S32 candidates = 0;
        std::vector<std::pair<LLJoint*, S32>> stack(1, std::make_pair(end, 0));
        while (!stack.empty())
        {
            const auto [joint, depth] = stack.back();
            stack.pop_back();
            if (depth < max_depth && !is_volume(joint))
            {
                for (LLJoint* child : children(joint))
                    stack.emplace_back(child, depth + 1);
            }
            if (!rigged(joint))
                continue;
            LLJoint* owner = is_volume(joint) ? joint->getParent() : joint;
            if (!owner || !studio.findJoint(owner->getName()))
                continue;
            // A hand's handle is the hand's, not a finger's: the thumb sticks out farthest
            // from the shoulder, so it must not set the point (#1458).
            if (hand && !c.mHandSkip.empty() && owner != end && owner->getName().find(c.mHandSkip) != std::string::npos)
                continue;
            ++candidates;
            const LLVector4a* extents = tab[joint->getJointNum()].getRiggedExtents();
            const LLMatrix4& world = joint->getWorldMatrix();
            LLVector3 corners[8];
            LLVector3 center;
            for (S32 k = 0; k < 8; ++k)
            {
                const LLVector3 local(extents[k & 1][0], extents[(k >> 1) & 1][1], extents[(k >> 2) & 1][2]);
                corners[k] = local * world;
                center += corners[k];
            }
            center *= 1.f / 8.f;
            LLVector3 dir = center - root_pos;
            if (dir.length() < 1.0e-4f)
                continue;
            dir.normVec();
            F32 support = 0.f;
            for (const LLVector3& corner : corners)
                support = llmax(support, (corner - center) * dir);
            const LLVector3 point = center + dir * (TIP_EXTREMITY * support);
            const F32 reach = (point - root_pos).length();
            if (reach > best_reach)
            {
                best_reach = reach;
                // A hand's handle rides the wrist, so curling or posing a finger never
                // drags it away; the point is still the knuckles found above.
                best_joint = hand ? end : owner;
                best_point = point;
            }
        }

        std::string why;
        if (best_joint)
        {
            tip.mJoint = best_joint->getName();
            tip.mOffset = (best_point - best_joint->getWorldPosition()) * ~best_joint->getWorldRotation();
            why = llformat("worn rigged mesh: farthest of %d rigged regions at/below %s", candidates, c.mJoints.back().c_str());
            if (hand)
                why += llformat("; a hand (%d rigged finger branches), searched %d joint(s) below the wrist, anchored on the wrist, \"%s\" skipped", branches, max_depth, c.mHandSkip.c_str());
        }
        else
        {
            for (const std::string& name : c.mTip)
            {
                if (avatar->getJoint(name))
                {
                    tip.mJoint = name;
                    break;
                }
            }
            why = "nothing rigged at/below " + c.mJoints.back() + ": fallback tip list";
        }

        // A hoof (or any handle point) below the end joint of a chain that is not a hand:
        // the chain grows toward it (up to mExtend joints), so the joint carrying the hoof
        // takes the handle's orientation and every joint above it -- the wrist / cannon
        // included -- folds when the limb has to shorten. A human hand keeps the wrist as
        // its end (the fingers are not limb segments). Only from a worn rigged mesh: without
        // one (system avatar, mesh still loading) nothing says whether the end is a hand.
        if (best_joint && c.mExtend > 0 && !hand && !tip.mJoint.empty() && tip.mJoint != c.mJoints.back())
        {
            std::vector<std::string> path;     // tip joint up to (not including) the end
            LLJoint* walk = avatar->getJoint(tip.mJoint);
            while (walk && walk != end && studio.findJoint(walk->getName()))
            {
                path.push_back(walk->getName());
                walk = walk->getParent();
            }
            if (walk == end && !path.empty())
            {
                tip.mJoints = c.mJoints;
                for (S32 k = (S32)path.size() - 1; k >= 0 && (S32)(tip.mJoints.size() - c.mJoints.size()) < c.mExtend; --k)
                    tip.mJoints.push_back(path[k]);
            }
        }

        const bool changed = previous.size() != mTips.size() || previous[i].mJoint != tip.mJoint
                             || previous[i].mBound != tip.mBound || (previous[i].mOffset - tip.mOffset).length() > 0.01f
                             || previous[i].mJoints != tip.mJoints;
        if (changed)
        {
            tip_changed[i] = true;
            std::string solves;
            for (const std::string& name : (tip.mJoints.empty() ? c.mJoints : tip.mJoints))
                solves += (solves.empty() ? "" : " -> ") + name;
            if (c.mSolver == SOLVER_TWOBONE)
                solves += "; pole bends " + c.mJoints[1];
            LLVector3 handle_pos = end->getWorldPosition();
            if (LLJoint* joint = tip.mJoint.empty() ? nullptr : avatar->getJoint(tip.mJoint))
                handle_pos = joint->getWorldPosition() + tip.mOffset * joint->getWorldRotation();
            LL_INFOS("PoseStudio") << "IK chain " << c.mName << ": handle on " << (tip.mJoint.empty() ? c.mJoints.back() : tip.mJoint)
                                   << ", " << (handle_pos - end->getWorldPosition()).length() << " m from " << c.mJoints.back()
                                   << " (" << why << "); solves " << solves
                                   << ((c.mRigged && !tip.mBound) ? "; hidden: no worn rigged mesh binds this chain" : "")
                                   << LL_ENDL;
        }
    }
    // A handle point moved (outfit or mesh LOD changed): a target still on its bones goes
    // to the new point; one the user placed stays exactly where it was put.
    if (same_avatar)
    {
        for (S32 i = 0; i < (S32)mChains.size() && i < (S32)mStates.size(); ++i)
        {
            if (!tip_changed[i] || !usesTarget(i) || mStates[i].mPlaced || !chainUsable(avatar, i))
                continue;
            snapTarget(avatar, i);
            mStates[i].mHasPole = false;
            if (mStates[i].mPinned)
                mStates[i].mPinBones = chainTargets(avatar, i);
        }
    }
}

const std::vector<std::string>& SLPoseIK::chainJoints(S32 chain) const
{
    if (chain >= 0 && chain < (S32)mTips.size() && !mTips[chain].mJoints.empty())
        return mTips[chain].mJoints;
    return mChains[chain].mJoints;
}

bool SLPoseIK::fkEffector(LLVOAvatar* avatar, S32 chain, LLVector3& tip_pos, LLVector3& end_pos, LLQuaternion& end_rot) const
{
    if (!fkJoint(avatar, chainJoints(chain).back(), end_pos, end_rot))
        return false;
    tip_pos = end_pos;
    if (hasTip(chain))
    {
        LLVector3 joint_pos;
        LLQuaternion joint_rot;
        if (fkJoint(avatar, mTips[chain].mJoint, joint_pos, joint_rot))
            tip_pos = joint_pos + mTips[chain].mOffset * joint_rot;
    }
    return true;
}

void SLPoseIK::captureDragStart(LLVOAvatar* avatar)
{
    mDrag.mStart.clear();
    SLPoseStudio& studio = SLPoseStudio::instance();
    FSPoserAnimator& animator = studio.getAnimator();
    // Every bone a drag can set: all chains (pins hold theirs) and, for mirror mode, the
    // opposite of each.
    std::vector<std::string> names;
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!chainUsable(avatar, i))
            continue;
        std::vector<std::string> bones = chainJoints(i);
        // Every eye set, not just the one turned now: a start pose covers a mid-drag switch.
        for (const Chain::EyeSet& eyes : mChains[i].mEyeSets)
            bones.insert(bones.end(), eyes.mJoints.begin(), eyes.mJoints.end());
        for (const std::string& name : bones)
        {
            names.push_back(name);
            const SLPoseStudio::joint_t* joint = studio.findJoint(name);
            if (joint && !joint->mirrorJointName().empty())
                names.push_back(joint->mirrorJointName());
        }
    }
    // An FK bone drag (#1566): the bones it turns, and the children "Move bone alone" sets.
    for (const std::string& name : mDrag.mBoneJoints)
        names.push_back(name);
    for (const std::string& name : names)
    {
        if (mDrag.mStart.count(name))
            continue;
        const SLPoseStudio::joint_t* joint = studio.findJoint(name);
        LLJoint* lljoint = avatar->getJoint(name);
        if (!joint || !lljoint)
            continue;
        StartBone bone;
        // Exactly what fkWorld reads.
        bone.mPosed = animator.isPosingAvatarJoint(avatar, *joint) && animator.getJointTargetRotation(avatar, *joint, bone.mLocal);
        if (!bone.mPosed)
            bone.mLocal = lljoint->getRotation();
        bone.mLocal.normalize();
        if (!studio.getRememberedAngles(avatar->getID(), name, bone.mReference))
            studio.getBoneAngles(avatar, *joint, bone.mReference);
        mDrag.mStart[name] = bone;
    }
}

void SLPoseIK::restoreDragStart(LLVOAvatar* avatar)
{
    // Silently (no edited signal, no mirror, no ease): only the solve that follows is an edit.
    SLPoseStudio& studio = SLPoseStudio::instance();
    FSPoserAnimator& animator = studio.getAnimator();
    for (const auto& [name, bone] : mDrag.mStart)
    {
        const SLPoseStudio::joint_t* joint = studio.findJoint(name);
        if (!joint)
            continue;
        // Played by the animation then and untouched since: leave it to the animation.
        if (!bone.mPosed && !animator.isPosingAvatarJoint(avatar, *joint))
            continue;
        animator.setJointTargetRotation(avatar, *joint, bone.mLocal);
    }
}

void SLPoseIK::startOf(LLVOAvatar* avatar, const std::string& name, LLQuaternion& local, SLPoseAngles& reference) const
{
    if (mDrag.mActive)
    {
        auto it = mDrag.mStart.find(name);
        if (it != mDrag.mStart.end())
        {
            local = it->second.mLocal;
            reference = it->second.mReference;
            return;
        }
    }
    SLPoseStudio& studio = SLPoseStudio::instance();
    const SLPoseStudio::joint_t* joint = studio.findJoint(name);
    LLJoint* lljoint = avatar ? avatar->getJoint(name) : nullptr;
    local = lljoint ? lljoint->getRotation() : LLQuaternion::DEFAULT;
    reference = SLPoseAngles();
    if (!joint || !avatar)
        return;
    LLQuaternion target;
    if (studio.getAnimator().isPosingAvatarJoint(avatar, *joint) && studio.getAnimator().getJointTargetRotation(avatar, *joint, target))
        local = target;
    if (!studio.getRememberedAngles(avatar->getID(), name, reference))
        studio.getBoneAngles(avatar, *joint, reference);
}

bool SLPoseIK::applyWorldRotation(LLVOAvatar* avatar, const std::string& name, const LLQuaternion& world,
                                  const LLQuaternion& parent_world, bool limits)
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    const SLPoseStudio::joint_t* joint = studio.findJoint(name);
    if (!joint)
        return false;

    // LL order: world = local * parent_world.
    LLQuaternion local = world * ~parent_world;
    local.normalize();
    if (!limits)
        return studio.setBoneRotation(avatar, *joint, local, false);

    // Limits through the dial model (bone_axes.xml ranges). Clamping each decoded dial on
    // its own is not continuous (the swing azimuth flips 180 degrees through the model's
    // poles, equivalent triples swap branches), so a clamped solve could land anywhere:
    // instead the bone turns from where it started toward the solved rotation and stops at
    // the last rotation still inside every range. The decode reference is fixed per drag.
    const SLBoneModel& model = studio.getBoneModel(name);
    LLQuaternion from;
    SLPoseAngles reference;
    startOf(avatar, name, from, reference);
    if (withinLimits(model, local, reference))
        return studio.setBoneRotation(avatar, *joint, local, false);
    // Already outside its ranges before the drag (a loaded pose, a non-human rig's own
    // stance): there is nothing to stop at continuously, so it is not limited.
    if (!withinLimits(model, from, reference))
        return studio.setBoneRotation(avatar, *joint, local, false);

    F32 inside = 0.f;
    F32 outside = 1.f;
    for (S32 i = 0; i < LIMIT_BISECTIONS; ++i)
    {
        const F32 mid = 0.5f * (inside + outside);
        if (withinLimits(model, slerp(mid, from, local), reference))
            inside = mid;
        else
            outside = mid;
    }
    LLQuaternion limited = slerp(inside, from, local);
    limited.normalize();
    return studio.setBoneRotation(avatar, *joint, limited, false);
}

std::vector<LLQuaternion> SLPoseIK::chainTargets(LLVOAvatar* avatar, S32 chain) const
{
    std::vector<LLQuaternion> out;
    SLPoseStudio& studio = SLPoseStudio::instance();
    for (const std::string& name : chainJoints(chain))
    {
        LLQuaternion rot;
        const SLPoseStudio::joint_t* joint = studio.findJoint(name);
        if (!joint || !studio.getAnimator().getJointTargetRotation(avatar, *joint, rot))
            rot = LLQuaternion::DEFAULT;
        out.push_back(rot);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Solvers
// ---------------------------------------------------------------------------

void SLPoseIK::solveChain(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot)
{
    static LLCachedControl<bool> limits_setting(gSavedSettings, "SLPoseStudioIKLimits", true);
    const bool limits = limits_setting;

    // The goal is for the tip (where the handle is); the solvers move the end joint. The
    // bones between end and tip are not solved, so the tip keeps its offset in the end
    // bone's frame: the end goes where that offset puts the tip on the goal.
    LLVector3 end_goal = goal;
    const ESolver solver = mChains[chain].mSolver;
    if (solver == SOLVER_REACH)
    {
        // Ctrl / rings turn the head in place; a move pulls the nose.
        if (end_rot)
            setEndRotation(avatar, chain, *end_rot, limits);
        else
            solveReach(avatar, chain, goal, limits);
        return;
    }
    if (solver == SOLVER_GAZE)
    {
        solveGaze(avatar, chain, goal, limits);
        return;
    }
    if (solver != SOLVER_LOOKAT && hasTip(chain))
    {
        LLVector3 tip_pos, end_pos;
        LLQuaternion end_now;
        if (fkEffector(avatar, chain, tip_pos, end_pos, end_now))
        {
            const LLVector3 offset_local = (tip_pos - end_pos) * ~end_now;
            end_goal = goal - offset_local * (end_rot ? *end_rot : end_now);
        }
    }

    switch (mChains[chain].mSolver)
    {
        case SOLVER_TWOBONE:
        case SOLVER_CCD:
            // Two bones: analytic, exact. Longer chains (a pony leg grown to its hoof, the
            // tail) and single bones: FABRIK, every joint taking part.
            if (chainJoints(chain).size() == 3)
                solveTwoBone(avatar, chain, end_goal, end_rot, limits);
            else
                solveMulti(avatar, chain, end_goal, end_rot, limits);
            break;
        case SOLVER_LOOKAT:
            solveLookAt(avatar, chain, goal, end_rot, limits);
            break;
        case SOLVER_PELVIS:
        case SOLVER_HIPS:
        default:
            break;  // the pelvis is moved directly by its drag
    }
}

bool SLPoseIK::limitJoint(S32 chain, S32 index) const
{
    // Limb hinges (elbow, knee, a pony's cannon) are NOT held to their dial ranges: those
    // are the pose HUD's dial ranges, not anatomy (the knee's bend dial stops at 60
    // degrees, the elbow's at 90), and holding a hinge to them left the shoulder / hip to
    // do all the work while the limb could not shorten. Two-bone hinges are capped
    // geometrically by Chain::mMaxBend instead; the limb's root (a ball joint) keeps its
    // dial ranges, and so does every joint of a CCD chain (tail, ears, wings).
    if (mChains[chain].mSolver == SOLVER_CCD)
        return true;
    return index == 0;
}

void SLPoseIK::setEndRotation(LLVOAvatar* avatar, S32 chain, const LLQuaternion& end_rot, bool limits)
{
    const std::string& end = chainJoints(chain).back();
    LLVector3 parent_pos;
    LLQuaternion parent_rot;
    if (fkParent(avatar, end, parent_pos, parent_rot))
        applyWorldRotation(avatar, end, end_rot, parent_rot, limits);
}

void SLPoseIK::solveTwoBone(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits)
{
    const Chain& c = mChains[chain];
    const ChainState& state = mStates[chain];
    const std::vector<std::string>& joints = chainJoints(chain);
    const std::string& root = joints[0];
    const std::string& mid = joints[1];
    const std::string& end = joints[2];
    const LLVector3 pole = state.mHasPole ? poleWorld(avatar, chain) : LLVector3::zero;
    const bool limit_root = limits && limitJoint(chain, 0);
    const bool limit_mid = limits && limitJoint(chain, 1);

    for (S32 iteration = 0; iteration < TWOBONE_ITERATIONS; ++iteration)
    {
        LLVector3 a, b, e, pa_pos;
        LLQuaternion wa, wb, we, pa;
        if (!fkJoint(avatar, root, a, wa) || !fkJoint(avatar, mid, b, wb) || !fkJoint(avatar, end, e, we)
            || !fkParent(avatar, root, pa_pos, pa))
            return;

        // Already there (e.g. turning the end bone in place) and no pole asking for a
        // bend: only the end rotation is asked for.
        if (!state.mHasPole && (e - goal).length() < REACHED_M)
        {
            if (end_rot)
                setEndRotation(avatar, chain, *end_rot, limits);
            LLVector3 e_now;
            LLQuaternion we_now;
            if (!fkJoint(avatar, end, e_now, we_now) || (e_now - goal).length() < REACHED_M)
                break;
            continue;
        }
        const LLVector3 upper = b - a;
        const LLVector3 lower = e - b;
        const F32 la = upper.length();
        const F32 lb = lower.length();
        const LLVector3 at = goal - a;
        const F32 dt = at.length();
        if (la < 1.0e-4f || lb < 1.0e-4f || dt < 1.0e-4f)
            return;
        const LLVector3 n = at / dt;
        const LLVector3 bend_hint = c.mBend * wa;
        LLVector3 side, knee, reach;
        F32 max_d = la + lb - 1.0e-5f;

        if (!legacyLimb())
        {
            // #1566: the knee's side comes from the bend plane the limb had at mouse down
            // (SLPoseIKMath, tested by scripts/check/test_pose_ik.cpp): the plane keeps its
            // normal while the goal moves, so the knee / elbow never flips across the
            // root -> goal line, and a straight limb bends the chain's own way (opposite
            // mBend). A pole the user placed wins (Blender).
            const SLPoseIKMath::BendFrame frame = bendFrame(avatar, chain);
            const SLPoseIKMath::V3 a3(a.mV[VX], a.mV[VY], a.mV[VZ]);
            const SLPoseIKMath::V3 n3(n.mV[VX], n.mV[VY], n.mV[VZ]);
            const SLPoseIKMath::V3 pole3(pole.mV[VX], pole.mV[VY], pole.mV[VZ]);
            const SLPoseIKMath::V3 goal3(goal.mV[VX], goal.mV[VY], goal.mV[VZ]);
            const SLPoseIKMath::V3 side3 = SLPoseIKMath::bendSide(a3, n3, frame, state.mHasPole ? &pole3 : nullptr);
            const SLPoseIKMath::TwoBoneResult r = SLPoseIKMath::twoBone(a3, la, lb, goal3, side3, c.mMaxBend,
                                                                        state.mHasPole ? POLE_MIN_BEND_DEG : 0.0);
            side.set((F32)side3.x, (F32)side3.y, (F32)side3.z);
            knee.set((F32)r.mKnee.x, (F32)r.mKnee.y, (F32)r.mKnee.z);
            reach.set((F32)r.mReach.x, (F32)r.mReach.y, (F32)r.mReach.z);
            if (state.mHasPole)
                max_d = llmin(max_d, sqrtf(la * la + lb * lb + 2.f * la * lb * cosf(POLE_MIN_BEND_DEG * DEG_TO_RAD)));
        }
        else
        {
            // Reach: out of range, the limb stretches straight toward the goal. Closer in, the
            // hinge folds up to mMaxBend from straight (its inner angle never closes below
            // 180 - mMaxBend). A set pole keeps a little bend, so the elbow / knee always has a
            // direction to point in.
            const F32 inner_min = (180.f - c.mMaxBend) * DEG_TO_RAD;
            const F32 folded = sqrtf(llmax(0.f, la * la + lb * lb - 2.f * la * lb * cosf(inner_min)));
            const F32 min_d = llmax(llabs(la - lb) + 1.0e-3f, folded);
            if (state.mHasPole)
            {
                const F32 cos_bend = cosf(POLE_MIN_BEND_DEG * DEG_TO_RAD);
                max_d = llmin(max_d, sqrtf(la * la + lb * lb + 2.f * la * lb * cos_bend));
            }
            const F32 d = (min_d < max_d) ? llclamp(dt, min_d, max_d) : max_d;

            // The bend plane holds the root, the goal and the pole (Blender): the elbow / knee
            // goes to the pole's side of the root -> goal line. No pole: the side it points to
            // now; a straight limb: opposite where the chain's lower bone swings (mBend).
            side = state.mHasPole ? (pole - a) : upper;
            side -= n * (side * n);
            if (side.length() < 0.05f * la)
            {
                side = -bend_hint;
                side -= n * (side * n);
                if (side.length() < 1.0e-6f)
                    side = perpendicular(n);
            }
            side.normVec();
            const F32 cos_a = llclamp((la * la + d * d - lb * lb) / (2.f * la * d), -1.f, 1.f);
            const F32 sin_a = sqrtf(llmax(0.f, 1.f - cos_a * cos_a));
            knee = a + n * (la * cos_a) + side * (la * sin_a);
            reach = a + n * d;
        }

        // The root carries the upper bone AND its bend plane (upper x lower) onto the new
        // ones: the elbow / knee swings into the pole's plane and then only bends about its
        // hinge. (Twisting the whole limb about the root -> goal line instead only rolled
        // the upper bone when the limb was nearly straight, and the elbow never moved.)
        LLVector3 u0 = upper / la;
        LLVector3 w0 = u0 % lower;
        if (w0.length() < 0.05f * lb)
            w0 = u0 % bend_hint;
        if (w0.length() < 1.0e-6f)
            w0 = perpendicular(u0);
        w0.normVec();
        LLVector3 u1 = knee - a;
        u1.normVec();
        LLVector3 w1 = u1 % (reach - knee);
        if (w1.length() < 1.0e-4f * lb)
            w1 = side % n;
        w1.normVec();
        LLMatrix3 frame_old, frame_new;
        frame_old.setRows(u0, w0, u0 % w0);
        frame_new.setRows(u1, w1, u1 % w1);
        LLQuaternion carry = ~LLQuaternion(frame_old) * LLQuaternion(frame_new);
        carry.normalize();
        applyWorldRotation(avatar, root, wa * carry, pa, limit_root);

        // The middle joint: the lower bone onto knee -> reach, from where the (possibly
        // limited) root actually put it -- a turn in the bend plane, i.e. about the hinge.
        LLVector3 a_now, b_now, e_now;
        LLQuaternion wa_now, wb_now, we_now;
        if (!fkJoint(avatar, root, a_now, wa_now) || !fkJoint(avatar, mid, b_now, wb_now) || !fkJoint(avatar, end, e_now, we_now))
            return;
        if ((e_now - b_now).length() > 1.0e-6f && (reach - b_now).length() > 1.0e-6f)
        {
            LLQuaternion bend;
            bend.shortestArc(e_now - b_now, reach - b_now);
            applyWorldRotation(avatar, mid, wb_now * bend, wa_now, limit_mid);
        }

        if (end_rot)
            setEndRotation(avatar, chain, *end_rot, limits);

        if (!fkJoint(avatar, end, e_now, we_now) || (e_now - goal).length() < REACHED_M)
            break;
        if (!limit_root && !limit_mid && dt <= max_d)
            break;  // analytic: one pass is exact without clamping
    }
}

void SLPoseIK::solveMulti(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits)
{
    // FABRIK on the joint positions, warm-started from the pose the chain is in (during a
    // drag: the pose at mouse down, see restoreDragStart), so the solution is a continuous
    // function of the goal and keeps the limb's own bend pattern (a pony hind leg's stifle
    // forward / hock back zig-zag). Every bone moves: the shortening is shared along the
    // whole chain instead of piling up at the root or curling the tip (CCD). Out of reach
    // the chain points straight at the goal. A pole turns the solved chain about the
    // root -> goal line until its first inner joint faces the pole (Blender). The positions
    // then become rotations root to end, each bone aimed at its solved child from where its
    // (already turned) parent put it, its bend plane carried along with no more twist than
    // needed (never a flip).
    const std::vector<std::string>& joints = chainJoints(chain);
    const S32 count = (S32)joints.size();   // count - 1 bones; the last joint is the end
    if (count < 2)
        return;
    const ChainState& state = mStates[chain];
    // Limited joints may stop short of their solved direction: solve again from there.
    const S32 passes = limits ? 3 : 1;
    auto toward = [](const LLVector3& v, const LLVector3& fallback)
    {
        const F32 l = v.length();
        return (l > 1.0e-6f) ? v * (1.f / l) : fallback;
    };

    for (S32 pass = 0; pass < passes; ++pass)
    {
        std::vector<LLVector3> p(count);
        for (S32 i = 0; i < count; ++i)
        {
            LLQuaternion unused;
            if (!fkJoint(avatar, joints[i], p[i], unused))
                return;
        }
        if (pass > 0 && (p[count - 1] - goal).length() < REACHED_M)
            break;
        std::vector<F32> len(count - 1);
        F32 total = 0.f;
        for (S32 i = 0; i < count - 1; ++i)
        {
            len[i] = (p[i + 1] - p[i]).length();
            total += len[i];
        }
        if (total < 1.0e-4f)
            return;

        const LLVector3 root = p[0];
        std::vector<LLVector3> q = p;
        const LLVector3 to_goal = goal - root;
        const F32 dist = to_goal.length();
        if (dist >= total - 1.0e-5f)
        {
            const LLVector3 dir = toward(to_goal, toward(p[count - 1] - root, LLVector3::x_axis));
            for (S32 i = 0; i < count - 1; ++i)
                q[i + 1] = q[i] + dir * len[i];
        }
        else
        {
            for (S32 iteration = 0; iteration < FABRIK_ITERATIONS; ++iteration)
            {
                q[count - 1] = goal;
                for (S32 i = count - 2; i >= 0; --i)
                    q[i] = q[i + 1] + toward(q[i] - q[i + 1], toward(p[i] - p[i + 1], LLVector3::x_axis)) * len[i];
                q[0] = root;
                for (S32 i = 0; i < count - 1; ++i)
                    q[i + 1] = q[i] + toward(q[i + 1] - q[i], toward(p[i + 1] - p[i], LLVector3::x_axis)) * len[i];
                if ((q[count - 1] - goal).length() < REACHED_M)
                    break;
            }
            if (state.mHasPole && count >= 3)
            {
                LLVector3 axis = q[count - 1] - root;
                if (axis.length() > 1.0e-4f)
                {
                    axis.normVec();
                    LLVector3 from = q[1] - root;
                    LLVector3 to = poleWorld(avatar, chain) - root;
                    from -= axis * (from * axis);
                    to -= axis * (to * axis);
                    if (from.length() > 1.0e-4f && to.length() > 1.0e-4f)
                    {
                        from.normVec();
                        to.normVec();
                        const LLQuaternion swing(atan2f((from % to) * axis, from * to), axis);
                        for (S32 i = 1; i < count - 1; ++i)
                            q[i] = root + (q[i] - root) * swing;
                    }
                }
            }
        }

        for (S32 i = 0; i < count - 1; ++i)
        {
            LLVector3 a, b, parent_pos;
            LLQuaternion ra, rb, parent_rot;
            if (!fkJoint(avatar, joints[i], a, ra) || !fkJoint(avatar, joints[i + 1], b, rb)
                || !fkParent(avatar, joints[i], parent_pos, parent_rot))
                return;
            const LLVector3 u = b - a;
            LLVector3 v = q[i + 1] - a;
            if (u.length() < 1.0e-6f || v.length() < 1.0e-6f)
                continue;
            LLQuaternion turn;
            turn.shortestArc(u, v);
            if (i + 2 < count)
            {
                // Carry the bend plane (this bone x the next one) onto the solved one, to
                // whichever side of it is nearer: a twist of at most 90 degrees.
                LLVector3 c_pos;
                LLQuaternion c_rot;
                if (fkJoint(avatar, joints[i + 2], c_pos, c_rot))
                {
                    v.normVec();
                    LLVector3 n_old = (u % (c_pos - b)) * turn;
                    LLVector3 n_new = v % (q[i + 2] - q[i + 1]);
                    n_old -= v * (n_old * v);
                    n_new -= v * (n_new * v);
                    if (n_old.length() > 1.0e-5f && n_new.length() > 1.0e-5f)
                    {
                        n_old.normVec();
                        n_new.normVec();
                        if (n_old * n_new < 0.f)
                            n_new = -n_new;
                        const F32 twist = atan2f((n_old % n_new) * v, n_old * n_new);
                        turn = turn * LLQuaternion(twist, v);
                        turn.normalize();
                    }
                }
            }
            applyWorldRotation(avatar, joints[i], ra * turn, parent_rot, limits && limitJoint(chain, i));
        }
        if (end_rot)
            setEndRotation(avatar, chain, *end_rot, limits);
    }

    // A chain limited at every joint (tail, ears, wings) may still be short of a goal it
    // could reach: a few damped CCD passes close what the limits allow.
    if (limits && mChains[chain].mSolver == SOLVER_CCD)
    {
        LLVector3 e_now;
        LLQuaternion re_now;
        if (fkJoint(avatar, joints[count - 1], e_now, re_now) && (e_now - goal).length() >= REACHED_M)
        {
            solveCCD(avatar, chain, goal, limits, CCD_REFINE_ITERATIONS);
            if (end_rot)
                setEndRotation(avatar, chain, *end_rot, limits);
        }
    }
}

void SLPoseIK::solveCCD(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool limits, S32 iterations)
{
    const std::vector<std::string>& joints = chainJoints(chain);
    const S32 count = (S32)joints.size();
    for (S32 iteration = 0; iteration < iterations; ++iteration)
    {
        for (S32 i = count - 2; i >= 0; --i)
        {
            LLVector3 jp, ep, pp;
            LLQuaternion jr, er, pr;
            if (!fkJoint(avatar, joints[i], jp, jr) || !fkJoint(avatar, joints.back(), ep, er)
                || !fkParent(avatar, joints[i], pp, pr))
                return;
            const LLVector3 to_end = ep - jp;
            const LLVector3 to_goal = goal - jp;
            if (to_end.length() < 1.0e-4f || to_goal.length() < 1.0e-4f)
                continue;
            LLQuaternion turn;
            turn.shortestArc(to_end, to_goal);
            // Damped: no joint turns more than CCD_MAX_STEP_RAD per pass (no flailing).
            const F32 angle = 2.f * acosf(llclamp(llabs(turn.mQ[VW]), 0.f, 1.f));
            if (angle > CCD_MAX_STEP_RAD)
                turn = slerp(CCD_MAX_STEP_RAD / angle, LLQuaternion::DEFAULT, turn);
            applyWorldRotation(avatar, joints[i], jr * turn, pr, limits && limitJoint(chain, i));
        }
        LLVector3 ep;
        LLQuaternion er;
        if (!fkJoint(avatar, joints.back(), ep, er) || (ep - goal).length() < REACHED_M)
            break;
    }
}

void SLPoseIK::solveLookAt(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits)
{
    const Chain& c = mChains[chain];
    const S32 count = (S32)c.mJoints.size();
    if (end_rot)
    {
        // Turning the head handle sets the head's facing directly.
        setEndRotation(avatar, chain, *end_rot, limits);
        return;
    }
    for (S32 i = 0; i < count; ++i)
    {
        LLVector3 ep, jp, pp;
        LLQuaternion er, jr, pr;
        if (!fkJoint(avatar, c.mJoints.back(), ep, er) || !fkJoint(avatar, c.mJoints[i], jp, jr)
            || !fkParent(avatar, c.mJoints[i], pp, pr))
            return;
        const LLVector3 facing = c.mForward * er;
        LLVector3 to_goal = goal - ep;
        if (to_goal.length() < 1.0e-3f)
            return;
        to_goal.normVec();
        LLQuaternion turn;
        turn.shortestArc(facing, to_goal);
        // The joints before the head take their share of the turn, the head the rest.
        const bool last = (i == count - 1);
        const F32 share = last ? 1.f : ((i < (S32)c.mShare.size()) ? c.mShare[i] : 0.4f);
        if (share <= 0.f)
            continue;
        if (!last)
            turn = slerp(share, turn);
        applyWorldRotation(avatar, c.mJoints[i], jr * turn, pr, limits);
    }
}

// ---------------------------------------------------------------------------
// #1566: stable limbs, head reach, gaze, body drags
// ---------------------------------------------------------------------------

// static
bool SLPoseIK::legacyLimb()
{
    static LLCachedControl<bool> legacy(gSavedSettings, "SLPoseStudioIKLegacyLimb", false);
    return legacy;
}

SLPoseIKMath::BendFrame SLPoseIK::bendFrame(LLVOAvatar* avatar, S32 chain) const
{
    // While dragging: the plane the limb bent in at mouse down, fixed for the whole drag.
    if (mDrag.mActive && chain >= 0 && chain < (S32)mDrag.mBendFrames.size() && mDrag.mBendFrames[chain].mValid)
        return mDrag.mBendFrames[chain];
    SLPoseIKMath::BendFrame frame;
    const std::vector<std::string>& joints = chainJoints(chain);
    LLVector3 a, b, e;
    LLQuaternion wa, wb, we;
    if (joints.size() != 3 || !fkJoint(avatar, joints[0], a, wa) || !fkJoint(avatar, joints[1], b, wb)
        || !fkJoint(avatar, joints[2], e, we))
        return frame;
    const LLVector3 side = -(mChains[chain].mBend * wa);
    return SLPoseIKMath::captureBendFrame(SLPoseIKMath::V3(a.mV[VX], a.mV[VY], a.mV[VZ]),
                                          SLPoseIKMath::V3(b.mV[VX], b.mV[VY], b.mV[VZ]),
                                          SLPoseIKMath::V3(e.mV[VX], e.mV[VY], e.mV[VZ]),
                                          SLPoseIKMath::V3(side.mV[VX], side.mV[VY], side.mV[VZ]));
}

namespace
{
SLPoseIKMath::V3 toV3(const LLVector3& v)
{
    return SLPoseIKMath::V3(v.mV[VX], v.mV[VY], v.mV[VZ]);
}

LLVector3 fromV3(const SLPoseIKMath::V3& v)
{
    return LLVector3((F32)v.x, (F32)v.y, (F32)v.z);
}

F32 angleOf(const LLVector3& a, const LLVector3& b)
{
    return atan2f((a % b).length(), a * b);
}

// The turn that aims bone u (world) along v, carrying the plane (u, next_old) onto
// (v, next_new) with no more twist than needed (as solveMulti does).
LLQuaternion aimCarry(const LLVector3& u, LLVector3 v, const LLVector3& next_old, const LLVector3& next_new)
{
    LLQuaternion turn;
    turn.shortestArc(u, v);
    v.normVec();
    LLVector3 n_old = (u % next_old) * turn;
    LLVector3 n_new = v % next_new;
    n_old -= v * (n_old * v);
    n_new -= v * (n_new * v);
    if (n_old.length() > 1.0e-5f && n_new.length() > 1.0e-5f)
    {
        n_old.normVec();
        n_new.normVec();
        if (n_old * n_new < 0.f)
            n_new = -n_new;
        turn = turn * LLQuaternion(atan2f((n_old % n_new) * v, n_old * n_new), v);
        turn.normalize();
    }
    return turn;
}
}

void SLPoseIK::solveReach(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool limits)
{
    // The neck and head as one chain whose last link ends at the nose: FABRIK on [neck,
    // head, nose] puts the head where the nose can reach the goal, the neck turns to carry
    // it there (keeping the head's plane, no roll), then the head aims the nose at the goal
    // from where the (possibly limited) neck actually put it.
    const Chain& c = mChains[chain];
    const std::vector<std::string>& joints = chainJoints(chain);
    const S32 count = (S32)joints.size();
    const F32 tip_length = c.mTipLength * handleScale(avatar);
    auto tipOf = [&](LLVector3& tip) -> bool
    {
        LLVector3 end_pos;
        LLQuaternion end_rot;
        if (!fkEffector(avatar, chain, tip, end_pos, end_rot))
            return false;
        if (!hasTip(chain))
            tip = end_pos + c.mForward * end_rot * tip_length;
        return true;
    };
    for (S32 pass = 0; pass < (limits ? 3 : 1); ++pass)
    {
        std::vector<SLPoseIKMath::V3> p;
        for (const std::string& name : joints)
        {
            LLVector3 pos;
            LLQuaternion rot;
            if (!fkJoint(avatar, name, pos, rot))
                return;
            p.push_back(toV3(pos));
        }
        LLVector3 tip;
        if (!tipOf(tip))
            return;
        if (pass > 0 && (tip - goal).length() < REACHED_M)
            break;
        p.push_back(toV3(tip));
        const std::vector<SLPoseIKMath::V3> q = SLPoseIKMath::fabrik(p, toV3(goal), FABRIK_ITERATIONS, REACHED_M);

        // Every joint but the last aims at its solved child.
        for (S32 i = 0; i + 1 < count; ++i)
        {
            LLVector3 a, b, parent_pos, next_pos;
            LLQuaternion ra, rb, parent_rot;
            if (!fkJoint(avatar, joints[i], a, ra) || !fkJoint(avatar, joints[i + 1], b, rb)
                || !fkParent(avatar, joints[i], parent_pos, parent_rot))
                return;
            if (i + 2 < count)
            {
                LLQuaternion unused;
                fkJoint(avatar, joints[i + 2], next_pos, unused);
            }
            else if (!tipOf(next_pos))
            {
                return;
            }
            const LLVector3 v = fromV3(q[i + 1]) - a;
            if ((b - a).length() < 1.0e-6f || v.length() < 1.0e-6f)
                continue;
            const LLQuaternion turn = aimCarry(b - a, v, next_pos - b, fromV3(q[i + 2]) - fromV3(q[i + 1]));
            applyWorldRotation(avatar, joints[i], ra * turn, parent_rot, limits);
        }
        // The head: the nose onto the goal, from where the neck put the head.
        LLVector3 h, parent_pos, nose;
        LLQuaternion rh, parent_rot;
        if (!fkJoint(avatar, joints.back(), h, rh) || !fkParent(avatar, joints.back(), parent_pos, parent_rot) || !tipOf(nose))
            return;
        if ((nose - h).length() > 1.0e-5f && (goal - h).length() > 1.0e-5f)
        {
            LLQuaternion turn;
            turn.shortestArc(nose - h, goal - h);
            applyWorldRotation(avatar, joints.back(), rh * turn, parent_rot, limits);
        }
    }
}

bool SLPoseIK::eyeCenter(LLVOAvatar* avatar, S32 chain, LLVector3& pos) const
{
    LLVector3 sum;
    S32 n = 0;
    for (const std::string& eye : gazeEyes(avatar, chain).mJoints)
    {
        LLVector3 p;
        LLQuaternion r;
        if (avatar->getJoint(eye) && fkJoint(avatar, eye, p, r))
        {
            sum += p;
            ++n;
        }
    }
    if (n > 0)
    {
        pos = sum * (1.f / (F32)n);
        return true;
    }
    LLQuaternion r;
    return fkJoint(avatar, mChains[chain].mJoints.back(), pos, r);
}

const SLPoseIK::Chain::EyeSet& SLPoseIK::gazeEyes(LLVOAvatar* avatar, S32 chain) const
{
    // 0 Auto (what the worn rigged meshes weight), 1 eyes, 2 alt eyes, 3 nose left / right
    // (Open Pony), 4 eyes + alt eyes.
    static LLCachedControl<S32> setting(gSavedSettings, "SLPoseStudioIKEyeBones", 0);
    const Chain& c = mChains[chain];
    const S32 picked = (S32)setting;
    if (picked >= 1 && picked <= SLPoseIKMath::EYE_SET_BOTH + 1)
        return c.mEyeSets[picked - 1];
    if (avatar && mTipsValid && mTipsAvatar == avatar->getID() && chain < (S32)mTips.size())
        return c.mEyeSets[mTips[chain].mAutoEyes];
    return c.mEyeSets[SLPoseIKMath::EYE_SET_EYES];
}

void SLPoseIK::aimEyes(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool unchanged_ok)
{
    // Each eye aims on its own, from its rest axis (the set's forward in its parent's frame:
    // an eye's local rotation is identity at rest), within the eye limit. Mirror mode must
    // not overwrite one eye with the other's reflection: callers suppress it.
    const Chain& c = mChains[chain];
    const Chain::EyeSet& eyes = gazeEyes(avatar, chain);
    const F32 eye_limit = (c.mLimits.empty() ? 30.f : c.mLimits[0]) * DEG_TO_RAD;
    if (!eyes.mPupil.empty())
    {
        // #1567 pupil eyes (the Open Pony): each eye looks by sliding its bone over the eye
        // and turning it to lie flat on the eye there (the dev kit's Shrinkwrap "Align to
        // Normal"; slid without the turn, the pupil disc cuts into the eye white on one side).
        // The look from the eye's own rest to the goal, in its parent's frame, through the
        // set's mapping to a position offset and a local rotation; the eye limit is the
        // pupil's angular range. Saved and exported like any posed bone.
        // Both are measured from the bone's true rest (the worn mesh's joint position, as the
        // live look-at uses) and written as absolute targets: the poser's own base is where the
        // bone was when it was captured (Freeze holds the frame on screen), which with the live
        // look-at running already carries a pupil offset and turn -- an offset added to that
        // base slid the pupil off the eye by the look-at's offset at capture.
        SLPoseStudio& studio = SLPoseStudio::instance();
        FSPoserAnimator& animator = studio.getAnimator();
        auto held_at = [&](const SLPoseStudio::joint_t& joint, LLVector3& position)
        {
            LLQuaternion rotation;
            LLVector3 scale;
            return animator.isPosingAvatarJoint(avatar, joint) && animator.getJointTargetTransform(avatar, joint, rotation, position, scale);
        };
        for (size_t e = 0; e < eyes.mJoints.size() && e < eyes.mPupil.size(); ++e)
        {
            const std::string& eye = eyes.mJoints[e];
            const SLPoseStudio::joint_t* joint = studio.findJoint(eye);
            LLJoint* bone = avatar->getJoint(eye);
            LLVector3 ep, pp;
            LLQuaternion er, pr;
            if (!joint || !bone || !bone->getParent() || !fkJoint(avatar, eye, ep, er) || !fkParent(avatar, eye, pp, pr))
                continue;
            // The goal seen from the pupil's rest, placed like fkWorld places a bone.
            const LLVector3 rest = SLPoseLookAtMotion::pupilRest(bone);
            LLVector3 rest_offset = rest;
            if (bone->getParent()->getXform()->getScaleChildOffset())
                rest_offset.scaleVec(bone->getParent()->getScale());
            const LLVector3 rest_world = pp + rest_offset * pr;
            const LLVector3 to_goal = (goal - rest_world) * ~pr;
            const SLPoseIKMath::PupilPose pose = SLPoseIKMath::pupilPose(eyes.mPupil[e], toV3(to_goal), toV3(eyes.mForward), eye_limit);
            const LLVector3 want = rest + fromV3(pose.mOffset);
            LLQuaternion turn((F32)pose.mTurn.mAngle, fromV3(pose.mTurn.mAxis));
            turn.normalize();
            if (!want.isFinite() || !turn.isFinite())
                continue;
            // The turn is in the parent's frame: it is the bone's local rotation (LL order:
            // world = local * parent), an absolute target in the poser.
            LLVector3 now;
            const bool held = held_at(*joint, now);
            if (unchanged_ok && held && dist_vec(now, want) < 1.0e-5f && nearlySameRotation(turn * pr, er, HOLD_EPSILON_DOT))
                continue;
            // The poser's position is its public offset from its base: base = held - public.
            const LLVector3 offset_now = animator.getJointPosition(avatar, *joint);
            studio.setBonePosition(avatar, *joint, held ? offset_now + (want - now) : want - bone->getPosition() + offset_now, false);
            if (held_at(*joint, now) && dist_vec(now, want) > 1.0e-6f)
                studio.setBonePosition(avatar, *joint, animator.getJointPosition(avatar, *joint) + (want - now), false);
            applyWorldRotation(avatar, eye, turn * pr, pr, false);
        }
        return;
    }
    for (const std::string& eye : eyes.mJoints)
    {
        LLVector3 ep, pp;
        LLQuaternion er, pr;
        if (!avatar->getJoint(eye) || !fkJoint(avatar, eye, ep, er) || !fkParent(avatar, eye, pp, pr))
            continue;
        LLVector3 rest = eyes.mForward * pr;
        rest.normVec();
        const SLPoseIKMath::EyeTurn t = SLPoseIKMath::eyeTurn(toV3(rest), toV3(goal - ep), eye_limit);
        const LLQuaternion turn((F32)t.mAngle, fromV3(t.mAxis));
        if (unchanged_ok && nearlySameRotation(pr * turn, er, HOLD_EPSILON_DOT))
            continue;
        applyWorldRotation(avatar, eye, pr * turn, pr, false);
    }
}

LLVector3 SLPoseIK::lookWorld(LLVOAvatar* avatar, S32 chain) const
{
    const ChainState& state = mStates[chain];
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (state.mHasLook && rootFrame(avatar, root_pos, root_rot))
        return root_pos + state.mLook * root_rot;
    // Not placed yet: straight ahead of the eyes.
    LLVector3 center, head_pos;
    LLQuaternion head_rot;
    eyeCenter(avatar, chain, center);
    fkJoint(avatar, mChains[chain].mJoints.back(), head_pos, head_rot);
    LLVector3 facing = mChains[chain].mForward * head_rot;
    facing.normVec();
    return center + facing * (2.f * LOOKAT_DISTANCE * handleScale(avatar));
}

void SLPoseIK::solveGaze(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool limits)
{
    // Eyes first: they turn up to their limit; the head takes what is left up to its own,
    // the neck the rest (SLPoseIKMath::gazeShares allocates; tested). Applied root first,
    // each joint turning about the line from where the head faces now to the goal, then the
    // eyes aim from their own positions with what remains, within their cone.
    const Chain& c = mChains[chain];
    const S32 count = (S32)c.mJoints.size();
    auto limitOf = [&](S32 k, F32 fallback) { return ((k < (S32)c.mLimits.size()) ? c.mLimits[k] : fallback) * DEG_TO_RAD; };
    const F32 eye_limit = limitOf(0, 30.f);
    auto facingNow = [&](LLVector3& center, LLVector3& facing, LLQuaternion& head_rot) -> bool
    {
        LLVector3 head_pos;
        if (!eyeCenter(avatar, chain, center) || !fkJoint(avatar, c.mJoints.back(), head_pos, head_rot))
            return false;
        facing = c.mForward * head_rot;
        facing.normVec();
        return true;
    };

    LLVector3 center, facing;
    LLQuaternion head_rot;
    if (!facingNow(center, facing, head_rot) || (goal - center).length() < 1.0e-3f)
        return;
    // Bottom-up allocation: eyes, then the joints tip -> root.
    F32 rest = llmax(0.f, angleOf(facing, goal - center) - eye_limit);
    std::vector<F32> share(count, 0.f);
    for (S32 j = count - 1; j >= 0; --j)
    {
        const F32 limit = limitOf(1 + (count - 1 - j), (j == count - 1) ? 45.f : 35.f);
        share[j] = llmin(rest, limit);
        rest -= share[j];
    }
    // Top-down application.
    for (S32 j = 0; j < count; ++j)
    {
        if (share[j] <= 1.0e-4f || !facingNow(center, facing, head_rot))
            continue;
        LLVector3 axis = facing % (goal - center);
        if (axis.length() < 1.0e-6f)
            break;
        axis.normVec();
        const F32 angle = llmin(share[j], angleOf(facing, goal - center));
        LLVector3 jp, pp;
        LLQuaternion jr, pr;
        if (!fkJoint(avatar, c.mJoints[j], jp, jr) || !fkParent(avatar, c.mJoints[j], pp, pr))
            return;
        applyWorldRotation(avatar, c.mJoints[j], jr * LLQuaternion(angle, axis), pr, limits);
    }
    // The eyes (the set "Eye IK" picks): each turned toward the goal from its own rest axis
    // by at most the eye limit. Each aims on its own: mirror mode must not overwrite one eye
    // with the other's reflection.
    SLPoseStudio& studio = SLPoseStudio::instance();
    studio.setMirrorSuppressed(true);
    aimEyes(avatar, chain, goal, false);
    studio.setMirrorSuppressed(false);
}

bool SLPoseIK::isBelow(LLVOAvatar* avatar, const std::string& joint, const std::string& ancestor) const
{
    for (LLJoint* walk = avatar ? avatar->getJoint(joint) : nullptr; walk; walk = walk->getParent())
    {
        if (walk->getName() == ancestor)
            return true;
    }
    return false;
}

void SLPoseIK::carryTargets(LLVOAvatar* avatar)
{
    // The dragged bone (root diamond: the pelvis; hips box: the pelvis; chest box: the chest)
    // carries every chain below it. A carried chain's target and pole ride along rigidly (its
    // bones did too, so nothing is re-solved and a gap to a placed target is kept); a held
    // one stays where it was at mouse down and its chain re-solves to reach it. Held: the
    // planted legs under the hips box; every chain under the root diamond or the chest box
    // when "IK targets follow the hips" (SLPoseStudioIKFollowRoot) is off. Each frame is a
    // pure function of the mouse-down state.
    static LLCachedControl<bool> follow(gSavedSettings, "SLPoseStudioIKFollowRoot", true);
    if (mDrag.mBodyJoint.empty())
        return;
    LLVector3 now_pos, root_pos;
    LLQuaternion now_rot, root_rot;
    if (!fkJoint(avatar, mDrag.mBodyJoint, now_pos, now_rot) || !rootFrame(avatar, root_pos, root_rot))
        return;
    const LLQuaternion delta = ~mDrag.mBodyStartRot * now_rot;
    auto carry = [&](const LLVector3& world) { return now_pos + ((world - mDrag.mBodyStartPos) * ~mDrag.mBodyStartRot) * now_rot; };
    auto toWorld = [&](const LLVector3& local) { return root_pos + local * root_rot; };
    auto toLocal = [&](const LLVector3& world) { return (world - root_pos) * ~root_rot; };
    const bool hips = mChains[mDrag.mChain].mSolver == SOLVER_HIPS;

    for (S32 i = 0; i < (S32)mChains.size() && i < (S32)mDrag.mStatesBefore.size(); ++i)
    {
        if (i == mDrag.mChain || !chainUsable(avatar, i) || !isBelow(avatar, chainJoints(i).front(), mDrag.mBodyJoint))
            continue;
        const ChainState& before = mDrag.mStatesBefore[i];
        ChainState& state = mStates[i];
        // Only targets the user owns ever hold; one still on its bones rides along (a target
        // follows its bones until placed). Root diamond: placed / pinned targets hold when
        // the checkbox is off. Hips box: legs stay planted (and anything pinned, e.g. a
        // pony's front hooves). Chest box: pinned ones hold, placed ones per the checkbox.
        const bool owned = before.mPlaced || before.mPinned;
        bool hold = !follow && owned;
        if (hips)
            hold = mChains[i].mPlanted || before.mPinned;
        else if (mChains[mDrag.mChain].mBox)
            hold = before.mPinned || (!follow && before.mPlaced);
        if (before.mHasLook && !hold)
            state.mLook = toLocal(carry(toWorld(before.mLook)));
        if (!usesTarget(i) || !before.mHasTarget)
            continue;
        if (hold)
        {
            state.mTargetPos = before.mTargetPos;
            state.mTargetRot = before.mTargetRot;
            state.mPole = before.mPole;
            LLVector3 target_pos;
            LLQuaternion target_rot;
            if (targetWorld(avatar, i, target_pos, target_rot))
                solveChain(avatar, i, target_pos, &target_rot);
        }
        else
        {
            state.mTargetPos = toLocal(carry(toWorld(before.mTargetPos)));
            LLQuaternion rot = (before.mTargetRot * root_rot) * delta;
            rot.normalize();
            state.mTargetRot = rot * ~root_rot;
            state.mTargetRot.normalize();
            if (before.mHasPole)
                state.mPole = toLocal(carry(toWorld(before.mPole)));
        }
        markSeen(avatar, i);
        if (state.mPinned)
            state.mPinBones = chainTargets(avatar, i);
    }
    // The body drag carried what it carries: the anchors start over from here.
    recordAnchors(avatar);
}

namespace
{
SLPoseIKMath::Frame toFrame(const LLVector3& pos, const LLQuaternion& rot)
{
    SLPoseIKMath::Frame f;
    f.mOrigin = toV3(pos);
    f.mX = toV3(LLVector3::x_axis * rot);
    f.mY = toV3(LLVector3::y_axis * rot);
    f.mZ = toV3(LLVector3::z_axis * rot);
    return f;
}
} // namespace

bool SLPoseIK::followFrame(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot) const
{
    const std::string& root = chainJoints(chain).front();
    const char* body = (root != "mChest" && isBelow(avatar, root, "mChest")) ? "mChest" : "mPelvis";
    return fkJoint(avatar, body, pos, rot);
}

bool SLPoseIK::anchorRelative(LLVOAvatar* avatar, S32 chain, bool follow_root, LLVector3& pos, LLQuaternion& rot) const
{
    // The chain root's parent, in the follow frame: the avatar root, or with "IK targets
    // follow the hips" off the body bone the hips / chest boxes move (the chest for chains
    // hanging below it, else the pelvis): then moving or turning the pelvis, torso or chest
    // carries nothing, as the boxes do with the box off; a bone turned below the chest (the
    // head, a collar) still does.
    LLJoint* root = avatar->getJoint(chainJoints(chain).front());
    LLJoint* anchor = root ? root->getParent() : nullptr;
    LLVector3 ap, fp;
    LLQuaternion ar, fr;
    if (!anchor || !fkWorld(avatar, anchor, ap, ar))
        return false;
    if (!(follow_root ? rootFrame(avatar, fp, fr) : followFrame(avatar, chain, fp, fr)))
        return false;
    // LL order: world = local * parent.
    pos = (ap - fp) * ~fr;
    rot = ar * ~fr;
    rot.normalize();
    return true;
}

void SLPoseIK::recordAnchor(LLVOAvatar* avatar, S32 chain)
{
    static LLCachedControl<bool> follow(gSavedSettings, "SLPoseStudioIKFollowRoot", true);
    ChainState& state = mStates[chain];
    state.mAnchorFollowRoot = follow;
    state.mHasAnchor = anchorRelative(avatar, chain, follow, state.mAnchorPos, state.mAnchorRot);
}

void SLPoseIK::recordAnchors(LLVOAvatar* avatar)
{
    for (S32 i = 0; i < (S32)mChains.size() && i < (S32)mStates.size(); ++i)
    {
        if (chainUsable(avatar, i))
            recordAnchor(avatar, i);
    }
}

void SLPoseIK::followAnchors(LLVOAvatar* avatar)
{
    // A placed target keeps its place RELATIVE to the bone its chain hangs from: turning the
    // head (head IK, nose reach, the dials, an FK drag) carries the ear targets, turning the
    // chest carries the hands'. Rigid, like the bones under it, so nothing re-solves. Pinned
    // targets (and planted legs) hold and their chains re-solve (holdPins). A body drag
    // carries its own (carryTargets); the anchors are just kept current then.
    static LLCachedControl<bool> follow(gSavedSettings, "SLPoseStudioIKFollowRoot", true);
    const bool body_drag = mDrag.mActive && !mDrag.mBodyJoint.empty();
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return;
    auto toWorld = [&](const LLVector3& local) { return root_pos + local * root_rot; };
    auto toLocal = [&](const LLVector3& world) { return (world - root_pos) * ~root_rot; };
    for (S32 i = 0; i < (S32)mChains.size() && i < (S32)mStates.size(); ++i)
    {
        if (!chainUsable(avatar, i))
            continue;
        ChainState& state = mStates[i];
        LLVector3 rel_pos;
        LLQuaternion rel_rot;
        if (!state.mHasAnchor || state.mAnchorFollowRoot != (bool)follow
            || !anchorRelative(avatar, i, follow, rel_pos, rel_rot))
        {
            recordAnchor(avatar, i);
            continue;
        }
        if ((rel_pos - state.mAnchorPos).length() <= HOLD_EPSILON_M && nearlySameRotation(rel_rot, state.mAnchorRot, HOLD_EPSILON_DOT))
            continue;
        const Chain& c = mChains[i];
        const bool dragged = mDrag.mActive && mDrag.mChain == i;
        const bool has_target = usesTarget(i) && state.mHasTarget;
        const bool target_follows = has_target && SLPoseIKMath::targetFollowsAnchor(state.mPlaced, state.mPinned, c.mPlanted, dragged);
        const bool look_follows = c.mSolver == SOLVER_GAZE && SLPoseIKMath::targetFollowsAnchor(state.mHasLook, state.mPinned, false, dragged);
        LLVector3 follow_pos;
        LLQuaternion follow_rot;
        if (!body_drag && (target_follows || look_follows)
            && (follow ? rootFrame(avatar, follow_pos, follow_rot) : followFrame(avatar, i, follow_pos, follow_rot)))
        {
            // Where the anchor was (in the world, the follow frame as it is now) and is.
            const LLVector3 was_pos = follow_pos + state.mAnchorPos * follow_rot;
            LLQuaternion was_rot = state.mAnchorRot * follow_rot;
            was_rot.normalize();
            LLVector3 now_pos;
            LLQuaternion now_rot;
            LLJoint* anchor = avatar->getJoint(chainJoints(i).front())->getParent();
            fkWorld(avatar, anchor, now_pos, now_rot);
            const SLPoseIKMath::Frame was = toFrame(was_pos, was_rot);
            const SLPoseIKMath::Frame now = toFrame(now_pos, now_rot);
            auto carry = [&](const LLVector3& local) { return toLocal(fromV3(SLPoseIKMath::carryPoint(was, now, toV3(toWorld(local))))); };
            if (look_follows)
                state.mLook = carry(state.mLook);
            if (target_follows)
            {
                state.mTargetPos = carry(state.mTargetPos);
                LLQuaternion rot = ((state.mTargetRot * root_rot) * ~was_rot) * now_rot;    // world, carried
                rot = rot * ~root_rot;
                rot.normalize();
                state.mTargetRot = rot;
                if (state.mHasPole)
                    state.mPole = carry(state.mPole);
                markSeen(avatar, i);
            }
        }
        recordAnchor(avatar, i);
    }
}

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------

void SLPoseIK::pinChain(LLVOAvatar* avatar, S32 chain)
{
    // A pin holds the chain's IK target where it is (out of reach included).
    ChainState& state = mStates[chain];
    if (mChains[chain].mSolver == SOLVER_GAZE)
    {
        // A pinned gaze is a look-at: the eyes keep finding that point whatever moves the head.
        const LLVector3 look = lookWorld(avatar, chain);
        LLVector3 root_pos;
        LLQuaternion root_rot;
        if (!rootFrame(avatar, root_pos, root_rot))
            return;
        state.mLook = (look - root_pos) * ~root_rot;
        state.mHasLook = true;
        state.mPinned = true;
        return;
    }
    if (!usesTarget(chain))
        return;
    if (!state.mHasTarget)
        snapTarget(avatar, chain);
    state.mPinned = true;
    state.mPlaced = true;
    state.mPinBones = chainTargets(avatar, chain);
}

// ---------------------------------------------------------------------------
// IK targets
// ---------------------------------------------------------------------------

bool SLPoseIK::usesTarget(S32 chain) const
{
    if (chain < 0 || chain >= (S32)mChains.size())
        return false;
    const ESolver solver = mChains[chain].mSolver;
    return solver == SOLVER_TWOBONE || solver == SOLVER_CCD;
}

bool SLPoseIK::rootFrame(LLVOAvatar* avatar, LLVector3& pos, LLQuaternion& rot) const
{
    // Targets live in the avatar's frame: they come along when it walks or turns.
    LLJoint* root = avatar ? avatar->getRootJoint() : nullptr;
    if (!root)
        return false;
    pos = root->getWorldPosition();
    rot = root->getWorldRotation();
    return true;
}

bool SLPoseIK::chainPose(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot, LLVector3& cone) const
{
    LLVector3 tip_pos, end_pos, root_pos;
    LLQuaternion end_rot, root_rot;
    if (!fkEffector(avatar, chain, tip_pos, end_pos, end_rot) || !rootFrame(avatar, root_pos, root_rot))
        return false;
    LLVector3 dir = tip_pos - end_pos;
    if (dir.length() < 1.0e-3f)
    {
        const std::vector<std::string>& joints = chainJoints(chain);
        LLVector3 prev_pos;
        LLQuaternion prev_rot;
        if (fkJoint(avatar, joints[joints.size() - 2], prev_pos, prev_rot))
            dir = end_pos - prev_pos;
    }
    if (dir.length() < 1.0e-4f)
        dir = LLVector3::x_axis * end_rot;
    dir.normVec();
    cone = dir * ~end_rot;
    pos = (tip_pos - root_pos) * ~root_rot;
    rot = end_rot * ~root_rot;
    rot.normalize();
    return true;
}

bool SLPoseIK::targetWorld(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot) const
{
    if (chain < 0 || chain >= (S32)mStates.size() || !mStates[chain].mHasTarget)
        return false;
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return false;
    const ChainState& state = mStates[chain];
    pos = root_pos + state.mTargetPos * root_rot;
    rot = state.mTargetRot * root_rot;
    rot.normalize();
    return true;
}

void SLPoseIK::setTargetWorld(LLVOAvatar* avatar, S32 chain, const LLVector3* pos, const LLQuaternion* rot)
{
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return;
    ChainState& state = mStates[chain];
    if (pos)
        state.mTargetPos = (*pos - root_pos) * ~root_rot;
    if (rot)
    {
        state.mTargetRot = *rot * ~root_rot;
        state.mTargetRot.normalize();
    }
}

LLVector3 SLPoseIK::poleWorld(LLVOAvatar* avatar, S32 chain) const
{
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return LLVector3::zero;
    return root_pos + mStates[chain].mPole * root_rot;
}

void SLPoseIK::setPoleWorld(LLVOAvatar* avatar, S32 chain, const LLVector3& pos)
{
    LLVector3 root_pos;
    LLQuaternion root_rot;
    if (!rootFrame(avatar, root_pos, root_rot))
        return;
    mStates[chain].mPole = (pos - root_pos) * ~root_rot;
    mStates[chain].mHasPole = true;
}

LLVector3 SLPoseIK::defaultPole(LLVOAvatar* avatar, S32 chain) const
{
    const Chain& c = mChains[chain];
    LLVector3 a, b, e;
    LLQuaternion wa, wb, we;
    if (c.mJoints.size() < 3 || !fkJoint(avatar, c.mJoints[0], a, wa) || !fkJoint(avatar, c.mJoints[1], b, wb)
        || !fkJoint(avatar, c.mJoints[2], e, we))
        return b;
    LLVector3 n = e - a;
    if (n.length() < 1.0e-4f)
        return b;
    n.normVec();
    // The side the elbow / knee points to (for a straight limb: opposite where the lower
    // bone would swing), half a limb length out in front of it.
    LLVector3 side = b - a;
    side -= n * (side * n);
    if (side.length() < 0.05f * (b - a).length())
    {
        side = -(c.mBend * wa);
        side -= n * (side * n);
    }
    if (side.length() < 1.0e-6f)
        return b;
    side.normVec();
    return b + side * (POLE_DISTANCE * ((b - a).length() + (e - b).length()));
}

void SLPoseIK::snapTarget(LLVOAvatar* avatar, S32 chain)
{
    LLVector3 pos, cone;
    LLQuaternion rot;
    if (!usesTarget(chain) || !chainPose(avatar, chain, pos, rot, cone))
        return;
    ChainState& state = mStates[chain];
    state.mHasTarget = true;
    state.mPlaced = false;
    state.mTargetPos = state.mSeenPos = pos;
    state.mTargetRot = state.mSeenRot = rot;
    state.mConeLocal = cone;
    recordAnchor(avatar, chain);
}

void SLPoseIK::markSeen(LLVOAvatar* avatar, S32 chain)
{
    LLVector3 pos, cone;
    LLQuaternion rot;
    if (!usesTarget(chain) || !chainPose(avatar, chain, pos, rot, cone))
        return;
    mStates[chain].mSeenPos = pos;
    mStates[chain].mSeenRot = rot;
    recordAnchor(avatar, chain);
}

void SLPoseIK::snapAllTargets(LLVOAvatar* avatar)
{
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!usesTarget(i) || !chainUsable(avatar, i))
            continue;
        snapTarget(avatar, i);
        mStates[i].mHasPole = false;
        if (mStates[i].mPinned)
            mStates[i].mPinBones = chainTargets(avatar, i);
    }
}

void SLPoseIK::updateTargets(LLVOAvatar* avatar)
{
    followAnchors(avatar);
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!usesTarget(i) || !chainUsable(avatar, i))
            continue;
        ChainState& state = mStates[i];
        if (!state.mHasTarget)
        {
            // First shown: from the pose as it is.
            snapTarget(avatar, i);
            continue;
        }
        // A placed target (dragged, turned, pinned or given a pole) is the user's: it stays
        // where it was put, position AND orientation, whatever moves the bones (the hips,
        // other handles, dials, mirror). Only Snap, a loaded pose or undo move it.
        // A body drag (root, hips, chest) moves the targets itself (carryTargets).
        if (state.mPlaced || state.mPinned || (mDrag.mActive && (mDrag.mChain == i || !mDrag.mBodyJoint.empty())))
            continue;
        LLVector3 pos, cone;
        LLQuaternion rot;
        if (!chainPose(avatar, i, pos, rot, cone))
            continue;
        if ((pos - state.mSeenPos).length() > HOLD_EPSILON_M || !nearlySameRotation(rot, state.mSeenRot, HOLD_EPSILON_DOT))
        {
            // Never placed: the handle still marks where its chain ends, so it follows.
            snapTarget(avatar, i);
            state.mHasPole = false;
        }
    }
}

void SLPoseIK::restoreTargets(LLVOAvatar* avatar, const std::vector<ChainState>& states)
{
    for (S32 i = 0; i < (S32)llmin(states.size(), mStates.size()); ++i)
    {
        ChainState& state = mStates[i];
        state.mHasTarget = states[i].mHasTarget;
        state.mPlaced = states[i].mPlaced;
        state.mTargetPos = states[i].mTargetPos;
        state.mTargetRot = states[i].mTargetRot;
        state.mConeLocal = states[i].mConeLocal;
        state.mHasPole = states[i].mHasPole;
        state.mPole = states[i].mPole;
        state.mHasLook = states[i].mHasLook;
        state.mLook = states[i].mLook;
        markSeen(avatar, i);
        if (state.mPinned)
            state.mPinBones = chainTargets(avatar, i);
    }
}

void SLPoseIK::snapToBones()
{
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || mDrag.mActive)
        return;
    syncAvatar(avatar);
    snapAllTargets(avatar);
}



void SLPoseIK::holdPins(LLVOAvatar* avatar, S32 except)
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    const bool was_applying = mApplying;
    mApplying = true;
    // A pinned chain holds on its own: it never drags its mirror partner along.
    studio.setMirrorSuppressed(true);
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        ChainState& state = mStates[i];
        if (i != except && state.mPinned && state.mHasLook && mChains[i].mSolver == SOLVER_GAZE && chainUsable(avatar, i))
        {
            // A pinned look-at: only the eyes re-aim (the head and neck are the user's).
            aimEyes(avatar, i, lookWorld(avatar, i), true);
            continue;
        }
        if (i == except || !state.mPinned || !state.mHasTarget || !usesTarget(i) || !chainUsable(avatar, i))
            continue;
        LLVector3 pos, cone;
        LLQuaternion rot;
        if (!chainPose(avatar, i, pos, rot, cone))
            continue;
        // Unchanged since it last held: nothing to do (an unreachable target stays unreached).
        if ((pos - state.mSeenPos).length() <= HOLD_EPSILON_M && nearlySameRotation(rot, state.mSeenRot, HOLD_EPSILON_DOT))
            continue;
        LLVector3 target_pos;
        LLQuaternion target_rot;
        if (!targetWorld(avatar, i, target_pos, target_rot))
            continue;
        solveChain(avatar, i, target_pos, &target_rot);
        markSeen(avatar, i);
        state.mPinBones = chainTargets(avatar, i);
    }
    studio.setMirrorSuppressed(false);
    mApplying = was_applying;
}

bool SLPoseIK::hasPins() const
{
    for (const ChainState& state : mStates)
    {
        if (state.mPinned)
            return true;
    }
    return false;
}

void SLPoseIK::clearPins()
{
    for (ChainState& state : mStates)
        state.mPinned = false;
}

// ---------------------------------------------------------------------------
// Studio notifications
// ---------------------------------------------------------------------------

void SLPoseIK::onStudioEdited(const LLUUID& avatar_id, bool committed)
{
    if (mApplying || avatar_id != mAvatarID || mAvatarID.isNull())
        return;

    SLPoseStudio& studio = SLPoseStudio::instance();
    // Edited some other way (dials, load, reset, sync, per-bone undo): the drag history
    // no longer describes the pose. An ease reaching its pose (settling) is not an edit:
    // it must not wipe the history (an eased IK undo ends with one).
    if (committed && !studio.isBlendSettling())
    {
        mUndo.clear();
        mRedo.clear();
    }
    if (mDrag.mActive)
        return;
    // A loaded pose / sequence step (it records its ease-in; a direct edit records none).
    const bool loaded = committed && studio.getLastEditEase(avatar_id) >= 0.f;
    if (!hasPins() && !loaded)
        return;

    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || avatar->getID() != avatar_id)
        return;
    if (studio.getBlendRemaining(avatar_id) > 0.f)
        return;     // easing in: wait for it to arrive (its settling edit comes back here)
    if (loaded)
    {
        // The targets -- placed ones too -- move to where the pose put the chains.
        snapAllTargets(avatar);
        return;
    }

    // A pinned chain whose own bones were changed directly re-pins where it now ends (the
    // edit wins); every other pinned chain holds its pin against the change.
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        ChainState& state = mStates[i];
        if (!state.mPinned)
            continue;
        const std::vector<LLQuaternion> now = chainTargets(avatar, i);
        bool changed = now.size() != state.mPinBones.size();
        for (size_t b = 0; !changed && b < now.size(); ++b)
            changed = !nearlySameRotation(now[b], state.mPinBones[b], BONE_CHANGED_DOT);
        if (changed)
        {
            snapTarget(avatar, i);
            pinChain(avatar, i);
        }
    }
    // Runs inside the edit's own notification: a committed edit is sent with the held
    // chains included, nothing more to notify.
    holdPins(avatar, -1);
}

void SLPoseIK::onStudioPosing(const LLUUID& avatar_id, bool posing)
{
    if (!posing && avatar_id == mAvatarID)
        reset();
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

void SLPoseIK::buildHandles(LLVOAvatar* avatar, std::vector<Handle>& out) const
{
    out.clear();
    if (!ikShown())
        return;     // the FK bones view alone (#1566)
    FkMemoScope fk_memo(*this);
    const F32 scale = handleScale(avatar);
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (!chainUsable(avatar, i))
            continue;
        const Chain& c = mChains[i];
        Handle handle;
        handle.mChain = i;
        LLVector3 tip_pos, end_pos;
        LLQuaternion end_rot;
        if (!fkEffector(avatar, i, tip_pos, end_pos, end_rot))
            continue;
        handle.mRot = end_rot;
        switch (c.mSolver)
        {
            case SOLVER_LOOKAT:
            {
                LLVector3 facing = c.mForward * end_rot;
                facing.normVec();
                handle.mDir = facing;
                handle.mPos = end_pos + facing * (LOOKAT_DISTANCE * scale);
                break;
            }
            case SOLVER_PELVIS:
            {
                // The root diamond sits under the hips (the hips box is on them, #1566).
                LLVector3 root_pos;
                LLQuaternion root_rot;
                LLVector3 down = -LLVector3::z_axis;
                if (rootFrame(avatar, root_pos, root_rot))
                    down = -LLVector3::z_axis * root_rot;
                handle.mPos = end_pos + down * (ROOT_HANDLE_DROP * scale);
                handle.mDir = LLVector3::z_axis * end_rot;
                break;
            }
            case SOLVER_HIPS:
                handle.mPos = end_pos;
                handle.mDir = LLVector3::z_axis * end_rot;
                break;
            case SOLVER_REACH:
            {
                // On the nose.
                LLVector3 facing = c.mForward * end_rot;
                facing.normVec();
                handle.mDir = facing;
                handle.mPos = hasTip(i) ? tip_pos : end_pos + facing * (c.mTipLength * scale);
                break;
            }
            case SOLVER_GAZE:
            {
                LLVector3 facing = c.mForward * end_rot;
                facing.normVec();
                handle.mDir = facing;
                handle.mPos = lookWorld(avatar, i);
                break;
            }
            default:
            {
                // On the tip (toe / hoof) when the chain has one, pointing on from the end.
                LLVector3 dir = tip_pos - end_pos;
                if (dir.length() < 1.0e-3f)
                {
                    LLVector3 prev_pos;
                    LLQuaternion prev_rot;
                    fkJoint(avatar, chainJoints(i)[chainJoints(i).size() - 2], prev_pos, prev_rot);
                    dir = end_pos - prev_pos;
                }
                if (dir.length() < 1.0e-4f)
                    dir = LLVector3::x_axis * end_rot;
                dir.normVec();
                handle.mPos = tip_pos;
                handle.mDir = dir;
                // An IK target: drawn where it is, turned as it is turned -- never re-derived
                // from the bones it drives.
                LLVector3 target_pos;
                LLQuaternion target_rot;
                if (usesTarget(i) && targetWorld(avatar, i, target_pos, target_rot))
                {
                    handle.mPos = target_pos;
                    handle.mRot = target_rot;
                    handle.mDir = mStates[i].mConeLocal * target_rot;
                    handle.mDir.normVec();
                }
                break;
            }
        }
        // The head's handle being moved sits on the point the drag asks for.
        if (mDrag.mActive && mDrag.mChain == i && !mDrag.mPole && !mDrag.mRotate
            && (c.mSolver == SOLVER_LOOKAT || c.mSolver == SOLVER_REACH || c.mSolver == SOLVER_GAZE))
            handle.mPos = mDrag.mGoal;
        out.push_back(handle);

        if (c.mSolver == SOLVER_TWOBONE)
        {
            Handle pole;
            pole.mChain = i;
            pole.mPole = true;
            LLVector3 mid_pos;
            LLQuaternion mid_rot;
            fkJoint(avatar, c.mJoints[1], mid_pos, mid_rot);
            // The pole target: where it was put, else in front of the elbow / knee.
            pole.mPos = mStates[i].mHasPole ? poleWorld(avatar, i) : defaultPole(avatar, i);
            if (mDrag.mActive && mDrag.mChain == i && mDrag.mPole)
                pole.mPos = mDrag.mGoal;
            pole.mDir = LLVector3::z_axis;
            pole.mRot = mid_rot;
            out.push_back(pole);
        }
    }
}

bool SLPoseIK::pick(LLVOAvatar* avatar, S32 x, S32 y, Handle& out) const
{
    std::vector<Handle> handles;
    buildHandles(avatar, handles);
    if (handles.empty())
        return false;

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 origin = camera->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();
    const F32 view_height = (F32)llmax(1, gViewerWindow->getWorldViewHeightScaled());
    const F32 px_to_m = 2.f * tanf(camera->getView() * 0.5f) / view_height;   // per metre of distance
    const F32 scale = handleScale(avatar);
    const F32 length = BASE_HANDLE_LENGTH * scale;

    F32 best_t = F32_MAX;
    bool found = false;
    for (const Handle& handle : handles)
    {
        const Chain& hc = mChains[handle.mChain];
        const bool point = handle.mPole || hc.mSolver == SOLVER_PELVIS || hc.mSolver == SOLVER_HIPS
                           || hc.mSolver == SOLVER_GAZE || hc.mBox;   // diamonds and boxes
        const LLVector3 center = point ? handle.mPos : handle.mPos + handle.mDir * (length * 0.5f);
        const LLVector3 oc = center - origin;
        const F32 along = oc * ray;
        if (along <= 0.f)
            continue;
        const F32 world_radius = handle.mPole ? length * 0.4f : length * 0.65f;
        const F32 radius = llmax(world_radius, MIN_PICK_PX * (handle.mPole ? 0.8f : 1.f) * px_to_m * along);
        const F32 miss2 = oc.lengthSquared() - along * along;
        if (miss2 > radius * radius)
            continue;
        // End handles win over pole handles at the same depth.
        const F32 t = along - (handle.mPole ? 0.f : radius * 0.5f);
        if (t < best_t)
        {
            best_t = t;
            out = handle;
            found = true;
        }
    }
    return found;
}

bool SLPoseIK::rayPlane(S32 x, S32 y, const LLVector3& point, const LLVector3& normal, LLVector3& hit) const
{
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();
    const F32 denom = ray * normal;
    if (llabs(denom) < 1.0e-4f)
        return false;
    const F32 t = ((point - origin) * normal) / denom;
    if (t <= 0.f)
        return false;
    hit = origin + ray * t;
    return true;
}

// ---------------------------------------------------------------------------
// Rotate rings
// ---------------------------------------------------------------------------

bool SLPoseIK::ringsEnabled() const
{
    static LLCachedControl<bool> rings(gSavedSettings, "SLPoseStudioIKRotateRings", true);
    return rings;
}

void SLPoseIK::ringAxes(const Handle& handle, LLVector3 axes[3]) const
{
    // The end bone's own axes (local): they turn with the bone, so the ring about the bone's
    // length twists it and the other two bend it, wherever the limb points.
    axes[0] = LLVector3::x_axis * handle.mRot;
    axes[1] = LLVector3::y_axis * handle.mRot;
    axes[2] = LLVector3::z_axis * handle.mRot;
    for (S32 i = 0; i < 3; ++i)
        axes[i].normVec();
}

bool SLPoseIK::selectedHandle(LLVOAvatar* avatar, Handle& out) const
{
    if (!ringsEnabled())
        return false;
    if (mSelectedChain < 0)
    {
        // An FK bone (#1566): its rings sit on its head, in its own axes. mChain stays -1.
        FKBone bone;
        if (mSelectedBone.empty() || !fkShown() || !findBone(avatar, mSelectedBone, bone))
            return false;
        out = Handle();
        out.mPos = bone.mHead;
        out.mRot = bone.mRot;
        out.mDir = bone.mTail - bone.mHead;
        out.mDir.normVec();
        return true;
    }
    std::vector<Handle> handles;
    buildHandles(avatar, handles);
    for (const Handle& handle : handles)
    {
        if (handle.mChain == mSelectedChain && !handle.mPole)
        {
            out = handle;
            return true;
        }
    }
    return false;
}

F32 SLPoseIK::metersPerPixel(const LLVector3& point) const
{
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const F32 view_height = (F32)llmax(1, gViewerWindow->getWorldViewHeightScaled());
    const F32 depth = llmax(0.05f, (point - camera->getOrigin()) * camera->getAtAxis());
    return 2.f * tanf(camera->getView() * 0.5f) / view_height * depth;
}

F32 SLPoseIK::ringRadius(const LLVector3& center) const
{
    return RING_RADIUS_PX * metersPerPixel(center);
}

bool SLPoseIK::pickRing(LLVOAvatar* avatar, S32 x, S32 y, Handle& handle, S32& axis) const
{
    // LLManipRotate::highlightManipulators: the mouse on each ring's plane, its distance
    // from the radius (scaled by how face-on the ring is) within a pick band; the ring
    // half toward the camera wins. Edge-on rings: the line they make across the view.
    axis = -1;
    Handle selected;
    if (!selectedHandle(avatar, selected))
        return false;
    const LLVector3 center = selected.mPos;
    const F32 mpp = metersPerPixel(center);
    const F32 radius = RING_RADIUS_PX * mpp;
    const F32 threshold = RING_PICK_PX * mpp;
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLVector3 axes[3];
    ringAxes(selected, axes);

    F32 best_front = -F32_MAX;
    for (S32 a = 0; a < 3; ++a)
    {
        const F32 proj = llabs(axes[a] * to_cam);
        LLVector3 hit;
        if (!rayPlane(x, y, center, axes[a], hit))
            continue;
        const LLVector3 v = hit - center;
        const F32 dist = v.length();
        if (llabs(dist - radius) * llmax(0.05f, proj) >= threshold)
            continue;
        const F32 front = (dist > 1.0e-6f) ? (v * to_cam) / dist : 0.f;
        if (front >= -0.05f && front > best_front)
        {
            best_front = front;
            axis = a;
        }
    }
    if (axis < 0)
    {
        LLVector3 hit;
        if (rayPlane(x, y, center, to_cam, hit))
        {
            const LLVector3 v = hit - center;
            for (S32 a = 0; a < 3; ++a)
            {
                if (llabs(axes[a] * to_cam) < 0.1f && llabs(v * axes[a]) < threshold && v.length() < radius + threshold)
                {
                    axis = a;
                    break;
                }
            }
        }
    }
    if (axis < 0)
        return false;
    handle = selected;
    return true;
}

// ---------------------------------------------------------------------------
// Translate gizmo
// ---------------------------------------------------------------------------

bool SLPoseIK::gizmoRotateMode() const
{
    // Frozen while a gizmo is dragged; otherwise Ctrl held (as the build tool's Ctrl swaps
    // move for rotate).
    if (mDrag.mActive)
        return mDrag.mRotate;
    return gKeyboard && (gKeyboard->currentMask(true) & MASK_CONTROL);
}

void SLPoseIK::moveAxes(const LLVector3& center, LLVector3 axes[3]) const
{
    // World axes, each flipped to the side facing the camera (LLManipTranslate) so the
    // arrows and squares are never behind the handle.
    const LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    for (S32 i = 0; i < 3; ++i)
    {
        axes[i].clear();
        axes[i].mV[i] = (to_cam.mV[i] >= 0.f) ? 1.f : -1.f;
    }
}

F32 SLPoseIK::mouseAlongLine(S32 x, S32 y, const LLVector3& point, const LLVector3& dir) const
{
    // Closest points of the line point + s * dir and the mouse ray (dir and ray unit).
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();
    const LLVector3 w0 = point - origin;
    const F32 b = dir * ray;
    const F32 denom = 1.f - b * b;
    if (denom < 1.0e-4f)
        return 0.f;     // looking down the line: no meaningful position along it
    return (b * (ray * w0) - (dir * w0)) / denom;
}

bool SLPoseIK::pickMove(LLVOAvatar* avatar, S32 x, S32 y, Handle& handle, S32& part) const
{
    part = -1;
    Handle selected;
    if (!selectedHandle(avatar, selected) || gizmoRotateMode() || selected.mChain < 0)
        return false;   // FK bones have rings only
    const LLVector3 center = selected.mPos;
    const F32 mpp = metersPerPixel(center);
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLVector3 axes[3];
    moveAxes(center, axes);
    const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();

    // Plane squares first (they sit inside the arrows' reach, nearer the center).
    for (S32 n = 0; n < 3; ++n)
    {
        if (llabs(axes[n] * to_cam) < MOVE_PLANE_MIN_DOT)
            continue;   // edge-on: hidden
        LLVector3 hit;
        if (!rayPlane(x, y, center, axes[n], hit))
            continue;
        const LLVector3& u = axes[(n + 1) % 3];
        const LLVector3& v = axes[(n + 2) % 3];
        const F32 su = ((hit - center) * u) / mpp;
        const F32 sv = ((hit - center) * v) / mpp;
        if (su >= MOVE_PLANE_LO_PX && su <= MOVE_PLANE_HI_PX && sv >= MOVE_PLANE_LO_PX && sv <= MOVE_PLANE_HI_PX)
        {
            part = 3 + n;
            handle = selected;
            return true;
        }
    }

    // Arrows: the ray's closest approach to each shaft (start to the head's tip).
    F32 best = F32_MAX;
    for (S32 a = 0; a < 3; ++a)
    {
        if (llabs(axes[a] * to_cam) > MOVE_ARROW_MAX_DOT)
            continue;   // pointing at the camera: hidden
        const F32 s = llclamp(mouseAlongLine(x, y, center, axes[a]), MOVE_ARROW_START_PX * mpp,
                              (MOVE_ARROW_LENGTH_PX + MOVE_HEAD_LENGTH_PX) * mpp);
        const LLVector3 on_axis = center + axes[a] * s;
        const LLVector3 oc = on_axis - origin;
        const F32 along = oc * ray;
        if (along <= 0.f)
            continue;
        const F32 miss = sqrtf(llmax(0.f, oc.lengthSquared() - along * along));
        if (miss < MOVE_PICK_PX * mpp && miss < best)
        {
            best = miss;
            part = a;
        }
    }
    if (part < 0)
        return false;
    handle = selected;
    return true;
}

void SLPoseIK::drawRibbon(const LLVector3& from, const LLVector3& to, F32 half_width, const LLColor4& color) const
{
    // A flat band facing the camera, both sides.
    LLVector3 dir = to - from;
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - from;
    LLVector3 side = dir % to_cam;
    if (side.length() < 1.0e-6f)
        return;
    side.normVec();
    side *= half_width;
    const LLVector3 a = from - side, b = from + side, c = to + side, d = to - side;
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::TRIANGLES);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(d.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(b.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(d.mV); gGL.vertex3fv(c.mV);
    gGL.end();
}

void SLPoseIK::drawQuad(const LLVector3& corner, const LLVector3& u, const LLVector3& v, const LLColor4& color) const
{
    const LLVector3 a = corner, b = corner + u, c = corner + u + v, d = corner + v;
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::TRIANGLES);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(d.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(b.mV);
    gGL.vertex3fv(a.mV); gGL.vertex3fv(d.mV); gGL.vertex3fv(c.mV);
    gGL.end();
}

void SLPoseIK::drawMoveGizmo(const LLVector3& center, bool xray) const
{
    static const LLColor4 AXIS_COLORS[3] = { LLColor4(1.f, 0.3f, 0.3f, 1.f), LLColor4(0.35f, 1.f, 0.35f, 1.f),
                                             LLColor4(0.35f, 0.55f, 1.f, 1.f) };
    const F32 mpp = metersPerPixel(center);
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLVector3 axes[3];
    moveAxes(center, axes);
    const bool moving = mDrag.mActive && (mDrag.mMoveAxis >= 0 || mDrag.mMovePlane >= 0);
    auto shade = [&](S32 part, const LLColor4& base, F32 alpha)
    {
        const bool active = mDrag.mActive && (mDrag.mMoveAxis == part || (part >= 3 && mDrag.mMovePlane == part - 3));
        const bool hover = !mDrag.mActive && mHoverMove == part;
        LLColor4 color = (active || hover) ? lerp(base, LLColor4::white, 0.35f) : base;
        F32 a = (active || hover) ? 0.95f : alpha;
        if (moving && !active)
            a = 0.15f;      // dragging another part
        color.mV[VALPHA] = xray ? a * 0.3f : a;
        return color;
    };

    for (S32 n = 0; n < 3; ++n)
    {
        if (llabs(axes[n] * to_cam) < MOVE_PLANE_MIN_DOT)
            continue;
        const LLVector3 u = axes[(n + 1) % 3] * ((MOVE_PLANE_HI_PX - MOVE_PLANE_LO_PX) * mpp);
        const LLVector3 v = axes[(n + 2) % 3] * ((MOVE_PLANE_HI_PX - MOVE_PLANE_LO_PX) * mpp);
        const LLVector3 corner = center + (axes[(n + 1) % 3] + axes[(n + 2) % 3]) * (MOVE_PLANE_LO_PX * mpp);
        drawQuad(corner, u, v, shade(3 + n, AXIS_COLORS[n], 0.35f));
    }
    for (S32 a = 0; a < 3; ++a)
    {
        if (llabs(axes[a] * to_cam) > MOVE_ARROW_MAX_DOT)
            continue;
        const LLColor4 color = shade(a, AXIS_COLORS[a], 0.75f);
        const LLVector3 start = center + axes[a] * (MOVE_ARROW_START_PX * mpp);
        const LLVector3 head = center + axes[a] * (MOVE_ARROW_LENGTH_PX * mpp);
        drawRibbon(start, head, MOVE_SHAFT_PX * mpp, color);
        drawCone(head, axes[a], MOVE_HEAD_LENGTH_PX * mpp, MOVE_HEAD_RADIUS_PX * mpp, color);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

bool SLPoseIK::handleHover(S32 x, S32 y, MASK mask)
{
    mHoverChain = -1;
    mHoverPole = false;
    mHoverRing = -1;
    mHoverMove = -1;
    if (mDrag.mActive || (mask & MASK_ALT))
        return false;
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar)
        return false;
    syncAvatar(avatar);
    updateTips(avatar);
    updateTargets(avatar);
    Handle handle;
    mHoverBone.clear();
    if (!pick(avatar, x, y, handle))
    {
        // Handles win; then the selected handle's gizmo: the translate arrows / squares,
        // or with Ctrl the rotate rings; then the FK bones (#1566).
        S32 part = -1;
        if (gizmoRotateMode() && pickRing(avatar, x, y, handle, part))
        {
            mHoverRing = part;
            gViewerWindow->setCursor(UI_CURSOR_TOOLROTATE);
            return true;
        }
        if (!gizmoRotateMode() && pickMove(avatar, x, y, handle, part))
        {
            mHoverMove = part;
            gViewerWindow->setCursor(UI_CURSOR_TOOLTRANSLATE);
            return true;
        }
        FKBone bone;
        F32 along = 0.f;
        if (fkShown() && pickBone(avatar, x, y, bone, along))
        {
            mHoverBone = bone.mJoint;
            gViewerWindow->setCursor(UI_CURSOR_HAND);
            return true;
        }
        return false;
    }
    mHoverChain = handle.mChain;
    mHoverPole = handle.mPole;
    gViewerWindow->setCursor(UI_CURSOR_HAND);
    return true;
}

bool SLPoseIK::handleMouseClick(S32 x, S32 y, MASK mask, S32 clicktype, bool down)
{
    if (!down || mDrag.mActive || (mask & MASK_ALT))
        return false;
    if (clicktype != CLICK_LEFT && clicktype != CLICK_RIGHT && clicktype != CLICK_DOUBLELEFT)
        return false;
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar)
        return false;
    syncAvatar(avatar);
    updateTips(avatar);
    updateTargets(avatar);
    Handle handle;
    S32 ring = -1;
    S32 move = -1;
    if (!pick(avatar, x, y, handle))
    {
        if (clicktype == CLICK_RIGHT)
            return false;
        if (gizmoRotateMode() ? !pickRing(avatar, x, y, handle, ring) : !pickMove(avatar, x, y, handle, move))
        {
            // The FK bones (#1566), under every IK handle and gizmo.
            return fkShown() && startBoneDrag(avatar, x, y, mask, -1);
        }
        if (handle.mChain < 0)
            return startBoneDrag(avatar, x, y, mask, ring);     // an FK bone's ring
    }

    const Chain& chain = mChains[handle.mChain];
    mSelectedBone.clear();      // an IK handle takes the gizmo
    if (clicktype == CLICK_RIGHT)
    {
        // Right-click a cone: pin / unpin it (the pelvis and pole handles have no pin).
        if (!handle.mPole && chain.mSolver != SOLVER_PELVIS && chain.mSolver != SOLVER_HIPS)
        {
            ChainState& state = mStates[handle.mChain];
            if (state.mPinned)
                state.mPinned = false;
            else
                pinChain(avatar, handle.mChain);
        }
        return true;
    }

    // Left: start a drag.
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mDrag = DragState();
    mDrag.mStatesBefore = mStates;
    mDrag.mChain = handle.mChain;
    mDrag.mPole = handle.mPole;
    mDrag.mRing = ring;
    mDrag.mMoveAxis = (move >= 0 && move < 3) ? move : -1;
    mDrag.mMovePlane = (move >= 3) ? move - 3 : -1;
    mDrag.mRotate = ring >= 0 || (move < 0 && !handle.mPole && (mask & MASK_CONTROL));
    mDrag.mRoll = ring < 0 && mDrag.mRotate && (mask & MASK_SHIFT);
    mDrag.mWasSelected = (mSelectedChain == handle.mChain);
    // Clicking a handle selects it: its rotate rings show.
    if (!handle.mPole && ringsEnabled())
        mSelectedChain = handle.mChain;
    mDrag.mStartX = x;
    mDrag.mStartY = y;
    mDrag.mStartGoal = handle.mPos;
    mDrag.mGoal = handle.mPos;
    mDrag.mStartRot = handle.mRot;
    mDrag.mGoalRot = handle.mRot;
    // Free drag: on the plane through the handle facing the camera (its depth along the
    // view axis is kept), holding the grab point's offset from the handle in the camera's
    // own frame -- so the depth stays put even if the camera follows the avatar mid-drag.
    mDrag.mPlaneNormal = camera->getAtAxis();
    mDrag.mDepth = (handle.mPos - camera->getOrigin()) * mDrag.mPlaneNormal;
    if (!rayPlane(x, y, handle.mPos, mDrag.mPlaneNormal, mDrag.mStartHit))
        mDrag.mStartHit = handle.mPos;
    mDrag.mGrabLeft = (handle.mPos - mDrag.mStartHit) * camera->getLeftAxis();
    mDrag.mGrabUp = (handle.mPos - mDrag.mStartHit) * camera->getUpAxis();
    if (move >= 0)
    {
        // LLManipTranslate: the axis / plane is frozen at mouse down; the handle keeps its
        // offset from the grab point along it.
        LLVector3 axes[3];
        moveAxes(handle.mPos, axes);
        if (mDrag.mMoveAxis >= 0)
        {
            mDrag.mMoveDir = axes[mDrag.mMoveAxis];
            mDrag.mMoveDown = mouseAlongLine(x, y, handle.mPos, mDrag.mMoveDir);
        }
        else
        {
            mDrag.mMoveDir = axes[mDrag.mMovePlane];
            if (!rayPlane(x, y, handle.mPos, mDrag.mMoveDir, mDrag.mMoveDownHit))
                mDrag.mMoveDownHit = handle.mPos;
        }
    }
    if (ring >= 0)
    {
        // LLManipRotate::handleMouseDownOnPart: freeze the axis, center and radius, and
        // decide edge-on once.
        LLVector3 axes[3];
        ringAxes(handle, axes);
        mDrag.mRingAxis = axes[ring];
        mDrag.mRingCenter = handle.mPos;
        mDrag.mRingRadius = ringRadius(handle.mPos);
        LLVector3 to_cam = camera->getOrigin() - mDrag.mRingCenter;
        to_cam.normVec();
        LLVector3 hit;
        mDrag.mRingEdgeOn = llabs(mDrag.mRingAxis * to_cam) < RING_EDGE_ON_DOT;
        if (!mDrag.mRingEdgeOn && rayPlane(x, y, mDrag.mRingCenter, mDrag.mRingAxis, hit))
        {
            LLVector3 v = hit - mDrag.mRingCenter;
            v -= mDrag.mRingAxis * (v * mDrag.mRingAxis);
            if (v.length() > 1.0e-6f)
            {
                v.normVec();
                mDrag.mRingDown = v;
            }
            else
            {
                mDrag.mRingEdgeOn = true;
            }
        }
        else
        {
            mDrag.mRingEdgeOn = true;
        }
        if (mDrag.mRingEdgeOn)
        {
            // Across the ring's line on the view plane through the center.
            mDrag.mRingCamNormal = to_cam;
            mDrag.mRingUp = to_cam % mDrag.mRingAxis;
            mDrag.mRingUp.normVec();
            if (rayPlane(x, y, mDrag.mRingCenter, to_cam, hit))
                mDrag.mRingDownDist = (hit - mDrag.mRingCenter) * mDrag.mRingUp;
        }
    }
    if (chain.mSolver == SOLVER_PELVIS || chain.mSolver == SOLVER_HIPS)
    {
        if (const SLPoseStudio::joint_t* pelvis = SLPoseStudio::instance().findJoint(chain.mJoints[0]))
            mDrag.mStartPelvisPos = SLPoseStudio::instance().getBonePosition(avatar, *pelvis);
    }
    // A pole diamond is only set once it moves (onDragHover): a plain click leaves the
    // limb, its bend and the undo history alone. Bones still easing in (an eased edit or
    // pose load) jump to where they are going: the drag starts from that pose.
    SLPoseStudio::instance().finishBlend(avatar);
    mDrag.mBefore = SLPoseStudio::instance().captureBoneStates(avatar);
    mDrag.mActive = true;
    captureDragStart(avatar);
    // #1566: every limb's bend plane as it is now, held for the whole drag (no knee flips).
    mDrag.mBendFrames.assign(mChains.size(), SLPoseIKMath::BendFrame());
    for (S32 i = 0; i < (S32)mChains.size(); ++i)
    {
        if (mChains[i].mSolver == SOLVER_TWOBONE && chainUsable(avatar, i) && chainJoints(i).size() == 3)
            mDrag.mBendFrames[i] = bendFrame(avatar, i);
    }
    // #1566: body drags carry (or hold) the chains below the bone they move.
    if (!handle.mPole && (chain.mSolver == SOLVER_PELVIS || chain.mSolver == SOLVER_HIPS || chain.mBox))
    {
        mDrag.mBodyJoint = (chain.mSolver == SOLVER_PELVIS || chain.mSolver == SOLVER_HIPS) ? chain.mJoints[0]
                                                                                           : chainJoints(handle.mChain).back();
        if (!fkJoint(avatar, mDrag.mBodyJoint, mDrag.mBodyStartPos, mDrag.mBodyStartRot))
            mDrag.mBodyJoint.clear();
    }
    // Nothing to solve until the mouse moves: a click leaves the pose exactly as it was.
    mDrag.mSolved = true;
    mDrag.mSolvedGoal = mDrag.mGoal;
    mDrag.mSolvedGoalRot = mDrag.mGoalRot;
    mTool->setMouseCapture(true);
    return true;
}

bool SLPoseIK::updateDragGoal(S32 x, S32 y, MASK mask)
{
    // Where the drag asks the handle / bone to go (mGoal) or turn to (mGoalRot), from the
    // mouse: a ring, an arrow, a plane square, Ctrl turning, or the free camera-plane move.
    // False when the mouse missed the plane it moves on (keep the last goal).
    LLViewerCamera* camera = LLViewerCamera::getInstance();

    if (mDrag.mRing >= 0)
    {
        // A rotate ring (LLManipRotate::dragConstrained): in the ring's plane, the angle
        // from the mouse-down direction to the mouse's, about the frozen axis -- absolute,
        // only unwrapped past 180 degrees, so no per-frame error builds up; a missed plane
        // hit keeps the last angle. Edge-on: the mouse's travel across the ring's line.
        LLVector3 hit;
        if (!mDrag.mRingEdgeOn)
        {
            if (rayPlane(x, y, mDrag.mRingCenter, mDrag.mRingAxis, hit))
            {
                LLVector3 v = hit - mDrag.mRingCenter;
                v -= mDrag.mRingAxis * (v * mDrag.mRingAxis);
                if (v.length() > 0.02f * mDrag.mRingRadius)
                {
                    v.normVec();
                    F32 a = atan2f((mDrag.mRingDown % v) * mDrag.mRingAxis, mDrag.mRingDown * v);
                    while (a - mDrag.mRingAngle > F_PI)
                        a -= F_TWO_PI;
                    while (a - mDrag.mRingAngle < -F_PI)
                        a += F_TWO_PI;
                    mDrag.mRingAngle = a;
                }
            }
        }
        else if (rayPlane(x, y, mDrag.mRingCenter, mDrag.mRingCamNormal, hit))
        {
            // The ring's near edge moves along -up for a positive turn.
            const F32 dist = (hit - mDrag.mRingCenter) * mDrag.mRingUp - mDrag.mRingDownDist;
            mDrag.mRingAngle = -dist / llmax(mDrag.mRingRadius, 1.0e-4f);
        }
        F32 angle = mDrag.mRingAngle;
        if (mask & MASK_SHIFT)
        {
            const F32 step = RING_SNAP_DEG * DEG_TO_RAD;
            angle = (F32)ll_round(angle / step) * step;
        }
        static LLCachedControl<bool> limits(gSavedSettings, "SLPoseStudioIKLimits", true);
        if (limits)
        {
            // Limited bones stop along the slerp from their start rotation, which takes the
            // short way past 180 degrees: keep the request on its own side.
            angle = llclamp(angle, -F_PI + 0.02f, F_PI - 0.02f);
        }
        mDrag.mGoalRot = mDrag.mStartRot * LLQuaternion(angle, mDrag.mRingAxis);
        mDrag.mGoalRot.normalize();
    }
    else if (mDrag.mMoveAxis >= 0)
    {
        // Along one world axis: where the mouse ray passes nearest the axis line (looking
        // straight down the axis keeps the last position).
        LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
        ray.normVec();
        if (llabs(ray * mDrag.mMoveDir) < 0.999f)
            mDrag.mGoal = mDrag.mStartGoal + mDrag.mMoveDir * (mouseAlongLine(x, y, mDrag.mStartGoal, mDrag.mMoveDir) - mDrag.mMoveDown);
    }
    else if (mDrag.mMovePlane >= 0)
    {
        // Within one world plane.
        LLVector3 hit;
        if (!rayPlane(x, y, mDrag.mStartGoal, mDrag.mMoveDir, hit))
            return false;
        LLVector3 delta = hit - mDrag.mMoveDownHit;
        delta -= mDrag.mMoveDir * (delta * mDrag.mMoveDir);
        mDrag.mGoal = mDrag.mStartGoal + delta;
    }
    else if (mDrag.mRotate)
    {
        // Ctrl: turn the end bone. Horizontal mouse turns about the camera's up axis,
        // vertical about its left axis; Ctrl+Shift rolls about the view axis.
        const F32 dx = (F32)(x - mDrag.mStartX) * DRAG_ROTATE_RAD_PER_PX;
        const F32 dy = (F32)(y - mDrag.mStartY) * DRAG_ROTATE_RAD_PER_PX;
        LLQuaternion turn;
        if (mDrag.mRoll)
        {
            turn = LLQuaternion(-dx, camera->getAtAxis());
        }
        else
        {
            turn = LLQuaternion(dx, camera->getUpAxis()) * LLQuaternion(-dy, camera->getLeftAxis());
        }
        mDrag.mGoalRot = mDrag.mStartRot * turn;
        mDrag.mGoalRot.normalize();
    }
    else
    {
        // The camera-facing plane at the handle's mouse-down depth, from where the camera
        // is now (the same plane as at mouse down unless the camera moved).
        const LLVector3 at = camera->getAtAxis();
        LLVector3 hit;
        if (!rayPlane(x, y, camera->getOrigin() + at * mDrag.mDepth, at, hit))
            return false;
        const LLVector3 free_goal = hit + camera->getLeftAxis() * mDrag.mGrabLeft + camera->getUpAxis() * mDrag.mGrabUp;
        LLVector3 delta = free_goal - mDrag.mStartGoal;
        if (mask & MASK_SHIFT)
        {
            // Shift: lock to the world axis the drag starts along.
            const F32 moved_px = (F32)(llabs(x - mDrag.mStartX) + llabs(y - mDrag.mStartY));
            if (mDrag.mAxisLock < 0 && moved_px >= AXIS_LOCK_PX)
            {
                S32 axis = 0;
                for (S32 i = 1; i < 3; ++i)
                {
                    if (llabs(delta.mV[i]) > llabs(delta.mV[axis]))
                        axis = i;
                }
                mDrag.mAxisLock = axis;
            }
            if (mDrag.mAxisLock >= 0)
            {
                // Along the locked axis: the point on it nearest the mouse ray.
                LLVector3 axis_dir;
                axis_dir.mV[mDrag.mAxisLock] = 1.f;
                const LLVector3 origin = camera->getOrigin();
                LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
                ray.normVec();
                const LLVector3 w0 = mDrag.mStartGoal - origin;
                const F32 b = axis_dir * ray;
                const F32 denom = 1.f - b * b;
                F32 s = 0.f;
                if (denom > 1.0e-4f)
                    s = (b * (ray * w0) - (axis_dir * w0)) / denom;
                else
                    s = delta.mV[mDrag.mAxisLock];
                delta.clear();
                delta.mV[mDrag.mAxisLock] = s;
            }
            else
            {
                delta.clear();
            }
        }
        else
        {
            mDrag.mAxisLock = -1;
        }
        mDrag.mGoal = mDrag.mStartGoal + delta;
    }
    return true;
}

void SLPoseIK::onDragHover(S32 x, S32 y, MASK mask)
{
    if (!mDrag.mActive)
        return;
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || avatar->getID() != mAvatarID)
        return;
    // Shift starts an FK bone move (#1566): it is not the free drag's axis lock there.
    const MASK goal_mask = (!mDrag.mBone.empty() && mDrag.mBoneMode == BONE_MOVE) ? (mask & ~MASK_SHIFT) : mask;
    if (!updateDragGoal(x, y, goal_mask))
        return;
    if (!mDrag.mBone.empty())
    {
        onBoneDragHover(avatar, x, y, mask);
        return;
    }
    const Chain& chain = mChains[mDrag.mChain];

    // The capturing tool gets a hover every frame (LLViewerWindow::updateUI), mouse moving
    // or not. Each solve is a pure function of the drag-start pose and the goal, so an
    // unchanged goal has nothing new to solve.
    if (mDrag.mSolved && mDrag.mGoal == mDrag.mSolvedGoal && mDrag.mGoalRot == mDrag.mSolvedGoalRot)
        return;
    mDrag.mSolved = true;
    mDrag.mSolvedGoal = mDrag.mGoal;
    mDrag.mSolvedGoalRot = mDrag.mGoalRot;

    SLPoseStudio& studio = SLPoseStudio::instance();
    mApplying = true;
    // Solve from the pose at mouse down, never from the last frame's solve: solving on top
    // of its own output every frame let minimal-arc twist, limit clamps and the decode
    // reference drift accumulate, and the limb wandered / flailed even with the mouse still.
    restoreDragStart(avatar);
    const bool target = usesTarget(mDrag.mChain);
    if (mDrag.mPole)
    {
        // Elbow / knee pole: the bend plane turns to it; the hand / foot stays on its target.
        setPoleWorld(avatar, mDrag.mChain, mDrag.mGoal);
        mStates[mDrag.mChain].mPlaced = true;
        LLVector3 target_pos;
        LLQuaternion target_rot;
        if (targetWorld(avatar, mDrag.mChain, target_pos, target_rot))
            solveChain(avatar, mDrag.mChain, target_pos, &target_rot);
    }
    else if (chain.mSolver == SOLVER_PELVIS || chain.mSolver == SOLVER_HIPS)
    {
        const SLPoseStudio::joint_t* pelvis = studio.findJoint(chain.mJoints[0]);
        LLVector3 parent_pos;
        LLQuaternion parent_rot;
        if (pelvis && fkParent(avatar, chain.mJoints[0], parent_pos, parent_rot))
        {
            if (mDrag.mRotate)
            {
                // Turned as a whole (no follower counter-rotation, no limits): the body turns.
                applyWorldRotation(avatar, chain.mJoints[0], mDrag.mGoalRot, parent_rot, false);
            }
            else
            {
                // World move -> the pelvis' public position offset, in its parent's frame.
                const LLVector3 local_delta = (mDrag.mGoal - mDrag.mStartGoal) * ~parent_rot;
                studio.setBonePosition(avatar, *pelvis, mDrag.mStartPelvisPos + local_delta, false);
            }
        }
        carryTargets(avatar);
    }
    else if (target)
    {
        // An IK target: moving changes only its position, turning (ring / Ctrl) only its
        // orientation. The chain reaches for its position (straight toward it when out of
        // reach; the handle stays where it was put) and the end bone takes its orientation.
        if (mDrag.mRotate)
            setTargetWorld(avatar, mDrag.mChain, nullptr, &mDrag.mGoalRot);
        else
            setTargetWorld(avatar, mDrag.mChain, &mDrag.mGoal, nullptr);
        mStates[mDrag.mChain].mPlaced = true;
        LLVector3 target_pos;
        LLQuaternion target_rot;
        if (targetWorld(avatar, mDrag.mChain, target_pos, target_rot))
            solveChain(avatar, mDrag.mChain, target_pos, &target_rot);
        if (chain.mBox)
            carryTargets(avatar);   // the chest box carries the arms / head (#1566)
    }
    else if (mDrag.mRotate)
    {
        // The head turns.
        solveChain(avatar, mDrag.mChain, mDrag.mStartGoal, &mDrag.mGoalRot);
    }
    else
    {
        // The head aims (lookat), the nose is pulled (reach), the eyes look (gaze).
        if (chain.mSolver == SOLVER_GAZE)
        {
            LLVector3 root_pos;
            LLQuaternion root_rot;
            if (rootFrame(avatar, root_pos, root_rot))
            {
                mStates[mDrag.mChain].mLook = (mDrag.mGoal - root_pos) * ~root_rot;
                mStates[mDrag.mChain].mHasLook = true;
            }
        }
        solveChain(avatar, mDrag.mChain, mDrag.mGoal, nullptr);
    }
    if (target)
        markSeen(avatar, mDrag.mChain);
    holdPins(avatar, mDrag.mChain);
    mApplying = false;
}

void SLPoseIK::onDragEnd(bool cancelled)
{
    if (!mDrag.mActive)
        return;
    DragState drag = mDrag;
    mDrag = DragState();

    // A plain click (nothing solved) on the already selected handle hides its rings.
    if (!cancelled && drag.mBone.empty() && drag.mRing < 0 && drag.mMoveAxis < 0 && drag.mMovePlane < 0 && !drag.mPole && drag.mWasSelected
        && drag.mSolvedGoal == drag.mStartGoal
        && drag.mSolvedGoalRot == drag.mStartRot)
        mSelectedChain = -1;

    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar || avatar->getID() != mAvatarID)
        return;
    SLPoseStudio& studio = SLPoseStudio::instance();
    if (cancelled)
    {
        mApplying = true;
        studio.restoreBoneStates(avatar, drag.mBefore);
        mApplying = false;
        restoreTargets(avatar, drag.mStatesBefore);
        return;
    }

    // The drag is over whether or not it changed a bone (an out-of-reach target can move
    // with none changing): later edits are not its release.
    studio.endPreview(avatar->getID());

    // A pinned target that was dragged holds at its new place.
    if (drag.mChain >= 0 && drag.mChain < (S32)mStates.size() && mStates[drag.mChain].mPinned)
        mStates[drag.mChain].mPinBones = chainTargets(avatar, drag.mChain);

    // One undo step for the whole drag: only the bones it changed.
    const std::vector<SLPoseStudio::BoneState> after = studio.captureBoneStates(avatar);
    UndoEntry entry;
    for (const SLPoseStudio::BoneState& now : after)
    {
        for (const SLPoseStudio::BoneState& was : drag.mBefore)
        {
            if (was.mJoint != now.mJoint)
                continue;
            if (was.mEdited != now.mEdited || !nearlySameRotation(was.mRotation, now.mRotation, HOLD_EPSILON_DOT)
                || (was.mPosition - now.mPosition).length() > 1.0e-5f)
            {
                entry.mBefore.push_back(was);
                entry.mAfter.push_back(now);
            }
            break;
        }
    }
    // Target / pole moves are part of the step too (an out-of-reach target can move
    // without any bone changing).
    bool targets_moved = false;
    for (size_t i = 0; i < llmin(drag.mStatesBefore.size(), mStates.size()) && !targets_moved; ++i)
    {
        const ChainState& was = drag.mStatesBefore[i];
        const ChainState& now = mStates[i];
        targets_moved = was.mHasTarget != now.mHasTarget || was.mHasPole != now.mHasPole || was.mPlaced != now.mPlaced
                        || (was.mTargetPos - now.mTargetPos).length() > 1.0e-5f
                        || !nearlySameRotation(was.mTargetRot, now.mTargetRot, HOLD_EPSILON_DOT)
                        || (now.mHasPole && (was.mPole - now.mPole).length() > 1.0e-5f)
                        || was.mHasLook != now.mHasLook || (was.mLook - now.mLook).length() > 1.0e-5f;
    }
    if (!entry.mBefore.empty() || targets_moved)
    {
        entry.mTargetsBefore = drag.mStatesBefore;
        entry.mTargetsAfter = mStates;
        mUndo.push_back(entry);
        if (mUndo.size() > 50)
            mUndo.erase(mUndo.begin());
        mRedo.clear();
    }
    if (!entry.mBefore.empty())
    {
        // The drag previewed locally; now it is a real edit (saved, shared, synced).
        mApplying = true;
        studio.notifyEdited(avatar, true);
        mApplying = false;
    }
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

bool SLPoseIK::canUndo(LLVOAvatar* avatar) const
{
    return avatar && avatar->getID() == mAvatarID && !mUndo.empty() && !mDrag.mActive;
}

bool SLPoseIK::canRedo(LLVOAvatar* avatar) const
{
    return avatar && avatar->getID() == mAvatarID && !mRedo.empty() && !mDrag.mActive;
}

void SLPoseIK::undo(LLVOAvatar* avatar)
{
    if (!canUndo(avatar))
        return;
    UndoEntry entry = mUndo.back();
    mUndo.pop_back();
    mApplying = true;
    SLPoseStudio::instance().restoreBoneStates(avatar, entry.mBefore);
    mApplying = false;
    restoreTargets(avatar, entry.mTargetsBefore);
    mRedo.push_back(entry);
}

void SLPoseIK::redo(LLVOAvatar* avatar)
{
    if (!canRedo(avatar))
        return;
    UndoEntry entry = mRedo.back();
    mRedo.pop_back();
    mApplying = true;
    SLPoseStudio::instance().restoreBoneStates(avatar, entry.mAfter);
    mApplying = false;
    restoreTargets(avatar, entry.mTargetsAfter);
    mUndo.push_back(entry);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void SLPoseIK::drawCone(const LLVector3& base, const LLVector3& dir, F32 length, F32 radius, const LLColor4& color) const
{
    constexpr S32 SEGMENTS = 16;
    const LLVector3 e1 = perpendicular(dir);
    LLVector3 e2 = dir % e1;
    e2.normVec();
    const LLVector3 apex = base + dir * length;
    const LLVector3 view = LLViewerCamera::getInstance()->getAtAxis();

    LLVector3 ring[SEGMENTS + 1];
    for (S32 i = 0; i <= SEGMENTS; ++i)
    {
        const F32 a = F_TWO_PI * (F32)i / (F32)SEGMENTS;
        ring[i] = base + (e1 * cosf(a) + e2 * sinf(a)) * radius;
    }

    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < SEGMENTS; ++i)
    {
        // Facet shading toward the camera, so the cone reads as a solid.
        LLVector3 n = (ring[i + 1] - ring[i]) % (apex - ring[i]);
        n.normVec();
        const F32 shade = 0.55f + 0.45f * llabs(n * view);
        gGL.color4f(color.mV[VRED] * shade, color.mV[VGREEN] * shade, color.mV[VBLUE] * shade, color.mV[VALPHA]);
        gGL.vertex3fv(apex.mV);
        gGL.vertex3fv(ring[i].mV);
        gGL.vertex3fv(ring[i + 1].mV);

        gGL.color4f(color.mV[VRED] * 0.5f, color.mV[VGREEN] * 0.5f, color.mV[VBLUE] * 0.5f, color.mV[VALPHA]);
        gGL.vertex3fv(base.mV);
        gGL.vertex3fv(ring[i + 1].mV);
        gGL.vertex3fv(ring[i].mV);
    }
    gGL.end();
}

void SLPoseIK::drawDiamond(const LLVector3& center, F32 size, const LLColor4& color) const
{
    const LLVector3 tips[6] = { center + LLVector3(size, 0, 0), center - LLVector3(size, 0, 0),
                                center + LLVector3(0, size, 0), center - LLVector3(0, size, 0),
                                center + LLVector3(0, 0, size), center - LLVector3(0, 0, size) };
    // 8 faces: (+-x) x (+-y) x (+-z)
    static const S32 faces[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 },
                                     { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
    gGL.begin(LLRender::TRIANGLES);
    for (S32 f = 0; f < 8; ++f)
    {
        const F32 shade = (f < 4) ? 1.f : 0.7f;
        gGL.color4f(color.mV[VRED] * shade, color.mV[VGREEN] * shade, color.mV[VBLUE] * shade, color.mV[VALPHA]);
        for (S32 v = 0; v < 3; ++v)
            gGL.vertex3fv(tips[faces[f][v]].mV);
    }
    gGL.end();
}

void SLPoseIK::drawBox(const LLVector3& center, const LLQuaternion& rot, F32 half, const LLColor4& color) const
{
    // A solid box in the bone's own axes, faces shaded toward the camera so it reads as one.
    LLVector3 axes[3] = { LLVector3::x_axis * rot, LLVector3::y_axis * rot, LLVector3::z_axis * rot };
    const LLVector3 view = LLViewerCamera::getInstance()->getAtAxis();
    gGL.begin(LLRender::TRIANGLES);
    for (S32 a = 0; a < 3; ++a)
    {
        const LLVector3& n = axes[a];
        const LLVector3 u = axes[(a + 1) % 3] * half;
        const LLVector3 v = axes[(a + 2) % 3] * half;
        const F32 shade = 0.55f + 0.45f * llabs(n * view);
        gGL.color4f(color.mV[VRED] * shade, color.mV[VGREEN] * shade, color.mV[VBLUE] * shade, color.mV[VALPHA]);
        for (S32 sign = -1; sign <= 1; sign += 2)
        {
            const LLVector3 c = center + n * (half * (F32)sign);
            const LLVector3 p0 = c - u - v, p1 = c + u - v, p2 = c + u + v, p3 = c - u + v;
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p1.mV); gGL.vertex3fv(p2.mV);
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p2.mV); gGL.vertex3fv(p3.mV);
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p2.mV); gGL.vertex3fv(p1.mV);
            gGL.vertex3fv(p0.mV); gGL.vertex3fv(p3.mV); gGL.vertex3fv(p2.mV);
        }
    }
    gGL.end();
}

void SLPoseIK::drawRing(const LLVector3& center, const LLVector3& normal, F32 radius, const LLColor4& color) const
{
    constexpr S32 SEGMENTS = 24;
    const LLVector3 e1 = perpendicular(normal);
    LLVector3 e2 = normal % e1;
    e2.normVec();
    gGL.color4fv(color.mV);
    gGL.begin(LLRender::LINES);
    for (S32 i = 0; i < SEGMENTS; ++i)
    {
        const F32 a0 = F_TWO_PI * (F32)i / (F32)SEGMENTS;
        const F32 a1 = F_TWO_PI * (F32)(i + 1) / (F32)SEGMENTS;
        const LLVector3 p0 = center + (e1 * cosf(a0) + e2 * sinf(a0)) * radius;
        const LLVector3 p1 = center + (e1 * cosf(a1) + e2 * sinf(a1)) * radius;
        gGL.vertex3fv(p0.mV);
        gGL.vertex3fv(p1.mV);
    }
    gGL.end();
}

void SLPoseIK::drawBand(const LLVector3& center, const LLVector3& normal, F32 radius, F32 width, const LLColor4& color,
                        F32 back_alpha) const
{
    // A flat annulus in the ring's plane plus a band across it, both double-sided: the
    // ring reads at any angle, edge-on included.
    const LLVector3 e1 = perpendicular(normal);
    LLVector3 e2 = normal % e1;
    e2.normVec();
    const LLVector3 across = normal * width;
    // The half away from the camera is dimmer (the build tool hides it); it cannot be picked.
    LLVector3 to_cam = LLViewerCamera::getInstance()->getOrigin() - center;
    to_cam.normVec();
    LLColor4 back = color;
    back.mV[VALPHA] *= back_alpha;
    gGL.begin(LLRender::TRIANGLES);
    auto quad = [](const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& d)
    {
        gGL.vertex3fv(a.mV); gGL.vertex3fv(b.mV); gGL.vertex3fv(c.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(d.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(c.mV); gGL.vertex3fv(b.mV);
        gGL.vertex3fv(a.mV); gGL.vertex3fv(d.mV); gGL.vertex3fv(c.mV);
    };
    for (S32 i = 0; i < RING_SEGMENTS; ++i)
    {
        const F32 a0 = F_TWO_PI * (F32)i / (F32)RING_SEGMENTS;
        const F32 a1 = F_TWO_PI * (F32)(i + 1) / (F32)RING_SEGMENTS;
        const LLVector3 d0 = e1 * cosf(a0) + e2 * sinf(a0);
        const LLVector3 d1 = e1 * cosf(a1) + e2 * sinf(a1);
        gGL.color4fv(((d0 + d1) * to_cam >= 0.f) ? color.mV : back.mV);
        quad(center + d0 * (radius - width), center + d0 * (radius + width),
             center + d1 * (radius + width), center + d1 * (radius - width));
        const LLVector3 p0 = center + d0 * radius;
        const LLVector3 p1 = center + d1 * radius;
        quad(p0 - across, p0 + across, p1 + across, p1 - across);
    }
    gGL.end();
}

void SLPoseIK::render()
{
    LLVOAvatar* avatar = getActiveAvatar();
    if (!avatar)
        return;
    syncAvatar(avatar);
    updateTips(avatar);
    // Nothing below changes the pose: each joint's FK once for the whole draw.
    FkMemoScope fk_memo(*this);
    updateTargets(avatar);

    std::vector<Handle> handles;
    buildHandles(avatar, handles);
    std::vector<FKBone> bones;
    if (fkShown())
        buildBones(avatar, bones);
    if (handles.empty() && bones.empty())
        return;

    const F32 scale = handleScale(avatar);
    const F32 length = BASE_HANDLE_LENGTH * scale;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Two passes: faint everywhere (x-ray through the avatar and the scene), then solid
    // where nothing is in front.
    for (S32 pass = 0; pass < 2; ++pass)
    {
        const bool xray = (pass == 0);
        LLRender::DepthScope depth(!xray, false);
        // FK bones first (#1566): the IK handles draw over them.
        for (const FKBone& bone : bones)
        {
            const bool selected = bone.mJoint == mSelectedBone;
            const bool dragging = mDrag.mActive && mDrag.mBone == bone.mJoint;
            const bool hover = !mDrag.mActive && mHoverBone == bone.mJoint;
            LLColor4 color = selected ? LLColor4(1.f, 0.62f, 0.2f, 1.f) : LLColor4(0.75f, 0.78f, 0.85f, 1.f);
            F32 alpha = selected ? 0.6f : 0.35f;
            if (dragging || hover)
            {
                color = lerp(color, LLColor4::white, 0.4f);
                alpha = 0.75f;
            }
            color.mV[VALPHA] = xray ? alpha * 0.35f : alpha;
            drawBone(bone, color);
        }
        for (const Handle& handle : handles)
        {
            const Chain& chain = mChains[handle.mChain];
            const ChainState& state = mStates[handle.mChain];
            const bool dragging = mDrag.mActive && mDrag.mChain == handle.mChain && mDrag.mPole == handle.mPole;
            const bool hover = !mDrag.mActive && mHoverChain == handle.mChain && mHoverPole == handle.mPole;

            LLColor4 color = chain.mColor;
            F32 alpha = 0.45f;
            if (dragging)
            {
                color = lerp(color, LLColor4::white, 0.6f);
                alpha = 0.8f;
            }
            else if (hover)
            {
                color = lerp(color, LLColor4::white, 0.35f);
                alpha = 0.7f;
            }
            color.mV[VALPHA] = xray ? alpha * 0.35f : alpha;

            if (handle.mPole)
            {
                drawDiamond(handle.mPos, length * 0.22f, color);
                continue;
            }
            if (chain.mSolver == SOLVER_PELVIS)
            {
                drawDiamond(handle.mPos, length * 0.45f, color);
                continue;
            }
            if (chain.mSolver == SOLVER_HIPS || chain.mBox)
            {
                // Hips / chest box (#1566), turned with its bone.
                drawBox(handle.mPos, handle.mRot, length * (chain.mSolver == SOLVER_HIPS ? 0.9f : 0.75f), color);
                continue;
            }
            if (chain.mSolver == SOLVER_GAZE)
            {
                // The look-at point, and the line of sight from the eyes.
                drawDiamond(handle.mPos, length * 0.3f, color);
                if (state.mPinned)
                {
                    LLColor4 pin(1.f, 0.25f, 0.25f, xray ? 0.35f : 0.9f);
                    drawRing(handle.mPos, handle.mDir, length * 0.5f, pin);
                }
                LLVector3 eyes;
                if (eyeCenter(avatar, handle.mChain, eyes))
                {
                    LLColor4 line = color;
                    line.mV[VALPHA] *= 0.6f;
                    gGL.color4fv(line.mV);
                    gGL.begin(LLRender::LINES);
                    gGL.vertex3fv(eyes.mV);
                    gGL.vertex3fv(handle.mPos.mV);
                    gGL.end();
                }
                continue;
            }
            drawCone(handle.mPos, handle.mDir, length, length * 0.38f, color);
            if (state.mPinned)
            {
                LLColor4 pin(1.f, 0.25f, 0.25f, xray ? 0.35f : 0.9f);
                drawRing(handle.mPos, handle.mDir, length * 0.6f, pin);
            }
            if (chain.mSolver == SOLVER_LOOKAT)
            {
                // The line of sight, head to handle.
                LLVector3 head_pos;
                LLQuaternion head_rot;
                if (fkJoint(avatar, chain.mJoints.back(), head_pos, head_rot))
                {
                    LLColor4 line = color;
                    line.mV[VALPHA] *= 0.6f;
                    gGL.color4fv(line.mV);
                    gGL.begin(LLRender::LINES);
                    gGL.vertex3fv(head_pos.mV);
                    gGL.vertex3fv(handle.mPos.mV);
                    gGL.end();
                }
            }
            else if (usesTarget(handle.mChain))
            {
                // Out of reach: a line from where the chain ends to where its target is.
                LLVector3 tip_pos, end_pos;
                LLQuaternion end_rot;
                if (fkEffector(avatar, handle.mChain, tip_pos, end_pos, end_rot) && (tip_pos - handle.mPos).length() > 0.01f)
                {
                    gGL.color4f(1.f, 1.f, 1.f, xray ? 0.25f : 0.7f);
                    gGL.begin(LLRender::LINES);
                    gGL.vertex3fv(tip_pos.mV);
                    gGL.vertex3fv(handle.mPos.mV);
                    gGL.end();
                }
            }
        }

        // The selected handle's gizmo (X red, Y green, Z blue, as in the build tool): the
        // translate arrows / squares, or with Ctrl the rotate rings; hidden while another
        // handle is being dragged, and a free (body) drag shows the arrows it is not using.
        const bool bone_gizmo = ringsEnabled() && mSelectedChain < 0 && !mSelectedBone.empty() && fkShown()
                                && (!mDrag.mActive || mDrag.mBone == mSelectedBone);
        const bool show_gizmo = bone_gizmo
                                || (ringsEnabled() && mSelectedChain >= 0 && (!mDrag.mActive || (mDrag.mChain == mSelectedChain && !mDrag.mPole)));
        if (show_gizmo && !bone_gizmo && !gizmoRotateMode())
        {
            for (const Handle& handle : handles)
            {
                if (handle.mChain == mSelectedChain && !handle.mPole)
                {
                    drawMoveGizmo(handle.mPos, xray);
                    break;
                }
            }
        }
        Handle ring_handle;
        if (show_gizmo && gizmoRotateMode() && selectedHandle(avatar, ring_handle))
        {
            // One pass: the selected IK handle's or FK bone's rings.
            for (const Handle& handle : std::vector<Handle>(1, ring_handle))
            {
                // Screen-size, on the handle (frozen while one is turned).
                const LLVector3 center = (mDrag.mActive && mDrag.mRing >= 0) ? mDrag.mRingCenter : handle.mPos;
                const F32 mpp = metersPerPixel(center);
                const F32 radius = (mDrag.mActive && mDrag.mRing >= 0) ? mDrag.mRingRadius : RING_RADIUS_PX * mpp;
                static const LLColor4 RING_COLORS[3] = { LLColor4(1.f, 0.3f, 0.3f, 1.f), LLColor4(0.35f, 1.f, 0.35f, 1.f),
                                                         LLColor4(0.35f, 0.55f, 1.f, 1.f) };
                LLVector3 axes[3];
                ringAxes(handle, axes);
                for (S32 a = 0; a < 3; ++a)
                {
                    const bool active = mDrag.mActive && mDrag.mRing == a;
                    const bool hover = !mDrag.mActive && mHoverRing == a;
                    F32 alpha = 0.5f;
                    if (active || hover)
                        alpha = 0.9f;
                    else if (mDrag.mActive && mDrag.mRing >= 0)
                        alpha = 0.15f;  // turning another ring
                    LLColor4 color = (active || hover) ? lerp(RING_COLORS[a], LLColor4::white, 0.35f) : RING_COLORS[a];
                    color.mV[VALPHA] = xray ? alpha * 0.3f : alpha;
                    const F32 width = RING_WIDTH_PX * mpp * ((active || hover) ? 1.6f : 1.f);
                    // The axis being turned stays as frozen at mouse down.
                    const LLVector3 axis = active ? mDrag.mRingAxis : axes[a];
                    drawBand(center, axis, radius, width, color, 0.35f);
                }
                break;
            }
        }
    }
    gGL.flush();
}

// ---------------------------------------------------------------------------
// FK bones view (#1566 item 7)
// ---------------------------------------------------------------------------

// static
bool SLPoseIK::ikShown()
{
    static LLCachedControl<bool> ik(gSavedSettings, "SLPoseStudioIKHandles", true);
    return ik;
}

// static
bool SLPoseIK::fkShown()
{
    static LLCachedControl<bool> fk(gSavedSettings, "SLPoseStudioFKBones", false);
    return fk;
}

void SLPoseIK::setSelectedBone(const std::string& joint_name)
{
    mSelectedBone = joint_name;
}

void SLPoseIK::buildBones(LLVOAvatar* avatar, std::vector<FKBone>& out) const
{
    // Body and hand bones always; Bento extras (spine, tail, wings, hind limbs) only where a
    // worn rigged mesh uses them; never the face (dozens of tiny bones on top of each other).
    out.clear();
    if (!avatar)
        return;
    FkMemoScope fk_memo(*this);
    SLPoseStudio& studio = SLPoseStudio::instance();
    static LLCachedControl<bool> all_bento(gSavedSettings, "SLPoseStudioFKAllBento", false);
    const std::vector<bool>& used = studio.riggedJoints(avatar);
    auto shown = [&](LLJoint* joint) -> bool
    {
        const SLPoseStudio::joint_t* pj = joint ? poserJoint(joint->getName()) : nullptr;
        if (!pj)
            return false;
        switch (pj->boneType())
        {
            case WHOLEAVATAR:
            case BODY:
            case HANDS:
                return true;
            case MISC:
            {
                // "Show all Bento bones": wings, tail, hind limbs, extra spine whatever is worn.
                if (all_bento)
                    return true;
                const S32 num = joint->getJointNum();
                return num >= 0 && num < (S32)used.size() && used[num];
            }
            default:
                return false;
        }
    };
    const F32 scale = handleScale(avatar);
    for (const SLPoseStudio::joint_t& pj : studio.getAnimator().PoserJoints)
    {
        LLJoint* joint = avatar->getJoint(pj.jointName());
        if (!joint || !shown(joint))
            continue;
        FKBone bone;
        bone.mJoint = pj.jointName();
        if (!fkJoint(avatar, bone.mJoint, bone.mHead, bone.mRot))
            continue;
        // The tail: the nearest shown joints below (through hidden ones), averaged.
        LLVector3 sum;
        S32 count = 0;
        std::vector<LLJoint*> stack(joint->mChildren.begin(), joint->mChildren.end());
        while (!stack.empty())
        {
            LLJoint* child = stack.back();
            stack.pop_back();
            if (!child || dynamic_cast<LLAvatarJointCollisionVolume*>(child))
                continue;
            if (shown(child))
            {
                LLVector3 pos;
                LLQuaternion rot;
                if (fkJoint(avatar, child->getName(), pos, rot))
                {
                    sum += pos;
                    ++count;
                }
                continue;
            }
            if (poserJoint(child->getName()))
                stack.insert(stack.end(), child->mChildren.begin(), child->mChildren.end());
        }
        if (count > 0)
        {
            bone.mTail = sum * (1.f / (F32)count);
        }
        else
        {
            // A leaf (fingertip, toe, head): on along its parent bone, a third as long.
            LLVector3 parent_pos;
            LLQuaternion parent_rot;
            LLVector3 dir = (joint->getParent() && fkJoint(avatar, joint->getParent()->getName(), parent_pos, parent_rot))
                                ? bone.mHead - parent_pos : LLVector3::zero;
            bone.mTail = bone.mHead + dir * 0.35f;
        }
        if ((bone.mTail - bone.mHead).length() < 0.005f * scale)
            bone.mTail = bone.mHead + LLVector3::x_axis * bone.mRot * (0.03f * scale);
        out.push_back(bone);
    }
}

bool SLPoseIK::findBone(LLVOAvatar* avatar, const std::string& name, FKBone& out) const
{
    std::vector<FKBone> bones;
    buildBones(avatar, bones);
    for (const FKBone& bone : bones)
    {
        if (bone.mJoint == name)
        {
            out = bone;
            return true;
        }
    }
    return false;
}

bool SLPoseIK::pickBone(LLVOAvatar* avatar, S32 x, S32 y, FKBone& out, F32& along) const
{
    std::vector<FKBone> bones;
    buildBones(avatar, bones);
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 origin = camera->getOrigin();
    LLVector3 ray = gViewerWindow->mouseDirectionGlobal(x, y);
    ray.normVec();
    const F32 view_height = (F32)llmax(1, gViewerWindow->getWorldViewHeightScaled());
    const F32 px_to_m = 2.f * tanf(camera->getView() * 0.5f) / view_height;
    F32 best = F32_MAX;
    bool found = false;
    for (const FKBone& bone : bones)
    {
        // Closest points of the mouse ray and the bone's segment.
        const LLVector3 seg = bone.mTail - bone.mHead;
        const F32 c = seg * seg;
        if (c < 1.0e-8f)
            continue;
        const LLVector3 w0 = origin - bone.mHead;
        const F32 b = ray * seg;
        const F32 d = ray * w0;
        const F32 e = seg * w0;
        const F32 denom = c - b * b;
        F32 t = (denom > 1.0e-8f) ? (e - b * d) / denom : 0.f;
        t = llclamp(t, 0.f, 1.f);
        const LLVector3 on_bone = bone.mHead + seg * t;
        const F32 s = (on_bone - origin) * ray;
        if (s <= 0.f)
            continue;
        const F32 miss = (origin + ray * s - on_bone).length();
        const F32 radius = llmax(0.12f * sqrtf(c), MIN_PICK_PX * 0.8f * px_to_m * s);
        if (miss <= radius && s < best)
        {
            best = s;
            out = bone;
            along = t;
            found = true;
        }
    }
    return found;
}

bool SLPoseIK::startBoneDrag(LLVOAvatar* avatar, S32 x, S32 y, MASK mask, S32 ring)
{
    FKBone bone;
    F32 along = 0.f;
    if (ring >= 0 ? !findBone(avatar, mSelectedBone, bone) : !pickBone(avatar, x, y, bone, along))
        return false;
    SLPoseStudio& studio = SLPoseStudio::instance();
    const SLPoseStudio::joint_t* pj = studio.findJoint(bone.mJoint);
    LLJoint* lljoint = avatar->getJoint(bone.mJoint);
    if (!pj || !lljoint)
        return false;

    // Clicking a bone picks it, here and in the floater's figure (the dials follow).
    mSelectedBone = bone.mJoint;
    mSelectedChain = -1;
    if (SLFloaterPoseStudio* floater = LLFloaterReg::findTypedInstance<SLFloaterPoseStudio>(FLOATER_NAME))
        floater->selectBone(bone.mJoint);

    auto posable = [&](LLJoint* joint)
    {
        const SLPoseStudio::joint_t* j = joint ? studio.findJoint(joint->getName()) : nullptr;
        return j && j->boneType() != COL_VOLUMES;
    };
    LLJoint* parent = lljoint->getParent();
    if (!posable(parent))
        parent = nullptr;
    LLJoint* grandparent = parent ? parent->getParent() : nullptr;
    if (!posable(grandparent))
        grandparent = nullptr;

    S32 mode = BONE_SWING;
    if (ring >= 0 || (mask & MASK_CONTROL))
        mode = BONE_ROTATE;
    else if (mask & MASK_SHIFT)
        mode = BONE_MOVE;
    else if (along <= 0.5f)
        mode = parent ? BONE_SWING_PARENT : BONE_MOVE;  // the hips have nothing above: they move

    const LLVector3 grab = bone.mHead + (bone.mTail - bone.mHead) * along;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    mDrag = DragState();
    mDrag.mStatesBefore = mStates;
    mDrag.mChain = -1;
    mDrag.mBone = bone.mJoint;
    mDrag.mBoneMode = mode;
    mDrag.mRing = ring;
    mDrag.mRotate = (mode == BONE_ROTATE);
    mDrag.mStartX = x;
    mDrag.mStartY = y;
    mDrag.mStartGoal = mDrag.mGoal = grab;
    mDrag.mStartRot = mDrag.mGoalRot = bone.mRot;
    mDrag.mPlaneNormal = camera->getAtAxis();
    mDrag.mDepth = (grab - camera->getOrigin()) * mDrag.mPlaneNormal;
    if (!rayPlane(x, y, grab, mDrag.mPlaneNormal, mDrag.mStartHit))
        mDrag.mStartHit = grab;
    mDrag.mGrabLeft = (grab - mDrag.mStartHit) * camera->getLeftAxis();
    mDrag.mGrabUp = (grab - mDrag.mStartHit) * camera->getUpAxis();

    if (mode == BONE_SWING)
    {
        // The grabbed point follows the mouse: the bone and its parent turn.
        if (parent)
            mDrag.mBoneSolve.push_back(parent->getName());
        mDrag.mBoneSolve.push_back(bone.mJoint);
        mDrag.mBoneOffset = (grab - bone.mHead) * ~bone.mRot;
    }
    else if (mode == BONE_SWING_PARENT)
    {
        // The chain above it carries the bone (its own rotation unchanged).
        LLVector3 parent_pos;
        LLQuaternion parent_rot;
        fkJoint(avatar, parent->getName(), parent_pos, parent_rot);
        if (grandparent)
            mDrag.mBoneSolve.push_back(grandparent->getName());
        mDrag.mBoneSolve.push_back(parent->getName());
        mDrag.mBoneOffset = (grab - parent_pos) * ~parent_rot;
    }
    else if (mode == BONE_MOVE)
    {
        mDrag.mBoneStartPos = studio.getBonePosition(avatar, *pj);
    }
    mDrag.mBoneJoints = mDrag.mBoneSolve;
    mDrag.mBoneJoints.push_back(bone.mJoint);
    for (LLJoint* child : lljoint->mChildren)
    {
        if (posable(child))
            mDrag.mBoneJoints.push_back(child->getName());
    }

    if (ring >= 0)
    {
        // As an IK handle's ring (LLManipRotate): axis, center, radius frozen at mouse down.
        Handle handle;
        if (!selectedHandle(avatar, handle))
            return false;
        LLVector3 axes[3];
        ringAxes(handle, axes);
        mDrag.mRingAxis = axes[ring];
        mDrag.mRingCenter = handle.mPos;
        mDrag.mRingRadius = ringRadius(handle.mPos);
        LLVector3 to_cam = camera->getOrigin() - mDrag.mRingCenter;
        to_cam.normVec();
        LLVector3 hit;
        mDrag.mRingEdgeOn = llabs(mDrag.mRingAxis * to_cam) < RING_EDGE_ON_DOT;
        if (!mDrag.mRingEdgeOn && rayPlane(x, y, mDrag.mRingCenter, mDrag.mRingAxis, hit))
        {
            LLVector3 v = hit - mDrag.mRingCenter;
            v -= mDrag.mRingAxis * (v * mDrag.mRingAxis);
            if (v.length() > 1.0e-6f)
            {
                v.normVec();
                mDrag.mRingDown = v;
            }
            else
            {
                mDrag.mRingEdgeOn = true;
            }
        }
        else
        {
            mDrag.mRingEdgeOn = true;
        }
        if (mDrag.mRingEdgeOn)
        {
            mDrag.mRingCamNormal = to_cam;
            mDrag.mRingUp = to_cam % mDrag.mRingAxis;
            mDrag.mRingUp.normVec();
            if (rayPlane(x, y, mDrag.mRingCenter, to_cam, hit))
                mDrag.mRingDownDist = (hit - mDrag.mRingCenter) * mDrag.mRingUp;
        }
    }

    studio.finishBlend(avatar);
    mDrag.mBefore = studio.captureBoneStates(avatar);
    mDrag.mActive = true;
    captureDragStart(avatar);
    mDrag.mSolved = true;
    mDrag.mSolvedGoal = mDrag.mGoal;
    mDrag.mSolvedGoalRot = mDrag.mGoalRot;
    mTool->setMouseCapture(true);
    return true;
}

void SLPoseIK::setBoneLocal(LLVOAvatar* avatar, const std::string& name, const LLQuaternion& local)
{
    SLPoseStudio& studio = SLPoseStudio::instance();
    const SLPoseStudio::joint_t* joint = studio.findJoint(name);
    if (!joint)
        return;
    LLQuaternion rot = local;
    rot.normalize();
    if (SLPoseStudio::isBoneAlone())
        studio.setBoneRotationKeepChildren(avatar, *joint, rot, false);
    else
        studio.setBoneRotation(avatar, *joint, rot, false);
}

void SLPoseIK::onBoneDragHover(LLVOAvatar* avatar, S32, S32, MASK)
{
    if (mDrag.mSolved && mDrag.mGoal == mDrag.mSolvedGoal && mDrag.mGoalRot == mDrag.mSolvedGoalRot)
        return;
    mDrag.mSolved = true;
    mDrag.mSolvedGoal = mDrag.mGoal;
    mDrag.mSolvedGoalRot = mDrag.mGoalRot;

    SLPoseStudio& studio = SLPoseStudio::instance();
    const SLPoseStudio::joint_t* joint = studio.findJoint(mDrag.mBone);
    if (!joint)
        return;
    mApplying = true;
    // As every IK drag: from the pose at mouse down, never on top of the last frame.
    restoreDragStart(avatar);
    LLVector3 parent_pos;
    LLQuaternion parent_rot;
    switch (mDrag.mBoneMode)
    {
        case BONE_ROTATE:
            // A ring or Ctrl+drag: the bone's world rotation is the goal; its head stays.
            if (fkParent(avatar, mDrag.mBone, parent_pos, parent_rot))
                setBoneLocal(avatar, mDrag.mBone, mDrag.mGoalRot * ~parent_rot);
            break;
        case BONE_MOVE:
            // Disconnected: the bone (and what hangs on it) moves on its own -- a position
            // offset in its parent's frame, as the pelvis' (saved with the pose).
            if (fkParent(avatar, mDrag.mBone, parent_pos, parent_rot))
                studio.setBonePosition(avatar, *joint, mDrag.mBoneStartPos + (mDrag.mGoal - mDrag.mStartGoal) * ~parent_rot, false);
            break;
        default:
            solvePointChain(avatar, mDrag.mBoneSolve, mDrag.mBoneOffset, mDrag.mGoal);
            break;
    }
    holdPins(avatar, -1);
    mApplying = false;
}

void SLPoseIK::solvePointChain(LLVOAvatar* avatar, const std::vector<std::string>& joints, const LLVector3& offset_local,
                               const LLVector3& goal)
{
    const S32 count = (S32)joints.size();
    if (count < 1)
        return;
    auto tipNow = [&](LLVector3& tip) -> bool
    {
        LLVector3 pos;
        LLQuaternion rot;
        if (!fkJoint(avatar, joints.back(), pos, rot))
            return false;
        tip = pos + offset_local * rot;
        return true;
    };
    auto posOf = [&](S32 i, LLVector3& pos) -> bool
    {
        if (i >= count)
            return tipNow(pos);
        LLQuaternion rot;
        return fkJoint(avatar, joints[i], pos, rot);
    };
    std::vector<SLPoseIKMath::V3> p;
    for (S32 i = 0; i <= count; ++i)
    {
        LLVector3 pos;
        if (!posOf(i, pos))
            return;
        p.push_back(toV3(pos));
    }
    const std::vector<SLPoseIKMath::V3> q = SLPoseIKMath::fabrik(p, toV3(goal), FABRIK_ITERATIONS, REACHED_M);
    for (S32 i = 0; i < count; ++i)
    {
        LLVector3 a, b, parent_pos;
        LLQuaternion ra, parent_rot;
        if (!fkJoint(avatar, joints[i], a, ra) || !fkParent(avatar, joints[i], parent_pos, parent_rot) || !posOf(i + 1, b))
            return;
        const LLVector3 v = fromV3(q[i + 1]) - a;
        if ((b - a).length() < 1.0e-6f || v.length() < 1.0e-6f)
            continue;
        LLQuaternion turn;
        LLVector3 next;
        if (i + 2 <= count && posOf(i + 2, next))
            turn = aimCarry(b - a, v, next - b, fromV3(q[i + 2]) - fromV3(q[i + 1]));
        else
            turn.shortestArc(b - a, v);
        applyWorldRotation(avatar, joints[i], ra * turn, parent_rot, false);
    }
}

void SLPoseIK::drawBone(const FKBone& bone, const LLColor4& color) const
{
    // Blender's octahedral bone: a square waist a tenth of the way along, points at both ends.
    LLVector3 dir = bone.mTail - bone.mHead;
    const F32 len = dir.length();
    if (len < 1.0e-5f)
        return;
    dir *= 1.f / len;
    // The waist turns with the bone (its own Y / Z axes, projected), so a twist shows.
    LLVector3 e1 = LLVector3::y_axis * bone.mRot;
    e1 -= dir * (e1 * dir);
    if (e1.length() < 1.0e-3f)
        e1 = perpendicular(dir);
    e1.normVec();
    LLVector3 e2 = dir % e1;
    e2.normVec();
    const F32 w = 0.1f * len;
    const LLVector3 waist = bone.mHead + dir * (0.1f * len);
    const LLVector3 c[4] = { waist + e1 * w, waist + e2 * w, waist - e1 * w, waist - e2 * w };
    const LLVector3 view = LLViewerCamera::getInstance()->getAtAxis();
    gGL.begin(LLRender::TRIANGLES);
    for (S32 k = 0; k < 4; ++k)
    {
        const LLVector3& p0 = c[k];
        const LLVector3& p1 = c[(k + 1) % 4];
        for (S32 end = 0; end < 2; ++end)
        {
            const LLVector3& apex = end ? bone.mTail : bone.mHead;
            LLVector3 n = (p1 - p0) % (apex - p0);
            n.normVec();
            const F32 shade = 0.55f + 0.45f * llabs(n * view);
            gGL.color4f(color.mV[VRED] * shade, color.mV[VGREEN] * shade, color.mV[VBLUE] * shade, color.mV[VALPHA]);
            gGL.vertex3fv(apex.mV); gGL.vertex3fv(p0.mV); gGL.vertex3fv(p1.mV);
            gGL.vertex3fv(apex.mV); gGL.vertex3fv(p1.mV); gGL.vertex3fv(p0.mV);
        }
    }
    gGL.end();
}
