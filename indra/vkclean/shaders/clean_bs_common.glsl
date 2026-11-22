// vkclean -- SPLIT TRANSPARENCY (RenderVKBlendSplit; LLCleanBlendSplitVK): the module's set, its params block and
// the translucency lighting volume's grid / SH helpers, shared by its four stages:
//   A  VOLUME   (clean_bs_volume.frag): a camera-aligned froxel grid of the light arriving at every cell --
//               sun / moon x visibility (cascades or one traced ray), the local lights (cluster walk; shadow-map
//               projectors, traced picks in the ray-query twin), the probes' / GI cache's ambient, the forward
//               alpha tail (haze, water fog, distance fade, froxel fog, precipitation veil) as an affine map per
//               cell. Built once per frame by a fullscreen pass over a 2D layout of the cells.
//   B  FRONT    (clean_draw.frag -DCLEAN_BS_FRONT): the nearest qualifying blend layer per pixel -- its decoded
//               surface and its identity (draw, primitive, face, water side).
//   C  LIGHT    (clean_bs_front.frag): that surface lit ONCE per pixel by the forward lighting (lightSurface: the
//               sun ray / cascades, local lights, probes, GI, SSR, the RT forward reflection) + the alpha tail.
//   D  BLEND    (clean_draw.frag -DCLEAN_BLEND_SPLIT, built on the CLEAN_BASIC placeholder module): every blend
//               layer -- material decode, ONE volume sample, an approximate specular, emissive / glow; the front
//               layer takes C's colour. No ray query, light loop, probe walk or GI cache read.
//
// THE GRID: x / y the main view's screen (the unjittered camera-relative view-projection of the build), z
// exponential view-depth slices from bs.cam.w (near) to bs.grid.w (far): slice coordinate
// w = ln(vz / near) / ln(far / near). Past far: the last slice (clamped). Cells hold:
//   0 / 1 / 2  the incoming radiance of R / G / B as L1 SH (x L0, yzw L1 along world x y z), the sun excluded --
//              in the forward path's "legacy irradiance" units: a white legacy diffuse surface of normal N
//              receives E(N) = shIrradiance(c, N)
//   3          rgb the sun / moon irradiance at normal incidence x its visibility (the forward sunlit x scol),
//              a the visibility alone (debug)
//   4 / 5      THE TAIL: final = colour x slope (5.rgb) + offset (4.rgb) -- haze, water fog, distance fade, froxel
//              fog and veil as the forward alpha FS applies them at that point (affine in colour).
#ifndef CLEAN_BS_COMMON_GLSL
#define CLEAN_BS_COMMON_GLSL

#ifndef BS_SET
#define BS_SET 2
#endif

layout(std140, set = BS_SET, binding = 0) uniform BsParamsBlock
{
    mat4  vp;      // camera-relative (P - cam.xyz) unjittered view-projection of the build
    mat4  ivp;     // its inverse
    mat4  pvp;     // the previous build's view-projection, relative to THIS build's camera (temporal reprojection)
    vec4  cam;     // xyz the camera (world), w the grid's near depth (m)
    vec4  fwd;     // xyz the camera forward, w ln(far / near)
    vec4  grid;    // xyz cells, w far (m)
    vec4  pfwd;    // xyz the previous camera's forward, w the previous camera's view depth offset (dot(cam - pcam, pfwd))
    vec4  knobs;   // x front min alpha, y front min alpha of a reflective layer, z temporal weight of this frame, w debug slice
    uvec4 flags;   // x debug view (BSD_*), y BSF_* bits, z frame counter, w render w | h << 16
    uvec4 tile;    // x tiles per row of the build's 2D layout, yzw 0
} bs;

