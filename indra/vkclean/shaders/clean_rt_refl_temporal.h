// vkclean -- THE OPAQUE RT REFLECTIONS' TEMPORAL RULE (clean_deferred_light.comp pass 13 rtrTemporal, mirror and
// near-mirror receivers below RTR_STOCH_ROUGH; the stochastic glossy band keeps its own long-EMA rule). Shared by the
// shader and the CPU fixture scripts/check/test_rt_refl_temporal.cpp (scripts/check/checks/rt_refl_temporal.py), so
// the rule the check runs is the rule the GPU runs. The forward layers' rule (clean_rt_fwd_temporal.h) is the model.
//
// THE "DANCE" IT REMOVES: the old rule clamped the history to this frame's gather mean +- 1.25 sigma (+ 2 %) and
// went back to this frame as soon as the history sat 0.25 sigma outside that box. sigma was the SPATIAL spread of
// the gather only, and on a mirror the gather is mostly the pixel's own ray (a 0.6 px Gaussian, plane / normal /
// hit-distance / lobe weights: the neighbours weigh little). The own ray's hit shading is itself noisy (its lights'
// shadow rays are sampled, its GI is a cache read): the box collapsed onto this frame, the anti-lag fired in about
// half the frames (the fixture), and every reset pixel showed one frame's sample. The reflection never converged;
// reset and held pixels alternated from frame to frame -- the shimmer, worst while the camera moves (the reprojected
// history differs most). Under DLSS RR that flicker reaches the network as signal, which it keeps.
//
// THE DARKENING IT REMOVES (2026-11-09): the clamp box was symmetric in LOG space, [x / k, x k] around this frame's
// value x. A hit's shadow ray makes a two-valued signal (the lamp blocked: ~0; another light picked: a scaled-up
// value). On a low frame the box's top x k sat under the history and clipped it down; on a high frame its bottom
// x / k stayed far below the history and clipped nothing. The history settled near the GEOMETRIC mean: 0.02 / 0.12
// at p = 0.5 converged to 0.83 of the true mean, 0.002 + 10 % x 0.3 to 0.92 -- shadows in reflections 3-17 % too
// dark everywhere. Now the box is LINEAR (the same width above and below this frame) while the history agrees with
// the noise, and the log statistics only decide whether the content changed.
//
// THE RULE (per pixel, fixed cost):
//   * cur: this frame's resolved gather (pass 13's res); mu / sigma: the glossy gather's mean / one tap's deviation;
//     n: its EFFECTIVE sample count (sum(w)^2 / sum(w^2));
//   * the history (reprojected, depth-validated, bilinear), its TEMPORAL VARIANCE hv -- per channel, the running mean
//     of ln(this frame's value / the history's)^2: how far the pixel's own estimate wanders from frame to frame,
//     measured on the pixel itself (a pixel whose neighbours weigh nothing still knows its noise), per CHANNEL (a hit
//     lit by a warm and a cool light in turn holds its luminance while each channel swings several x: a luminance
//     variance would call that a change every frame) and in LOG space (the noise of a reflection scales with its
//     level: a light switched off in the mirror is a drop by a factor, not covered by the bright history's absolute
//     spread -- the ghost a linear moment held for seconds in the fixture) -- and its length hlen (frames). hv is
//     updated on every frame at a fixed rate, a reset one included (the pixel's noise is not the content's: a
//     variance restarted on each reset never grew, and the reset fed itself); a fresh one starts at RRT_HV_SEED. It
//     rises at 1 / RRT_HV_FRAMES and falls at 1 / RRT_HV_RELEASE: a hit whose shadow ray finds the blocked lamp in
//     one frame of several keeps that deviation in mind between such frames (at one rate it decayed in a run of
//     ordinary frames, and the next dark one tripped the anti-lag -- with 2 rays per hit the mean settled 5-20 %
//     dark). One frame's term is at most RRT_HV_GROW x the variance so far (RRT_HV_GROW_MIN at least): a light
//     switched off does not inflate the variance by its own step (ln 5 ^ 2 / 8 at once), so the frames after the
//     switch still see the leftover history as a change; a pixel that really is noisier gets there in a few frames;
//   * ANTI-LAG ("off"): per channel, the history's log ratio to mu in units of RRT_TEST sqrt(hv) (+ 3 %); off > 1 is
//     content that really changed (an avatar moving in the mirror, a light switched or recoloured): the blend goes
//     back toward this frame -- fully by off = 1 + 1 / RRT_TEST_RAMP on a history of 4+ frames (a partial reset
//     left a share of the switched-off light that only the clamp's bias used to remove), by off = 2 on a younger
//     one (a fresh history still learns its noise: BOOT); otherwise an accumulation of hlen + 1 frames capped at
//     `frames` (a fresh history converges as a running mean -- 1/2, 1/3, ... -- instead of an EMA's slow start);
//   * the CLAMP: while off <= RRT_LIN_END the history agrees with the pixel's noise, and the box is linear --
//     [min(mu, cur) - w, max(mu, cur) + w], w = RRT_BOX (max(hist, mu) sqrt(exp(hv) - 1) + sigma) (a log deviation
//     as a relative one, of the level the history holds -- never of this frame's value, whose low frames would narrow
//     the box on one side only); from RRT_LIN_END to off = 1 it blends into the log box [min(mu, cur) / k -
//     RRT_BOX sigma, max(mu, cur) k + RRT_BOX sigma], k = exp(RRT_BOX sqrt(hv)), which clips the remains of a change;
//   * no history: this frame (pass 13's spatial gather); the variance from the gather's relative standard error.
// The new length is 1 / the blend weight (an anti-lag reset restarts the count).
//
// SPARSE SPIKES (a rare bright light pick: 0.1 + 2 % x 3): the anti-lag cannot tell a one-frame spike from a light
// switched on in one frame. It takes the spike, and the next frame takes the base back: the integral equals the
// running mean's (unbiased, the fixture's BIAS), but the spike shows for a frame. Only more rays per hit (or rays
// stratified across frames) remove that.
#ifndef CLEAN_RT_REFL_TEMPORAL_H
#define CLEAN_RT_REFL_TEMPORAL_H
#ifdef __cplusplus
#define RRT_FN inline
#define RRT_OUT(T) T&
#else
#define RRT_FN
#define RRT_OUT(T) out T
#endif
#define RRT_BOX          float(1.25)   // the clamp box: temporal deviations / one tap's spread (linear)
#define RRT_TEST         float(2.5)    // the anti-lag: history vs mu, in temporal deviations (log)
#define RRT_TEST_MIN     float(0.03)   // ... + this log ratio (3 %): a noise-free pixel keeps a little slack
#define RRT_TEST_RAMP    float(6.0)    // ... a history of 4+ frames goes back fully at off = 1 + 1 / this
#define RRT_LIN_END      float(0.7)    // the linear box up to this anti-lag ratio, then the log box by off = 1
#define RRT_HV_FRAMES    float(8.0)    // the temporal variance: its running mean upward (frames, a fixed rate)
#define RRT_HV_RELEASE   float(16.0)   // ... downward
#define RRT_HV_MAX       float(4.0)    // ... one frame's contribution at most (a log ratio of e^2)
#define RRT_HV_GROW      float(3.5)    // ... and at most this x the variance so far
#define RRT_HV_GROW_MIN  float(0.25)   // ... but at least this (a log ratio of 0.5: a fresh seed grows to the noise)
#define RRT_EPS          float(1e-3)   // the log's floor (radiance units: a black reflection is not infinitely noisy)
#define RRT_HV_SEED      float(0.03)   // no history: the variance's floor (~17 %: an own-ray-only gather shows no
                                       // spread, and a seed of 0 reset the fresh history on its first frames' noise)
