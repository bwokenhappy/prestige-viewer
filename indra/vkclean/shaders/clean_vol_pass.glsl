// vkclean -- VOLUMETRIC FOG (#718): what the four froxel COMPUTE passes share -- their
// pipeline layout {draw set 0, draw set 1, own set}, the own set's bindings, the push block
// and the froxel -> world mapping. (Consumers use clean_vol_apply.glsl instead.)
//
// OWN SET (set 2; LLCleanVolFogVK, one set per history parity p):
//   0  uCur      rgba16f storage  the CURRENT frame's cells: the injection writes the medium
//                                 (sigma_s rgb, sigma_t), the light pass overwrites it in
//                                 place with (in-scatter rgb per metre, sigma_t)
//   1  VolList   SSBO             the local volume primitives (clean_vol_inject.comp)
//   2  uHistPrev sampler3D        last frame's history (linear: the reprojection's taps)
//   3  uHistCur  rgba16f storage  this frame's history (temporal writes, integration reads)
//   4  uInteg    rgba16f storage  the integrated volume (in-scatter rgb, transmittance a);
//                                 draw set 0 binding 45 samples the same image
#ifndef CLEAN_VOL_PASS_GLSL
#define CLEAN_VOL_PASS_GLSL

#define VOL_SET 2

layout(push_constant) uniform VolPush {
    mat4 reproj;   // temporal pass: this frame's camera-relative world -> LAST frame's grid
                   // clip (last vol_vp x translate(eye - last eye)); 0 in the other passes
    vec4 jitter;   // xyz this frame's sample offset inside a cell (cell units, [-0.5, 0.5)),
                   // w frame index (noise rotation)
    vec4 fwd;      // xyz the camera's forward axis (world, unit), w time (s; noise scroll)
    vec4 p0;       // per pass (header of each shader)
    vec4 p1;
} vp;

// Camera-relative world position of the continuous froxel coordinate g (cell units; the
// centre of cell i is i + 0.5): screen uv = g.xy / dims.xy on the unjittered projection,
// view depth = depth(g.z / dims.z).
vec3 volRelPos(vec3 g)
{
    const vec3 dims = volGrid().xyz;
    if(cmOn())
    {
        vec3 O,V; cmRay(g.xy/dims.xy,MEDIUM_EYE,O,V);
        const CameraMediumView view=ssr.medium[MEDIUM_EYE];
        const float factor=view.forward.w>0.5 ? 1.0 : max(dot(V,view.forward.xyz),1e-5);
        const float z=cmDepth(g.z/dims.z,view,factor);
        return O-volEye().xyz+V*(z/factor);
    }
    const vec2 ndc = g.xy / dims.xy * 2.0 - 1.0;
    const vec4 w = volInvVP() * vec4(ndc, 0.5, 1.0);
    const vec3 dir = w.xyz / w.w;
    const float vz = volDepthOfSlice(g.z / dims.z);
    return dir * (vz / max(dot(dir, vp.fwd.xyz), 1e-6));
}

// Interleaved gradient noise (Jimenez 2014).
float volIgn(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }

// PER-CELL ROTATION (#1606): a fixed offset in [0, 1)^3 per cell, spatially well spread (IGN over
// the column with a per-slice shift; y uses the transposed linear form so x and y are not
// functions of each other; z an integer hash). Every per-frame sequence below is rotated by it
// (Cranley-Patterson): each cell still walks a low-discrepancy sequence over the frames -- its
// temporal average converges like a stratified one -- while neighbouring cells are at
// different points of it in the same frame. Before, the whole grid took ONE jitter offset per
// frame: a shadow / gobo edge finer than a cell aliased into a moire that moved coherently
// every frame (the crawl), and the temporal pass's neighbourhood box only ever saw the current
// phase, so it clamped the converged history back toward it.
vec3 volCellRotation(uvec3 c)
{
    uint h = c.x * 73856093u ^ c.y * 19349663u ^ c.z * 83492791u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    const float s = 5.588238 * float(c.z);
    return vec3(volIgn(vec2(c.xy) + s), volIgn(vec2(c.yx) + vec2(31.0, 17.0) + s), float(h >> 8) * (1.0 / 16777216.0));
}

// This frame's sample position inside cell c (cell units): the frame's Halton (2, 3, 5)
// offset (vp.jitter, stepped every frame by the C++ side, independent of TAA) rotated by the
// cell's own offset (volCellRotation). The injection and the light pass call this with the
// same c: the medium and its lighting are sampled at one point.
vec3 volSampleCoord(uvec3 c)
{
    return vec3(c) + fract(vp.jitter.xyz + 0.5 + volCellRotation(c));
}

// This frame's point of a per-cell rotated 1D / 2D low-discrepancy sequence (golden-ratio Weyl /
// Roberts R2) for the stochastic terms (ray directions, light picks, cascade pick); frame: a
// frame counter. The rotation is its own hash of the cell (not volCellRotation's: the numbers
// must not be tied to the cell's sample offset within a frame); salt decorrelates the terms of
// one cell from each other.
vec3 volSeqRotation(uvec3 c)
{
    uint h = c.x * 2654435761u ^ c.y * 2246822519u ^ c.z * 3266489917u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    uint g = h * 0x9E3779B9u + 0x7F4A7C15u; g ^= g >> 16; g *= 0x85ebca6bu; g ^= g >> 13;
    uint k = g * 0xC2B2AE35u + 0x165667B1u; k ^= k >> 16;
    return vec3(float(h >> 8), float(g >> 8), float(k >> 8)) * (1.0 / 16777216.0);
}
float volSeq1(uvec3 c, float frame, float salt)
{
    return fract(volSeqRotation(c).x + salt * 0.7548777 + mod(frame, 4096.0) * 0.6180339887);
}
vec2 volSeq2(uvec3 c, float frame, float salt)
{
    return fract(volSeqRotation(c).yz + salt * vec2(0.3819660, 0.2451223) + mod(frame, 4096.0) * vec2(0.7548777, 0.5698403));
}

#endif
