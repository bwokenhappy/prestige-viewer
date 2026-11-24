/*
 * Prestige Duotone -- the image printed with two inks.
 *
 * Brightness is mapped onto a smooth gradient between a shadow colour and a
 * highlight colour (poster and album-cover style).
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float3 ShadowColor <
	ui_type = "color"; ui_label = "Shadow Colour";
	ui_tooltip = "Ink used for the dark parts.";
> = float3(0.10, 0.05, 0.35);

uniform float3 HighlightColor <
	ui_type = "color"; ui_label = "Highlight Colour";
	ui_tooltip = "Ink used for the bright parts.";
> = float3(1.0, 0.55, 0.45);

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Steepness of the gradient between the two colours.";
> = 0.35;

namespace PrestigeDuotone
{
	PRESTIGE_PS(PS_Duotone)
	{
		const float3 c = Prestige::Source(uv);
		float l = Prestige::Luma(saturate(c));
		l = lerp(l, l * l * (3.0 - 2.0 * l), ContrastAmount);
		// Blend in linear light so the midpoint is not muddy.
		const float3 lin = lerp(pow(ShadowColor, 2.2), pow(HighlightColor, 2.2), l);
		return Prestige::Finish(c, pow(lin, 1.0 / 2.2), Strength);
	}
}

technique PrestigeDuotone <
	ui_label = "Prestige Duotone";
	ui_tooltip = "Two-colour print: shadows in one colour, highlights in another.";
>
{
	pass Duotone { VertexShader = PostProcessVS; PixelShader = PrestigeDuotone::PS_Duotone; }
}
