/*
 * Prestige Dreamy Diffusion -- a mist / diffusion filter in front of the lens.
 *
 * Like a "pro-mist" glass: highlights bloom into soft halos, the whole image gets a
 * faint veil that lifts the blacks and lowers contrast, and skin looks smoother.
 * The halo is a quarter-resolution separable Gaussian (radius scaled with the
 * snapshot size), mixed in linear light.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Diffusion <
	ui_type = "slider"; ui_label = "Diffusion"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Filter strength (like 1/8, 1/4, 1/2... mist grades).";
> = 0.5;

uniform float Radius <
	ui_type = "slider"; ui_label = "Halo Size"; ui_min = 4.0; ui_max = 80.0; ui_step = 0.5;
	ui_tooltip = "Size of the halo, in pixels of a 1080p image.";
> = 24.0;

uniform float HighlightBias <
	ui_type = "slider"; ui_label = "Highlight Bias"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "0 = the whole image is veiled evenly, 1 = mostly the highlights glow.";
> = 0.6;

uniform float BlackLift <
	ui_type = "slider"; ui_label = "Black Lift"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much the mist lifts the shadows (lower contrast).";
> = 0.4;

namespace PrestigeDreamyDiffusion
{
	texture TexPrefilter { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurH { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurV { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
	sampler SampPrefilter { Texture = TexPrefilter; };
	sampler SampBlurH { Texture = TexBlurH; };
	sampler SampBlurV { Texture = TexBlurV; };

	float3 Weighted(float2 uv)
	{
		const float3 c = Prestige::ToLinear(Prestige::Source(uv));
		const float l = Prestige::Luma(c);
		return c * lerp(1.0, smoothstep(0.05, 0.6, l) * 1.5, HighlightBias);
	}

	PRESTIGE_PS(PS_Prefilter)
	{
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
		const float3 h = Weighted(uv + o * float2(-1, -1)) + Weighted(uv + o * float2(1, -1))
		               + Weighted(uv + o * float2(-1, 1)) + Weighted(uv + o * float2(1, 1));
		return float4(h * 0.25, 1.0);
	}

	PRESTIGE_PS(PS_BlurH)
	{
		return Prestige::GaussianBlur(SampPrefilter, uv, float2(4.0 * BUFFER_RCP_WIDTH, 0.0), Radius * PRESTIGE_SCALE * 0.25);
	}

	PRESTIGE_PS(PS_BlurV)
	{
		return Prestige::GaussianBlur(SampBlurH, uv, float2(0.0, 4.0 * BUFFER_RCP_HEIGHT), Radius * PRESTIGE_SCALE * 0.25);
	}

	PRESTIGE_PS(PS_Composite)
	{
		const float3 c = Prestige::Source(uv);
		const float3 lin = Prestige::ToLinear(c);
		const float3 halo = tex2D(SampBlurV, uv).rgb;
		// Mist: part of the light is spread into the halo (energy roughly kept),
		// and a little scattered veil lifts the blacks.
		float3 r = lin * (1.0 - 0.35 * Diffusion) + halo * (0.35 + 0.65 * Diffusion) * Diffusion;
		const float veil = Prestige::Luma(halo) * 0.25 + 0.01;
		r = r + veil * BlackLift * Diffusion * (1.0 - saturate(lin));
		return Prestige::Finish(c, Prestige::ToDisplay(r), Strength);
	}
}

technique PrestigeDreamyDiffusion <
	ui_label = "Prestige Dreamy Diffusion";
	ui_tooltip = "Mist diffusion filter: glowing highlights, soft veil, lifted blacks.";
>
{
	pass Prefilter { VertexShader = PostProcessVS; PixelShader = PrestigeDreamyDiffusion::PS_Prefilter; RenderTarget = PrestigeDreamyDiffusion::TexPrefilter; }
	pass BlurH { VertexShader = PostProcessVS; PixelShader = PrestigeDreamyDiffusion::PS_BlurH; RenderTarget = PrestigeDreamyDiffusion::TexBlurH; }
	pass BlurV { VertexShader = PostProcessVS; PixelShader = PrestigeDreamyDiffusion::PS_BlurV; RenderTarget = PrestigeDreamyDiffusion::TexBlurV; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PrestigeDreamyDiffusion::PS_Composite; }
}
