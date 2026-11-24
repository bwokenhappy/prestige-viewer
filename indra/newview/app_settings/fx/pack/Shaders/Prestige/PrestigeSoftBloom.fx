/*
 * Prestige Soft Bloom -- glowing highlights with optional lens dust.
 *
 * Bright parts of the picture bleed a soft, colourful glow (quarter-resolution
 * separable Gaussian, radius scaled with the snapshot size). Lens dust makes faint
 * specks and smudges appear inside the glow, like a lived-in lens.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Threshold <
	ui_type = "slider"; ui_label = "Threshold"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How bright something must be to glow (0 = everything glows a little).";
> = 0.6;

uniform float Radius <
	ui_type = "slider"; ui_label = "Radius"; ui_min = 4.0; ui_max = 120.0; ui_step = 0.5;
	ui_tooltip = "Size of the glow, in pixels of a 1080p image.";
> = 32.0;

uniform float Amount <
	ui_type = "slider"; ui_label = "Amount"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "Brightness of the glow.";
> = 0.6;

uniform float Dust <
	ui_type = "slider"; ui_label = "Lens Dust"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Specks and smudges on the lens that light up inside the glow.";
> = 0.0;

namespace PrestigeSoftBloom
{
	texture TexPrefilter { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurH { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurV { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
	sampler SampPrefilter { Texture = TexPrefilter; };
	sampler SampBlurH { Texture = TexBlurH; };
	sampler SampBlurV { Texture = TexBlurV; };

	float3 Bright(float2 uv)
	{
		const float3 c = Prestige::ToLinear(Prestige::Source(uv));
		const float l = max(c.r, max(c.g, c.b));
		const float t = Threshold * Threshold;              // threshold given in display terms
		const float w = smoothstep(t * 0.8, t + 0.15, l);
		return c * w;
	}

	PRESTIGE_PS(PS_Prefilter)
	{
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
		const float3 h = Bright(uv + o * float2(-1, -1)) + Bright(uv + o * float2(1, -1))
		               + Bright(uv + o * float2(-1, 1)) + Bright(uv + o * float2(1, 1));
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

	// Procedural dust: one soft speck per cell (some cells empty) plus a faint smudge.
	float DustMask(float2 uv)
	{
		const float2 p = Prestige::Centered(uv) * 14.0;
		const float2 cell = floor(p);
		const float3 h = Prestige::Hash3(cell, 41u);
		const float2 centre = cell + 0.2 + h.xy * 0.6;
		const float radius = 0.08 + h.z * 0.22;
		const float speck = (1.0 - smoothstep(radius * 0.5, radius, length(p - centre))) * step(0.45, h.x * h.y + 0.3);
		const float smudge = smoothstep(0.55, 0.85, Prestige::Fbm(p * 0.18, 43u));
		return saturate(speck * 0.8 + smudge * 0.6);
	}

	PRESTIGE_PS(PS_Composite)
	{
		const float3 c = Prestige::Source(uv);
		float3 glow = tex2D(SampBlurV, uv).rgb;
		glow *= 1.0 + DustMask(uv) * Dust * 4.0;
		const float3 lin = Prestige::ToLinear(c) + glow * Amount;
		return Prestige::Finish(c, Prestige::ToDisplay(lin), Strength);
	}
}

technique PrestigeSoftBloom <
	ui_label = "Prestige Soft Bloom";
	ui_tooltip = "Soft glow around bright areas, with optional lens dust.";
>
{
	pass Prefilter { VertexShader = PostProcessVS; PixelShader = PrestigeSoftBloom::PS_Prefilter; RenderTarget = PrestigeSoftBloom::TexPrefilter; }
	pass BlurH { VertexShader = PostProcessVS; PixelShader = PrestigeSoftBloom::PS_BlurH; RenderTarget = PrestigeSoftBloom::TexBlurH; }
	pass BlurV { VertexShader = PostProcessVS; PixelShader = PrestigeSoftBloom::PS_BlurV; RenderTarget = PrestigeSoftBloom::TexBlurV; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PrestigeSoftBloom::PS_Composite; }
}
