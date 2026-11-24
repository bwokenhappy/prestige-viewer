/*
 * Prestige Vignette -- darkened (or lightened) corners.
 *
 * Shape from round to frame-shaped, adjustable size and softness. Negative amounts
 * give a white, dreamy vignette. Computed in linear light so it looks like a real
 * lens falloff rather than a grey smudge.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Amount <
	ui_type = "slider"; ui_label = "Amount"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Positive = dark corners, negative = white corners.";
> = 0.5;

uniform float Size <
	ui_type = "slider"; ui_label = "Size"; ui_min = 0.0; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "How large the untouched centre is.";
> = 0.5;

uniform float Softness <
	ui_type = "slider"; ui_label = "Softness"; ui_min = 0.05; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "Width of the transition into the corners.";
> = 0.9;

uniform float Roundness <
	ui_type = "slider"; ui_label = "Roundness"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "1 = a circle, 0 = follows the shape of the frame.";
> = 0.75;

namespace PrestigeVignette
{
	PRESTIGE_PS(PS_Vignette)
	{
		const float3 c = Prestige::Source(uv);
		const float v = 1.0 - Prestige::Vignette(uv, Size, Softness, Roundness);   // 0 centre .. 1 corners
		float3 lin = Prestige::ToLinear(c);
		if (Amount >= 0.0)
			lin *= 1.0 - v * Amount;
		else
			lin = lerp(lin, Prestige::ToLinear(1.0.xxx), v * -Amount);
		return Prestige::Finish(c, Prestige::ToDisplay(lin), Strength);
	}
}

technique PrestigeVignette <
	ui_label = "Prestige Vignette";
	ui_tooltip = "Dark or white corners with adjustable shape, size and softness.";
>
{
	pass Vignette { VertexShader = PostProcessVS; PixelShader = PrestigeVignette::PS_Vignette; }
}
