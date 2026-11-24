/*
 * Prestige Instant Photo -- the look of an instant camera print.
 *
 * Faded blacks, soft highlights, warm cast with slightly cool shadows, a gentle
 * vignette and, optionally, the white paper frame with the wide bottom edge.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Fade <
	ui_type = "slider"; ui_label = "Fade"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Lifts the blacks and softens the highlights, like an aged print.";
> = 0.5;

uniform float Warmth <
	ui_type = "slider"; ui_label = "Warmth"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Colour cast of the print: positive = warm, negative = cool.";
> = 0.45;

uniform float VignetteAmount <
	ui_type = "slider"; ui_label = "Vignette"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Soft darkening towards the corners of the picture.";
> = 0.4;

uniform bool WhiteFrame <
	ui_label = "White Frame";
	ui_tooltip = "Puts the picture in an instant-print paper frame (wider at the bottom).";
> = false;

namespace PrestigeInstantPhoto
{
	static const float BORDER = 0.05;   // side and top border, in image heights
	static const float BOTTOM = 0.17;   // bottom border

	float3 Look(float3 c, float2 uv)
	{
		// Lower contrast, lifted shadows, rolled-off whites.
		float3 g = Prestige::Saturation(c, 1.0 - 0.25 * Fade);
		g = lerp(float3(0.10, 0.09, 0.11) * Fade, 1.0 - 0.07 * Fade, saturate(g));
		g = Prestige::Warmth(g, Warmth);
		// Instant film: cool-green shadows, creamy highlights.
		const float l = Prestige::Luma(g);
		g += (float3(-0.015, 0.01, 0.025) * (1.0 - l) + float3(0.02, 0.01, -0.02) * l) * Fade * 2.0;
		const float v = Prestige::Vignette(uv, 0.55, 1.1, 0.8);
		return g * lerp(1.0, v, VignetteAmount * 0.6);
	}

	PRESTIGE_PS(PS_Instant)
	{
		const float3 original = Prestige::Source(uv);
		if (!WhiteFrame)
			return Prestige::Finish(original, Look(original, uv), Strength);

		// Cover-fit the photo inside the window of the frame.
		const float2 p = uv * float2(PRESTIGE_ASPECT, 1.0);
		const float2 win = float2(PRESTIGE_ASPECT - 2.0 * BORDER, 1.0 - BORDER - BOTTOM);
		const float2 centre = float2(PRESTIGE_ASPECT * 0.5, BORDER + win.y * 0.5);
		const float s = max(win.x / PRESTIGE_ASPECT, win.y);
		const float2 photoUV = (p - centre) / (float2(PRESTIGE_ASPECT, 1.0) * s) + 0.5;
		float3 photo = Look(tex2Dlod(ReShade::BackBuffer, float4(photoUV, 0, 0)).rgb, photoUV);

		// Paper: warm white with faint fibre noise; a thin shadow where the photo meets it.
		const float2 d = abs(p - centre) - win * 0.5;            // > 0 outside the window
		const float outside = max(d.x, d.y);
		const float edge = 0.6 * BUFFER_RCP_HEIGHT;
		const float paperMask = smoothstep(-edge, edge, outside);
		const float fibre = Prestige::ValueNoise(vpos.xy / (3.0 * PRESTIGE_SCALE), 7u) - 0.5;
		float3 paper = float3(0.955, 0.945, 0.915) + fibre * 0.02;
		paper *= 1.0 - 0.12 * exp(-max(outside, 0.0) / 0.006);    // inner shadow on the paper
		photo *= 1.0 - 0.10 * exp(-max(-outside, 0.0) / 0.004);  // photo edge slightly darker
		return Prestige::Finish(original, lerp(photo, paper, paperMask), Strength);
	}
}

technique PrestigeInstantPhoto <
	ui_label = "Prestige Instant Photo";
	ui_tooltip = "Faded, warm instant-print look with a soft vignette and an optional white paper frame.";
>
{
	pass Instant { VertexShader = PostProcessVS; PixelShader = PrestigeInstantPhoto::PS_Instant; }
}
