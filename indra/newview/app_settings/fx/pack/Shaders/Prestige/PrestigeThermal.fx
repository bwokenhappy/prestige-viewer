/*
 * Prestige Thermal -- a heat camera.
 *
 * A "temperature" is estimated per pixel (brightness, warm colours run hotter),
 * avatars glow hot through Slipstream's avatar mask, things cool down with distance
 * (depth) and the sky is cold. The result is shown through a heat palette at the
 * soft, low resolution of a thermal sensor.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform int Palette <
	ui_type = "combo"; ui_label = "Palette";
	ui_items = "Ironbow\0Rainbow\0White hot\0Black hot\0";
	ui_tooltip = "Colour scale used for temperature.";
> = 0;

uniform float AvatarHeat <
	ui_type = "slider"; ui_label = "Avatar Heat"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much hotter avatars show (needs Slipstream's avatar mask; no effect elsewhere).";
> = 0.6;

uniform float DistanceCooling <
	ui_type = "slider"; ui_label = "Distance Cooling"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Far things and the sky read colder.";
> = 0.5;

uniform float ContrastAmount <
	ui_type = "slider"; ui_label = "Contrast"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Spread of the temperature range.";
> = 0.4;

namespace PrestigeThermal
{
	texture TexHeat { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = R16F; MipLevels = 5; };
	sampler SampHeat { Texture = TexHeat; };

	float HeatAt(float2 uv)
	{
		const float3 c = saturate(Prestige::Source(uv));
		float h = Prestige::Luma(c) * 0.65 + (c.r - c.b) * 0.25 + 0.15;
		h += Prestige::AvatarMask(uv) * AvatarHeat * 0.6;
		const float depth = ReShade::GetLinearizedDepth(uv);
		const float metres = depth * RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;
		h -= DistanceCooling * (0.35 * saturate(metres / 150.0) + 0.25 * step(0.999, depth));
		return h;
	}

	PRESTIGE_PS(PS_Heat)
	{
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT) * 0.5;
		const float h = (HeatAt(uv + o) + HeatAt(uv - o) + HeatAt(uv + float2(o.x, -o.y)) + HeatAt(uv + float2(-o.x, o.y))) * 0.25;
		return float4(h, 0, 0, 1);
	}

	float3 Ironbow(float t)
	{
		const float3 c0 = float3(0.0, 0.0, 0.05), c1 = float3(0.25, 0.0, 0.55), c2 = float3(0.8, 0.05, 0.45);
		const float3 c3 = float3(1.0, 0.45, 0.0), c4 = float3(1.0, 0.85, 0.15), c5 = float3(1.0, 1.0, 0.9);
		t = saturate(t) * 5.0;
		if (t < 1.0) return lerp(c0, c1, t);
		if (t < 2.0) return lerp(c1, c2, t - 1.0);
		if (t < 3.0) return lerp(c2, c3, t - 2.0);
		if (t < 4.0) return lerp(c3, c4, t - 3.0);
		return lerp(c4, c5, t - 4.0);
	}

	PRESTIGE_PS(PS_Thermal)
	{
		const float3 original = Prestige::Source(uv);
		// Sensor softness: about 3 px at 1080p.
		const float lod = max(0.0, log2(1.5 * PRESTIGE_SCALE));
		float h = tex2Dlod(SampHeat, float4(uv, 0, lod)).r;
		h = saturate((h - 0.5) * (1.0 + ContrastAmount * 1.5) + 0.5);
		float3 r;
		if (Palette == 1) r = Prestige::HSVToRGB(float3(0.7 * (1.0 - h), 0.9, 0.25 + 0.75 * smoothstep(0.0, 0.2, h)));
		else if (Palette == 2) r = h.xxx;
		else if (Palette == 3) r = (1.0 - h).xxx;
		else r = Ironbow(h);
		return Prestige::Finish(original, r, Strength);
	}
}

technique PrestigeThermal <
	ui_label = "Prestige Thermal";
	ui_tooltip = "Heat-camera view: avatars glow hot, distance and sky read cold.";
>
{
	pass Heat { VertexShader = PostProcessVS; PixelShader = PrestigeThermal::PS_Heat; RenderTarget = PrestigeThermal::TexHeat; }
	pass Thermal { VertexShader = PostProcessVS; PixelShader = PrestigeThermal::PS_Thermal; }
}
