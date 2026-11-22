/**
 * @file llvrbodyheadset.cpp
 * @brief VR headset body tracking (#1583). See llvrbodyheadset.h; the mapping is
 *        llvrbodyheadsetmath.h, the OpenXR side LLXrSession (requestBodyTracker / bodyJoints).
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodyheadset.h"

#include "llformat.h"
#include "llviewercontrol.h"
#include "llvrbodyheadsetmath.h"

#include <chrono>
#include <string>

namespace fb = slfbt;

namespace
{
using Clock = std::chrono::steady_clock;

constexpr F32 COUNT_LOG_INTERVAL = 60.f;   // the valid joint counts, at most this often
constexpr F32 GAP_HINT = 2.f;              // body back after a longer gap: the recalibrate hint
constexpr F32 ACTIVE_LOG_INTERVAL = 5.f;   // body tracked / not tracked lines, at most this often
constexpr F32 IDLE_AFTER = 10.f;           // Auto: no body this long releases the tracker
constexpr F32 RECHECK_EVERY = 30.f;        // ... then one re-check this often
constexpr F32 PROBE_FOR = 2.f;              // ... a re-check's tracker looks this long for a body

// The tracker role slot each body role is reported in (the LLVRBodyFrame index).
constexpr uint32_t kRoleSlot[fb::B_COUNT] = {LLXR_TRK_WAIST,     LLXR_TRK_CHEST,     LLXR_TRK_LEFT_FOOT,
                                             LLXR_TRK_RIGHT_FOOT, LLXR_TRK_LEFT_KNEE, LLXR_TRK_RIGHT_KNEE};

struct HeadsetState
{
    bool had = false;            // a tracker existed last frame
    uint32_t generation = 0;     // LLXrBodyJoints::generation seen
    int set = fb::JS_NONE;
    bool active = false;         // the runtime tracked a body last frame
    Clock::time_point inactiveSince{};
    uint32_t fidelity = 0;       // the last fidelity status logged
    Clock::time_point lastCountLog{};
    Clock::time_point lastActiveLog{};
    // Auto's idle back-off: no body for a while releases the tracker (idle), a re-check later.
    F32 noBodyFor = 0.f;         // seconds the tracker has existed without a body
    bool idle = false;           // released: nothing runs until idleLeft is spent
    F32 idleLeft = 0.f;
    bool probing = false;        // a re-check's tracker: quiet, kept only if a body shows
    bool idleLogged = false;     // the release was logged (once until a body is found again)
    fb::RolePose held[fb::B_COUNT];
    F32 heldAge[fb::B_COUNT] = {};
};
HeadsetState sH;

int jointSetOf(const LLXrBodyJoints& b)
{
    switch (b.api)
    {
    case LLXR_BODY_FB: return b.fullBody ? fb::JS_FB_FULL : fb::JS_FB_UPPER;
    case LLXR_BODY_HTC: return fb::JS_HTC;
    case LLXR_BODY_BD: return fb::JS_BD;
    default: return fb::JS_NONE;
    }
}

const char* fidelityName(uint32_t f) { return f == 2 ? "high (inside-out legs)" : (f == 1 ? "low (generated legs)" : "n/a"); }

void clearHolds()
{
    for (int b = 0; b < fb::B_COUNT; ++b)
    {
        sH.held[b] = fb::RolePose{};
        sH.heldAge[b] = 0.f;
    }
}
} // namespace

bool LLVRBodyHeadset::offered()
{
    return LLXrSession::instance().bodyTrackingOffered();
}

bool LLVRBodyHeadset::poll(bool want, bool backoff, F32 dt, LLVRBodyFrame& out)
{
    LLXrSession& xr = LLXrSession::instance();
    if (!want)
    {
        if (!sH.had && !sH.idle) return false;   // (not selected, no tracker: the whole cost)
        if (sH.had)
        {
            xr.requestBodyTracker(false, false);
            LL_INFOS("VRBody") << "headset body tracking off" << LL_ENDL;
        }
        sH = HeadsetState{};
        return false;
    }
    if (!backoff) sH.idle = sH.probing = false;   // (selected explicitly: the tracker stays)
    if (sH.idle)
    {
        // Auto, no body for a while: no tracker, nothing located, until the next re-check.
        sH.idleLeft -= dt;
        if (sH.idleLeft > 0.f) return false;
        sH.idle = false;
        sH.probing = true;
        sH.noBodyFor = 0.f;
    }
    static LLCachedControl<U32> s_fidelity(gSavedSettings, "SlipstreamVRBodyFidelity", 1);
    const bool high = s_fidelity != 0;
    const bool exists = xr.requestBodyTracker(true, high, sH.probing);
    const LLXrBodyJoints* bj = exists ? xr.bodyJoints() : nullptr;
    if (!exists)
    {
        if (sH.had && !sH.probing) LL_INFOS("VRBody") << "headset body tracking off" << LL_ENDL;
        const bool idleLogged = sH.idleLogged;
        sH = HeadsetState{};
        sH.idleLogged = idleLogged;
        return false;
    }
    sH.had = true;
    if (!bj) return false;   // (the session is not running)
    const Clock::time_point now = Clock::now();
    if (bj->generation != sH.generation)
    {
        sH.generation = bj->generation;
        sH.set = jointSetOf(*bj);
        sH.active = false;
        sH.fidelity = 0;
        sH.inactiveSince = now;
        clearHolds();
        if (!sH.probing)
            LL_INFOS("VRBody") << "headset body tracking on: " << fb::jointSetName(sH.set) << ", fidelity asked "
                               << (high ? "high" : "low")
                               << (fb::jointSetHasLegs(sH.set) ? "" : "; this runtime gives no legs (they stay on the animation)")
                               << LL_ENDL;
    }
    if (bj->active)
    {
        sH.noBodyFor = 0.f;
        if (sH.probing)
        {
            sH.probing = false;
            sH.idleLogged = false;
            LL_INFOS("VRBody") << "re-check: a body is tracked, headset body tracking on: " << fb::jointSetName(sH.set)
                               << ", fidelity asked " << (high ? "high" : "low") << LL_ENDL;
        }
    }
    else if (backoff)
    {
        // Auto must not keep a tracker for someone who never turned body tracking on: after
        // IDLE_AFTER without a body (PROBE_FOR on a re-check) the tracker goes, and a re-check
        // every RECHECK_EVERY (one create, a few locates) looks again.
        sH.noBodyFor += dt;
        if (sH.noBodyFor >= (sH.probing ? PROBE_FOR : IDLE_AFTER))
        {
            const bool wasProbing = sH.probing;
            xr.requestBodyTracker(false, high, true);
            if (!sH.idleLogged)
                LL_INFOS("VRBody") << "no body tracked for " << (wasProbing ? PROBE_FOR : IDLE_AFTER)
                                   << " s (body tracking off in the headset?): the body tracker is released; Auto "
                                      "re-checks every "
                                   << RECHECK_EVERY << " s" << LL_ENDL;
            HeadsetState idle;
            idle.idle = true;
            idle.idleLeft = RECHECK_EVERY;
            idle.idleLogged = true;
            sH = idle;
            return false;
        }
    }
    if (bj->active != sH.active)
    {
        sH.active = bj->active;
        // (At most one tracked / not tracked line per ACTIVE_LOG_INTERVAL: a flickering body
        // must not flood the log.)
        const bool quiet = sH.lastActiveLog != Clock::time_point{} &&
                           std::chrono::duration<F32>(now - sH.lastActiveLog).count() < ACTIVE_LOG_INTERVAL;
        if (!quiet) sH.lastActiveLog = now;
        if (quiet)
        {
            if (!bj->active) sH.inactiveSince = now;
        }
        else if (bj->active)
        {
            const F32 gap = std::chrono::duration<F32>(now - sH.inactiveSince).count();
            LL_INFOS("VRBody") << "headset body tracked (confidence " << llformat("%.2f", bj->confidence) << ")"
                               << (gap > GAP_HINT ? llformat(" after %.0f s: if the body sits too high / low (the runtime may "
                                                             "reset your height, e.g. Virtual Desktop when the Quest menu "
                                                             "opens), wrist panel > Body while standing straight",
                                                             gap)
                                                  : std::string())
                               << LL_ENDL;
        }
        else
        {
            sH.inactiveSince = now;
            LL_INFOS("VRBody") << "headset body not tracked (body tracking off in the headset, or the runtime lost "
                                  "you): the body fades back to the animation"
                               << LL_ENDL;
        }
    }
    if (!bj->active)
    {
        clearHolds();
        return false;
    }
    if (bj->fidelity != sH.fidelity)
    {
        sH.fidelity = bj->fidelity;
        LL_INFOS("VRBody") << "headset body tracking fidelity: " << fidelityName(bj->fidelity)
                           << (high && bj->fidelity == 1 ? " -- high was asked: this headset / runtime gives generated legs" : "")
                           << LL_ENDL;
    }

    // The joints in body space (no floor offset: the mapping uses only differences, and the
    // roles go back out in the app space LLVRBody reads).
    fb::BodyJoint joints[fb::kMaxBodyJoints];
    const int n = (int)llmin(bj->count, (uint32_t)fb::kMaxBodyJoints);
    U32 valid = 0, tracked = 0;
    for (int i = 0; i < n; ++i)
    {
        const LLXrBodyJoint& j = bj->joint[i];
        joints[i].valid = j.valid;
        joints[i].tracked = j.tracked;
        joints[i].pos = fb::xrPosToBody(j.position, 0.f);
        valid += j.valid ? 1 : 0;
        tracked += j.tracked ? 1 : 0;
    }
    fb::RolePose roles[fb::B_COUNT];
    fb::mapBodyJoints(sH.set, joints, n, roles);

    // Legs generated (low fidelity): used, marked untracked, steadied more.
    const bool hasLegs = fb::jointSetHasLegs(sH.set);
    const bool legsGenerated = sH.set == fb::JS_FB_FULL && bj->fidelity != 2;
    const F32 conf = llclamp(bj->confidence, 0.f, 1.f);
    out = LLVRBodyFrame{};
    out.kind = LLVRBodySource::HEADSET;
    out.bonePoses = true;
    out.poses.generation = bj->generation;
    std::string roleList;
    for (int b = 0; b < fb::B_COUNT; ++b)
    {
        const bool leg = b >= fb::B_LFOOT;
        const fb::RolePose r = fb::holdRole(roles[b], sH.held[b], sH.heldAge[b], dt);
        LLXrTrackerPose& tp = out.poses.role[kRoleSlot[b]];
        tp.active = !leg || hasLegs;
        if (tp.active) ++out.poses.connected;
        out.confidence[kRoleSlot[b]] = leg && legsGenerated ? conf * 0.6f : conf;
        if (!r.valid) continue;
        tp.valid = true;
        tp.tracked = r.tracked && !(leg && legsGenerated);
        fb::bodyPosToXr(r.pos, tp.position);
        fb::bodyRotToXr(r.rot, tp.orientation);
        roleList += llformat("%s%s%s", roleList.empty() ? "" : ", ", fb::bodyName(b), tp.tracked ? "" : " (generated)");
    }
    // #1584: the elbow joints as the arms' poles (positions only; LLVRBodyArms fades them).
    for (int side = 0; side < 2; ++side)
    {
        const int ej = fb::elbowJoint(sH.set, side);
        if (ej < 0 || ej >= n || !joints[ej].valid || !fb::finite(joints[ej].pos)) continue;
        LLXrTrackerPose& tp = out.poses.role[side ? LLXR_TRK_RIGHT_ELBOW : LLXR_TRK_LEFT_ELBOW];
        tp.active = tp.valid = true;
        tp.tracked = joints[ej].tracked;
        fb::bodyPosToXr(joints[ej].pos, tp.position);
    }

    if (sH.lastCountLog == Clock::time_point{} ||
        std::chrono::duration<F32>(now - sH.lastCountLog).count() >= COUNT_LOG_INTERVAL)
    {
        sH.lastCountLog = now;
        LL_INFOS("VRBody") << "headset body: " << valid << " / " << n << " joints valid, " << tracked << " tracked; confidence "
                           << llformat("%.2f", conf) << ", fidelity " << fidelityName(bj->fidelity) << "; roles: "
                           << (roleList.empty() ? std::string("none") : roleList) << LL_ENDL;
    }
    return true;
}
