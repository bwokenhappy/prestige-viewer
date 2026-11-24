/*
 * Prestige Background Blur -- portrait-mode background softening from real depth.
 *
 * Everything behind the subject is blurred more the further it is (thin-lens style:
 * blur ~ 1 - subject / distance), the foreground and the subject stay sharp. With
 * Slipstream's avatar mask, avatars are never blurred and never bleed into the
 * blurred background (no halos). The subject distance can be set by hand or found
 * automatically (nearest avatars, else whatever is in the centre of the frame).
 *
 * Passes: avatar depth statistics (64x64 + mips) -> focus (1x1) -> half-resolution
 * premultiplied prepare (+mips) -> horizontal -> vertical variable-radius Gaussian
 * -> full-resolution composite. Blur radius is in pixels of a 1080p image.
 *
 * Original Prestige code. LGPL-2.1-only (Prestige Viewer).
 */

#include "PrestigeCommon.fxh"

PRESTIGE_STRENGTH_UNIFORM

uniform float BlurAmount <
	ui_type = "slider"; ui_label = "Blur Amount"; ui_min = 0.0; ui_max = 40.0; ui_step = 0.5;
	ui_tooltip = "Blur radius of the far background, in pixels of a 1080p image.";
> = 14.0;

uniform float SubjectDistance <
	ui_type = "slider"; ui_label = "Subject Distance"; ui_min = 0.0; ui_max = 200.0; ui_step = 0.1;
	ui_tooltip = "Distance to the subject in metres; everything beyond it is blurred.\n0 = automatic (nearest avatars, or the centre of the frame).";
> = 0.0;

uniform float BlurRamp <
	ui_type = "slider"; ui_label = "Blur Ramp"; ui_min = 0.25; ui_max = 4.0; ui_step = 0.05;
	ui_tooltip = "How quickly the blur builds up behind the subject (higher = like a wider aperture).";
> = 1.5;

namespace PrestigeBackgroundBlur
{
	// RG16F: mips are made with a linear blit, which 32-bit float formats do not support everywhere.
	texture TexAvatarDepth { Width = 64; Height = 64; Format = RG16F; MipLevels = 7; };
	sampler SampAvatarDepth { Texture = TexAvatarDepth; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
	texture TexFocus { Width = 1; Height = 1; Format = R32F; };
	sampler SampFocus { Texture = TexFocus; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };

	texture TexPrep { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurH { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; MipLevels = 6; };
	texture TexBlurV { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
	sampler SampPrep { Texture = TexPrep; };
	sampler SampBlurH { Texture = TexBlurH; };
	sampler SampBlurV { Texture = TexBlurV; };

	// --- subject distance --------------------------------------------------------

	// Avatar coverage and coverage / distance per cell; the 1x1 mip gives their means,
	// and coverage / (coverage / distance) is the harmonic mean distance of the avatar
	// pixels, which leans towards the nearer avatars.
	PRESTIGE_PS(PS_AvatarDepth)
	{
		float2 acc = 0.0;
		[unroll]
		for (int i = 0; i < 4; ++i)
		{
			const float2 p = uv + (float2(i & 1, i >> 1) - 0.5) * (0.5 / 64.0);
			const float mask = Prestige::AvatarMask(p);
			acc += float2(mask, mask / max(Prestige::Metres(p), 0.5));
		}
		return float4(acc * 0.25, 0, 1);
	}

	PRESTIGE_PS(PS_Focus)
	{
		if (SubjectDistance > 0.0)
			return float4(SubjectDistance, 0, 0, 1);
		const float2 avatars = tex2Dlod(SampAvatarDepth, float4(0.5, 0.5, 0, 6)).xy;
		if (avatars.x > 1e-5 && avatars.y > 1e-7)
			return float4(avatars.x / avatars.y, 0, 0, 1);
		// No avatars: focus on the centre of the frame (nearest of five taps).
		float d = Prestige::Metres(float2(0.5, 0.5));
		d = min(d, Prestige::Metres(float2(0.47, 0.5)));
		d = min(d, Prestige::Metres(float2(0.53, 0.5)));
		d = min(d, Prestige::Metres(float2(0.5, 0.46)));
		d = min(d, Prestige::Metres(float2(0.5, 0.54)));
		return float4(d, 0, 0, 1);
	}

	// Blur amount 0..1 at uv (0 = sharp).
	float CoC(float2 uv)
	{
		const float subject = tex2Dlod(SampFocus, float4(0.5, 0.5, 0, 0)).x;
		const float m = Prestige::Metres(uv);
		const float behind = 1.0 - (subject + 0.25) / max(m, 0.01);   // thin-lens far blur
		return saturate(behind * BlurRamp) * (1.0 - Prestige::AvatarMask(uv));
	}

	// --- blur --------------------------------------------------------------------

	// Premultiplied colour: sharp pixels get weight 0 so the subject never bleeds out.
	PRESTIGE_PS(PS_Prep)
	{
		float4 acc = 0.0;
		const float2 o = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT) * 0.5;
		[unroll]
		for (int i = 0; i < 4; ++i)
		{
			const float2 p = uv + (float2(i & 1, i >> 1) * 2.0 - 1.0) * o;
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
		return Prestige::Finish(c, lerp(c, blurred, amount), Strength);
	}
}

technique PrestigeBackgroundBlur <
	ui_label = "Prestige Background Blur";
	ui_tooltip = "Depth-based background blur behind the subject; avatars stay sharp (Slipstream avatar mask).";
>
{
	pass AvatarDepth { VertexShader = PostProcessVS; PixelShader = PrestigeBackgroundBlur::PS_AvatarDepth; RenderTarget = PrestigeBackgroundBlur::TexAvatarDepth; }
	pass Focus { VertexShader = PostProcessVS; PixelShader = PrestigeBackgroundBlur::PS_Focus; RenderTarget = PrestigeBackgroundBlur::TexFocus; }
	pass Prep { VertexShader = PostProcessVS; PixelShader = PrestigeBackgroundBlur::PS_Prep; RenderTarget = PrestigeBackgroundBlur::TexPrep; }
	pass BlurH { VertexShader = PostProcessVS; PixelShader = PrestigeBackgroundBlur::PS_BlurH; RenderTarget = PrestigeBackgroundBlur::TexBlurH; }
	pass BlurV { VertexShader = PostProcessVS; PixelShader = PrestigeBackgroundBlur::PS_BlurV; RenderTarget = PrestigeBackgroundBlur::TexBlurV; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PrestigeBackgroundBlur::PS_Composite; }
}
