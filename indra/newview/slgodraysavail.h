/**
 * @file slgodraysavail.h
 * @brief Whether the god rays setting can do anything, judged from the settings alone.
 *
 * The Vulkan renderer skips the god rays pass while volumetric fog is on or the physical
 * sky's camera medium is ready (LLCleanRendererVK::mVolFogNow / mCameraMediumReady, set in
 * ensureVolFog: the medium is on when RenderVKPhysicalSky is on and its resources exist; the
 * fog is on when RenderVKVolumetricFog is not 0). The Preferences, Photo Studio and Camera
 * mode god rays controls all grey out through this one predicate, so they cannot drift.
 * It deliberately ignores transient state (an ortho view, VR, a snapshot, a medium still
 * warming up): the controls grey by the user's choice, not by what a frame is doing.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_GODRAYSAVAIL_H
#define SL_GODRAYSAVAIL_H

#include "llviewercontrol.h"

namespace SLGodRays
{
// True when volumetric fog is off and the physical sky is off, the only case the pass runs.
inline bool availableFromSettings()
{
    return gSavedSettings.getU32("RenderVKVolumetricFog") == 0 &&
           !gSavedSettings.getBOOL("RenderVKPhysicalSky");
}

// The tooltip the controls show while they are greyed.
inline const char* unavailableTip()
{
    return "Replaced by volumetric fog and the physical sky while either is on. "
           "Turn both off to use god rays.";
}
} // namespace SLGodRays

#endif // SL_GODRAYSAVAIL_H
