/*
 * Prestige Sepia Gold -- antique toned print with golden highlights.
 *
 * Brightness is mapped onto a toning gradient: deep brown shadows, warm sepia
 * mid-tones and gilded, creamy highlights, with an optional age fade.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Gold <
	ui_type = "slider"; ui_label = "Gold"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How golden (rather than plain brown) the highlights are.";
> = 0.6;

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Contrast of the toned print.";
> = 0.3;

uniform float Fade <
	ui_type = "slider"; ui_label = "Age Fade"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Lifts the blacks like an old, faded print.";
> = 0.25;

namespace PrestigeSepiaGold
{
	PRESTIGE_PS(PS_Sepia)
	{
		const float3 c = Prestige::Source(uv);
		float l = Prestige::Luma(saturate(c));
		l = lerp(l, l * l * (3.0 - 2.0 * l), ContrastAmount);
		const float3 dark = float3(0.16, 0.09, 0.05);
		const float3 mid = float3(0.62, 0.45, 0.28);
		const float3 hi = lerp(float3(0.98, 0.92, 0.80), float3(1.0, 0.86, 0.52), Gold);
		float3 r = l < 0.5 ? lerp(dark, mid, l * 2.0) : lerp(mid, hi, l * 2.0 - 1.0);
		r = lerp(r, 0.22 + r * 0.74, Fade);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeSepiaGold <
	ui_label = "Prestige Sepia Gold";
	ui_tooltip = "Antique sepia print with golden highlights.";
>
{
	pass Sepia { VertexShader = PostProcessVS; PixelShader = PrestigeSepiaGold::PS_Sepia; }
}
