/**
 * @file llvrspacewarp.h
 * @brief #1594 (epic #1072) VR SPACE WARP: the frame policy -- when the world renders, when a
 *        frame is synthesised from the last real one, and when space warp engages at all.
 *
 * App-side space warp (extrapolation, never interpolation: no frame is held back). While it is
 * ENGAGED every frame the headset shows is the warp pass's output: the newest real frame
 * reprojected per eye (its depth, positionally) to that frame's own predicted eye poses, with
 * moving content carried forward by the motion vectors (LLVRSpaceWarpVK, clean_vr_spacewarp.comp).
 * The world itself renders on every N-th xrWaitFrame only (N = 2: 60 real frames on a 120 Hz
 * Index), the rest of the frames are warp only (no world recording, no UI build: the viewer's
 * renderFrame returns after the warp).
 *
 * WHY EVERY SHOWN FRAME IS A WARP (and not "real frame, warp, real frame"): there is ONE graphics
 * queue. A real frame that needs more than a display period cannot reach its own display slot,
 * and a warp frame submitted after it waits behind it on the queue. So the real frame is rendered
 * ONE PERIOD AHEAD -- its camera is located for the display time of the NEXT frame (render-ahead,
 * LLXrSession::locateViews) -- and every frame, the real ones too, submits a warp of the newest
 * real frame to its own eye poses. On a real frame that warp is submitted BEFORE the world (its
 * own command buffer and submit, then xrEndFrame), so it never waits for the world it precedes;
 * the warp frame after it reprojects that world (finished by then) by a near-identity correction
 * to its fresh poses. The real frame may therefore take more than a display period of GPU time:
 * about one period plus the runtime's lead (xrWaitFrame returns ahead of the slot) minus the
 * compositor's margin and the warp -- ~1.4 periods (ESTIMATE, ~12 ms at 120 Hz; the summary line's
 * missed native slots are the runtime's own verdict). A heavier world still misses slots.
 *
 * FRAME KINDS (beginFrame, once per xrWaitFrame, before the camera is located):
 *   Off    space warp is not engaged: today's path, bit for bit.
 *   Prime  engaged, but no real frame in the warp's history yet: render and show the world as
 *          today (camera at the display time) and capture it into the history.
 *   Real   engaged: warp the history to this frame's poses and submit it; then render the world
 *          for the NEXT display time (aheadNs) and capture it.
 *   Warp   engaged: warp the history to this frame's poses and submit it; nothing else.
 *
 * ENGAGEMENT (SlipstreamVRSpaceWarp: 0 off, 1 auto (default), 2 always):
 *   auto engages when the world's GPU time (the renderer's sampled world span, LLXrSession::
 *   noteGpuFrame) stays over kEngageFraction of the headset's native frame period for
 *   kEngageSec, or when the runtime throttles the app for kThrottleFrames waited frames in a row
 *   (it would show half rate with rotation-only reprojection); it disengages when that time stays under kDisengageFraction
 *   for kDisengageSec (doubled, up to kDisengageMaxSec, when it re-engages within 10 s of going off;
 *   never while the runtime still throttles). Every edge is logged with the timings ("VR" tag, "space warp"), and a
 *   summary line (once a minute) counts real / warped frames and the warp's own GPU time while engaged.
 *   RenderVKVRSpaceWarpDebugRatio N (2..4, not saved) forces it on with one real frame per N
 *   shown frames (the same extrapolation math), for testing on a GPU that holds the rate.
 *
 * OBSERVABLE (#1594): status() / statusText() / shortText() say in plain words whether warp is on, and the
 * measured rendered / shown rates (the VR settings menu, the wrist header, the status bar's frame rate box);
 * the log has a line at each engage / disengage (with the rates and why), a line when the session turns out
 * to be unable to warp (mono), and one summary a minute. The status bar's FPS counts frames the headset was
 * given (every waited frame runs the viewer's idle), so while warp is on it is the SHOWN rate; realHz() is
 * the rendered one.
 *
 * Desktop: nothing here is ever called (LLXrSession::waitFrame is VR only); kind() stays Off.
 * VR with the setting Off: beginFrame returns Off every frame and nothing else changes.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#pragma once

#include "llvrspacewarpstatus.h"

#include <cstdint>
#include <string>

class LLVRSpaceWarp
{
public:
    enum class Kind : uint8_t { Off, Prime, Real, Warp };

    static LLVRSpaceWarp& instance();

    // LLXrSession::waitFrame, after xrWaitFrame (main thread): this frame's kind. displayNs: the
    // frame's predicted display time; nativePeriodNs: the panel's period (0: unknown -> Off);
    // periodNs: the period the runtime predicts now; throttled: the runtime holds the app at a lower
    // rate (then every waited frame is a real one); stereo: the session renders layered stereo (space
    // warp needs per-eye depth: mono sessions stay Off).
    // missedNative: native display slots missed since the previous frame (the summary line while engaged).
    Kind beginFrame(int64_t displayNs, int64_t nativePeriodNs, int64_t periodNs, bool throttled, uint32_t missedNative,
                    bool stereo);
    // What space warp is doing now (plain words; measured rates over the last second).
    llvrsw::StatusIn status() const;
    std::string statusText() const { return llvrsw::statusText(status()); }
    std::string shortText() const { return llvrsw::shortText(status()); }
    float shownHz() const { return mShownHz; }
    float realHz() const { return mRealHz; }
    // A frame that was not waited (no session running): Off.
    void noFrame() { mKind = Kind::Off; }
    Kind kind() const { return mKind; }
    // The camera of a Real frame renders for this much after the frame's display time (one native
    // period; the runtime's period while it throttles); 0 otherwise.
    int64_t aheadNs() const { return mKind == Kind::Real ? mAheadNs : 0; }

    // The renderer's world GPU span of a real frame (ms; fresh samples only).
    void noteGpu(float worldMs);
    // The context recorded a VR world frame whose camera was located for renderNs (every VR world
    // frame, engaged or not: the source frame's motion-vector interval is the gap to the previous one).
    void noteRecorded(int64_t renderNs);
    // The latest real frame went into the warp history (ok) -- or could not (no inputs, out of memory).
    void noteCaptured(bool ok);
    // The history no longer matches the world (a cut, a teleport, a resize, the session restarted).
    void dropHistory(const char* why);
    // A warp was submitted (gpuMs: its measured GPU time when a timestamp came back, else < 0) or
    // could not be made this frame (the frame then fell back to rendering the world).
    void noteWarp(bool ok, float gpuMs);

    bool engaged() const { return mEngaged; }
    bool historyValid() const { return mHistoryValid; }
    // The source frame's timing for the extrapolation: its render (camera) time and the previous
    // recorded real frame's (the interval its motion vectors span).
    int64_t historyRenderNs() const { return mHistRenderNs; }
    int64_t historyPrevRenderNs() const { return mHistPrevRenderNs; }
    // RenderVKVRSpaceWarpDebug (0 off; 1 tint the hole-filled pixels; 2 also tint every warp-only frame).
    uint32_t debugView() const;
    // Shown frames per real frame while engaged (2, or the debug ratio).
    uint32_t ratio() const { return mRatio; }

    static constexpr double kSummarySec = 60.0;         // the summary line's period
    static constexpr float kEngageFraction = 0.95f;
    static constexpr float kDisengageFraction = 0.75f;
    static constexpr double kEngageSec = 0.25;
    static constexpr double kDisengageSec = 3.0;
    static constexpr double kDisengageMaxSec = 30.0;   // (backoff after a quick re-engage)
    static constexpr uint32_t kThrottleFrames = 10;     // waited frames the runtime must throttle in a row
    static constexpr double kRealBudgetPeriods = 1.4;  // ESTIMATE: what a real frame may take (see noteGpu)

private:
    LLVRSpaceWarp() = default;
    Kind decide(int64_t displayNs, int64_t nativePeriodNs, int64_t periodNs, bool throttled, uint32_t missedNative,
                bool stereo);
    void account(Kind k, double t);
    void setEngaged(bool on, const char* why);
    void logStats(double nowSec);
    void logIdle(double nowSec);

    Kind mKind = Kind::Off;
    bool mEngaged = false;
    bool mHistoryValid = false;
    int mLastStereo = -1;           // -1: not seen yet (the first frame logs what the session can do)
    bool mSeen = false;
    bool mThrottledNow = false;
    uint32_t mMode = 1;             // the setting last seen (edges logged)
    uint32_t mRatio = 2;
    uint32_t mSinceReal = 0;        // shown frames since the last real one
    int64_t mNativeNs = 0;
    int64_t mAheadNs = 0;           // this Real frame's render-ahead
    int64_t mDisplayNs = 0;         // this frame's
    int64_t mLastRecordedNs = 0;    // the last recorded VR world frame's render time
    int64_t mPrevRecordedNs = 0;    // ... and the one before it
    int64_t mHistRenderNs = 0, mHistPrevRenderNs = 0;
    int64_t mPendingRenderNs = 0, mPendingPrevNs = 0;   // the frame recorded this frame (captured -> history)
    // hysteresis: when the GPU time last crossed into over / under (seconds; < 0 = not there)
    double mOverSince = -1.0, mUnderSince = -1.0;
    double mDisengageSec = kDisengageSec;   // grows (to kDisengageMaxSec) when auto flaps
    double mOffAt = -1.0;                   // the last auto disengage
    float mGpuMs = -1.f;            // the last fresh world sample
    bool mWarnedOverBudget = false;
    // measured rates (1 s windows), the engagement's totals, the summary's clock
    double mWinStart = -1.0;
    uint32_t mWinShown = 0, mWinReal = 0;
    float mShownHz = 0.f, mRealHz = 0.f;
    double mEngStart = -1.0, mSummaryAt = -1.0;
    uint32_t mEngShown = 0, mEngReal = 0;
    // summary stats while engaged (every kSummarySec)
    double mStatsStart = -1.0;
    uint32_t mStatReal = 0, mStatWarp = 0, mStatPrime = 0, mStatFail = 0;
    uint32_t mStatMissed = 0, mStatThrottled = 0;
    uint32_t mThrottledFrames = 0;
    double mStatWarpGpuSum = 0.0;
    float mStatWarpGpuMax = 0.f;
    uint32_t mStatWarpGpuN = 0;
    double mStatWorldSum = 0.0;
    float mStatWorldMax = 0.f;
    uint32_t mStatWorldN = 0;
};
