// Host test of frame generation's generator seam (#1544): indra/newview/vkgfx/llvkframegengenerator.h (the
// neutral inputs) and llvkframegendlss.cpp (DLSS Frame Generation behind LLVKFrameGenGenerator), compiled
// against a fake add-on (LLVKNeuralRecon's fg* calls are defined here) and scripts/check/stubs (logging,
// and a stand-in for glm: the host has none).
//
// THE ORACLE. LLVKFrameGenDlss::toEval must give the add-on exactly what the viewer gave it before the
// generator seam existed. `oracle` below is a frozen copy of that derivation, statement for statement:
// LLCleanDlssRRVK::frameGenConstants (with the three lines of LLCleanDlssRRVK::record that fed it: the
// evaluate's jitter, mv_scale and render size) and llvulkancontext.cpp's fillFrameGenEval, as of the commit
// before this file. Changed only where it had to compile here: the renderer's Frame and the present thread
// are small local structs with the same member names, and RenderVKFrameGenConventionDebug is an argument.
// Do not "fix" the oracle: a change of the add-on's inputs is a change to LLVKFrameGenDlss::toEval AND a
// deliberate edit here, in one commit that says so.
// Both sides use the stand-in glm, so the comparison is exact; the product build calls the real glm on the
// same bytes as before.
//
// Built and run by scripts/check/checks/framegen_generator.py.
#include "vk_common.h"

// (every standard header the viewer's headers below include, before `private` is redefined)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define private public
#include "llvkframegendlss.h"
#include "vk_device.h"
#undef private

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// ---- the fakes ----
namespace llstub
{
void logLine(const char*, const std::string&) {}
}

namespace fake
{
bool capsReported = true;
std::string capsError;
LLVKNeuralRecon::FgCaps caps;
bool entryPoints = true;
int createResult = 0, evaluateResult = 0;
std::string createError, evaluateError;
int creates = 0, releases = 0, evaluates = 0, submits = 0;
uint32_t createW = 0, createH = 0, evalIndex = 0, evalCount = 0;
VkFormat createFormat = VK_FORMAT_UNDEFINED;
LLVKNeuralRecon::FgEval lastEval;
void reset()
{
    capsReported = true; capsError.clear(); caps = LLVKNeuralRecon::FgCaps{}; entryPoints = true;
    createResult = evaluateResult = 0; createError.clear(); evaluateError.clear();
    creates = releases = evaluates = submits = 0;
    LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
    nr.mLib = &nr; nr.mTried = true; nr.mCfg.enabled = true; nr.mAvailable = true; nr.mReason.clear();
    nr.mLibVersion = "1.4.0"; nr.mFeatureDir = "/addon";
}
}

LLVKNeuralRecon& LLVKNeuralRecon::instance()
{
    static LLVKNeuralRecon s;
    return s;
}
bool LLVKNeuralRecon::fgHasEntryPoints() const { return fake::entryPoints; }
bool LLVKNeuralRecon::fgCaps(FgCaps& out, std::string* err) const
{
    out = fake::caps;
    if (err) *err = fake::capsError;
    return fake::capsReported;
}
int LLVKNeuralRecon::fgCreate(VkCommandBuffer, uint32_t w, uint32_t h, VkFormat format, std::string& err)
{
    ++fake::creates;
    fake::createW = w; fake::createH = h; fake::createFormat = format;
    err = fake::createError;
    return fake::createResult;
}
int LLVKNeuralRecon::fgEvaluate(VkCommandBuffer, const FgEval& e, uint32_t index, uint32_t count, std::string& err)
{
    ++fake::evaluates;
    fake::lastEval = e;
    fake::evalIndex = index; fake::evalCount = count;
    err = fake::evaluateError;
    return fake::evaluateResult;
}
void LLVKNeuralRecon::fgRelease() { ++fake::releases; }
void VkDeviceContext::submitImmediate(const std::function<void(VkCommandBuffer)>& record, const char*, int)
{
    ++fake::submits;
    record(VK_NULL_HANDLE);
}

