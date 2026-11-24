/*
 * Prestige Split Toning -- different tints for shadows and highlights.
 *
 * The darkroom / Lightroom classic: shadows lean towards one hue, highlights towards
 * another, with a balance control for where the split sits. Brightness is kept.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float3 ShadowColor <
	ui_type = "color"; ui_label = "Shadow Tint";
	ui_tooltip = "Hue for the shadows.";
> = float3(0.20, 0.45, 0.65);

uniform float3 HighlightColor <
	ui_type = "color"; ui_label = "Highlight Tint";
	ui_tooltip = "Hue for the highlights.";
> = float3(1.0, 0.75, 0.45);

uniform float Balance <
	ui_type = "slider"; ui_label = "Balance"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Negative = the shadow tint reaches further, positive = the highlight tint does.";
> = 0.0;

uniform float Amount <
	ui_type = "slider"; ui_label = "Tint Strength"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How strongly both tints are applied.";
> = 0.5;

namespace PrestigeSplitToning
{
	PRESTIGE_PS(PS_Split)
	{
		const float3 c = Prestige::Source(uv);
		const float l = Prestige::Luma(saturate(c));
		const float pivot = 0.5 - Balance * 0.3;
		const float hi = smoothstep(pivot - 0.35, pivot + 0.35, l);
		const float3 tint = lerp(ShadowColor, HighlightColor, hi);
		// Soft-light the tint (normalised to mid grey) and restore the brightness.
		const float3 layer = 0.5 + (tint - Prestige::Luma(tint)) * Amount;
		float3 r = Prestige::SoftLight(c, layer);
		r *= l / max(Prestige::Luma(r), 1e-4);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeSplitToning <
	ui_label = "Prestige Split Toning";
	ui_tooltip = "Separate colour tints for shadows and highlights.";
>
{
	pass Split { VertexShader = PostProcessVS; PixelShader = PrestigeSplitToning::PS_Split; }
}
