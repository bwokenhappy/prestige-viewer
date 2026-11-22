/**
 * @file llvrcamera.cpp
 * @brief VR milestone M1 (#1074): the head-tracked viewer camera. See llvrcamera.h.
 *
 * Frames: OpenXR app space is right-handed, +Y up, -Z forward (metres). SL agent space is
 * X = at, Y = left, Z = up. A vector maps as SL = (-xr.z, -xr.x, xr.y) (a proper rotation).
 * Rotations are done on vectors with the standard quaternion formula, so no quaternion
 * multiplication convention is involved.
 *
 * #1607 the horizontal rig is llvrcameramath.h's mapping (agent = origin + rot(room - ref, turn) * k)
 * with ref and turn owned here. Until #1607 the rig was the avatar's yaw and its head joint: a
 * snap / smooth turn rotated your room offset about the recentre point (the eye swept an arc as
 * wide as you had walked from it), and every body turn (the turn limit, walking) swung the eye
 * about the pelvis by the head's forward offset, with the head joint read under the root's lagging
 * rotation and caught up by the smoothing.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrcamera.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcriticaldamp.h"
#include "llformat.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvrbody.h"
#include "llvrbodymapping.h"
#include "llvrcameramath.h"
#include "llvrhands.h"
#include "m3math.h"
#include "vkgfx/llxrsession.h"

#include <chrono>
#include <cmath>

bool LLVRCamera::sActive = false;

namespace
{
namespace vc = slvrcam;

enum RigMode : U8 { RIG_STAND, RIG_SIT, RIG_FLY };

const char* rigModeName(RigMode m)
{
    return m == RIG_SIT ? "seated" : (m == RIG_FLY ? "fly cam" : "standing");
}

struct VRCameraState
{
    // Recentre: the head's yaw becomes the avatar's facing and its horizontal position the
    // avatar's eyes; the floor height maps onto the avatar's eye height.
    bool recenterPending = true;   // the first located pose takes yaw + horizontal position
    bool recenterHeight = false;   // a user recentre also takes the height
    bool haveRef = false;
    bool refProvisional = false;   // #1076: taken from an untracked pose: redone on the first good one
    F32 refHeight = 1.6f;
    uint32_t spaceGeneration = 0;

    // #1607 the horizontal rig (llvrcameramath.h): origin = the collision shape / seat eye / fly
    // point; ref and turn owned here. The origin's last position (global: a region crossing
    // shifts agent space, not the avatar) for its motion.
    vc::Mapping map;
    RigMode mode = RIG_STAND;
    bool wasSitting = false;
    bool wasTeleporting = false;
    bool haveOrigin = false;
    LLVector3d prevOriginGlobal;
    F32 prevRigYaw = 0.f;
    F32 turnRequest = 0.f;         // turnView since the last apply()
    vc::V2 head;                   // this frame's head (room, SL axes)
    vc::V2 headOff;                // ... from the origin (agent space)
    // A re-anchor (the head onto the origin) waiting for the blink; the view holds meanwhile.
    bool reanchorPending = false;
    bool blinkDarkNow = false;
    const char* reanchorReason = "";
    F32 deferredTurn = 0.f;        // a sit / stand turns the view with the seat, in the dark
    std::chrono::steady_clock::time_point reanchorSince{};
    // The room-scale catch-up (followRoom).
    vc::FollowState follow;
    vc::FollowCommand followCmd;
    std::chrono::steady_clock::time_point followAt{};

    // The avatar's eyes relative to its root (standing: the root's position and yaw; sitting:
    // the avatar's position and the seat's yaw), smoothed (the head joint carries the
    // animation's bob: walk cycles, breathing). Horizontal: the draw offset's eye point (and
    // the seat's origin); vertical: the eye height above the avatar's position.
    bool anchorValid = false;
    bool anchorRejected = false;   // (the eye-reach guard logged; until an anchor passes again)
    LLVector3 anchorLocal;

    // Desktop camera parameters, restored when the head-tracked camera stops.
    bool saved = false;
    F32 savedNear = 0.f;

    // #1076: this frame's app space -> agent space mapping (apply()), shared by the camera,
    // the controllers (appToAgent / handPose) and the hands: a point maps as
    // anchor + rotZ(xy - ref, turn) * k + z (height - refHeight) * k.
    bool mapValid = false;
    LLVector3 mapAnchor;
    F32 mapTurn = 0.f;
    F32 mapRefX = 0.f, mapRefY = 0.f;
    F32 mapHeightAdd = 0.f;   // LOCAL space: + the configured eye height (a height above the floor)
    bool drawValid = false;   // selfDrawOffset: standing on the avatar this frame

    // #1076 fly cam: the rig detached from the avatar (origin flyPos).
    bool fly = false;         // requested (setFlyCam)
    bool flyApplied = false;  // what apply() last composed
    LLVector3 flyPos;
    F32 flyYaw = 0.f;         // the avatar's facing when it started (a recentre in fly cam)

    // #1121: this frame's head axes in agent space (with the rig turn), for the avatar's head
    // follow (headAngles) and the body turn (bodyFollowHead).
    LLVector3 headAt = LLVector3::x_axis;
    LLVector3 headUp = LLVector3::z_axis;
    bool bodyTurning = false;   // bodyFollowHead: turning the body toward the head

    // #1078 world scale: camera metres per real metre this frame (head motion, eye distance,
    // hands), and the avatar's standing eye height above its feet (the "scale to the avatar"
    // view height mode; held while sitting).
    F32 mapScale = 1.f;
    F32 avatarEyeHeight = 0.f;
    bool heightRecentred = false;   // a recentre took your real height (else SlipstreamVREyeHeight, live)
    // #1078 rig motion (the vignette): the view's motion that is not your head's -- where the
    // room's origin maps (walking, flying, a vehicle, the fly cam) and the turn's rate.
    bool motionValid = false;
    LLVector3d prevRoomOrigin;
    F32 prevTurn = 0.f;
    std::chrono::steady_clock::time_point prevAt{};
    F32 rigSpeed = 0.f, rigTurnRate = 0.f;
};
VRCameraState sVR;

LLVector3 xrToSL(F32 x, F32 y, F32 z)
{
    return LLVector3(-z, -x, y);
}

// Standard active rotation of v by the unit quaternion q = (x, y, z, w): v + 2w(u x v) + 2u x (u x v).
void rotateByQuat(const float q[4], const F32 v[3], F32 out[3])
{
    const F32 ux = q[0], uy = q[1], uz = q[2], w = q[3];
    const F32 tx = 2.f * (uy * v[2] - uz * v[1]);
    const F32 ty = 2.f * (uz * v[0] - ux * v[2]);
    const F32 tz = 2.f * (ux * v[1] - uy * v[0]);
    out[0] = v[0] + w * tx + (uy * tz - uz * ty);
    out[1] = v[1] + w * ty + (uz * tx - ux * tz);
    out[2] = v[2] + w * tz + (ux * ty - uy * tx);
}

// An OpenXR head-local direction, rotated by the head, in SL axes.
LLVector3 headDirSL(const float q[4], F32 x, F32 y, F32 z)
{
    const F32 v[3] = {x, y, z};
    F32 r[3];
    rotateByQuat(q, v, r);
    return xrToSL(r[0], r[1], r[2]);
}

LLVector3 rotZ(const LLVector3& v, F32 a)
{
    const F32 c = std::cos(a), s = std::sin(a);
    return LLVector3(v.mV[VX] * c - v.mV[VY] * s, v.mV[VX] * s + v.mV[VY] * c, v.mV[VZ]);
}

F32 yawOf(const LLVector3& fwd)
{
    return std::atan2(fwd.mV[VY], fwd.mV[VX]);
}

F32 yawDiff(F32 a, F32 b)
{
    return vc::wrapAngle(a - b);
}

vc::V2 v2(const LLVector3& v)
{
    return vc::V2(v.mV[VX], v.mV[VY]);
}

// #1607 diagnostic, every 10 s while the headset camera runs: how far your head was from the
// avatar's collision shape (the room-scale offset the avatar catches up), how far the avatar
// walked after you and how much of it the view absorbed, the largest draw offset (the avatar
// drawn under the headset), the view's own turning (snap / smooth turns), the avatar's turning
// (which no longer moves the view) and the re-anchors (the view jumping onto the avatar, behind
// the blink) with the last one's reason.
struct RigStability
{
    bool started = false;
    std::chrono::steady_clock::time_point t0{};
    F32 headOffMax = 0.f, drawMax = 0.f, absorbed = 0.f, viewTurn = 0.f, bodyTurn = 0.f, followTime = 0.f;
    F32 prevBodyYaw = 0.f;
    U32 frames = 0, reanchors = 0;
    const char* lastReason = "";
};
RigStability sRig;

void rigStabilityTick(F32 dt, F32 headOff, F32 drawOff, F32 absorbed, F32 viewTurn, F32 bodyYaw, bool following)
{
    const auto now = std::chrono::steady_clock::now();
    if (!sRig.started)
    {
        const U32 reanchors = sRig.reanchors;
        const char* reason = sRig.lastReason;
        sRig = RigStability{};
        sRig.started = true;
        sRig.t0 = now;
        sRig.prevBodyYaw = bodyYaw;
        sRig.reanchors = reanchors;   // (counted between windows too)
        sRig.lastReason = reason;
    }
    sRig.headOffMax = llmax(sRig.headOffMax, headOff);
    sRig.drawMax = llmax(sRig.drawMax, drawOff);
    sRig.absorbed += absorbed;
    sRig.viewTurn += std::fabs(viewTurn);
    sRig.bodyTurn += std::fabs(yawDiff(bodyYaw, sRig.prevBodyYaw));
    sRig.prevBodyYaw = bodyYaw;
    if (following) sRig.followTime += dt;
    ++sRig.frames;
    if (std::chrono::duration<F32>(now - sRig.t0).count() < 10.f) return;
    LL_INFOS("VR") << llformat("rig over 10 s (%u frames, %s): head up to %.1f cm from the collision shape, avatar "
                               "drawn up to %.1f cm off it; caught up %.1f s (%.1f cm absorbed); view turned %.0f deg, "
                               "body turned %.0f deg (the view ignores it); %u re-anchors%s%s",
                               sRig.frames, rigModeName(sVR.mode), sRig.headOffMax * 100.f, sRig.drawMax * 100.f,
                               sRig.followTime, sRig.absorbed * 100.f, sRig.viewTurn * RAD_TO_DEG,
                               sRig.bodyTurn * RAD_TO_DEG, sRig.reanchors, sRig.reanchors ? ", last: " : "",
                               sRig.reanchors ? sRig.lastReason : "")
                   << LL_ENDL;
    sRig.started = false;
    sRig.reanchors = 0;
}

// The head onto the origin (headOffset 0): a re-anchor's commit (in the dark, or a recentre).
void commitReanchor(const char* how)
{
    vc::reanchor(sVR.map, sVR.head);
    sVR.map.turn = vc::wrapAngle(sVR.map.turn + sVR.deferredTurn);
    const bool was = sVR.reanchorPending;
    sVR.deferredTurn = 0.f;
    sVR.reanchorPending = false;
    sVR.blinkDarkNow = false;
    sVR.follow = vc::FollowState{};
    if (was)
        LL_INFOS("VR") << "view re-anchored on the avatar (" << sVR.reanchorReason << ", " << how << ")" << LL_ENDL;
}

// Hold the view and ask for the blink; the re-anchor commits on its dark frame.
void requestReanchor(const char* reason)
{
    if (!sVR.reanchorPending)
    {
        sVR.reanchorPending = true;
        sVR.reanchorSince = std::chrono::steady_clock::now();
        sVR.reanchorReason = reason;
        ++sRig.reanchors;
        sRig.lastReason = reason;
    }
}

// Back to the desktop camera parameters (the session stopped, or no pose / avatar).
void leaveVRCamera()
{
    sVR.mapValid = false;
    sVR.drawValid = false;
    sVR.bodyTurning = false;
    sVR.motionValid = false;   // #1078
    sVR.rigSpeed = sVR.rigTurnRate = 0.f;
    sVR.fly = sVR.flyApplied = false;   // #1076: a new session starts on the avatar
    // #1607: the mapping and the origin's last position are kept: a pose lost for a moment
    // (focus, the dashboard) comes back where it was; a jump meanwhile goes through the blink.
    sVR.blinkDarkNow = false;
    sVR.turnRequest = 0.f;
    sVR.follow = vc::FollowState{};
    sVR.followCmd = vc::FollowCommand{};
    sRig.started = false;
    LLVRHands::release();   // #1076 (no-op when none are registered)
    if (!sVR.saved) return;
    sVR.saved = false;
    sVR.anchorValid = false;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    cam->setNear(sVR.savedNear);
    if (gViewerWindow)
    {
        cam->setAspect(gViewerWindow->getWorldViewAspectRatio());
        cam->setViewHeightInPixels(gViewerWindow->getWorldViewHeightRaw());
    }
    cam->setView(cam->getDefaultFOV());
    LL_INFOS("VR") << "head-tracked camera off: the desktop camera is back" << LL_ENDL;
}
} // namespace

bool LLVRCamera::sessionRunning()
{
    return LLXrSession::active() && LLXrSession::instance().sessionRunning();
}

void LLVRCamera::recenter()
{
    if (!LLXrSession::active()) return;
    sVR.recenterPending = true;
    sVR.recenterHeight = true;
    LL_INFOS("VR") << "recentre requested" << LL_ENDL;
}

void LLVRCamera::apply()
{
    if (!LLXrSession::active())   // desktop: one static read
    {
        return;
    }
    LLXrSession& xr = LLXrSession::instance();

    LLXrHeadPose hp;
    if (!xr.headPose(hp) || !hp.renderWidth || !hp.renderHeight || !isAgentAvatarValid())
    {
        sActive = false;
        leaveVRCamera();
        return;
    }

    static LLCachedControl<F32> s_eye_height(gSavedSettings, "SlipstreamVREyeHeight", 1.6f);
    static LLCachedControl<F32> s_near_clip(gSavedSettings, "SlipstreamVRNearClip", 0.05f);
    static LLCachedControl<F32> s_smoothing(gSavedSettings, "SlipstreamVRHeadSmoothing", 0.2f);
    const F32 eye_height = llclamp((F32)s_eye_height, 0.5f, 2.5f);
    const auto now = std::chrono::steady_clock::now();

    // ---- The head in SL axes (app space, before the recentre).
    const LLVector3 head_pos = xrToSL(hp.position[0], hp.position[1], hp.position[2]);
    // LOCAL has its origin at the head height at session start: add the configured eye height
    // so all spaces give a height above the floor.
    const F32 head_height = head_pos.mV[VZ] + (hp.floorSpace ? 0.f : eye_height);
    const LLVector3 head_at = headDirSL(hp.orientation, 0.f, 0.f, -1.f);   // OpenXR forward
    const LLVector3 head_up = headDirSL(hp.orientation, 0.f, 1.f, 0.f);    // OpenXR up
    sVR.head = v2(head_pos);

    // ---- The rig's mode and the avatar's facing (the seat's rotation when sitting).
    const bool sitting = gAgentAvatarp->isSitting();
    const LLQuaternion rig_rot = sitting ? gAgentAvatarp->getRenderRotation() : gAgent.getFrameAgent().getQuaternion();
    const F32 avatar_rig_yaw = yawOf(LLVector3::x_axis * rig_rot);
    // #1076 fly cam: entering keeps the current eye and facing (the origin stays where it was).
    // Leaving is composed in the dark (LLVRInput's blink): the head goes straight onto the avatar.
    bool fly_left = false;
    if (sVR.fly != sVR.flyApplied)
    {
        if (sVR.fly)
        {
            if (!sVR.mapValid)
            {
                sVR.fly = false;   // (no eye yet to start from)
            }
            else
            {
                sVR.flyPos = sVR.mapAnchor;
                sVR.flyYaw = avatar_rig_yaw;
                LL_INFOS("VR") << "fly cam on" << LL_ENDL;
            }
        }
        else
        {
            fly_left = true;
            sVR.anchorValid = false;   // straight onto the avatar's eyes (the view is dark: blink)
            LL_INFOS("VR") << "fly cam off: back on the avatar" << LL_ENDL;
        }
        sVR.flyApplied = sVR.fly;
    }
    const RigMode mode = sVR.fly ? RIG_FLY : (sitting ? RIG_SIT : RIG_STAND);
    // A sit / stand changes the eye anchor's frame (the seat's / the root's): taken afresh.
    if (sVR.haveOrigin && sitting != sVR.wasSitting) sVR.anchorValid = false;
    sVR.wasSitting = sitting;

    // ---- Recentre: the first pose, the runtime re-centring its space, or the user's request.
    if (hp.spaceGeneration != sVR.spaceGeneration)
    {
        sVR.spaceGeneration = hp.spaceGeneration;
        sVR.recenterPending = true;
    }
    // #1076: a reference is only taken from a pose that can anchor it (LLXrHeadPose::placeable:
    // tracked, shown, a plausible height). The first headset run recentred on a 5 cm "tracked"
    // head while the session was not even visible. Without any reference yet the camera needs
    // one to render at all: a provisional one is taken (the configured eye height, never the
    // pose's) and replaced by the first placeable pose. A user recentre on a bad pose waits.
    const bool pose_good = hp.placeable();
    const bool redo_provisional = sVR.refProvisional && pose_good;
    if ((sVR.recenterPending && (pose_good || !sVR.haveRef)) || redo_provisional)
    {
        sVR.recenterPending = false;
        // #1607: what you face becomes the avatar's facing (the fly cam's: where it started),
        // where your head is becomes the origin (the collision shape, standing).
        const F32 rig_yaw = mode == RIG_FLY ? sVR.flyYaw : avatar_rig_yaw;
        sVR.map.turn = vc::wrapAngle(rig_yaw - yawOf(head_at));
        sVR.deferredTurn = 0.f;
        sVR.turnRequest = 0.f;
        commitReanchor("recentre");
        const bool take_height = sVR.recenterHeight && pose_good;
        if (take_height || !sVR.haveRef)
            sVR.refHeight = take_height ? head_height : eye_height;
        if (take_height) sVR.heightRecentred = true;   // #1078 (the setting no longer drives it)
        if (pose_good) sVR.recenterHeight = false;   // (a user's height recentre waits for a good pose)
        else sVR.recenterPending = sVR.recenterHeight;
        sVR.haveRef = true;
        sVR.refProvisional = !pose_good;
        LL_INFOS("VR") << llformat("recentred%s: head yaw %.1f deg, position (%.2f, %.2f), height %.2f m (reference %.2f m)",
                                   pose_good ? (redo_provisional ? " on the first tracked pose" : "")
                                             : " provisionally (head not tracked / shown yet, or an implausible height)",
                                   yawOf(head_at) * RAD_TO_DEG, sVR.head.x, sVR.head.y, head_height, sVR.refHeight)
                       << LL_ENDL;
    }
    // #1078: until a recentre took your real height, the eye height preference is the
    // reference, live (Preferences > Safety changes it during the session).
    if (sVR.haveRef && !sVR.heightRecentred) sVR.refHeight = eye_height;

    // ---- The avatar's eyes relative to its body, smoothed against the animation's head bob.
    // Standing, relative to the root as drawn (its position, which carries the draw offset, and
    // its yaw, which lags the agent's on a turn: both cancel out); sitting, to the avatar's
    // position and the seat's yaw (as before #1607). Up: above the avatar's position.
    const LLVector3 avatar_pos = gAgentAvatarp->getRenderPosition();
    LLJoint* root = gAgentAvatarp->getRootJoint();
    const bool body_from_root = !sitting && root;
    const LLVector3 root_pos = body_from_root ? root->getWorldPosition() : avatar_pos;
    const F32 body_yaw = body_from_root ? yawOf(LLVector3::x_axis * root->getWorldRotation()) : avatar_rig_yaw;
    const LLVector3 body_base(root_pos.mV[VX], root_pos.mV[VY], avatar_pos.mV[VZ]);
    LLVector3 eye_anchor = gAgentAvatarp->mHeadp ? gAgentAvatarp->mHeadp->getWorldPosition()
                                                 : avatar_pos + LLVector3(0.f, 0.f, 0.8f);
    // #1121: the avatar's neck and head follow the headset (SLPoseLookAt), and the head joint
    // swings with the neck: anchored on it, the eye would move with that turn on top of the
    // headset's own tracked motion (the world swims). Anchor on the neck pivot plus the head's
    // offset as if the neck itself were not turned (the skeleton's rest rotations are identity:
    // the neck's parent's rotation keeps a lean of the chest or a lying-down sit).
    LLJoint* neck_parent = gAgentAvatarp->mNeckp ? gAgentAvatarp->mNeckp->getParent() : nullptr;
    if (neck_parent && gAgentAvatarp->mHeadp)
    {
        const LLVector3 neck = gAgentAvatarp->mNeckp->getWorldPosition();
        const LLVector3 head_off = (gAgentAvatarp->mHeadp->getWorldPosition() - neck) * ~gAgentAvatarp->mNeckp->getWorldRotation();
        const LLVector3 anchor_head = neck + head_off * neck_parent->getWorldRotation();
        if (anchor_head.isFinite()) eye_anchor = anchor_head;
    }
    // The avatar's own eyes from the head joint (#1592: was a fixed 0.1 / 0.06, under the eyes);
    // the body layer puts the head joint under the camera by the same offset.
    eye_anchor += rotZ(LLVRBodyMap::eyeFromHead(gAgentAvatarp, gAgentAvatarp->mHeadp), body_yaw);
    const LLVector3 anchor_local = rotZ(eye_anchor - body_base, -body_yaw);
    // The eye anchor is a body quantity: the head is never further from the avatar than LL's
    // largest animated pelvis offset (5 m) plus the body. A head joint further off comes from a
    // skeleton not placed yet (its root still at the region origin) or posed from a wrong
    // anchor -- and once the body layer drives the hips from the camera's mapping (#1565, the
    // 1.7 body estimate), a wrong anchor holds itself: the hips go under it, the head lands on
    // it, the next capture reads it back (the camera stayed under the ground). Rejected: the
    // eyes go to their rest height above the avatar, and the next frame captures afresh.
    const F32 eye_reach = 5.f + 1.5f * llmax(gAgentAvatarp->mBodySize.mV[VZ], 1.f);
    const bool anchor_ok = anchor_local.isFinite() && anchor_local.magVec() <= eye_reach;
    const bool held_ok = !sVR.anchorValid || sVR.anchorLocal.magVec() <= eye_reach;
    if (!anchor_ok || !held_ok)
    {
        if (!sVR.anchorRejected)
        {
            LL_WARNS("VR") << llformat("eye anchor rejected: head joint %.1f m from the avatar (held anchor %.1f m, reach %.1f m); "
                                       "the eyes go to their rest height until the skeleton is placed",
                                       anchor_local.magVec(), sVR.anchorValid ? sVR.anchorLocal.magVec() : 0.f, eye_reach)
                           << LL_ENDL;
            sVR.anchorRejected = true;
        }
        sVR.anchorLocal = LLVector3(0.1f, 0.f, 0.86f);   // (the mHeadp-less rest eye below, in the body frame)
        sVR.anchorValid = false;
    }
    else if (sVR.anchorValid && LLVRBody::holdsAnchor())
    {
        // #1565: the body trackers drive the hips / spine from your real body, so the neck now
        // moves with your own crouches and leans -- which the headset's offset already adds.
        // Following it would count them twice (the world swims): the anchor holds where the
        // animated avatar had it until the body layer lets go.
        // #1592: except its height: the driven body's standing eyes (the Open Pony on its hind
        // legs: that stance's, not the animated quadruped's; any body: not where the stand
        // animation slouched them), where the body maps your head at the reference height.
        F32 standing_eye = 0.f;
        if (LLVRBody::standingEyeHeight(standing_eye))
            sVR.anchorLocal.mV[VZ] = lerp(sVR.anchorLocal.mV[VZ], standing_eye - gAgentAvatarp->getPelvisToFoot(),
                                          LLSmoothInterpolation::getInterpolant(0.1f));
    }
    else if (!sVR.anchorValid || (anchor_local - sVR.anchorLocal).magVec() > 1.5f || s_smoothing <= 0.f)
    {
        sVR.anchorLocal = anchor_local;   // first frame, a sit / stand jump, or smoothing off
        sVR.anchorValid = true;
    }
    else
    {
        sVR.anchorLocal = lerp(sVR.anchorLocal, anchor_local,
                               LLSmoothInterpolation::getInterpolant(llclamp((F32)s_smoothing, 0.01f, 2.f)));
    }
    if (anchor_ok && held_ok && sVR.anchorRejected)
    {
        sVR.anchorRejected = false;
        LL_INFOS("VR") << llformat("eye anchor back on the head joint (%.2f m from the avatar)", anchor_local.magVec())
                       << LL_ENDL;
    }

    // ---- #1078 world scale and view height (per account: each avatar has its own size).
    // Scale k = camera metres per real metre: your head's motion, the distance between your
    // eyes (stereo depth) and your hands scale by it, so a k below 1 makes you small in a big
    // world. View height mode: 0 = your standing height maps onto the avatar's eyes (1:1, as
    // mouselook); 1 = the view at YOUR real height above the avatar's feet (whatever the
    // avatar's eye height); 2 = the world scaled so your standing height maps onto the
    // avatar's eyes (a short avatar: the world looks big, as it would to it).
    static LLCachedControl<F32> s_world_scale(gSavedPerAccountSettings, "SlipstreamVRWorldScale", 1.f);
    static LLCachedControl<U32> s_height_mode(gSavedPerAccountSettings, "SlipstreamVRViewHeightMode", 0);
    const F32 foot_z = avatar_pos.mV[VZ] - gAgentAvatarp->getPelvisToFoot();
    if (!sitting || sVR.avatarEyeHeight <= 0.f)
        sVR.avatarEyeHeight = llclamp(sVR.anchorLocal.mV[VZ] + gAgentAvatarp->getPelvisToFoot(), 0.1f, 10.f);
    F32 k = 1.f / llclamp((F32)s_world_scale, 0.25f, 4.f);
    if (s_height_mode == 2u) k *= sVR.avatarEyeHeight / llmax(0.3f, sVR.refHeight);
    k = llclamp(k, 0.1f, 10.f);
    sVR.map.k = k;

    // ---- #1607 the origin: the collision shape's centre (standing: the avatar's position as the
    // viewer has it), the seat's eye anchor (sitting), the fly cam's point. Up: the avatar's eyes.
    LLVector3 anchor;
    if (mode == RIG_FLY)
    {
        anchor = sVR.flyPos;
    }
    else
    {
        anchor = mode == RIG_SIT ? avatar_pos + rotZ(sVR.anchorLocal, avatar_rig_yaw)
                                 : LLVector3(avatar_pos.mV[VX], avatar_pos.mV[VY], avatar_pos.mV[VZ] + sVR.anchorLocal.mV[VZ]);
        if (s_height_mode == 1u && mode == RIG_STAND)
            anchor.mV[VZ] = foot_z + sVR.refHeight * k;   // the eye = feet + your real height (scaled)
    }
    const vc::V2 origin = v2(anchor);
    const LLVector3d origin_global = gAgent.getPosGlobalFromAgent(LLVector3(origin.x, origin.y, 0.f));
    vc::V2 moved;
    if (sVR.haveOrigin)
    {
        const LLVector3d d = origin_global - sVR.prevOriginGlobal;
        moved = vc::V2((F32)d.mdV[VX], (F32)d.mdV[VY]);
        if (!vc::finite(moved)) moved = vc::V2();
    }
    const F32 dt = sVR.motionValid ? llclamp(std::chrono::duration<F32>(now - sVR.prevAt).count(), 0.f, 0.5f) : 0.f;
    sVR.map.origin = origin;

    // ---- #1607 what the origin's motion does to the view. Walking with the stick, flying, a
    // vehicle: the view moves with it. The avatar catching up with your room-scale position,
    // and anything that jumps it (a server correction, a push, a sit / stand): the view holds,
    // and a jump is taken behind the blink (the head goes onto the origin in the dark).
    F32 absorbed = 0.f;
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
    if (!sVR.haveOrigin)
    {
        // (first frame on the avatar: the recentre above, or the head onto the origin)
        if (!sVR.reanchorPending) vc::reanchor(sVR.map, sVR.head);
    }
    else if (fly_left)
    {
        // The blink's black frame (LLVRInput): the head straight onto the avatar.
        if (!sVR.reanchorPending) sVR.reanchorReason = "fly cam off";
        sVR.reanchorPending = true;   // (so the commit logs)
        commitReanchor("in the blink");
    }
    else if (teleporting)
    {
        // The view holds through the teleport (its fade, LLVRInput::comfortFadeTick, takes
        // 0.2 s to go dark); the head goes onto the avatar on the first frame after it, which
        // the fade still keeps dark (0.4 s).
        vc::absorb(sVR.map, moved);
        absorbed = vc::length(moved);
    }
    else if (sVR.wasTeleporting)
    {
        if (sVR.reanchorPending) commitReanchor("after the teleport, in its fade");
        else vc::reanchor(sVR.map, sVR.head);
    }
    else if (mode == RIG_FLY)
    {
        // (the fly point moves only by flyMove: the view's own motion)
    }
    else
    {
        if (mode != sVR.mode && sVR.mode != RIG_FLY)
        {
            // Sit / stand: the view holds, turns with the body in the dark, and lands on the
            // seat's eyes / the collision shape.
            sVR.deferredTurn += yawDiff(avatar_rig_yaw, sVR.prevRigYaw);
            vc::absorb(sVR.map, moved);
            absorbed = vc::length(moved);
            requestReanchor(mode == RIG_SIT ? "sat down" : "stood up");
        }
        else if (sVR.reanchorPending)
        {
            vc::absorb(sVR.map, moved);   // (holding for the blink)
            absorbed = vc::length(moved);
        }
        else if (vc::isJump(vc::length(moved), gAgent.getVelocity().magVec(), dt, mode == RIG_SIT ? 5.f : 0.5f))
        {
            // (seated, only a jump no vehicle makes in one frame: a seat's velocity is not
            // always the avatar's, and a fast vehicle must never blink)
            vc::absorb(sVR.map, moved);
            absorbed = vc::length(moved);
            requestReanchor(mode == RIG_SIT ? "the seat jumped" : "the avatar jumped (a server correction or a push)");
        }
        else if (mode == RIG_STAND && sVR.followCmd.absorb &&
                 std::chrono::duration<F32>(now - sVR.followAt).count() < 0.25f)
        {
            vc::absorb(sVR.map, moved);   // the avatar walking after you: the view stays
            absorbed = vc::length(moved);
        }
        // A vehicle turning turns the view with it (about the seat, as before #1607).
        if (mode == RIG_SIT && sVR.mode == RIG_SIT)
            sVR.map.turn = vc::wrapAngle(sVR.map.turn + yawDiff(avatar_rig_yaw, sVR.prevRigYaw));
    }
    // The re-anchor's blink: committed on its dark frame, or after 0.6 s without one (no blink
    // runs: the wall is up, the input is not polled).
    if (sVR.reanchorPending && (sVR.blinkDarkNow || std::chrono::duration<F32>(now - sVR.reanchorSince).count() > 0.6f))
        commitReanchor(sVR.blinkDarkNow ? "in the blink" : "without a blink");
    sVR.mode = mode;
    sVR.wasTeleporting = teleporting;
    sVR.prevRigYaw = avatar_rig_yaw;
    sVR.prevOriginGlobal = origin_global;
    sVR.haveOrigin = true;

    // ---- #1607 snap / smooth turns: about the head.
    const F32 view_turn = sVR.turnRequest;
    if (view_turn != 0.f) vc::turnAboutHead(sVR.map, sVR.head, view_turn);
    sVR.turnRequest = 0.f;

    // The head's room-scale offset from the origin; walking off past 1.5 m (into a wall the
    // avatar cannot pass, or faster than it catches up) re-anchors behind the blink.
    sVR.headOff = vc::headOffset(sVR.map, sVR.head);
    if (mode == RIG_STAND && !teleporting && !sVR.reanchorPending && vc::length(sVR.headOff) > 1.5f)
        requestReanchor("your head is 1.5 m from the avatar");

    // ---- The camera: the head through the mapping, up from the avatar's eyes.
    const vc::V2 eye_xy = vc::toAgent(sVR.map, sVR.head);
    const LLVector3 eye(eye_xy.x, eye_xy.y, anchor.mV[VZ] + (head_height - sVR.refHeight) * k);
    const LLVector3 at = rotZ(head_at, sVR.map.turn);
    const LLVector3 up = rotZ(head_up, sVR.map.turn);
    sVR.headAt = at;   // #1121
    sVR.headUp = up;

    LLViewerCamera* cam = LLViewerCamera::getInstance();
    if (!sVR.saved)
    {
        sVR.saved = true;
        sVR.savedNear = cam->getNear();
        LL_INFOS("VR") << "head-tracked camera on: " << hp.renderWidth << "x" << hp.renderHeight << ", vertical FOV "
                       << 2.f * std::atan(hp.tanHalfY) * RAD_TO_DEG << " deg, aspect " << hp.aspect << ", near "
                       << llclamp((F32)s_near_clip, 0.01f, 0.5f) << " m" << LL_ENDL;
    }
    // The frustum before the location (updateCameraLocation derives the cull FOV terms from
    // it); aspect before the FOV (the FOV clamp depends on it). The VR frustum is never sent
    // to the simulator as the agent's FOV (setViewNoBroadcast), and the speed-FoV boost is a
    // desktop effect.
    cam->setSpeedFovBoost(0.f);
    cam->setAspect(hp.aspect);
    cam->setViewNoBroadcast(2.f * std::atan(hp.tanHalfY));
    cam->setNear(llclamp((F32)s_near_clip, 0.01f, 0.5f));
    cam->setViewHeightInPixels((S32)hp.renderHeight);
    LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);
    // The true up (with the head's roll): left = up x at and up = at x left rebuild exactly
    // these axes.
    cam->updateCameraLocation(eye, up, eye + at);

    sActive = true;
    xr.setWorldCameraApplied(true);

    // #1076: the mapping for the controllers, then the hands at this frame's grip poses.
    sVR.mapValid = true;
    sVR.mapAnchor = LLVector3(origin.x, origin.y, anchor.mV[VZ]);
    sVR.mapTurn = sVR.map.turn;
    sVR.mapRefX = sVR.map.ref.x;
    sVR.mapRefY = sVR.map.ref.y;
    sVR.mapHeightAdd = hp.floorSpace ? 0.f : eye_height;
    sVR.mapScale = k;
    sVR.drawValid = mode == RIG_STAND;

    // #1607 diagnostic (the draw offset as selfDrawOffset gives it for the root's yaw now).
    {
        const vc::V2 draw = vc::drawOffset(sVR.headOff, v2(sVR.anchorLocal), body_yaw);
        rigStabilityTick(dt, mode == RIG_STAND ? vc::length(sVR.headOff) : 0.f, mode == RIG_STAND ? vc::length(draw) : 0.f,
                         absorbed, view_turn, avatar_rig_yaw, mode == RIG_STAND && sVR.follow.active);
    }

    // #1078 rig motion: where the room's origin maps (it moves with walking, flying, a vehicle,
    // the fly cam -- not with your head, a turn about it or the avatar catching up) and the
    // turn's rate since the last frame. A jump (teleport, sit / stand, a re-anchor: > 2 m in one
    // frame) or a snap turn (>= 12 degrees at once) is not motion you watch: it counts as none.
    {
        const vc::V2 room0 = vc::toAgent(sVR.map, vc::V2());
        const LLVector3d room_origin = gAgent.getPosGlobalFromAgent(LLVector3(room0.x, room0.y, anchor.mV[VZ]));
        if (dt > 1e-4f && dt < 0.25f)
        {
            const F32 moved_view = (F32)(room_origin - sVR.prevRoomOrigin).magVec();
            const F32 turned = std::fabs(yawDiff(sVR.map.turn, sVR.prevTurn));
            sVR.rigSpeed = moved_view < 2.f ? moved_view / dt : 0.f;
            sVR.rigTurnRate = turned < 12.f * DEG_TO_RAD ? turned / dt : 0.f;
        }
        else
        {
            sVR.rigSpeed = sVR.rigTurnRate = 0.f;
        }
        sVR.motionValid = true;
        sVR.prevRoomOrigin = room_origin;
        sVR.prevTurn = sVR.map.turn;
        sVR.prevAt = now;
    }
    LLVRHands::update();
}

bool LLVRCamera::firstPerson()
{
    return sActive && !sVR.flyApplied;
}

bool LLVRCamera::flyCam()
{
    return LLXrSession::active() && sVR.fly;
}

void LLVRCamera::setFlyCam(bool on)
{
    if (!LLXrSession::active()) return;
    sVR.fly = on;   // composed by the next apply()
}

void LLVRCamera::flyMove(const LLVector3& delta)
{
    if (!LLXrSession::active() || !sVR.fly || !sVR.flyApplied || !isAgentAvatarValid()) return;
    LLVector3 p = sVR.flyPos + delta;
    const LLVector3 avatar = gAgentAvatarp->getRenderPosition();
    const F32 max_dist = llmax(1.f, gAgentCamera.getCameraMaxZoomDistance());
    LLVector3 off = p - avatar;
    if (off.magVec() > max_dist)
    {
        off.normVec();
        p = avatar + off * max_dist;
    }
    sVR.flyPos = p;
}

void LLVRCamera::turnView(F32 radians)
{
    if (!LLXrSession::active() || !std::isfinite(radians)) return;
    sVR.turnRequest += radians;   // (about the head, by this frame's apply())
    // The head's facing turns now: bodyFollowHead and the walk turn, later this frame, compare
    // it with the avatar's (turned below) -- last frame's facing would read the turn as a head
    // turn past the limit.
    sVR.headAt = rotZ(sVR.headAt, radians);
    sVR.headUp = rotZ(sVR.headUp, radians);
    // Standing on the avatar: it turns with you, sent as a normal rotation (the view never reads
    // it back). As a key / joystick turn, the sim and the AO play the turn animations.
    if (sActive && !sVR.flyApplied && rigFollowsAgent() && !gAgent.getAutoPilot())
    {
        gAgent.rotate(radians, LLVector3::z_axis);
        gAgent.setControlFlags(radians > 0.f ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG);
    }
}

F32 LLVRCamera::headYaw()
{
    // (this frame's turns included: turnView)
    return sActive && sVR.mapValid ? yawOf(sVR.headAt) : yawOf(LLViewerCamera::getInstance()->getAtAxis());
}

bool LLVRCamera::rigFollowsAgent()
{
    return isAgentAvatarValid() && !gAgentAvatarp->isSitting();
}

bool LLVRCamera::headAngles(const LLQuaternion& frame, F32& yaw, F32& pitch, F32& roll)
{
    if (!sActive || sVR.flyApplied || !sVR.mapValid) return false;
    const LLQuaternion inv = ~frame;
    LLVector3 at = sVR.headAt * inv;
    LLVector3 up = sVR.headUp * inv;
    if (at.normVec() < 1e-3f || up.normVec() < 1e-3f) return false;
    const LLVector3 left = up % at;
    yaw = std::atan2(at.mV[VY], at.mV[VX]);
    pitch = std::atan2(at.mV[VZ], std::sqrt(at.mV[VX] * at.mV[VX] + at.mV[VY] * at.mV[VY]));
    // Roll: the head's left axis against the level one (frame up x at, and at x that) about the
    // look axis.
    LLVector3 level_left = LLVector3::z_axis % at;
    roll = 0.f;
    if (level_left.normVec() > 1e-3f)
    {
        const LLVector3 level_up = at % level_left;
        roll = std::atan2(left * level_up, left * level_left);
    }
    return std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(roll);
}

void LLVRCamera::bodyFollowHead(F32 dt, bool walking)
{
    // Past START the body starts turning and keeps on until it faces the head within STOP
    // (a wide hysteresis: small head turns never move the body, and a turn, once started,
    // does not stop half way and restart). The rate eases with the remaining angle (TAU),
    // between MIN_RATE and MAX_RATE.
    // #1584: START is SlipstreamVRBodyTurnLimit; this turn is the only thing that turns the body
    // on a head turn (the body layer's hips face the avatar: LLVRBodySource::setBodyYaw).
    // #1607: the view does not depend on the body's rotation; the body pivots under the head
    // (selfDrawOffset).
    static LLCachedControl<F32> s_turn_limit(gSavedSettings, "SlipstreamVRBodyTurnLimit", 65.f);
    const F32 START = llclamp((F32)s_turn_limit, 30.f, 150.f) * DEG_TO_RAD;
    constexpr F32 STOP = 4.f * DEG_TO_RAD;
    constexpr F32 TAU = 0.25f;
    constexpr F32 MIN_RATE = 30.f * DEG_TO_RAD;
    constexpr F32 MAX_RATE = 150.f * DEG_TO_RAD;
    constexpr F32 MIN_HORIZONTAL = 0.3f;   // looking nearly straight up / down: no yaw to follow

    if (!sActive || sVR.flyApplied || !sVR.mapValid || walking || dt <= 0.f || !rigFollowsAgent() || gAgent.getAutoPilot())
    {
        sVR.bodyTurning = false;
        return;
    }
    const LLVector3& at = sVR.headAt;
    if (std::sqrt(at.mV[VX] * at.mV[VX] + at.mV[VY] * at.mV[VY]) < MIN_HORIZONTAL) return;
    const LLVector3 body = gAgent.getAtAxis();
    const F32 rel = yawDiff(yawOf(at), yawOf(body));
    if (!sVR.bodyTurning)
    {
        if (std::fabs(rel) <= START) return;
        sVR.bodyTurning = true;
        // Once per turn (the next one needs the head past START again): the in-headset test line.
        LL_INFOS("VR") << llformat("body turns to follow the head (%.0f deg from the body)", rel * RAD_TO_DEG) << LL_ENDL;
    }
    if (std::fabs(rel) < STOP)
    {
        sVR.bodyTurning = false;
        LL_INFOS("VR") << "body turn done: facing the head" << LL_ENDL;
        return;
    }
    const F32 eased = std::fabs(rel) * (1.f - std::exp(-dt / TAU));
    const F32 mag = llmin(std::fabs(rel), llclamp(eased, MIN_RATE * dt, MAX_RATE * dt));
    const F32 step = rel > 0.f ? mag : -mag;
    gAgent.rotate(step, LLVector3::z_axis);
    // As a key / joystick / autopilot turn: the sim and the AO play the turn animations (the
    // flag lasts this frame's agent update).
    gAgent.setControlFlags(step > 0.f ? AGENT_CONTROL_YAW_POS : AGENT_CONTROL_YAW_NEG);
}

void LLVRCamera::followRoom(F32 dt, bool allowed)
{
    // Standing on the avatar, no autopilot, nothing pending: the avatar walks after your head.
    const bool ok = allowed && sActive && sVR.mapValid && sVR.mode == RIG_STAND && !sVR.flyApplied &&
                    !sVR.reanchorPending && rigFollowsAgent() && !gAgent.getAutoPilot() &&
                    gAgent.getTeleportState() == LLAgent::TELEPORT_NONE;
    const bool was = sVR.follow.active;
    sVR.followCmd = vc::stepFollow(sVR.follow, sVR.headOff, yawOf(gAgent.getAtAxis()), ok, dt);
    sVR.followAt = std::chrono::steady_clock::now();
    if (sVR.follow.active != was)
        LL_INFOS("VR") << llformat(sVR.follow.active ? "the avatar walks after you (head %.2f m from it)"
                                                     : "the avatar caught up (head %.2f m from it)",
                                   vc::length(sVR.headOff))
                       << LL_ENDL;
    const vc::FollowCommand& c = sVR.followCmd;
    if (c.at > 0) gAgent.setControlFlags(c.nudge ? AGENT_CONTROL_NUDGE_AT_POS : AGENT_CONTROL_AT_POS);
    else if (c.at < 0) gAgent.setControlFlags(c.nudge ? AGENT_CONTROL_NUDGE_AT_NEG : AGENT_CONTROL_AT_NEG);
    if (c.left > 0) gAgent.setControlFlags(c.nudge ? AGENT_CONTROL_NUDGE_LEFT_POS : AGENT_CONTROL_LEFT_POS);
    else if (c.left < 0) gAgent.setControlFlags(c.nudge ? AGENT_CONTROL_NUDGE_LEFT_NEG : AGENT_CONTROL_LEFT_NEG);
}

bool LLVRCamera::selfDrawOffset(const LLQuaternion& rootRot, LLVector3& offset)
{
    if (!sActive || !sVR.mapValid || !sVR.drawValid || !sVR.anchorValid || !rigFollowsAgent()) return false;
    const vc::V2 d = vc::drawOffset(sVR.headOff, v2(sVR.anchorLocal), yawOf(LLVector3::x_axis * rootRot));
    if (!vc::finite(d) || vc::length(d) > 3.f) return false;   // (never far from the collision shape)
    offset.set(d.x, d.y, 0.f);
    return true;
}

bool LLVRCamera::wantsBlink()
{
    return sActive && sVR.reanchorPending;
}

void LLVRCamera::blinkDark()
{
    if (sVR.reanchorPending) sVR.blinkDarkNow = true;   // (this frame's apply() commits)
}

bool LLVRCamera::appToAgent(const float p[3], LLVector3& out)
{
    if (!sVR.mapValid) return false;
    const LLVector3 s = xrToSL(p[0], p[1], p[2]);
    const F32 k = sVR.mapScale;   // #1078 world scale (1 = real size)
    const LLVector3 off = rotZ(LLVector3(s.mV[VX] - sVR.mapRefX, s.mV[VY] - sVR.mapRefY, 0.f), sVR.mapTurn) * k;
    out = sVR.mapAnchor + LLVector3(off.mV[VX], off.mV[VY], (s.mV[VZ] + sVR.mapHeightAdd - sVR.refHeight) * k);
    return true;
}

void LLVRCamera::rigMotion(F32& speed, F32& turnRate)
{
    const bool on = sActive && sVR.mapValid && sVR.motionValid;
    speed = on ? sVR.rigSpeed : 0.f;
    turnRate = on ? sVR.rigTurnRate : 0.f;
}

LLVector3 LLVRCamera::appDirToAgent(const float d[3])
{
    return rotZ(xrToSL(d[0], d[1], d[2]), sVR.mapValid ? sVR.mapTurn : 0.f);
}

LLVector3 LLVRCamera::appAxisToAgent(const float q[4], F32 x, F32 y, F32 z)
{
    return rotZ(headDirSL(q, x, y, z), sVR.mapValid ? sVR.mapTurn : 0.f);
}

bool LLVRCamera::eyeView(U32 eye, LLVector3& pos, LLVector3& at, LLVector3& left, LLVector3& up, F32 tan[4], bool display)
{
    if (!sActive || !sVR.mapValid || eye > 1u) return false;
    LLXrHeadPose hp;
    if (!LLXrSession::instance().headPose(hp) || !hp.eyesValid) return false;
    // #1594: the camera's eyes, or this frame's display-time eyes (the same unless the frame renders ahead)
    const float* ep = display ? hp.dispEyePos[eye] : hp.eyePos[eye];
    const float* eo = display ? hp.dispEyeOri[eye] : hp.eyeOri[eye];
    if (!appToAgent(ep, pos)) return false;
    // The eye's own axes (OpenXR -Z forward, -X left, +Y up), turned onto the rig like the head's.
    at = appAxisToAgent(eo, 0.f, 0.f, -1.f);
    left = appAxisToAgent(eo, -1.f, 0.f, 0.f);
    up = appAxisToAgent(eo, 0.f, 1.f, 0.f);
    const F32* fv = display ? hp.dispEyeFov[eye] : hp.eyeFov[eye];
    tan[0] = std::tan(fv[0]);   // left (negative)
    tan[1] = std::tan(fv[1]);   // right
    tan[2] = std::tan(fv[3]);   // down (negative)
    tan[3] = std::tan(fv[2]);   // up
    return std::isfinite(tan[0] + tan[1] + tan[2] + tan[3]) && tan[1] > tan[0] && tan[3] > tan[2];
}

bool LLVRCamera::referenceHeight(F32& h)
{
    if (!sVR.haveRef) return false;
    h = sVR.refHeight;
    return true;
}

void LLVRCamera::setReferenceHeight(F32 h)
{
    if (!(h >= 0.5f && h <= 2.5f)) return;
    sVR.refHeight = h;
    sVR.heightRecentred = true;   // (the eye height setting no longer drives it, as after a recentre)
    LL_INFOS("VR") << llformat("reference height %.2f m (the body's measured eye height)", h) << LL_ENDL;
}

bool LLVRCamera::rigMapping(F32& turn, LLVector3& anchor, F32& refX, F32& refY)
{
    if (!sActive || !sVR.mapValid) return false;
    turn = sVR.mapTurn;
    anchor = sVR.mapAnchor;
    refX = sVR.mapRefX;
    refY = sVR.mapRefY;
    return true;
}

bool LLVRCamera::handPose(U32 hand, bool aim, LLVector3& pos, LLQuaternion& rot)
{
    if (!sActive || !sVR.mapValid || hand >= LLXR_HAND_COUNT) return false;
    LLXrInputState in;
    if (!LLXrSession::instance().inputState(in)) return false;
    const LLXrPose& p = aim ? in.hand[hand].aim : in.hand[hand].grip;
    if (!in.hand[hand].active || !p.valid || !appToAgent(p.position, pos)) return false;
    // The pose's own axes in SL terms: OpenXR -Z (forward) = at, -X = left, +Y = up.
    LLMatrix3 m;
    m.setRows(appAxisToAgent(p.orientation, 0.f, 0.f, -1.f), appAxisToAgent(p.orientation, -1.f, 0.f, 0.f),
              appAxisToAgent(p.orientation, 0.f, 1.f, 0.f));
    rot = m.quaternion();
    rot.normalize();
    return true;
}
