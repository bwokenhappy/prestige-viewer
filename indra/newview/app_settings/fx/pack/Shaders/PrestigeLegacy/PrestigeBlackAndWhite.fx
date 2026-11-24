/*
 * Black and White -- the classic snapshot filter app_settings/filters/BlackAndWhite.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * One operation: grayscale (Rec. 709 luminance).
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#include "PrestigeLegacy.fxh"


float3 PS_PrestigeBlackAndWhite(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Grayscale(c);
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeBlackAndWhite < ui_label = "Black and White (classic)"; ui_tooltip = "The classic snapshot filter: grey from the picture's luminance."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeBlackAndWhite; }
}
