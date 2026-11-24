/*
 * Prestige Slide Film -- classic 1960s-70s colour reversal (slide) film style.
 *
 * Deep, rich shadows; saturated crimson reds, deep blues and warm yellows; greens a
 * little olive; warm highlights and a firm contrast curve.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float SaturationAmount <
	ui_type = "slider"; ui_label = "Saturation"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "Colour richness (reds and blues get the most).";
> = 1.3;

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Depth of the shadows and snap of the mid-tones.";
> = 0.5;

uniform float WarmthAmount <
	ui_type = "slider"; ui_label = "Warmth"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Colour cast of the highlights.";
> = 0.3;

namespace PrestigeSlideFilm
{
	PRESTIGE_PS(PS_Slide)
	{
		const float3 c = Prestige::Source(uv);
		float3 hsv = Prestige::RGBToHSV(saturate(c));
		const float h = hsv.x;
		// Hue-specific saturation: reds (0 / 1) and blues (~0.6) richer, greens tamer.
		const float red = smoothstep(0.08, 0.0, min(h, 1.0 - h));
		const float blue = smoothstep(0.12, 0.0, abs(h - 0.6));
		const float green = smoothstep(0.12, 0.0, abs(h - 0.3));
		hsv.y = saturate(hsv.y * SaturationAmount * (1.0 + 0.25 * red + 0.2 * blue - 0.15 * green));
		hsv.x = frac(hsv.x + green * 0.025 - red * 0.008);      // greens towards olive, reds towards crimson
		hsv.z *= 1.0 - 0.12 * blue * hsv.y;                      // deeper blues
		float3 r = Prestige::HSVToRGB(hsv);

		// Firm toe (rich blacks), gentle shoulder.
		r = pow(saturate(r), 1.0 + 0.25 * ContrastAmount);
		r = Prestige::SCurve(r, ContrastAmount * 0.6);
		const float l = Prestige::Luma(r);
		r = lerp(r, Prestige::Warmth(r, WarmthAmount), smoothstep(0.3, 0.9, l));
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeSlideFilm <
	ui_label = "Prestige Slide Film";
	ui_tooltip = "Vintage colour slide look: rich reds and blues, deep shadows, warm highlights.";
>
{
	pass Slide { VertexShader = PostProcessVS; PixelShader = PrestigeSlideFilm::PS_Slide; }
}
