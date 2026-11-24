/*
 * Prestige VHS -- a worn home-video tape.
 *
 * Soft luma, colour that bleeds sideways and lags behind the picture, wobbling
 * tracking with a noisy band rolling through the frame, head-switching noise at the
 * bottom, scanlines and tape snow. Everything is measured in tape lines and 1080p
 * pixels, so snapshots of any size match the viewfinder.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float ChromaBleed <
	ui_type = "slider"; ui_label = "Colour Bleed"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How far colour smears to the side of edges.";
> = 0.6;

uniform float Tracking <
	ui_type = "slider"; ui_label = "Tracking Errors"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Line wobble and the rolling noise band of a badly tracked tape.";
> = 0.35;

uniform float Scanlines <
	ui_type = "slider"; ui_label = "Scanlines"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Darkness of the gaps between video lines.";
> = 0.3;

uniform float Noise <
	ui_type = "slider"; ui_label = "Noise"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Tape snow and grain.";
> = 0.3;

uniform float Timer < source = "timer"; >;

namespace PrestigeVHS
{
	static const float LINES = 480.0;   // video lines over the image height

	PRESTIGE_PS(PS_VHS)
	{
		const float3 original = Prestige::Source(uv);
		const float t = Timer * 0.001;
		const uint frame = uint(Timer * 0.03);
		const float line = floor(uv.y * LINES);

		// --- tracking: per-line jitter, a rolling noise band, head switching at the bottom
		const float bandY = frac(t * 0.08);
		const float band = exp(-pow((uv.y - bandY) * 14.0, 2.0)) * Tracking;
		const float lineNoise = Prestige::Hash(float2(line, 3.0), frame) - 0.5;
		float shift = lineNoise * (0.0015 + band * 0.02) * Tracking;
		shift += sin(uv.y * 9.0 + t * 1.7) * 0.0008 * Tracking;              // slow skew
		const float head = smoothstep(0.965, 1.0, uv.y);                     // head switching
		shift += head * (0.02 + lineNoise * 0.02) * (0.3 + Tracking);

		const float2 suv = float2(uv.x + shift, uv.y);
		const float px = BUFFER_RCP_WIDTH * PRESTIGE_SCALE;

		// --- luma: slightly soft (5 taps over ~2 px); chroma: wide, lagging smear
		float y = 0.0;
		[unroll]
		for (int i = -2; i <= 2; ++i)
			y += Prestige::RGBToYIQ(tex2Dlod(Prestige::BackBufferMirror, float4(suv + float2(i * px * 0.6, 0), 0, 0)).rgb).x;
		y /= 5.0;

		float2 iq = 0.0;
		const float spread = (1.0 + ChromaBleed * 14.0) * px;
		const float lag = ChromaBleed * 4.0 * px;
		float wsum = 0.0;
		[unroll]
		for (int j = 0; j < 9; ++j)
		{
			const float w = 1.0 - j / 10.0;                       // trailing smear
			iq += Prestige::RGBToYIQ(tex2Dlod(Prestige::BackBufferMirror, float4(suv - float2(lag + j * spread * 0.5, 0), 0, 0)).rgb).yz * w;
			wsum += w;
		}
		iq /= wsum;

		float3 c = Prestige::YIQToRGB(float3(y, iq * 0.85));
		c = lerp(0.04, 0.96, c);                                      // tape black / white levels

		// --- scanlines
		const float scan = 0.5 + 0.5 * cos(uv.y * LINES * 6.2831853);
		c *= 1.0 - Scanlines * 0.35 * scan;

		// --- snow, band noise, head noise
		const float snow = Prestige::Hash(floor(vpos.xy / (1.5 * PRESTIGE_SCALE)), frame + 9u);
		c += (snow - 0.5) * Noise * 0.12;
		const float streak = step(1.0 - 0.02 * Noise - band * 0.05, Prestige::Hash(float2(floor(uv.x * 160.0), line), frame + 31u));
		c += streak * 0.6 * (Noise + band);
		c = lerp(c, snow.xxx, head * 0.5 * (0.3 + Noise));

		return Prestige::Finish(original, c, Strength);
	}
}

technique PrestigeVHS <
	ui_label = "Prestige VHS";
	ui_tooltip = "Worn videotape: colour bleed, tracking wobble and noise, scanlines and snow. Animated.";
>
{
	pass VHS { VertexShader = PostProcessVS; PixelShader = PrestigeVHS::PS_VHS; }
}
