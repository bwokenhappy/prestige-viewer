/*
 * Prestige Halftone -- printed with dots, like a comic book or a newspaper.
 *
 * Colour mode separates the image into cyan, magenta, yellow and black inks, each a
 * grid of round dots on its own screen angle; ink mode prints black dots only. The
 * dot size is in pixels of a 1080p image, and each dot reads the average colour of
 * its cell from a mip-mapped copy, so any snapshot size prints the same pattern.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float DotSize <
	ui_type = "slider"; ui_label = "Dot Size"; ui_min = 3.0; ui_max = 30.0; ui_step = 0.5;
	ui_tooltip = "Spacing of the dot grid, in pixels of a 1080p image.";
> = 8.0;

uniform int Mode <
	ui_type = "combo"; ui_label = "Inks";
	ui_items = "Colour (CMYK)\0Black ink\0";
	ui_tooltip = "Four-colour print or black dots only.";
> = 0;

uniform float PaperTone <
	ui_type = "slider"; ui_label = "Paper Age"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "0 = bright white paper, 1 = yellowed newsprint.";
> = 0.3;

namespace PrestigeHalftone
{
	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 8; };
	sampler SampSource { Texture = TexSource; AddressU = MIRROR; AddressV = MIRROR; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	float4 ToCMYK(float3 c)
	{
		const float k = 1.0 - max(c.r, max(c.g, c.b));
		const float3 cmy = (1.0 - c - k) / max(1.0 - k, 1e-4);
		return float4(cmy, k);
	}

	// Coverage (0..1) of one ink screen at pixel pos; channel picks the ink.
	float Screen(float2 pos, float angleDeg, float cell, int channel)
	{
		const float a = radians(angleDeg);
		const float2x2 rot = float2x2(cos(a), -sin(a), sin(a), cos(a));
		const float2 p = mul(rot, pos) / cell;
		const float2 centre = floor(p) + 0.5;
		const float2 centrePos = mul(transpose(rot), centre * cell);      // back to pixels
		const float2 centreUV = centrePos * float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
		const float lod = max(0.0, log2(cell) - 1.0);
		const float3 col = tex2Dlod(SampSource, float4(centreUV, 0, lod)).rgb;
		float ink;
		if (channel == 4)
			ink = 1.0 - Prestige::Luma(col);
		else
		{
			const float4 cmyk = ToCMYK(col);
			ink = channel == 0 ? cmyk.x : channel == 1 ? cmyk.y : channel == 2 ? cmyk.z : cmyk.w;
		}
		// Dot area proportional to ink: radius = sqrt(ink / pi) cells, anti-aliased.
		const float radius = sqrt(saturate(ink) / 3.14159265) * 1.08;
		const float d = length(p - centre);
		const float aa = 0.75 / cell;
		return 1.0 - smoothstep(radius - aa, radius + aa, d);
	}

	PRESTIGE_PS(PS_Halftone)
	{
		const float3 original = Prestige::Source(uv);
		const float cell = DotSize * PRESTIGE_SCALE;
		const float2 pos = vpos.xy;
		const float3 paper = lerp(float3(0.98, 0.97, 0.95), float3(0.93, 0.88, 0.74), PaperTone);
		float3 r;
		if (Mode == 1)
		{
			r = paper * (1.0 - Screen(pos, 45.0, cell, 4) * 0.92);
		}
		else
		{
			const float cyan = Screen(pos, 15.0, cell, 0);
			const float magenta = Screen(pos, 75.0, cell, 1);
			const float yellow = Screen(pos, 0.0, cell, 2);
			const float black = Screen(pos, 45.0, cell, 3);
			// Subtractive inks on paper.
			r = paper;
			r *= 1.0 - cyan * float3(0.95, 0.25, 0.05);
			r *= 1.0 - magenta * float3(0.05, 0.9, 0.3);
			r *= 1.0 - yellow * float3(0.02, 0.08, 0.9);
			r *= 1.0 - black * 0.9;
		}
		return Prestige::Finish(original, r, Strength);
	}
}

technique PrestigeHalftone <
	ui_label = "Prestige Halftone";
	ui_tooltip = "Comic / newspaper print made of CMYK or black dots.";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeHalftone::PS_Copy; RenderTarget = PrestigeHalftone::TexSource; }
	pass Halftone { VertexShader = PostProcessVS; PixelShader = PrestigeHalftone::PS_Halftone; }
}
