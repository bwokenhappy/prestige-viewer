/*
 * Prestige Teal & Orange -- the blockbuster complementary grade.
 *
 * Shadows and cool colours are pushed towards teal, highlights and warm colours
 * (skin) towards orange. Skin protection keeps faces natural instead of turning them
 * neon.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Amount <
	ui_type = "slider"; ui_label = "Amount"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How far colours are pushed towards teal and orange.";
> = 0.6;

uniform float Balance <
	ui_type = "slider"; ui_label = "Balance"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Negative = more teal, positive = more orange.";
> = 0.0;

uniform float SkinProtect <
	ui_type = "slider"; ui_label = "Skin Protection"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Keeps skin tones natural.";
> = 0.5;

uniform float Punch <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Adds a filmic contrast curve.";
> = 0.3;

namespace PrestigeTealOrange
{
	static const float3 TEAL = float3(0.0, 0.50, 0.56);
	static const float3 ORANGE = float3(1.0, 0.56, 0.22);

	PRESTIGE_PS(PS_Grade)
	{
		const float3 c = Prestige::Source(uv);
		const float l = Prestige::Luma(saturate(c));
		const float3 hsv = Prestige::RGBToHSV(saturate(c));

		// Warm hues (red..yellow) lean orange, cool hues (green..blue) lean teal; the
		// tonal split decides the rest.
		const float warmHue = smoothstep(0.30, 0.0, abs(frac(hsv.x + 0.5) - 0.5 - 0.07) - 0.02);
		const float tonal = smoothstep(0.15, 0.75, l + Balance * 0.25);
		const float toOrange = saturate(lerp(tonal, warmHue, hsv.y * 0.6));

		const float3 target = lerp(TEAL, ORANGE, toOrange);
		float3 graded = Prestige::Tint(c, target, Amount * 0.55);

		// Skin: orange-ish hue with moderate saturation.
		const float skin = smoothstep(0.09, 0.02, abs(hsv.x - 0.07)) * smoothstep(0.1, 0.25, hsv.y) * smoothstep(0.75, 0.45, hsv.y);
		graded = lerp(graded, lerp(c, graded, 0.3), skin * SkinProtect);

		graded = Prestige::SCurve(graded, Punch);
		graded = Prestige::Saturation(graded, 1.0 + Amount * 0.15);
		return Prestige::Finish(c, graded, Strength);
	}
}

technique PrestigeTealOrange <
	ui_label = "Prestige Teal & Orange";
	ui_tooltip = "Blockbuster grade: teal shadows, orange highlights and skin.";
>
{
	pass Grade { VertexShader = PostProcessVS; PixelShader = PrestigeTealOrange::PS_Grade; }
}