// THE SET (LLCleanBlendSplitVK; every image GENERAL): 0 the params, 1-6 the volume as the readers sample it
// (trilinear, clamped), 7-11 the front surface F0-F4, 12 the front depth, 13 the front layer's lit colour (stage C),
// 14-19 the volume as stage A writes it, 20-23 the previous build's lighting (stage A's history).
#ifdef BS_READ_VOLUME
layout(set = BS_SET, binding = 1) uniform sampler3D uBsShR;
layout(set = BS_SET, binding = 2) uniform sampler3D uBsShG;
layout(set = BS_SET, binding = 3) uniform sampler3D uBsShB;
layout(set = BS_SET, binding = 4) uniform sampler3D uBsSun;
layout(set = BS_SET, binding = 5) uniform sampler3D uBsPostO;
layout(set = BS_SET, binding = 6) uniform sampler3D uBsPostS;
#endif
#ifdef BS_READ_FRONT
// F0 RGBA8_SRGB albedo (linear on read) + the base alpha; F1 RGBA16F oct N, oct Ng; F2 RGBA8 x rough | gloss,
// y metal, z ao | env, w the legacy glare coverage; F3 B10G11R11 PBR emissive | legacy specular colour; F4 RG32UI the
// identity (bsFrontKey); 12 the front depth (reversed-Z); 13 the lit colour (rgb, a the lit coverage).
layout(set = BS_SET, binding = 7) uniform texture2D uBsF0;
layout(set = BS_SET, binding = 8) uniform texture2D uBsF1;
layout(set = BS_SET, binding = 9) uniform texture2D uBsF2;
layout(set = BS_SET, binding = 10) uniform texture2D uBsF3;
layout(set = BS_SET, binding = 11) uniform utexture2D uBsKey;
layout(set = BS_SET, binding = 12) uniform texture2D uBsDepth;
layout(set = BS_SET, binding = 13) uniform texture2D uBsLit;
#endif
#ifdef BS_WRITE_VOLUME
layout(set = BS_SET, binding = 14, rgba16f) uniform writeonly image3D uBsOutShR;
layout(set = BS_SET, binding = 15, rgba16f) uniform writeonly image3D uBsOutShG;
layout(set = BS_SET, binding = 16, rgba16f) uniform writeonly image3D uBsOutShB;
layout(set = BS_SET, binding = 17, rgba16f) uniform writeonly image3D uBsOutSun;
layout(set = BS_SET, binding = 18, rgba16f) uniform writeonly image3D uBsOutPostO;
layout(set = BS_SET, binding = 19, rgba16f) uniform writeonly image3D uBsOutPostS;
layout(set = BS_SET, binding = 20) uniform sampler3D uBsHistR;
layout(set = BS_SET, binding = 21) uniform sampler3D uBsHistG;
layout(set = BS_SET, binding = 22) uniform sampler3D uBsHistB;
layout(set = BS_SET, binding = 23) uniform sampler3D uBsHistSun;
#endif

// THE FRONT LAYER'S IDENTITY (stage B writes it, stage D compares it): x the draw (gl_DrawID, saturated at 0xFFFF) |
// the face (bit 16, gl_FrontFacing) | the water side (bit 17: the post-water list) | the model (bits 18-19) | cel (20)
// | skin (21) | a legacy rez-effect emission in F3 (22) | valid (31); y the primitive (gl_PrimitiveID). A triangle covers a pixel once, so the draw, primitive,
// face and list name the front fragment exactly (the two face passes of the sorted draw rasterize each triangle once).
const uint BSK_VALID = 0x80000000u;
uvec2 bsKey(uint drawId, uint prim, bool front, bool post)
{
    return uvec2(min(drawId, 0xFFFFu) | (front ? 0x10000u : 0u) | (post ? 0x20000u : 0u) | BSK_VALID, prim);
}
bool bsKeyMatch(uvec2 stored, uvec2 own) { return (stored.x & 0x8003FFFFu) == own.x && stored.y == own.y; }
// pad0 bit 22 of the split pipelines' draws (B and D): the post-water list (the key's side bit)
const uint BS_POST_PUSH = 0x00400000u;

// bs.flags.y
const uint BSF_FRONT  = 1u;    // stage B / C ran this frame: the front layer's identity and colour are valid
const uint BSF_HISTORY = 2u;    // the previous build is valid (temporal reprojection)
const uint BSF_CASCADE = 4u;    // the sun cascades were rendered (plain sun visibility source)
const uint BSF_LOCAL   = 8u;    // local lights on
const uint BSF_PROBES  = 16u;   // the probe irradiance feeds the ambient
const uint BSF_TRACE   = 32u;   // the ray-query twin traces (sun / picked light / sky visibility)
const uint BSF_POST    = 64u;   // the post textures hold this frame's tail (else identity)

