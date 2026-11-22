/**
 * @file llvrcamera.h
 * @brief VR milestone M1 (#1074, epic #1072): the head-tracked viewer camera.
 *
 * While an OpenXR session runs (LLXrSession), the viewer camera is the headset: VR camera mode,
 * modelled on mouselook. The result goes into LLViewerCamera (origin, axes, the VR frustum, the
 * VR near plane), so audio, LOD, picking and name tags follow the head.
 *
 * #1607 the rig is locally owned (llvrcameramath.h): the room maps onto the avatar's collision
 * shape centre (standing; the seat's eye anchor when sitting, the fly cam's point in fly cam)
 * through a reference point and a turn that the camera owns -- never the avatar's rotation, so a
 * body turn never moves the view, and a snap / smooth turn rotates about your head. The eye
 * height is the avatar's eyes' (head hidden, as in mouselook). The local avatar is drawn offset
 * (selfDrawOffset) so its eyes stand under the headset; walking about the room, the avatar walks
 * after you (followRoom, normal movement) while the view holds still. A jump the view did not
 * ask for (a server correction, a sit / stand) is taken behind the comfort blink.
 *
 * Desktop (no VR chosen at startup): apply() returns on one static read, active() is false.
 */
#pragma once

#include "stdtypes.h"
#include "v3math.h"
#include "llquaternion.h"

class LLVRCamera
{
public:
    // The head-tracked camera drove this frame's viewer camera. False on the desktop.
    static bool active() { return sActive; }
    // A VR session is running (menu items). False on the desktop.
    static bool sessionRunning();

    // idle(), after the desktop camera was computed (updateCamera / flycam / camera pilot):
    // compose this frame's head pose onto the avatar rig and write it into LLViewerCamera.
    // Restores the desktop camera parameters when the session stops. No-op without VR.
    static void apply();

    // Re-zero yaw and position (including the height) on the current head pose: what you
    // face becomes the avatar's facing, where your head is becomes the avatar's eyes.
    static void recenter();

    // ---- #1076 (M3): app (OpenXR) space -> agent space, with THIS frame's rig mapping (the
    // one the camera used). Valid after apply() put the head pose into the camera; false /
    // unrotated otherwise. App space: OpenXR axes, metres; agent space: SL region metres.
    static bool appToAgent(const float p[3], LLVector3& out);   // a point
    static LLVector3 appDirToAgent(const float d[3]);           // a direction
    // The axis (x, y, z) of an app-space orientation (quaternion x y z w), in agent space.
    static LLVector3 appAxisToAgent(const float q[4], F32 x, F32 y, F32 z);
    // A controller's pose in agent space (hand: LLXR_HAND_LEFT / RIGHT; aim: the pointing ray
    // pose, else the grip / hand pose). rot maps SL local axes (X at = the pose's forward,
    // Y left, Z up) into agent space. False: no session, the hand is not tracked, or the
    // head-tracked camera did not run this frame. Main thread. This is the hook for Pose
    // Studio (avatar hands from the controllers, later from body tracking).
    static bool handPose(U32 hand, bool aim, LLVector3& pos, LLQuaternion& rot);
    // #1565 (body trackers): this frame's rig mapping: the turn (app space -> agent space,
    // radians about Z), the anchor (agent space: the collision shape's centre at the eyes'
    // height, standing) the reference (refX, refY: SL axes, app space; #1607 owned by the
    // camera, moved by turns and the avatar's catch-up) maps onto. False: none this frame.
    static bool rigMapping(F32& turn, LLVector3& anchor, F32& refX, F32& refY);
    // #1592 your standing eye height the view maps onto the avatar's eyes (the last height
    // recentre, else SlipstreamVREyeHeight); false: no reference yet. The body scales your height
    // by the same value, so the avatar's eyes stay at the camera. setReferenceHeight: the body's
    // Body button measured it (a height between 0.5 and 2.5 m), as a height recentre would.
    static bool referenceHeight(F32& h);
    static void setReferenceHeight(F32 h);
    // #1075 LAYERED STEREO: eye `eye`'s view (0 = left, 1 = right) for THIS frame's camera, in
    // agent space -- its located position, axes (at, left, up; SL frame) and FOV tangents
    // (tanLeft < 0 < tanRight, tanDown < 0 < tanUp) -- with the rig mapping apply() used, so the
    // two eyes sit around the camera origin (the cyclopean head). False: no session, the
    // head-tracked camera did not run this frame, or the runtime located no eyes.
    // #1594 display: the eye at THIS frame's display time instead of the camera's (they differ only
    // on a space warp real frame, whose camera renders one display period ahead): the warp's target.
    static bool eyeView(U32 eye, LLVector3& pos, LLVector3& at, LLVector3& left, LLVector3& up, F32 tan[4],
                        bool display = false);

