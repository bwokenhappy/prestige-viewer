/*
 * Prestige Moonlight -- a cool, dim night ("day for night").
 *
 * Our eyes lose colour and shift towards blue in the dark: the image is darkened,
 * desaturated and tinted blue, more so in the shadows, while bright lights keep a
 * little of their colour so lamps still glow.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Blueness <
	ui_type = "slider"; ui_label = "Blue Tint"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Strength of the moonlit blue cast.";
> = 0.65;

uniform float Darkness <
	ui_type = "slider"; ui_label = "Darkness"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much darker the scene becomes.";
> = 0.45;

uniform float Desaturate <
	ui_type = "slider"; ui_label = "Desaturation"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much colour the night takes away (bright lights keep some).";
> = 0.6;

namespace PrestigeMoonlight
{
	PRESTIGE_PS(PS_Moon)
	{
		const float3 c = Prestige::Source(uv);
		const float l = Prestige::Luma(saturate(c));
		const float lights = smoothstep(0.7, 0.95, l);           // lamps keep their colour
		float3 r = Prestige::Saturation(c, lerp(1.0 - Desaturate, 1.0, lights));
		r = Prestige::Tint(r, float3(0.45, 0.6, 1.0), Blueness * (0.85 - 0.6 * l) * (1.0 - lights));
		// Darken with a gamma (keeps highlights), plus a silver lift in the brights.
		r = pow(max(r, 0.0), 1.0 + Darkness * 1.2) * (1.0 - Darkness * 0.25);
		r += float3(0.0, 0.01, 0.03) * Blueness * smoothstep(0.4, 1.0, l);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeMoonlight <
	ui_label = "Prestige Cool Moonlight";
	ui_tooltip = "Cool, dim, blue moonlit night; lamps keep their glow.";
>
{
	pass Moon { VertexShader = PostProcessVS; PixelShader = PrestigeMoonlight::PS_Moon; }
}
