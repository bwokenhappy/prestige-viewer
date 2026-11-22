/**
 * @file llvrspacewarp.cpp
 * @brief #1594 VR space warp: the frame policy (engagement, cadence). See llvrspacewarp.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#include "linden_common.h"

#include "llvrspacewarp.h"

#include "llerror.h"
#include "../llviewercontrol.h"

#include <algorithm>
#include <chrono>

namespace
{
double nowSec()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point t0 = Clock::now();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}
const char* modeName(uint32_t m)
{
    return m == 0 ? "off" : m == 2 ? "always" : "auto";
}
} // namespace

LLVRSpaceWarp& LLVRSpaceWarp::instance()
{
    static LLVRSpaceWarp s;
    return s;
}

uint32_t LLVRSpaceWarp::debugView() const
{
    static LLCachedControl<U32> s_dbg(gSavedSettings, "RenderVKVRSpaceWarpDebug", 0);
    return std::min<uint32_t>(s_dbg, 2u);
}

void LLVRSpaceWarp::setEngaged(bool on, const char* why)
{
    if (on == mEngaged) return;
    const double t = nowSec();
    const double periodMs = (double)mNativeNs * 1e-6;
    if (on)
    {
        // Back on soon after an auto disengage (a load that sits near the line, or a runtime that
        // throttles a CPU-bound frame): wait longer before the next disengage -- no flapping.
        if (mOffAt >= 0.0 && t - mOffAt < 10.0)
        {
            mDisengageSec = std::min(mDisengageSec * 2.0, kDisengageMaxSec);
            LL_INFOS("VR") << "space warp: re-engaged " << t - mOffAt << " s after it went off; it now waits "
                           << mDisengageSec << " s of headroom before going off again" << LL_ENDL;
        }
        else
            mDisengageSec = kDisengageSec;
        LL_INFOS("VR") << "space warp ENGAGED (" << why << "): the headset was showing " << mShownHz << " Hz with the world rendered at "
                       << mRealHz << " Hz; world GPU " << mGpuMs << " ms against the native period " << periodMs << " ms ("
                       << (periodMs > 0.0 ? 1000.0 / periodMs : 0.0) << " Hz); from now the world renders on 1 of " << mRatio
                       << " frames (" << (periodMs > 0.0 ? 1000.0 / (periodMs * mRatio) : 0.0)
                       << " Hz) and every shown frame is reprojected to its own eye poses" << LL_ENDL;
        mEngStart = t;
        mEngShown = mEngReal = 0;
        mStatsStart = t;
        mStatMissed = mStatThrottled = 0;
        mStatReal = mStatWarp = mStatPrime = mStatFail = 0;
        mStatWarpGpuSum = mStatWorldSum = 0.0;
        mStatWarpGpuMax = mStatWorldMax = 0.f;
        mStatWarpGpuN = mStatWorldN = 0;
    }
    else
    {
        const double span = mEngStart >= 0.0 ? t - mEngStart : 0.0;
        LL_INFOS("VR") << "space warp off (" << why << "): world GPU " << mGpuMs << " ms against the native period " << periodMs
                       << " ms; it was on for " << span << " s, rendering " << (span > 0.0 ? mEngReal / span : 0.0)
                       << " Hz and showing " << (span > 0.0 ? mEngShown / span : 0.0) << " Hz" << LL_ENDL;
        mEngStart = -1.0;
        mStatsStart = -1.0;
    }
    mEngaged = on;
    mWarnedOverBudget = false;
    // A new engagement starts from a fresh real frame (Prime); off forgets the history.
    mHistoryValid = false;
    mSinceReal = 0;
    mOverSince = mUnderSince = -1.0;
}

LLVRSpaceWarp::Kind LLVRSpaceWarp::beginFrame(int64_t displayNs, int64_t nativePeriodNs, int64_t periodNs, bool throttled,
                                              uint32_t missedNative, bool stereo)
{
    mSeen = true;
    mThrottledNow = throttled;
    const Kind k = decide(displayNs, nativePeriodNs, periodNs, throttled, missedNative, stereo);
    account(k, nowSec());
    return k;
}

llvrsw::StatusIn LLVRSpaceWarp::status() const
{
    llvrsw::StatusIn s;
    static LLCachedControl<U32> s_mode(gSavedSettings, "SlipstreamVRSpaceWarp", 1);
    s.seen = mSeen;
    s.mode = std::min<uint32_t>(s_mode, 2u);
    s.stereo = mLastStereo == 1;
    s.timing = mNativeNs > 0;
    s.engaged = mEngaged;
    s.throttled = mThrottledNow;
    s.shownHz = mShownHz;
    s.realHz = mRealHz;
    s.gpuMs = mGpuMs;
    s.budgetMs = (float)((double)mNativeNs * 1e-6);
    return s;
}

// Once per frame: the measured rates (1 s windows), the engagement's totals and the minute's summary line.
void LLVRSpaceWarp::account(Kind k, double t)
{
    const bool real = k != Kind::Warp;   // (Off, Prime, Real render the world)
    ++mWinShown;
    if (real) ++mWinReal;
    if (mEngaged)
    {
        ++mEngShown;
        if (real) ++mEngReal;
    }
    if (mWinStart < 0.0) mWinStart = t;
    if (t - mWinStart >= 1.0)
    {
        const double span = t - mWinStart;
        mShownHz = (float)(mWinShown / span);
        mRealHz = (float)(mWinReal / span);
        mWinStart = t;
        mWinShown = mWinReal = 0;
    }
    if (mSummaryAt < 0.0) mSummaryAt = t;
    if (t - mSummaryAt >= kSummarySec)
    {
        mSummaryAt = t;
        if (mEngaged) logStats(t);
        else logIdle(t);
    }
}

void LLVRSpaceWarp::logIdle(double)
{
    LL_INFOS("VR") << "space warp (summary): " << statusText() << " Headset shown " << mShownHz << " Hz, world rendered " << mRealHz
                   << " Hz" << LL_ENDL;
}

LLVRSpaceWarp::Kind LLVRSpaceWarp::decide(int64_t displayNs, int64_t nativePeriodNs, int64_t periodNs, bool throttled,
                                          uint32_t missedNative, bool stereo)
{
    static LLCachedControl<U32> s_mode(gSavedSettings, "SlipstreamVRSpaceWarp", 1);
    static LLCachedControl<U32> s_ratio(gSavedSettings, "RenderVKVRSpaceWarpDebugRatio", 0);
    mKind = Kind::Off;
    mDisplayNs = displayNs;
    const uint32_t mode = std::min<uint32_t>(s_mode, 2u);
    const uint32_t dbg = s_ratio;
    if (mode != mMode)
    {
        LL_INFOS("VR") << "space warp setting: " << modeName(mMode) << " -> " << modeName(mode)
                       << (mode != 0 && !stereo ? " (no effect: this VR session is mono, space warp needs layered stereo)" : "") << LL_ENDL;
        mMode = mode;
    }
    if ((int)stereo != mLastStereo)
    {
        mLastStereo = stereo ? 1 : 0;
        if (stereo)
            LL_INFOS("VR") << "space warp: available (layered stereo session, setting " << modeName(mode) << ")" << LL_ENDL;
        else
            LL_WARNS("VR") << "space warp: NOT available -- this VR session is mono (layered stereo is off: SlipstreamVRStereo is read when "
                              "the viewer starts and is not saved), and space warp needs each eye's depth; the setting ("
                           << modeName(mode) << ") has no effect until the viewer is started with layered stereo" << LL_ENDL;
    }
    if (!stereo || nativePeriodNs <= 0)
    {
        setEngaged(false, "no stereo frame timing");
        return mKind;
    }
    mNativeNs = nativePeriodNs;
    const bool forced = dbg >= 2u;
    const uint32_t ratio = forced ? std::min<uint32_t>(dbg, 4u) : 2u;
    if (ratio != mRatio)
    {
        if (mEngaged)
            LL_INFOS("VR") << "space warp: 1 real frame in " << ratio << " now (was " << mRatio << ")" << LL_ENDL;
        mRatio = ratio;
    }
    const double t = nowSec();
    // (a single hitch makes SteamVR halve the rate at once: only a throttle that lasts engages)
    mThrottledFrames = throttled ? mThrottledFrames + 1u : 0u;
    if (mEngaged)
    {
        mStatMissed += missedNative;
        if (throttled) ++mStatThrottled;
    }
    if (forced)
        setEngaged(true, "forced by RenderVKVRSpaceWarpDebugRatio");
    else if (mode == 0)
        setEngaged(false, "turned off");
    else if (mode == 2)
        setEngaged(true, "set to always");
    else
    {
        // AUTO, with hysteresis (noteGpu keeps the over / under windows).
        if (!mEngaged && mThrottledFrames >= kThrottleFrames)
            setEngaged(true, "the runtime throttles the app to a lower rate");
        else if (!mEngaged && mOverSince >= 0.0 && t - mOverSince >= kEngageSec)
            setEngaged(true, "the world's GPU time is over 95% of the frame budget");
        else if (mEngaged && !throttled && mUnderSince >= 0.0 && t - mUnderSince >= mDisengageSec)
        {
            setEngaged(false, "the world's GPU time is under 75% of the frame budget");
            mOffAt = t;
        }
    }
    if (!mEngaged) return mKind;

    if (!mHistoryValid)
    {
        mKind = Kind::Prime;
        mSinceReal = 0;
        ++mStatPrime;
    }
    else if (throttled || mSinceReal + 1u >= mRatio)
    {
        // (the runtime still holds the app at a lower rate -- until it sees the frames arrive on
        // time: every waited frame renders the world, ahead by the period the runtime predicts, so
        // the real rate never drops below the runtime's)
        mKind = Kind::Real;
        mAheadNs = (throttled && periodNs > 0) ? periodNs : mNativeNs;
        mSinceReal = 0;
        ++mStatReal;
    }
    else
    {
        mKind = Kind::Warp;
        ++mSinceReal;
        ++mStatWarp;
    }
    return mKind;
}

void LLVRSpaceWarp::logStats(double t)
{
    const double span = std::max(1e-3, t - mStatsStart);
    const double periodMs = (double)mNativeNs * 1e-6;
    const uint32_t shown = mStatReal + mStatWarp + mStatPrime;
    LL_INFOS("VR") << "space warp (summary, " << span << " s, engaged, 1 real in " << mRatio << "): " << shown << " frames shown ("
                   << shown / span << " Hz), worlds rendered " << (mStatReal + mStatPrime) << " (" << mStatPrime
                   << " without a history), warp-only " << mStatWarp << ", warps refused " << mStatFail
                   << "; warp GPU avg " << (mStatWarpGpuN ? mStatWarpGpuSum / mStatWarpGpuN : -1.0) << " max "
                   << mStatWarpGpuMax << " ms; world GPU avg " << (mStatWorldN ? mStatWorldSum / mStatWorldN : -1.0)
                   << " max " << mStatWorldMax << " ms (display period " << periodMs << " ms); the runtime: native slots missed "
                   << mStatMissed << ", throttled frames " << mStatThrottled
                   << (mStatMissed * 20u > shown ? " -> the real frame is too slow even for space warp" : "") << LL_ENDL;
    mStatsStart = t;
    mStatMissed = mStatThrottled = 0;
    mStatReal = mStatWarp = mStatPrime = mStatFail = 0;
    mStatWarpGpuSum = mStatWorldSum = 0.0;
    mStatWarpGpuMax = mStatWorldMax = 0.f;
    mStatWarpGpuN = mStatWorldN = 0;
}

void LLVRSpaceWarp::noteGpu(float worldMs)
{
    if (!(worldMs > 0.f)) return;
    mGpuMs = worldMs;
    const double budget = (double)mNativeNs * 1e-6;
    if (!(budget > 0.0)) return;
    const double t = nowSec();
    if (worldMs > kEngageFraction * budget)
    {
        mUnderSince = -1.0;
        if (mOverSince < 0.0) mOverSince = t;
    }
    else if (worldMs < kDisengageFraction * budget)
    {
        mOverSince = -1.0;
        if (mUnderSince < 0.0) mUnderSince = t;
    }
    else
    {
        // Between the two: hold the state (no flapping), restart both windows.
        mOverSince = mUnderSince = -1.0;
    }
    if (mEngaged)
    {
        mStatWorldSum += worldMs;
        mStatWorldMax = std::max(mStatWorldMax, worldMs);
        ++mStatWorldN;
        // The warp after a real frame waits for it on the queue and must still reach its own slot:
        // the real frame has about one period plus the runtime's lead (running start) minus the
        // compositor's margin -- ~1.4 periods (ESTIMATE; the 10 s line's missed slots are the
        // runtime's own verdict). Past that, slots are missed even with space warp. Once per engagement.
        if (!mWarnedOverBudget && worldMs > kRealBudgetPeriods * budget)
        {
            mWarnedOverBudget = true;
            LL_WARNS("VR") << "space warp: the world needs " << worldMs << " ms on the GPU, more than ~" << kRealBudgetPeriods
                           << " display periods (" << budget * kRealBudgetPeriods
                           << " ms): frames may still be missed (see the space warp summary line)" << LL_ENDL;
        }
    }
}

void LLVRSpaceWarp::noteRecorded(int64_t renderNs)
{
    mPendingPrevNs = mLastRecordedNs;
    mPendingRenderNs = renderNs;
    mPrevRecordedNs = mLastRecordedNs;
    mLastRecordedNs = renderNs;
}

void LLVRSpaceWarp::noteCaptured(bool ok)
{
    if (!ok)
    {
        if (mHistoryValid) LL_INFOS("VR") << "space warp: the real frame could not be kept for warping" << LL_ENDL;
        mHistoryValid = false;
        return;
    }
    mHistoryValid = true;
    mHistRenderNs = mPendingRenderNs;
    mHistPrevRenderNs = mPendingPrevNs;
}

void LLVRSpaceWarp::dropHistory(const char* why)
{
    if (mHistoryValid) LL_INFOS("VR") << "space warp: history dropped (" << why << "): the next frame renders the world" << LL_ENDL;
    mHistoryValid = false;
}

void LLVRSpaceWarp::noteWarp(bool ok, float gpuMs)
{
    if (!ok)
    {
        ++mStatFail;
        return;
    }
    if (gpuMs > 0.f)
    {
        mStatWarpGpuSum += gpuMs;
        mStatWarpGpuMax = std::max(mStatWarpGpuMax, gpuMs);
        ++mStatWarpGpuN;
    }
}
