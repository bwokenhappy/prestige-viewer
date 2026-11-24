/*
 * Intense -- the classic snapshot filter app_settings/filters/Intense.xml (LLImageFilter) as
 * ReShade FX, for camera mode's filter stack: the viewfinder shows what the photo gets.
 * One operation: saturate 3.0 (chroma about the grey axis).
 * Prestige Viewer -- SPDX-License-Identifier: LGPL-2.1-only
 */

#include "PrestigeLegacy.fxh"

uniform float Saturation < ui_type = "slider"; ui_min = 0.0; ui_max = 5.0; ui_step = 0.05;
	ui_label = "Saturation"; ui_tooltip = "Colour strength (1 = unchanged; the classic filter uses 3)."; > = 3.0;

float3 PS_PrestigeIntense(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 orig = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 c = PrestigeLegacy::ToFilter(orig);
	c = PrestigeLegacy::Saturate(c, Saturation);
	return PrestigeLegacy::FromFilter(c, orig);
}

technique PrestigeIntense < ui_label = "Intense (classic)"; ui_tooltip = "The classic snapshot filter: much stronger colours."; >
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_PrestigeIntense; }
}
