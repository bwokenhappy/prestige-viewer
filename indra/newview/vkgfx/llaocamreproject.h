/**
 * @file llaocamreproject.h
 * @brief Camera-only reprojection for the AO temporal accumulator: when the motion path is needed.
 *
 * The AO temporal accumulator (#843, RenderVKAOTemporal, clean_ao.comp aoTemporal) reprojects its
 * history through last frame's camera (frame info taa_prev_vp) and, on frames whose pre-pass wrote
 * them, along the motion vectors. Without an upscaler it used to be the one thing keeping the whole
 * motion path alive: the motion target and no-history mask, the previous-instance / joint / palette /
 * skinned-position copies, and the depth + motion pre-pass instead of the depth-only one.
 *
 * RenderVKAOCameraReproject (live A/B, not saved) lets the accumulator use the camera alone when it is
 * the ONLY consumer of that path: the previous camera is still tracked (the frame info's taa_prev_vp
 * and reset verdict), but no motion vectors or previous rows are made. Moving objects then lean on the
 * accumulator's own history rejection (depth test along the receiver's plane, the 3 x 3 neighbourhood
 * clamp with its anti-lag length cut, the reset verdict).
 *
 * Every other reader keeps the full path. Some force it (the upscaler, motion blur, the RT shadow /
 * radiance denoisers, the RT passes that track moving casters or reuse along the motion vectors, the
 * motion-vector debug view). Others only read the motion target when it exists (user FX with a motion
 * input, a live snapshot's motion rejection): with the setting on they still block camera-only, so
 * their output never changes. The adaptive shading rate (VRS) follows the camera reprojection in
 * camera-only frames instead (clean_vrs_rate.comp, mode.z).
 *
 * Pure functions: scripts/check/test_ao_cam_reproject.cpp pins them.
 */

#ifndef LL_LLAOCAMREPROJECT_H
#define LL_LLAOCAMREPROJECT_H

namespace llaocam
{

struct Inputs
{
    bool setting = false;         // RenderVKAOCameraReproject
    bool aoTemporal = false;      // aoWantsTemporal(): AO on, the accumulator on
    bool stereo = false;          // stereoActive(): the plumbing serves the per-view resolve only
    // Readers that force the motion path:
    bool upscaler = false;        // taaRequested(): FSR / DLSS (and frame generation on top of them)
    bool debugMotion = false;     // RenderVKDebugMotionVectors
    bool motionBlur = false;      // motionBlurWanted()
    bool rtMaskTemporal = false;  // rtMaskWantsTemporal(): RT shadow mask denoiser (motion + previous rows)
    bool localDenoiser = false;   // mPdnWant: the stochastic local-light denoiser
    bool rtMotion = false;        // rtMotionConsumerActive(): GI, ReSTIR reuse, RTAO, RT sun / reflections
    // Readers that use the motion target only when it exists:
    bool fxMotion = false;        // a live FX technique samples the motion input
    bool liveSnapshot = false;    // a live snapshot is armed (motion rejection)
};

// Any reader other than the AO accumulator that needs (or would see) the motion target.
inline bool otherMotionReader(const Inputs& in)
{
    return in.upscaler || in.debugMotion || in.motionBlur || in.rtMaskTemporal || in.localDenoiser || in.rtMotion
        || in.fxMotion || in.liveSnapshot;
}

// This frame the AO accumulator reprojects with the camera alone and the motion path is dropped.
inline bool cameraOnly(const Inputs& in)
{
    return in.setting && in.aoTemporal && !in.stereo && !otherMotionReader(in);
}

// The full motion path (motion target, previous rows, depth + motion pre-pass) is wanted.
// Stereo frames keep it for the per-view resolve only (#1075 step 10).
inline bool motionPath(const Inputs& in)
{
    if (in.stereo) return in.upscaler;
    const bool forcing = in.upscaler || in.debugMotion || in.motionBlur || in.rtMaskTemporal || in.localDenoiser;
    return forcing || (in.aoTemporal && !cameraOnly(in));
}

// The previous camera (taa_prev_vp, the reset verdict) is tracked: with the motion path, or camera-only.
inline bool cameraHistory(const Inputs& in, bool motionPathBuilt)
{
    return motionPathBuilt || cameraOnly(in);
}

} // namespace llaocam

#endif // LL_LLAOCAMREPROJECT_H
