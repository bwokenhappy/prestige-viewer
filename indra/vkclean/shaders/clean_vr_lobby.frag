#version 450
// vkclean -- #1596 the VR login room (LLXrSession::recordLobbyEye): drawn straight into each eye
// image by a fullscreen triangle (clean_glow_combine.vert), before the world exists (login,
// loading). No world renderer runs then -- no frame info, haze medium, light lists, G-buffer or
// glow chain -- so the room is ray-cast here in closed form, per pixel, from that eye's located
// pose and FOV (exact stereo parallax, no depth buffer, no descriptors: push constants only).
//
//  - THE DOORWAY: a frame with depth -- core jambs and lintel (the reveal), proud casing, plinth
//    blocks, a cornice cap and a threshold -- each an edge-chamfered box, sphere-traced inside its
//    bounding box. Dark blue-charcoal, glossy (roughness 0.15), slightly metallic.
//  - THE OPENING: a two-sided emissive membrane exactly kDoorWidth x kDoorHeight in the doorway's
//    plane (the pointing / walk-through tests use the same rectangle). Its radiance is read from a
//    virtual passage beyond it (a warm bright core 1.8 m behind, seen with parallax, cooler and
//    dimmer towards the reveal, brighter towards the floor): light through a passage. HDR (~1-9).
//  - LIGHT SPILL: the opening is a rectangular area light of its own average radiance: exact
//    Lambert polygon irradiance (vector form factor) on the frame and the floor; their gloss is the
//    GGX lobe's coverage of the opening (representative point, roughness-widened) -- the reflection
//    shows the opening's own gradient. A soft fill sphere in front stands for the light the fog and
//    the floor send back (the frame's front faces catch it).
//  - FOG: a density field (a low layer over the whole floor + a plume rolling out of the doorway +
//    a glow inside the opening), shaped by three octaves of value noise drifting out of the doorway
//    at different speeds with a travelling roll. Marched with FIXED stratified samples (no temporal
//    filter exists here, and none is relied on): uniform ones for extinction and the room's ambient
//    in-scatter, equi-angular ones about the opening for its light (Henyey-Greenstein, forward).
//  - BLOOM: the opening is the only HDR source in the room, so its glare is evaluated in closed form
//    (an angular falloff around the opening as seen from this eye, three widths) instead of a blur
//    chain: same look, stereo-consistent, no extra targets.
//  - Tone map (extended Reinhard, linear toe: the dark room stays as dark as before), sRGB encode
//    when the view is UNORM (MUTABLE_FORMAT eye images), a static +-0.5 LSB dither (no crawl).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(push_constant) uniform LobbyPush
{
    vec4 eyeTime;   // xyz the eye (app space, m); w the animation clock (s, wraps hourly)
    vec4 rayBase;   // xyz the (unnormalised) view ray through the image's top-left corner; w the room's centre, door-local x
    vec4 rayDu;     // xyz its change across the image's width; w the room's centre, door-local z
    vec4 rayDv;     // xyz its change down the image's height
    vec4 door;      // xyz the doorway's centre on the floor (app space); w its yaw about +Y (rad)
    vec4 params;    // x opening width, y opening height (m), z brightness (1; more when hovered), w 1: encode sRGB
} pc;

const float PI = 3.14159265;
const float kSill = 0.035;                                  // the threshold's top (the opening starts here)
const vec3 kEmit = vec3(1.0, 0.92, 0.80);                   // the opening's light: warm white (its core)
const vec3 kEdgeTint = vec3(0.78, 0.86, 1.0);               // ... cooler towards its edges
const float kEmitLum = 4.0;                                 // its radiance scale (HDR: drives the glare)
const vec3 kBg = vec3(0.00193, 0.00217, 0.00350);           // the dark space (sRGB 0.025 0.028 0.045, as before)
const vec3 kAmb = vec3(0.022, 0.027, 0.038);                // the room's dim blue-grey ambient (mist, fill)
const vec3 kFrameBase = vec3(0.016, 0.019, 0.026);          // near-black blue-grey
const float kFrameRough = 0.15;
const float kFrameMetal = 0.2;
const vec3 kFloorBase = vec3(0.020, 0.022, 0.027);
const float kFloorRough = 0.42;
const float kFogScale = 0.55;                               // extinction (1/m) at full density
const float kFogAlbedo = 0.9;
const float kFogG = 0.35;                                   // forward scattering
const int kStepsUniform = 14;
const int kStepsLight = 12;

