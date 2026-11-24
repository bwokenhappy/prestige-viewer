/*
 * PrestigeLUT.fx -- a colour lookup table (a 3D LUT baked into a 2D strip) for camera mode's
 * filter stack and Photo Studio's Effects.
 *
 * The strip: width SIZE * SIZE, height SIZE; pixel (x = blue * SIZE + red, y = green), the
 * layout ReShade LUT shaders use. Camera mode's "Import LUT..." converts a .cube file
 * (Lightroom, DaVinci Resolve, Photoshop; 1D or 3D, up to 65) into such a strip in your
 * effects Textures folder and writes a small effect beside it that sets the three
 * definitions below and includes this file -- one effect per LUT, so several can stack.
 * You can also use this file directly: set PRESTIGE_LUT_FILE and PRESTIGE_LUT_SIZE in its
 * preprocessor definitions (Effects page). Without them it changes nothing.
 *
 * Sampling is trilinear: a bilinear tap in each of the two blue slices around the colour,
 * blended by blue's fraction. The table applies to the display picture's gamma-encoded
 * values (0..1), as LUTs made for sRGB / Rec. 709 output expect. On an HDR display (scRGB)
 * the SDR range is encoded first; highlights above SDR white pass unchanged.
 *
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#include "ReShade.fxh"

#ifndef PRESTIGE_LUT_SIZE
	#define PRESTIGE_LUT_SIZE 33
#endif
#ifndef PRESTIGE_LUT_LABEL
	#define PRESTIGE_LUT_LABEL "LUT"
#endif
#ifndef PRESTIGE_SCRGB_WHITE
	#define PRESTIGE_SCRGB_WHITE (203.0 / 80.0)
#endif

uniform float LutIntensity < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_label = "Intensity"; ui_tooltip = "How much of the LUT's look is applied (1 = all of it)."; > = 1.0;
uniform float LutExposure < ui_type = "slider"; ui_min = -2.0; ui_max = 2.0; ui_step = 0.05;
	ui_label = "Exposure before LUT"; ui_units = " EV";
	ui_tooltip = "Brightens or darkens the picture going into the LUT (many LUTs expect a particular exposure)."; > = 0.0;

namespace PrestigeLutFx
{
#ifdef PRESTIGE_LUT_FILE
	texture LutTex < source = PRESTIGE_LUT_FILE; > { Width = PRESTIGE_LUT_SIZE * PRESTIGE_LUT_SIZE; Height = PRESTIGE_LUT_SIZE; Format = RGBA8; };
	sampler LutSampler { Texture = LutTex; AddressU = CLAMP; AddressV = CLAMP; MagFilter = LINEAR; MinFilter = LINEAR; MipFilter = POINT; };

	float3 Lookup(float3 c)
	{
		const float n = PRESTIGE_LUT_SIZE;
		c = saturate(c) * (n - 1.0);
		const float b0 = floor(c.b);
		const float b1 = min(b0 + 1.0, n - 1.0);
		const float2 uv = float2((c.r + 0.5) / (n * n), (c.g + 0.5) / n);
		const float3 s0 = tex2Dlod(LutSampler, float4(uv.x + b0 / n, uv.y, 0, 0)).rgb;
		const float3 s1 = tex2Dlod(LutSampler, float4(uv.x + b1 / n, uv.y, 0, 0)).rgb;
		return lerp(s0, s1, c.b - b0);
	}
#else
	float3 Lookup(float3 c) { return saturate(c); }
#endif

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

	float4 PS_Lut(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		float4 col = tex2D(ReShade::BackBuffer, uv);
		const float gain = exp2(LutExposure);
#if BUFFER_COLOR_SPACE == 2
		const float3 lin = col.rgb * gain / PRESTIGE_SCRGB_WHITE;
		const float3 sdr = saturate(lin);
		const float3 graded = SrgbDecode(Lookup(SrgbEncode(sdr))) + (lin - sdr);
		col.rgb = lerp(col.rgb, graded * PRESTIGE_SCRGB_WHITE, LutIntensity);
#else
		// Exposure in linear light, back to the display encoding for the table.
		const float3 enc = SrgbEncode(SrgbDecode(saturate(col.rgb)) * gain);
		col.rgb = lerp(col.rgb, Lookup(enc), LutIntensity);
#endif
		return col;
	}
}

technique PrestigeLUT < ui_label = PRESTIGE_LUT_LABEL; ui_tooltip = "A colour lookup table (LUT)."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLutFx::PS_Lut; }
}