// bs.flags.x (RenderVKBlendSplitDebug)
const uint BSD_NONE    = 0u;
const uint BSD_VOLUME  = 1u;    // every blend layer: a white diffuse surface lit by the volume (+ the tail)
const uint BSD_SLICE   = 2u;    // fullscreen: the volume's slice bs.knobs.w (the overlay pass)
const uint BSD_FRONT   = 3u;    // the front layer green, every other layer blue
const uint BSD_C_ONLY  = 4u;    // the front layer's stage-C colour, every other layer black
const uint BSD_V_ONLY  = 5u;    // every other layer's volume colour, the front layer black
const uint BSD_HEAT    = 6u;    // layers shaded per pixel (each layer adds a constant)

// Normalised grid coordinate of the world point P (xy the screen, z the slice), and its view depth.
vec3 bsGridCoord(vec3 P, out float vz)
{
    const vec3 rel = P - bs.cam.xyz;
    const vec4 c = bs.vp * vec4(rel, 1.0);
    vz = dot(rel, bs.fwd.xyz);
    const vec2 uv = abs(c.w) > 1e-6 ? c.xy / c.w * 0.5 + 0.5 : vec2(0.5);
    const float w = log(max(vz, bs.cam.w) / bs.cam.w) / max(bs.fwd.w, 1e-4);
    return vec3(clamp(uv, vec2(0.0), vec2(1.0)), clamp(w, 0.0, 1.0));
}
// The view depth of slice coordinate w (0..1).
float bsSliceDepth(float w) { return bs.cam.w * exp(w * bs.fwd.w); }

// ---- L1 SH (radiance; Y00 = 0.282095, Y1m = 0.488603 (x, y, z)) ----
const float BS_Y0 = 0.282095;
const float BS_Y1 = 0.488603;
// Cosine-lobe convolution: A0 = pi, A1 = 2 pi / 3.
const float BS_A0 = 3.14159265;
const float BS_A1 = 2.09439510;
// Irradiance at normal N from the radiance coefficients c (x L0, yzw L1 x y z). Clamped at 0 (L1 rings).
float bsShIrr(vec4 c, vec3 N) { return max(BS_A0 * BS_Y0 * c.x + BS_A1 * BS_Y1 * dot(c.yzw, N), 0.0); }
vec3 bsShIrradiance(vec4 r, vec4 g, vec4 b, vec3 N) { return vec3(bsShIrr(r, N), bsShIrr(g, N), bsShIrr(b, N)); }
// Radiance along direction D (the blurry L1 reconstruction; clamped at 0).
float bsShRad(vec4 c, vec3 D) { return max(BS_Y0 * c.x + BS_Y1 * dot(c.yzw, D), 0.0); }
vec3 bsShRadiance(vec4 r, vec4 g, vec4 b, vec3 D) { return vec3(bsShRad(r, D), bsShRad(g, D), bsShRad(b, D)); }
// The coefficients of a delta light of irradiance E (at normal incidence) from direction l: its exact projection
// (energy-preserving over the sphere of normals; L1 reconstructs 0.75 E facing it and 0.25 E side-on -- the
// deep layers' local lights are that much softer than the front layer's exact ones).
vec4 bsShDelta(float E, vec3 l) { return E * vec4(BS_Y0, BS_Y1 * l); }
// The coefficients of an irradiance pair measured at +Z (Eu) and -Z (Ed): a vertical gradient (no horizontal L1).
//   E(+z) = A0 Y0 c0 + A1 Y1 c1z, E(-z) = A0 Y0 c0 - A1 Y1 c1z
vec4 bsShVertical(float Eu, float Ed) { return vec4(0.5 * (Eu + Ed) / (BS_A0 * BS_Y0), 0.0, 0.0, 0.5 * (Eu - Ed) / (BS_A1 * BS_Y1)); }

#endif   // CLEAN_BS_COMMON_GLSL
