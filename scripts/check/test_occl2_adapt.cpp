// Regression fixture for adaptive two-phase occlusion (RenderVKOcclusion2Phase 2):
// indra/newview/vkgfx/llcleanoccl2adapt.h. Built and run by scripts/check/checks/occl2_adapt.py.
//
// The simulation mirrors the renderer: every frame calls frame(); a frame that runs two-phase
// writes a stats slot that is harvested (sample()) 3 two-phase frames later; a probe's first
// frame is a seed (no stats) and invalidates the pending slots, as does every stood-down frame.
// Checks:
//   1. steady open view (3% culled): stands down after the window, then only short probes run;
//   2. indoors (40% culled): never stands down;
//   3. per-frame noise around the threshold (0% / 20% alternating, mean 10%): never stands down;
//   4. a scene that flips every 3 s between 2% and 40%: transitions stay bounded (back-off);
//   5. a camera cut / view change while stood down probes early, and a probe into an indoor
//      scene resumes within a few frames;
//   6. flying / spinning outdoors (the view changes every frame) backs probes off like timed ones;
//   7. a cut left pending every frame (no seed can record) still ends a probe;
//   8. a probe never decides before kProbeSamples harvested frames (the 3-frame stats lag).
#include "vkgfx/llcleanoccl2adapt.h"

#include <cstdio>
#include <deque>
#include <functional>

