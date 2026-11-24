/*
 * Video -- the classic snapshot filter app_settings/filters/Video.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * darken 0.15, then at half strength (added): scan lines, gamma 0.25 and two 3x3 blurs -- an old video look.
 * (Its linearize 0.0 changes nothing and is left out.)
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#include "PrestigeLegacy.fxh"

uniform float LineSpacing < ui_type = "slider"; ui_min = 0.005; ui_max = 0.1; ui_step = 0.001;
	ui_label = "Line spacing"; ui_tooltip = "Scan line wavelength as a share of the picture height (the classic filter uses 0.02)."; > = 0.02;

float3 PS_PrestigeVideoBlur(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	const float3 c = PrestigeLegacy::ToFilter(orig);
	return PrestigeLegacy::FromFilter(PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, 0.5, c, PrestigeLegacy::Box3(ReShade::BackBuffer, uv)), orig);
}

float3 PS_PrestigeVideo(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Brighten(c, -0.15, float3(1.0, 1.0, 1.0));
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, 0.5, c, PrestigeLegacy::Screen(c, 1, LineSpacing, 0.0, uv));
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, 0.5, c, PrestigeLegacy::Gamma(c, 0.25, float3(1.0, 1.0, 1.0)));
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeVideo < ui_label = "Video (classic)"; ui_tooltip = "The classic snapshot filter: bright, soft video with scan lines."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeVideo; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeVideoBlur; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeVideoBlur; }
}
