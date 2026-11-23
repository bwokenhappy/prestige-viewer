/**
 * @file slvideorecorder.cpp
 * @brief Video Studio (#1668): recording a ride to video. See slvideorecorder.h.
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

#include "llviewerprecompiledheaders.h"

#include "slvideorecorder.h"

#include "llinstantreplay.h"
#include "llinstantreplayaudio.h"

#include "lldir.h"
#include "llfile.h"
#include "llimagepng.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "vkgfx/llcleanreplayvk.h"
#include "vkgfx/llvulkancontext.h"
#include "vkgfx/vk_device.h"
#include "vk_world_shaders_spv.h"   // (generated at build time, build/newview/vkshaders)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace
{
using slvtrack::RecState;

// The replay's clock (llinstantreplayaudio.cpp uses the same: steady_clock seconds).
double now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void notify(const std::string& msg)
{
    LLSD args;
    args["MESSAGE"] = msg;
    LLNotificationsUtil::add("SystemMessageTip", args);
}

// "Slipstream <date time>" in <Videos>/Slipstream (made on demand).
std::string outputBase()
{
    const std::string dir = LLInstantReplay::videoFolder();
    LLFile::mkdir(gDirUtilp->getDirName(dir));
    LLFile::mkdir(dir);
    const std::time_t tt = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", std::localtime(&tt));
    return dir + gDirUtilp->getDirDelimiter() + "Prestige Video " + stamp;
}

// A bitrate for the picture height (H.264 Main, VBR where the driver has it).
uint32_t bitrateFor(uint32_t height)
{
    return height <= 720 ? 12000u : height <= 1080 ? 24000u : height <= 1440 ? 40000u : 60000u;
}

// ---- the PNG writer: a worker thread, a queue of at most kPngQueue frames ----
struct PngFrame
{
    std::vector<uint8_t> rgb;   // bottom-up RGB (LLImageRaw's order)
    uint32_t w = 0, h = 0;
    uint64_t index = 0;
};
class PngWriter
{
public:
    explicit PngWriter(std::string folder) : mFolder(std::move(folder))
    {
        mThread = std::thread([this] { run(); });
    }
    ~PngWriter() { finish(); }
    bool hasRoom()
    {
        std::lock_guard<std::mutex> lk(mMutex);
        return mQueue.size() < SLVideoRecorder::kPngQueue;
    }
    void push(PngFrame&& f)
    {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mQueue.push_back(std::move(f));
        }
        mCv.notify_one();
    }
    // Waits for the queue to drain and the thread to end.
    void finish()
    {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mDone = true;
        }
        mCv.notify_one();
        if (mThread.joinable()) mThread.join();
    }
    bool idle()
    {
        std::lock_guard<std::mutex> lk(mMutex);
        return mQueue.empty() && !mBusy;
    }
    // A write failed (disk full, out of memory): the recording stops. Once.
    bool takeError(std::string& msg)
    {
        std::lock_guard<std::mutex> lk(mMutex);
        if (mError.empty()) return false;
        msg.swap(mError);
        mError.clear();
        mFailed = true;
        return true;
    }
    uint64_t written() const { return mWritten.load(); }
    const std::string& folder() const { return mFolder; }

private:
    void run()
    {
        for (;;)
        {
            PngFrame f;
            {
                std::unique_lock<std::mutex> lk(mMutex);
                mCv.wait(lk, [this] { return mDone || !mQueue.empty(); });
                if (mQueue.empty()) return;   // done and drained
                f = std::move(mQueue.front());
                mQueue.pop_front();
                mBusy = true;
            }
            std::string err;
            if (!mFailed) write(f, err);
            std::lock_guard<std::mutex> lk(mMutex);
            mBusy = false;
            if (!err.empty() && mError.empty() && !mFailed) mError = err;
        }
    }
    void write(const PngFrame& f, std::string& err)
    {
        try
        {
            LLPointer<LLImageRaw> raw = new LLImageRaw((U16)f.w, (U16)f.h, 3);
            if (!raw->getData() || f.rgb.size() < (size_t)f.w * f.h * 3)
            {
                err = "out of memory for a frame";
                return;
            }
            memcpy(raw->getData(), f.rgb.data(), (size_t)f.w * f.h * 3);
            LLPointer<LLImagePNG> png = new LLImagePNG();
            char name[32];
            snprintf(name, sizeof(name), "frame_%06llu.png", (unsigned long long)f.index);
            const std::string path = mFolder + gDirUtilp->getDirDelimiter() + name;
            if (!png->encode(raw, 0.f) || !png->save(path))
            {
                err = "couldn't write " + path;
                return;
            }
            ++mWritten;
        }
        catch (const std::bad_alloc&)
        {
            err = "out of memory writing a frame";
        }
    }

    std::string mFolder;
    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::deque<PngFrame> mQueue;
    bool mDone = false, mBusy = false, mFailed = false;
    std::string mError;
    std::atomic<uint64_t> mWritten{ 0 };
};

struct Recorder
{
    slvtrack::RecClock clock;
    SLVideoRecorder::Options opts;
    SLVideoRecorder::Path path = SLVideoRecorder::Path::H264;
    std::string base;        // the output (without extension / the folder)
    std::string lastOutput;
    double stoppedAt = 0.0;

    // H.264
    std::unique_ptr<LLReplayEncoder> enc;
    std::unique_ptr<LLInstantReplayAudio> audio;
    bool encFailed = false;
    bool saveStarted = false;

    // PNG
    std::unique_ptr<PngWriter> png;
    bool armed = false;      // a live snapshot is armed for the next frame
    uint32_t allocFails = 0;
    uint64_t nextIndex = 0;

    std::string stopReason;  // why it stopped by itself (said once it is written)
};
Recorder sRec;

LLReplayEncoder::Config encoderConfig()
{
    LLReplayEncoder::Config c;
    c.maxHeight = sRec.opts.height;
    c.fps = sRec.opts.fps;
    c.bitrateKbps = bitrateFor(sRec.opts.height);
    c.seconds = SLVideoRecorder::kMaxSeconds;
    c.budgetBytes = SLVideoRecorder::kBudgetBytes;
    c.record = true;
    return c;
}

// The recording ends by itself (the ride is over, it is full, or something failed).
void stopWith(const std::string& reason)
{
    if (!sRec.clock.running()) return;
    if (!reason.empty() && sRec.stopReason.empty()) sRec.stopReason = reason;
    SLVideoRecorder::stop();
}

void releaseEncoder(bool deviceLost)
{
    sRec.audio.reset();
    if (sRec.enc)
    {
        if (deviceLost) sRec.enc->abandon();
        else sRec.enc->shutdown();   // waits for its queues (once, after the file is written)
        sRec.enc.reset();
    }
}

void finishAll()
{
    sRec.clock.finished();
    sRec.saveStarted = false;
    sRec.armed = false;
    sRec.stopReason.clear();
}

// ---- the PNG path, once per main loop iteration ----
void updatePng()
{
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (!sRec.png) return;
    std::string err;
    if (sRec.png->takeError(err))
    {
        LL_WARNS("VideoStudio") << "image sequence: " << err << LL_ENDL;
        stopWith(err);
    }
    if (!sRec.clock.running()) return;
    if (sRec.armed && vk.liveSnapshotFinished())
    {
        if (!sRec.png->hasRoom()) return;   // the writer is behind: this frame waits (fixed step)
        PngFrame f;
        std::string why;
        bool ok = false;
        try
        {
            ok = vk.readLiveSnapshot(f.rgb, f.w, f.h, why) && f.w && f.h;
        }
        catch (const std::bad_alloc&)
        {
            why = "out of memory";
        }
        sRec.armed = false;
        if (!ok)
        {
            // A failed read (out of memory, a lost frame): the same ride time is shot again; after
            // kAllocRetries in a row the recording stops with what it has.
            if (++sRec.allocFails > SLVideoRecorder::kAllocRetries)
            {
                LL_WARNS("VideoStudio") << "image sequence: a frame could not be read (" << why << "); stopping" << LL_ENDL;
                stopWith("a frame could not be read (" + why + ")");
                return;
            }
        }
        else
        {
            sRec.allocFails = 0;
            f.index = ++sRec.nextIndex;
            sRec.png->push(std::move(f));
            sRec.clock.captured(now());
            sRec.clock.submitted(true);
            if (sRec.clock.videoSeconds(now()) >= SLVideoRecorder::kMaxSeconds)
            {
                stopWith("the longest recording is " + std::to_string(SLVideoRecorder::kMaxSeconds) + " s");
                return;
            }
        }
    }
    if (!sRec.armed)
    {
        // The next frame is the shot: the world only (point 1), resampled to the chosen height.
        uint32_t ww = 0, wh = 0;
        if (!vk.liveFrameInfo(ww, wh) || !ww || !wh) return;   // minimised: wait
        const uint32_t h = std::min<uint32_t>(sRec.opts.height, wh) & ~1u;
        const uint32_t w = (uint32_t)((uint64_t)ww * h / wh) & ~1u;
        LLVulkanContext::LiveSnapshot req;
        req.point = 1;
        req.mode = 0;
        req.outW = std::max(16u, w);
        req.outH = std::max(16u, h);
        std::string why;
        if (vk.beginLiveSnapshot(req, why))
        {
            sRec.armed = true;
        }
        else if (++sRec.allocFails > SLVideoRecorder::kAllocRetries)
        {
            LL_WARNS("VideoStudio") << "image sequence: no room for a frame (" << why << "); stopping" << LL_ENDL;
            stopWith("no room for a frame (" + why + ")");
        }
    }
}

// Writing the file / folder, then idle.
void updateFinishing()
{
    if (sRec.path == SLVideoRecorder::Path::Png)
    {
        if (sRec.armed)
        {
            LLVulkanContext::instance().cancelLiveSnapshot();
            sRec.armed = false;
        }
        if (sRec.png && !sRec.png->idle()) return;   // still writing
        const uint64_t n = sRec.png ? sRec.png->written() : 0;
        std::string err;
        if (sRec.png) sRec.png->takeError(err);
        if (sRec.png) sRec.png->finish();
        const std::string folder = sRec.png ? sRec.png->folder() : std::string();
        sRec.png.reset();
        if (n)
        {
            sRec.lastOutput = folder;
            LL_INFOS("VideoStudio") << "image sequence saved: " << n << " frames in " << folder << LL_ENDL;
            notify("Video Studio: " + std::to_string(n) + " frames saved to " + folder
                   + (sRec.stopReason.empty() ? std::string() : " (stopped: " + sRec.stopReason + ")"));
        }
        else
        {
            LLFile::rmdir(folder);
            notify("Video Studio: nothing was recorded" + (err.empty() ? std::string() : " (" + err + ")"));
        }
        finishAll();
        return;
    }
    // H.264: the MP4 on the encoder's worker thread, then the encoder goes.
    if (!sRec.enc || !sRec.enc->ready())
    {
        notify("Video Studio: nothing was recorded" + (sRec.stopReason.empty() ? std::string() : " (" + sRec.stopReason + ")"));
        releaseEncoder(false);
        finishAll();
        return;
    }
    if (!sRec.saveStarted)
    {
        const double t = now();
        // The frames still in the encoder when it stopped (a few slots) finish first.
        if (t - sRec.stoppedAt < 0.25)
        {
            sRec.enc->poll(t);
            return;
        }
        sRec.enc->poll(t);
        const double seconds = sRec.enc->bufferedSeconds() + 2.0;
        LLReplayAudioClip clip;
        if (sRec.audio) clip = sRec.audio->clip(t, (uint32_t)seconds + 1);
        if (!sRec.enc->saveAsync(sRec.base + ".mp4", 1000000u, t, std::move(clip)))
        {
            if (!sRec.enc->saving())
            {
                notify("Video Studio: nothing was recorded" + (sRec.stopReason.empty() ? std::string() : " (" + sRec.stopReason + ")"));
                releaseEncoder(false);
                finishAll();
            }
            return;
        }
        sRec.saveStarted = true;
        sRec.audio.reset();
        LL_INFOS("VideoStudio") << "saving " << sRec.base << ".mp4: " << sRec.enc->describe() << LL_ENDL;
        return;
    }
    bool ok = false;
    std::string msg;
    if (!sRec.enc->takeSaveResult(ok, msg)) return;   // still writing
    LL_INFOS("VideoStudio") << (ok ? "saved " : "save failed: ") << msg << LL_ENDL;
    if (ok) sRec.lastOutput = sRec.base + ".mp4";
    notify(ok ? "Video Studio: video saved: " + msg
                    + (sRec.stopReason.empty() ? std::string() : " (stopped: " + sRec.stopReason + ")")
              : "Video Studio: the video wasn't saved: " + msg);
    releaseEncoder(false);
    finishAll();
}
} // namespace

SLVideoRecorder::Path SLVideoRecorder::pathFor()
{
    static LLCachedControl<bool> s_force_png(gSavedSettings, "SLVideoStudioForcePNG", false);
    if (s_force_png) return Path::Png;
    LLVulkanContext& vk = LLVulkanContext::instance();
    if (!vk.isInitialized() || vk.hdrDisplayActive() || !LLInstantReplay::encoderAvailable()) return Path::Png;
    return Path::H264;
}

bool SLVideoRecorder::start(const Options& options, std::string& why)
{
    if (busy())
    {
        why = "a recording is still being saved";
        return false;
    }
    if (!LLVulkanContext::instance().isInitialized())
    {
        why = "the renderer isn't running";
        return false;
    }
    sRec.opts = options;
    sRec.opts.height = std::clamp<uint32_t>(options.height, 144u, 4320u);
    sRec.opts.fps = std::clamp<uint32_t>(options.fps, 10u, 120u);
    sRec.path = pathFor();
    const bool fixed = sRec.path == Path::Png ? true : options.fixedStep;
    sRec.base = outputBase();
    sRec.stopReason.clear();
    sRec.saveStarted = false;
    sRec.armed = false;
    sRec.allocFails = 0;
    sRec.nextIndex = 0;
    sRec.encFailed = false;
    if (sRec.path == Path::Png)
    {
        LLFile::mkdir(sRec.base);
        if (!LLFile::isdir(sRec.base))
        {
            why = "couldn't make the folder " + sRec.base;
            return false;
        }
        try
        {
            sRec.png = std::make_unique<PngWriter>(sRec.base);
        }
        catch (const std::exception& e)   // bad_alloc, or no thread
        {
            why = std::string("couldn't start the image writer: ") + e.what();
            return false;
        }
    }
    else if (!fixed && options.sound)
    {
        try
        {
            sRec.audio = std::make_unique<LLInstantReplayAudio>();
            sRec.audio->setKeepSeconds(kMaxSeconds + 5);
            sRec.audio->start();
        }
        catch (const std::bad_alloc&)
        {
            sRec.audio.reset();   // the video without its sound
            LL_WARNS("VideoStudio") << "no memory for the sound recorder; recording without sound" << LL_ENDL;
        }
    }
    sRec.clock.start(fixed, (double)sRec.opts.fps);
    LL_INFOS("VideoStudio") << "recording: " << (sRec.path == Path::Png ? "PNG sequence" : "H.264") << " " << sRec.opts.height << "p "
                            << sRec.opts.fps << " fps, " << (fixed ? "fixed step" : "real time")
                            << (sRec.audio ? ", with sound" : "") << " -> " << sRec.base << LL_ENDL;
    return true;
}

void SLVideoRecorder::stop()
{
    if (!sRec.clock.running()) return;
    sRec.clock.stop();
    sRec.stoppedAt = now();
    LL_INFOS("VideoStudio") << "recording stopped: " << sRec.clock.frames << " frames"
                            << (sRec.stopReason.empty() ? std::string() : " (" + sRec.stopReason + ")") << LL_ENDL;
}

bool SLVideoRecorder::recording()
{
    return sRec.clock.running();
}

bool SLVideoRecorder::busy()
{
    return sRec.clock.state != RecState::Idle;
}

slvtrack::RecState SLVideoRecorder::state()
{
    return sRec.clock.state;
}

double SLVideoRecorder::rideTime()
{
    return sRec.clock.rideTime(now());
}

double SLVideoRecorder::videoSeconds()
{
    return sRec.clock.videoSeconds(sRec.clock.state == RecState::Finishing ? sRec.stoppedAt : now());
}

std::string SLVideoRecorder::status()
{
    switch (sRec.clock.state)
    {
    case RecState::Idle: return std::string();
    case RecState::Preroll: return "Starting...";
    case RecState::Recording:
        return llformat("REC %.1f s%s", videoSeconds(), sRec.clock.fixedStep ? " (fixed step)" : "");
    case RecState::Finishing: return "Saving...";
    }
    return std::string();
}

bool SLVideoRecorder::rideDone(const slvtrack::Ride& ride)
{
    return slvtrack::rideRecorded(sRec.clock, ride, now());
}

const std::string& SLVideoRecorder::lastOutput()
{
    return sRec.lastOutput;
}

void SLVideoRecorder::captureWorld(VkCommandBuffer cmd, VkImage image, VkFormat format, uint32_t w, uint32_t h, VkDeviceContext& dev)
{
    if (!sRec.clock.running() || sRec.path != Path::H264) return;
    if (!sRec.enc)
    {
        if (sRec.encFailed) return;
        if (!dev.encodeQueue())
        {
            sRec.encFailed = true;
            stopWith("this GPU / driver can't encode H.264");
            return;
        }
        LLReplayEncoder::Device d;
        d.instance = dev.instance();
        d.physical = dev.physicalDevice();
        d.device = dev.device();
        d.gfxFamily = dev.queueFamily();
        d.gfxQueue = dev.queue();
        d.encodeFamily = dev.encodeFamily();
        d.encodeQueue = dev.encodeQueue();
        // (#1544: the encoder's submits through the viewer's own -- the queue lock, the device-lost check)
        d.submit2 = [](VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) { return vkQueueSubmit2(q, n, s, f); };
        std::string err;
        try
        {
            sRec.enc = std::make_unique<LLReplayEncoder>();
            if (!sRec.enc->init(d, kWorldReplayNv12Spv, kWorldReplayNv12Spv_bytes, err))
            {
                sRec.enc.reset();
                sRec.encFailed = true;
            }
        }
        catch (const std::bad_alloc&)
        {
            sRec.enc.reset();
            sRec.encFailed = true;
            err = "out of memory";
        }
        if (!sRec.enc)
        {
            LL_WARNS("VideoStudio") << "encoder init failed: " << err << LL_ENDL;
            stopWith("the encoder couldn't start: " + err);
            return;
        }
    }
    if (!sRec.enc->ready()) return;
    const double t = now();
    if (sRec.enc->recordCapture(cmd, image, format, w, h, sRec.clock.encoderClock(t), encoderConfig()))
        sRec.clock.captured(t);
}

void SLVideoRecorder::afterSubmit(bool frameRan)
{
    if (!sRec.enc || !sRec.enc->ready())
    {
        sRec.clock.submitted(false);
        return;
    }
    if (frameRan) sRec.enc->submit();
    else sRec.enc->cancel();
    sRec.enc->poll(sRec.clock.encoderClock(now()));
    sRec.clock.submitted(frameRan);
    std::string err;
    if (sRec.enc->takeError(err)) LL_WARNS("VideoStudio") << "encoder: " << err << LL_ENDL;
    if (sRec.clock.running() && sRec.enc->full())
        stopWith(llformat("the recording reached its limit (%u s or %llu MB)", kMaxSeconds,
                          (unsigned long long)(kBudgetBytes >> 20)));
}

void SLVideoRecorder::shutdown(bool deviceLost)
{
    sRec.armed = false;   // (the context is going away: its live snapshot goes with it, no call back into it)
    if (sRec.clock.running() || sRec.clock.state == RecState::Finishing)
        LL_WARNS("VideoStudio") << "renderer going away: the recording ends" << (deviceLost ? " (device lost)" : "") << LL_ENDL;
    // A recording in flight is written with what it has when the device survives (the encoder's
    // shutdown joins a save in progress); a lost device discards it (as instant replay, #1547).
    if (sRec.enc && !deviceLost && sRec.enc->ready() && !sRec.saveStarted && sRec.clock.state != RecState::Idle)
    {
        const double t = now();
        sRec.enc->poll(t);
        sRec.enc->saveAsync(sRec.base + ".mp4", 1000000u, t, sRec.audio ? sRec.audio->clip(t, kMaxSeconds + 5) : LLReplayAudioClip());
    }
    releaseEncoder(deviceLost);
    if (sRec.png) sRec.png->finish();
    sRec.png.reset();
    finishAll();
}

void SLVideoRecorder::update()
{
    if (sRec.clock.state == RecState::Idle) return;
    if (sRec.clock.running() && sRec.path == Path::Png) updatePng();
    if (sRec.clock.state == RecState::Finishing) updateFinishing();
}
