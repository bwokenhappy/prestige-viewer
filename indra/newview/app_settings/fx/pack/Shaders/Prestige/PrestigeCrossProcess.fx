/*
 * Prestige Cross Process -- slide film developed in negative chemicals.
 *
 * Punchy red and green curves, a flattened and lifted blue channel: yellow-green
 * highlights, blue-cyan shadows, high saturation. The lomography look.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Steepness of the red and green curves.";
> = 0.7;

uniform float Shift <
	ui_type = "slider"; ui_label = "Colour Shift"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How strongly the blue channel is flattened (blue shadows, yellow highlights).";
> = 0.6;

uniform float SaturationAmount <
	ui_type = "slider"; ui_label = "Saturation"; ui_min = 0.5; ui_max = 1.8; ui_step = 0.01;
	ui_tooltip = "Colour intensity after processing.";
> = 1.2;

namespace PrestigeCrossProcess
{
	PRESTIGE_PS(PS_Cross)
	{
		const float3 c = Prestige::Source(uv);
		const float3 x = saturate(c);
		float3 r;
		r.r = lerp(x.r, smoothstep(0.04, 0.96, x.r), ContrastAmount);
		r.g = lerp(x.g, smoothstep(0.0, 1.05, x.g) * 1.04, ContrastAmount * 0.75);
		r.b = lerp(x.b, 0.2 + x.b * 0.6, Shift);
		const float l = Prestige::Luma(r);
		r += float3(0.02, 0.035, -0.04) * Shift * l;            // yellow-green highlights
		r += float3(-0.02, 0.0, 0.03) * Shift * (1.0 - l);      // blue-cyan shadows
		r = Prestige::Saturation(r, SaturationAmount);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeCrossProcess <
	ui_label = "Prestige Cross Process";
	ui_tooltip = "Lomo cross-processed slide film: yellow-green highlights, blue shadows, punchy colour.";
>
{
	pass Cross { VertexShader = PostProcessVS; PixelShader = PrestigeCrossProcess::PS_Cross; }
}
