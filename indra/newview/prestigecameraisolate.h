/**
 * @file prestigecameraisolate.h
 * @brief Camera mode's "Isolate your subject": the shot saved as the subject alone on a
 *        transparent background (a PNG for posters and profiles).
 *
 * Modes (PrestigeCameraIsolate): 0 off, 1 my avatar (with what it wears), 2 picked subjects
 * (avatars and animesh clicked in the viewfinder while picking is on: a click toggles one, Esc
 * ends picking; the overlay marks the picked ones). The pick list lasts one camera-mode session.
 *
 * HOW THE CUT-OUT IS MADE. Not by rendering twice over black and white: this renderer already
 * has a transparent avatar layer (llsnapshotavatars.h snapshotRawAvatar, the Vulkan renderer's
 * vkgfx/llcleansnapmattevk.cpp). One offscreen render sequence at the shot's size keeps only
 * the chosen avatars in the main view while the whole world still lights, shadows and reflects
 * them (sun, ambient, probes, shadows unchanged), and carries their COVERAGE through the colour
 * chain as alpha: soft for hair and lace (alpha-blended and alpha-masked surfaces composite
 * their alpha "over"), partly transparent for worn glass, antialiased by the supersampling, and
 * unpremultiplied on the GPU into straight-alpha RGBA. Sky, clouds, particles, precipitation,
 * glow, bloom, lens effects, god rays and depth of field are not in it. What the world has in
 * front of the subject (a wall, a table, an avatar left out) cuts it, as the camera sees it.
 * The scope (who is in the layer) is handed to that render for the length of one capture by an
 * RAII override (LLSnapshotScopeOverride): the Snapshot floater's own scope and ticks are never
 * touched, and every path out restores them.
 *
 * Objects that are not worn (a car, a chair, a statue) cannot be isolated: the layer's
 * avatar-only main view is decided per instance in the GPU cull (clean_cull.comp
 * kCullAvatarsOnly, clean_cluster_select.comp CLV_AVATARS_ONLY) and has no per-object keep
 * list. Clicking one while picking says so.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef PRESTIGE_CAMERAISOLATE_H
#define PRESTIGE_CAMERAISOLATE_H

#include "stdtypes.h"

#include <string>
#include <vector>

class LLImageRaw;

namespace PrestigeCameraIsolate
{
enum Mode : S32
{
    MODE_OFF = 0,
    MODE_MY_AVATAR = 1,
    MODE_PICKED = 2,
    MODE_COUNT = 3
};

S32  mode();          // PrestigeCameraIsolate, clamped
void setMode(S32 mode);
bool enabled();       // a mode is on: shots are cut out, saved as PNG

// ---- picking (MODE_PICKED) ----
bool picking();
void setPicking(bool on);
// A click in the viewfinder (window UI coordinates) while picking: the avatar or animesh under it
// joins or leaves the pick (an attachment counts as its wearer). False outside the frame.
bool pickAt(S32 x, S32 y);
size_t pickedCount();   // you included, when picked
void clearPicks();
// Where the picked subjects in view are, window UI coordinates (y up), for the overlay's marks.
void markPoints(std::vector<std::pair<S32, S32>>& out);

// ---- the shot ----
// The subject alone, straight-alpha RGBA, rows bottom-up, w x h (scaled up when the video memory
// only allows a smaller render). False (with a notice) when there is nothing to cut out or the
// render failed.
bool capture(LLImageRaw& raw, S32 w, S32 h);
// Before an inventory upload: the power-of-two size an SL texture gets, resampled in
// premultiplied space so the edges do not darken (the upload's own scale is then a no-op).
void prepareForTexture(LLImageRaw& raw);

// The status pill ("Isolate: my avatar · PNG"); empty when off.
std::string statusLabel();
// A camera-mode session ends: picking off, the picks forgotten.
void endSession();
} // namespace PrestigeCameraIsolate

#endif // PRESTIGE_CAMERAISOLATE_H
