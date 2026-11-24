/*
 * Spotlight -- the classic snapshot filter app_settings/filters/Spotlight.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * Brightens the middle (a vignette added, up to 40 %) and darkens the edges (down to 20 %).
 * (Its linearize 0.0 changes nothing and is left out.)
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#include "PrestigeLegacy.fxh"


float3 PS_PrestigeSpotlight(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	float a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, 0.0, 0.4, float4(0.0, 0.0, 1.0, 2.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Contrast(c, 1.0, float3(1.0, 1.0, 1.0)));
	a = PrestigeLegacy::StencilAlpha(PrestigeLegacy::SHAPE_VIGNETTE, -0.8, 0.0, float4(0.0, 0.0, 1.0, 2.0), uv);
	c = PrestigeLegacy::Blend(PrestigeLegacy::MODE_ADD, a, c, PrestigeLegacy::Contrast(c, 1.0, float3(1.0, 1.0, 1.0)));
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeSpotlight < ui_label = "Spotlight (classic)"; ui_tooltip = "The classic snapshot filter: a pool of light in the middle, dark edges."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeSpotlight; }
}
