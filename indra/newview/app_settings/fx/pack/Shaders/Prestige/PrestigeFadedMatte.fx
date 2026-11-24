/*
 * Prestige Faded Matte -- lifted, tinted blacks and soft whites.
 *
 * The matte finish of modern editorial and social photography: no true black, a
 * gentle roll-off in the highlights, slightly muted colour, and shadows that carry a
 * chosen tint.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Fade <
	ui_type = "slider"; ui_label = "Black Lift"; ui_min = 0.0; ui_max = 0.4; ui_step = 0.005;
	ui_tooltip = "How grey the blacks become.";
> = 0.12;

uniform float Rolloff <
	ui_type = "slider"; ui_label = "Highlight Roll-off"; ui_min = 0.0; ui_max = 0.3; ui_step = 0.005;
	ui_tooltip = "How much the whites are softened.";
> = 0.06;

uniform float3 ShadowTint <
	ui_type = "color"; ui_label = "Shadow Tint";
	ui_tooltip = "Colour of the lifted blacks.";
> = float3(0.30, 0.32, 0.38);

uniform float Desaturate <
	ui_type = "slider"; ui_label = "Desaturation"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How muted the colours become.";
> = 0.2;

namespace PrestigeFadedMatte
{
	PRESTIGE_PS(PS_Matte)
	{
		const float3 c = Prestige::Source(uv);
		float3 r = Prestige::Saturation(saturate(c), 1.0 - Desaturate);
		// Smooth toe: lift blacks towards the tint, a gentle shoulder at the top.
		const float3 floorCol = ShadowTint * (Fade / max(Prestige::Luma(ShadowTint), 0.05)) * 0.9;
		r = floorCol + r * (1.0 - floorCol - Rolloff);
		r = lerp(r, Prestige::SCurve(r, 0.3), 0.5);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeFadedMatte <
	ui_label = "Prestige Faded Matte";
	ui_tooltip = "Matte editorial look: lifted, tinted blacks, soft whites, muted colour.";
>
{
	pass Matte { VertexShader = PostProcessVS; PixelShader = PrestigeFadedMatte::PS_Matte; }
}
