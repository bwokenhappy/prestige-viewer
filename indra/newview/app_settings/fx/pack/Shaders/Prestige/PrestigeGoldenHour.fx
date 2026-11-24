/*
 * Prestige Golden Hour -- low, warm evening sun.
 *
 * Warm white balance, richer warm colours, and a soft sun glow from a chosen point
 * (it can sit just outside the frame) that washes the image with golden light.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float WarmthAmount <
	ui_type = "slider"; ui_label = "Warmth"; ui_min = 0.0; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "Overall golden colour balance.";
> = 0.7;

uniform float SunGlow <
	ui_type = "slider"; ui_label = "Sun Glow"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Brightness of the warm light coming from the sun position.";
> = 0.45;

uniform float2 SunPosition <
	ui_type = "slider"; ui_label = "Sun Position"; ui_min = -0.5; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "Where the sun light comes from (x, y across the image; outside 0..1 = beyond the frame).";
> = float2(0.85, 0.2);

uniform float GlowSize <
	ui_type = "slider"; ui_label = "Glow Size"; ui_min = 0.1; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "How far the sun glow spreads across the image.";
> = 0.8;

namespace PrestigeGoldenHour
{
	PRESTIGE_PS(PS_Golden)
	{
		const float3 c = Prestige::Source(uv);
		float3 r = Prestige::Warmth(c, WarmthAmount);
		// Richer oranges and reds, slightly softer blues.
		const float3 hsv = Prestige::RGBToHSV(saturate(r));
		const float warm = smoothstep(0.2, 0.0, abs(frac(hsv.x + 0.5) - 0.5 - 0.06));
		r = Prestige::Saturation(r, 1.0 + 0.25 * warm * WarmthAmount - 0.1 * (1.0 - warm) * WarmthAmount);

		// Sun glow: aspect-correct distance from the sun point, soft falloff.
		const float2 d = (uv - SunPosition) * float2(PRESTIGE_ASPECT, 1.0);
		const float dist = length(d) / GlowSize;
		const float glow = exp(-dist * dist * 2.5) * 0.8 + exp(-dist * 1.2) * 0.35;
		const float3 sunCol = float3(1.0, 0.66, 0.30);
		r = Prestige::Screen(r, sunCol * glow * SunGlow);
		// Golden light lifts the shadows a touch towards amber.
		r += float3(0.035, 0.018, 0.0) * WarmthAmount * (1.0 - Prestige::Luma(saturate(r)));
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeGoldenHour <
	ui_label = "Prestige Golden Hour";
	ui_tooltip = "Warm evening light with a soft golden sun glow.";
>
{
	pass Golden { VertexShader = PostProcessVS; PixelShader = PrestigeGoldenHour::PS_Golden; }
}
