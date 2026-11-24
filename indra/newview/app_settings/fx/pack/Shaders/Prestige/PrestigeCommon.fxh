/*
 * PrestigeCommon.fxh -- shared helpers for the Prestige photo filters.
 *
 * Original Prestige code (not taken from any third-party shader pack).
 *
 * Resolution independence: every size a user sets (grain, blur radius, line width,
 * dot size...) is given in pixels of a 1080-line image and multiplied by
 * PRESTIGE_SCALE (BUFFER_HEIGHT / 1080), so a 4K or 8K snapshot looks like the
 * viewfinder. Blurs keep a fixed number of taps and read from mip levels when the
 * taps spread out, so their cost does not grow with the snapshot size.
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

#pragma once

#include "ReShade.fxh"

#ifndef __SLIPSTREAM__
	#define __SLIPSTREAM__ 0
#endif
#if __SLIPSTREAM__
	#include "Slipstream.fxh"
#endif

// One "1080p pixel" in this buffer's pixels, and in UV units.
#define PRESTIGE_SCALE (BUFFER_HEIGHT / 1080.0)
#define PRESTIGE_PX (float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT) * PRESTIGE_SCALE)
#define PRESTIGE_ASPECT (BUFFER_WIDTH * BUFFER_RCP_HEIGHT)
// The display point is 8-bit sRGB (0..1); the HDR point is linear with values above 1.
#define PRESTIGE_HDR (BUFFER_COLOR_SPACE >= 2)

// Format for full-resolution copies of the image: 8-bit at the display point (saves
// memory on 8K snapshots), half float at the HDR point (keeps values above 1).
#if PRESTIGE_HDR
	#define PRESTIGE_COPY_FORMAT RGBA16F
#else
	#define PRESTIGE_COPY_FORMAT RGBA8
#endif

// The "Strength" slider every Prestige filter has: 0 = original image, 1 = full filter.
#define PRESTIGE_STRENGTH_UNIFORM \
	uniform float Strength < \
		ui_type = "slider"; ui_label = "Strength"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; \
		ui_tooltip = "How much of the filter is mixed over the original image (0 = off, 1 = full)."; \
	> = 1.0;

// Fullscreen pixel shader signature used by every pass.
#define PRESTIGE_PS(name) float4 name(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target

namespace Prestige
{
	// Back buffer with mirrored edges (for distortions that read slightly outside the
	// image); ReShade::BackBuffer clamps.
	sampler BackBufferMirror { Texture = ReShade::BackBufferTex; AddressU = MIRROR; AddressV = MIRROR; };

	float3 Source(float2 uv) { return tex2Dlod(ReShade::BackBuffer, float4(uv, 0, 0)).rgb; }

	// Keeps display-point results in range; HDR-point results may stay above 1.
	float3 Clamp(float3 c)
	{
#if PRESTIGE_HDR
		return max(c, 0.0);
#else
		return saturate(c);
#endif
	}

	// Mix the filtered colour over the original by the Strength slider.
	float4 Finish(float3 original, float3 filtered, float strength)
	{
		return float4(lerp(original, Clamp(filtered), saturate(strength)), 1.0);
	}

	// ------------------------------------------------------------------ colour

	float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

	float3 Saturation(float3 c, float s) { return lerp(Luma(c).xxx, c, s); }

	float3 Contrast(float3 c, float k, float pivot) { return (c - pivot) * k + pivot; }

	// Smooth S-curve on 0..1: amount 0 = straight line, 1 = strong.
	float3 SCurve(float3 c, float amount)
	{
		const float3 x = saturate(c);
		const float3 s = x * x * (3.0 - 2.0 * x);
		return lerp(c, s, amount);
	}

	// Approximate display <-> linear light (identity at the HDR point, which is linear).
	float3 ToLinear(float3 c)
	{
#if PRESTIGE_HDR
		return c;
#else
		return pow(max(c, 0.0), 2.2);
#endif
	}
	float3 ToDisplay(float3 c)
	{
#if PRESTIGE_HDR
		return c;
#else
		return pow(max(c, 0.0), 1.0 / 2.2);
#endif
	}

	float3 Screen(float3 a, float3 b) { return 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b)); }

	float3 Overlay(float3 a, float3 b)
	{
		const float3 lo = 2.0 * a * b;
		const float3 hi = 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
		return lerp(lo, hi, step(0.5, a));
	}

	// Gentle soft-light blend (a = base, b = blend layer).
	float3 SoftLight(float3 a, float3 b)
	{
		a = saturate(a);
		return (1.0 - 2.0 * b) * a * a + 2.0 * b * a;
	}

	float3 RGBToHSV(float3 c)
	{
		const float mx = max(c.r, max(c.g, c.b));
		const float mn = min(c.r, min(c.g, c.b));
		const float d = mx - mn;
		float h = 0.0;
		if (d > 1e-5)
		{
			if (mx == c.r) h = (c.g - c.b) / d;
			else if (mx == c.g) h = 2.0 + (c.b - c.r) / d;
			else h = 4.0 + (c.r - c.g) / d;
			h = frac(h / 6.0 + 1.0);
		}
		return float3(h, mx > 1e-5 ? d / mx : 0.0, mx);
	}

	float3 HSVToRGB(float3 hsv)
	{
		const float3 k = saturate(abs(frac(hsv.x + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
		return hsv.z * lerp(1.0.xxx, k, hsv.y);
	}

	// NTSC-style luma / chroma (used by the analogue looks).
	float3 RGBToYIQ(float3 c)
	{
		return float3(dot(c, float3(0.299, 0.587, 0.114)),
		              dot(c, float3(0.596, -0.274, -0.322)),
		              dot(c, float3(0.211, -0.523, 0.312)));
	}
	float3 YIQToRGB(float3 y)
	{
		return float3(dot(y, float3(1.0, 0.956, 0.621)),
		              dot(y, float3(1.0, -0.272, -0.647)),
		              dot(y, float3(1.0, -1.106, 1.703)));
	}

	// Simple white balance: positive = warmer (amber), negative = cooler (blue).
	float3 Warmth(float3 c, float w)
	{
		return c * (1.0 + w * float3(0.10, 0.02, -0.12));
	}

	// Mix colour c towards a tint while keeping its luma.
	float3 Tint(float3 c, float3 tint, float amount)
	{
		const float l = Luma(c);
		const float3 t = tint * (l / max(Luma(tint), 1e-3));
		return lerp(c, t, amount);
	}

	// ------------------------------------------------------------------ noise

	// 32-bit integer mixer (avalanching multiply / xor-shift).
	uint HashU(uint x)
	{
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}

	// Uniform 0..1 from an integer cell and a seed.
	float Hash(int2 cell, uint seed)
	{
		const uint h = HashU(uint(cell.x) ^ HashU(uint(cell.y) + seed * 0x9e3779b9u));
		return float(h >> 8) * (1.0 / 16777215.0);
	}
	float Hash(float2 p, uint seed) { return Hash(int2(floor(p)), seed); }
	float3 Hash3(float2 p, uint seed)
	{
		return float3(Hash(p, seed), Hash(p, seed + 17u), Hash(p, seed + 41u));
	}

	// Smooth value noise, about one feature per unit of p.
	float ValueNoise(float2 p, uint seed)
	{
		const float2 i = floor(p);
		float2 f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		const float a = Hash(i, seed);
		const float b = Hash(i + float2(1, 0), seed);
		const float c = Hash(i + float2(0, 1), seed);
		const float d = Hash(i + float2(1, 1), seed);
		return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
	}

	// Three octaves of value noise (soft cloudy shapes), 0..1.
	float Fbm(float2 p, uint seed)
	{
		float v = 0.0, a = 0.5;
		[unroll]
		for (int o = 0; o < 3; ++o)
		{
			v += a * ValueNoise(p, seed + uint(o) * 101u);
			p = p * 2.03 + 7.1;
			a *= 0.5;
		}
		return v / 0.875;
	}

	// Roughly Gaussian noise (mean 0, sd ~0.3) from three uniform draws.
	float GaussNoise(float2 p, uint seed)
	{
		return (Hash(p, seed) + Hash(p, seed + 7u) + Hash(p, seed + 13u)) - 1.5;
	}

	// ------------------------------------------------------------------ geometry

	// Aspect-correct offset from the centre (x scaled by the aspect ratio, y in -0.5..0.5).
	float2 Centered(float2 uv) { return (uv - 0.5) * float2(PRESTIGE_ASPECT, 1.0); }

	// Vignette factor 1 (untouched) .. 0 (black) at uv.
	// size: radius where darkening starts (in image heights from the centre),
	// softness: width of the falloff, roundness: 1 = circle, 0 = follows the frame.
	float Vignette(float2 uv, float size, float softness, float roundness)
	{
		const float2 d = (uv - 0.5) * lerp(float2(1.0, 1.0), float2(PRESTIGE_ASPECT, 1.0), roundness);
		const float r = length(d) * 2.0;
		return 1.0 - smoothstep(size, size + max(softness, 1e-3), r);
	}

	// ------------------------------------------------------------------ blur

	// One direction of a separable Gaussian with a fixed number of taps (8 each side,
	// the kernel spans +-3 sigma). texel = one texel of s at mip 0 along the blur
	// axis, in UV; radius in mip-0 texels. Reads coarser mips when the taps spread
	// further than a texel, so s needs MipLevels for big radii.
	float4 GaussianBlur(sampler s, float2 uv, float2 texel, float radius)
	{
		const float stepTexels = max(radius, 1e-3) / 8.0;
		const float lod = max(0.0, log2(stepTexels));
		float4 sum = tex2Dlod(s, float4(uv, 0, lod));
		float wsum = 1.0;
		[unroll]
		for (int i = 1; i <= 8; ++i)
		{
			const float w = exp(-4.5 * (i / 8.0) * (i / 8.0));
			const float2 o = texel * (stepTexels * i);
			sum += (tex2Dlod(s, float4(uv + o, 0, lod)) + tex2Dlod(s, float4(uv - o, 0, lod))) * w;
			wsum += 2.0 * w;
		}
		return sum / wsum;
	}

	// Blurry local average of s around uv: a 3x3 tent read from a mip whose texels are
	// about half the radius. radiusUV in UV of the image height; texHeight = height of
	// s at mip 0 in texels. s needs MipLevels.
	float4 SoftAverage(sampler s, float2 uv, float radiusUV, float texHeight)
	{
		const float lod = max(0.0, log2(radiusUV * texHeight * 0.5));
		const float2 o = float2(radiusUV / PRESTIGE_ASPECT, radiusUV) * 0.5;
		float4 c = 0.0;
		[unroll]
		for (int y = -1; y <= 1; ++y)
		{
			[unroll]
			for (int x = -1; x <= 1; ++x)
			{
				const float w = (2.0 - abs(x)) * (2.0 - abs(y));
				c += tex2Dlod(s, float4(uv + o * float2(x, y), 0, lod)) * w;
			}
		}
		return c / 16.0;
	}

	// ------------------------------------------------------------------ depth

	// View distance in metres (the sky is the far plane).
	float Metres(float2 uv) { return ReShade::GetLinearizedDepth(uv) * RESHADE_DEPTH_LINEARIZATION_FAR_PLANE; }
	bool IsSky(float2 uv) { return ReShade::GetLinearizedDepth(uv) > 0.999; }

	// 1 on an avatar, 0 elsewhere (always 0 without Slipstream's avatar mask).
	float AvatarMask(float2 uv)
	{
#if __SLIPSTREAM__
		return Slipstream::GetAvatarId(uv) > 0.5 ? 1.0 : 0.0;
#else
		return 0.0 * uv.x;
#endif
	}

	// ------------------------------------------------------------------ edges

	// Sobel edge magnitude of luma, sampled `width` pixels apart (in UV via px).
	float SobelLuma(sampler s, float2 uv, float2 px, float lod)
	{
		const float tl = Luma(tex2Dlod(s, float4(uv + px * float2(-1, -1), 0, lod)).rgb);
		const float t  = Luma(tex2Dlod(s, float4(uv + px * float2( 0, -1), 0, lod)).rgb);
		const float tr = Luma(tex2Dlod(s, float4(uv + px * float2( 1, -1), 0, lod)).rgb);
		const float l  = Luma(tex2Dlod(s, float4(uv + px * float2(-1,  0), 0, lod)).rgb);
		const float r  = Luma(tex2Dlod(s, float4(uv + px * float2( 1,  0), 0, lod)).rgb);
		const float bl = Luma(tex2Dlod(s, float4(uv + px * float2(-1,  1), 0, lod)).rgb);
		const float b  = Luma(tex2Dlod(s, float4(uv + px * float2( 0,  1), 0, lod)).rgb);
		const float br = Luma(tex2Dlod(s, float4(uv + px * float2( 1,  1), 0, lod)).rgb);
		const float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
		const float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
		return sqrt(gx * gx + gy * gy);
	}
}
