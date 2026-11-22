/**
 * @file slvideorecorder.h
 * @brief Video Studio (#1668): recording a ride to video.
 *
 * TWO PATHS, the same clock (slvtrack::RecClock):
 *  - H.264 (a GPU with a Vulkan video encode queue): the world image at capture point 2 -- after the
 *    world and the HUD pass, BEFORE the interface is composed (Video Studio hides the HUDs) -- goes
 *    to an encoder of its own (vkgfx/llcleanreplayvk.cpp in record mode: nothing is evicted), and
 *    is written as an MP4 to <Videos>/Slipstream when the ride ends. Real time: the viewer's own
 *    sound goes with it (llinstantreplayaudio.cpp). Not with HDR output (the encoder is SDR).
 *  - PNG SEQUENCE (no encoder, HDR output, or SLVideoStudioForcePNG): one live snapshot of the world
 *    (point 1) per frame, written as numbered PNGs into a folder in <Videos>/Slipstream by a worker
 *    thread. Always fixed step (a read waits for its frame), never sound.
 *
 * BOUNDS: an H.264 recording holds at most kMaxSeconds and kBudgetBytes of bitstream (it stops and
 * saves at either); the PNG queue holds at most kPngQueue frames (the ride waits for the writer);
 * a sequence is at most kMaxSeconds long. A failed allocation retries on the next frames, then the
 * recording stops with what it has, and says so in the log; nothing throws out of a frame.
 *
 * Costs nothing while idle: every hook is one read of busy().
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

#ifndef SL_VIDEORECORDER_H
#define SL_VIDEORECORDER_H

#include "slvideotrack.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

class VkDeviceContext;

namespace SLVideoRecorder
{
// The limits (bounded memory, #1668).
constexpr uint32_t kMaxSeconds = 300;                 // one recording (the encoder's longest)
constexpr uint64_t kBudgetBytes = 1024ull << 20;      // H.264 bitstream held until the file is written
constexpr uint32_t kPngQueue = 3;                     // PNG frames waiting for the writer
constexpr uint32_t kAllocRetries = 30;                // frames a failed allocation is retried for

enum class Path
{
    H264,
    Png
};
struct Options
{
    uint32_t height = 1080;   // the picture's height (never above the window's; aspect kept)
    uint32_t fps = 30;
    bool fixedStep = true;    // smooth video, may render slower than real time; no sound
    bool sound = true;        // real time, H.264 only
};

// The path a recording would take now (H.264 when the GPU can, the display is SDR and PNG is not forced).
Path pathFor();
// Starts recording (the ride's clock begins at the first frame the video takes). False + `why`.
bool start(const Options& options, std::string& why);
// Stops: the file is written in the background (busy() until it is).
void stop();
// Preroll or recording.
bool recording();
// Recording or still writing the file.
bool busy();
slvtrack::RecState state();
// The ride's time for the frame about to render (0 during the preroll).
double rideTime();
// Seconds of video so far.
double videoSeconds();
// The status line for the overlay ("", "Starting", "Recording 12.0 s", "Saving ...").
std::string status();
// A Once ride has reached its end in the video (the frame at its end was recorded).
bool rideDone(const slvtrack::Ride& ride);
// The last finished recording's file or folder ("" before one).
const std::string& lastOutput();

// ---- hooks ----
// LLVulkanContext, capture point 2 (the world, before the interface), SDR world frames only.
void captureWorld(VkCommandBuffer cmd, VkImage image, VkFormat format, uint32_t w, uint32_t h, VkDeviceContext& dev);
// Right after the frame's graphics submission (frameRan false: the frame was dropped).
void afterSubmit(bool frameRan);
// Before the device is destroyed (deviceLost: leave its objects).
void shutdown(bool deviceLost);
// The main loop, after the frame (the PNG path's reads, writing the file, notices). Only while busy().
void update();
} // namespace SLVideoRecorder

#endif // SL_VIDEORECORDER_H
