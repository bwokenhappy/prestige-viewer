/*
 * Prestige Halation -- the red-orange glow film gets around bright highlights.
 *
 * On film, strong light passes through the emulsion, bounces off the film base and
 * exposes the red layer again from behind: bright edges get a warm, red halo.
 * Highlights are extracted at quarter resolution, blurred with a separable Gaussian
 * (radius scaled with the snapshot size) and added back tinted.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Threshold <
	ui_type = "slider"; ui_label = "Threshold"; ui_min = 0.4; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How bright a highlight must be to glow.";
> = 0.72;

uniform float Radius <
	ui_type = "slider"; ui_label = "Radius"; ui_min = 2.0; ui_max = 60.0; ui_step = 0.5;
	ui_tooltip = "Size of the glow, in pixels of a 1080p image (scaled for bigger snapshots).";
> = 14.0;

uniform float Amount <
	ui_type = "slider"; ui_label = "Amount"; ui_min = 0.0; ui_max = 3.0; ui_step = 0.01;
	ui_tooltip = "Brightness of the glow.";
> = 1.2;

uniform float3 GlowColor <
	ui_type = "color"; ui_label = "Glow Colour";
	ui_tooltip = "Colour of the halo. Film halation is red to orange.";
> = float3(1.0, 0.22, 0.06);

namespace PrestigeHalation
{
	texture TexPrefilter { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurH { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurV { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
	sampler SampPrefilter { Texture = TexPrefilter; };
	sampler SampBlurH { Texture = TexBlurH; };
	sampler SampBlurV { Texture = TexBlurV; };

	float3 Highlight(float2 uv)
	{
		const float3 c = Prestige::Source(uv);
		const float l = max(c.r, max(c.g, c.b));
		const float knee = 0.08;
		const float w = smoothstep(Threshold - knee, Threshold + knee, l) * (l - Threshold * 0.5);
		return c * max(w, 0.0) / max(l, 1e-4);
	}

	PRESTIGE_PS(PS_Prefilter)
	{
		// The quarter-resolution texel covers 4x4 pixels: four bilinear taps average it.
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
		const float3 h = Highlight(uv + o * float2(-1, -1)) + Highlight(uv + o * float2(1, -1))
		               + Highlight(uv + o * float2(-1, 1)) + Highlight(uv + o * float2(1, 1));
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
		const float glowLuma = Prestige::Luma(tex2D(SampBlurV, uv).rgb);
		// Halation is a single-colour halo: use the glow's brightness, coloured.
		const float3 glow = GlowColor * glowLuma * Amount;
		const float3 result = c + glow * (1.0 - saturate(c) * 0.6);
		return Prestige::Finish(c, result, Strength);
	}
}

technique PrestigeHalation <
	ui_label = "Prestige Halation";
	ui_tooltip = "Warm red glow around bright highlights, like light bouncing inside film.";
>
{
	pass Prefilter { VertexShader = PostProcessVS; PixelShader = PrestigeHalation::PS_Prefilter; RenderTarget = PrestigeHalation::TexPrefilter; }
	pass BlurH { VertexShader = PostProcessVS; PixelShader = PrestigeHalation::PS_BlurH; RenderTarget = PrestigeHalation::TexBlurH; }
	pass BlurV { VertexShader = PostProcessVS; PixelShader = PrestigeHalation::PS_BlurV; RenderTarget = PrestigeHalation::TexBlurV; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PrestigeHalation::PS_Composite; }
}
