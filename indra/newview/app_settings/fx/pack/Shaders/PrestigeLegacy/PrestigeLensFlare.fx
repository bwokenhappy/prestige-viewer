/*
 * Lens Flare -- the classic snapshot filter app_settings/filters/LensFlare.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * linearize 0.01, a diagonal gradient glow, then five vignettes added along the diagonal (the flare's rings).
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#define PRESTIGE_LEGACY_HISTOGRAM
#include "PrestigeLegacy.fxh"


float2 PS_PrestigeLensFlareLevels(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return PrestigeLegacy::Levels(0.01);
}

float3 PS_PrestigeLensFlare(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Linearize(c, PrestigeLegacy::LevelsFor(), float3(1.0, 1.0, 1.0));
	float a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_GRADIENT, 0.5, 0.0, float4(-1.0, 1.0, 1.0, -1.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.1, 0.1, 0.0)));
	a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 0.5, float4(-1.0, 1.0, 1.5, 5.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.6, 0.0, 0.0)));
	a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 0.5, float4(-1.0, 1.0, 1.0, 5.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.6, 0.6, 0.0)));
	a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 0.5, float4(0.5, -0.5, 0.10, 20.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.7, 0.0, 0.0)));
	a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 0.5, float4(0.6, -0.6, 0.05, 20.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.7, 0.0, 0.0)));
	a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 0.5, float4(0.4, -0.4, 0.025, 20.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.7, 0.0, 0.0)));
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeLensFlare < ui_label = "Lens Flare (classic)"; ui_tooltip = "The classic snapshot filter: a glow from the top left with flare rings across the picture."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Bright; RenderTarget = PrestigeLegacy::BrightTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Cumulative; RenderTarget = PrestigeLegacy::CumTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeLensFlareLevels; RenderTarget = PrestigeLegacy::LevelsTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeLensFlare; }
}
