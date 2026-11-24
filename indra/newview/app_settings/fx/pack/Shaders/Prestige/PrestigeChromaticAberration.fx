/*
 * Prestige Chromatic Aberration -- lateral colour fringing of a real lens.
 *
 * Red and blue are magnified slightly differently, growing towards the corners. The
 * fringe is integrated over seven taps so it is a smooth rainbow instead of three
 * hard ghost copies. The fringe width is given in pixels of a 1080p image.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Amount <
	ui_type = "slider"; ui_label = "Amount"; ui_min = 0.0; ui_max = 12.0; ui_step = 0.1;
	ui_tooltip = "Width of the colour fringe at the corners, in pixels of a 1080p image.";
> = 3.0;

uniform float Falloff <
	ui_type = "slider"; ui_label = "Falloff"; ui_min = 1.0; ui_max = 4.0; ui_step = 0.05;
	ui_tooltip = "How quickly the fringe grows towards the edges (higher = clean centre, fringes only near the corners).";
> = 2.0;

uniform bool SwapColors <
	ui_label = "Swap Fringe Colours";
	ui_tooltip = "Off: red fringes outwards, blue inwards. On: the opposite.";
> = false;

namespace PrestigeChromaticAberration
{
	PRESTIGE_PS(PS_CA)
	{
		const float3 original = Prestige::Source(uv);
		const float2 d = Prestige::Centered(uv);
		const float corner = length(float2(PRESTIGE_ASPECT, 1.0) * 0.5);
		const float r = length(d) / corner;
		// Fringe half-width in UV of height at this radius.
		const float shift = (Amount / 1080.0) * pow(r, Falloff) * 0.5 * (SwapColors ? -1.0 : 1.0);
		const float2 dirUV = (r > 1e-5 ? d / length(d) : float2(0, 0)) * float2(1.0 / PRESTIGE_ASPECT, 1.0);

		float3 sum = 0.0, wsum = 0.0;
		[unroll]
		for (int i = 0; i < 7; ++i)
		{
			const float t = i / 3.0 - 1.0;   // -1 .. 1
			const float3 w = saturate(1.0 - abs(t - float3(1.0, 0.0, -1.0)) * 0.75);
			sum += tex2Dlod(Prestige::BackBufferMirror, float4(uv + dirUV * shift * t, 0, 0)).rgb * w;
			wsum += w;
		}
		return Prestige::Finish(original, sum / wsum, Strength);
	}
}

technique PrestigeChromaticAberration <
	ui_label = "Prestige Chromatic Aberration";
	ui_tooltip = "Smooth red / blue colour fringes towards the corners, like an uncorrected lens.";
>
{
	pass CA { VertexShader = PostProcessVS; PixelShader = PrestigeChromaticAberration::PS_CA; }
}
