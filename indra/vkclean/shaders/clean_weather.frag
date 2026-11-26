#version 460
#extension GL_GOOGLE_include_directive : require
// vkclean — WEATHER PRECIPITATION DRAW, fragment stage (#783): premultiplied over the scene
// (ONE, ONE_MINUS_SRC_ALPHA), depth tested against the opaque + water depth, no depth
// write. Lit cheaply from the world draws' sun / ambient: a drop mostly shows the sky it
// refracts, a flake is a white diffuser. Coverage goes to the reactive mask (TAA / FSR do
// not smear the streaks); no glow. #954: each takes the veil of its own distance (v_veil).
// #1517 -DWEATHER_LATE (every frame that feeds frame generation): the same particles drawn AFTER
// the post chain into the display's UI layer (RGBA8 UNORM, premultiplied sRGB-encoded colour +
// coverage) against the display-size depth, so the generator's world input holds none of them.
// The colour goes through the scene's own exposure and tail (clean_tonemap_common.glsl's
// tonemapTail on the tonemap pass's push block and grade set: the same tonemapper, grade, LUT and
// curves). The layer is composited over the world in its encoded space (min(layer + world *
// (1 - a), 1)); a particle is blended in linear light in the scene, so the texel written here is
// the one that makes that composite show the linear-light "over" of the particle on the finished
// world under it (read here). Premultiplied (ONE / ONE_MINUS_SRC_ALPHA on colour and coverage).
// One output, no glow / reactive.
#define WEATHER_VS   // (read-only access to the shared buffers)
#include "clean_weather_common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in float v_alpha;
layout(location = 2) flat in float v_snow;
layout(location = 4) flat in float v_veil;   // #954 (clean_weather.vert)
#if !defined(WEATHER_STREAM) && !defined(WEATHER_SPLASH)
layout(location = 5) flat in vec3 v_dbg;     // RenderVKWeatherOcclDebug: the roof test's colour (-1 off)
#endif
#ifdef WEATHER_SPLASH
layout(location = 3) flat in float v_age;
#endif
#ifdef WEATHER_STREAM
layout(location = 3) flat in float v_len;
#endif

layout(location = 0) out vec4 outColor;
#ifdef WEATHER_LATE
// Set 1 = the renderer's grade set (mDslGrade) and the push block = the tonemap pass's own (112 B:
// p0 exposure / tonemapper / mix, p1.x no_post, p1.z dynamic exposure on, p5.x the display's paper
// white in HDR output, 0 in SDR; p5.z here: 1 = the world texel reads linear, an _SRGB view).
#define TONEMAP_NIGHT_EYE
#include "clean_tonemap_common.glsl"
layout(r32f, set = 2, binding = 0) uniform readonly image2D uLateExposure;   // the renderer's 1x1 dynamic exposure
layout(set = 2, binding = 1) uniform sampler2D uLateWorld;   // the finished world (SDR: encoded; HDR: nits)

// The sRGB curve continued past white (clean_display_common.glsl's encodeExt / decodeExt): the
// space the UI layer is composited in.
vec3 wxEncodeExt(vec3 x)
{
    const vec3 a = abs(x);
    return sign(x) * mix(1.055 * pow(a, vec3(1.0 / 2.4)) - 0.055, 12.92 * a, lessThanEqual(a, vec3(0.0031308)));
}
vec3 wxDecodeExt(vec3 e)
{
    const vec3 a = abs(e);
    return sign(e) * mix(pow((a + 0.055) / 1.055, vec3(2.4)), a / 12.92, lessThanEqual(a, vec3(0.04045)));
}
#else
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outReact;
#endif

