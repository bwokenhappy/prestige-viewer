/*
 * Prestige Comic Ink -- cel-shaded comic panel with bold ink lines.
 *
 * Shading is reduced to a few flat bands, colours are kept vivid, and thick ink lines
 * are drawn both where colours change and where the depth jumps (object silhouettes
 * against what is behind them). Line width is in pixels of a 1080p image.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float InkStrength <
	ui_type = "slider"; ui_label = "Ink Strength"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Darkness of the ink lines.";
> = 0.9;

uniform float InkWidth <
	ui_type = "slider"; ui_label = "Ink Width"; ui_min = 0.5; ui_max = 5.0; ui_step = 0.05;
	ui_tooltip = "Width of the ink lines, in pixels of a 1080p image.";
> = 1.6;

uniform int ShadeLevels <
	ui_type = "slider"; ui_label = "Shade Levels"; ui_min = 2; ui_max = 8;
	ui_tooltip = "Number of flat shading bands.";
> = 4;

uniform float SaturationAmount <
	ui_type = "slider"; ui_label = "Saturation"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.01;
	ui_tooltip = "Colour punch.";
> = 1.3;

namespace PrestigeComicInk
{
	texture TexSource { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = PRESTIGE_COPY_FORMAT; MipLevels = 6; };
	sampler SampSource { Texture = TexSource; AddressU = MIRROR; AddressV = MIRROR; };

	PRESTIGE_PS(PS_Copy)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	// Relative depth jump around uv (silhouette lines).
	float DepthEdge(float2 uv, float2 px)
	{
		const float d = ReShade::GetLinearizedDepth(uv);
		const float dl = ReShade::GetLinearizedDepth(uv - float2(px.x, 0));
		const float dr = ReShade::GetLinearizedDepth(uv + float2(px.x, 0));
		const float du = ReShade::GetLinearizedDepth(uv - float2(0, px.y));
		const float dd = ReShade::GetLinearizedDepth(uv + float2(0, px.y));
		const float nearest = min(d, min(min(dl, dr), min(du, dd)));
		const float farthest = max(d, max(max(dl, dr), max(du, dd)));
		return saturate((farthest - nearest) / max(nearest, 1e-5) * 4.0 - 0.3);
	}

	PRESTIGE_PS(PS_Comic)
	{
		const float3 original = Prestige::Source(uv);
		const float widthPx = InkWidth * PRESTIGE_SCALE;
		const float lod = max(0.0, log2(widthPx) - 0.5);
		const float2 px = widthPx * float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
		const float3 base = tex2Dlod(SampSource, float4(uv, 0, lod)).rgb;

		// Flat shading: quantise brightness (soft steps), keep hue.
		const float l = Prestige::Luma(base);
		const float n = ShadeLevels;
		const float f = l * n;
		const float ql = (floor(f) + smoothstep(0.45, 0.55, frac(f))) / n;
		float3 r = base * (max(ql, 0.06) / max(l, 1e-3));
		r = Prestige::Saturation(r, SaturationAmount);

		const float colourEdge = smoothstep(0.12, 0.4, Prestige::SobelLuma(SampSource, uv, px, lod));
		const float ink = max(colourEdge, DepthEdge(uv, px)) * InkStrength;
		r *= 1.0 - ink;
		return Prestige::Finish(original, r, Strength);
	}
}

technique PrestigeComicInk <
	ui_label = "Prestige Comic Ink";
	ui_tooltip = "Cel-shaded comic look with bold ink outlines (uses depth for silhouettes).";
>
{
	pass Copy { VertexShader = PostProcessVS; PixelShader = PrestigeComicInk::PS_Copy; RenderTarget = PrestigeComicInk::TexSource; }
	pass Comic { VertexShader = PostProcessVS; PixelShader = PrestigeComicInk::PS_Comic; }
}
