/*
 * Prestige Fisheye -- barrel lens distortion.
 *
 * Straight lines bow outwards around the centre like an ultra-wide or fisheye lens.
 * Negative values give pincushion distortion instead. Zoom below 1 shows the round
 * edge of the image circle.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Distortion <
	ui_type = "slider"; ui_label = "Distortion"; ui_min = -0.3; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "Positive = barrel / fisheye, negative = pincushion.";
> = 0.6;

uniform float Zoom <
	ui_type = "slider"; ui_label = "Zoom"; ui_min = 0.6; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "1 = fill the frame; below 1 the image circle's black edge shows.";
> = 1.0;

uniform float EdgeDarkening <
	ui_type = "slider"; ui_label = "Edge Darkening"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Natural light falloff of a wide lens towards the edges.";
> = 0.3;

namespace PrestigeFisheye
{
	PRESTIGE_PS(PS_Fisheye)
	{
		const float3 original = Prestige::Source(uv);
		const float2 d = Prestige::Centered(uv);
		const float corner = length(float2(PRESTIGE_ASPECT, 1.0) * 0.5);
		const float r = length(d) / corner / Zoom;               // 0 centre .. 1 corner (at zoom 1)
		// Source radius: f(1) = 1 so the corners stay put at zoom 1.
		const float k = Distortion;
		const float rs = r * (1.0 + k * r * r) / (1.0 + k);
		const float2 suv = (r > 1e-6 ? d * (rs / max(r * Zoom, 1e-6)) : d) / float2(PRESTIGE_ASPECT, 1.0) + 0.5;

		float3 c = tex2Dlod(ReShade::BackBuffer, float4(suv, 0, 0)).rgb;
		// Outside the source image: black, anti-aliased.
		const float2 outPx = max(abs(suv - 0.5) - 0.5, 0.0) * float2(BUFFER_WIDTH, BUFFER_HEIGHT);
		c *= 1.0 - saturate(max(outPx.x, outPx.y));
		c *= 1.0 - EdgeDarkening * 0.6 * pow(saturate(r), 3.0);
		return Prestige::Finish(original, c, Strength);
	}
}

technique PrestigeFisheye <
	ui_label = "Prestige Fisheye";
	ui_tooltip = "Barrel (fisheye) or pincushion lens distortion.";
>
{
	pass Fisheye { VertexShader = PostProcessVS; PixelShader = PrestigeFisheye::PS_Fisheye; }
}
