/**
 * @file slcamerarestore.h
 * @brief Camera mode (#1572): everything camera mode can change is put back when it closes.
 *
 * Entry snapshots every setting the overlay's dials, the Photo Studio rail, the drive (bracketing),
 * the DoF preview and a camera-mode preset can write -- exposure, depth of field, motion blur,
 * white balance and grade, field of view, the rail's look / haze / effects / scene values, the
 * Looks and camera presets -- plus the camera roll and the sky. Any exit puts them back exactly.
 * Camera mode's own state (its SLCameraMode* settings: mode dial, ISO, grid, aspect, level,
 * drive, focus mode, rail layout) is not part of it and persists.
 *
 * Crash-safe: the snapshot is also kept in SLCameraModeRestore (persisted, saved with the very
 * settings it guards), so a viewer that quit or died in camera mode puts them back at the next
 * startup.
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

#ifndef SL_CAMERARESTORE_H
#define SL_CAMERARESTORE_H

#include <string>
#include <vector>

namespace SLCameraRestore
{
// Every setting camera mode can change (and so snapshots and restores).
const std::vector<std::string>& guardedSettings();

// Camera mode opens: snapshot the guarded settings, the roll and the sky.
void capture();
// Camera mode closes: put them all back and forget the snapshot. @a in_session: also the
// session-only state (the roll, the sky); false at quit, where there is no world left to touch.
void restore(bool in_session = true);
// Startup: a snapshot left by a viewer that quit or crashed in camera mode is put back.
void restoreAfterCrash();

// The rail or a preset is about to change the sky: the sky gets restored too.
void noteSkyTouched();
// <Prestige> Sky and water changes are kept (PrestigeCameraEnvForPhotoOnly off): the sky is not restored.
void keepSky();
// </Prestige>
} // namespace SLCameraRestore

#endif // SL_CAMERARESTORE_H
