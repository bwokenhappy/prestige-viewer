/*
 * Prestige CRT -- an old tube monitor.
 *
 * Curved glass with rounded corners, visible scanlines that thicken on bright lines,
 * an aperture-grille phosphor mask (triad width scales with the snapshot size), a
 * soft phosphor glow and darker edges.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Curvature <
	ui_type = "slider"; ui_label = "Curvature"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Bulge of the glass.";
> = 0.35;

uniform float ScanlineAmount <
	ui_type = "slider"; ui_label = "Scanlines"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Visibility of the dark gaps between scanlines.";
> = 0.55;

uniform float MaskAmount <
	ui_type = "slider"; ui_label = "Phosphor Mask"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Visibility of the red / green / blue phosphor stripes.";
> = 0.4;

uniform float Glow <
	ui_type = "slider"; ui_label = "Glow"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Soft light bleeding around bright parts of the screen.";
> = 0.35;

namespace PrestigeCRT
{
	static const float LINES = 270.0;   // scanlines over the screen height

	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 7; };
	sampler SampSource { Texture = TexSource; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	PRESTIGE_PS(PS_CRT)
	{
		const float3 original = Prestige::Source(uv);
		// Barrel warp of the glass.
		float2 p = (uv - 0.5) * 2.0;
		const float k = Curvature * 0.25;
		p *= 1.0 + k * dot(p * float2(PRESTIGE_ASPECT / 1.78, 1.0), p * float2(PRESTIGE_ASPECT / 1.78, 1.0)) * 0.5;
		p /= 1.0 + k * 0.5;
		const float2 cuv = p * 0.5 + 0.5;

		// Rounded screen corners, anti-aliased.
		const float2 q = abs(p) - (1.0 - 0.06 * (0.5 + Curvature));
		const float cornerR = 0.06 * (0.5 + Curvature);
		const float box = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - cornerR;
		const float inside = 1.0 - smoothstep(-2.0 * BUFFER_RCP_HEIGHT, 2.0 * BUFFER_RCP_HEIGHT, box);

		// Sample on the scanline grid vertically (each line one colour), smooth horizontally.
		const float lineF = cuv.y * LINES;
		const float lineY = (floor(lineF) + 0.5) / LINES;
		const float lineLod = max(0.0, log2(BUFFER_HEIGHT / LINES) - 1.0);   // average the rows of a line
		float3 c = tex2Dlod(SampSource, float4(cuv.x, lineY, 0, lineLod)).rgb;
		const float3 lin = Prestige::ToLinear(c);

		// Beam profile: bright lines are wider, so the gap closes on bright content.
		const float dist = abs(frac(lineF) - 0.5) * 2.0;           // 0 at line centre, 1 in the gap
		const float width = lerp(0.55, 1.0, saturate(Prestige::Luma(lin) * 1.3));
		const float beam = exp(-pow(dist / width, 2.0) * 2.5);
		float3 r = lin * lerp(1.0, beam * 1.6, ScanlineAmount);

		// Aperture grille: R, G, B stripes, one triad = 3 pixels at 1080p.
		const float triad = 3.0 * PRESTIGE_SCALE;
		const float phase = frac(vpos.x / triad);
		const float3 stripes = saturate(1.5 - abs(phase * 3.0 - float3(0.5, 1.5, 2.5)) * 1.2);
		r *= lerp(1.0.xxx, stripes * 1.9 + 0.12, MaskAmount);

		// Phosphor glow from a soft, wide sample.
		const float3 glow = Prestige::ToLinear(Prestige::SoftAverage(SampSource, cuv, 10.0 / 1080.0, BUFFER_HEIGHT).rgb);
		r += glow * Glow * 0.5;

		// Edge darkening of the tube.
		r *= 1.0 - 0.35 * pow(saturate(length(p * float2(0.85, 1.0)) - 0.3), 2.0);
		r = Prestige::ToDisplay(r) * inside;
		return Prestige::Finish(original, r, Strength);
	}
}

technique PrestigeCRT <
	ui_label = "Prestige CRT";
	ui_tooltip = "Old tube monitor: curved glass, scanlines, phosphor mask and glow.";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeCRT::PS_Copy; RenderTarget = PrestigeCRT::TexSource; }
	pass CRT { VertexShader = PostProcessVS; PixelShader = PrestigeCRT::PS_CRT; }
}