RRT_FN float rrtLum(vec3 c) { return float(0.2126) * c.x + float(0.7152) * c.y + float(0.0722) * c.z; }
RRT_FN float rrtLogRatio(float a, float b) { return log((max(a, float(0.0)) + RRT_EPS) / (max(b, float(0.0)) + RRT_EPS)); }
RRT_FN vec3 rrtLogRatio3(vec3 a, vec3 b) { return vec3(rrtLogRatio(a.x, b.x), rrtLogRatio(a.y, b.y), rrtLogRatio(a.z, b.z)); }
// The temporal variance after this frame (per channel): c this frame's estimate, h the history.
RRT_FN vec3 rrtTemporalVar(vec3 hv, vec3 c, vec3 h)
{
    const vec3 d = rrtLogRatio3(c, h);
    const vec3 h0 = vec3(max(hv.x, float(0.0)), max(hv.y, float(0.0)), max(hv.z, float(0.0)));
    const vec3 g = vec3(min(max(h0.x * RRT_HV_GROW, RRT_HV_GROW_MIN), RRT_HV_MAX),
                        min(max(h0.y * RRT_HV_GROW, RRT_HV_GROW_MIN), RRT_HV_MAX),
                        min(max(h0.z * RRT_HV_GROW, RRT_HV_GROW_MIN), RRT_HV_MAX));
    const vec3 d2 = vec3(min(d.x * d.x, g.x), min(d.y * d.y, g.y), min(d.z * d.z, g.z));
    // (up at 1 / RRT_HV_FRAMES, down at 1 / RRT_HV_RELEASE)
    const vec3 rt = vec3(d2.x > h0.x ? float(1.0) / RRT_HV_FRAMES : float(1.0) / RRT_HV_RELEASE,
                         d2.y > h0.y ? float(1.0) / RRT_HV_FRAMES : float(1.0) / RRT_HV_RELEASE,
                         d2.z > h0.z ? float(1.0) / RRT_HV_FRAMES : float(1.0) / RRT_HV_RELEASE);
    return h0 + (d2 - h0) * rt;
}
// cur / mu / sigma / n: this frame (above); hist / hv / hlen / histOk: the reprojected history; frames: the cap of
// the accumulation (>= 1). Returns the blend; v / len: the next frame's history variance and length.
RRT_FN vec3 rrtResolve(vec3 cur, vec3 mu, vec3 sigma, float n, vec3 hist, vec3 hv, float hlen, bool histOk,
                       float frames, RRT_OUT(vec3) v, RRT_OUT(float) len)
{
    if (!histOk)
    {
        // (the gather's relative standard error, at least RRT_HV_SEED: an own-ray-only gather shows no spread)
        const float rn = sqrt(max(n, float(1.0)));
        const vec3 d = vec3(sigma.x / (max(cur.x, float(0.0)) + RRT_EPS), sigma.y / (max(cur.y, float(0.0)) + RRT_EPS),
                            sigma.z / (max(cur.z, float(0.0)) + RRT_EPS)) / rn;
        v = vec3(clamp(d.x * d.x, RRT_HV_SEED, RRT_HV_MAX), clamp(d.y * d.y, RRT_HV_SEED, RRT_HV_MAX),
                 clamp(d.z * d.z, RRT_HV_SEED, RRT_HV_MAX));
        len = float(1.0);
        return cur;
    }
    const vec3 hvc = vec3(clamp(hv.x, float(0.0), RRT_HV_MAX), clamp(hv.y, float(0.0), RRT_HV_MAX),
                          clamp(hv.z, float(0.0), RRT_HV_MAX));
    const vec3 sT = vec3(sqrt(hvc.x), sqrt(hvc.y), sqrt(hvc.z));
    // the anti-lag (log): the history against this frame's gather mean, in temporal deviations
    const vec3 lr = rrtLogRatio3(hist, mu);
    const float off = max(max(abs(lr.x) / (RRT_TEST * sT.x + RRT_TEST_MIN), abs(lr.y) / (RRT_TEST * sT.y + RRT_TEST_MIN)),
                          abs(lr.z) / (RRT_TEST * sT.z + RRT_TEST_MIN));
    // the linear box: the log deviation as a relative one (sqrt(exp(hv) - 1)) of the history's level
    const vec3 rs = vec3(sqrt(exp(hvc.x) - float(1.0)), sqrt(exp(hvc.y) - float(1.0)), sqrt(exp(hvc.z) - float(1.0)));
    const vec3 w = (max(max(hist, mu), vec3(0.0)) * rs + sigma) * RRT_BOX;
    const vec3 hcL = clamp(hist, min(mu, cur) - w, max(mu, cur) + w);
    // the log box (a change's remains)
    const vec3 k = vec3(exp(RRT_BOX * sT.x), exp(RRT_BOX * sT.y), exp(RRT_BOX * sT.z));
    const vec3 loG = vec3(min(mu.x, cur.x) / k.x, min(mu.y, cur.y) / k.y, min(mu.z, cur.z) / k.z) - sigma * RRT_BOX;
    const vec3 hcG = clamp(hist, loG, max(mu, cur) * k + sigma * RRT_BOX);
    const vec3 hc = mix(hcL, hcG, clamp((off - RRT_LIN_END) / (float(1.0) - RRT_LIN_END), float(0.0), float(1.0)));
    const float a0 = float(1.0) / clamp(hlen + float(1.0), float(1.0), max(frames, float(1.0)));
    // (the reset's ramp: 1 on a history of up to 2 frames, RRT_TEST_RAMP from 4)
    const float ramp = mix(float(1.0), RRT_TEST_RAMP, clamp((hlen - float(2.0)) / float(2.0), float(0.0), float(1.0)));
    const float a = mix(a0, float(1.0), clamp((off - float(1.0)) * ramp, float(0.0), float(1.0)));
    v = rrtTemporalVar(hv, cur, hist);
    len = float(1.0) / a;
    return mix(hc, cur, a);
}
#endif
