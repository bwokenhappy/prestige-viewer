/*
 * Prestige Cinema Bars -- letterbox (or pillarbox) to a cinema aspect ratio.
 *
 * Wider ratios than the image get black bars top and bottom; narrower ones get bars
 * at the sides. Works for any snapshot size because it is computed from the actual
 * image aspect ratio.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform int AspectRatio <
	ui_type = "combo"; ui_label = "Aspect Ratio";
	ui_items = "Scope 2.39:1\0Flat 1.85:1\0Univisium 2.00:1\0Ultra wide 2.76:1\0Classic 4:3 (1.33:1)\0";
	ui_tooltip = "The frame shape to crop to.";
> = 0;

uniform float Opacity <
	ui_type = "slider"; ui_label = "Bar Opacity"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "1 = solid black bars, lower = see-through bars (handy for framing).";
> = 1.0;

uniform float Position <
	ui_type = "slider"; ui_label = "Frame Position"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Moves the picture window between the bars (up / left at -1, down / right at +1).";
> = 0.0;

uniform float Softness <
	ui_type = "slider"; ui_label = "Edge Softness"; ui_min = 0.0; ui_max = 8.0; ui_step = 0.1;
	ui_tooltip = "Feathers the bar edge, in pixels of a 1080p image.";
> = 0.0;

namespace PrestigeCinemaBars
{
	float TargetAspect()
	{
		if (AspectRatio == 1) return 1.85;
		if (AspectRatio == 2) return 2.0;
		if (AspectRatio == 3) return 2.76;
		if (AspectRatio == 4) return 4.0 / 3.0;
		return 2.39;
	}

	PRESTIGE_PS(PS_Bars)
	{
		const float3 c = Prestige::Source(uv);
		const float target = TargetAspect();
		float inside;
		const float feather = max(Softness, 0.5) * PRESTIGE_SCALE;
		if (target >= PRESTIGE_ASPECT)
		{
			const float h = PRESTIGE_ASPECT / target;                 // window height in UV
			const float centre = 0.5 + Position * (1.0 - h) * 0.5;
			const float distPx = (h * 0.5 - abs(uv.y - centre)) * BUFFER_HEIGHT;
			inside = saturate(distPx / feather + 0.5);
		}
		else
		{
			const float w = target / PRESTIGE_ASPECT;                 // window width in UV
			const float centre = 0.5 + Position * (1.0 - w) * 0.5;
			const float distPx = (w * 0.5 - abs(uv.x - centre)) * BUFFER_WIDTH;
			inside = saturate(distPx / feather + 0.5);
		}
		const float3 result = c * lerp(1.0 - Opacity, 1.0, inside);
		return Prestige::Finish(c, result, Strength);
	}
}

technique PrestigeCinemaBars <
	ui_label = "Prestige Cinema Bars";
	ui_tooltip = "Black bars for a cinema aspect ratio: 2.39:1, 1.85:1, 2.00:1 and more.";
>
{
	pass Bars { VertexShader = PostProcessVS; PixelShader = PrestigeCinemaBars::PS_Bars; }
}
