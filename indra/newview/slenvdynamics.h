/**
 * @file slenvdynamics.h
 * @brief How bright an environment's light can drive the frame: the pure math shared by the
 *        frame packer (llappviewer.cpp), the renderer (llcleanrenderervk.cpp) and the regression
 *        fixture (scripts/check/test_env_dynamics.cpp). No viewer types, header only.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 *
 * SUNLIT WHITE (sunlitWhite / sunlitWhiteClassic): the radiance a white diffuse surface facing the
 * sun reaches from the environment's own light -- the sun term surfaces get (atmos_k[5], the
 * WindLight light-path attenuation and sky_sunlight_scale applied) plus the sky ambient at its
 * strongest. It is the scale the environment lights the scene at: about 0.5 to 0.9 for Second
 * Life's default skies, around 11 for a creator sky with an orange sun at the stored maximum (3).
 *
 * EXPLICIT-ZERO PROBE AMBIANCE (explicitZeroClampWeight): a sky that SAYS "reflection probe
 * ambiance 0" (the key present: not a classic WindLight sky) is lit in linear HDR like any PBR
 * sky -- LL's calcAtmosphericVarsLinear / pbrBaseLight, which we match term for term -- yet Second
 * Life viewers show it through the NoPost programs: no exposure, no tonemapper, a hard clamp. A
 * modest sun stays inside 0..1 and the clamp is invisible (that is how such skies are usually
 * authored: "ambiance 0 = no tone mapping"). A bright one (sunlit white past 1) clips every sunlit
 * surface flat: a yellow cliff goes pale yellow to white. Those skies take the HDR path instead
 * (exposure range, tonemapper, sky HDR scale), blended in between 1 and 2 so a day cycle crossing
 * the band does not pop. The returned weight is the no-post share: 1 = LL's clamp, 0 = the HDR
 * path (what the same sky shows with ambiance 0.01).
 *
 * BLOOM SUNLIT LIMIT (bloomThreshold): the bloom extract's threshold is raised to the sunlit
 * white, the knee scaled with it, so a surface lit only by the environment's sun and sky never
 * blooms by itself (the sun disc, speculars, emissive and local lights still do). Below the
 * user's threshold (default 2.5) it changes nothing: every default sky stays exactly as it was.
 *
 * ENVIRONMENT DYNAMICS (RenderEnvironmentDynamics, 0..1; Preferences > Graphics): how far the
 * frame goes past what Second Life's own viewers show. Our sun / ambient scales, exposure range
 * and tonemapper already match LL's for the same sky; what LL does not have is the luminance
 * part of the bloom (LL's glow extracts the glow channel only: GLOW_MIN_LUMINANCE 9999) and the
 * HDR display's highlight headroom. 0 = neither (classic-viewer look), 1 = both (our full look).
 */
#ifndef SL_ENVDYNAMICS_H
#define SL_ENVDYNAMICS_H

#include <algorithm>
#include <cmath>

namespace slenv
{
    inline float srgbToLinear(float cs)
    {
        return cs <= 0.04045f ? cs / 12.92f : std::pow((std::max(cs, 0.f) + 0.055f) / 1.055f, 2.4f);
    }