// ---- the oracle (frozen: see the top of the file) ----
namespace oracle
{
struct ViewConstants   // (LLVKFrameGen::ViewConstants)
{
    float viewToClip[16], clipToView[16], clipToPrevClip[16], prevClipToClip[16];
    float camPos[3], camFwd[3], camRight[3], camUp[3];
    float jitter[2];
    float mvScale[2];
    float zNear, zFar, fovY, aspect;
    bool depthInverted, mvJittered, reset;
};
struct Want { uint32_t rw = 0, rh = 0; };
struct Frame           // (the members of LLCleanDlssRRVK::Frame the derivation read)
{
    Want w;
    const float* reproj = nullptr;
    const float* view = nullptr;
    const float* proj = nullptr;
    float jitterPx[2] = {0.f, 0.f};
    float jitterSign = 1.f;
};
struct Present         // (the members of LLVKFrameGenPresent fillFrameGenEval read)
{
    GpuImage hudless, depth, motion, scratch;
    const GpuImage& fgHudless(uint32_t) const { return hudless; }
    const GpuImage& fgDepth(uint32_t) const { return depth; }
    const GpuImage& fgMotion(uint32_t) const { return motion; }
    const GpuImage& fgScratch() const { return scratch; }
};

// LLCleanDlssRRVK::record: the evaluate's own jitter, mv_scale and render size (reset: the caller's).
void evalOf(const Frame& f, LLVKNeuralRecon::Eval& e)
{
    const Want& w = f.w;
    e.jitter[0] = f.jitterPx[0] * f.jitterSign;
    e.jitter[1] = f.jitterPx[1] * f.jitterSign;
    e.mvScale[0] = (float)w.rw;
    e.mvScale[1] = (float)w.rh;
    e.rw = w.rw; e.rh = w.rh;
}

// LLCleanDlssRRVK::frameGenConstants
ViewConstants frameGenConstants(const Frame& f, const LLVKNeuralRecon::Eval& e)
{
    ViewConstants c{};
    const glm::mat4 view = glm::make_mat4(f.view);
    const glm::mat4 proj = glm::make_mat4(f.proj);
    const glm::mat4 reproj = f.reproj ? glm::make_mat4(f.reproj) : glm::mat4(1.f);
    const glm::mat4 invProj = glm::inverse(proj);
    const glm::mat4 invReproj = glm::inverse(reproj);
    std::memcpy(c.viewToClip, glm::value_ptr(proj), sizeof(c.viewToClip));
    std::memcpy(c.clipToView, glm::value_ptr(invProj), sizeof(c.clipToView));
    std::memcpy(c.clipToPrevClip, glm::value_ptr(reproj), sizeof(c.clipToPrevClip));
    std::memcpy(c.prevClipToClip, glm::value_ptr(invReproj), sizeof(c.prevClipToClip));
    const glm::mat4 toWorld = glm::inverse(view);
    for (int i = 0; i < 3; ++i)
    {
        c.camRight[i] = toWorld[0][i];
        c.camUp[i] = toWorld[1][i];
        c.camFwd[i] = -toWorld[2][i];   // (view space looks down -Z)
        c.camPos[i] = toWorld[3][i];
    }
    c.jitter[0] = e.jitter[0];
    c.jitter[1] = e.jitter[1];
    c.mvScale[0] = e.mvScale[0];   // (the render width: UV -> motion-resource pixels)
    c.mvScale[1] = e.mvScale[1];
    // Reversed-Z perspective: depth = (A z + B) / -z with A = n / (f - n), B = n f / (f - n).
    const float a = proj[2][2], b = proj[3][2];
    c.zNear = (1.f + a) != 0.f ? std::fabs(b / (1.f + a)) : 0.1f;
    c.zFar = a > 1e-7f ? std::fabs(b / a) : 1.0e6f;
    c.fovY = proj[1][1] != 0.f ? 2.f * std::atan(1.f / std::fabs(proj[1][1])) : 1.f;
    c.aspect = proj[0][0] != 0.f ? std::fabs(proj[1][1] / proj[0][0]) : 1.f;
    c.depthInverted = true;
    c.mvJittered = false;
    c.reset = e.reset;
    return c;
}

// llvulkancontext.cpp: fillFrameGenEval (conv: RenderVKFrameGenConventionDebug)
LLVKNeuralRecon::FgEval fillFrameGenEval(const Present& f, uint32_t slot, const ViewConstants& c, bool hdr10, uint32_t conv)
{
    LLVKNeuralRecon::FgEval e;
    const GpuImage& depth = f.fgDepth(slot);
    const GpuImage& motion = f.fgMotion(slot);
    e.color = f.fgHudless(slot).image;
    e.colorView = f.fgHudless(slot).view;
    e.colorFormat = f.fgHudless(slot).format;
    e.w = f.fgHudless(slot).extent.width;
    e.h = f.fgHudless(slot).extent.height;
    e.depth = depth.image; e.depthView = depth.view; e.depthFormat = depth.format;
    e.depthW = depth.extent.width; e.depthH = depth.extent.height;
    e.motion = motion.image; e.motionView = motion.view; e.motionFormat = motion.format;
    e.motionW = motion.extent.width; e.motionH = motion.extent.height;
    e.output = f.fgScratch().image;
    e.outputView = f.fgScratch().view;
    std::memcpy(e.viewToClip, c.viewToClip, sizeof(e.viewToClip));
    std::memcpy(e.clipToView, c.clipToView, sizeof(e.clipToView));
    std::memcpy(e.clipToPrevClip, c.clipToPrevClip, sizeof(e.clipToPrevClip));
    std::memcpy(e.prevClipToClip, c.prevClipToClip, sizeof(e.prevClipToClip));
    for (int i = 0; i < 3; ++i)
    {
        e.camPos[i] = c.camPos[i]; e.camUp[i] = c.camUp[i];
        e.camRight[i] = c.camRight[i]; e.camFwd[i] = c.camFwd[i];
    }
    e.jitter[0] = c.jitter[0]; e.jitter[1] = c.jitter[1];
    e.mvScale[0] = c.mvScale[0]; e.mvScale[1] = c.mvScale[1];
    e.zNear = c.zNear; e.zFar = c.zFar; e.fovY = c.fovY; e.aspect = c.aspect;
    e.depthInverted = c.depthInverted;
    e.colorHdr = hdr10;
    e.reset = c.reset;
    if (conv & 1u)
    {
        float* const mats[4] = {e.viewToClip, e.clipToView, e.clipToPrevClip, e.prevClipToClip};
        for (float* m : mats)
            for (int r = 0; r < 4; ++r)
                for (int k = r + 1; k < 4; ++k) std::swap(m[r * 4 + k], m[k * 4 + r]);
    }
    if (conv & 2u)
    {
        e.mvScale[0] = 1.f;
        e.mvScale[1] = 1.f;
    }
    return e;
}
} // namespace oracle

