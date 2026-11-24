/*
 * Prestige Pixel Art -- a retro game screen.
 *
 * The picture is rebuilt from big square pixels (size in pixels of a 1080p image),
 * each the average colour of its area, with a limited number of colour levels and
 * optional ordered (Bayer) dithering between them.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float PixelSize <
	ui_type = "slider"; ui_label = "Pixel Size"; ui_min = 2.0; ui_max = 32.0; ui_step = 0.5;
	ui_tooltip = "Size of one big pixel, in pixels of a 1080p image.";
> = 6.0;

uniform int ColourLevels <
	ui_type = "slider"; ui_label = "Colour Levels"; ui_min = 2; ui_max = 16;
	ui_tooltip = "Shades per colour channel (4 = 64 colours, 8 = 512).";
> = 5;

uniform float Dither <
	ui_type = "slider"; ui_label = "Dithering"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Checkerboard dithering that fakes in-between colours.";
> = 0.6;

uniform float SaturationAmount <
	ui_type = "slider"; ui_label = "Saturation"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "Colour punch, for that console-palette look.";
> = 1.2;

namespace PrestigePixelArt
{
	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 9; };
	sampler SampSource { Texture = TexSource; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	float Bayer4(int2 p)
	{
		// 4x4 ordered-dither threshold built from the 2x2 pattern, 0..1.
		const int2 a = p & 1;
		const int2 b = (p >> 1) & 1;
		const int v2a = (a.x ^ a.y) * 2 + a.y;   // 0 2 / 3 1
		const int v2b = (b.x ^ b.y) * 2 + b.y;
		return (v2a * 4 + v2b + 0.5) / 16.0;
	}

	PRESTIGE_PS(PS_Pixel)
	{
		const float3 original = Prestige::Source(uv);
		const float cell = PixelSize * PRESTIGE_SCALE;
		// Grid anchored on the image centre so the frame is symmetric.
		const float2 centrePx = float2(BUFFER_WIDTH, BUFFER_HEIGHT) * 0.5;
		const float2 cellIndex = floor((vpos.xy - centrePx) / cell);
		const float2 cellCentre = (cellIndex + 0.5) * cell + centrePx;
		const float lod = max(0.0, log2(cell) - 0.5);
		float3 c = tex2Dlod(SampSource, float4(cellCentre * float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT), 0, lod)).rgb;
		c = saturate(Prestige::Saturation(c, SaturationAmount));

		const float n = ColourLevels - 1.0;
		const float threshold = lerp(0.5, Bayer4(int2(cellIndex) & 3), Dither);
		const float3 q = floor(c * n + threshold) / n;
		return Prestige::Finish(original, q, Strength);
	}
}

technique PrestigePixelArt <
	ui_label = "Prestige Pixel Art";
	ui_tooltip = "Big square pixels, limited colours and dithering, like a retro game.";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigePixelArt::PS_Copy; RenderTarget = PrestigePixelArt::TexSource; }
	pass Pixel { VertexShader = PostProcessVS; PixelShader = PrestigePixelArt::PS_Pixel; }
}
