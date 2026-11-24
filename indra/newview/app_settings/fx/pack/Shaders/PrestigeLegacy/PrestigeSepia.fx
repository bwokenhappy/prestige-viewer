/*
 * Sepia -- the classic snapshot filter app_settings/filters/Sepia.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * One operation: the sepia colour matrix.
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#include "PrestigeLegacy.fxh"


float3 PS_PrestigeSepia(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Sepia(c);
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeSepia < ui_label = "Sepia (classic)"; ui_tooltip = "The classic snapshot filter: warm brown tones."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeSepia; }
}