// ---- the cases ----
namespace
{
template <typename T> T handle(uintptr_t v) { return (T)v; }

GpuImage image(uintptr_t id, VkFormat format, uint32_t w, uint32_t h)
{
    GpuImage im;
    im.image = handle<VkImage>(id);
    im.view = handle<VkImageView>(id + 1);
    im.format = format;
    im.extent = VkExtent2D{w, h};
    return im;
}

struct Camera
{
    float view[16], proj[16], reproj[16];
    bool haveReproj;
    float jitterPx[2], jitterSign;
    uint32_t rw, rh, dw, dh;
    VkFormat colorFormat;
    bool hdr10, upscalerReset, generatorReset;
};

// world -> view: a rotation about Y by `yaw`, then about X by `pitch`, of a camera at `pos` (column-major)
void viewMatrix(float yaw, float pitch, const float pos[3], float out[16])
{
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float r[3][3] = {{cy, 0.f, -sy}, {sy * sp, cp, cy * sp}, {sy * cp, -sp, cy * cp}};   // rows of the rotation
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) out[c * 4 + row] = c == row ? 1.f : 0.f;
    for (int row = 0; row < 3; ++row)
    {
        for (int c = 0; c < 3; ++c) out[c * 4 + row] = r[row][c];
        out[12 + row] = -(r[row][0] * pos[0] + r[row][1] * pos[1] + r[row][2] * pos[2]);
    }
}

