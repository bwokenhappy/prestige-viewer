/*
 * Prestige Oil Paint -- painterly Kuwahara filter.
 *
 * Each pixel looks at four overlapping, softly weighted quadrants around it and takes
 * the colour of the calmest ones (lowest variance). Flat areas turn into smooth
 * strokes while edges stay crisp. The brush size is in pixels of a 1080p image;
 * sampling reads mip levels so a 4K / 8K snapshot paints with the same strokes.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float BrushSize <
	ui_type = "slider"; ui_label = "Brush Size"; ui_min = 1.5; ui_max = 14.0; ui_step = 0.1;
	ui_tooltip = "Radius of a paint stroke, in pixels of a 1080p image.";
> = 5.0;

uniform float Sharpness <
	ui_type = "slider"; ui_label = "Edge Sharpness"; ui_min = 1.0; ui_max = 12.0; ui_step = 0.1;
	ui_tooltip = "How strictly the calmest quadrant wins: higher = crisper edges, lower = softer, blended strokes.";
> = 7.0;

uniform float PaintSaturation <
	ui_type = "slider"; ui_label = "Saturation"; ui_min = 0.5; ui_max = 1.8; ui_step = 0.01;
	ui_tooltip = "Colour richness of the paint.";
> = 1.15;

uniform float Canvas <
	ui_type = "slider"; ui_label = "Canvas Texture"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Woven canvas showing through the paint.";
> = 0.35;

namespace PrestigeOilPaint
{
	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 5; };
	sampler SampSource { Texture = TexSource; AddressU = MIRROR; AddressV = MIRROR; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	PRESTIGE_PS(PS_Kuwahara)
	{
		const float3 original = Prestige::Source(uv);
		const float radiusPx = BrushSize * PRESTIGE_SCALE;        // in this buffer's pixels
		const float stepPx = radiusPx / 3.0;
		const float lod = max(0.0, log2(stepPx) - 0.5);
		const float2 stepUV = stepPx * float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);

		float3 mean[4], sq[4];
		float wsum[4];
		[unroll]
		for (int k = 0; k < 4; ++k) { mean[k] = 0.0; sq[k] = 0.0; wsum[k] = 0.0; }

		[unroll]
		for (int y = -3; y <= 3; ++y)
		{
			[unroll]
			for (int x = -3; x <= 3; ++x)
			{
				const float2 o = float2(x, y) / 3.0;
				const float r2 = dot(o, o);
				if (r2 > 1.05) continue;
				const float3 c = tex2Dlod(SampSource, float4(uv + float2(x, y) * stepUV, 0, lod)).rgb;
				const float g = exp(-1.5 * r2);
				// Soft quadrant memberships (overlap along the axes).
				const float wxp = smoothstep(-0.4, 0.4, o.x), wxn = 1.0 - wxp;
				const float wyp = smoothstep(-0.4, 0.4, o.y), wyn = 1.0 - wyp;
				const float w[4] = { wxp * wyp * g, wxn * wyp * g, wxn * wyn * g, wxp * wyn * g };
				[unroll]
				for (int q = 0; q < 4; ++q)
				{
					mean[q] += c * w[q];
					sq[q] += c * c * w[q];
					wsum[q] += w[q];
				}
			}
		}

		float3 result = 0.0;
		float total = 0.0;
		[unroll]
		for (int q = 0; q < 4; ++q)
		{
			const float3 m = mean[q] / wsum[q];
			const float3 v = abs(sq[q] / wsum[q] - m * m);
			const float sigma = sqrt(v.r + v.g + v.b);
			const float w = 1.0 / (1.0 + pow(sigma * 20.0, Sharpness));
			result += m * w;
			total += w;
		}
		result /= max(total, 1e-6);
		result = Prestige::Saturation(result, PaintSaturation);

		// Canvas weave: two crossing sine threads with a little irregularity.
		const float2 p = vpos.xy / (2.2 * PRESTIGE_SCALE);
		const float wobble = Prestige::ValueNoise(p * 0.25, 5u) * 2.0;
		const float weave = sin(p.x + wobble) * sin(p.y - wobble);
		result *= 1.0 + weave * 0.06 * Canvas;
		result += (Prestige::ValueNoise(p * 0.5, 11u) - 0.5) * 0.04 * Canvas;

		return Prestige::Finish(original, result, Strength);
	}
}

technique PrestigeOilPaint <
	ui_label = "Prestige Oil Paint";
	ui_tooltip = "Turns the picture into a painting with smooth strokes and crisp edges (Kuwahara filter).";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeOilPaint::PS_Copy; RenderTarget = PrestigeOilPaint::TexSource; }
	pass Paint { VertexShader = PostProcessVS; PixelShader = PrestigeOilPaint::PS_Kuwahara; }
}
