/**
 * @file slposeik.h
 * @brief Pose Studio IK handles (#530): a translucent cone at the end of each bone chain
 *        (hands, feet, head, pelvis; tail, wings, ears, hind limbs on Bento skeletons)
 *        that you drag in the world, solved by Pose Studio's own IK into ordinary Pose
 *        Studio bone rotations -- so IK-made poses save, mirror, sequence and share like
 *        hand-made ones.
 *
 * Solvers: analytic two-bone for 3-joint limbs (arms, legs, hind limbs) with an elbow /
 * knee pole handle on the second joint; FABRIK for longer chains -- a pony leg grown down
 * to its hoof (ik_chains.xml "extend"), the tail, wings, ears -- so every joint folds when
 * the limb shortens; look-at for the head (the neck takes part of the turn); the pelvis
 * handle moves / turns the pelvis itself. Every solved rotation goes through the bone's
 * dial model (bone_axes.xml); with SLPoseStudioIKLimits a limb's root and end and every
 * joint of a tail / wing / ear chain stop at their dial ranges, while limb hinges are
 * capped geometrically (max_bend) -- their dial ranges are the pose HUD's, not anatomy.
 *
 * Mouse: drag a cone to move the end of its chain on the plane through it facing the
 * camera (its depth from the camera never changes during the drag; Shift locks to the
 * world axis you start moving along). Ctrl+drag turns the end bone
 * (hand / foot / head facing); Ctrl+Shift+drag rolls it. Right-click a cone to pin it:
 * a pinned hand / foot / head holds its place in the world while you pose the rest.
 * One undo step per drag (Pose Studio's Undo button).
 *
 * A drag solves from the pose captured at mouse down every time (never on top of its own
 * last result), so the pose depends only on where the handle is and stands still when the
 * mouse does. Limits stop a bone along its path from the start rotation (no per-dial clamp).
 * The handle sits where the worn rigged mesh ends the limb: the farthest rigged-vertex
 * region (LLVOAvatar's joint rigging info) below the chain's end joint, e.g. the hoof on a
 * pony rig, capped near the knuckles on a human hand. The solver moves the end joint; the
 * handle point rides along at its fixed offset. Bento chains (and wings) only show when a
 * worn rigged mesh binds them.
 *
 * Handles are IK targets (as in Blender): each limb handle has its own position and
 * orientation, kept in the avatar's root frame, independent of the bones. Moving it moves
 * only its position, the rings only turn it; the chain's end bone takes its orientation and
 * the chain reaches for its position -- stretching toward it, straight, when it is out of
 * reach (a line shows the gap). A handle starts on its bones and follows them until it is
 * placed (dragged, turned, pinned, or its pole moved); from then on it stays exactly where
 * it was put -- position and orientation -- whatever moves the bones (the hips, other
 * handles, dials, mirror, a mesh LOD change); the gap line shows where the chain ends now.
 * A pinned target also re-solves its chain onto itself after other edits. "Snap", a loaded
 * pose and undo / redo put targets back on the bones / where they were.
 * Elbow / knee diamonds are pole targets in front of the joint they bend.
 *
 * Gizmos (like the build tool): click a handle to select it. The selected handle shows
 * the translate gizmo -- three world-axis arrows (X red, Y green, Z blue) and three plane
 * squares, screen-size, on the side facing the camera, picked and dragged like
 * LLManipTranslate (an arrow moves along its axis only, a square within its plane). Hold
 * Ctrl to swap it for the rotate rings: three rings in the target's own axes, picked and
 * dragged like LLManipRotate; turning one turns the target (the end bone follows) while the
 * handle point stays where it is. Shift: 15 degree ring steps. Dragging the handle body
 * itself is still the free (camera-plane) move; Ctrl+drag on it turns it freely. Click the
 * selected handle again to hide the gizmo.
 *
 * #1566: two-bone limbs keep the bend plane they had at mouse down (SLPoseIKMath::bendSide,
 * tested in scripts/check/test_pose_ik.cpp), so a knee / elbow never flips while its handle
 * moves; SLPoseStudioIKLegacyLimb brings back the old per-pass side for comparison. The head
 * handle sits on the nose and pulls it (neck + head + nose as one FABRIK chain); the white
 * gaze diamond is a look-at point the eyes turn to first, then the head, then the neck, each
 * within its limit (right-click pins it: the eyes keep finding it). The hips box moves the
 * pelvis with the legs planted; the chest box bends the spine and carries the arms / head;
 * the root diamond (under the hips) carries every target along unless "IK targets follow
 * the hips" (SLPoseStudioIKFollowRoot) is off, when they stay and re-solve. The tail shows
 * whenever a worn rigged mesh uses it.
 *
 * The chains are data (app_settings/posestudio/ik_chains.xml).
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

#ifndef SL_POSEIK_H
#define SL_POSEIK_H

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/signals2.hpp>

#include "llframetimer.h"
#include "llpointer.h"
#include "llsingleton.h"
#include "slposeikmath.h"
#include "slposestudio.h"
#include "v4color.h"

class LLJoint;
class LLVOAvatar;
class SLPoseIKTool;

class SLPoseIK : public LLSingleton<SLPoseIK>
{
    LLSINGLETON(SLPoseIK);
    ~SLPoseIK() override;
    LOG_CLASS(SLPoseIK);

public:
    enum ESolver
    {
        SOLVER_TWOBONE = 0,
        SOLVER_CCD,
        SOLVER_LOOKAT,
        SOLVER_PELVIS,
        SOLVER_REACH,   // neck + head: the nose (tip) reaches the pulled point (#1566)
        SOLVER_GAZE,    // eyes -> head -> neck look-at, each within its limit (#1566)
        SOLVER_HIPS,    // the hips box: moves the pelvis, planted legs re-solve (#1566)
    };

    struct Chain
    {
        std::string mName;
        ESolver mSolver = SOLVER_CCD;
        std::vector<std::string> mJoints;   // root -> end
        // Fallback tip when no worn rigged mesh is rigged below the end joint (system
        // avatar, mesh still loading): the first of these the avatar has; empty: the end.
        std::vector<std::string> mTip;
        // A hand: when at least mHandBranches child branches of the end joint carry rigged
        // fingers, the tip search stops mHandDepth joints below the end (the knuckles), so
        // a human hand's handle is on the hand, not a fingertip. 0: no cap.
        S32 mHandBranches = 0;
        S32 mHandDepth = 1;
        std::string mHandSkip;      // hand only: joints whose name contains this never set the handle (the thumb)
        std::string mMirror;
        LLColor4 mColor{ 0.4f, 0.8f, 1.f, 1.f };
        bool mBento = false;
        std::string mGroup;         // "wings": shown by SLPoseStudioIKWings
        bool mRigged = false;       // only shown when a worn rigged mesh binds the chain
        LLVector3 mForward{ 1.f, 0.f, 0.f };    // lookat: aim axis in the end joint's frame
        LLVector3 mBend{ 1.f, 0.f, 0.f };       // twobone: where a straight limb bends (root frame)
        // twobone / ccd: when the handle point (worn mesh) lies below the end joint and the
        // end is not a hand, the chain grows by up to this many joints toward it, so a hoof
        // below the wrist / cannon is the end bone and the joints above it all fold. 0: never.
        S32 mExtend = 0;
        F32 mMaxBend = 160.f;                   // twobone: hinge bend cap, degrees from straight
        std::vector<F32> mShare;                // lookat: turn share of each joint before the end
        // #1566
        bool mBox = false;                      // drawn as a box (hips, chest) instead of a cone
        bool mPlanted = false;                  // its target stays put when the hips box moves (legs)
        // gaze: the eye bones it can turn, by SLPoseIKMath::EyeSetPick (eyes, alt, nose, both);
        // "Eye IK" (SLPoseStudioIKEyeBones) picks one. mForward: the set's aim axis in each
        // eye's parent frame.
        struct EyeSet
        {
            std::vector<std::string> mJoints;
            LLVector3 mForward{ 1.f, 0.f, 0.f };
            // #1567 type "pupil" (the Open Pony): the eyes look by moving, not turning; one
            // mapping per joint (empty: a turning set).
            std::vector<SLPoseIKMath::PupilBone> mPupil;
        };
        EyeSet mEyeSets[SLPoseIKMath::EYE_SET_BOTH + 1];
        std::vector<F32> mLimits;               // gaze: cone limits, degrees: eyes, then mJoints tip -> root
        F32 mTipLength = 0.12f;                 // reach: nose distance in front of the end joint without a tip joint
    };

    // Handles are drawn and pickable: Pose Studio open, "IK handles" on, its target posed
    // by us (self, owned animesh, or a verified consent grant), not in mouselook.
    bool isActive() const;
    LLVOAvatar* getActiveAvatar() const;

    // World overlay segment (world camera loaded, agent coordinates).
    void render();

    // From LLViewerWindow, after the UI had its chance and before the current tool:
    // true when a handle took the click / hover.
    bool handleMouseClick(S32 x, S32 y, MASK mask, S32 clicktype, bool down);
    bool handleHover(S32 x, S32 y, MASK mask);
    void clearHover() { mHoverChain = -1; mHoverPole = false; mHoverRing = -1; mHoverMove = -1; mHoverBone.clear(); }

    // Drag (mouse captured by the IK tool).
    void onDragHover(S32 x, S32 y, MASK mask);
    void onDragEnd(bool cancelled);
private:
    bool updateDragGoal(S32 x, S32 y, MASK mask);
public:

    // One undo step per drag. Any edit made another way clears these.
    bool canUndo(LLVOAvatar* avatar) const;
    bool canRedo(LLVOAvatar* avatar) const;
    void undo(LLVOAvatar* avatar);
    void redo(LLVOAvatar* avatar);

    bool hasPins() const;
    void clearPins();
    // FK bones view (#1566 item 7): the bone picked in the world or in the floater's figure.
    void setSelectedBone(const std::string& joint_name);
    const std::string& getSelectedBone() const { return mSelectedBone; }
    // Every IK target (and pole) back onto its bones, as they are now.
    void snapToBones();
    // Forgets pins, poles and undo history (target changed, posing stopped, studio closed).
    void reset();
    // Pose Studio's Reset All (joint empty) / Reset bone ran on avatar: every handle, pin,
    // pole and look-at point (Reset All), or those of the chains the bone belongs to, go
    // back to rest on the reset skeleton, with the solver state cached for them.
    void onBonesReset(LLVOAvatar* avatar, const std::string& joint);

private:
    struct ChainState
    {
        bool mPinned = false;                   // the target holds against other edits
        // Dragged, turned, pinned or given a pole since it was last put on its bones: the
        // target is the user's and never follows the bones again (see updateTargets).
        bool mPlaced = false;
        std::vector<LLQuaternion> mPinBones;    // chain bone targets when the pin was last held
        bool mHasPole = false;
        LLVector3 mPole;                        // pole target, root frame
        // The IK target (two-bone / CCD chains), root frame: handle point and the end
        // bone's world orientation, and the cone's axis in the target's frame.
        bool mHasTarget = false;
        LLVector3 mTargetPos;
        LLQuaternion mTargetRot;
        LLVector3 mConeLocal{ 1.f, 0.f, 0.f };
        // The chain's own pose (root frame) when the target last matched / solved it: when
        // the bones differ from this, something else moved them.
        LLVector3 mSeenPos;
        LLQuaternion mSeenRot;
        // Gaze (#1566): the look-at point, root frame, once it has been dragged.
        bool mHasLook = false;
        LLVector3 mLook;
        // The bone the chain hangs from (its root's parent) when the targets last matched it,
        // in the follow frame (the avatar root, or with "IK targets follow the hips" off the
        // chest / pelvis): when it moves, unpinned placed targets ride along (followAnchors).
        bool mHasAnchor = false;
        bool mAnchorFollowRoot = true;
        LLVector3 mAnchorPos;
        LLQuaternion mAnchorRot;
    };

    struct Handle
    {
        S32 mChain = -1;
        bool mPole = false;
        LLVector3 mPos;     // goal point (pick / drag origin)
        LLVector3 mDir;     // cone axis
        LLQuaternion mRot;  // end joint world rotation
    };

    // The chain's handle point on the current avatar, from its worn rigged meshes.
    struct TipInfo
    {
        bool mBound = false;    // a worn rigged mesh binds the chain (or below its end)
        std::string mJoint;     // empty: the end joint itself
        LLVector3 mOffset;      // handle point in mJoint's frame
        // The joints the solver works on, root -> end (the chain's own, grown toward the
        // handle point by Chain::mExtend); empty: the chain's own.
        std::vector<std::string> mJoints;
        // gaze: the eye set "Auto" turns, from what the worn rigged meshes weight.
        SLPoseIKMath::EyeSetPick mAutoEyes = SLPoseIKMath::EYE_SET_EYES;
    };

    // What every bone a drag may touch was at mouse down. Each drag frame restores these
    // and solves from them, so a solve is a pure function of (start pose, goal): nothing
    // accumulates from frame to frame, and the pose stands still while the mouse does.
    struct StartBone
    {
        LLQuaternion mLocal;        // what fkWorld read: the target if posed, else the live rotation
        SLPoseAngles mReference;    // dial decode reference
        bool mPosed = false;
    };

    struct DragState
    {
        bool mActive = false;
        S32 mChain = -1;
        bool mPole = false;
        bool mRotate = false;
        bool mRoll = false;
        S32 mStartX = 0;
        S32 mStartY = 0;
        LLVector3 mStartGoal;
        LLVector3 mStartHit;
        LLVector3 mPlaneNormal;
        F32 mDepth = 0.f;           // free drag: the handle's depth along the view axis at mouse down
        F32 mGrabLeft = 0.f;        // free drag: handle - grab point, camera left / up components
        F32 mGrabUp = 0.f;
        LLQuaternion mStartRot;
        LLVector3 mStartPelvisPos;  // pelvis: public position at mouse down
        std::vector<ChainState> mStatesBefore;  // targets / poles at mouse down
        S32 mAxisLock = -1;         // Shift: world axis, chosen once the drag has a direction
        // Rotate ring drag (mRotate is set too), LLManipRotate's model: everything frozen at
        // mouse down (axis, center, radius, edge-on or not). In the ring's plane the angle is
        // between the mouse-down and current mouse directions from the center (unwrapped,
        // never accumulated per frame); edge-on, the mouse's travel across the ring.
        S32 mRing = -1;
        LLVector3 mRingAxis;
        LLVector3 mRingCenter;
        F32 mRingRadius = 0.f;
        bool mRingEdgeOn = false;
        LLVector3 mRingDown;        // in-plane: unit mouse-down direction from the center
        LLVector3 mRingUp;          // edge-on: direction across the ring on the view plane
        LLVector3 mRingCamNormal;   // edge-on: that view plane's normal
        F32 mRingDownDist = 0.f;
        F32 mRingAngle = 0.f;
        // Translate gizmo drag: an arrow (world axis 0..2) or a plane square (its normal's
        // axis 0..2), frozen at mouse down like LLManipTranslate.
        S32 mMoveAxis = -1;
        S32 mMovePlane = -1;
        LLVector3 mMoveDir;         // arrow: unit world axis; plane: unit normal
        F32 mMoveDown = 0.f;        // arrow: the mouse's parameter along the axis at mouse down
        LLVector3 mMoveDownHit;     // plane: the mouse's hit on the plane at mouse down
        bool mWasSelected = false;
        LLVector3 mGoal;
        LLQuaternion mGoalRot;
        std::vector<SLPoseStudio::BoneState> mBefore;
        std::map<std::string, StartBone> mStart;
        bool mSolved = false;       // mGoal / mGoalRot below were solved already
        LLVector3 mSolvedGoal;
        LLQuaternion mSolvedGoalRot;
        // #1566: per chain, the bend plane its limb had at mouse down (two-bone solves keep it).
        std::vector<SLPoseIKMath::BendFrame> mBendFrames;
        // #1566: a body drag (root diamond, hips box, chest box): the bone that carries the
        // other chains and where it was at mouse down (world).
        std::string mBodyJoint;
        LLVector3 mBodyStartPos;
        LLQuaternion mBodyStartRot;
        // #1566: an FK bone drag (mChain is -1).
        std::string mBone;
        S32 mBoneMode = 0;                      // EBoneDrag
        std::vector<std::string> mBoneJoints;   // extra bones restored every frame (the solve's, the children)
        std::vector<std::string> mBoneSolve;    // root -> end, for the swing modes
        LLVector3 mBoneOffset;                  // grabbed point in the solve end's frame
        LLVector3 mBoneStartPos;                // BONE_MOVE: the bone's public position at mouse down
    };

    struct UndoEntry
    {
        std::vector<SLPoseStudio::BoneState> mBefore;
        std::vector<SLPoseStudio::BoneState> mAfter;
        std::vector<ChainState> mTargetsBefore;
        std::vector<ChainState> mTargetsAfter;
    };

    // FK bones view (#1566 item 7): every posed body / hand bone (Bento bones only where a
    // worn rigged mesh uses them, never the face) drawn like Blender's octahedral bones.
    struct FKBone
    {
        std::string mJoint;
        LLVector3 mHead;        // the joint
        LLVector3 mTail;        // its child joint(s), or on along the parent bone for a leaf
        LLQuaternion mRot;      // the joint's world rotation
    };
    enum EBoneDrag
    {
        BONE_SWING = 0,         // the grabbed point follows the mouse; the bone and its parent turn
        BONE_SWING_PARENT,      // near half: the chain above it turns (its parent and grandparent)
        BONE_MOVE,              // Shift: the bone moves on its own (position offset: disconnected)
        BONE_ROTATE,            // a ring, or Ctrl+drag: the bone turns about its head
    };
    static bool ikShown();
    static bool fkShown();
    void buildBones(LLVOAvatar* avatar, std::vector<FKBone>& out) const;
    bool findBone(LLVOAvatar* avatar, const std::string& name, FKBone& out) const;
    bool pickBone(LLVOAvatar* avatar, S32 x, S32 y, FKBone& out, F32& along) const;
    bool startBoneDrag(LLVOAvatar* avatar, S32 x, S32 y, MASK mask, S32 ring);
    void onBoneDragHover(LLVOAvatar* avatar, S32 x, S32 y, MASK mask);
    // Turns joints (root -> end) so the point at offset_local in the end joint's frame
    // reaches goal (FABRIK, no limits: FK is the user's hand).
    void solvePointChain(LLVOAvatar* avatar, const std::vector<std::string>& joints, const LLVector3& offset_local,
                         const LLVector3& goal);
    // The bone's local rotation becomes `local`, honouring "Move bone alone".
    void setBoneLocal(LLVOAvatar* avatar, const std::string& name, const LLQuaternion& local);
    void drawBone(const FKBone& bone, const LLColor4& color) const;

    void loadChains();
    bool chainUsable(LLVOAvatar* avatar, S32 chain) const;
    void syncAvatar(LLVOAvatar* avatar);
    F32 handleScale(LLVOAvatar* avatar) const;

    // Forward kinematics on the targets Pose Studio drives the joints to (the joints
    // themselves ease toward them over a few frames), in agent coordinates.
    bool fkWorld(LLVOAvatar* avatar, LLJoint* joint, LLVector3& pos, LLQuaternion& rot) const;
    bool fkJoint(LLVOAvatar* avatar, const std::string& name, LLVector3& pos, LLQuaternion& rot) const;
    bool fkParent(LLVOAvatar* avatar, const std::string& name, LLVector3& pos, LLQuaternion& rot) const;
    // FK MEMO: while a scope is open, fkWorld computes each joint once and reuses it (it
    // otherwise walks to the root on every call, three or four linear name scans per level:
    // with the FK bones shown, ~1200 levels a frame). Open a scope only where the pose does not change
    // until it closes (a draw, a pick); scopes nest, the outermost clears the memo.
    class FkMemoScope
    {
    public:
        explicit FkMemoScope(const SLPoseIK& ik);
        ~FkMemoScope();
        FkMemoScope(const FkMemoScope&) = delete;
        FkMemoScope& operator=(const FkMemoScope&) = delete;
    private:
        const SLPoseIK& mIK;
    };
    // SLPoseStudio::findJoint (a linear case-insensitive scan), remembered per joint name.
    const SLPoseStudio::joint_t* poserJoint(const std::string& name) const;
    // Handle points from the worn rigged meshes (recomputed when the outfit's rigging
    // changes, never mid-drag); logs each chain's choice at INFO.
    void updateTips(LLVOAvatar* avatar);
    bool hasTip(S32 chain) const { return chain >= 0 && chain < (S32)mTips.size() && !mTips[chain].mJoint.empty(); }
    // The joints the chain solves, root -> end (see TipInfo::mJoints).
    const std::vector<std::string>& chainJoints(S32 chain) const;
    // Where the chain's handle is (tip), and the end joint's position and rotation.
    bool fkEffector(LLVOAvatar* avatar, S32 chain, LLVector3& tip_pos, LLVector3& end_pos, LLQuaternion& end_rot) const;
    // Drag start snapshot (see StartBone).
    void captureDragStart(LLVOAvatar* avatar);
    void restoreDragStart(LLVOAvatar* avatar);
    // The bone's start rotation and dial reference: the drag's snapshot while dragging,
    // else what it is now.
    void startOf(LLVOAvatar* avatar, const std::string& name, LLQuaternion& local, SLPoseAngles& reference) const;
    // Sets the bone so its world rotation becomes `world` (parent world rotation given),
    // through the dial model and its limits.
    bool applyWorldRotation(LLVOAvatar* avatar, const std::string& name, const LLQuaternion& world,
                            const LLQuaternion& parent_world, bool limits);

    void buildHandles(LLVOAvatar* avatar, std::vector<Handle>& out) const;
    bool pick(LLVOAvatar* avatar, S32 x, S32 y, Handle& out) const;
    bool rayPlane(S32 x, S32 y, const LLVector3& point, const LLVector3& normal, LLVector3& hit) const;
    // Rotate rings: shown on the selected chain's handle.
    bool ringsEnabled() const;
    void ringAxes(const Handle& handle, LLVector3 axes[3]) const;
    bool selectedHandle(LLVOAvatar* avatar, Handle& out) const;
    bool pickRing(LLVOAvatar* avatar, S32 x, S32 y, Handle& handle, S32& axis) const;
    // Translate gizmo (LLManipTranslate's look): three world-axis arrows and three plane
    // squares on the selected handle, on the side facing the camera. part: 0..2 the arrow
    // along that axis, 3..5 the square across axis part - 3.
    void moveAxes(const LLVector3& center, LLVector3 axes[3]) const;
    bool pickMove(LLVOAvatar* avatar, S32 x, S32 y, Handle& handle, S32& part) const;
    // The parameter along a line (point + s * dir) nearest the mouse ray.
    F32 mouseAlongLine(S32 x, S32 y, const LLVector3& point, const LLVector3& dir) const;
    // Ctrl swaps the selected handle's translate arrows for the rotate rings.
    bool gizmoRotateMode() const;
    // Metres per screen pixel at a point's depth; ring radius there (screen-size constant).
    F32 metersPerPixel(const LLVector3& point) const;
    F32 ringRadius(const LLVector3& center) const;

    // Solves the chain so its tip (handle point) reaches goal (and, if given, its end bone
    // takes end_rot).
    void solveChain(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot);
    void solveTwoBone(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits);
    void solveMulti(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits);
    void solveCCD(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool limits, S32 iterations);
    // Dial limits on this joint of the chain: every joint of a CCD chain (tail, wings, ears);
    // a limb's root only (its hinges are capped by Chain::mMaxBend, the end by setEndRotation).
    bool limitJoint(S32 chain, S32 index) const;
    void solveLookAt(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, const LLQuaternion* end_rot, bool limits);
    void setEndRotation(LLVOAvatar* avatar, S32 chain, const LLQuaternion& end_rot, bool limits);
    // #1566: the old limb solve (side from the bones each pass), for comparison.
    static bool legacyLimb();
    // The bend plane a two-bone chain has now (or had at mouse down, while dragging).
    SLPoseIKMath::BendFrame bendFrame(LLVOAvatar* avatar, S32 chain) const;
    // Neck + head as one chain whose last link ends at the nose (tip).
    void solveReach(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool limits);
    // Eyes first, then head, then neck, each within its limit.
    void solveGaze(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool limits);
    bool eyeCenter(LLVOAvatar* avatar, S32 chain, LLVector3& pos) const;
    // gaze: the eye set the chain turns now (SLPoseStudioIKEyeBones; Auto: mTips' pick).
    const Chain::EyeSet& gazeEyes(LLVOAvatar* avatar, S32 chain) const;
    // Turns the gaze chain's eyes toward goal, each from its own rest axis within the eye
    // limit. unchanged_ok: leave an eye already there alone (a pinned look-at re-aiming).
    void aimEyes(LLVOAvatar* avatar, S32 chain, const LLVector3& goal, bool unchanged_ok);
    LLVector3 lookWorld(LLVOAvatar* avatar, S32 chain) const;
    // Body drags (#1566 items 3, 5): the other chains' targets follow the dragged bone, or
    // hold in place and re-solve (planted legs under the hips box; every chain under the root
    // diamond with "IK targets follow the hips" off).
    bool isBelow(LLVOAvatar* avatar, const std::string& joint, const std::string& ancestor) const;
    void carryTargets(LLVOAvatar* avatar);
    // #1566: a placed, unpinned target rides on the bone its chain hangs from, whatever moves
    // that bone (another chain's solve, FK drag, dials, a body box, the root).
    bool followFrame(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot) const;
    bool anchorRelative(LLVOAvatar* avatar, S32 chain, bool follow_root, LLVector3& pos, LLQuaternion& rot) const;
    void recordAnchor(LLVOAvatar* avatar, S32 chain);
    void recordAnchors(LLVOAvatar* avatar);
    void followAnchors(LLVOAvatar* avatar);

    // Pins: re-solve every pinned chain (but `except`) back onto its pin.
    void holdPins(LLVOAvatar* avatar, S32 except);
    void pinChain(LLVOAvatar* avatar, S32 chain);

    // IK targets (see ChainState).
    bool usesTarget(S32 chain) const;
    bool rootFrame(LLVOAvatar* avatar, LLVector3& pos, LLQuaternion& rot) const;
    // The chain's bones as a target would describe them, root frame.
    bool chainPose(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot, LLVector3& cone) const;
    bool targetWorld(LLVOAvatar* avatar, S32 chain, LLVector3& pos, LLQuaternion& rot) const;
    void setTargetWorld(LLVOAvatar* avatar, S32 chain, const LLVector3* pos, const LLQuaternion* rot);
    LLVector3 poleWorld(LLVOAvatar* avatar, S32 chain) const;
    void setPoleWorld(LLVOAvatar* avatar, S32 chain, const LLVector3& pos);
    // Where a pole diamond sits before one is set: in front of the joint it bends.
    LLVector3 defaultPole(LLVOAvatar* avatar, S32 chain) const;
    void snapTarget(LLVOAvatar* avatar, S32 chain);
    void markSeen(LLVOAvatar* avatar, S32 chain);
    void snapAllTargets(LLVOAvatar* avatar);
    void updateTargets(LLVOAvatar* avatar);
    void restoreTargets(LLVOAvatar* avatar, const std::vector<ChainState>& states);
    std::vector<LLQuaternion> chainTargets(LLVOAvatar* avatar, S32 chain) const;

    void onStudioEdited(const LLUUID& avatar_id, bool committed);
    void onStudioPosing(const LLUUID& avatar_id, bool posing);

    void drawCone(const LLVector3& base, const LLVector3& dir, F32 length, F32 radius, const LLColor4& color) const;
    void drawDiamond(const LLVector3& center, F32 size, const LLColor4& color) const;
    void drawBox(const LLVector3& center, const LLQuaternion& rot, F32 half, const LLColor4& color) const;
    void drawRing(const LLVector3& center, const LLVector3& normal, F32 radius, const LLColor4& color) const;
    void drawBand(const LLVector3& center, const LLVector3& normal, F32 radius, F32 width, const LLColor4& color,
                  F32 back_alpha) const;
    void drawRibbon(const LLVector3& from, const LLVector3& to, F32 half_width, const LLColor4& color) const;
    void drawQuad(const LLVector3& corner, const LLVector3& u, const LLVector3& v, const LLColor4& color) const;
    void drawMoveGizmo(const LLVector3& center, bool xray) const;

    std::vector<Chain> mChains;
    std::vector<ChainState> mStates;    // per chain, for mAvatarID
    LLUUID mAvatarID;
    std::vector<TipInfo> mTips;         // per chain, for mTipsAvatar
    LLUUID mTipsAvatar;
    size_t mTipsKey = 0;
    bool mTipsValid = false;
    LLFrameTimer mTipsTimer;

    // FkMemoScope state: the open scopes' joint transforms (cleared when the outermost
    // closes), and the poser joint per skeleton joint name (fixed table: at most one entry
    // per joint name the skeleton has).
    struct FkMemoEntry
    {
        LLVector3 mPos;
        LLQuaternion mRot;
    };
    mutable std::unordered_map<const LLJoint*, FkMemoEntry> mFkMemo;
    mutable S32 mFkMemoDepth = 0;
    mutable std::unordered_map<std::string, const SLPoseStudio::joint_t*> mPoserJointByName;

    DragState mDrag;
    S32 mHoverChain = -1;
    bool mHoverPole = false;
    S32 mHoverRing = -1;        // axis of the selected handle's ring under the mouse
    S32 mHoverMove = -1;        // part of the selected handle's translate gizmo under the mouse
    S32 mSelectedChain = -1;    // its handle shows the rotate rings
    std::string mSelectedBone;  // FK view: the picked bone (its rings show with Ctrl)
    std::string mHoverBone;
    bool mApplying = false;     // our own edits: the studio's edited signal ignores them

    std::vector<UndoEntry> mUndo;
    std::vector<UndoEntry> mRedo;

    LLPointer<SLPoseIKTool> mTool;
    boost::signals2::scoped_connection mEditedConnection;
    boost::signals2::scoped_connection mPosingConnection;
};

#endif // SL_POSEIK_H
