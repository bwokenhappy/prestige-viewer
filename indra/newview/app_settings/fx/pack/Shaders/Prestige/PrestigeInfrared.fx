/*
 * Prestige Infrared -- the world seen in near-infrared light.
 *
 * Leaves reflect infrared strongly, so foliage glows. Colour mode imitates false-
 * colour infrared film (infrared shown as red, red as green, green as blue): trees
 * and grass turn red-pink, skies deep blue. Black-and-white mode gives white,
 * snowy foliage under dark skies, with a soft infrared glow.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform int Mode <
	ui_type = "combo"; ui_label = "Mode";
	ui_items = "False colour (red foliage)\0Black and white (white foliage)\0";
	ui_tooltip = "Colour infrared film or black-and-white infrared.";
> = 0;

uniform float Foliage <
	ui_type = "slider"; ui_label = "Foliage Glow"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How strongly green plants light up.";
> = 0.75;

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Contrast of the result (darker skies, brighter leaves).";
> = 0.4;

namespace PrestigeInfrared
{
	PRESTIGE_PS(PS_IR)
	{
		const float3 c = Prestige::Source(uv);
		const float3 x = saturate(c);
		// Foliage: green clearly above red and blue.
		const float leafiness = saturate((x.g - max(x.r, x.b)) * 5.0) * smoothstep(0.05, 0.25, x.g);
		// Infrared brightness: mostly red-ish reflectance, foliage boosted, blue (sky, water) dark.
		const float ir = saturate(0.6 * x.r + 0.35 * x.g - 0.15 * x.b + leafiness * Foliage * 0.9);

		float3 r;
		if (Mode == 0)
		{
			r = float3(ir, x.r, x.g);                              // IR->R, R->G, G->B
			r = Prestige::Saturation(r, 1.15);
		}
		else
		{
			const float g = saturate(ir * 1.1 + leafiness * Foliage * 0.3);
			r = g.xxx;
		}
		r = Prestige::SCurve(r, ContrastAmount);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeInfrared <
	ui_label = "Prestige Infrared";
	ui_tooltip = "Infrared photography: glowing foliage, dark skies; false colour or black and white.";
>
{
	pass IR { VertexShader = PostProcessVS; PixelShader = PrestigeInfrared::PS_IR; }
}
