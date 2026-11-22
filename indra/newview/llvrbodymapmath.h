/**
 * @file llvrbodymapmath.h
 * @brief #1592 VR body mapping: the pure part (no viewer types). Which SL joint each of the body
 *        solver's logical roles drives, per role whether tracking drives it, and how a body that
 *        is not built like a human (the Open Pony) is turned into one the human solver can drive.
 *        Shared by LLVRBodyMap (llvrbodymapping.h) and the fixtures in
 *        scripts/check/test_vr_bodymap.cpp (checks/vr_bodymap.py, host compiler only).
 *
 * Conventions as llvrbodymath.h: SL axes (X forward, Y left, Z up), metres, Hamilton quaternions
 * (qmul(a, b): b first). The SL rest pose has every local rotation identity.
 *
 * Roles: pelvis, spine (mTorso), chest, neck, head; per side collar, upper arm, forearm, hand,
 * fingers (the Bento finger set: on / off only), thigh, shin, foot, and the hoof (the bone past the
 * hand that ends a quadruped's foreleg; none on a human). Each maps to a joint name or
 * none. Each role is TRACKED (tracking drives it), ANIMATION (the layer leaves it to the
 * animation) or OFF (held in the profile's base pose: no tracking, no animation). Weights are
 * per part (hips, spine, head and neck, each arm, each hand's fingers, each leg).
 *
 * Retargeting ("virtual skeleton"): the solver (llvrbodymath.h / llvrbodyarmmath.h) assumes a
 * human rest: upright spine, T-pose arms along +-Y, legs hanging. A profile gives each role a
 * frame R (rest-relative rotation) so the solver sees a human-like skeleton:
 *   virtual offset of a joint in its parent's virtual frame = R_parent * actual offset
 *   actual world rotation = virtual world rotation * R_joint
 *   actual local = conj(R_parent) * virtual local * R_joint
 * Forward kinematics of the actual skeleton with the converted locals lands every joint exactly
 * where the virtual skeleton's FK puts it (a fixture checks it). Joints between roles (Bento
 * spine, the hoof under the hand) take the frame of their nearest mapped ancestor.
 *
 * Frames (computeFrames, from the avatar's own rest offsets, joint overrides included):
 *   - Arms whose rest bones do not point along +-Y (a quadruped's forelegs hang down) get
 *     R = the rotation taking the bone onto +-Y, so they reach as T-pose arms; the hand's bone
 *     is the one to its hoof (the cannon) when a hoof is mapped. Such a limb's collar keeps the
 *     chest's frame (it stays on the body, no collar turn) and its hoof the hand's (the hoof
 *     straight on the cannon is the hand straight on the forearm). A human's arms (within 30 degrees of +-Y) keep identity.
 *   - An upright profile (the Open Pony standing on its hind legs) pitches the spine: R_spine =
 *     the pitch about Y taking the rest pelvis -> neck line onto +Z. The neck / head and legs
 *     keep identity: in the upright stance the head faces forward on top of the barrel and the
 *     hind legs hang under it as in the rest pose.
 * Base pose (OFF, and an upright profile's stance where tracking has nothing): the spine
 * upright, the head forward, the arms hanging (a retargeted foreleg as it rests), the legs
 * straight (virtual world rotations), so
 * the actual locals are: pelvis = P, neck = conj(P), upper arms = conj(P), hips = conj(P) (P the
 * pitch), everything else identity (the collars ride with the barrel) -- for the Open Pony a pony
 * standing on its hind legs, the forelegs hanging as arms.
 *
 * The Open Pony (doc/open_pony_rig.md: dev kit V3.2, "[OP] OpenPony Mesh and Rigging.blend", body
 * mesh Body.012, read with scripts/tools/blend_rig_dump.py and its vertex groups): the barrel deforms on PELVIS /
 * BUTT (mPelvis), mTorso, BELLY (mTorso), CHEST (mChest), NECK (mNeck), HEAD (mHead) -- the
 * classic spine through its collision volumes; mSpine1-4 are zero-length and carry no weight.
 * Forelegs: L_CLAVICLE (mCollar: the scapula), L_UPPER_ARM (mShoulder), L_LOWER_ARM (mElbow),
 * L_HAND (mWrist: the cannon, the horse's "knee" being its wrist), and the front hoof on mHandRing1
 * (no other finger bone is weighted). Hind legs: L_UPPER_LEG
 * (mHip), L_LOWER_LEG (mKnee), L_FOOT (mAnkle: the cannon), the hind hoof on mFoot. mHindLimb*,
 * mToe*, mWing*, the other fingers: unweighted. The rig's spine is horizontal (joint positions
 * uploaded with the mesh): pelvis -> chest runs forward, the neck rises from the chest's front.
 *
 * Text format (export / import, and the per-avatar setting): lines "key value...", '#' comments.
 *   slipstream-body-map 1
 *   profile <id>             human | pony_bipedal | pony_arms | pony_head (the built-in it starts from)
 *   role <role> <joint|-> <tracked|animation|off>
 *   weight <part> <0..1>
 *   upright 0|1, reach_rebase 0|1, neck_lean 0|1
 */
