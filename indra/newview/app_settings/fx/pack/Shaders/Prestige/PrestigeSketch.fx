/*
 * Prestige Sketch -- pencil drawing on paper.
 *
 * Outlines come from the image's edges (Sobel on a mip-mapped copy, so the line
 * width is the same at any snapshot size); shadows are hatched with diagonal pencil
 * strokes that cross over in the darkest areas; the paper has a light tooth.
 * Optionally keeps some of the original colour, like a coloured-pencil sketch.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float LineStrength <
	ui_type = "slider"; ui_label = "Line Strength"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "Darkness of the outlines.";
> = 1.0;

uniform float LineWidth <
	ui_type = "slider"; ui_label = "Line Width"; ui_min = 0.5; ui_max = 4.0; ui_step = 0.05;
	ui_tooltip = "Width of the pencil outlines, in pixels of a 1080p image.";
> = 1.2;

uniform float Shading <
	ui_type = "slider"; ui_label = "Hatching"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much the shadows are shaded with diagonal pencil strokes.";
> = 0.6;

uniform float Colour <
	ui_type = "slider"; ui_label = "Colour"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "0 = graphite pencil, 1 = coloured pencils.";
> = 0.0;

namespace PrestigeSketch
{
	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 6; };
	sampler SampSource { Texture = TexSource; AddressU = MIRROR; AddressV = MIRROR; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	// Diagonal strokes: 1 on a stroke, 0 between; direction +-1, spacing in pixels.
	float Hatch(float2 pos, float dir, float spacing, float tone, uint seed)
	{
		const float2 p = pos / spacing;
		const float d = p.x + dir * p.y;
		const float wobble = (Prestige::ValueNoise(float2(p.x - dir * p.y, d) * 0.15, seed) - 0.5) * 0.6;
		const float f = abs(frac(d + wobble) - 0.5) * 2.0;           // 0 at a stroke's middle
		return 1.0 - smoothstep(tone * 0.9, tone * 0.9 + 0.25, f);   // darker tone = wider strokes
	}

	PRESTIGE_PS(PS_Sketch)
	{
		const float3 original = Prestige::Source(uv);
		const float widthPx = LineWidth * PRESTIGE_SCALE;
		const float lod = max(0.0, log2(widthPx) - 0.5);
		const float3 soft = tex2Dlod(SampSource, float4(uv, 0, lod + 1.0)).rgb;
		const float edge = Prestige::SobelLuma(SampSource, uv, widthPx * float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT), lod);
		const float line = smoothstep(0.08, 0.45, edge * LineStrength);

		// Tone for the hatching: inverted, gently contrasted luma.
		const float l = Prestige::Luma(soft);
		const float tone = saturate((1.0 - l) * 1.15 - 0.1);
		const float spacing = 5.0 * PRESTIGE_SCALE;
		float shade = Hatch(vpos.xy, 1.0, spacing, saturate(tone - 0.25), 3u) * smoothstep(0.25, 0.45, tone);
		shade = max(shade, Hatch(vpos.xy, -1.0, spacing * 1.1, saturate(tone - 0.5), 7u) * smoothstep(0.5, 0.7, tone));
		shade *= Shading;

		// Paper with a little tooth; pencil graphite is slightly warm grey.
		const float tooth = Prestige::ValueNoise(vpos.xy / (1.5 * PRESTIGE_SCALE), 13u);
		const float3 paper = float3(0.97, 0.96, 0.93) - tooth * 0.035;
		const float graphite = saturate(line * 0.85 + shade * 0.55) * (0.85 + tooth * 0.3);
		float3 drawing = paper * (1.0 - graphite * float3(0.80, 0.80, 0.78));

		// Coloured pencil: tint the drawing with the original colour.
		const float3 pencil = lerp(1.0.xxx, Prestige::Saturation(soft, 1.2) * 1.1, 0.75);
		drawing = lerp(drawing, drawing * pencil, Colour);
		return Prestige::Finish(original, drawing, Strength);
	}
}

technique PrestigeSketch <
	ui_label = "Prestige Sketch";
	ui_tooltip = "Pencil sketch: outlines, hatched shadows and paper texture.";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeSketch::PS_Copy; RenderTarget = PrestigeSketch::TexSource; }
	pass Sketch { VertexShader = PostProcessVS; PixelShader = PrestigeSketch::PS_Sketch; }
}
