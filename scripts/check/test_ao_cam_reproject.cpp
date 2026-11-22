// Fixtures for llaocamreproject.h (#843 camera-only AO reprojection): built and run by
// scripts/check/checks/ao_cam_reproject.py.
#include "vkgfx/llaocamreproject.h"

#include <cstdio>

static int g_fail = 0;
#define EXPECT(cond, what)                                                        \
    do {                                                                          \
        if (!(cond)) { std::printf("FAIL: %s\n", what); ++g_fail; }              \
    } while (0)

// The owner's comparison: native, SMAA, GTAO + the accumulator, nothing else reads motion.
static llaocam::Inputs aoOnly(bool setting)
{
    llaocam::Inputs in;
    in.setting = setting;
    in.aoTemporal = true;
    return in;
}

int main()
{
    // Setting off: exactly the old rule -- the accumulator keeps the motion path.
    {
        const llaocam::Inputs in = aoOnly(false);
        EXPECT(!llaocam::cameraOnly(in), "setting off must never go camera-only");
        EXPECT(llaocam::motionPath(in), "setting off: the accumulator keeps the motion path");
        EXPECT(llaocam::cameraHistory(in, true), "setting off: the camera is tracked with the motion path");
    }
    // Setting on, the accumulator the only reader: camera-only, no motion path, camera tracked.
    {
        const llaocam::Inputs in = aoOnly(true);
        EXPECT(llaocam::cameraOnly(in), "AO the only reader: camera-only");
        EXPECT(!llaocam::motionPath(in), "AO the only reader: the motion path is dropped");
        EXPECT(llaocam::cameraHistory(in, false), "camera-only still tracks the previous camera");
    }
    // AO off or the accumulator off: nothing (neither path nor camera-only).
    {
        llaocam::Inputs in = aoOnly(true);
        in.aoTemporal = false;
        EXPECT(!llaocam::cameraOnly(in), "no accumulator: no camera-only");
        EXPECT(!llaocam::motionPath(in), "no accumulator, no reader: no motion path");
        EXPECT(!llaocam::cameraHistory(in, false), "no reader: no camera history");
    }
    // Every other reader alone keeps the full motion path, setting on or off.
    struct Reader { bool llaocam::Inputs::*flag; const char* name; bool forcing; };
    const Reader readers[] = {
        {&llaocam::Inputs::upscaler, "upscaler (FSR / DLSS / frame generation)", true},
        {&llaocam::Inputs::debugMotion, "motion-vector debug view", true},
        {&llaocam::Inputs::motionBlur, "motion blur", true},
        {&llaocam::Inputs::rtMaskTemporal, "RT shadow mask denoiser", true},
        {&llaocam::Inputs::localDenoiser, "stochastic local-light denoiser", true},
        {&llaocam::Inputs::rtMotion, "GI / ReSTIR / RTAO / RT sun / RT reflections", false},
        {&llaocam::Inputs::fxMotion, "FX motion input", false},
        {&llaocam::Inputs::liveSnapshot, "armed live snapshot", false},
    };
    for (const Reader& r : readers)
    {
        for (int setting = 0; setting < 2; ++setting)
        {
            llaocam::Inputs in = aoOnly(setting != 0);
            in.*(r.flag) = true;
            char what[256];
            std::snprintf(what, sizeof(what), "%s (setting %d): must keep the motion path", r.name, setting);
            EXPECT(!llaocam::cameraOnly(in), what);
            EXPECT(llaocam::motionPath(in), what);
            // Without the accumulator a forcing reader still wants the path; an opportunistic one does not.
            in.aoTemporal = false;
            std::snprintf(what, sizeof(what), "%s without the accumulator (setting %d)", r.name, setting);
            EXPECT(llaocam::motionPath(in) == r.forcing, what);
        }
    }
    // Stereo: the plumbing serves the per-view resolve only; never camera-only.
    {
        llaocam::Inputs in = aoOnly(true);
        in.stereo = true;
        EXPECT(!llaocam::cameraOnly(in), "stereo never goes camera-only");
        EXPECT(!llaocam::motionPath(in), "stereo without a resolve pays nothing");
        in.upscaler = true;
        EXPECT(llaocam::motionPath(in), "stereo with a resolve keeps the path");
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