float gHw;      // the opening's half width
float gH;       // and height
float gTime;
vec2 gRoom;     // the room's centre (where you stood when the wall was placed), door-local xz

// ---- geometry (door-local: x right, y up from the floor, z towards the room; opening at z = 0)

// An edge-chamfered box (centre c, half size b, chamfer ch cut along every edge). The chamfer is a
// max of planes: a lower bound of the distance, safe to sphere-trace.
float sdChamferBox(vec3 p, vec3 c, vec3 b, float ch)
{
    vec3 q = abs(p - c) - b;
    float box = length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
    float e = max(max(q.x + q.y, q.y + q.z), q.x + q.z);
    return max(box, (e + ch) * 0.70710678);
}

float sdFrame(vec3 p)
{
    vec3 m = vec3(abs(p.x), p.y, p.z);   // (mirrored: both jambs from one)
    float hw = gHw, H = gH;
    // the core jamb (the reveal): 13 cm wide, 30 cm deep, up to the lintel's top
    float d = sdChamferBox(m, vec3(hw + 0.065, 0.5 * (H + 0.15), 0.0), vec3(0.065, 0.5 * (H + 0.15), 0.15), 0.010);
    // the core lintel
    d = min(d, sdChamferBox(p, vec3(0.0, H + 0.075, 0.0), vec3(hw + 0.13, 0.075, 0.15), 0.010));
    // the casing: 3 cm back from the opening, 18 cm wide, 5 cm proud of the reveal each side
    d = min(d, sdChamferBox(m, vec3(hw + 0.12, 0.5 * (H + 0.03), 0.0), vec3(0.09, 0.5 * (H + 0.03), 0.20), 0.025));
    // the head casing
    d = min(d, sdChamferBox(p, vec3(0.0, H + 0.13, 0.0), vec3(hw + 0.21, 0.10, 0.20), 0.025));
    // the plinth blocks at the casing's foot
    d = min(d, sdChamferBox(m, vec3(hw + 0.125, 0.13, 0.0), vec3(0.105, 0.13, 0.22), 0.015));
    // the cornice cap
    d = min(d, sdChamferBox(p, vec3(0.0, H + 0.265, 0.0), vec3(hw + 0.27, 0.035, 0.25), 0.012));
    // the threshold
    d = min(d, sdChamferBox(p, vec3(0.0, 0.5 * kSill, 0.0), vec3(hw + 0.25, 0.5 * kSill, 0.27), 0.008));
    return d;
}

vec3 frameNormal(vec3 p)
{
    const vec2 k = vec2(1.0, -1.0);
    const float h = 0.0006;
    return normalize(k.xyy * sdFrame(p + k.xyy * h) + k.yyx * sdFrame(p + k.yyx * h) +
                     k.yxy * sdFrame(p + k.yxy * h) + k.xxx * sdFrame(p + k.xxx * h));
}

// Ray (o, d) against an axis-aligned box: (enter, exit); enter > exit = a miss.
vec2 boxHit(vec3 o, vec3 d, vec3 bmin, vec3 bmax)
{
    vec3 dd = mix(d, vec3(1e-7), lessThan(abs(d), vec3(1e-7)));
    vec3 inv = 1.0 / dd;
    vec3 t0 = (bmin - o) * inv, t1 = (bmax - o) * inv;
    vec3 lo = min(t0, t1), hi = max(t0, t1);
    return vec2(max(max(lo.x, lo.y), lo.z), min(min(hi.x, hi.y), hi.z));
}

