/**
 * @file llvkframegendlss.h
 * @brief Frame generation's DLSS generator (#1517, #1544): DLSS Frame Generation through the DLSS add-on
 *        (LLVKNeuralRecon's fg* calls), behind LLVKFrameGenGenerator. The only file that calls them, and
 *        the only place the renderer's camera becomes the add-on's (toEval).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLVKFRAMEGENDLSS_H
#define LL_LLVKFRAMEGENDLSS_H

#include "llvkframegengenerator.h"
#include "llvkneuralrecon.h"   // LLVKNeuralRecon::FgEval (no add-on ABI in it)

class LLVKFrameGenDlss final : public LLVKFrameGenGenerator
{
public:
    llfg::Generator kind() const override { return llfg::Generator::Dlss; }
    // The add-on's own block (llfg::addonBlock), then the GPU's answer (llfg::capsBlock).
    llfg::Block init(VkDeviceContext& dev, llfg::GeneratorCaps& caps, std::string& detail, std::string& log) override;
    // The add-on's feature is made for the display size and the colour format; the render size, the scratch
    // format and the slot count do not matter to it.
    bool fits(const llfg::GenSize& size) const override
    {
        return mReady && size.displayW == mW && size.displayH == mH && size.colorFormat == mFormat;
    }
    bool prepare(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err) override;
    uint64_t imageBytes(const llfg::GenSize&) const override { return 0; }   // (the add-on's own, not ours to admit)
    bool evaluate(VkCommandBuffer cmd, uint32_t slot, const llfg::GenInputs& in, uint32_t index, uint32_t count,
                  std::string& err) override;
    void release(VkDeviceContext& dev) override;

    // The add-on's evaluate inputs for a rendered frame (pure). Matrices: the DLSS-FG guide's "float[4][4] in
    // row-major order (assuming post-multiplication)" is the row-vector convention (v * M): its bytes are
    // glm's column-major, column-vector bytes as they are, so they are copied, never transposed. Motion: the
    // guide wants PIXELS of the motion resource; MotionUnits::UvPrevMinusCur times the render size is that.
    // Jitter: the camera's, with the sign the upscaler applied. in.debug is RenderVKFrameGenConventionDebug:
    // bit 0 transposes the four matrices, bit 1 gives the motion vectors a scale of 1 (both the old layout,
    // for an in-world A/B).
    static LLVKNeuralRecon::FgEval toEval(const llfg::GenInputs& in);

private:
    bool mMade = false;    // a create was recorded (released even after a refusal: the add-on may hold a part)
    bool mReady = false;   // ... and accepted
    uint32_t mW = 0, mH = 0;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
};

#endif // LL_LLVKFRAMEGENDLSS_H
