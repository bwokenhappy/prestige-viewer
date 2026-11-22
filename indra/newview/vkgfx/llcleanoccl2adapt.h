/**
 * @file llcleanoccl2adapt.h
 * @brief Adaptive two-phase occlusion (RenderVKOcclusion2Phase = 2): the stand-down decision.
 *        Pure arithmetic (no Vulkan, no viewer state) so the regression fixture
 *        scripts/check/test_occl2_adapt.cpp drives exactly the code the viewer runs.
 *
 * WHY. Two-phase Hi-Z occlusion (clean_occl2.comp) costs a roughly fixed amount every frame --
 * the pre-pass split in two, the Hi-Z build from phase 1's depth, the test / compaction
 * dispatches -- and saves in proportion to what it culls. Inside buildings it wins big (owner's
 * RTX 5090, house facing the sim: 55 -> 71 FPS); in an open view it culls little and the fixed
 * part shows (same house facing off-sim: 84 -> 82-83 FPS; open outdoor scenes measured ~3 ms a
 * frame slower at 4K on a GTX 1080). So the adaptive mode stands down to the plain pre-pass
 * (no split, no Hi-Z, no test) while the culled share stays small, and probes now and then.
 *
 * THE MEASURE is the culled share of the tested draws, (culled + alpha culled) / (tested +
 * alpha tested), summed over a window (a ratio of sums: a frame that tests more weighs more),
 * from the stats ring the renderer already harvests (occl2Tested / occl2Culled / ...). It is a
 * draw-entry share, not a triangle or GPU-time share: GPU timers are off in normal play, and the
 * shader's stats are per draw entry.
 *
 * THE STATES.
 *   Running   -- two-phase every frame. Each window (>= kWindowSecs and >= kWindowMinSamples
 *                harvested frames) whose share is under kStandDownShare stands down.
 *   StoodDown -- the plain pre-pass; nothing is freed (re-enabling is instant). A probe starts
 *                after the probe interval, or early: >= kViewProbeGapSecs after the last one on
 *                a camera cut, >= max(that, half the interval) when the camera moved / turned
 *                far since the last probe (flying over an open sim must not probe every second).
 *   Probing   -- two-phase again: the renderer seeds the visibility history on the probe's
 *                first frame (the camera-cut seed: P1 = this frame's frustum set, so phase 1 is
 *                not empty and the share is representative). The stats ring lags 3 frames, so
 *                a probe runs until kProbeSamples frames were harvested (~seed + 3 + 4 frames);
 *                share >= kResumeShare resumes Running, otherwise it stands down again.
 * HYSTERESIS: stand down under 8%, resume only at 16% or more; Running lasts at least one window;
 * a failed probe doubles the probe interval (2 s up to 16 s), and so does a stand-down within
 * kSettleSecs of a resume (a scene that keeps flipping); kSettleSecs of Running resets it.
 * Soft, as every pressure degrade: the setting is never rewritten.
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace LLCleanOccl2Adapt
{
    constexpr double   kWindowSecs       = 1.5;    // Running's decision window
    constexpr uint32_t kWindowMinSamples = 8;      // ... and at least this many harvested frames
    constexpr double   kStandDownShare   = 0.08;   // a window culling less stands down
    constexpr double   kResumeShare      = 0.16;   // a probe culling at least this resumes
    constexpr uint32_t kProbeSamples     = 4;      // harvested frames a probe decides on
    constexpr uint32_t kProbeMaxFrames   = 16;     // a probe without them by then failed
    constexpr double   kProbeFirstSecs   = 2.0;    // probe interval after a settled run
    constexpr double   kProbeMaxSecs     = 16.0;   // back-off cap
    constexpr double   kViewProbeGapSecs = 1.0;    // a view-change probe waits this long after the last
    constexpr double   kSettleSecs       = 10.0;   // Running this long resets the back-off
    // The renderer's "view changed" test (camera since the last probe / stand-down).
    constexpr float    kViewMoveMeters   = 32.f;
    constexpr float    kViewTurnCos      = 0.7071f;   // 45 degrees

    // One harvested stats slot (clean_occl2.comp stats words 0 1 4 5).
    struct Sample
    {
        uint32_t tested = 0, culled = 0, alpha_tested = 0, alpha_culled = 0;
    };

    enum class Mode : uint8_t { Running, StoodDown, Probing };

    // What the renderer logs on a stand-down or a resume (failed probes are not logged).
    struct Transition
    {
        bool     stood_down = false;   // false: resumed
        uint64_t tested = 0, culled = 0;
        uint32_t samples = 0;
        double   secs = 0.0;           // the window / probe the decision measured
        double   share = 0.0;
        double   next_probe_secs = 0.0;   // stood down: the probe interval
    };

    inline double share(uint64_t tested, uint64_t culled)
    {
        return tested ? (double)culled / (double)tested : 0.0;
    }

    class Governor
    {
    public:
        Mode mode() const { return mMode; }
        double probeInterval() const { return mInterval; }

        // Back to Running with a fresh window (mode not adaptive, stereo, setting off).
        void reset()
        {
            *this = Governor();
        }

        // Once per main-view frame that could run two-phase, before recording it. True: this
        // frame runs two-phase. *probe_start: a probe starts this frame (seed the history).
        // view_changed: the camera moved or turned far since the last probe / stand-down;
        // cut: a camera cut (teleport, region crossing, ...) is pending since then.
        // A cut probes >= kViewProbeGapSecs after the last probe; a view change only after half
        // the backed-off interval too -- flying or spinning over an open sim changes the view
        // all the time, and must not turn into a probe a second.
        bool frame(double now, bool view_changed, bool cut, bool* probe_start)
        {
            if (probe_start) *probe_start = false;
            if (mMode == Mode::Running)
            {
                if (mRunSince < 0.0) mRunSince = now;
                return true;
            }
            if (mMode == Mode::StoodDown)
            {
                const double since = now - mLastProbe;
                const bool due = now >= mNextProbe
                              || (cut && since >= kViewProbeGapSecs)
                              || (view_changed && since >= std::max(kViewProbeGapSecs, mInterval * 0.5));
                if (!due) return false;
                mMode = Mode::Probing;
                mLastProbe = now;
                mProbeFrames = 0;
                clearSums();
                mWinStart = now;
                if (probe_start) *probe_start = true;
                return true;
            }
            // Probing
            if (++mProbeFrames > kProbeMaxFrames)
            {
                probeFailed(now);   // the stats never came: stay down
                return false;
            }
            return true;
        }

        // A camera cut (the frame seeds instead of testing): Running restarts its window
        // (another scene); a probe drops what it measured so far but keeps its frame count, so
        // a cut that stays pending (no seed can record: an allocation refusal) still ends the
        // probe at kProbeMaxFrames.
        void cut(double now)
        {
            if (mMode == Mode::Running) { clearSums(); mWinStart = -1.0; }
            else if (mMode == Mode::Probing) { clearSums(); mWinStart = now; }
        }

        // One harvested stats slot of a two-phase frame.
        void sample(const Sample& s, double now)
        {
            if (mMode == Mode::StoodDown) return;   // (the renderer never harvests then)
            if (mWinStart < 0.0) mWinStart = now;
            mTested += (uint64_t)s.tested + s.alpha_tested;
            mCulled += (uint64_t)s.culled + s.alpha_culled;
            ++mSamples;
            if (mMode == Mode::Probing)
            {
                if (mSamples < kProbeSamples) return;
                const double sh = share(mTested, mCulled);
                if (sh >= kResumeShare)
                {
                    record(false, now, sh);
                    mMode = Mode::Running;
                    mRunSince = now;
                    mResumed = true;
                    clearSums();
                    mWinStart = -1.0;
                }
                else
                {
                    probeFailed(now);
                }
                return;
            }
            // Running
            if (now - mWinStart < kWindowSecs || mSamples < kWindowMinSamples) return;
            const double sh = share(mTested, mCulled);
            if (sh < kStandDownShare)
            {
                // A stand-down soon after a resume: the scene keeps flipping, back off.
                if (mResumed && now - mRunSince < kSettleSecs) mInterval = std::min(mInterval * 2.0, kProbeMaxSecs);
                mMode = Mode::StoodDown;
                mNextProbe = now + mInterval;
                mLastProbe = now;
                record(true, now, sh);
                clearSums();
                return;
            }
            if (now - mRunSince >= kSettleSecs) mInterval = kProbeFirstSecs;
            clearSums();
            mWinStart = -1.0;
        }

        // The last stand-down / resume, once.
        bool takeTransition(Transition& t)
        {
            if (!mHasTransition) return false;
            t = mTransition;
            mHasTransition = false;
            return true;
        }

    private:
        void clearSums() { mTested = mCulled = 0; mSamples = 0; }
        void probeFailed(double now)
        {
            mInterval = std::min(mInterval * 2.0, kProbeMaxSecs);
            mMode = Mode::StoodDown;
            mNextProbe = now + mInterval;
            mLastProbe = now;
            clearSums();
        }
        void record(bool down, double now, double sh)
        {
            mTransition.stood_down = down;
            mTransition.tested = mTested;
            mTransition.culled = mCulled;
            mTransition.samples = mSamples;
            mTransition.secs = mWinStart >= 0.0 ? now - mWinStart : 0.0;
            mTransition.share = sh;
            mTransition.next_probe_secs = down ? mInterval : 0.0;
            mHasTransition = true;
        }

        Mode     mMode = Mode::Running;
        uint64_t mTested = 0, mCulled = 0;
        uint32_t mSamples = 0;
        uint32_t mProbeFrames = 0;
        double   mWinStart = -1.0;
        double   mRunSince = -1.0;
        double   mNextProbe = 0.0;
        double   mLastProbe = -1e9;
        double   mInterval = kProbeFirstSecs;
        bool     mResumed = false;
        bool     mHasTransition = false;
        Transition mTransition;
    };
}
