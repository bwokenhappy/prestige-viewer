// Tests for indra/newview/vkgfx/llupscalermode.h: the upscaler mode the renderer runs (FSR 3.1 / DLSS are the
// only temporal anti-aliasing) and the one-time move of TAA users to it.
// Built and run by scripts/check/checks/taa_removed.py.
#include "vkgfx/llupscalermode.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    // effectiveMode(upscaler, aaType, fsrUsable, dlssAvailable)
    CHECK(llups::effectiveMode(0, 0, true, false) == 0);   // off stays off
    CHECK(llups::effectiveMode(0, 2, true, false) == 0);   // SMAA: no temporal
    CHECK(llups::effectiveMode(0, 3, true, false) == 1);   // a late legacy TAA value = FSR Native AA
    CHECK(llups::effectiveMode(0, 3, false, false) == 0);  // ... unless FSR can't run
    CHECK(llups::effectiveMode(4, 0, true, false) == 4);
    CHECK(llups::effectiveMode(4, 0, false, true) == 0);   // FSR preset, FSR unusable -> native, no temporal
    CHECK(llups::effectiveMode(6, 0, false, false) == 0);
    CHECK(llups::effectiveMode(8, 0, false, true) == 8);   // DLSS available: kept even without FSR
    CHECK(llups::effectiveMode(8, 0, true, false) == 8);   // DLSS missing: kept (FSR runs the matching preset)
    CHECK(llups::effectiveMode(8, 0, false, false) == 0);  // neither
    CHECK(llups::effectiveMode(12, 0, true, true) == 0);   // out of range = off
    CHECK(llups::effectiveMode(2, 3, true, false) == 2);   // an upscaler already chosen wins over the legacy AA value

    // migrate(upscaler, aaType, taaDebug, dlssAvailable)
    llups::Migration m = llups::migrate(0, 3, false, true);
    CHECK(m.changed && m.upscaler == 7 && m.aaType == 2);
    m = llups::migrate(0, 3, false, false);
    CHECK(m.changed && m.upscaler == 1 && m.aaType == 2);
    m = llups::migrate(4, 3, false, true);
    CHECK(m.changed && m.upscaler == 4 && m.aaType == 2);   // keeps their upscaler
    m = llups::migrate(0, 1, true, false);
    CHECK(m.changed && m.upscaler == 1 && m.aaType == 1);   // RenderVKTAA debug on, FXAA kept
    m = llups::migrate(0, 2, false, true);
    CHECK(!m.changed && m.upscaler == 0 && m.aaType == 2);
    m = llups::migrate(5, 0, false, false);
    CHECK(!m.changed && m.upscaler == 5 && m.aaType == 0);

    // temporalRuns(mode, fsrUsable, dlssThisFrame): jitter + a temporal resolve this frame (review I1)
    CHECK(!llups::temporalRuns(0, true, true));            // off
    CHECK(llups::temporalRuns(1, true, false));            // FSR Native AA
    CHECK(!llups::temporalRuns(4, false, false));          // (effectiveMode makes this 0 anyway)
    CHECK(llups::temporalRuns(8, false, true));            // DLSS runs this frame, FSR unusable
    CHECK(llups::temporalRuns(8, true, false));            // DLSS stood down: FSR runs the tier
    CHECK(!llups::temporalRuns(8, false, false));          // DLSS stood down, FSR unusable: native, no jitter

    // fsrTier(mode, fsrUsable): the FSR preset a frame renders at when DLSS does not run (0 = native)
    CHECK(llups::fsrTier(0, true) == 0);
    CHECK(llups::fsrTier(3, true) == 3);
    CHECK(llups::fsrTier(6, true) == 6);
    CHECK(llups::fsrTier(8, true) == 2);                   // DLSS Quality -> FSR Quality
    CHECK(llups::fsrTier(11, true) == 5);
    CHECK(llups::fsrTier(8, false) == 0);                  // nothing to upscale with: native
    CHECK(llups::fsrTier(4, false) == 0);

    // postAaType(aaType): the FXAA / SMAA pass on a frame without a temporal resolve -- a legacy 3 (TAA)
    // that could not run as FSR gets SMAA, never no anti-aliasing (review minor 1)
    CHECK(llups::postAaType(0) == 0);
    CHECK(llups::postAaType(1) == 1);
    CHECK(llups::postAaType(2) == 2);
    CHECK(llups::postAaType(3) == 2);
    CHECK(llups::postAaType(7) == 0);

    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