// reversed-Z perspective, Vulkan's Y down (column-major); zFar <= 0: infinite far
void projMatrix(float fovY, float aspect, float zNear, float zFar, float out[16])
{
    const float f = 1.f / std::tan(fovY * 0.5f);
    for (int i = 0; i < 16; ++i) out[i] = 0.f;
    out[0] = f / aspect;
    out[5] = -f;
    out[10] = zFar > 0.f ? zNear / (zFar - zNear) : 0.f;
    out[14] = zFar > 0.f ? zNear * zFar / (zFar - zNear) : zNear;
    out[11] = -1.f;
}

Camera cameraA()   // HDR10, 1440p from 960p, a finite far plane, a camera step since the last frame
{
    Camera c{};
    const float pos[3] = {128.5f, 22.25f, -64.f};
    viewMatrix(0.7f, -0.2f, pos, c.view);
    projMatrix(1.0471976f, 2560.f / 1440.f, 0.1f, 4096.f, c.proj);
    for (int i = 0; i < 16; ++i) c.reproj[i] = (i % 5 == 0) ? 1.f : 0.f;
    c.reproj[1] = 0.003f; c.reproj[4] = -0.002f; c.reproj[8] = 0.011f; c.reproj[12] = 0.0125f; c.reproj[13] = -0.004f;
    c.reproj[14] = 0.0005f;
    c.haveReproj = true;
    c.jitterPx[0] = 0.25f; c.jitterPx[1] = -0.375f; c.jitterSign = 1.f;
    c.rw = 1707; c.rh = 960; c.dw = 2560; c.dh = 1440;
    c.colorFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    c.hdr10 = true; c.upscalerReset = false; c.generatorReset = false;
    return c;
}

Camera cameraB()   // SDR, 1080p from 720p, an infinite far plane, no camera history, the other jitter sign, a reset
{
    Camera c{};
    const float pos[3] = {-3.f, 1001.5f, 250.125f};
    viewMatrix(-2.4f, 0.9f, pos, c.view);
    projMatrix(1.5707964f, 1920.f / 1080.f, 0.05f, 0.f, c.proj);
    c.haveReproj = false;
    c.jitterPx[0] = -0.4375f; c.jitterPx[1] = 0.1875f; c.jitterSign = -1.f;
    c.rw = 1280; c.rh = 720; c.dw = 1920; c.dh = 1080;
    c.colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
    c.hdr10 = false; c.upscalerReset = true; c.generatorReset = true;
    return c;
}

oracle::Present presentOf(const Camera& c)
{
    oracle::Present p;
    p.hudless = image(0x1000, c.colorFormat, c.dw, c.dh);
    p.depth = image(0x2000, VK_FORMAT_R32_SFLOAT, c.rw, c.rh);
    p.motion = image(0x3000, VK_FORMAT_R16G16_SFLOAT, c.rw, c.rh);
    p.scratch = image(0x4000, c.colorFormat, c.dw, c.dh);
    return p;
}

// What the viewer built before the seam: the upscaler's evaluate -> frameGenConstants -> fillFrameGenEval,
// then the render loop's `eval.reset = frameReset(constants.reset)`.
LLVKNeuralRecon::FgEval before(const Camera& c, uint32_t conv)
{
    oracle::Frame f;
    f.w.rw = c.rw; f.w.rh = c.rh;
    f.view = c.view; f.proj = c.proj; f.reproj = c.haveReproj ? c.reproj : nullptr;
    f.jitterPx[0] = c.jitterPx[0]; f.jitterPx[1] = c.jitterPx[1]; f.jitterSign = c.jitterSign;
    LLVKNeuralRecon::Eval e;
    oracle::evalOf(f, e);
    e.reset = c.upscalerReset;
    const oracle::ViewConstants k = oracle::frameGenConstants(f, e);
    LLVKNeuralRecon::FgEval out = oracle::fillFrameGenEval(presentOf(c), 1, k, c.hdr10, conv);
    out.reset = c.generatorReset;
    return out;
}