namespace
{
    using namespace LLCleanOccl2Adapt;
    int g_fail = 0;
    void check(bool ok, const char* what)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++g_fail;
        }
    }

    struct Sim
    {
        Governor gov;
        std::deque<Sample> ring;   // pending stats slots, oldest first
        double now = 0.0;
        double dt = 1.0 / 60.0;
        uint64_t frames = 0, twoPhaseFrames = 0, downFrames = 0;
        int standDowns = 0, resumes = 0, probes = 0;

        // share: the culled share this frame's two-phase test would measure.
        void step(double share, bool view_changed = false, bool cut = false)
        {
            if (cut) gov.cut(now);
            bool probe = false;
            const bool run = gov.frame(now, view_changed, cut, &probe);
            if (probe) ++probes;
            if (!run)
            {
                ring.clear();   // stood down: the pending slots are invalidated
                ++downFrames;
            }
            else if (probe || cut)
            {
                ring.clear();   // a seed frame: no stats, pending slots invalidated
            }
            else
            {
                ++twoPhaseFrames;
                // split: harvest the slot written 3 two-phase frames ago
                if (ring.size() >= 3)
                {
                    gov.sample(ring.front(), now);
                    ring.pop_front();
                }
                Sample s;
                s.tested = 2000;
                s.culled = (uint32_t)(share * 2000.0 + 0.5);
                s.alpha_tested = 100;
                s.alpha_culled = (uint32_t)(share * 100.0 + 0.5);
                ring.push_back(s);
            }
            Transition t;
            while (gov.takeTransition(t)) (t.stood_down ? standDowns : resumes)++;
            now += dt;
            ++frames;
        }
        void run(double secs, const std::function<double(double)>& share)
        {
            const double end = now + secs;
            while (now < end) step(share(now));
        }
    };

    void steadyOutdoor()
    {
        Sim s;
        s.run(3.0, [](double) { return 0.03; });
        check(s.gov.mode() != Mode::Running, "outdoor 3%: stood down within 3 s");
        check(s.standDowns == 1, "outdoor 3%: exactly one stand-down");
        const uint64_t f0 = s.frames, tp0 = s.twoPhaseFrames;
        s.run(60.0, [](double) { return 0.03; });
        check(s.resumes == 0, "outdoor 3%: never resumes");
        check(s.standDowns == 1, "outdoor 3%: no further stand-down lines");
        const double share_tp = (double)(s.twoPhaseFrames - tp0) / (double)(s.frames - f0);
        if (share_tp > 0.03) std::printf("  outdoor: %.1f%% of frames ran two-phase while stood down\n", share_tp * 100.0);
        check(share_tp <= 0.03, "outdoor 3%: probes cost at most 3% of frames once backed off");
        check(s.gov.probeInterval() == kProbeMaxSecs, "outdoor 3%: probe interval backed off to the cap");
    }

    void steadyIndoor()
    {
        Sim s;
        s.run(60.0, [](double) { return 0.40; });
        check(s.gov.mode() == Mode::Running, "indoor 40%: stays running");
        check(s.standDowns == 0 && s.downFrames == 0, "indoor 40%: never stands down");
    }

    void noisyAroundThreshold()
    {
        Sim s;
        uint64_t k = 0;
        s.run(60.0, [&](double) { return (k++ & 1) ? 0.20 : 0.0; });
        check(s.standDowns == 0, "per-frame 0%/20% noise (mean 10%): never stands down");
    }

    void flipping()
    {
        Sim s;
        s.run(120.0, [](double t) { return ((int)(t / 3.0) & 1) ? 0.40 : 0.02; });
        const int transitions = s.standDowns + s.resumes;
        if (transitions > 24) std::printf("  flipping: %d transitions in 120 s\n", transitions);
        check(transitions <= 24, "3 s flipping scene: at most 24 transitions in 120 s (back-off)");
        check(transitions >= 2, "3 s flipping scene: still follows the scene");
    }

    void cutProbesEarly()
    {
        Sim s;
        s.run(3.0, [](double) { return 0.03; });
        check(s.gov.mode() == Mode::StoodDown, "cut: stood down first");
        s.run(10.0, [](double) { return 0.03; });   // backs off past the first interval
        const int p0 = s.probes;
        s.step(0.5, false, true);                    // teleport into a building
        check(s.probes == p0 + 1, "cut while stood down: probes at once");
        for (int i = 0; i < 12 && s.gov.mode() != Mode::Running; ++i) s.step(0.5);
        check(s.gov.mode() == Mode::Running, "cut into a building: resumes within 12 frames");
        check(s.resumes == 1, "cut into a building: one resume line");
        // a view change (no cut) also probes early, but not within kViewProbeGapSecs
        Sim v;
        v.run(3.0, [](double) { return 0.03; });
        v.run(1.0, [](double) { return 0.03; });
        const int q0 = v.probes;
        v.step(0.03, true);
        v.step(0.03, true);
        check(v.probes <= q0 + 1, "view change: at most one probe per kViewProbeGapSecs");
    }

    // Flying / spinning over an open sim: the view changes every frame. Probes must back off
    // like timed ones, not run every kViewProbeGapSecs.
    void flyingOutdoor()
    {
        Sim s;
        s.run(3.0, [](double) { return 0.03; });
        const uint64_t f0 = s.frames, tp0 = s.twoPhaseFrames;
        const double end = s.now + 60.0;
        while (s.now < end) s.step(0.03, true);
        const double share_tp = (double)(s.twoPhaseFrames - tp0) / (double)(s.frames - f0);
        if (share_tp > 0.03) std::printf("  flying: %.1f%% of frames ran two-phase while stood down\n", share_tp * 100.0);
        check(share_tp <= 0.03, "flying outdoors (view changes every frame): probes at most 3% of frames");
        check(s.standDowns == 1 && s.resumes == 0, "flying outdoors: one stand-down, no resume");
    }

    // A cut that stays pending every frame (the seed cannot record: an allocation refusal)
    // must still end the probe at kProbeMaxFrames, not hold it open.
    void pendingCutEndsProbe()
    {
        Governor g;
        double now = 0.0;
        for (int i = 0; i < 200; ++i)
        {
            bool p = false;
            g.frame(now, false, false, &p);
            Sample s; s.tested = 1000; s.culled = 0;
            if (g.mode() == Mode::Running) g.sample(s, now);
            now += 1.0 / 60.0;
        }
        check(g.mode() == Mode::StoodDown, "pending cut: stood down");
        bool started = false;
        for (uint32_t i = 0; i < 4 * kProbeMaxFrames; ++i)
        {
            g.cut(now);
            bool p = false;
            g.frame(now, false, true, &p);
            started = started || p;
            now += 1.0 / 60.0;
            if (started && g.mode() == Mode::StoodDown) break;
        }
        check(started, "pending cut: a probe started");
        check(g.mode() == Mode::StoodDown, "pending cut: the probe ended (no stats) within kProbeMaxFrames");
    }

    void probeWaitsForLag()
    {
        Governor g;
        // stand down
        double now = 0.0;
        for (int i = 0; i < 200; ++i)
        {
            bool p = false;
            g.frame(now, false, false, &p);
            Sample s; s.tested = 1000; s.culled = 0;
            if (g.mode() == Mode::Running) g.sample(s, now);
            now += 1.0 / 60.0;
        }
        check(g.mode() == Mode::StoodDown, "lag: stood down");
        bool p = false;
        g.frame(now + kProbeMaxSecs, true, false, &p);
        check(p && g.mode() == Mode::Probing, "lag: probe started");
        Sample hi; hi.tested = 1000; hi.culled = 900;
        for (uint32_t i = 0; i + 1 < kProbeSamples; ++i) g.sample(hi, now + kProbeMaxSecs);
        check(g.mode() == Mode::Probing, "lag: no decision before kProbeSamples samples");
        g.sample(hi, now + kProbeMaxSecs);
        check(g.mode() == Mode::Running, "lag: resumes on the kProbeSamples-th high sample");
    }
}

int main()
{
    steadyOutdoor();
    steadyIndoor();
    noisyAroundThreshold();
    flipping();
    cutProbesEarly();
    flyingOutdoor();
    pendingCutEndsProbe();
    probeWaitsForLag();
    if (g_fail) return 1;
    std::printf("occl2-adapt: ok\n");
    return 0;
}