    // ---- #1076 locomotion (LLVRInput, before idle()) ----
    // Turn the view by `radians` about the up axis (+ = left): snap / smooth turn, about your
    // head (#1607: taken by this frame's apply()). Standing, the avatar turns with you (sent as
    // a normal rotation); sitting, only the view turns.
    static void turnView(F32 radians);
    // The head's facing in agent space (yaw about Z, radians), from this frame's camera.
    static F32 headYaw();
    // The rig follows the avatar's yaw (standing); false while sitting (the seat's rotation).
    static bool rigFollowsAgent();

    // ---- #1121 the avatar's head follows the headset ----
    // This frame's head orientation expressed in `frame`'s axes (a rotation in agent space:
    // the avatar's root joint for the head drive, LLQuaternion::DEFAULT for agent axes): yaw
    // about up (+ = left), pitch (+ = up), roll about the look axis (+ = the left ear up),
    // radians. False unless the head-tracked camera drove this frame from the avatar
    // (firstPerson(); not in fly cam, not on the desktop).
    static bool headAngles(const LLQuaternion& frame, F32& yaw, F32& pitch, F32& roll);
    // LLVRInput locomotion, every frame: once the head has turned past the neck's comfortable
    // range from the standing body (SlipstreamVRBodyTurnLimit), the body (the agent's rotation,
    // sent to the sim) turns smoothly until it faces where you look; the view never depends on
    // it (#1607). Never while sitting, walking with the stick (that already turns the body to
    // the head), in fly cam or under autopilot.
    static void bodyFollowHead(F32 dt, bool walking);

    // ---- #1607 the avatar follows the headset ----
    // LLVRInput, every frame after the sticks: standing, once your head is past 0.35 m from the
    // avatar's collision shape (walking about the room), the avatar walks after you with the
    // normal movement controls until it is under your head again; the view does not move while
    // it catches up. allowed: false while the stick walks, the wall freezes movement, a blink
    // runs or a script holds the controls.
    static void followRoom(F32 dt, bool allowed);
    // updateRootPositionAndRotation, self, standing: the offset (agent space, horizontal) to add
    // to the avatar root's placement so the avatar's eyes stand under the headset, for the
    // root's rotation `rootRot`. False (no offset): no head-tracked camera on the avatar, sitting,
    // fly cam, the desktop.
    static bool selfDrawOffset(const LLQuaternion& rootRot, LLVector3& offset);
    // A re-anchor (the view jumps onto the avatar) waits for the comfort blink: LLVRInput starts
    // one, and calls blinkDark() on its fully black frame (this frame's apply() takes it).
    static bool wantsBlink();
    static void blinkDark();

    // ---- #1076 fly cam: the view leaves the body and flies like desktop camming ----
    // The head-tracked camera drove this frame from the avatar's head (as mouselook: own head /
    // avatar hidden). False in fly cam and on the desktop.
    static bool firstPerson();
    static bool flyCam();
    // On: the rig detaches at the current eye position and facing (no jump). Off: back on the
    // avatar at once, facing where you faced -- LLVRInput blinks the view to black around it
    // (never a fly-back: a rush through space is a motion-sickness trigger).
    static void setFlyCam(bool on);
    // Move the detached rig (agent space metres), kept within the desktop camera's distance
    // limit from the avatar (getCameraMaxZoomDistance; the interest list stays on the avatar).
    static void flyMove(const LLVector3& delta);

    // ---- #1078 comfort ----
    // This frame's view motion that is not your head's own: the rig's speed (m/s: walking,
    // flying, a vehicle, the fly cam) and its turn rate (rad/s: smooth turning; a snap turn
    // and a teleport / sit jump count as none). 0 without a head-tracked frame.
    static void rigMotion(F32& speed, F32& turnRate);

private:
    static bool sActive;
};
