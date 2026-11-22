// Host test of FSR frame generation's host maths (#1544): indra/newview/vkgfx/llvkframegenfsrmath.h -- the
// constant buffers' layouts, what goes into them, the sizes of the optical flow images, every pass's
// dispatch size, which of the ping-pong images a pass binds, the order of the passes and the reset rule.
// The header is pure (no Vulkan, no viewer headers); the backend (llvkframegenfsr.cpp) records exactly what
// these functions say.
//
// THE ORACLES. Two frozen copies, statement for statement, of code this header replaces or mirrors:
//   - oracleDeviceToViewDepth: the upscaler's inline derivation (LLCleanFSR3VK::record, as of the commit
//     before llfsr::deviceToViewDepth was extracted). The extracted function must give the same bits.
//   - oracleSpdSetup: AMD's ffxSpdSetup (FidelityFX SDK v1.1.4, gpu/spd/ffx_spd.h, the FFX_CPU branch).
// Do not "fix" them: a change there is a change of what the GPU is given.
//
// PAN (the motion units). The inputs pass both upscalers run writes UV units, previous minus current.
// FSR's shaders turn the stored vector into UV with fMotionVectorScale (LoadInputMotionVector) and read the
// previous frame at uv + mv, the half-way frame at uv + mv / 2 (computeGameFieldMvs). A camera pan that
// moves the picture 10 px to the right must come out as exactly -10 render pixels and a half-way position 5 px
// to the left -- and the same vector stored in other units, or with the other sign, must NOT.
//
// Built and run by scripts/check/checks/framegen_fsr.py.
#include "llvkframegenfsrmath.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace
{
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ---- what the backend hands fillFiConstants: llfg::GenInputs' members, by name ----
enum class Units : uint8_t { UvPrevMinusCur };
struct Extent { uint32_t width = 0, height = 0; };
struct ImageRef { Extent extent; };
struct Camera
{
    float proj[16] = {};
    float jitterPx[2] = {0.f, 0.f};
    float jitterSign = 1.f;
    uint32_t renderW = 0, renderH = 0;
    Units motion = Units::UvPrevMinusCur;
};
struct Inputs
{
    ImageRef color;
    Camera camera;
    bool colorHdr = false;
    float peakNits = 0.f;
    float frameMs = 0.f;
    uint32_t debug = 0;   // (llfg::GenInputs::debug: RenderVKFrameGenFsrDebug)
};

// Reversed-Z perspective, column-major (glm's memory): depth 1 at the near plane, 0 at the far plane
// (zFar <= 0: infinite far).
void projection(float out[16], float fovY, float aspect, float zNear, float zFar, bool flipY)
{
    std::memset(out, 0, 16 * sizeof(float));
    const float f = 1.f / std::tan(fovY * 0.5f);
    out[0] = f / aspect;
    out[5] = flipY ? -f : f;
    out[11] = -1.f;
    if (zFar > 0.f)
    {
        out[10] = zNear / (zFar - zNear);
        out[14] = zNear * zFar / (zFar - zNear);
    }
    else
        out[14] = zNear;
}

// The upscaler's inline code before the extraction (LLCleanFSR3VK::record), unchanged but for the names.
void oracleDeviceToViewDepth(const float* proj, const float* invProj, float out[4])
{
    const double b = invProj[14], c = invProj[11], e = invProj[15];
    double F0 = -1e-7, F1 = 1.0;
    if (std::fabs(c) > 1e-20)
    {
        F0 = -e / c;
        F1 = b / c;
        if (std::fabs(F0) < (double)FLT_EPSILON) F0 = -(double)FLT_EPSILON;
        if (F1 / (0.5 - F0) < 0.0) F1 = -F1;
    }
    out[0] = (float)F0;
    out[1] = (float)F1;
    out[2] = std::fabs(proj[0]) > 1e-20f ? 1.f / std::fabs(proj[0]) : 1.f;
    out[3] = std::fabs(proj[5]) > 1e-20f ? 1.f / std::fabs(proj[5]) : 1.f;
}

// AMD's ffxSpdSetup (gpu/spd/ffx_spd.h, FFX_CPU), unchanged but for the types.
void oracleSpdSetup(uint32_t dispatchThreadGroupCountXY[2], uint32_t workGroupOffset[2], uint32_t numWorkGroupsAndMips[2],
                    const uint32_t rectInfo[4], int32_t mips)
{
    workGroupOffset[0] = rectInfo[0] / 64;
    workGroupOffset[1] = rectInfo[1] / 64;
    uint32_t endIndexX = (rectInfo[0] + rectInfo[2] - 1) / 64;
    uint32_t endIndexY = (rectInfo[1] + rectInfo[3] - 1) / 64;
    dispatchThreadGroupCountXY[0] = endIndexX + 1 - workGroupOffset[0];
    dispatchThreadGroupCountXY[1] = endIndexY + 1 - workGroupOffset[1];
    numWorkGroupsAndMips[0] = (dispatchThreadGroupCountXY[0]) * (dispatchThreadGroupCountXY[1]);
    if (mips >= 0)
        numWorkGroupsAndMips[1] = uint32_t(mips);
    else
    {
        uint32_t resolution = rectInfo[2] > rectInfo[3] ? rectInfo[2] : rectInfo[3];
        numWorkGroupsAndMips[1] = uint32_t(std::fmin(std::floor(std::log2(float(resolution))), float(12)));
    }
}

bool groupsAre(const llfsrfg::Groups& g, uint32_t x, uint32_t y, uint32_t z = 1)
{
    if (g.x == x && g.y == y && g.z == z) return true;
    std::printf("  groups %u x %u x %u, expected %u x %u x %u\n", g.x, g.y, g.z, x, y, z);
    return false;
}

// ---- LAYOUT: the std140 blocks of the GLSL callbacks ----
void testLayouts()
{
    using namespace llfsrfg;
    CHECK(sizeof(FiConstants) == 176);
    CHECK(offsetof(FiConstants, renderSize) == 0);
    CHECK(offsetof(FiConstants, displaySize) == 8);
    CHECK(offsetof(FiConstants, displaySizeRcp) == 16);
    CHECK(offsetof(FiConstants, cameraNear) == 24);
    CHECK(offsetof(FiConstants, cameraFar) == 28);
    CHECK(offsetof(FiConstants, upscalerTargetSize) == 32);
    CHECK(offsetof(FiConstants, mode) == 40);
    CHECK(offsetof(FiConstants, reset) == 44);
    CHECK(offsetof(FiConstants, deviceToViewDepth) == 48);
    CHECK(offsetof(FiConstants, deltaTime) == 64);
    CHECK(offsetof(FiConstants, hudLessAttachedFactor) == 68);
    CHECK(offsetof(FiConstants, distortionFieldSize) == 72);
    CHECK(offsetof(FiConstants, opticalFlowScale) == 80);
    CHECK(offsetof(FiConstants, opticalFlowBlockSize) == 88);
    CHECK(offsetof(FiConstants, dispatchFlags) == 92);
    CHECK(offsetof(FiConstants, maxRenderSize) == 96);
    CHECK(offsetof(FiConstants, opticalFlowHalfResMode) == 104);
    CHECK(offsetof(FiConstants, numInstances) == 108);
    CHECK(offsetof(FiConstants, interpolationRectBase) == 112);
    CHECK(offsetof(FiConstants, interpolationRectSize) == 120);
    CHECK(offsetof(FiConstants, debugBarColor) == 128);
    CHECK(offsetof(FiConstants, backBufferTransferFunction) == 140);
    CHECK(offsetof(FiConstants, minMaxLuminance) == 144);
    CHECK(offsetof(FiConstants, tanHalfFov) == 152);
    CHECK(offsetof(FiConstants, jitter) == 160);
    CHECK(offsetof(FiConstants, motionVectorScale) == 168);
    CHECK(sizeof(PyramidConstants) == 16);
    CHECK(offsetof(PyramidConstants, workGroupOffset) == 8);
    CHECK(sizeof(OfConstants) == 32);
    CHECK(offsetof(OfConstants, pyramidLevel) == 8);
    CHECK(offsetof(OfConstants, pyramidLevelCount) == 12);
    CHECK(offsetof(OfConstants, frameIndex) == 16);
    CHECK(offsetof(OfConstants, backbufferTransferFunction) == 20);
    CHECK(offsetof(OfConstants, minMaxLuminance) == 24);
    CHECK(sizeof(OfSpdConstants) == 32);
    CHECK(offsetof(OfSpdConstants, workGroupOffset) == 8);
    CHECK(offsetof(OfSpdConstants, numWorkGroupOpticalFlowInputPyramid) == 16);
    // (every block fits a sub-slot of the constant ring, and a frame's blocks fit a present slot's region)
    CHECK(sizeof(FiConstants) <= kCbSubSlotBytes);
    CHECK(kCbOf + kOfLevels * kCbSubSlotBytes <= kCbSlotBytes);
    CHECK(kCbSlotBytes == 12 * kCbSubSlotBytes);
    CHECK(kCbFi != kCbPyramidGame && kCbPyramidGame != kCbPyramidOutput && kCbPyramidOutput != kCbOfSpd && kCbOfSpd < kCbOf);
}

// ---- DEPTH: the extracted derivation ----
void testDeviceToViewDepth()
{
    struct Case { float fovY, aspect, zNear, zFar; bool flipY; } cases[] = {
        {1.0472f, 16.f / 9.f, 0.1f, 512.f, true},     // finite far
        {1.0472f, 16.f / 9.f, 0.05f, 0.f, true},      // infinite far
        {2.4435f, 21.f / 9.f, 0.25f, 4096.f, false},  // wide field of view
    };
    for (const Case& c : cases)
    {
        float proj[16], inv[16], got[4], want[4];
        projection(proj, c.fovY, c.aspect, c.zNear, c.zFar, c.flipY);
        CHECK(llfsr::invert4(proj, inv));
        // (the inverse is one: proj x inv = identity)
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
            {
                double s = 0.0;
                for (int k = 0; k < 4; ++k) s += (double)proj[k * 4 + row] * (double)inv[col * 4 + k];
                CHECK(near(s, col == row ? 1.0 : 0.0, 1e-4));
            }
        llfsr::deviceToViewDepth(proj, inv, got);
        oracleDeviceToViewDepth(proj, inv, want);
        CHECK(std::memcmp(got, want, sizeof(got)) == 0);
        // The shaders' ConvertFromDeviceDepthToViewSpace: Z = [1] / (depth - [0]). Positive, the near plane at
        // depth 1, and farther for a smaller depth (reversed-Z).
        const auto viewZ = [&](double d) { return (double)got[1] / (d - (double)got[0]); };
        CHECK(viewZ(0.5) > 0.0);
        CHECK(near(viewZ(1.0), c.zNear, c.zNear * 1e-3));
        CHECK(viewZ(0.25) > viewZ(0.5));
        if (c.zFar > 0.f)
            CHECK(near(viewZ(0.0), c.zFar, c.zFar * 1e-2));
        else
            CHECK(near(viewZ(0.5), 2.0 * c.zNear, c.zNear * 1e-3));
        // [2] / [3]: the tangents of half the horizontal / vertical field of view.
        CHECK(near(got[3], std::tan(c.fovY * 0.5f), 1e-4));
        CHECK(near(got[2], std::tan(c.fovY * 0.5f) * c.aspect, 1e-3));
    }
    // No usable inverse (a degenerate projection): the guarded defaults, never a division by zero.
    float zero[16] = {}, inv[16], got[4];
    CHECK(!llfsr::invert4(zero, inv));
    llfsr::deviceToViewDepth(zero, zero, got);
    CHECK(std::isfinite(got[0]) && std::isfinite(got[1]) && got[2] == 1.f && got[3] == 1.f);
}

// ---- SPD ----
void testSpdSetup()
{
    struct Case { uint32_t w, h; int32_t mips; } cases[] = {
        {2560, 1440, -1}, {3840, 2160, -1}, {1707, 960, -1}, {1280, 720, -1}, {64, 64, -1}, {65, 1, -1}, {1, 1, -1},
        {2560, 1440, 4}, {3840, 2160, 4}, {1920, 1080, 4},
    };
    for (const Case& c : cases)
    {
        uint32_t groups[2], offset[2], nm[2];
        const uint32_t rect[4] = {0, 0, c.w, c.h};
        oracleSpdSetup(groups, offset, nm, rect, c.mips);
        const llfsr::SpdSetup s = llfsr::spdSetup(c.w, c.h, c.mips);
        CHECK(s.groups[0] == groups[0] && s.groups[1] == groups[1]);
        CHECK(s.offset[0] == offset[0] && s.offset[1] == offset[1]);
        CHECK(s.numWorkGroups == nm[0]);
        CHECK(s.mips == nm[1]);
    }
    // (the numbers themselves, at the two sizes the plan names)
    const llfsr::SpdSetup a = llfsr::spdSetup(2560, 1440);
    CHECK(a.groups[0] == 40 && a.groups[1] == 23 && a.numWorkGroups == 920 && a.mips == 11);
    const llfsr::SpdSetup b = llfsr::spdSetup(3840, 2160, 4);
    CHECK(b.groups[0] == 60 && b.groups[1] == 34 && b.numWorkGroups == 2040 && b.mips == 4);
}

// ---- SIZES ----
void testSizes()
{
    using namespace llfsrfg;
    const uint32_t flow1440[7][2] = {{320, 180}, {160, 90}, {80, 45}, {40, 23}, {20, 12}, {10, 6}, {5, 3}};
    const uint32_t flow2160[7][2] = {{480, 270}, {240, 135}, {120, 68}, {60, 34}, {30, 17}, {15, 9}, {8, 5}};
    for (uint32_t l = 0; l < kOfLevels; ++l)
    {
        const Size a = ofFlowSize(2560, 1440, l), b = ofFlowSize(3840, 2160, l);
        CHECK(a.w == flow1440[l][0] && a.h == flow1440[l][1]);
        CHECK(b.w == flow2160[l][0] && b.h == flow2160[l][1]);
        const Size la = ofLumaSize(2560, 1440, l), lb = ofLumaSize(3840, 2160, l);
        CHECK(la.w == (2560u >> l) && la.h == (1440u >> l));
        CHECK(lb.w == (3840u >> l) && lb.h == (2160u >> l));
    }
    // (not a multiple of the block size: rounded up; never an empty image)
    CHECK(ofFlowSize(1921, 1081, 0).w == 241 && ofFlowSize(1921, 1081, 0).h == 136);
    CHECK(ofLumaSize(40, 20, 6).w == 1 && ofLumaSize(40, 20, 6).h == 1);
    CHECK(kOfLevels == 7 && kOfBlockSize == 8);
    CHECK(kScdHistogramWidth == 2304 && kScdOutputWidth == 3);
    // The inpainting pyramid: half the display size, the whole mip chain; the passes bind 13 mips (the last repeats).
    CHECK(pyramidSize(3840, 2160).w == 1920 && pyramidSize(3840, 2160).h == 1080);
    CHECK(pyramidMips(3840, 2160) == 11);
    CHECK(pyramidMips(2560, 1440) == 11);
    CHECK(pyramidMips(1280, 720) == 10);
    CHECK(pyramidMips(2, 2) == 1);
    CHECK(kPyramidMipBindings == 13);
}

// ---- DISPATCH: every pass's group count (the plan's table 4.2) ----
void testDispatch()
{
    using namespace llfsrfg;
    struct Frame { uint32_t rw, rh, dw, dh; };
    const Frame q1440{1707, 960, 2560, 1440};    // 1440p, Quality
    const Frame q2160{2560, 1440, 3840, 2160};   // 4K, Quality
    const auto g = [](const Frame& f, Step s, uint32_t level = 0) { return dispatchGroups(s, level, f.rw, f.rh, f.dw, f.dh); };
    // render-size passes: ceil(R / 8)
    for (Step s : {Step::ReconstructAndDilate, Step::Setup, Step::ReconstructPrevDepth, Step::GameVectorField,
                   Step::DisocclusionMask})
    {
        CHECK(groupsAre(g(q1440, s), 214, 120));
        CHECK(groupsAre(g(q2160, s), 320, 180));
    }
    // display-size passes: ceil(D / 8)
    for (Step s : {Step::Interpolate, Step::Inpainting})
    {
        CHECK(groupsAre(g(q1440, s), 320, 180));
        CHECK(groupsAre(g(q2160, s), 480, 270));
    }
    CHECK(groupsAre(g(q1440, Step::OfPrepareLuma), 80, 45));
    CHECK(groupsAre(g(q2160, Step::OfPrepareLuma), 120, 68));
    // The luminance pyramid's SPD: the display rect, FOUR mips as the SDK passes (not the six it writes).
    CHECK(groupsAre(g(q1440, Step::OfLuminancePyramid), 40, 23));
    CHECK(groupsAre(g(q2160, Step::OfLuminancePyramid), 60, 34));
    CHECK(spdFor(Step::OfLuminancePyramid, 1707, 960, 2560, 1440).mips == 4);
    CHECK(spdFor(Step::OfLuminancePyramid, 1707, 960, 2560, 1440).numWorkGroups == 920);
    CHECK(groupsAre(g(q1440, Step::OfScdHistogram), 7, 16, 9));
    CHECK(groupsAre(g(q2160, Step::OfScdHistogram), 10, 16, 9));
    CHECK(groupsAre(g(q1440, Step::OfScdDivergence), 9, 3));
    CHECK(groupsAre(g(q2160, Step::OfScdDivergence), 9, 3));
    CHECK(groupsAre(g(q1440, Step::OfSearch, 0), 160, 90));
    CHECK(groupsAre(g(q1440, Step::OfSearch, 3), 20, 12));
    CHECK(groupsAre(g(q1440, Step::OfSearch, 6), 3, 2));
    CHECK(groupsAre(g(q2160, Step::OfSearch, 0), 240, 135));
    CHECK(groupsAre(g(q2160, Step::OfSearch, 6), 4, 3));
    CHECK(groupsAre(g(q1440, Step::OfFilter, 0), 20, 45));
    CHECK(groupsAre(g(q1440, Step::OfFilter, 6), 1, 1));
    CHECK(groupsAre(g(q2160, Step::OfFilter, 0), 30, 68));
    CHECK(groupsAre(g(q2160, Step::OfFilter, 6), 1, 2));
    // scale: the NEXT (finer) level's size / 4
    CHECK(groupsAre(g(q1440, Step::OfScale, 1), 80, 45));
    CHECK(groupsAre(g(q1440, Step::OfScale, 6), 3, 2));
    CHECK(groupsAre(g(q2160, Step::OfScale, 1), 120, 68));
    // the game field's SPD: the render rect; the output's: the display rect
    CHECK(groupsAre(g(q1440, Step::GameVectorPyramid), 27, 15));
    CHECK(spdFor(Step::GameVectorPyramid, 1707, 960, 2560, 1440).mips == 10);
    CHECK(groupsAre(g(q2160, Step::GameVectorPyramid), 40, 23));
    CHECK(groupsAre(g(q1440, Step::InpaintingPyramid), 40, 23));
    CHECK(spdFor(Step::InpaintingPyramid, 1707, 960, 2560, 1440).mips == 11);
    CHECK(groupsAre(g(q2160, Step::InpaintingPyramid), 60, 34));
    // the optical flow field: uint(D / 8.0 + 7) / 8
    CHECK(groupsAre(g(q1440, Step::OpticalFlowVectorField), 40, 23));
    CHECK(groupsAre(g(q2160, Step::OpticalFlowVectorField), 60, 34));
    // (never a zero dispatch)
    CHECK(groupsAre(dispatchGroups(Step::OfScale, 6, 8, 8, 8, 8), 1, 1));
    CHECK(groupsAre(dispatchGroups(Step::OfScdHistogram, 0, 8, 8, 8, 8), 1, 16, 9));
    // Not compute passes: no groups.
    for (Step s : {Step::ClearAll, Step::ClearReconstructedPrevDepth, Step::ClearReconstructedDepthInterpolated,
                   Step::CopyPreviousSource})
        CHECK(!isCompute(s));
}

// ---- PING-PONG: the SDK's A / B selection ----
void testOfSelection()
{
    using namespace llfsrfg;
    // Even frames write input pyramid 1 (index 0) and read 2 as the previous; odd frames the reverse.
    CHECK(ofInputs(0).current == 0 && ofInputs(0).previous == 1);
    CHECK(ofInputs(1).current == 1 && ofInputs(1).previous == 0);
    CHECK(ofInputs(2).current == 0);   // (only the parity counts)
    for (uint32_t parity = 0; parity < 2; ++parity)
        for (uint32_t level = 0; level < kOfLevels; ++level)
        {
            const OfFlow f = ofFlowTargets(parity, level);
            // A = optical flow 2 when the frame's and the level's parities differ, else 1; B the other.
            const uint32_t a = ((parity != 0) != ((level & 1u) != 0)) ? 1u : 0u;
            CHECK(f.a == a && f.b == 1u - a);
            CHECK(f.filterToShared == (level == 0));
            CHECK(f.scales == (level > 0));
            // The chain: this level's scale writes B of the level below, which is that level's A (the search
            // refines it in place).
            if (level > 0)
            {
                const OfFlow below = ofFlowTargets(parity, level - 1);
                CHECK(below.a == f.b);
            }
        }
    CHECK(ofFlowTargets(0, 0).a == 0 && ofFlowTargets(0, 1).a == 1 && ofFlowTargets(1, 0).a == 1 && ofFlowTargets(1, 1).a == 0);
}

// ---- ORDER ----
void testPassPlan()
{
    using namespace llfsrfg;
    const auto stepsOf = [](const Plan& p, Stage stage)
    {
        Plan out;
        for (uint32_t i = 0; i < p.count; ++i)
            if (stageOf(p.steps[i].step) == stage) out.steps[out.count++] = p.steps[i];
        return out;
    };
    const auto is = [](const Plan& p, std::initializer_list<Step> want)
    {
        if (p.count != want.size()) { std::printf("  %u steps, expected %zu\n", p.count, want.size()); return false; }
        uint32_t i = 0;
        for (Step s : want)
            if (p.steps[i++].step != s) { std::printf("  step %u differs\n", i - 1); return false; }
        return true;
    };
    for (bool reset : {false, true})
    {
        const Plan p = passPlan(reset);
        CHECK(p.count <= kMaxPlanSteps);
        // The three stages come one after the other: prepare, optical flow, interpolation.
        uint32_t last = 0;
        for (uint32_t i = 0; i < p.count; ++i)
        {
            const uint32_t s = (uint32_t)stageOf(p.steps[i].step);
            CHECK(s >= last);
            last = s;
        }
        // A. prepare: the clear, then the pass (after everything is cleared on a reset).
        if (reset)
            CHECK(is(stepsOf(p, Stage::Prepare), {Step::ClearAll, Step::ClearReconstructedPrevDepth, Step::ReconstructAndDilate}));
        else
            CHECK(is(stepsOf(p, Stage::Prepare), {Step::ClearReconstructedPrevDepth, Step::ReconstructAndDilate}));
        // B. optical flow: luma, its pyramid, the scene change detection, then level 6 down to 0: search, filter,
        // and -- above level 0 -- scale into the next level.
        const Plan of = stepsOf(p, Stage::OpticalFlow);
        CHECK(of.count == 4 + 7 * 3 - 1);
        CHECK(of.steps[0].step == Step::OfPrepareLuma && of.steps[1].step == Step::OfLuminancePyramid);
        CHECK(of.steps[2].step == Step::OfScdHistogram && of.steps[3].step == Step::OfScdDivergence);
        uint32_t i = 4;
        for (int level = 6; level >= 0; --level)
        {
            CHECK(of.steps[i].step == Step::OfSearch && of.steps[i].level == (uint32_t)level); ++i;
            CHECK(of.steps[i].step == Step::OfFilter && of.steps[i].level == (uint32_t)level); ++i;
            if (level > 0) { CHECK(of.steps[i].step == Step::OfScale && of.steps[i].level == (uint32_t)level); ++i; }
        }
        CHECK(i == of.count);
        // C. interpolation: on a reset only setup, the interpolation, the output's pyramid, the inpainting and
        // the copy of the current picture (ffxFrameInterpolationDispatch's bExecutePreparationPasses).
        if (reset)
            CHECK(is(stepsOf(p, Stage::Interpolation), {Step::Setup, Step::Interpolate, Step::InpaintingPyramid, Step::Inpainting,
                                                        Step::CopyPreviousSource}));
        else
            CHECK(is(stepsOf(p, Stage::Interpolation), {Step::Setup, Step::ClearReconstructedDepthInterpolated,
                                                        Step::ReconstructPrevDepth, Step::GameVectorField, Step::GameVectorPyramid,
                                                        Step::OpticalFlowVectorField, Step::DisocclusionMask, Step::Interpolate,
                                                        Step::InpaintingPyramid, Step::Inpainting, Step::CopyPreviousSource}));
        CHECK(p.steps[p.count - 1].step == Step::CopyPreviousSource);
    }
    CHECK(passPlan(true).steps[0].step == Step::ClearAll);
}

// ---- RESET ----
void testResetFor()
{
    using namespace llfsrfg;
    CHECK(resetFor(false, 8, 7, false) == false);    // the next frame: history kept
    CHECK(resetFor(false, 8, 7, true) == true);      // first use
    CHECK(resetFor(true, 8, 7, false) == true);      // the caller's reset
    CHECK(resetFor(false, 9, 7, false) == true);     // a frame was skipped
    CHECK(resetFor(false, 7, 7, false) == true);     // the same frame again
    CHECK(resetFor(false, 3, 7, false) == true);     // went back
    CHECK(resetFor(false, 1, 0, false) == false);
    // serial 0: the frame loop states none -- only the caller's reset and first use decide.
    CHECK(resetFor(false, 0, 7, false) == false);
    CHECK(resetFor(false, 0, 0, false) == false);
    CHECK(resetFor(true, 0, 0, false) == true);
    // The optical flow frame index: 0 on a reset, else one more.
    CHECK(ofFrameIndex(true, 41) == 0);
    CHECK(ofFrameIndex(false, 41) == 42);
}

// ---- CONSTANTS ----
Inputs frame(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, bool hdr)
{
    Inputs in;
    projection(in.camera.proj, 1.0472f, (float)dw / (float)dh, 0.1f, 512.f, true);
    in.camera.jitterPx[0] = 0.25f;
    in.camera.jitterPx[1] = -0.375f;
    in.camera.jitterSign = -1.f;   // (DLSS's: never applied here -- FSR takes the jitter as rendered)
    in.camera.renderW = rw;
    in.camera.renderH = rh;
    in.color.extent.width = dw;
    in.color.extent.height = dh;
    in.colorHdr = hdr;
    in.peakNits = hdr ? 1000.f : 0.f;
    in.frameMs = 16.6f;
    return in;
}

void testFiConstants()
{
    using namespace llfsrfg;
    for (bool hdr : {true, false})
        for (bool reset : {false, true})
        {
            const Inputs in = frame(1707, 960, 2560, 1440, hdr);
            const FiConstants k = fillFiConstants(in, reset);
            CHECK(k.renderSize[0] == 1707 && k.renderSize[1] == 960);
            CHECK(k.maxRenderSize[0] == 1707 && k.maxRenderSize[1] == 960);
            CHECK(k.displaySize[0] == 2560 && k.displaySize[1] == 1440);
            CHECK(k.displaySizeRcp[0] == 1.f / 2560.f && k.displaySizeRcp[1] == 1.f / 1440.f);
            CHECK(k.upscalerTargetSize[0] == 2560 && k.upscalerTargetSize[1] == 1440);
            CHECK(k.interpolationRectBase[0] == 0 && k.interpolationRectBase[1] == 0);
            CHECK(k.interpolationRectSize[0] == 2560 && k.interpolationRectSize[1] == 1440);
            CHECK(k.mode == 0);
            CHECK(k.reset == (reset ? 1 : 0));
            float inv[16], d2v[4];
            CHECK(llfsr::invert4(in.camera.proj, inv));
            llfsr::deviceToViewDepth(in.camera.proj, inv, d2v);
            CHECK(std::memcmp(k.deviceToViewDepth, d2v, sizeof(d2v)) == 0);
            CHECK(near(k.cameraNear, 0.1, 1e-4));
            CHECK(near(k.cameraFar, 512.0, 5.0));
            CHECK(k.deltaTime == 16.6f);
            CHECK(k.hudLessAttachedFactor == 0);
            CHECK(k.distortionFieldSize[0] == 1 && k.distortionFieldSize[1] == 1);
            // The optical flow vectors are in display pixels.
            CHECK(k.opticalFlowScale[0] == 1.f / 2560.f && k.opticalFlowScale[1] == 1.f / 1440.f);
            CHECK(k.opticalFlowBlockSize == 8);
            CHECK(k.dispatchFlags == 0);
            // (Task 9) RenderVKFrameGenFsrDebug: bit 1 the SDK's tear lines, bit 2 its reset indicators -- the
            // shaders' own dispatch flags; nothing else reaches them (bit 4, the SDK's debug view pass, is not built)
            for (uint32_t d : {1u, 2u, 3u, 4u, 7u, 0xFFFFFFF8u})
            {
                Inputs dbg = in;
                dbg.debug = d;
                CHECK(llfsrfg::fillFiConstants(dbg, false).dispatchFlags == (d & 3u));
            }
            CHECK(k.opticalFlowHalfResMode == 0 && k.numInstances == 0);
            CHECK(k.debugBarColor[0] == 0.f && k.debugBarColor[1] == 0.f && k.debugBarColor[2] == 0.f);
            CHECK(k.backBufferTransferFunction == (hdr ? 1u : 0u));   // PQ / sRGB
            CHECK(k.minMaxLuminance[0] == 0.f);
            CHECK(k.minMaxLuminance[1] == (hdr ? 1000.f : 80.f));
            // The horizontal half field of view's tangent: 1 / |proj[0]|.
            CHECK(k.tanHalfFov == 1.f / std::fabs(in.camera.proj[0]));
            CHECK(k.tanHalfFov == k.deviceToViewDepth[2]);
            // The jitter as rendered: never the sign another generator's upscaler applied.
            CHECK(k.jitter[0] == 0.25f && k.jitter[1] == -0.375f);
            CHECK(k.motionVectorScale[0] == 1.f && k.motionVectorScale[1] == 1.f);
            // The optical flow's own blocks.
            const OfConstants of = fillOfConstants(in, 3, 42);
            CHECK(of.inputLumaResolution[0] == 2560 && of.inputLumaResolution[1] == 1440);
            CHECK(of.pyramidLevel == 3 && of.pyramidLevelCount == 7 && of.frameIndex == 42);
            CHECK(of.backbufferTransferFunction == k.backBufferTransferFunction);
            CHECK(of.minMaxLuminance[0] == k.minMaxLuminance[0] && of.minMaxLuminance[1] == k.minMaxLuminance[1]);
        }
    // The display's peak: unknown or absurd values stay inside what PQ can carry.
    Inputs in = frame(1280, 720, 2560, 1440, true);
    in.peakNits = 0.f;
    CHECK(fillFiConstants(in, false).minMaxLuminance[1] == 80.f);
    in.peakNits = 40.f;
    CHECK(fillFiConstants(in, false).minMaxLuminance[1] == 80.f);
    in.peakNits = 1.0e9f;
    CHECK(fillFiConstants(in, false).minMaxLuminance[1] == 10000.f);
    // An infinite far plane: a finite, large far distance (no shader reads it).
    projection(in.camera.proj, 1.0472f, 16.f / 9.f, 0.1f, 0.f, true);
    const llfsrfg::FiConstants inf = fillFiConstants(in, false);
    CHECK(std::isfinite(inf.cameraFar) && inf.cameraFar > 1.0e5f);
    CHECK(near(inf.cameraNear, 0.1, 1e-4));
    // The SPD blocks.
    const PyramidConstants pg = fillPyramidConstants(Step::GameVectorPyramid, 1707, 960, 2560, 1440);
    CHECK(pg.mips == 10 && pg.numWorkGroups == 27 * 15 && pg.workGroupOffset[0] == 0 && pg.workGroupOffset[1] == 0);
    const PyramidConstants po = fillPyramidConstants(Step::InpaintingPyramid, 1707, 960, 2560, 1440);
    CHECK(po.mips == 11 && po.numWorkGroups == 40 * 23);
    const OfSpdConstants os = fillOfSpdConstants(2560, 1440);
    CHECK(os.mips == 4 && os.numWorkGroups == 920 && os.numWorkGroupOpticalFlowInputPyramid == 920);
    CHECK(os.workGroupOffset[0] == 0 && os.workGroupOffset[1] == 0);
}

// ---- PAN ----
// What the shaders do with a stored motion vector (ffx_frameinterpolation_callbacks_glsl.h LoadInputMotionVector:
// stored x fMotionVectorScale = UV; ffx_frameinterpolation_game_motion_vector_field.h computeGameFieldMvs: the
// previous frame is read at uv + mv, the half-way frame is at uv + mv x 0.5). True when a static point seen at
// render pixel `px` this frame, which the camera pan moved `panPx` pixels to the right since the previous
// frame, comes out where it must: at px - panPx in the previous frame and px - panPx / 2 half-way.
bool panHolds(const float stored[2], const float scale[2], uint32_t rw, uint32_t rh, float panPx)
{
    const float px[2] = {640.5f, 360.5f};
    const float uv[2] = {px[0] / (float)rw, px[1] / (float)rh};
    const float mv[2] = {stored[0] * scale[0], stored[1] * scale[1]};
    const float prevPx[2] = {(uv[0] + mv[0]) * (float)rw, (uv[1] + mv[1]) * (float)rh};
    const float halfPx[2] = {(uv[0] + mv[0] * 0.5f) * (float)rw, (uv[1] + mv[1] * 0.5f) * (float)rh};
    return near(prevPx[0], px[0] - panPx, 1e-2) && near(prevPx[1], px[1], 1e-2) &&
           near(halfPx[0], px[0] - panPx * 0.5f, 1e-2) && near(halfPx[1], px[1], 1e-2);
}

void testPan()
{
    const uint32_t rw = 1280, rh = 720;
    const float panPx = 10.f;
    float scale[2];
    llfsrfg::motionVectorScale(Units::UvPrevMinusCur, rw, rh, scale);
    CHECK(scale[0] == 1.f && scale[1] == 1.f);
    // The inputs pass's value for a static point: prev_uv - uv = (-10 / 1280, 0).
    const float stored[2] = {-panPx / (float)rw, 0.f};
    CHECK(panHolds(stored, scale, rw, rh, panPx));
    // In FSR's own terms: exactly -10 render pixels.
    CHECK(near(stored[0] * scale[0] * (float)rw, -10.0, 1e-4));
    // MIS-STATED UNITS must fail: the same motion written in pixels, or as current minus previous, or in
    // clip units, handed over as "UV, previous minus current".
    const float pixels[2] = {-panPx, 0.f};
    const float flipped[2] = {panPx / (float)rw, 0.f};
    const float clip[2] = {-2.f * panPx / (float)rw, 0.f};
    CHECK(!panHolds(pixels, scale, rw, rh, panPx));
    CHECK(!panHolds(flipped, scale, rw, rh, panPx));
    CHECK(!panHolds(clip, scale, rw, rh, panPx));
    // ... and so must the right vector with another generator's scale (pixels of the motion image: the render size).
    const float dlss[2] = {(float)rw, (float)rh};
    CHECK(!panHolds(stored, dlss, rw, rh, panPx));
    // The constant block carries that scale, at any render size.
    Inputs in = frame(rw, rh, 2560, 1440, false);
    const llfsrfg::FiConstants k = llfsrfg::fillFiConstants(in, false);
    CHECK(panHolds(stored, k.motionVectorScale, rw, rh, panPx));
    in = frame(2560, 1440, 2560, 1440, true);
    const float stored2[2] = {-panPx / 2560.f, 0.f};
    CHECK(panHolds(stored2, llfsrfg::fillFiConstants(in, false).motionVectorScale, 2560, 1440, panPx));
}

// ---- MEMORY: the plan's section 5 ----
void testImageBytes()
{
    using namespace llfsrfg;
    struct Case { uint32_t rw, rh, dw, dh; double mb; } cases[] = {
        {3840, 2160, 3840, 2160, 360.0}, {2560, 1440, 3840, 2160, 205.0}, {1920, 1080, 3840, 2160, 150.0},
        {2560, 1440, 2560, 1440, 160.0}, {1707, 960, 2560, 1440, 90.0},   {1280, 720, 2560, 1440, 67.0},
    };
    for (const Case& c : cases)
    {
        const double mb = (double)imageBytes(c.rw, c.rh, c.dw, c.dh, 4) / 1.0e6;
        if (!near(mb, c.mb, 2.0)) std::printf("  %ux%u -> %ux%u: %.1f MB, expected about %.0f\n", c.rw, c.rh, c.dw, c.dh, mb, c.mb);
        CHECK(near(mb, c.mb, 2.0));
    }
    // 34 bytes a render pixel; the rest follows the display size only.
    const uint64_t a = imageBytes(1280, 720, 2560, 1440, 4), b = imageBytes(1920, 1080, 2560, 1440, 4);
    CHECK(b - a == 34ull * (1920ull * 1080ull - 1280ull * 720ull));
    // The previous picture is kept in the world image's format.
    CHECK(imageBytes(1280, 720, 2560, 1440, 8) - a == 4ull * 2560ull * 1440ull);
    CHECK(imageBytes(0, 0, 2560, 1440, 4) == 0 && imageBytes(1280, 720, 0, 0, 4) == 0);
}
} // namespace

int main()
{
    testLayouts();
    testDeviceToViewDepth();
    testSpdSetup();
    testSizes();
    testDispatch();
    testOfSelection();
    testPassPlan();
    testResetFor();
    testFiConstants();
    testPan();
    testImageBytes();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("ok\n");
    return 0;
}
