/*
 * PrestigeLegacy.fxh -- the classic snapshot filters (app_settings/filters/*.xml, run on the CPU
 * by LLImageFilter) as ReShade FX, so camera mode's viewfinder shows what the photo gets.
 *
 * The operations follow llimage/llimagefilter.cpp on gamma-encoded 0..1 values (the 8-bit
 * image LLImageFilter works on): linearize (auto-levels from the brightness histogram),
 * grayscale, sepia, saturate, contrast, brighten / darken, gamma, colorize, screen (halftone /
 * lines), 3x3 blur and sharpen, and the stencil (uniform, gradient, vignette, scan lines) with
 * its blend modes (blend, add, add_back, fade). Geometry is in units of the image height, as
 * there, so a filter covers the same part of the picture at any size; the 3x3 kernels step
 * in 1080p pixels (BUFFER_HEIGHT / 1080), so the photo at 8K matches the viewfinder.
 *
 * Coordinates: LLImageFilter's rows run bottom-up (j = 0 is the bottom row).
 *
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#pragma once

#include "ReShade.fxh"

#ifndef PRESTIGE_SCRGB_WHITE
	// scRGB (BUFFER_COLOR_SPACE 2: HDR display, linear, 1 = 80 nits): SDR white at 203 nits.
	#define PRESTIGE_SCRGB_WHITE (203.0 / 80.0)
#endif

namespace PrestigeLegacy
{
	static const int SHAPE_UNIFORM = 0;
	static const int SHAPE_GRADIENT = 1;
	static const int SHAPE_VIGNETTE = 2;
	static const int SHAPE_SCANLINES = 3;
	static const int MODE_BLEND = 0;
	static const int MODE_ADD = 1;
	static const int MODE_ADD_BACK = 2;
	static const int MODE_FADE = 3;
	static const float PI = 3.14159265;

	// ---- the back buffer as LLImageFilter sees it (gamma-encoded, 0..1) and back ----
	float3 SrgbEncode(float3 l)
	{
		l = max(l, 0.0);
		return lerp(1.055 * pow(l, 1.0 / 2.4) - 0.055, l * 12.92, step(l, 0.0031308));
	}
	float3 SrgbDecode(float3 e)
	{
		e = max(e, 0.0);
		return lerp(pow((e + 0.055) / 1.055, 2.4), e / 12.92, step(e, 0.04045));
	}
	float3 ToFilter(float3 c)
	{
#if BUFFER_COLOR_SPACE == 2
		return SrgbEncode(saturate(c / PRESTIGE_SCRGB_WHITE));
#else
		return saturate(c);
#endif
	}
	// `orig`: the back buffer's value before; HDR highlights above SDR white are kept.
	float3 FromFilter(float3 e, float3 orig)
	{
#if BUFFER_COLOR_SPACE == 2
		const float3 sdr = saturate(orig / PRESTIGE_SCRGB_WHITE) * PRESTIGE_SCRGB_WHITE;
		return SrgbDecode(saturate(e)) * PRESTIGE_SCRGB_WHITE + (orig - sdr);
#else
		return saturate(e);
#endif
	}

	// ---- the stencil: how much of an operation's result lands on this pixel ----
	// p: the stencil's four parameters, as in the filter files.
	float StencilAlpha(int shape, float mn, float mx, float4 p, float2 uv)
	{
		const float W = BUFFER_WIDTH, H = BUFFER_HEIGHT;
		const float i = uv.x * W, j = (1.0 - uv.y) * H;
		float a = 1.0;
		if (shape == SHAPE_VIGNETTE)
		{
			const float2 c = float2((W + p.x * H) * 0.5, (H + p.y * H) * 0.5);
			const float w = max(p.z * H * 0.5, 1e-3);
			const float g = p.w <= 0.0 ? 1.0 : p.w;
			const float2 d = float2(i, j) - c;
			a = exp(-pow(dot(d, d) / (w * w), g) * 0.5);
		}
		else if (shape == SHAPE_SCANLINES)
		{
			const float wl = p.x <= 0.0 ? 10.0 : p.x * H * 0.5;
			const float d = sin(radians(p.y)) * i - cos(radians(p.y)) * j;
			a = sin(2.0 * PI * d / wl) > 0.0 ? 1.0 : 0.0;
		}
		else if (shape == SHAPE_GRADIENT)
		{
			const float2 s = float2((W + p.x * H) * 0.5, (H + p.y * H) * 0.5);
			const float2 e = float2((W + p.z * H) * 0.5, (H + p.w * H) * 0.5);
			const float2 g = e - s;
			a = saturate(dot(float2(i, j) - s, g) / max(dot(g, g), 1e-6));
		}
		mn = clamp(mn, -1.0, 1.0);
		mx = clamp(mx, -1.0, 1.0);
		return mn + a * (mx - mn);
	}
	// An operation's result `r` over the pixel `c`, by the stencil's alpha and blend mode.
	float3 Blend(int mode, float a, float3 c, float3 r)
	{
		if (mode == MODE_ADD) return saturate(c + a * r);
		if (mode == MODE_ADD_BACK) return saturate((1.0 - a) * c + r);
		if (mode == MODE_FADE) return saturate(a * r);
		return lerp(c, r, a);
	}

	// ---- per-pixel operations (results before the stencil) ----
	float3 Grayscale(float3 c) { return saturate(dot(c, float3(0.2125, 0.7154, 0.0721))).xxx; }
	float3 Sepia(float3 c)
	{
		return saturate(float3(dot(c, float3(0.3588, 0.7044, 0.1368)), dot(c, float3(0.2990, 0.5870, 0.1140)),
		                       dot(c, float3(0.2392, 0.4696, 0.0912))));
	}
	// LLImageFilter::filterSaturate: chroma scaled about the grey axis.
	float3 Saturate(float3 c, float s)
	{
		const float m = (c.r + c.g + c.b) / 3.0;
		return saturate(m + s * (c - m));
	}
	float3 Contrast(float3 c, float slope, float3 alpha) { return lerp(c, saturate(slope * c + 0.5 * (1.0 - slope)), alpha); }
	float3 Brighten(float3 c, float add, float3 alpha) { return lerp(c, saturate(c + add), alpha); }
	float3 Gamma(float3 c, float g, float3 alpha) { return lerp(c, saturate(pow(max(c, 0.0), 1.0 / max(g, 1e-3))), alpha); }
	float3 Colorize(float3 c, float3 color, float3 alpha) { return saturate((1.0 - alpha) * c + alpha * color); }
	// Halftone (mode 0, "2Dsine") or lines (mode 1, "line"): grey from the red channel.
	float3 Screen(float3 c, int mode, float wavelength, float angle, float2 uv)
	{
		const float W = BUFFER_WIDTH, H = BUFFER_HEIGHT;
		const float i = uv.x * W, j = (1.0 - uv.y) * H;
		const float wl = max(wavelength * H * 0.5, 1e-3);
		const float sn = sin(radians(angle)), cs = cos(radians(angle));
		float v;
		if (mode == 0)
			v = (sin(2.0 * PI * (cs * i + sn * j) / wl) * sin(2.0 * PI * (-sn * i + cs * j) / wl) + 1.0) * 0.5;
		else
			v = (sin(2.0 * PI * (sn * i - cs * j) / wl) + 1.0) * 0.5;
		const float o = c.r >= v ? saturate(pow(max(c.r - v, 0.0), 0.25)) : 0.0;
		return o.xxx;
	}
	// Auto-levels: the brightness range [lo, hi] (0..255 bins, from the histogram) to 0..1.
	float3 Linearize(float3 c, float2 lohi, float3 alpha)
	{
		const float3 x = c * 255.0;
		float3 v;
		if (lohi.y <= lohi.x)
			v = step(lohi.xxx, x);
		else
			v = saturate((x - lohi.x) / (lohi.y - lohi.x));
		return lerp(c, v, alpha);
	}

	// ---- 3x3 kernels on the back buffer (LLImageFilter::convolve), 1080p-pixel steps ----
	float2 KernelStep() { return BUFFER_PIXEL_SIZE * max(BUFFER_HEIGHT / 1080.0, 0.5); }
	float3 Box3(sampler s, float2 uv)
	{
		const float2 d = KernelStep();
		float3 sum = 0.0;
		[unroll] for (int y = -1; y <= 1; ++y)
			[unroll] for (int x = -1; x <= 1; ++x)
				sum += ToFilter(tex2Dlod(s, float4(uv + float2(x, y) * d, 0, 0)).rgb);
		return sum / 9.0;
	}
	float3 Sharpen3(sampler s, float2 uv)
	{
		const float2 d = KernelStep();
		float3 sum = 0.0;
		[unroll] for (int y = -1; y <= 1; ++y)
			[unroll] for (int x = -1; x <= 1; ++x)
				sum += ToFilter(tex2Dlod(s, float4(uv + float2(x, y) * d, 0, 0)).rgb) * ((x == 0 && y == 0) ? 9.0 : -1.0);
		return saturate(sum);
	}
}

// ---- the brightness histogram for Linearize (declare PRESTIGE_LEGACY_HISTOGRAM first) ----
// Three small passes before the filter's own: the picture's brightness on a 128 x 72 grid, its
// cumulative histogram (256 bins), then the tails' bins -> PrestigeLegacy::LevelsFor(tail).
#ifdef PRESTIGE_LEGACY_HISTOGRAM
namespace PrestigeLegacy
{
	texture BrightTex { Width = 128; Height = 72; Format = R16F; };
	sampler BrightSampler { Texture = BrightTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
	texture CumTex { Width = 256; Height = 1; Format = R32F; };
	sampler CumSampler { Texture = CumTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
	texture LevelsTex { Width = 1; Height = 1; Format = RG32F; };
	sampler LevelsSampler { Texture = LevelsTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

	// One back-buffer pixel per grid cell (its centre pixel, not an average: the tails matter).
	float PS_Bright(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		const float2 px = (floor(uv * BUFFER_SCREEN_SIZE) + 0.5) * BUFFER_PIXEL_SIZE;
		const float3 c = ToFilter(tex2Dlod(ReShade::BackBuffer, float4(px, 0, 0)).rgb);
		return floor((c.r + c.g + c.b) * 255.0 / 3.0 + 1e-3);   // LLImageFilter's integer bin
	}
	// Bin i: the share of the grid at or below brightness i.
	float PS_Cumulative(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		const float bin = floor(pos.x);
		float n = 0.0;
		[loop] for (int y = 0; y < 72; ++y)
			[loop] for (int x = 0; x < 128; ++x)
				n += tex2Dfetch(BrightSampler, int2(x, y)).x <= bin ? 1.0 : 0.0;
		return n / (128.0 * 72.0);
	}
	// x: the lowest bin whose share reaches the tail; y: the highest at or below 1 - tail.
	float2 Levels(float tail)
	{
		tail = saturate(tail);
		float lo = 255.0, hi = 0.0;
		bool foundLo = false;
		[loop] for (int i = 0; i < 256; ++i)
		{
			const float cum = tex2Dfetch(CumSampler, int2(i, 0)).x;
			if (!foundLo && cum >= tail - 1e-6) { lo = i; foundLo = true; }
			if (cum <= 1.0 - tail + 1e-6) hi = i;
		}
		return float2(lo, hi);
	}
	float2 LevelsFor() { return tex2Dfetch(LevelsSampler, int2(0, 0)).xy; }
}
#endif
