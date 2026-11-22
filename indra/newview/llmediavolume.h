/**
 * @file llmediavolume.h
 * @brief The volume a media instance's plugin plays at: the media's own volume, the master Media
 *        volume and the distance falloff from a media face combined.
 *
 * Standard C++ only, so scripts/check/test_media_volume.cpp builds it on the host (check
 * media-volume).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#ifndef LL_LLMEDIAVOLUME_H
#define LL_LLMEDIAVOLUME_H

struct LLMediaVolumeInputs
{
    float requested = 1.f;      // this media's own volume (its slider, its mute)
    float master = 1.f;         // master volume x Media volume, 0 when either is muted (llvieweraudio)
    double proximity = 0.0;     // camera distance to the media face in metres; <= 0: not on a face / unknown
    float rolloffMin = 0.f;     // MediaRollOffMin: full volume up to here
    float rolloffMax = 0.f;     // MediaRollOffMax: silent beyond here
    float rolloffRate = 0.f;    // MediaRollOffRate
    bool audible = true;        // false when another media is the only one allowed to be heard
};

// attenuated = 1 / (1 + rate * (d - min))^2 between min and max; never louder than unattenuated.
inline float llmedia_effective_volume(const LLMediaVolumeInputs& in)
{
    if (!in.audible)
        return 0.f;

    float volume = in.requested * in.master;
    if (in.proximity > 0)
    {
        if (in.proximity > in.rolloffMax)
        {
            volume = 0.f;
        }
        else if (in.proximity > in.rolloffMin)
        {
            // the +1 is there so that for distance min the volume stays the same
            const double adjusted_distance = in.proximity - in.rolloffMin;
            double attenuation = 1.0 + (in.rolloffRate * adjusted_distance);
            attenuation = 1.0 / (attenuation * attenuation);
            // the attenuation multiplier should never be more than one since that would increase volume
            volume = volume * (float)(attenuation < 1.0 ? attenuation : 1.0);
        }
    }
    return volume;
}

#endif // LL_LLMEDIAVOLUME_H
