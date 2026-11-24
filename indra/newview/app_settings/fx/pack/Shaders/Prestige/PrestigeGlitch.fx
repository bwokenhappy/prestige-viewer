/*
 * Prestige Glitch -- digital datamosh / broken-signal look, animated.
 *
 * Rectangular blocks and whole bands jump sideways, colour channels split apart and
 * a few blocks lose colour depth. The pattern changes in bursts over time (timer
 * driven), so every snapshot freezes a different glitch.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Intensity <
	ui_type = "slider"; ui_label = "Intensity"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How many blocks glitch and how far they jump.";
> = 0.45;

uniform float BlockSize <
	ui_type = "slider"; ui_label = "Block Size"; ui_min = 4.0; ui_max = 120.0; ui_step = 1.0;
	ui_tooltip = "Height of a glitch block, in pixels of a 1080p image (blocks are 4x wider than tall).";
> = 28.0;

uniform float Speed <
	ui_type = "slider"; ui_label = "Speed"; ui_min = 0.0; ui_max = 3.0; ui_step = 0.01;
	ui_tooltip = "How often the glitch pattern changes (0 = frozen).";
> = 1.0;

uniform float RGBSplit <
	ui_type = "slider"; ui_label = "RGB Split"; ui_min = 0.0; ui_max = 40.0; ui_step = 0.5;
	ui_tooltip = "How far the red and blue channels separate, in pixels of a 1080p image.";
> = 8.0;

uniform float Timer < source = "timer"; >;

namespace PrestigeGlitch
{
	PRESTIGE_PS(PS_Glitch)
	{
		const float3 original = Prestige::Source(uv);
		const uint frame = uint(Timer * 0.001 * 12.0 * Speed);
		// Bursts: some frames are calm, some are wild.
		const float burst = smoothstep(0.35, 0.9, Prestige::Hash(float2(frame, 1.0), 77u));
		const float level = Intensity * (0.35 + 0.65 * burst);

		// Wide horizontal bands that tear sideways.
		const float bandH = BlockSize * 3.0 / 1080.0;
		const float band = floor(uv.y / bandH);
		const float bandHit = step(1.0 - level * 0.35, Prestige::Hash(float2(band, 2.0), frame));
		float offset = (Prestige::Hash(float2(band, 5.0), frame) - 0.5) * 0.15 * bandHit;

		// Small blocks that jump on their own.
		const float2 cellUV = float2(BlockSize * 4.0, BlockSize) * PRESTIGE_PX;
		const float2 cell = floor(uv / cellUV);
		const float blockHit = step(1.0 - level * 0.25, Prestige::Hash(cell, frame + 3u));
		offset += (Prestige::Hash(cell, frame + 11u) - 0.5) * 0.08 * blockHit;
		offset *= level * 2.0;

		const float2 guv = float2(uv.x + offset, uv.y);
		const float split = RGBSplit * PRESTIGE_PX.x * (0.4 + level * 1.6 * max(bandHit, blockHit) + burst * 0.4);
		float3 c;
		c.r = tex2Dlod(Prestige::BackBufferMirror, float4(guv + float2(split, 0), 0, 0)).r;
		c.g = tex2Dlod(Prestige::BackBufferMirror, float4(guv, 0, 0)).g;
		c.b = tex2Dlod(Prestige::BackBufferMirror, float4(guv - float2(split, 0), 0, 0)).b;

		// A few blocks lose colour depth or invert.
		const float crush = blockHit * step(0.6, Prestige::Hash(cell, frame + 23u));
		c = lerp(c, floor(c * 4.0 + 0.5) / 4.0, crush);
		const float invert = blockHit * step(0.93, Prestige::Hash(cell, frame + 29u));
		c = lerp(c, 1.0 - c, invert);

		// Thin digital noise lines.
		const float lineHit = step(1.0 - level * 0.02, Prestige::Hash(float2(floor(uv.y * 540.0), 9.0), frame));
		c = lerp(c, Prestige::Hash3(float2(floor(uv.x * 96.0), floor(uv.y * 540.0)), frame), lineHit * 0.7);

		return Prestige::Finish(original, c, Strength);
	}
}

technique PrestigeGlitch <
	ui_label = "Prestige Glitch";
	ui_tooltip = "Broken digital signal: jumping blocks, torn bands and RGB split. Animated.";
>
{
	pass Glitch { VertexShader = PostProcessVS; PixelShader = PrestigeGlitch::PS_Glitch; }
}