// ---- the opening

// The membrane's radiance where a ray (direction d) meets it at `hit` (z = 0): the virtual
// passage beyond it -- a bright core 1.8 m further along the ray -- brighter towards the floor,
// a little dimmer along the reveal.
vec3 membrane(vec3 hit, vec3 d)
{
    vec3 q = hit + d * (1.8 / max(abs(d.z), 0.08));
    vec2 rel = (q.xy - vec2(0.0, 0.45 * gH)) / vec2(0.75, 1.1);
    float core = exp(-dot(rel, rel));
    float edge = smoothstep(0.0, 0.18, min(gHw - abs(hit.x), gH - hit.y));
    float vgrad = 1.0 + 0.3 * (1.0 - clamp(hit.y / gH, 0.0, 1.0));
    float shimmer = 1.0 + 0.04 * sin(gTime * 0.7 + q.y * 2.3 + q.x * 1.7);
    return mix(kEdgeTint, kEmit, core) * (kEmitLum * pc.params.z * (0.15 + 1.6 * core) * mix(0.5, 1.0, edge) * vgrad * shimmer);
}

vec3 openingAverage()
{
    return kEmit * kEmitLum * pc.params.z;
}

vec3 ffEdge(vec3 a, vec3 b)
{
    vec3 c = cross(a, b);
    return acos(clamp(dot(a, b), -0.9999, 0.9999)) * c / max(length(c), 1e-6);
}

// The opening's vector form factor at x: irradiance on a surface of normal n is
// PI * L * max(dot(n, F), 0), and PI * length(F) its (projected) solid angle. Pointed towards the
// opening whichever side x is on (the membrane emits both ways).
vec3 openingFF(vec3 x)
{
    vec3 c0 = normalize(vec3(-gHw, kSill, 0.0) - x);
    vec3 c1 = normalize(vec3(gHw, kSill, 0.0) - x);
    vec3 c2 = normalize(vec3(gHw, gH, 0.0) - x);
    vec3 c3 = normalize(vec3(-gHw, gH, 0.0) - x);
    vec3 F = (ffEdge(c0, c1) + ffEdge(c1, c2) + ffEdge(c2, c3) + ffEdge(c3, c0)) * (0.5 / PI);
    vec3 toC = vec3(0.0, 0.5 * (kSill + gH), 0.0) - x;
    return dot(F, toC) < 0.0 ? -F : F;
}

// The point of the opening nearest the ray (x, r) and how much of a lobe of width sigma (rad) it
// fills: 1 inside, a gaussian falloff outside its edge, scaled down when the opening is smaller
// than the lobe (omega: its solid angle from x).
float openingLobe(vec3 x, vec3 r, float sigma, float omega, out vec3 P)
{
    vec2 q = x.xy;
    if (abs(r.z) > 1e-4)
    {
        float t = -x.z / r.z;
        if (t > 0.0) q = x.xy + r.xy * t;
    }
    P = vec3(clamp(q, vec2(-gHw, kSill), vec2(gHw, gH)), 0.0);
    float ang = acos(clamp(dot(r, normalize(P - x)), -1.0, 1.0));
    return exp(-(ang * ang) / (sigma * sigma)) * min(1.0, omega / (PI * sigma * sigma));
}

// ---- surfaces

