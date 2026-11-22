/**
 * @file llvrbodymapping.h
 * @brief #1592 VR body mapping: which avatar joint each body-tracking role drives, per role
 *        tracked / animation / off, weights per part, and the retargeting that lets the human
 *        solver drive a body built otherwise (the Open Pony on its hind legs, or its forelegs as
 *        reaching arms). The pure part, the built-in profiles and the Open Pony's bone usage
 *        (from its dev kit) are in llvrbodymapmath.h.
 *
 * Profiles: Human (the solver's own mapping), Open Pony Bipedal / Arms only / Head only, and
 * Custom (edited in the window, or imported). The choice (SlipstreamVRBodyMapChoice: "auto", a
 * built-in id, or "custom") and the custom profile's text (SlipstreamVRBodyMapCustom) are per
 * account, like SlipstreamVRWorldScale: each avatar keeps its own. Auto detects the profile from
 * the worn meshes' rigging every few seconds (the Open Pony's hooves on mHandRing1 / mFoot), and
 * from the skeleton's shape (a horizontal spine: a quadruped).
 *
 * Users: LLVRBodyMotion (llvrbody.cpp) and LLVRBodyArmsLayer (llvrbodyarms.cpp) look their joints
 * up by role, measure the skeleton through the frames (virtual offsets), convert their solved
 * locals (actualLocal), then finish() every joint state with its role's state and part weight.
 * LLVRBody::update calls update() each VR frame. The window: SLFloaterVRBodyMap
 * ("vr_body_mapping"). Log tag "VRBody": profile chosen / detected (with why), import / export.
 */
#pragma once

#include "llquaternion.h"
#include "v3math.h"
#include "llvrbodymapmath.h"

#include <string>

class LLCharacter;
class LLJoint;
class LLJointState;

class LLVRBodyMap
{
public:
    // The profile in use (Auto: the detected built-in).
    static const slbmap::Profile& profile();
    // Bumps on every change (states, weights, joints); jointGeneration only when joint names change
    // (the body layer's joint states are then made anew).
    static U32 generation();
    static U32 jointGeneration();
    // "auto", "human", "pony_bipedal", "pony_arms", "pony_head" or "custom" (saved per account).
    static std::string choice();
    static void choose(const std::string& choice);
    // An edit from the window: becomes the custom profile (saved), choice "custom".
    static void edit(const slbmap::Profile& p);
    // Auto's last detection (slbmap::Detected) and why.
    static int detected();
    static std::string detectReason();
    static void redetect();
    // LLVRBody::update, every VR frame: the per-account load, then a detection every 5 s.
    static void update(F32 dt);
    static bool exportFile(const std::string& path, std::string& err);
    static bool importFile(const std::string& path, std::string& err);

    // ---- For the body layer -------------------------------------------------------------------
    // The joint a role drives on this character (nullptr: disabled or missing).
    static LLJoint* jointFor(LLCharacter* ch, int role);
    // This profile's frames on the character's rest skeleton (joint overrides included). upright:
    // the profile's stance applies now (LLVRBody::upright()); false keeps the body as it rests.
    static void frames(LLCharacter* ch, slbmap::Frames& out, slbmap::RestRig* rig = nullptr, bool upright = true);
    // The role a joint belongs to: its own (own = true), else the nearest mapped ancestor's; a
    // Bento finger bone under a hand or hoof: that side's fingers (a finger bone mapped to a role
    // of its own -- the Open Pony's hoof on mHandRing1 -- is that role's). -1: none (above the
    // pelvis).
    static int roleOf(LLJoint* j, bool* own = nullptr);
    // The retarget frame of a joint (its role's; identity for none).
    static LLQuaternion frameOf(const slbmap::Frames& f, LLJoint* j);
    // The avatar's eye point from its head joint, in the head's rest frame (slbmap::eyeFromHead:
    // the Open Pony's pupils, else the eye joints, else the SL skeleton's eyes). The VR camera's
    // eye point and the body's head under the camera both use it. Re-read every 2 s.
    static LLVector3 eyeFromHead(LLCharacter* ch, LLJoint* head);
    // The role's state on one joint state after a layer's solve. base: the motion's pose weight.
    // ANIMATION: weight 0. OFF: the base pose. TRACKED: as solved -- on an upright profile blended
    // over the base pose by the solved weight (the stance never drops back to the animation).
    // The part's weight scales the result. own: the joint is the role's own (else it holds
    // straight under it). Not for a joint whose role is -1.
    static void finish(LLJointState* st, int role, bool own, const slbmap::Frames& f, F32 base);
};
