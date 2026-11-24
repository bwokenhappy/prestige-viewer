/**
 * @file prestigecubelut.h
 * @brief Camera filters: .cube colour lookup tables (Adobe / Resolve / Lightroom / Photoshop
 *        exports) -- parse, evaluate, and bake to the 2D strip a LUT effect samples.
 *
 * Plain C++ (no viewer types), so a small local test can build it on its own.
 *
 * FORMAT (Adobe Cube LUT Specification 1.0, plus what DaVinci Resolve writes):
 *  - '#' starts a comment (whole line or the rest of a line); blank lines; CRLF; a UTF-8 BOM.
 *  - TITLE "text"; LUT_1D_SIZE n (2..65536); LUT_3D_SIZE n (2..65 here: the strip is n*n x n
 *    and 65 is the largest size Resolve / Lightroom export); DOMAIN_MIN r g b / DOMAIN_MAX r g b
 *    (default 0 0 0 / 1 1 1); LUT_1D_INPUT_RANGE lo hi / LUT_3D_INPUT_RANGE lo hi (Resolve's
 *    form of the domain). Other keywords (LUT_IN_VIDEO_RANGE, ...) are skipped.
 *  - Then the table: one "r g b" line per entry, red changing fastest. A file with both sizes
 *    (Resolve's shaper form) holds the 1D table first; it is applied before the 3D one.
 *
 * STRIP (bakeStrip): width n*n, height n, RGBA8, rows top-down; pixel (x = b * n + r, y = g)
 * holds the table at (r, g, b) / (n - 1). That is the layout ReShade LUT shaders sample
 * (PrestigeLUT.fx: two bilinear taps in neighbouring blue slices, lerped = trilinear).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef PRESTIGE_CUBELUT_H
#define PRESTIGE_CUBELUT_H

#include <cstdint>
#include <string>
#include <vector>

namespace PrestigeCubeLut
{
constexpr int kMax3D = 65;      // largest LUT_3D_SIZE accepted
constexpr int kMax1D = 65536;   // largest LUT_1D_SIZE accepted
constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;

struct Lut
{
    std::string title;
    int size1D = 0, size3D = 0;                 // 0 = no table of that kind
    float min1D[3] = {0.f, 0.f, 0.f}, max1D[3] = {1.f, 1.f, 1.f};
    float min3D[3] = {0.f, 0.f, 0.f}, max3D[3] = {1.f, 1.f, 1.f};
    std::vector<float> table1D;                 // size1D x rgb
    std::vector<float> table3D;                 // size3D^3 x rgb, red fastest
};

// Parses the text of a .cube file. False with a message for the user ("line 12: ...").
bool parse(const std::string& text, Lut& out, std::string& error);

// The table at `in` (any values; the domain maps them, outside it they clamp): the 1D table
// (per channel, linear), then the 3D table (trilinear).
void apply(const Lut& lut, const float in[3], float out[3]);

// The strip's grid: the 3D size, or for a 1D-only table 33 (up to 33 entries) or 65.
int stripSize(const Lut& lut);

// The strip at grid size n (2..kMax3D): n*n x n RGBA8 (alpha 255), rows top-down. Values are
// clamped to 0..1. False when n is out of range or the memory is not there.
bool bakeStrip(const Lut& lut, int n, std::vector<uint8_t>& rgba);
} // namespace PrestigeCubeLut

#endif