// What the viewer builds now: the upscaler's llfg::GenCamera (LLCleanDlssRRVK::frameGenConstants), the render
// loop's llfg::GenInputs (fillGenInputs -- fillFrameGenEval before #1544 Task 6 --, frameReset, generatorDebug).
llfg::GenInputs inputsOf(const Camera& c, uint32_t conv)
{
    llfg::GenCamera k{};
    std::memcpy(k.proj, c.proj, sizeof(k.proj));
    std::memcpy(k.view, c.view, sizeof(k.view));
    if (c.haveReproj)
        std::memcpy(k.reproj, c.reproj, sizeof(k.reproj));
    else
        k.reproj[0] = k.reproj[5] = k.reproj[10] = k.reproj[15] = 1.f;
    k.jitterPx[0] = c.jitterPx[0]; k.jitterPx[1] = c.jitterPx[1];
    k.jitterSign = c.jitterSign;
    k.renderW = c.rw; k.renderH = c.rh;
    k.motion = llfg::MotionUnits::UvPrevMinusCur;
    k.depthInverted = true;
    k.reset = c.upscalerReset;
    const oracle::Present p = presentOf(c);
    llfg::GenInputs in;
    in.color = llfg::imageRef(p.fgHudless(1));
    in.depth = llfg::imageRef(p.fgDepth(1));
    in.motion = llfg::imageRef(p.fgMotion(1));
    in.output = llfg::imageRef(p.fgScratch());
    in.camera = k;
    in.colorHdr = c.hdr10;
    in.reset = c.generatorReset;
    in.debug = conv;
    return in;
}

bool same(const float* a, const float* b, size_t n) { return std::memcmp(a, b, n * sizeof(float)) == 0; }

// Field by field, bit for bit.
void compare(const LLVKNeuralRecon::FgEval& a, const LLVKNeuralRecon::FgEval& b)
{
    CHECK(a.color == b.color && a.depth == b.depth && a.motion == b.motion && a.output == b.output);
    CHECK(a.colorView == b.colorView && a.depthView == b.depthView && a.motionView == b.motionView && a.outputView == b.outputView);
    CHECK(a.colorFormat == b.colorFormat && a.depthFormat == b.depthFormat && a.motionFormat == b.motionFormat);
    CHECK(a.w == b.w && a.h == b.h && a.depthW == b.depthW && a.depthH == b.depthH && a.motionW == b.motionW && a.motionH == b.motionH);
    CHECK(same(a.viewToClip, b.viewToClip, 16));
    CHECK(same(a.clipToView, b.clipToView, 16));
    CHECK(same(a.clipToPrevClip, b.clipToPrevClip, 16));
    CHECK(same(a.prevClipToClip, b.prevClipToClip, 16));
    CHECK(same(a.camPos, b.camPos, 3));
    CHECK(same(a.camUp, b.camUp, 3));
    CHECK(same(a.camRight, b.camRight, 3));
    CHECK(same(a.camFwd, b.camFwd, 3));
    CHECK(same(a.jitter, b.jitter, 2));
    CHECK(same(a.mvScale, b.mvScale, 2));
    CHECK(same(&a.zNear, &b.zNear, 1) && same(&a.zFar, &b.zFar, 1) && same(&a.fovY, &b.fovY, 1) && same(&a.aspect, &b.aspect, 1));
    CHECK(a.depthInverted == b.depthInverted && a.colorHdr == b.colorHdr && a.reset == b.reset);
}

