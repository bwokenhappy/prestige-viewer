/*
 * Toy Camera -- the classic snapshot filter app_settings/filters/Toycamera.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * Under a fading vignette: linearize 0.05, grayscale, contrast 1.1; then a 3x3 blur toward the edges.
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#define PRESTIGE_LEGACY_HISTOGRAM
#include "PrestigeLegacy.fxh"


float2 PS_PrestigeToycameraLevels(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return PrestigeLegacy::Levels(0.05);
}

float3 PS_PrestigeToycameraBlur(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	const float3 c = PrestigeLegacy::ToFilter(orig);
	const float a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 1.0, 0.0, float4(0.0, 0.0, 0.5, 2.0), uv);
	return PrestigeLegacy::FromFilter(PrestigeLegacy::Blend(PrestigeLegacy::MODE_BLEND, a, c, PrestigeLegacy::Box3(ReShade::BackBuffer, uv)), orig);
}

float3 PS_PrestigeToycamera(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	const float a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 1.0, float4(0.0, 0.0, 1.2, 3.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_FADE, a, c, PrestigeLegacy::Linearize(c, PrestigeLegacy::LevelsFor(), float3(1.0, 1.0, 1.0)));
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_FADE, a, c, PrestigeLegacy::Grayscale(c));
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_FADE, a, c, PrestigeLegacy::Contrast(c, 1.1, float3(1.0, 1.0, 1.0)));
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeToycamera < ui_label = "Toy Camera (classic)"; ui_tooltip = "The classic snapshot filter: a grainy black and white toy camera with dark, soft edges."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Bright; RenderTarget = PrestigeLegacy::BrightTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Cumulative; RenderTarget = PrestigeLegacy::CumTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeToycameraLevels; RenderTarget = PrestigeLegacy::LevelsTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeToycamera; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeToycameraBlur; }
}