vec3 shade(vec3 p, vec3 n, vec3 V, vec3 base, float rough, float metal)
{
    vec3 Lavg = openingAverage();
    vec3 F = openingFF(p);
    float omega = PI * length(F);
    vec3 F0 = mix(vec3(0.04), base, metal);
    vec3 diffC = base * (1.0 - metal);
    float NoV = max(dot(n, V), 1e-3);
    vec3 Fr = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);
    float alpha = rough * rough;
    float sigma = 1.6 * alpha + 0.02;
    vec3 r = reflect(-V, n);

    // The opening: diffuse spill + its reflection.
    vec3 col = diffC * Lavg * max(dot(n, F), 0.0);
    vec3 P;
    float cov = openingLobe(p, r, sigma, omega, P);
    vec3 Lp = normalize(P - p);
    col += Fr * membrane(P, Lp) * cov * smoothstep(0.0, 0.05, dot(n, Lp));

    // The fill: the lit fog and floor in front of the opening (on this point's side), a soft sphere.
    vec3 fc = vec3(0.0, 0.75, p.z >= 0.0 ? 0.8 : -0.8);
    vec3 fl = fc - p;
    float fd = length(fl);
    fl /= fd;
    float sinB = min(0.75 / fd, 0.999);
    vec3 Lfill = Lavg * 0.018;
    col += diffC * Lfill * sinB * sinB * max(dot(n, fl), 0.0);
    float beta = asin(sinB);
    float angF = acos(clamp(dot(r, fl), -1.0, 1.0));
    float covF = (1.0 - smoothstep(beta - sigma, beta + sigma, angF)) * min(1.0, (sinB * sinB) / (sigma * sigma));
    col += Fr * Lfill * covF;

    // The room: a dim ambient and the reflection of the misty floor / the dark space.
    vec3 env = mix(kBg * 1.2, kAmb * 0.5, smoothstep(0.25, -0.25, r.y));
    col += diffC * kAmb * 0.35 + Fr * env;
    return col;
}

// ---- fog

uint hashU(ivec3 i)
{
    uvec3 u = uvec3(i) * uvec3(1597334673u, 3812015801u, 2798796415u);
    return (u.x ^ u.y ^ u.z) * 1597334673u;
}

float hashF(ivec3 i)
{
    return float(hashU(i) >> 8) * (1.0 / 16777216.0);
}

float vnoise(vec3 x)
{
    vec3 fl = floor(x);
    ivec3 i = ivec3(fl);
    vec3 f = x - fl;
    vec3 s = f * f * (3.0 - 2.0 * f);
    float a = mix(hashF(i), hashF(i + ivec3(1, 0, 0)), s.x);
    float b = mix(hashF(i + ivec3(0, 1, 0)), hashF(i + ivec3(1, 1, 0)), s.x);
    float c = mix(hashF(i + ivec3(0, 0, 1)), hashF(i + ivec3(1, 0, 1)), s.x);
    float d = mix(hashF(i + ivec3(0, 1, 1)), hashF(i + ivec3(1, 1, 1)), s.x);
    return mix(mix(a, b, s.y), mix(c, d, s.y), s.z);
}

// Extinction (1/m) at door-local p.
float fogDensity(vec3 p)
{
    // A low layer over the floor around the room's centre (where you stand), fading out well
    // inside the march box.
    float rRoom = length(p.xz - gRoom);
    float layer = exp(-p.y / 0.3) * (1.0 - smoothstep(3.5, 6.5, rRoom));
    // A taller plume rolling out of the doorway, densest at the threshold (both sides, the
    // room's side larger), under ~1.9 m.
    float pz = p.z - 0.45;
    float plume = exp(-p.y / 0.55) * (1.0 - smoothstep(1.4, 1.9, p.y)) *
                  exp(-(p.x * p.x) / 1.44 - (pz * pz) / (pz > 0.0 ? 2.0 : 0.6));
    // A glow inside the opening (light through a passage).
    vec3 a = vec3(abs(p.x) - gHw, p.y - 0.5 * gH, abs(p.z) - 0.18);
    float inside = (1.0 - smoothstep(-0.1, 0.05, a.x)) * (1.0 - smoothstep(0.5 * gH - 0.2, 0.5 * gH, abs(a.y))) *
                   (1.0 - smoothstep(-0.05, 0.1, a.z));
    float base = 0.9 * layer + 0.6 * plume + 0.35 * inside;
    if (base < 0.002) return 0.0;

    // Rolling: out of the doorway along +z, a slow travelling roll lifting and dropping the layers,
    // a meander across, each octave at its own speed (reads as swirl).
    float t = gTime;
    vec3 q = p * vec3(0.9, 1.9, 0.9);
    q.y += 0.18 * sin(q.z * 1.7 - t * 0.35);
    q.x += 0.25 * sin(q.z * 1.1 + t * 0.13);
    float n = 0.55 * vnoise(q + vec3(0.0, 0.0, -t * 0.06));
    n += 0.30 * vnoise(q * 2.03 + vec3(t * 0.05, t * 0.02, -t * 0.11));
    n += 0.15 * vnoise(q * 4.1 + vec3(-t * 0.09, 0.0, -t * 0.17));
    return kFogScale * base * (0.1 + 1.8 * smoothstep(0.38, 0.78, n));
}

