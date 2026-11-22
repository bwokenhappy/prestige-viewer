/**
 * @file llvrbodyshare.cpp
 * @brief #1584 VR body share. See llvrbodyshare.h; the decisions are llvrbodysharemath.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodyshare.h"

#include "fsposeranimator.h"
#include "llformat.h"
#include "lljoint.h"
#include "lljointstate.h"
#include "llviewercontrol.h"
#include "llvrbodysharemath.h"
#include "llvrcamera.h"
#include "slposestudio.h"

#include <chrono>
#include <unordered_map>

F32 LLVRBodyShare::sEase = 0.25f;

namespace
{
using Clock = std::chrono::steady_clock;
constexpr F32 REPORT_STALE = 1.f;      // the layer reported nothing this long: the stream ends
constexpr F32 STATS_INTERVAL = 60.f;
constexpr int MAX_BONES = slshare::kMaxBones;

struct ShareState
{
    // Joint -> Pose Studio's bone index (-1: not a shared bone); built as joints are seen.
    std::unordered_map<const LLJoint*, int> index;
    bool pelvisOf[MAX_BONES] = {};
    slshare::DeadbandParams deadband[MAX_BONES];   // per bone kind (slshare::boneClassOf)
    // This frame's report (the layer), and the one plan() reads.
    bool building[MAX_BONES] = {};
    bool driven[MAX_BONES] = {};
    LLJoint* joint[MAX_BONES] = {};
    int top = 0;                        // bones in use: indices < top
    bool setChanged = false;            // the driven set changed since the last plan
    Clock::time_point lastReport{};
    bool layerOn = false;
    // The stream.
    bool live = false;                  // active() last tick (the on / off log, takeEnded)
    bool pelvisLogged = false;          // the pelvis rest line, once per stream
    bool ended = false;
    slshare::BoneTrack tracks[MAX_BONES];
    slshare::RateState rate;
    Clock::time_point lastPlan{};
    // Stats.
    Clock::time_point statsStart{};
    U32 lines = 0, fulls = 0, bones = 0;
    size_t bytes = 0;
    int lastBones = 0;
    U32 trigByClass[slshare::BC_COUNT] = {};   // bones that asked for a line, per kind (the summary)
    int cls[MAX_BONES] = {};
};
ShareState sS;

slshare::Q toQ(const LLQuaternion& q) { return slshare::Q(q.mQ[VX], q.mQ[VY], q.mQ[VZ], q.mQ[VW]); }
LLQuaternion toLLQ(const slshare::Q& q) { return LLQuaternion(q.x, q.y, q.z, q.w); }

int poserIndexOf(const LLJoint* j)
{
    auto it = sS.index.find(j);
    if (it != sS.index.end()) return it->second;
    int idx = -1;
    if (SLPoseStudio::instanceExists())
    {
        const auto& joints = SLPoseStudio::instance().getAnimator().PoserJoints;
        for (size_t i = 0; i < joints.size() && i < (size_t)MAX_BONES; ++i)
        {
            if (joints[i].boneType() == COL_VOLUMES || joints[i].jointName() != j->getName()) continue;
            idx = (int)i;
            break;
        }
    }
    if (sS.index.size() < 512) sS.index[j] = idx;   // (a skeleton's joints: bounded)
    if (idx >= 0)
    {
        sS.pelvisOf[idx] = j->getName() == "mPelvis";
        sS.cls[idx] = slshare::boneClassOf(j->getName().c_str());
        sS.deadband[idx] = slshare::deadbandFor(sS.cls[idx]);
    }
    return idx;
}
} // namespace

bool LLVRBodyShare::wanted()
{
    static LLCachedControl<bool> s_share(gSavedSettings, "SlipstreamVRBodyShare", true);
    static LLCachedControl<bool> s_pose_share(gSavedSettings, "SLPoseSyncShare", true);
    return s_share && s_pose_share && LLVRCamera::active();
}

void LLVRBodyShare::beginLayer()
{
    for (int i = 0; i < sS.top; ++i) sS.building[i] = false;
}

void LLVRBodyShare::addState(const LLJointState* state)
{
    if (!state || state->getWeight() <= 0.01f || !state->getJoint()) return;
    const int idx = poserIndexOf(state->getJoint());
    if (idx < 0) return;
    if (idx >= sS.top)
    {
        for (int i = sS.top; i <= idx; ++i) sS.building[i] = sS.driven[i] = false;
        sS.top = idx + 1;
    }
    sS.building[idx] = true;
    sS.joint[idx] = const_cast<LLJoint*>(state->getJoint());
}

void LLVRBodyShare::endLayer()
{
    bool any = false;
    for (int i = 0; i < sS.top; ++i)
    {
        if (sS.building[i] != sS.driven[i]) sS.setChanged = true;
        sS.driven[i] = sS.building[i];
        any = any || sS.driven[i];
    }
    sS.layerOn = any;
    sS.lastReport = Clock::now();
}

void LLVRBodyShare::layerStopped()
{
    for (int i = 0; i < sS.top; ++i)
    {
        if (sS.driven[i]) sS.setChanged = true;
        sS.driven[i] = sS.building[i] = false;
    }
    sS.layerOn = false;
    sS.index.clear();   // (an avatar rebuilt later may reuse joint addresses)
}

bool LLVRBodyShare::active()
{
    if (!sS.layerOn && !sS.live) return false;   // (no VR body: the whole cost)
    const bool on = sS.layerOn && wanted() &&
                    std::chrono::duration<F32>(Clock::now() - sS.lastReport).count() < REPORT_STALE;
    if (on != sS.live)
    {
        sS.live = on;
        const Clock::time_point now = Clock::now();
        if (on)
        {
            for (slshare::BoneTrack& t : sS.tracks) t = slshare::BoneTrack{};
            sS.rate = slshare::RateState{};
            sS.lastPlan = now;
            sS.statsStart = now;
            sS.lines = sS.fulls = sS.bones = 0;
            sS.bytes = 0;
            sS.pelvisLogged = false;
            LL_INFOS("VRBody") << "body share on:your VR body goes out through Pose Studio sharing (Slipstream viewers with the helper)"
                               << LL_ENDL;
        }
        else
        {
            sS.ended = true;
            LL_INFOS("VRBody") << "body share off (" << sS.lines << " lines, " << sS.bytes << " bytes this time)" << LL_ENDL;
        }
    }
    return on;
}

int LLVRBodyShare::plan(bool fullAsked)
{
    static const slshare::RateParams kRate;
    const Clock::time_point now = Clock::now();
    const F32 dt = llclamp(std::chrono::duration<F32>(now - sS.lastPlan).count(), 0.f, 1.f);
    sS.lastPlan = now;
    if (fullAsked) sS.rate.fullWanted = true;
    slshare::Q rot[MAX_BONES];
    slshare::V3 pos[MAX_BONES];
    bool hasPos[MAX_BONES] = {};
    for (int i = 0; i < sS.top; ++i)
    {
        LLJoint* j = sS.driven[i] ? sS.joint[i] : nullptr;
        if (!j) continue;
        rot[i] = toQ(j->getRotation());
        hasPos[i] = sS.pelvisOf[i];
        if (hasPos[i])
        {
            // From the rest position the receivers' base is (a mesh's override, else the root).
            LLVector3 over;
            LLUUID mesh;
            const bool hasOver = j->hasAttachmentPosOverride(over, mesh);
            const LLVector3& drawn = j->getPosition();
            pos[i] = slshare::pelvisOffset(slshare::V3(drawn.mV[VX], drawn.mV[VY], drawn.mV[VZ]), hasOver,
                                           slshare::V3(over.mV[VX], over.mV[VY], over.mV[VZ]));
            if (!sS.pelvisLogged)
            {
                // (Once per stream: what the receivers' pelvis is placed from.)
                sS.pelvisLogged = true;
                const LLVector3& def = j->getDefaultPosition();
                LL_INFOS("VRBody") << "body share pelvis: rest "
                                   << (hasOver ? llformat("override (%.3f, %.3f, %.3f)", over.mV[VX], over.mV[VY], over.mV[VZ])
                                               : std::string("at the root (no override)"))
                                   << llformat("; drawn (%.3f, %.3f, %.3f), sent offset (%.3f, %.3f, %.3f); skeleton default (%.3f, %.3f, %.3f)",
                                               drawn.mV[VX], drawn.mV[VY], drawn.mV[VZ], pos[i].x, pos[i].y, pos[i].z, def.mV[VX],
                                               def.mV[VY], def.mV[VZ])
                                   << LL_ENDL;
            }
        }
    }
    const bool setChanged = sS.setChanged;
    const slshare::Plan p = slshare::plan(sS.tracks, rot, pos, hasPos, sS.driven, sS.top, dt, setChanged, sS.rate, kRate, sS.deadband);
    if (p.kind)
    {
        sS.setChanged = false;
        sEase = p.ease;
        sS.lastBones = p.bones;
        if (p.kind == 1)
            for (int k = 0; k < p.nTrig; ++k) ++sS.trigByClass[sS.cls[p.trig[k]]];
        // Debug: what asked for this line (explains the rate): each bone and how far past its
        // keep threshold it was (1 = at it; below 1: a settle).
        LL_DEBUGS("VRBody") << "body share line: " << (p.kind == 2 ? "keyframe" : (setChanged ? "delta (release / new bone)" : "delta"))
                            << ", " << p.bones << " bones, triggered by:";
        for (int k = 0; k < p.nTrig; ++k)
        {
            const int i = p.trig[k];
            LL_CONT << " " << (sS.joint[i] ? sS.joint[i]->getName() : std::string("?")) << llformat(" %.1fx", p.trigErr[k]);
        }
        LL_CONT << LL_ENDL;
    }
    // Stats now and then.
    const F32 span = std::chrono::duration<F32>(now - sS.statsStart).count();
    if (span >= STATS_INTERVAL)
    {
        const U32* tc = sS.trigByClass;
        LL_INFOS("VRBody") << llformat("body share: %.2f lines/s, %.0f bytes/s, %.1f bones per line, %u keyframes in the last %.0f s; "
                                       "delta triggers: limbs %u, spine %u, hips %u, head %u, fingers %u",
                                       sS.lines / span, sS.bytes / span, sS.lines ? (F32)sS.bones / sS.lines : 0.f, sS.fulls, span,
                                       tc[slshare::BC_LIMB], tc[slshare::BC_SPINE], tc[slshare::BC_PELVIS], tc[slshare::BC_HEAD],
                                       tc[slshare::BC_FINGER])
                           << LL_ENDL;
        sS.statsStart = now;
        sS.lines = sS.fulls = sS.bones = 0;
        sS.bytes = 0;
        for (U32& c : sS.trigByClass) c = 0;
    }
    return p.kind;
}

bool LLVRBodyShare::heldBone(U32 poserIndex, LLQuaternion& rot, LLVector3& pos)
{
    if ((int)poserIndex >= sS.top || !sS.driven[poserIndex] || !sS.tracks[poserIndex].sent) return false;
    const slshare::BoneTrack& t = sS.tracks[poserIndex];
    rot = toLLQ(t.rot);
    pos = sS.pelvisOf[poserIndex] ? LLVector3(t.pos.x, t.pos.y, t.pos.z) : LLVector3::zero;
    return true;
}

void LLVRBodyShare::noteSent(size_t bytes, bool full)
{
    ++sS.lines;
    sS.fulls += full ? 1 : 0;
    sS.bones += (U32)llmax(sS.lastBones, 0);
    sS.bytes += bytes;
}

bool LLVRBodyShare::takeEnded()
{
    const bool e = sS.ended;
    sS.ended = false;
    return e;
}

void LLVRBodyShare::requestFull()
{
    sS.rate.fullWanted = true;
}