#pragma once

#include "llvrbodymath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace slbmap
{
using slfbt::Q;
using slfbt::V3;

enum Role : int
{
    R_PELVIS = 0, R_SPINE, R_CHEST, R_NECK, R_HEAD,
    R_L_COLLAR, R_L_UPPER_ARM, R_L_FOREARM, R_L_HAND, R_L_FINGERS,
    R_R_COLLAR, R_R_UPPER_ARM, R_R_FOREARM, R_R_HAND, R_R_FINGERS,
    R_L_THIGH, R_L_SHIN, R_L_FOOT,
    R_R_THIGH, R_R_SHIN, R_R_FOOT,
    R_L_HOOF, R_R_HOOF,   // the bone past the hand that ends the limb (a hoof); a human has none
    R_COUNT
};
enum Part : int
{
    P_HIPS = 0, P_SPINE, P_HEAD, P_L_ARM, P_R_ARM, P_L_FINGERS, P_R_FINGERS, P_L_LEG, P_R_LEG, P_COUNT
};
enum State : int
{
    S_TRACKED = 0, S_ANIMATION, S_OFF, S_COUNT
};

struct RoleInfo
{
    const char* key;        // the text format's name
    const char* label;      // the window's
    int part;
    int parent;             // the solver's chain parent role (-1: the avatar's root)
    const char* human;      // the SL joint of the human profile
};
inline const RoleInfo& roleInfo(int r)
{
    static const RoleInfo k[R_COUNT] = {
        {"pelvis", "Pelvis", P_HIPS, -1, "mPelvis"},
        {"spine", "Spine", P_SPINE, R_PELVIS, "mTorso"},
        {"chest", "Chest", P_SPINE, R_SPINE, "mChest"},
        {"neck", "Neck", P_HEAD, R_CHEST, "mNeck"},
        {"head", "Head", P_HEAD, R_NECK, "mHead"},
        {"l_collar", "Left collar", P_L_ARM, R_CHEST, "mCollarLeft"},
        {"l_upper_arm", "Left upper arm", P_L_ARM, R_L_COLLAR, "mShoulderLeft"},
        {"l_forearm", "Left forearm", P_L_ARM, R_L_UPPER_ARM, "mElbowLeft"},
        {"l_hand", "Left hand", P_L_ARM, R_L_FOREARM, "mWristLeft"},
        {"l_fingers", "Left fingers", P_L_FINGERS, R_L_HAND, "fingers"},
        {"r_collar", "Right collar", P_R_ARM, R_CHEST, "mCollarRight"},
        {"r_upper_arm", "Right upper arm", P_R_ARM, R_R_COLLAR, "mShoulderRight"},
        {"r_forearm", "Right forearm", P_R_ARM, R_R_UPPER_ARM, "mElbowRight"},
        {"r_hand", "Right hand", P_R_ARM, R_R_FOREARM, "mWristRight"},
        {"r_fingers", "Right fingers", P_R_FINGERS, R_R_HAND, "fingers"},
        {"l_thigh", "Left thigh", P_L_LEG, R_PELVIS, "mHipLeft"},
        {"l_shin", "Left shin", P_L_LEG, R_L_THIGH, "mKneeLeft"},
        {"l_foot", "Left foot", P_L_LEG, R_L_SHIN, "mAnkleLeft"},
        {"r_thigh", "Right thigh", P_R_LEG, R_PELVIS, "mHipRight"},
        {"r_shin", "Right shin", P_R_LEG, R_R_THIGH, "mKneeRight"},
        {"r_foot", "Right foot", P_R_LEG, R_R_SHIN, "mAnkleRight"},
        {"l_hoof", "Left hoof", P_L_ARM, R_L_HAND, ""},
        {"r_hoof", "Right hoof", P_R_ARM, R_R_HAND, ""},
    };
    return k[r < 0 || r >= R_COUNT ? 0 : r];
}
inline bool isFingers(int r) { return r == R_L_FINGERS || r == R_R_FINGERS; }
inline bool isHoof(int r) { return r == R_L_HOOF || r == R_R_HOOF; }
inline int hoofOf(int side) { return side ? R_R_HOOF : R_L_HOOF; }
// The arm roles (collar .. hand) of a side: 0 left, 1 right; -1 not an arm role.
inline int armSide(int r)
{
    if (r >= R_L_COLLAR && r <= R_L_HAND) return 0;
    if (r >= R_R_COLLAR && r <= R_R_HAND) return 1;
    return -1;
}

inline const char* partKey(int p)
{
    static const char* const k[P_COUNT] = {"hips", "spine", "head", "l_arm", "r_arm", "l_fingers", "r_fingers", "l_leg", "r_leg"};
    return k[p < 0 || p >= P_COUNT ? 0 : p];
}
inline const char* partLabel(int p)
{
    static const char* const k[P_COUNT] = {"Hips", "Spine", "Head and neck", "Left arm", "Right arm", "Left fingers", "Right fingers",
                                           "Left leg", "Right leg"};
    return k[p < 0 || p >= P_COUNT ? 0 : p];
}
inline const char* stateKey(int s)
{
    static const char* const k[S_COUNT] = {"tracked", "animation", "off"};
    return k[s < 0 || s >= S_COUNT ? 0 : s];
}

// ---- Profiles ------------------------------------------------------------------------------
struct Profile
{
    std::string id = "human";      // the built-in it is (or started from)
    bool custom = false;           // edited (saved as text)
    std::string joint[R_COUNT];    // "" = no joint (the role is disabled); fingers: "fingers"
    int state[R_COUNT] = {};
    float weight[P_COUNT] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
    bool upright = false;          // stand a horizontal body on its hind legs (the spine pitched up)
    bool reachRebase = false;      // the hands reach from the avatar's own shoulders (a quadruped's)
    bool neckLean = true;          // the neck leans toward the headset's position (a human head is there)

    bool enabled(int r) const { return r >= 0 && r < R_COUNT && !joint[r].empty(); }
    // The role drives its joint at all (not left to the animation, a joint assigned).
    bool drives(int r) const { return enabled(r) && state[r] != S_ANIMATION; }
    bool tracked(int r) const { return enabled(r) && state[r] == S_TRACKED; }
};

inline bool sameMapping(const Profile& a, const Profile& b)
{
    if (a.upright != b.upright || a.reachRebase != b.reachRebase || a.neckLean != b.neckLean) return false;
    for (int r = 0; r < R_COUNT; ++r)
        if (a.joint[r] != b.joint[r] || a.state[r] != b.state[r]) return false;
    for (int p = 0; p < P_COUNT; ++p)
        if (std::fabs(a.weight[p] - b.weight[p]) > 1e-4f) return false;
    return true;
}
// The joint names (only) differ: the body layer's joint states must be made anew.
inline bool sameJoints(const Profile& a, const Profile& b)
{
    for (int r = 0; r < R_COUNT; ++r)
        if (a.joint[r] != b.joint[r]) return false;
    return true;
}

inline Profile humanProfile()
{
    Profile p;
    p.id = "human";
    for (int r = 0; r < R_COUNT; ++r)
    {
        p.joint[r] = roleInfo(r).human;
        p.state[r] = S_TRACKED;
    }
    return p;
}

// The Open Pony's joints (doc/open_pony_rig.md): the classic chain for the forelegs and hind
// legs. The foreleg is an arm bone for bone: mShoulder the upper arm, mElbow the forearm, mWrist
// the cannon (the horse's "knee" is its wrist; the cannon is its hand), and the hoof on
// mHandRing1 (a finger). The hoof role gives mHandRing1 the controller's pose (your hand at the
// wrist: slfbt::solveHoofArm); finger tracking never bends it. The hind hoof (mFoot) hangs
// under the foot role and keeps the animation's pose.
inline Profile ponyBase()
{
    Profile p = humanProfile();
    p.joint[R_L_FINGERS] = p.joint[R_R_FINGERS] = "fingers";
    p.state[R_L_FINGERS] = p.state[R_R_FINGERS] = S_ANIMATION;
    p.joint[R_L_HOOF] = "mHandRing1Left";
    p.joint[R_R_HOOF] = "mHandRing1Right";
    p.reachRebase = true;   // (a foreleg reaches from the pony's shoulder, scaled to its length)
    return p;
}
// Bipedal: stands on its hind legs like a human in VR. Your legs drive the hind legs, your arms
// (controllers) the forelegs as arms, the headset the head and neck, your torso the barrel.
inline Profile ponyBipedal()
{
    Profile p = ponyBase();
    p.id = "pony_bipedal";
    p.upright = true;
    return p;
}
// Arms only: a quadruped on its animations; the forelegs follow the controllers (reaching from
// the pony's own shoulders), the head and neck the headset. Nothing else is driven.
inline Profile ponyArms()
{
    Profile p = ponyBase();
    p.id = "pony_arms";
    p.neckLean = false;   // (the headset is not where a quadruped's head is)
    for (int r : {R_PELVIS, R_SPINE, R_CHEST, R_L_THIGH, R_L_SHIN, R_L_FOOT, R_R_THIGH, R_R_SHIN, R_R_FOOT}) p.state[r] = S_ANIMATION;
    return p;
}
// Head only: a quadruped on its animations; the head and neck follow the headset.
inline Profile ponyHead()
{
    Profile p = ponyArms();
    p.id = "pony_head";
    p.reachRebase = false;
    for (int r = 0; r < R_COUNT; ++r)
        if (r != R_NECK && r != R_HEAD) p.state[r] = S_ANIMATION;
    return p;
}
inline bool builtinProfile(const std::string& id, Profile& out)
{
    if (id == "human") out = humanProfile();
    else if (id == "pony_bipedal") out = ponyBipedal();
    else if (id == "pony_arms") out = ponyArms();
    else if (id == "pony_head") out = ponyHead();
    else return false;
    return true;
}
inline bool isPonyProfile(const Profile& p) { return p.id.compare(0, 5, "pony_") == 0; }
// The roles a standing body needs: an upright stance pitches the pelvis and spine and stands on
// the hind legs, so none of them can stay with a quadruped's animation.
inline bool standingRole(int r)
{
    return r == R_PELVIS || r == R_SPINE || r == R_CHEST || (r >= R_L_THIGH && r <= R_R_FOOT);
}
// A profile made consistent (every profile in use goes through this: built-in, edited, imported).
//   - A pony profile whose spine or chest is tracked stands up: your torso is upright, and a
//     horizontal barrel cannot follow it. (#1592: the owner set torso and chest to Tracked on
//     Arms only and the pony stayed on four legs: only Bipedal had the stance.)
//   - An upright profile drives the hips, spine and hind legs (tracked where the animation had
//     them): a quadruped's walk on a pitched-up pelvis points the hind legs forward.
inline void settle(Profile& p)
{
    // - A hoof on a joint another role already drives (an older custom map with the hand on
    //   mHandRing1) is no hoof: one joint, one role.
    for (int side = 0; side < 2; ++side)
    {
        const int h = hoofOf(side);
        for (int r = 0; r < R_COUNT && p.enabled(h); ++r)
            if (r != h && !isFingers(r) && p.joint[r] == p.joint[h]) p.joint[h].clear();
    }
    if (!p.upright && isPonyProfile(p) && (p.tracked(R_SPINE) || p.tracked(R_CHEST))) p.upright = true;
    if (p.upright)
        for (int r = 0; r < R_COUNT; ++r)
            if (standingRole(r) && p.enabled(r) && p.state[r] == S_ANIMATION) p.state[r] = S_TRACKED;
}
// Back on four legs (the window's "stands on its hind legs" unticked): the stance's roles go back
// to the animation.
inline void onFourLegs(Profile& p)
{
    p.upright = false;
    for (int r = 0; r < R_COUNT; ++r)
        if (standingRole(r)) p.state[r] = S_ANIMATION;
}

// ---- Detection -----------------------------------------------------------------------------
// What the worn rigged meshes weight (from their rigging tables) and the skeleton's rest shape.
struct RigFacts
{
    int riggedCount = 0;          // joints any worn mesh weights (0: nothing loaded yet)
    bool ring1[2] = {};           // mHandRing1Left / Right
    bool otherFingers[2] = {};    // any other mHand* finger bone of that side (Ring2/3, Index, ...)
    bool foot[2] = {};            // mFootLeft / Right
    bool hindLimbs = false;       // any mHindLimb* (Bento quadrupeds)
    V3 pelvisToNeck;              // the rest pelvis -> neck line (joint overrides included)
};
// A body whose spine runs more forward than up (a quadruped's barrel).
inline bool horizontalBody(const V3& pelvisToNeck)
{
    const float h = std::sqrt(pelvisToNeck.x * pelvisToNeck.x + pelvisToNeck.y * pelvisToNeck.y);
    return slfbt::length(pelvisToNeck) > 1e-3f && h > std::fabs(pelvisToNeck.z);
}
enum Detected : int
{
    D_HUMAN = 0, D_OPEN_PONY, D_QUADRUPED
};
// The Open Pony: its front hooves on mHandRing1 with no other finger weighted (a Bento human
// hand weights every finger), its hind hooves on mFoot. Another quadruped: a horizontal spine or
// Bento hind limbs. why: a short reason for the log and the window.
inline int detect(const RigFacts& f, std::string* why = nullptr)
{
    auto say = [&](const char* s) {
        if (why) *why = s;
    };
    const bool hooves = f.ring1[0] && f.ring1[1] && !f.otherFingers[0] && !f.otherFingers[1] && f.foot[0] && f.foot[1];
    if (f.riggedCount > 0 && hooves)
    {
        say("Open Pony rigging: front hooves on mHandRing1 (no other finger weighted), hind hooves on mFoot");
        return D_OPEN_PONY;
    }
    if (horizontalBody(f.pelvisToNeck))
    {
        say("a horizontal spine (a quadruped's joint positions)");
        return D_QUADRUPED;
    }
    if (f.hindLimbs)
    {
        say("Bento hind limbs weighted (a quadruped)");
        return D_QUADRUPED;
    }
    say(f.riggedCount > 0 ? "human rigging" : "no rigged mesh yet: human");
    return D_HUMAN;
}
// The built-in profile Auto picks: the Open Pony on its animations with arms and head tracked
// (the least surprising for a quadruped; Bipedal is one choice away), another quadruped the head
// only (its forelegs need not be on the arm bones), else human.
inline const char* autoProfileFor(int detected)
{
    return detected == D_OPEN_PONY ? "pony_arms" : (detected == D_QUADRUPED ? "pony_head" : "human");
}

// ---- Frames (retargeting) ------------------------------------------------------------------
// The avatar's rest offsets between consecutive role joints (each in the earlier joint's frame,
// summed through any joints in between; every rest rotation is identity, so these are also the
// root-frame vectors). have: the role's joint exists.
struct RestRig
{
    bool have[R_COUNT] = {};
    V3 off[R_COUNT];              // from the chain parent role's joint (roleInfo.parent) to this one
};
struct Frames
{
    Q R[R_COUNT];                 // virtual -> actual (actual world = virtual world * R)
    Q baseWorld[R_COUNT];         // the base pose, virtual world rotations (root frame)
    Q pitch;                      // the upright pitch (identity unless upright)
    bool upright = false;
    bool identity = true;         // every R identity (a human): the conversions can be skipped
    // Per side (0 left): the limb is retargeted onto a T-pose arm (a quadruped's foreleg), and
    // its collar then stays with the chest (no collar turn: a pony's scapula turned out to the
    // side is the "terribly distorted" shoulder).
    bool armRetarget[2] = {};
};
// The bone a role's frame is measured from: the role's own rest bone, i.e. the offset to its
// first mapped child along the chain (the hand: toward its hoof when one is mapped -- the Open
// Pony's cannon -- else on along the forearm; fingers, hoof: none).
inline bool boneDir(const RestRig& rig, int r, V3& dir)
{
    int child = -1;
    switch (r)
    {
    case R_L_COLLAR: child = R_L_UPPER_ARM; break;
    case R_L_UPPER_ARM: child = R_L_FOREARM; break;
    case R_L_FOREARM: child = R_L_HAND; break;
    case R_R_COLLAR: child = R_R_UPPER_ARM; break;
    case R_R_UPPER_ARM: child = R_R_FOREARM; break;
    case R_R_FOREARM: child = R_R_HAND; break;
    default: break;
    }
    if (r == R_L_HAND || r == R_R_HAND)
    {
        const int hoof = r == R_L_HAND ? R_L_HOOF : R_R_HOOF;
        child = rig.have[hoof] && slfbt::length(rig.off[hoof]) >= 1e-3f ? hoof : r;
    }
    if (child < 0 || !rig.have[child] || slfbt::length(rig.off[child]) < 1e-3f) return false;
    dir = slfbt::normalized(rig.off[child]);
    return true;
}
inline void computeFrames(const Profile& p, const RestRig& rig, Frames& out)
{
    out = Frames{};
    // Upright: the pitch about Y taking the rest pelvis -> neck line onto +Z.
    if (p.upright)
    {
        V3 v;
        for (int r : {R_SPINE, R_CHEST, R_NECK})
            if (rig.have[r]) v = v + rig.off[r];
        if (std::sqrt(v.x * v.x + v.z * v.z) > 1e-3f)
        {
            out.pitch = slfbt::qaxis(V3(0.f, 1.f, 0.f), std::atan2(-v.x, v.z));
            out.upright = true;
        }
    }
    for (int r : {R_PELVIS, R_SPINE, R_CHEST}) out.R[r] = out.pitch;
    // Arms: a limb whose upper arm rests more than 30 degrees off +-Y (a quadruped's foreleg
    // hangs down) reaches as a T-pose arm: its bones get R = the turn taking them onto +-Y with
    // the back kept back (the elbow still points back, the hoof's front is the thumb side). Its
    // collar keeps the chest's frame: it stays where it rests on the body. A human arm (along
    // +-Y) keeps identity throughout.
    for (int side = 0; side < 2; ++side)
    {
        const V3 out_(0.f, side ? -1.f : 1.f, 0.f);
        const int first = side ? R_R_COLLAR : R_L_COLLAR;
        V3 d;
        out.armRetarget[side] = boneDir(rig, first + 1, d) && slfbt::dot(d, out_) <= 0.866f;
        Q prev = out.armRetarget[side] ? out.R[R_CHEST] : Q();
        out.R[first] = prev;
        if (out.armRetarget[side])
        {
            // One frame for the upper arm and forearm: their rest shoulder -> wrist line onto +-Y,
            // the back kept back. (Each bone turned onto +-Y on its own straightens the slightly
            // bent rest leg in the virtual skeleton, and the solver then rolls both bones about
            // their axes -- in opposite ways -- to bend it again: a twisted foreleg.) The hand
            // (the cannon) gets its own, the same way.
            const V3 back(-1.f, 0.f, 0.f);
            const int fa = first + 2, hd = first + 3;
            V3 line = rig.have[fa] && rig.have[hd] ? rig.off[fa] + rig.off[hd] : (rig.have[fa] ? rig.off[fa] : out_);
            const Q limb = slfbt::qalignPair(slfbt::normalized(line, out_), back, out_, back);
            out.R[first + 1] = out.R[first + 2] = prev = limb;
            if (boneDir(rig, hd, d)) prev = slfbt::qalignPair(d, back, out_, back);
            out.R[hd] = prev;
        }
        else
        {
            for (int r = first + 1; r <= first + 3; ++r)
            {
                Q R = prev;   // (no bone of its own: the one above's)
                if (boneDir(rig, r, d)) R = slfbt::dot(d, out_) > 0.866f ? Q() : slfbt::qfromto(d, out_);
                out.R[r] = R;
                prev = R;
            }
        }
        out.R[first + 4] = prev;      // (fingers: the hand's)
        out.R[hoofOf(side)] = prev;   // (the hoof: straight on from the hand)
    }
    // Base pose (virtual world): spine up, head forward, legs hanging (identity); the arms
    // hanging down: the collar as it rests (a retargeted one: with the chest), the arm bones
    // along -Z.
    for (int side = 0; side < 2; ++side)
    {
        const int first = side ? R_R_COLLAR : R_L_COLLAR;
        const V3 out_(0.f, side ? -1.f : 1.f, 0.f);
        out.baseWorld[first] = out.armRetarget[side] ? Q() : slfbt::qconj(out.R[first]);
        // (A bone retargeted from its rest keeps that rest -- the pony's forelegs hang as they
        // rest; a human arm resting along +-Y is lowered.)
        const Q hang = slfbt::qfromto(out_, V3(0.f, 0.f, -1.f));
        for (int r = first + 1; r <= first + 4; ++r)
            out.baseWorld[r] = slfbt::qangle(Q(), out.R[r]) > 1e-4f ? slfbt::qconj(out.R[r]) : hang;
        out.baseWorld[hoofOf(side)] = out.baseWorld[first + 3];   // (straight under the hand)
    }
    for (int r = 0; r < R_COUNT; ++r)
        if (slfbt::qangle(Q(), out.R[r]) > 1e-4f) out.identity = false;
}

// The conversions (see the file comment).
inline V3 virtualOffset(const Q& rParent, const V3& actualOffset) { return slfbt::qrot(rParent, actualOffset); }
inline Q actualLocal(const Q& rParent, const Q& rJoint, const Q& virtualLocal)
{
    return slfbt::qnormalized(slfbt::qmul(slfbt::qmul(slfbt::qconj(rParent), virtualLocal), rJoint));
}
inline Q virtualWorld(const Q& actualWorld, const Q& rJoint) { return slfbt::qnormalized(slfbt::qmul(actualWorld, slfbt::qconj(rJoint))); }
// A role's base pose as an actual local rotation under its chain parent role.
inline Q baseLocal(const Frames& f, int r)
{
    const int pr = roleInfo(r).parent;
    const Q vParent = pr < 0 ? Q() : f.baseWorld[pr];
    const Q rParent = pr < 0 ? Q() : f.R[pr];
    const Q vLocal = slfbt::qmul(slfbt::qconj(vParent), f.baseWorld[r]);
    return actualLocal(rParent, f.R[r], vLocal);
}

// ---- The eyes (the VR camera's eye point on the avatar) ----------------------------------------
// The VR camera sits at the avatar's eyes, and the body puts the head joint under the camera by
// the same offset, so both use this one point: the eyes' midpoint from the head joint, in the
// head's rest frame (the avatar's metres). #1592 / #1584: it was a fixed (0.10, 0, 0.06) -- 2 cm
// under a human's eyes and 8 cm under the Open Pony's (0.12, 0, 0.14): the owner's view sat low on
// the neck and the chest showed at the bottom of a level view.
//   - The Open Pony (a pony profile): its eyes are where its pupils are -- flat pupils on
//     mFaceNoseLeft / Right (#1567), placed by the mesh's joint positions; un-overridden those
//     bones are a human's nose, not an eye, so only overridden ones count.
//   - Else the eye joints mEyeLeft / Right (a mesh's override, else the skeleton's).
//   - Else, or if the point is not plausibly an eye (ahead of the head joint, around its height,
//     on the middle), the SL skeleton's own eyes.
constexpr float kEyeForward = 0.098f;   // avatar_skeleton.xml: mEyeLeft / Right from mHead
constexpr float kEyeUp = 0.079f;
struct EyeJoints
{
    bool pupils = false;          // both pupil bones found, with joint position overrides
    V3 pupilMid;                  // their midpoint from the head joint
    bool eyes = false;            // both eye joints found
    V3 eyeMid;
};
enum EyeSource : int
{
    EYE_PUPILS = 0, EYE_JOINTS, EYE_DEFAULT
};
inline bool plausibleEye(const V3& v)
{
    return v.x > 0.02f && v.x < 0.4f && v.z > -0.05f && v.z < 0.4f && std::fabs(v.y) < 0.05f;
}
inline V3 eyeFromHead(bool pony, const EyeJoints& e, int* source = nullptr)
{
    int s = EYE_DEFAULT;
    V3 v(kEyeForward, 0.f, kEyeUp);
    if (pony && e.pupils && plausibleEye(e.pupilMid))
    {
        s = EYE_PUPILS;
        v = e.pupilMid;
    }
    else if (e.eyes && plausibleEye(e.eyeMid))
    {
        s = EYE_JOINTS;
        v = e.eyeMid;
    }
    if (source) *source = s;
    return V3(v.x, 0.f, v.z);   // (on the middle: the camera's eyes are the headset's)
}

// ---- Reach (a limb not built like your arm) --------------------------------------------------
// A pony's foreleg starts far below and behind its eyes, and its length is not your arm's. So the
// controller is taken relative to where your shoulder is (under your eye, in the body's heading)
// and scaled by the limb's reach over yours, from the avatar's own shoulder: your arm stretched
// out is the foreleg stretched out, your hand at your shoulder the hoof at the pony's.
// Positions in agent space (your body already scaled to the avatar by its eye height), eyeH the
// avatar's eye height, side +1 left / -1 right.
constexpr float kHumanShoulderBack = 0.06f;    // eye heights behind the eye
constexpr float kHumanShoulderOut = 0.11f;     // ... out to the side
constexpr float kHumanShoulderDown = 0.17f;    // ... below the eye
constexpr float kHumanReach = 0.40f;           // ... from the shoulder to the controller's grip, arm straight
inline V3 humanShoulder(const V3& eye, float yaw, float eyeH, float side)
{
    return eye + slfbt::qrot(slfbt::qyaw(yaw), V3(-kHumanShoulderBack, side * kHumanShoulderOut, -kHumanShoulderDown) * eyeH);
}
// The scale from your reach to the limb's (limbReach: shoulder joint -> the point the controller
// holds, along the rest bones), kept within 0.3 .. 2.5.
inline float reachScale(float limbReach, float eyeH)
{
    const float h = kHumanReach * eyeH;
    if (!(h > 1e-3f) || !(limbReach > 1e-3f)) return 1.f;
    const float k = limbReach / h;
    return k < 0.3f ? 0.3f : (k > 2.5f ? 2.5f : k);
}
inline V3 remapReach(const V3& target, const V3& yourShoulder, const V3& limbShoulder, float scale)
{
    return limbShoulder + (target - yourShoulder) * scale;
}

// ---- Text format ---------------------------------------------------------------------------
inline bool validJointName(const std::string& s)
{
    if (s.empty() || s.size() > 40) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
inline std::string toText(const Profile& p)
{
    std::string s = "slipstream-body-map 1\nprofile " + p.id + "\n";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "upright %d\nreach_rebase %d\nneck_lean %d\n", p.upright ? 1 : 0, p.reachRebase ? 1 : 0,
                  p.neckLean ? 1 : 0);
    s += buf;
    for (int r = 0; r < R_COUNT; ++r)
        s += std::string("role ") + roleInfo(r).key + " " + (p.joint[r].empty() ? std::string("-") : p.joint[r]) + " " +
             stateKey(p.state[r]) + "\n";
    for (int q = 0; q < P_COUNT; ++q)
    {
        std::snprintf(buf, sizeof(buf), "weight %s %.2f\n", partKey(q), p.weight[q]);
        s += buf;
    }
    return s;
}
// Reads the text (a built-in named by "profile" first, then the lines over it). Unknown keys,
// roles and malformed lines are skipped (counted in *skipped). False: not a body map.
inline bool fromText(const std::string& text, Profile& out, int* skipped = nullptr)
{
    int bad = 0;
    bool header = false;
    Profile p = humanProfile();
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        char a[48] = {}, b[48] = {}, c[48] = {}, d[16] = {};
        const int n = std::sscanf(line.c_str(), "%47s %47s %47s %15s", a, b, c, d);
        if (n <= 0) continue;
        const std::string k = a;
        if (k == "slipstream-body-map")
        {
            header = n >= 2 && std::atoi(b) >= 1;
            continue;
        }
        if (!header)
        {
            ++bad;
            continue;
        }
        if (k == "profile" && n >= 2)
        {
            Profile base;
            if (builtinProfile(b, base)) p = base;
            else ++bad;
        }
        else if ((k == "upright" || k == "reach_rebase" || k == "neck_lean") && n >= 2)
        {
            const bool v = std::atoi(b) != 0;
            (k == "upright" ? p.upright : (k == "reach_rebase" ? p.reachRebase : p.neckLean)) = v;
        }
        else if (k == "role" && n >= 4)
        {
            int r = -1, st = -1;
            for (int i = 0; i < R_COUNT; ++i)
                if (!std::strcmp(b, roleInfo(i).key)) r = i;
            for (int i = 0; i < S_COUNT; ++i)
                if (!std::strcmp(d, stateKey(i))) st = i;
            const std::string j = c;
            const bool jointOk = j == "-" || (isFingers(r) ? j == "fingers" : validJointName(j));
            if (r < 0 || st < 0 || !jointOk)
            {
                ++bad;
                continue;
            }
            p.joint[r] = j == "-" ? std::string() : j;
            p.state[r] = st;
        }
        else if (k == "weight" && n >= 3)
        {
            int q = -1;
            for (int i = 0; i < P_COUNT; ++i)
                if (!std::strcmp(b, partKey(i))) q = i;
            const float w = (float)std::atof(c);
            if (q < 0 || !std::isfinite(w))
            {
                ++bad;
                continue;
            }
            p.weight[q] = w < 0.f ? 0.f : (w > 1.f ? 1.f : w);
        }
        else ++bad;
    }
    if (skipped) *skipped = bad;
    if (!header) return false;
    p.custom = true;
    out = p;
    return true;
}
} // namespace slbmap
