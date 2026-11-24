/*
 * Prestige Light Leak -- stray light fogging the edge of the film.
 *
 * A warm, uneven glow bleeds in from one edge of the frame (choose the angle), with a
 * hot core and a softer coloured fringe, screen-blended over the picture.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Intensity <
	ui_type = "slider"; ui_label = "Intensity"; ui_min = 0.0; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "Brightness of the leak.";
> = 0.8;

uniform float Angle <
	ui_type = "slider"; ui_label = "Direction"; ui_min = 0.0; ui_max = 360.0; ui_step = 1.0;
	ui_tooltip = "Edge the light leaks in from, in degrees (0 = right, 90 = top, 180 = left, 270 = bottom).";
> = 20.0;

uniform float Size <
	ui_type = "slider"; ui_label = "Size"; ui_min = 0.1; ui_max = 1.5; ui_step = 0.01;
	ui_tooltip = "How far the leak reaches into the picture.";
> = 0.55;

uniform float3 LeakColor <
	ui_type = "color"; ui_label = "Colour";
	ui_tooltip = "Colour of the leak's fringe (its core is hotter and yellower).";
> = float3(1.0, 0.30, 0.10);

namespace PrestigeLightLeak
{
	float Leak(float2 p, float2 dir, float size, float stretch)
	{
		// p: aspect-correct position; the leak source sits on the frame edge along dir.
		const float2 halfSize = float2(PRESTIGE_ASPECT, 1.0) * 0.5;
		const float t = min(halfSize.x / max(abs(dir.x), 1e-4), halfSize.y / max(abs(dir.y), 1e-4));
		const float2 src = dir * t;
		const float2 d = p - src;
		const float along = dot(d, -dir);                  // into the picture
		const float across = dot(d, float2(-dir.y, dir.x));
		const float e = (along * along) / (size * size) + (across * across) / (size * size * stretch * stretch);
		return exp(-e * 2.0);
	}

	PRESTIGE_PS(PS_Leak)
	{
		const float3 c = Prestige::Source(uv);
		const float a = radians(Angle);
		const float2 dir = float2(cos(a), -sin(a));         // screen y points down
		const float2 p = Prestige::Centered(uv);

		// Main leak plus a smaller one beside it, broken up by soft noise.
		const float2 side = float2(-dir.y, dir.x);
		float leak = Leak(p, dir, Size, 1.8);
		leak += 0.5 * Leak(p - side * Size * 0.6, dir, Size * 0.55, 1.2);
		const float n = Prestige::Fbm(p * 3.0 / Size + float2(Angle * 0.1, 0.0), 19u);
		leak *= 0.55 + 0.9 * n;

		const float core = smoothstep(0.45, 1.0, leak);
		const float3 col = lerp(LeakColor, float3(1.0, 0.85, 0.55), core) * leak * Intensity;
		const float3 r = Prestige::Screen(c, col);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeLightLeak <
	ui_label = "Prestige Light Leak";
	ui_tooltip = "Warm stray light bleeding in from the edge of the frame.";
>
{
	pass Leak { VertexShader = PostProcessVS; PixelShader = PrestigeLightLeak::PS_Leak; }
}
