/*
 * Prestige Watercolor -- loose washes of paint on textured paper.
 *
 * Colours are softened into washes that wander slightly (pigment bleeding), pigment
 * collects and darkens along the edges of each wash, the paper's grain shows
 * through (granulation), and the brightest areas are left as bare paper.
 * Sizes are in pixels of a 1080p image; blurs read mip levels of a copy.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float WashSize <
	ui_type = "slider"; ui_label = "Wash Size"; ui_min = 2.0; ui_max = 24.0; ui_step = 0.1;
	ui_tooltip = "How soft and loose the washes are, in pixels of a 1080p image.";
> = 7.0;

uniform float EdgeDarkening <
	ui_type = "slider"; ui_label = "Edge Darkening"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Pigment pooling along the edges of each wash.";
> = 0.6;

uniform float Paper <
	ui_type = "slider"; ui_label = "Paper Texture"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Grain of the cold-pressed paper showing through the paint.";
> = 0.5;

uniform float SaturationAmount <
	ui_type = "slider"; ui_label = "Colour"; ui_min = 0.3; ui_max = 1.6; ui_step = 0.01;
	ui_tooltip = "Pigment saturation.";
> = 1.05;

namespace PrestigeWatercolor
{
	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 10; };
	sampler SampSource { Texture = TexSource; AddressU = MIRROR; AddressV = MIRROR; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	PRESTIGE_PS(PS_Paint)
	{
		const float3 original = Prestige::Source(uv);
		const float radiusUV = WashSize / 1080.0;
		// Pigment wanders: displace the lookup with soft noise.
		const float2 np = Prestige::Centered(uv) * (1080.0 / (WashSize * 6.0));
		const float2 wander = float2(Prestige::ValueNoise(np, 3u), Prestige::ValueNoise(np + 17.0, 5u)) - 0.5;
		const float2 wuv = uv + wander * radiusUV * float2(1.0 / PRESTIGE_ASPECT, 1.0) * 1.5;

		const float3 wash = Prestige::SoftAverage(SampSource, wuv, radiusUV, BUFFER_HEIGHT).rgb;
		const float3 wide = Prestige::SoftAverage(SampSource, wuv, radiusUV * 3.0, BUFFER_HEIGHT).rgb;

		// Soften values into broad tones, keep the hue.
		float3 r = Prestige::Saturation(wash, SaturationAmount);
		// Edge pooling: where the wash differs from its wider surroundings, pigment collects.
		const float edge = saturate(length(wash - wide) * 3.0);
		r *= 1.0 - edge * EdgeDarkening * 0.45;

		// Paper granulation: pigment settles in the paper's valleys.
		const float2 pp = vpos.xy / PRESTIGE_SCALE;
		const float grain = Prestige::Fbm(pp / 6.0, 7u) * 0.7 + Prestige::ValueNoise(pp / 1.6, 9u) * 0.3;
		const float density = 1.0 - Prestige::Luma(r);
		r = 1.0 - (1.0 - r) * (1.0 + (grain - 0.5) * Paper * 0.9 * (0.5 + density));
		// Bare paper in the highlights; paper tint overall.
		const float3 paper = float3(0.985, 0.975, 0.95) - (1.0 - grain) * 0.04 * Paper;
		r = lerp(r, paper, smoothstep(0.82, 0.97, Prestige::Luma(wash)));
		r *= paper;
		return Prestige::Finish(original, r, Strength);
	}
}

technique PrestigeWatercolor <
	ui_label = "Prestige Watercolor";
	ui_tooltip = "Soft watercolour washes with pooled edges on textured paper.";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeWatercolor::PS_Copy; RenderTarget = PrestigeWatercolor::TexSource; }
	pass Paint { VertexShader = PostProcessVS; PixelShader = PrestigeWatercolor::PS_Paint; }
}