void testToEval()
{
    const Camera cams[2] = {cameraA(), cameraB()};
    for (const Camera& c : cams)
        for (uint32_t conv = 0; conv < 4; ++conv)
        {
            const LLVKNeuralRecon::FgEval want = before(c, conv);
            const LLVKNeuralRecon::FgEval got = LLVKFrameGenDlss::toEval(inputsOf(c, conv));
            compare(got, want);
            // (the oracle is not empty-handed: the values are the ones the add-on's guide asks for)
            CHECK(got.w == c.dw && got.h == c.dh && got.depthW == c.rw && got.motionH == c.rh);
            CHECK(got.mvScale[0] == ((conv & 2u) ? 1.f : (float)c.rw) && got.mvScale[1] == ((conv & 2u) ? 1.f : (float)c.rh));
            CHECK(got.jitter[0] == c.jitterPx[0] * c.jitterSign && got.jitter[1] == c.jitterPx[1] * c.jitterSign);
            CHECK(got.colorHdr == c.hdr10 && got.reset == c.generatorReset && got.depthInverted);
            // the projection as it is (bit 0: transposed), and its inverse really is one
            CHECK(got.viewToClip[(conv & 1u) ? 14 : 11] == -1.f && got.viewToClip[(conv & 1u) ? 11 : 14] == c.proj[14]);
            float id = 0.f;   // (row 0 of clipToView x viewToClip, whichever way round: 1 0 0 0)
            for (int k = 0; k < 4; ++k) id += got.viewToClip[k * 4] * got.clipToView[k];
            CHECK(std::fabs(id - 1.f) < 1e-4f);
            CHECK(std::fabs(got.zNear - (c.hdr10 ? 0.1f : 0.05f)) < 1e-4f);
            CHECK(c.hdr10 ? std::fabs(got.zFar - 4096.f) < 1.f : got.zFar == 1.0e6f);
        }
    // The camera's own reset is not the generator's: only GenInputs::reset reaches the add-on.
    {
        llfg::GenInputs in = inputsOf(cameraA(), 0);
        in.camera.reset = true;
        in.reset = false;
        CHECK(!LLVKFrameGenDlss::toEval(in).reset);
    }
    // The sign is the upscaler's: the same raw jitter with the other sign gives the opposite jitter.
    {
        llfg::GenInputs in = inputsOf(cameraA(), 0);
        in.camera.jitterSign = -1.f;
        const LLVKNeuralRecon::FgEval e = LLVKFrameGenDlss::toEval(in);
        CHECK(e.jitter[0] == -0.25f && e.jitter[1] == 0.375f);
    }
}

void testInit()
{
    VkDeviceContext dev;
    LLVKFrameGenDlss gen;
    CHECK(gen.kind() == llfg::Generator::Dlss);
    LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
    llfg::GeneratorCaps caps;
    std::string detail, log;
    auto init = [&]() { detail.clear(); log.clear(); return gen.init(dev, caps, detail, log); };

    fake::reset();   // the add-on never loaded: the loader's reason goes to the log only
    nr.mLib = nullptr; nr.mReason = "not found in /addon";
    CHECK(init() == llfg::Block::NoAddon && detail.empty() && log == "not found in /addon" && caps.maxGenerated == 0);
    fake::reset();   // switched off
    nr.mLib = nullptr; nr.mCfg.enabled = false;
    CHECK(init() == llfg::Block::AddonOff && detail.empty() && log.empty());
    fake::reset();   // too old
    fake::entryPoints = false;
    CHECK(init() == llfg::Block::OldAddon && detail.empty() && log.empty());
    fake::reset();   // loaded, cannot run: its reason is the player's detail
    nr.mAvailable = false; nr.mReason = "NGX did not start";
    CHECK(init() == llfg::Block::Unsupported && detail == "NGX did not start" && log.empty());
    fake::reset();   // the capability call failed
    fake::capsReported = false; fake::capsError = "caps: error 7";
    CHECK(init() == llfg::Block::CapsFailed && detail.empty() && log == "caps: error 7");
    fake::reset();   // NVIDIA's library is missing
    fake::caps.why = (uint32_t)llfg::AddonWhy::LibraryMissing;
    CHECK(init() == llfg::Block::NoFgLibrary && detail == "libnvidia-ngx-dlssg.so" && log == "looked in /addon");
    fake::reset();   // the driver is too old
    fake::caps.needsNewerDriver = true; fake::caps.minDriverMajor = 580; fake::caps.minDriverMinor = 88;
    CHECK(init() == llfg::Block::Unsupported && detail == "needs a newer driver (min 580.88)");
    fake::reset();   // it runs: the add-on's own maximum (LLVKFrameGen clamps it), the driver may pace, Reflex as the device has it
    fake::caps.available = true; fake::caps.maxGenerated = 5;
    dev.mLowLatency2 = true;
    CHECK(init() == llfg::Block::None && detail.empty() && log.empty());
    CHECK(caps.maxGenerated == 5 && caps.driverPacing && caps.reflex);
    dev.mLowLatency2 = false;
    CHECK(init() == llfg::Block::None && !caps.reflex);
}

