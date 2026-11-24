/**
 * @file slcamerapresets.h
 * @brief Camera mode presets (#1231, #1572): the whole camera-mode setup saved under a name and
 *        recalled in one click -- exposure mode, f-number, shutter, ISO, EV, white balance, focal
 *        length, roll, the DoF and motion blur switches, aspect, grid, focus mode, the rail's look /
 *        sky / haze / effects / scene values -- and the camera's placement (#1231), which is put
 *        back when the preset is loaded in the region it was saved in.
 *
 * One file per preset (LLSD XML) in the user settings folder, camera_mode_presets/. Loading a
 * preset inside camera mode is undone on exit like every other camera-mode change
 * (slcamerarestore.h).
 *
 * The overlay's Presets panel lists them (click: load, x: delete after a confirming second
 * click) and saves the current setup under a typed name.
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

#ifndef SL_CAMERAPRESETS_H
#define SL_CAMERAPRESETS_H

#include "indra_constants.h"
#include "llrect.h"
#include "stdtypes.h"

#include <string>
#include <vector>

namespace SLCameraPresets
{
// The store.
std::vector<std::string> list();
bool save(const std::string& name);
bool load(const std::string& name);
bool remove(const std::string& name);
// <Prestige> The setup under another name (false: no such setup, the name is taken, or it could not be written).
bool rename(const std::string& from, const std::string& to);
// </Prestige>
} // namespace SLCameraPresets

#endif // SL_CAMERAPRESETS_H
