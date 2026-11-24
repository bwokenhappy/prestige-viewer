/*
 * Prestige Bleach Bypass -- the silver-retention film process.
 *
 * Skipping the bleach step leaves silver in the film: a black-and-white image laid
 * over the colour one. The result is low saturation, hard contrast and metallic
 * highlights -- gritty war-film and thriller looks.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Silver <
	ui_type = "slider"; ui_label = "Silver Retention"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much of the black-and-white silver layer stays in the image.";
> = 0.7;

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Extra contrast on top of the process.";
> = 0.35;

uniform float Coolness <
	ui_type = "slider"; ui_label = "Cool Cast"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Positive = steely blue-green cast, negative = warmer.";
> = 0.25;

namespace PrestigeBleachBypass
{
	PRESTIGE_PS(PS_Bleach)
	{
		const float3 c = Prestige::Source(uv);
		const float l = Prestige::Luma(saturate(c));
		// Overlay the luma layer onto the colour layer, then pull saturation down.
		const float3 silverLayer = Prestige::Overlay(saturate(c), l.xxx);
		float3 r = lerp(c, silverLayer, Silver);
		r = Prestige::Saturation(r, 1.0 - 0.55 * Silver);
		r = Prestige::SCurve(r, ContrastAmount);
		r = Prestige::Warmth(r, -Coolness * 0.6) * (1.0 + float3(-0.01, 0.01, 0.0) * Coolness);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeBleachBypass <
	ui_label = "Prestige Bleach Bypass";
	ui_tooltip = "Desaturated, high-contrast, metallic film look (silver retention).";
>
{
	pass Bleach { VertexShader = PostProcessVS; PixelShader = PrestigeBleachBypass::PS_Bleach; }
}