float hg(float g, float mu)
{
    float k = 1.0 + g * g - 2.0 * g * mu;
    return (1.0 - g * g) / (4.0 * PI * k * sqrt(k));
}

// ---- output

vec3 toSrgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 fromSrgb(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main()
{
    gHw = 0.5 * pc.params.x;
    gH = pc.params.y;
    gTime = pc.eyeTime.w;
    gRoom = vec2(pc.rayBase.w, pc.rayDu.w);

    // The view ray, into door-local space.
    vec3 dirW = normalize(pc.rayBase.xyz + v_uv.x * pc.rayDu.xyz + v_uv.y * pc.rayDv.xyz);
    float cy = cos(pc.door.w), sy = sin(pc.door.w);
    vec3 axR = vec3(cy, 0.0, -sy), axN = vec3(sy, 0.0, cy);
    vec3 rel = pc.eyeTime.xyz - pc.door.xyz;
    vec3 o = vec3(dot(rel, axR), rel.y, dot(rel, axN));
    vec3 d = vec3(dot(dirW, axR), dirW.y, dot(dirW, axN));

    // Nearest of: the floor, the opening, the frame.
    float tHit = 1e30;
    int kind = 0;   // 0 the dark space, 1 the frame, 2 the opening, 3 the floor
    if (d.y < -1e-5 && o.y > 0.0)
    {
        tHit = -o.y / d.y;
        kind = 3;
    }
    if (abs(d.z) > 1e-6)
    {
        float tm = -o.z / d.z;
        vec3 pm = o + d * tm;
        if (tm > 0.0 && tm < tHit && abs(pm.x) < gHw && pm.y > kSill && pm.y < gH)
        {
            tHit = tm;
            kind = 2;
        }
    }
    vec2 bb = boxHit(o, d, vec3(-gHw - 0.28, 0.0, -0.28), vec3(gHw + 0.28, gH + 0.31, 0.28));
    if (bb.x < bb.y && bb.y > 0.0 && bb.x < tHit)
    {
        float t = max(bb.x, 0.0);
        float tEnd = min(bb.y, tHit);
        for (int i = 0; i < 72 && t < tEnd; ++i)
        {
            float s = sdFrame(o + d * t);
            if (s < 0.0004 * (1.0 + t))
            {
                tHit = t;
                kind = 1;
                break;
            }
            t += s * 0.9;
        }
    }

    vec3 col = kBg;
    vec3 p = o + d * tHit;
    if (kind == 2) col = membrane(p, d);
    else if (kind == 1) col = shade(p, frameNormal(p), -d, kFrameBase, kFrameRough, kFrameMetal);
    else if (kind == 3)
    {
        // (fades into the dark space far away: no horizon line)
        float distFade = smoothstep(5.0, 9.0, length(p.xz - gRoom));
        col = mix(shade(p, vec3(0.0, 1.0, 0.0), -d, kFloorBase, kFloorRough, 0.0), kBg, distFade);
    }

    // The fog, from the eye to the hit (inside the fog's box).
    vec3 Lavg = openingAverage();
    vec3 inscat = vec3(0.0);
    float Tsurf = 1.0;
    // (the march box: the room's floor layer around its centre, and the doorway's plume)
    vec2 boxLo = min(gRoom - 6.8, vec2(-1.6)), boxHi = max(gRoom + 6.8, vec2(1.6));
    vec2 fb = boxHit(o, d, vec3(boxLo.x, 0.0, boxLo.y), vec3(boxHi.x, 3.2, boxHi.y));
    float ta = max(fb.x, 0.0);
    float tb = min(fb.y, kind == 0 ? 40.0 : tHit);
    if (tb > ta)
    {
        // Uniform: extinction and the ambient in-scatter; the optical depth at each step's end.
        float dt = (tb - ta) / float(kStepsUniform);
        float tauAt[kStepsUniform + 1];
        float tau = 0.0;
        tauAt[0] = 0.0;
        for (int i = 0; i < kStepsUniform; ++i)
        {
            float s = fogDensity(o + d * (ta + (float(i) + 0.5) * dt));
            inscat += exp(-(tau + 0.5 * s * dt)) * (kFogAlbedo * s * dt) * kAmb;
            tau += s * dt;
            tauAt[i + 1] = tau;
        }
        Tsurf = exp(-tau);

        // Equi-angular about the opening's centre: its light, where the 1/r^2 changes fastest.
        vec3 c = vec3(0.0, 0.5 * (kSill + gH), 0.0);
        float delta = dot(c - o, d);
        float D = max(length(o + d * delta - c), 0.03);
        float thA = atan((ta - delta) / D), thB = atan((tb - delta) / D);
        vec3 lit = vec3(0.0);
        for (int j = 0; j < kStepsLight; ++j)
        {
            float tj = delta + D * tan(mix(thA, thB, (float(j) + 0.5) / float(kStepsLight)));
            vec3 x = o + d * tj;
            float s = fogDensity(x);
            if (s <= 0.0) continue;
            float pdf = D / ((thB - thA) * (D * D + (tj - delta) * (tj - delta)));
            float fu = clamp((tj - ta) / dt, 0.0, float(kStepsUniform));
            int k = min(int(fu), kStepsUniform - 1);
            float tauJ = mix(tauAt[k], tauAt[k + 1], fu - float(k));
            float omega = PI * length(openingFF(x));
            float ph = hg(kFogG, dot(normalize(c - x), d));
            lit += vec3(exp(-tauJ) * kFogAlbedo * s * omega * ph / pdf);
        }
        inscat += lit * (1.0 / float(kStepsLight)) * Lavg;
    }
    col = col * Tsurf + inscat;

    // Bloom: the opening's glare around it as seen from this eye (it is the only HDR source).
    {
        vec3 Pc = vec3(0.0, 0.5 * (kSill + gH), 0.0);
        vec3 q = Pc;
        if (abs(d.z) > 1e-5) q = o + d * (-o.z / d.z);
        vec3 P = vec3(clamp(q.xy, vec2(-gHw, kSill), vec2(gHw, gH)), 0.0);
        float ang = acos(clamp(dot(d, normalize(P - o)), -1.0, 1.0));
        float facing = abs(normalize(o - Pc).z);
        col += Lavg * facing * (0.040 * exp(-ang / 0.015) + 0.010 * exp(-ang / 0.045) + 0.002 * exp(-ang / 0.2));
    }

    // Tone map (extended Reinhard, white 10: linear at the dark end), encode, dither.
    const float w2 = 100.0;
    col = max(col, vec3(0.0));
    col = col * (1.0 + col / w2) / (1.0 + col);
    float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    vec3 enc = clamp(toSrgb(col) + dither / 255.0, 0.0, 1.0);
    o_color = vec4(pc.params.w > 0.5 ? enc : fromSrgb(enc), 1.0);
}
