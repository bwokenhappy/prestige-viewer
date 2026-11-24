/*
 * Prestige Pastel -- soft, airy, chalky colours.
 *
 * Colours are lightened towards white and their saturation is evened out: strong
 * colours are tamed, dull ones get a little colour, so everything looks like soft
 * pastel chalk. An optional tint sets the mood (pink, mint, lavender...).
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Softness <
	ui_type = "slider"; ui_label = "Lightness"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much the picture is lifted towards white.";
> = 0.5;

uniform float SaturationAmount <
	ui_type = "slider"; ui_label = "Colour"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Pastel saturation: every colour ends up near this level.";
> = 0.45;

uniform float3 TintColor <
	ui_type = "color"; ui_label = "Tint";
	ui_tooltip = "Mood colour washed over the image (white = none).";
> = float3(1.0, 0.92, 0.95);

namespace PrestigePastel
{
	PRESTIGE_PS(PS_Pastel)
	{
		const float3 c = Prestige::Source(uv);
		float3 hsv = Prestige::RGBToHSV(saturate(c));
		hsv.y = lerp(hsv.y, SaturationAmount * 0.6, 0.6) * smoothstep(0.0, 0.08, hsv.y) + hsv.y * 0.1;
		hsv.z = lerp(hsv.z, 0.55 + hsv.z * 0.45, Softness);
		float3 r = Prestige::HSVToRGB(saturate(hsv));
		r = lerp(r, r * TintColor / max(Prestige::Luma(TintColor), 0.05), 0.5);
		r = lerp(r, 1.0.xxx, Softness * 0.12);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigePastel <
	ui_label = "Prestige Pastel";
	ui_tooltip = "Soft, light, chalky pastel colours with a mood tint.";
>
{
	pass Pastel { VertexShader = PostProcessVS; PixelShader = PrestigePastel::PS_Pastel; }
}
