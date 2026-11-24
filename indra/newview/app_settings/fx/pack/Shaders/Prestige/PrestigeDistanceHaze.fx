/*
 * Prestige Distance Haze -- atmospheric haze that grows with real distance.
 *
 * Uses the depth buffer (metres = linear depth x far plane) to fade distant things
 * into a haze colour, starting a set distance from the camera. Height falloff thins
 * the haze higher up (estimated from the view ray, assuming a roughly level camera
 * with the viewer's usual field of view), so hills and the sky above the horizon
 * stay clearer than the ground and the horizon line.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float Start <
	ui_type = "slider"; ui_label = "Start Distance"; ui_min = 0.0; ui_max = 500.0; ui_step = 1.0;
	ui_tooltip = "Distance from the camera, in metres, where the haze begins.";
> = 15.0;

uniform float Density <
	ui_type = "slider"; ui_label = "Density"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.005;
	ui_tooltip = "Thickness of the haze (0.25 = about half hidden at 200 m beyond the start).";
> = 0.25;

uniform float3 HazeColor <
	ui_type = "color"; ui_label = "Haze Colour";
	ui_tooltip = "Colour of the air: pale blue for clear days, warm for dust, grey for fog.";
> = float3(0.72, 0.79, 0.88);

uniform float HeightFalloff <
	ui_type = "slider"; ui_label = "Height Falloff"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How quickly the haze thins out above the camera (0 = even haze everywhere).";
> = 0.3;

namespace PrestigeDistanceHaze
{
	// tan(vertical FOV / 2) for the viewer's default ~60 degree view.
	static const float TAN_HALF_FOV = 0.58;

	PRESTIGE_PS(PS_Haze)
	{
		const float3 c = Prestige::Source(uv);
		const float depth = ReShade::GetLinearizedDepth(uv);
		const float metres = depth * RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;   // sky = far plane
		const float dist = max(metres - Start, 0.0);

		// Height of the point above the camera along this view ray (level camera).
		const float slope = (0.5 - uv.y) * 2.0 * TAN_HALF_FOV;
		const float rise = clamp(slope * metres, -60.0, 2000.0);
		// Exponential height fog: average density along the ray relative to camera height.
		const float b = HeightFalloff * 0.04;
		const float bh = b * rise;
		const float heightTerm = abs(bh) > 1e-3 ? (1.0 - exp(-bh)) / bh : 1.0 - 0.5 * bh;

		const float opticalDepth = dist * Density * 0.014 * heightTerm;
		const float haze = 1.0 - exp(-max(opticalDepth, 0.0));

		// Haze also flattens contrast slightly before it covers things.
		float3 r = lerp(c, Prestige::Saturation(c, 0.85), saturate(haze * 2.0));
		r = lerp(r, HazeColor, haze);
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeDistanceHaze <
	ui_label = "Prestige Distance Haze";
	ui_tooltip = "Depth-based atmospheric haze: start distance, density, colour and height falloff.";
>
{
	pass Haze { VertexShader = PostProcessVS; PixelShader = PrestigeDistanceHaze::PS_Haze; }
}
