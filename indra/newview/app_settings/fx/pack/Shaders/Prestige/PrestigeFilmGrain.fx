/*
 * Prestige Film Grain -- organic, luminance-aware film grain.
 *
 * Grain is strongest in the mid-tones (like real negative film), softer in deep
 * shadows and clipped highlights, and is re-drawn 24 times a second. Grain size is
 * in pixels of a 1080-line image, so a 4K / 8K snapshot has the same look.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Intensity <
	ui_type = "slider"; ui_label = "Intensity"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How visible the grain is.";
> = 0.35;

uniform float GrainSize <
	ui_type = "slider"; ui_label = "Grain Size"; ui_min = 0.5; ui_max = 4.0; ui_step = 0.05;
	ui_tooltip = "Size of a grain clump, in pixels of a 1080p image (scaled for bigger snapshots).";
> = 1.4;

uniform float ColorGrain <
	ui_type = "slider"; ui_label = "Colour Grain"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "0 = monochrome grain, 1 = separate grain per colour layer (like colour negative film).";
> = 0.25;

uniform float ShadowGrain <
	ui_type = "slider"; ui_label = "Shadow Grain"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much grain the shadows get compared with the mid-tones.";
> = 0.5;

uniform float Timer < source = "timer"; >;

namespace PrestigeFilmGrain
{
	// One noise layer: value noise at the grain size, plus a finer octave so it does
	// not look like a blurred grid.
	float Layer(float2 pos, float size, uint seed)
	{
		const float2 p = pos / size;
		const float a = Prestige::ValueNoise(p, seed) - 0.5;
		const float b = Prestige::ValueNoise(p * 2.17 + 3.3, seed + 5u) - 0.5;
		return (a * 0.7 + b * 0.45) * 2.0;
	}

	PRESTIGE_PS(PS_Grain)
	{
		const float3 c = Prestige::Source(uv);
		const uint frame = uint(Timer * 0.024);   // 24 new grain patterns per second
		const uint seed = Prestige::HashU(frame) | 1u;
		const float size = GrainSize * PRESTIGE_SCALE;

		const float mono = Layer(vpos.xy, size, seed);
		const float3 colour = float3(Layer(vpos.xy, size, seed + 101u),
		                             Layer(vpos.xy, size, seed + 211u),
		                             Layer(vpos.xy, size, seed + 307u));
		const float3 n = lerp(mono.xxx, colour, ColorGrain);

		// Mid-tone weighted response; highlights roll off to nothing.
		const float l = Prestige::Luma(saturate(c));
		const float mid = 1.0 - pow(abs(l * 2.0 - 1.0), 2.0);
		const float response = lerp(ShadowGrain * (1.0 - l), 1.0, mid) * (1.0 - smoothstep(0.85, 1.0, l) * 0.8);

		const float3 result = c + n * (Intensity * 0.22 * response);
		return Prestige::Finish(c, result, Strength);
	}
}

technique PrestigeFilmGrain <
	ui_label = "Prestige Film Grain";
	ui_tooltip = "Organic film grain that follows the image's tones. Grain size scales with the snapshot size.";
>
{
	pass Grain { VertexShader = PostProcessVS; PixelShader = PrestigeFilmGrain::PS_Grain; }
}
