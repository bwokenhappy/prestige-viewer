/**
 * @file slcamerareview.h
 * @brief Camera mode (#1577): image review. Every shot of the session keeps a preview, a
 *        thumbnail and a histogram; camera mode's kit overlay (slcameraview.cpp) shows the
 *        thumbnails and a card for any of them (#1572).
 *
 * The session is the camera mode session: its shots (a preview copy at most 1280 pixels on
 * the long side, a thumbnail and a histogram each, at most MAX_SHOTS of them, the oldest
 * dropped) are freed when camera mode closes. The files on disk are the snapshot save path's.
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

#ifndef SL_CAMERAREVIEW_H
#define SL_CAMERAREVIEW_H

#include "llrect.h"
#include "stdtypes.h"

#include <string>

class LLImageRaw;

namespace SLCameraReview
{
constexpr size_t MAX_SHOTS = 16;

// A shot was taken (the full image, as saved); @a label describes it ("1/125 f/8 ISO 100").
// Returns its serial (unique for the process; 0: not kept, out of memory).
U32 add(const LLImageRaw& image, const std::string& label);
// The file the shot was saved to, once the save finished.
void setPath(U32 serial, const std::string& path);

// The session's shots, oldest first (#1572: the kit overlay reads them).
constexpr size_t HIST_BINS = 64;
size_t             count();
U32                serial(size_t i);
const LLImageRaw*  thumbnail(size_t i); // at most 160 px on the long side
const LLImageRaw*  preview(size_t i);   // at most 1280 px on the long side
void               imageSize(size_t i, S32& w, S32& h);
const std::string& label(size_t i);
const std::string& path(size_t i);
const std::string& timeTaken(size_t i); // "13:32"
const F32*         histogram(size_t i); // HIST_BINS values, 0..1
// Deletes the shot's file from disk. False when it was not there (any more).
bool               deleteFile(size_t i);
// Out of the review (the file stays unless deleteFile took it).
void               remove(size_t i);
// Camera mode closed: the session's shots are freed.
void clear();
bool empty();
} // namespace SLCameraReview

#endif // SL_CAMERAREVIEW_H
