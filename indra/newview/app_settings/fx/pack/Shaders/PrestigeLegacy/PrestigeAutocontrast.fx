/*
 * Autocontrast -- the classic snapshot filter app_settings/filters/Autocontrast.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * One operation: linearize 0.01 -- the darkest and brightest 1 % of the picture's brightness
 * histogram are clipped and the rest stretched to the full range.
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#define PRESTIGE_LEGACY_HISTOGRAM
#include "PrestigeLegacy.fxh"

uniform float Tail < ui_type = "slider"; ui_min = 0.0; ui_max = 0.2; ui_step = 0.005;
	ui_label = "Clip"; ui_tooltip = "Share of the darkest and of the brightest pixels clipped (the classic filter uses 0.01)."; > = 0.01;

float2 PS_PrestigeAutocontrastLevels(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return PrestigeLegacy::Levels(Tail);
}

float3 PS_PrestigeAutocontrast(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Linearize(c, PrestigeLegacy::LevelsFor(), float3(1.0, 1.0, 1.0));
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeAutocontrast < ui_label = "Autocontrast (classic)"; ui_tooltip = "The classic snapshot filter: stretches the picture's brightness to the full range."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Bright; RenderTarget = PrestigeLegacy::BrightTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PrestigeLegacy::PS_Cumulative; RenderTarget = PrestigeLegacy::CumTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeAutocontrastLevels; RenderTarget = PrestigeLegacy::LevelsTex; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeAutocontrast; }
}
