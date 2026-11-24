/*
 * Miniature -- the classic snapshot filter app_settings/filters/Miniature.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * linearize 0.02, contrast 1.02, saturate 1.2, a 3x3 sharpen under a soft vignette (up to
 * 25 %), then the tilt-shift: ten 3x3 blurs, each blended by a gradient that is full at the
 * bottom edge and gone 37.5 % up (and the same from the top). Here the ten blurs are one
 * Gaussian of the same width (ten 3x3 box passes: sigma = sqrt(10 * 2 / 3) = 2.6 px at 1080p),
 * blended by 1 - (1 - gradient)^10 -- the share ten successive blends leave blurred.
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#define PRESTIGE_LEGACY_HISTOGRAM
#include "PrestigeLegacy.fxh"

uniform float Band < ui_type = "slider"; ui_min = 0.1; ui_max = 0.5; ui_step = 0.005;
	ui_label = "Blur band"; ui_tooltip = "How far the blur reaches in from the top and bottom edges, as a share of the height (the classic filter uses 0.375)."; > = 0.375;
uniform float BlurScale < ui_type = "slider"; ui_min = 0.0; ui_max = 4.0; ui_step = 0.05;
	ui_label = "Blur"; ui_tooltip = "Blur strength (1 = the classic filter's ten passes)."; > = 1.0;

namespace PrestigeMiniatureFx
{
	texture BlurHTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
	sampler BlurH { Texture = BlurHTex; };
	texture BlurVTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
	sampler BlurV { Texture = BlurVTex; };

	// The two gradient stencils (min 1, max 0): 1 at the edge, 0 at Band of the height from it.
	float BandAlpha(float2 uv)
	{
		const float j = 1.0 - uv.y;   // 0 at the bottom, as LLImageFilter counts rows
		const float b = max(Band, 1e-3);
		return max(1.0 - saturate(j / b), 1.0 - saturate((1.0 - j) / b));
	}

	float3 Gauss(sampler s, float2 uv, float2 dir)
	{
		const float sigma = max(2.582 * BlurScale, 1e-3);
		const float2 stepUv = dir * BUFFER_PIXEL_SIZE * max(BUFFER_HEIGHT / 1080.0, 0.5);
		float3 sum = 0.0;
		float wsum = 0.0;
		[loop] for (int k = -8; k <= 8; ++k)
		{
			const float w = exp(-(k * k) / (2.0 * sigma * sigma));
			sum += PrestigeLegacy::ToFilter(tex2Dlod(s, float4(uv + stepUv * k, 0, 0)).rgb) * w;
			wsum += w;
		}
		return sum / wsum;
	}

	float2 PS_Levels(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		return PrestigeLegacy::Levels(0.02);
	}
	float3 PS_Tone(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
		float3 c = PrestigeLegacy::ToFilter(orig);
		c = PrestigeLegacy::Linearize(c, PrestigeLegacy::LevelsFor(), float3(1.0, 1.0, 1.0));
		c = PrestigeLegacy::Contrast(c, 1.02, float3(1.0, 1.0, 1.0));
		c = PrestigeLegacy::Saturate(c, 1.2);
		return PrestigeLegacy::FromFilter(c, orig);
	}
	float3 PS_Sharpen(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
		const float3 c = PrestigeLegacy::ToFilter(orig);
		const float a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 0.25, float4(0.0, 0.0, 0.25, 2.0), uv);
		return PrestigeLegacy::FromFilter(PrestigeLegacy::Blend(PrestigeLegacy::MODE_BLEND, a, c, PrestigeLegacy::Sharpen3(ReShade::BackBuffer, uv)), orig);
	}
	float4 PS_BlurH(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		return float4(Gauss(ReShade::BackBuffer, uv, float2(1.0, 0.0)), 1.0);
	}
	float4 PS_BlurV(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		// BlurH already holds filter-space (gamma-encoded 0..1) values: sampled as they are.
		const float sigma = max(2.582 * BlurScale, 1e-3);
		const float2 stepUv = float2(0.0, 1.0) * BUFFER_PIXEL_SIZE * max(BUFFER_HEIGHT / 1080.0, 0.5);
		float3 sum = 0.0;
		float wsum = 0.0;
		[loop] for (int k = -8; k <= 8; ++k)
		{
			const float w = exp(-(k * k) / (2.0 * sigma * sigma));
			sum += tex2Dlod(BlurH, float4(uv + stepUv * k, 0, 0)).rgb * w;
			wsum += w;
		}
		return float4(sum / wsum, 1.0);
	}
	float3 PS_Mix(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
	{
		const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
		const float3 c = PrestigeLegacy::ToFilter(orig);
		const float a = 1.0 - pow(1.0 - BandAlpha(uv), 10.0);
		return PrestigeLegacy::FromFilter(lerp(c, tex2D(BlurV, uv).rgb, BlurScale > 0.0 ? a : 0.0), orig);
	}
}

technique PrestigeMiniature < ui_label = "Miniature (classic)"; ui_tooltip = "The classic snapshot filter: tilt-shift -- blurred top and bottom, crisp, colourful middle."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Bright; RenderTarget = PrestigeLegacy::BrightTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Cumulative; RenderTarget = PrestigeLegacy::CumTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeMiniatureFx::PS_Levels; RenderTarget = PrestigeLegacy::LevelsTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeMiniatureFx::PS_Tone; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeMiniatureFx::PS_Sharpen; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeMiniatureFx::PS_BlurH; RenderTarget = PrestigeMiniatureFx::BlurHTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeMiniatureFx::PS_BlurV; RenderTarget = PrestigeMiniatureFx::BlurVTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeMiniatureFx::PS_Mix; }
}
