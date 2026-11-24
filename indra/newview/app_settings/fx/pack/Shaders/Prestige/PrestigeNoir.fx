/*
 * Prestige Noir -- hard black and white with film grain.
 *
 * A colour filter decides how colours turn into greys (red darkens skies, green
 * lightens foliage...), then a steep curve, grain and a vignette finish the
 * classic detective-film look.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform int Filter <
	ui_type = "combo"; ui_label = "Colour Filter";
	ui_items = "Neutral\0Red (dark skies)\0Orange\0Yellow\0Green (light foliage)\0Blue\0";
	ui_tooltip = "The lens filter a black-and-white photographer would use: decides which colours turn light or dark.";
> = 1;

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Hardness of the light: deep blacks and bright whites.";
> = 0.7;

uniform float Grain <
	ui_type = "slider"; ui_label = "Grain"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Film grain (size scales with the snapshot).";
> = 0.4;

uniform float VignetteAmount <
	ui_type = "slider"; ui_label = "Vignette"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Darkening towards the corners.";
> = 0.5;

uniform float Timer < source = "timer"; >;

namespace PrestigeNoir
{
	float3 FilterWeights()
	{
		if (Filter == 1) return float3(0.70, 0.25, 0.05);
		if (Filter == 2) return float3(0.55, 0.38, 0.07);
		if (Filter == 3) return float3(0.40, 0.50, 0.10);
		if (Filter == 4) return float3(0.18, 0.72, 0.10);
		if (Filter == 5) return float3(0.10, 0.30, 0.60);
		return float3(0.2126, 0.7152, 0.0722);
	}

	PRESTIGE_PS(PS_Noir)
	{
		const float3 c = Prestige::Source(uv);
		float g = dot(saturate(c), FilterWeights());
		// Steep curve with a little toe crush.
		g = saturate(g + (g - 0.5) * ContrastAmount * 0.6);
		g = lerp(g, g * g * (3.0 - 2.0 * g), ContrastAmount);
		g *= lerp(1.0, Prestige::Vignette(uv, 0.4, 1.2, 0.6), VignetteAmount);

		// Grain: mid-tone weighted, 24 patterns a second, 1.3 px clumps at 1080p.
		const uint seed = Prestige::HashU(uint(Timer * 0.024)) | 1u;
		const float2 p = vpos.xy / (1.3 * PRESTIGE_SCALE);
		const float n = (Prestige::ValueNoise(p, seed) - 0.5) + (Prestige::ValueNoise(p * 2.1 + 5.0, seed + 3u) - 0.5) * 0.6;
		g += n * Grain * 0.3 * (1.0 - abs(g * 2.0 - 1.0) * 0.7);

		return Prestige::Finish(c, g.xxx, Strength);
	}
}

technique PrestigeNoir <
	ui_label = "Prestige Noir";
	ui_tooltip = "High-contrast black and white with a colour filter, grain and vignette.";
>
{
	pass Noir { VertexShader = PostProcessVS; PixelShader = PrestigeNoir::PS_Noir; }
}
