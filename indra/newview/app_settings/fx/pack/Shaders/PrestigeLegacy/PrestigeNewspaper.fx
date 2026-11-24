/*
 * Newspaper -- the classic snapshot filter app_settings/filters/Newspaper.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * linearize 0.1, grayscale, then a halftone screen (2D sine, wavelength 0.02 of the height).
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#define PRESTIGE_LEGACY_HISTOGRAM
#include "PrestigeLegacy.fxh"

uniform float DotSize < ui_type = "slider"; ui_min = 0.005; ui_max = 0.1; ui_step = 0.001;
	ui_label = "Dot size"; ui_tooltip = "Halftone wavelength as a share of the picture height (the classic filter uses 0.02)."; > = 0.02;
uniform float Angle < ui_type = "slider"; ui_min = 0.0; ui_max = 90.0; ui_step = 1.0;
	ui_label = "Screen angle"; ui_units = " deg"; > = 0.0;

float2 PS_PrestigeNewspaperLevels(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return PrestigeLegacy::Levels(0.1);
}

float3 PS_PrestigeNewspaper(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Linearize(c, PrestigeLegacy::LevelsFor(), float3(1.0, 1.0, 1.0));
	c = PrestigeLegacy::Grayscale(c);
	c = PrestigeLegacy::Screen(c, 0, DotSize, Angle, uv);
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeNewspaper < ui_label = "Newspaper (classic)"; ui_tooltip = "The classic snapshot filter: a black and white halftone print."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Bright; RenderTarget = PrestigeLegacy::BrightTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Cumulative; RenderTarget = PrestigeLegacy::CumTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeNewspaperLevels; RenderTarget = PrestigeLegacy::LevelsTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeNewspaper; }
}
