/*
 * Prestige Night Vision -- an image-intensifier tube.
 *
 * The scene is amplified onto a green phosphor screen: dark areas become visible,
 * bright lights bloom, sensor noise crawls over the image, faint scanlines run
 * across it and the round tube darkens the edges.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Gain <
	ui_type = "slider"; ui_label = "Gain"; ui_min = 1.0; ui_max = 8.0; ui_step = 0.05;
	ui_tooltip = "Amplification of the light.";
> = 3.0;

uniform float Noise <
	ui_type = "slider"; ui_label = "Noise"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Sensor noise (animated).";
> = 0.4;

uniform float TubeVignette <
	ui_type = "slider"; ui_label = "Tube Vignette"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Round dark edge of the intensifier tube (0 = full frame).";
> = 0.7;

uniform float Scanlines <
	ui_type = "slider"; ui_label = "Scanlines"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Faint horizontal lines of the display.";
> = 0.25;

uniform float Timer < source = "timer"; >;

namespace PrestigeNightVision
{
	texture TexSource { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; MipLevels = 6; };
	sampler SampSource { Texture = TexSource; };

	PRESTIGE_PS(PS_Copy)
	{
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT) * 0.5;
		const float3 c = Prestige::Source(uv + o) + Prestige::Source(uv - o)
		               + Prestige::Source(uv + float2(o.x, -o.y)) + Prestige::Source(uv + float2(-o.x, o.y));
		return float4(Prestige::ToLinear(c * 0.25), 1.0);
	}

	PRESTIGE_PS(PS_NV)
	{
		const float3 original = Prestige::Source(uv);
		const float lin = Prestige::Luma(Prestige::ToLinear(original));
		// Bright lights bloom inside the tube.
		const float bloom = Prestige::Luma(Prestige::SoftAverage(SampSource, uv, 12.0 / 1080.0, BUFFER_HEIGHT * 0.5).rgb);
		float v = 1.0 - exp(-(lin + bloom * smoothstep(0.2, 0.8, bloom) * 0.8) * Gain);   // soft-saturating gain

		const uint seed = Prestige::HashU(uint(Timer * 0.03)) | 1u;
		const float n = Prestige::GaussNoise(vpos.xy / (1.2 * PRESTIGE_SCALE), seed);
		v += n * Noise * 0.25 * (0.4 + v);
		v *= 1.0 - Scanlines * 0.3 * (0.5 + 0.5 * cos(uv.y * 540.0 * 6.2831853));

		const float tube = Prestige::Vignette(uv, lerp(2.0, 0.85, TubeVignette), 0.15 + 0.3 * (1.0 - TubeVignette), 1.0);
		v *= lerp(1.0, tube, TubeVignette);

		const float3 phosphor = lerp(float3(0.0, 0.08, 0.02), float3(0.55, 1.0, 0.45), saturate(v)) * saturate(v * 1.2 + 0.05);
		return Prestige::Finish(original, phosphor, Strength);
	}
}

technique PrestigeNightVision <
	ui_label = "Prestige Night Vision";
	ui_tooltip = "Green image-intensifier view with bloom, noise and a round tube. Animated noise.";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeNightVision::PS_Copy; RenderTarget = PrestigeNightVision::TexSource; }
	pass NV { VertexShader = PostProcessVS; PixelShader = PrestigeNightVision::PS_NV; }
}
