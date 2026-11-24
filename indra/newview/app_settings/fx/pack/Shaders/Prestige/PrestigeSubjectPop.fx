/*
 * Prestige Subject Pop -- make the avatars stand out from the scene.
 *
 * Uses Slipstream's avatar mask (SLIPSTREAM_AVATAR_MASK): avatars get a little
 * clarity, saturation and exposure; the background is gently dimmed and
 * desaturated. The mask is blurred (quarter resolution, separable, radius in 1080p
 * pixels) so the transition is soft, never a cut-out edge. Without Slipstream it
 * falls back to "things closer than a few metres" from the depth buffer.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float SubjectLift <
	ui_type = "slider"; ui_label = "Subject Lift"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "Clarity, saturation and brightness boost on the avatars.";
> = 0.5;

uniform float BackgroundDim <
	ui_type = "slider"; ui_label = "Background Dim"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much darker everything that is not an avatar becomes.";
> = 0.3;

uniform float BackgroundDesaturate <
	ui_type = "slider"; ui_label = "Background Desaturation"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
	ui_tooltip = "How much colour the background loses.";
> = 0.3;

uniform float EdgeSoftness <
	ui_type = "slider"; ui_label = "Edge Softness"; ui_min = 1.0; ui_max = 60.0; ui_step = 0.5;
	ui_tooltip = "Width of the soft transition around the avatars, in pixels of a 1080p image.";
> = 12.0;

namespace PrestigeSubjectPop
{
	texture TexMask { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = R16F; MipLevels = 5; };
	texture TexMaskH { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = R16F; MipLevels = 5; };
	texture TexMaskV { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = R16F; };
	sampler SampMask { Texture = TexMask; };
	sampler SampMaskH { Texture = TexMaskH; };
	sampler SampMaskV { Texture = TexMaskV; };

	texture TexColor { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; MipLevels = 6; };
	sampler SampColor { Texture = TexColor; };

	float SubjectAt(float2 uv)
	{
#if __SLIPSTREAM__
		return Prestige::AvatarMask(uv);
#else
		return 1.0 - smoothstep(4.0, 10.0, Prestige::Metres(uv));
#endif
	}

	PRESTIGE_PS(PS_Mask)
	{
		// A quarter-resolution texel covers 4x4 pixels: average four point taps in it.
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
		const float m = SubjectAt(uv + o * float2(-1, -1)) + SubjectAt(uv + o * float2(1, -1))
		              + SubjectAt(uv + o * float2(-1, 1)) + SubjectAt(uv + o * float2(1, 1));
		return float4(m * 0.25, 0, 0, 1);
	}

	PRESTIGE_PS(PS_MaskH)
	{
		return Prestige::GaussianBlur(SampMask, uv, float2(4.0 * BUFFER_RCP_WIDTH, 0.0), EdgeSoftness * PRESTIGE_SCALE * 0.25);
	}

	PRESTIGE_PS(PS_MaskV)
	{
		return Prestige::GaussianBlur(SampMaskH, uv, float2(0.0, 4.0 * BUFFER_RCP_HEIGHT), EdgeSoftness * PRESTIGE_SCALE * 0.25);
	}

	PRESTIGE_PS(PS_Color)
	{
		return float4(Prestige::Source(uv), 1.0);
	}

	PRESTIGE_PS(PS_Composite)
	{
		const float3 c = Prestige::Source(uv);
		// Soft mask: full strength inside the avatars, fading out across the edge.
		const float soft = tex2D(SampMaskV, uv).r;
		const float subject = saturate(max(soft * 1.5 - 0.15, SubjectAt(uv) * soft * 2.0));

		// Subject: clarity (local contrast vs a ~20 px neighbourhood), saturation, exposure.
		const float l = Prestige::Luma(c);
		const float wide = Prestige::Luma(Prestige::SoftAverage(SampColor, uv, 20.0 / 1080.0, BUFFER_HEIGHT * 0.5).rgb);
		const float mid = 1.0 - pow(abs(saturate(l) * 2.0 - 1.0), 2.0);
		float3 lifted = c + clamp(l - wide, -0.15, 0.15) * mid * SubjectLift * 0.9;
		lifted = Prestige::Saturation(lifted, 1.0 + 0.3 * SubjectLift);
		lifted *= 1.0 + 0.12 * SubjectLift;

		// Background: dim and desaturate.
		float3 background = Prestige::Saturation(c, 1.0 - BackgroundDesaturate * 0.8);
		background *= 1.0 - BackgroundDim * 0.45;

		return Prestige::Finish(c, lerp(background, lifted, subject), Strength);
	}
}

technique PrestigeSubjectPop <
	ui_label = "Prestige Subject Pop";
	ui_tooltip = "Lifts the avatars (clarity, colour, brightness) and dims the background, with soft edges.";
>
{
	pass Mask { VertexShader = PostProcessVS; PixelShader = PrestigeSubjectPop::PS_Mask; RenderTarget = PrestigeSubjectPop::TexMask; }
	pass MaskH { VertexShader = PostProcessVS; PixelShader = PrestigeSubjectPop::PS_MaskH; RenderTarget = PrestigeSubjectPop::TexMaskH; }
	pass MaskV { VertexShader = PostProcessVS; PixelShader = PrestigeSubjectPop::PS_MaskV; RenderTarget = PrestigeSubjectPop::TexMaskV; }
	pass Color { VertexShader = PostProcessVS; PixelShader = PrestigeSubjectPop::PS_Color; RenderTarget = PrestigeSubjectPop::TexColor; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PrestigeSubjectPop::PS_Composite; }
}