void main()
{
    float a;
    vec3 col;
#if defined(WEATHER_STREAM)
    // M4 STREAM: a thin falling sheet -- a soft profile across, the water's pulses scrolling
    // down it (per-emitter phase), fading in at the edge and at the landing.
    {
        const float seed = v_snow - 3.0;
        const float across = 1.0 - v_uv.y * v_uv.y;
        const float x = v_uv.x;
        const float s = fract(x * v_len * 2.2 - wp.eye.w * 4.5 + seed * 7.0);
        const float pulse = 0.55 + 0.45 * smoothstep(0.25, 0.75, s) * (1.0 - smoothstep(0.75, 1.0, s));
        const float ends = smoothstep(0.0, 0.04, x) * (1.0 - smoothstep(0.94, 1.0, x));
        a = v_alpha * across * pulse * ends;
        col = wp.amb.rgb * 0.85 + wp.sun.rgb * (0.3 * wp.sun.w);
    }
#elif defined(WEATHER_SPLASH)
    // M2 SPLASH: a few droplets thrown up and out from the hit on ballistic arcs (per-splash
    // seeded), fading over the splash's short life. v_uv: x across (-1..1), y up (0..1).
    {
        const float seed = v_snow - 2.0;
        const float t = v_age;
        a = 0.0;
        for (int k = 0; k < 5; ++k)
        {
            const float h1 = fract(sin((seed * 7.0 + float(k)) * 12.9898) * 43758.5453);
            const float h2 = fract(sin((seed * 3.0 + float(k)) * 78.233) * 24634.6345);
            const vec2 p = vec2((h1 * 2.0 - 1.0) * 0.9 * t, (2.6 + 1.4 * h2) * t - 3.4 * t * t);
            const vec2 q = v_uv - vec2(p.x, max(p.y, 0.0));
            a += exp(-dot(q, q) * 90.0);
        }
        a = min(a, 1.0) * v_alpha * (1.0 - t * t);
        col = wp.amb.rgb * 0.8 + wp.sun.rgb * (0.25 * wp.sun.w);
    }
#else
    if (v_snow > 0.5)
    {
        const float r2 = dot(v_uv, v_uv);
        a = v_alpha * (1.0 - smoothstep(0.2, 1.0, r2));
        col = wp.amb.rgb * 0.9 + wp.sun.rgb * (0.6 * wp.sun.w);
        // <Prestige> SANDSTORM (wp.veil2.z 0..1): the "flakes" are grains of sand -- tan, and
        // denser at the core (a harder grain edge)
        if (wp.veil2.z > 0.0)
        {
            col *= mix(vec3(1.0), vec3(0.86, 0.66, 0.42), wp.veil2.z);
            a = mix(a, v_alpha * (1.0 - smoothstep(0.05, 0.7, r2)), wp.veil2.z);
        }
        // </Prestige>
    }
    else
    {
        const float across = 1.0 - v_uv.y * v_uv.y;
        const float along = smoothstep(0.0, 0.2, v_uv.x) * (1.0 - smoothstep(0.55, 1.0, v_uv.x));
        a = v_alpha * across * along;
        col = wp.amb.rgb * 0.75 + wp.sun.rgb * (0.2 * wp.sun.w);
        if (v_dbg.x >= 0.0)
        {
            col = v_dbg;
            a = min(a * 6.0, 1.0);
        }
    }
#endif
    if (a < 1.0 / 512.0) discard;
    // #954 VEIL: the precipitation between the camera and the particle (the scene behind it was
    // veiled over its whole path: drawn over that, this is exactly the veil of the particle's own
    // distance -- clean_weather_veil.frag).
    col = col * v_veil + wp.veil.rgb * (1.0 - v_veil);
#ifdef WEATHER_LATE
    // Exposure as clean_tonemap.frag applies it (none under no_post), then the scene's tail: the
    // particle's own display colour.
    vec3 e = max(col, vec3(0.0));
    if (pc.p1.x < 1.0)   // (a share: tmNoPost)
    {
        float s = (pc.p1.z > 0.5) ? imageLoad(uLateExposure, ivec2(0)).r : 1.0;
        if (isnan(s) || isinf(s) || s <= 0.0) s = 1.0;
        e = tmExpose(e, s);
    }
    const vec3 p = tonemapTail(e);
    // The world under it in the layer's space.
    vec3 w = max(texelFetch(uLateWorld, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0));
    if (pc.p5.x > 0.0) w = wxEncodeExt(w / pc.p5.x);
    else if (pc.p5.z > 0.5) w = linear_to_srgb(w);
    // What the pixel shows with the particle blended in linear light, as the scene would...
    const float al = clamp(a, 0.0, 1.0);
    const vec3 want = wxEncodeExt(wxDecodeExt(w) * (1.0 - al) + wxDecodeExt(p) * al);
    // ...written as the layer texel the composite turns into it: want = layer + world * (1 - a).
    // (>= 0: the curve is concave through 0; the UNORM layer caps it at 1 -- paper white in HDR)
    outColor = vec4(clamp(want - w * (1.0 - al), vec3(0.0), vec3(1.0)), al);
#else
    outColor = vec4(col * a, a);
    outGlow = vec4(0.0);
    outReact = vec4(min(a * 2.0, 1.0), 0.0, 0.0, 0.0);
#endif
}
