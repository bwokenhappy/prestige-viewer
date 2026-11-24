/*
 * Prestige Pop Art -- posterised, saturated screen-print colours with ink outlines.
 *
 * Each colour channel is reduced to a few flat levels (with anti-aliased steps),
 * saturation is pushed and dark outlines trace the edges.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform int Levels <
	ui_type = "slider"; ui_label = "Levels"; ui_min = 2; ui_max = 8;
	ui_tooltip = "Flat colour steps per channel (fewer = bolder poster).";
> = 3;

uniform float SaturationAmount <
	ui_type = "slider"; ui_label = "Saturation"; ui_min = 0.5; ui_max = 2.5; ui_step = 0.01;
	ui_tooltip = "Colour punch before posterising.";
> = 1.6;

uniform float Outline <
	ui_type = "slider"; ui_label = "Outlines"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Darkness of the ink outlines (width scales with the snapshot size).";
> = 0.6;

namespace PrestigePopArt
{
	float3 Posterize(float3 c, float levels, float aa)
	{
		const float3 f = saturate(c) * levels;
		const float3 q = floor(f) + smoothstep(0.5 - aa, 0.5 + aa, frac(f));
		return q / levels;
	}

	PRESTIGE_PS(PS_Pop)
	{
		const float3 c = Prestige::Source(uv);
		float3 r = Prestige::Saturation(c, SaturationAmount);
		r = Prestige::SCurve(r, 0.3);
		r = Posterize(r, Levels - 1.0, 0.04);
		const float edge = Prestige::SobelLuma(ReShade::BackBuffer, uv, PRESTIGE_PX * 1.2, 0.0);
		r *= 1.0 - smoothstep(0.15, 0.5, edge) * Outline;
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigePopArt <
	ui_label = "Prestige Pop Art";
	ui_tooltip = "Posterised, saturated screen-print colours with ink outlines.";
>
{
	pass Pop { VertexShader = PostProcessVS; PixelShader = PrestigePopArt::PS_Pop; }
}
