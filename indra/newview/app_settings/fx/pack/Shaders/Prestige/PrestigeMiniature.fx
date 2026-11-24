/*
 * Prestige True 3D Miniature -- tilt-shift "toy town" from real depth.
 *
 * A fake tilt-shift blurs a flat band of the screen. This one uses the depth buffer:
 * only things near the focus distance stay sharp, and blur grows in front of and
 * behind them the way it does with a macro lens on a real model (blur ~ |distance -
 * focus| / distance), so it follows rooftops, streets and hills correctly. A colour
 * lift (saturation, contrast, a touch of brightness) sells the toy-model look.
 *
 * Passes: focus (1x1) -> half-resolution premultiplied prepare (+mips) ->
 * horizontal -> vertical variable-radius Gaussian -> composite. Blur radius is in
 * pixels of a 1080p image.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float FocusDistance <
	ui_type = "slider"; ui_label = "Focus Distance"; ui_min = 0.0; ui_max = 500.0; ui_step = 0.5;
	ui_tooltip = "Distance in metres that stays sharp. 0 = automatic: whatever is in the centre of the frame.";
> = 0.0;

uniform float FocusBand <
	ui_type = "slider"; ui_label = "Focus Band"; ui_min = 0.0; ui_max = 100.0; ui_step = 0.5;
	ui_tooltip = "Depth of the sharp zone around the focus distance, in metres.";
> = 6.0;

uniform float BlurAmount <
	ui_type = "slider"; ui_label = "Blur Amount"; ui_min = 0.0; ui_max = 30.0; ui_step = 0.5;
	ui_tooltip = "Strongest blur, in pixels of a 1080p image.";
> = 12.0;

uniform float ColourPop <
	ui_type = "slider"; ui_label = "Toy Colours"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Saturation and contrast lift that makes the scene look like a painted model.";
> = 0.5;

namespace PrestigeMiniature
{
	texture TexFocus { Width = 1; Height = 1; Format = R32F; };
	sampler SampFocus { Texture = TexFocus; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

	texture TexPrep { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurH { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurV { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
	sampler SampPrep { Texture = TexPrep; };
	sampler SampBlurH { Texture = TexBlurH; };
	sampler SampBlurV { Texture = TexBlurV; };

	PRESTIGE_PS(PS_Focus)
	{
		if (FocusDistance > 0.0)
			return float4(FocusDistance, 0, 0, 1);
		// Automatic: mean distance of five taps around the centre (sky ignored).
		const float2 taps[5] = { float2(0.5, 0.5), float2(0.53, 0.5), float2(0.47, 0.5), float2(0.5, 0.54), float2(0.5, 0.46) };
		float sum = 0.0, n = 0.0;
		[unroll]
		for (int i = 0; i < 5; ++i)
		{
			const float d = ReShade::GetLinearizedDepth(taps[i]);
			const float valid = d < 0.999 ? 1.0 : 0.0;
			sum += d * RESHADE_DEPTH_LINEARIZATION_FAR_PLANE * valid;
			n += valid;
		}
		return float4(n > 0.0 ? sum / n : RESHADE_DEPTH_LINEARIZATION_FAR_PLANE * 0.5, 0, 0, 1);
	}

	// Blur amount 0..1 at uv (0 = sharp).
	float CoC(float2 uv)
	{
		const float focus = tex2Dlod(SampFocus, float4(0.5, 0.5, 0, 0)).x;
		const float m = Prestige::Metres(uv);
		const float outside = max(abs(m - focus) - FocusBand * 0.5, 0.0);
		return saturate(outside / max(m, 0.05) * 2.5);
	}

	PRESTIGE_PS(PS_Prep)
	{
		float4 acc = 0.0;
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT) * 0.5;
		[unroll]
		for (int i = 0; i < 4; ++i)
		{
			const float2 p = uv + (float2(i & 1, i >> 1) * 2.0 - 1.0) * o;
			// Sharp pixels are left out of the blur, so they do not smear into it.
			const float w = smoothstep(0.02, 0.12, CoC(p));
			acc += float4(Prestige::ToLinear(Prestige::Source(p)) * w, w);
		}
		return acc * 0.25;
	}

	float RadiusTexels(float2 uv) { return CoC(uv) * BlurAmount * PRESTIGE_SCALE * 0.5; }

	PRESTIGE_PS(PS_BlurH)
	{
		return Prestige::GaussianBlur(SampPrep, uv, float2(2.0 * BUFFER_RCP_WIDTH, 0.0), RadiusTexels(uv));
	}

	PRESTIGE_PS(PS_BlurV)
	{
		return Prestige::GaussianBlur(SampBlurH, uv, float2(0.0, 2.0 * BUFFER_RCP_HEIGHT), RadiusTexels(uv));
	}

	PRESTIGE_PS(PS_Composite)
	{
		const float3 c = Prestige::Source(uv);
		const float4 b = tex2D(SampBlurV, uv);
		const float3 blurred = Prestige::ToDisplay(b.rgb / max(b.a, 1e-4));
		const float amount = smoothstep(0.02, 0.2, CoC(uv)) * saturate(b.a * 4.0) * saturate(BlurAmount);
		float3 r = lerp(c, blurred, amount);
		// Toy colours.
		r = Prestige::Saturation(r, 1.0 + 0.6 * ColourPop);
		r = Prestige::SCurve(r, 0.45 * ColourPop);
		r *= 1.0 + 0.06 * ColourPop;
		return Prestige::Finish(c, r, Strength);
	}
}

technique PrestigeMiniature <
	ui_label = "Prestige True 3D Miniature";
	ui_tooltip = "Depth-based tilt-shift: a sharp slice at the focus distance, blur in front and behind, toy colours.";
>
{
	pass Focus { VertexShader = PostProcessVS; PixelShader = PrestigeMiniature::PS_Focus; RenderTarget = PrestigeMiniature::TexFocus; }
	pass Prep { VertexShader = PostProcessVS; PixelShader = PrestigeMiniature::PS_Prep; RenderTarget = PrestigeMiniature::TexPrep; }
	pass BlurH { VertexShader = PostProcessVS; PixelShader = PrestigeMiniature::PS_BlurH; RenderTarget = PrestigeMiniature::TexBlurH; }
	pass BlurV { VertexShader = PostProcessVS; PixelShader = PrestigeMiniature::PS_BlurV; RenderTarget = PrestigeMiniature::TexBlurV; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PrestigeMiniature::PS_Composite; }
}
