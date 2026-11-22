// Daytime low-key exposure A/B. Independent policy on top of Slipstream's meter;
// no scene classification or change to lighting, bloom, or authored EEP ranges.
#ifndef SL_INTERIOR_EXPOSURE_H
#define SL_INTERIOR_EXPOSURE_H

#include <cmath>

inline float slInteriorExposureWeight(bool enabled, bool skyRange, bool noPost,
                                      bool physicalNight, float sunZ)
{
    if (!enabled || skyRange || noPost || physicalNight || !std::isfinite(sunZ))
        return 0.f;
    // Fade from the horizon to four degrees: the night curve and the legacy
    // sun/moon handover see zero weight, without a time-of-day toggle pop.
    const float t = sunZ <= 0.f ? 0.f : (sunZ >= 0.06975647f ? 1.f : sunZ / 0.06975647f);
    return t * t * (3.f - 2.f * t);
}

#endif
