/*
 * Colors 1970 -- the classic snapshot filter app_settings/filters/Colors1970.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * linearize 0.1, contrast 0.8, colorize red 0.3, then a vignette (gamma 10) that tints the middle.
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#define PRESTIGE_LEGACY_HISTOGRAM
#include "PrestigeLegacy.fxh"

uniform float Tail < ui_type = "slider"; ui_min = 0.0; ui_max = 0.3; ui_step = 0.005;
	ui_label = "Clip"; ui_tooltip = "Auto-levels clip (the classic filter uses 0.1)."; > = 0.1;

float2 PS_PrestigeColors1970Levels(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return PrestigeLegacy::Levels(Tail);
}

float3 PS_PrestigeColors1970(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Linearize(c, PrestigeLegacy::LevelsFor(), float3(1.0, 1.0, 1.0));
	c = PrestigeLegacy::Contrast(c, 0.8, float3(1.0, 1.0, 1.0));
	c = PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.3, 0.0, 0.0));
	const float a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 1.0, float4(0.0, 0.0, 1.0, 10.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_BLEND, a, c, PrestigeLegacy::Colorize(c, float3(1.0, 1.0, 1.0), float3(0.1, 0.1, 0.0)));
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeColors1970 < ui_label = "Colors 1970 (classic)"; ui_tooltip = "The classic snapshot filter: faded, warm 1970s print colours."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Bright; RenderTarget = PrestigeLegacy::BrightTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Cumulative; RenderTarget = PrestigeLegacy::CumTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeColors1970Levels; RenderTarget = PrestigeLegacy::LevelsTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeColors1970; }
}