void testFeature()
{
    VkDeviceContext dev;
    LLVKFrameGenDlss gen;
    fake::reset();
    llfg::GenSize size;
    size.displayW = 2560; size.displayH = 1440; size.renderW = 1707; size.renderH = 960;
    size.colorFormat = size.scratchFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    size.slots = 3;
    std::string err;
    CHECK(!gen.fits(size));
    gen.release(dev);                       // nothing made: nothing released
    CHECK(fake::releases == 0);
    CHECK(gen.prepare(dev, size, err) && err.empty() && fake::creates == 1 && fake::submits == 1);
    CHECK(fake::createW == 2560 && fake::createH == 1440 && fake::createFormat == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    CHECK(gen.fits(size) && gen.imageBytes(size) == 0);
    // the add-on's feature does not depend on the render size, the scratch format or the slots ...
    { llfg::GenSize s = size; s.renderW = 1280; s.renderH = 720; s.slots = 2; s.scratchFormat = VK_FORMAT_R8G8B8A8_UNORM; CHECK(gen.fits(s)); }
    // ... but on the display size and the colour format
    { llfg::GenSize s = size; s.displayW = 1920; CHECK(!gen.fits(s)); }
    { llfg::GenSize s = size; s.displayH = 1080; CHECK(!gen.fits(s)); }
    { llfg::GenSize s = size; s.colorFormat = VK_FORMAT_B8G8R8A8_UNORM; CHECK(!gen.fits(s)); }
    gen.release(dev);
    CHECK(fake::releases == 1 && !gen.fits(size));
    gen.release(dev);
    CHECK(fake::releases == 1);
    // a refused create: its text, or its result; it fits nothing, and is still released (the add-on may hold a part)
    fake::createResult = -3;
    CHECK(!gen.prepare(dev, size, err) && err == "result -3" && !gen.fits(size));
    gen.release(dev);
    CHECK(fake::releases == 2);
    fake::createError = "no memory";
    err.clear();
    CHECK(!gen.prepare(dev, size, err) && err == "no memory");
    gen.release(dev);

    // evaluate: the add-on gets toEval's inputs, the index and the count
    fake::reset();
    const llfg::GenInputs in = inputsOf(cameraA(), 0);
    err.clear();
    CHECK(gen.evaluate(VK_NULL_HANDLE, 2, in, 2, 3, err) && err.empty() && fake::evaluates == 1);
    CHECK(fake::evalIndex == 2 && fake::evalCount == 3);
    compare(fake::lastEval, before(cameraA(), 0));
    fake::evaluateResult = 5;
    CHECK(!gen.evaluate(VK_NULL_HANDLE, 0, in, 1, 1, err) && err == "result 5");
    fake::evaluateError = "device lost";
    err.clear();
    CHECK(!gen.evaluate(VK_NULL_HANDLE, 0, in, 1, 1, err) && err == "device lost");
}
} // namespace

int main()
{
    testToEval();
    testInit();
    testFeature();
    if (failures)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("frame generation generator tests pass\n");
    return 0;
}
