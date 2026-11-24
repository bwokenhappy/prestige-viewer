/*
 * Prestige Sharpen & Clarity -- crisp detail and mid-tone punch.
 *
 * Sharpen boosts fine detail (radius ~1 px at 1080p, scaled for bigger snapshots so
 * an 8K shot is not over-sharpened at the pixel level). Clarity boosts local
 * contrast over a larger radius, mostly in the mid-tones, for texture and depth
 * without crunchy highlights. Both work on brightness only (no colour fringes) and
 * are limited near strong edges to avoid halos.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Sharpen <
	ui_type = "slider"; ui_label = "Sharpen"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "Fine-detail sharpening.";
> = 0.6;

uniform float Clarity <
	ui_type = "slider"; ui_label = "Clarity"; ui_min = -1.0; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "Mid-tone local contrast (negative values soften, for skin).";
> = 0.4;

uniform float ClarityRadius <
	ui_type = "slider"; ui_label = "Clarity Radius"; ui_min = 5.0; ui_max = 80.0; ui_step = 0.5;
	ui_tooltip = "Size of the local-contrast detail, in pixels of a 1080p image.";
> = 25.0;

namespace PrestigeSharpenClarity
{
	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 10; };
	sampler SampSource { Texture = TexSource; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	PRESTIGE_PS(PS_Sharpen)
	{
		const float3 c = Prestige::Source(uv);
		const float l = Prestige::Luma(c);

		// Small blur: 4 diagonal taps ~0.8 px (at 1080p) away, at the matching mip.
		const float r = 0.8 * PRESTIGE_SCALE;
		const float lod = max(0.0, log2(r) - 0.5);
		const float2 o = r * float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
		const float small = Prestige::Luma(tex2Dlod(SampSource, float4(uv + o, 0, lod)).rgb
		                                 + tex2Dlod(SampSource, float4(uv - o, 0, lod)).rgb
		                                 + tex2Dlod(SampSource, float4(uv + float2(o.x, -o.y), 0, lod)).rgb
		                                 + tex2Dlod(SampSource, float4(uv + float2(-o.x, o.y), 0, lod)).rgb) * 0.25;
		// The centre at the same mip, so 4K/8K compare like with like.
		const float centre = Prestige::Luma(tex2Dlod(SampSource, float4(uv, 0, lod)).rgb);
		float detail = clamp(centre - small, -0.08, 0.08);          // limit: no halos on hard edges

		// Clarity: large-radius local contrast, mid-tone weighted.
		const float wide = Prestige::Luma(Prestige::SoftAverage(SampSource, uv, ClarityRadius / 1080.0, BUFFER_HEIGHT).rgb);
		const float mid = 1.0 - pow(abs(l * 2.0 - 1.0), 2.0);
		const float local = clamp(l - wide, -0.2, 0.2) * mid;

		const float newL = max(l + detail * Sharpen * 1.5 + local * Clarity, 0.0);
		const float3 result = c * (newL / max(l, 1e-4));
		return Prestige::Finish(c, lerp(c + (newL - l), result, saturate(l * 8.0)), Strength);
	}
}

technique PrestigeSharpenClarity <
	ui_label = "Prestige Sharpen & Clarity";
	ui_tooltip = "Crisp fine detail plus mid-tone local contrast; scaled for any snapshot size.";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeSharpenClarity::PS_Copy; RenderTarget = PrestigeSharpenClarity::TexSource; }
	pass Sharpen { VertexShader = PostProcessVS; PixelShader = PrestigeSharpenClarity::PS_Sharpen; }
}