    inline float smoothstep01(float e0, float e1, float x)
    {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // Non-classic (linear) sky: sunlit = atmos_k[5] (linear, x sky_sunlight_scale); amblit_base =
    // atmos_k[4] (pow(ambient, 0.9) x 0.57, sRGB-ish), neutralised to luminance as the surfaces do
    // (clean_atmos_common.glsl atmosAmbientTerm, at its strongest: N.L = 0).
    inline float sunlitWhite(const float sunlit[3], const float amblit_base[3], float ambient_scale)
    {
        const float sun = std::max(std::max(sunlit[0], sunlit[1]), sunlit[2]);
        const float amb = 0.2126f * srgbToLinear(amblit_base[0]) + 0.7152f * srgbToLinear(amblit_base[1])
                        + 0.0722f * srgbToLinear(amblit_base[2]);
        return std::max(sun, 0.f) + std::max(amb, 0.f) * std::max(ambient_scale, 0.f);
    }

    // Classic sky: the legacy recombination (LL softenLightF classic_mode: sunlit x 1.35, the sum in
    // sRGB space, x 1.1): srgb_to_linear(ambient x 0.9 + sunlit x 1.35 x 0.7) x 1.1.
    inline float sunlitWhiteClassic(const float sunlit[3], const float amblit_base[3], float ambient_scale)
    {
        const float sun = std::max(std::max(sunlit[0], sunlit[1]), sunlit[2]);
        const float amb = std::max(std::max(amblit_base[0], amblit_base[1]), amblit_base[2]) * ambient_scale;
        return 1.1f * srgbToLinear(std::max(amb * 0.9f + sun * 1.35f * 0.7f, 0.f));
    }

    // The packer's own sunlit (fillAtmosK's, per channel): the light colour through WindLight's
    // light-path extinction, linearised for a non-classic sky, x sky_sunlight_scale.
    inline void skySunlit(const float light[3], const float blue_density[3], float haze_density, float density_mul,
                          float max_y, float lightnorm_z, float sun_scale, bool classic, float out[3])
    {
        const float ahf = 1.f / std::max(1e-6f, lightnorm_z);
        for (int c = 0; c < 3; ++c)
        {
            const float atten = (blue_density[c] + haze_density * 0.25f) * (density_mul * max_y);
            const float s = light[c] * std::exp(-atten * ahf);
            out[c] = (classic ? s : srgbToLinear(s)) * sun_scale;
        }
    }

    // The same from the packed atmosphere block (wf.atmos / mAtmos, layout in
    // clean_atmos_common.glsl): rows 1 blue density + haze density, 2 light + cloud shadow,
    // 3 ambient + max_y, 4 light norm + density multiplier, 8.w classic, 12.zw sun / ambient scale.
    inline float sunlitWhiteFromPacked(const float* a)
    {
        const bool classic = a[8 * 4 + 3] > 0.5f;
        float sunlit[3], amb_base[3];
        skySunlit(&a[2 * 4], &a[1 * 4], a[1 * 4 + 3], a[4 * 4 + 3], a[3 * 4 + 3], a[4 * 4 + 2], a[12 * 4 + 2], classic,
                  sunlit);
        const float cs = a[2 * 4 + 3];
        for (int c = 0; c < 3; ++c)
        {
            const float t = a[3 * 4 + c] + (1.f - a[3 * 4 + c]) * cs * 0.5f;
            amb_base[c] = std::pow(std::max(t, 0.f), 0.9f) * 0.57f;
        }
        return classic ? sunlitWhiteClassic(sunlit, amb_base, a[12 * 4 + 3]) : sunlitWhite(sunlit, amb_base, a[12 * 4 + 3]);
    }

    constexpr float kClampFullBelow = 1.f;   // sunlit white at or below: LL's clamp exactly
    constexpr float kClampNoneAbove = 2.f;   // at or above: the HDR path exactly
    constexpr float kWeightSteps = 32.f;     // (quantised: the tonemap LUT re-bakes on a change)

    // The no-post share for an explicit-zero-ambiance sky (1 = LL's clamp, 0 = the HDR path).
    inline float explicitZeroClampWeight(float sunlit_white)
    {
        if (!(sunlit_white == sunlit_white)) return 1.f;   // (NaN: as LL)
        const float w = 1.f - smoothstep01(kClampFullBelow, kClampNoneAbove, sunlit_white);
        return std::round(w * kWeightSteps) / kWeightSteps;
    }

    // The bloom extract's threshold / knee with the sunlit limit.
    inline void bloomThreshold(float threshold, float knee, float sunlit_white, float& t_out, float& k_out)
    {
        t_out = threshold;
        k_out = knee;
        if (threshold > 0.f && sunlit_white > threshold && std::isfinite(sunlit_white))
        {
            t_out = sunlit_white;
            k_out = knee * (sunlit_white / threshold);
        }
    }

    inline float dynamics(float setting) { return std::isfinite(setting) ? std::clamp(setting, 0.f, 1.f) : 1.f; }

    // The bloom's luminance (threshold) term weight; the glow channel is not scaled.
    inline float bloomLuminanceWeight(float dyn) { return dynamics(dyn); }

    // The HDR display's highlight headroom (peak / paper white), 1 = none past paper white.
    inline float headroomRatio(float ratio, float dyn) { return 1.f + std::max(ratio - 1.f, 0.f) * dynamics(dyn); }
}

#endif // SL_ENVDYNAMICS_H
