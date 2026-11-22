// vkclean -- #1585 HEADLESS SELF (VR "Show my body in VR", SlipstreamVRShowBody): the rigged VS's
// per-vertex head test. The rule and its CPU reference: indra/newview/llheadlessbody.h.
//
// A main-view rigged draw pushes u_ambient_color.w = 0.5 (LLCleanRendererVK::selfHideW) while the
// mode is on. Then a vertex of the agent's OWN avatar (instance bit 13, not HUD) whose summed skin
// weight on head-side joints (mHead and every joint under it: skull, eyes, Bento face, ears, jaw,
// tongue, ...) is at least one half collapses onto the mHead joint's position: whole-head
// triangles become points, the triangles that cross the cut close over the neck as a small cap
// (VRChat's head-bone shrink, without shrinking the neck). The point is a WORLD position, so
// every eye of a stereo pass sees the same closed cap -- no sliver reaches toward an eye. Mirrors,
// probes, shadows and RT never push 0.5: they draw the whole avatar.
//
// Inputs (all existing records, no per-frame work outside the mode):
//   the weights        draw set 0 binding 70 = the pre-skin's weight arena (llcleanqvertex.h
//                      QWeight, 10 B per vertex), at rig_cache RC_WBASE (the main cull writes it
//                      for own rows under HEADLESS_SELF) + the vertex's index in its source slot;
//   the head joints    the skin meta (binding 38, LLCleanSkinStore::SkinMeta): words 2 / 3 / 114 /
//                      115 bits 0..13 = the skin's head-side joints (bit k = joint_nums[k]), word
//                      115 bits 16..31 = the mHead joint number + 1, word 0 = the avatar's
//                      joint-table slot;
//   the target         the joint table (binding 2): mHead's world translation, minus the eye.
// The includer declares avatar_joints (clean_joint_table.glsl) and, where it has one, the cel
// skin-meta view (clean_cel_common.glsl under CEL_VERTEX); it is declared here otherwise.
#ifndef CLEAN_HEADLESS_SELF_GLSL
#define CLEAN_HEADLESS_SELF_GLSL

#ifndef CEL_VERTEX
layout(std430, set = 0, binding = 38) readonly buffer CelSkinMetaSSBO { uint cel_skin_meta[]; };
const uint CEL_SKIN_META_WORDS = 1892u;   // LLCleanSkinStore::SkinMeta / 4
#endif
layout(std430, set = 0, binding = 70) readonly buffer HeadlessWeights { uint hl_wts[]; };

const uint HL_META_LO   = 2u;       // SkinMeta::head_lo[0..1]
const uint HL_META_HI   = 114u;     // SkinMeta::head_hi[0..1]
const uint HL_MASK_HI   = 0x3FFFu;  // head_hi[1]: skin joints 96..109 (llheadlessbody.h kMaskHiBits)
const uint HL_SKIN_MAX  = 109u;     // the pre-skin's joint clamp (MAX_JOINTS - 1)

// The push word says "headless self" (0.5; 1 = hide, 0.25 = system head only, 0 = none).
bool headlessPass(float w) { return w > 0.4 && w < 0.6; }

bool hlJointIsHead(uvec4 m, uint j)
{
    j = min(j, HL_SKIN_MAX);
    const uint w = j < 64u ? (j < 32u ? m.x : m.y) : (j < 96u ? m.z : (m.w & HL_MASK_HI));
    return ((w >> (j & 31u)) & 1u) != 0u;
}

// The vertex's weight on head-side joints, unorm16 (= llheadlessbody.h headWeight; the record's
// decode = clean_preskin.comp loadWeights).
uint hlHeadWeight(uint rec, uvec4 m)
{
    const uint b = rec * 10u;
    const uint wd = b >> 2u;
    if (wd + 3u > uint(hl_wts.length())) return 0u;
    const uint a = hl_wts[wd], mid = hl_wts[wd + 1u], c = hl_wts[wd + 2u];
    uint jw, w01, w2;
    if ((b & 2u) == 0u) { jw = a;                         w01 = mid;                       w2 = c & 0xFFFFu; }
    else                { jw = (a >> 16u) | (mid << 16u); w01 = (mid >> 16u) | (c << 16u); w2 = c >> 16u; }
    const uint q0 = w01 & 0xFFFFu, q1 = w01 >> 16u;
    const uint qs = q0 + q1 + w2;
    const uint q3 = qs >= 65535u ? 0u : 65535u - qs;
    uint h = 0u;
    if (hlJointIsHead(m, jw & 0xFFu))          h += q0;
    if (hlJointIsHead(m, (jw >> 8u) & 0xFFu))  h += q1;
    if (hlJointIsHead(m, (jw >> 16u) & 0xFFu)) h += w2;
    if (hlJointIsHead(m, (jw >> 24u) & 0x7Fu)) h += q3;   // (bit 7: the weld flag)
    return h;
}

// True when this vertex of skin `skin_id` (weights from record `wbase` + `local`) is a head
// vertex; `target` = mHead's position relative to `eye` (the space of the pre-skinned stream).
bool headlessCollapse(uint skin_id, uint wbase, uint local, vec3 eye, out vec3 target)
{
    target = vec3(0.0);
    if (wbase == 0xFFFFFFFFu || skin_id >= 0x100000u
        || (skin_id + 1u) * CEL_SKIN_META_WORDS > uint(cel_skin_meta.length()))   // (38 aliased: too short)
        return false;
    const uint mb = skin_id * CEL_SKIN_META_WORDS;
    const uvec4 m = uvec4(cel_skin_meta[mb + HL_META_LO], cel_skin_meta[mb + HL_META_LO + 1u],
                          cel_skin_meta[mb + HL_META_HI], cel_skin_meta[mb + HL_META_HI + 1u]);
    if ((m.x | m.y | m.z | (m.w & HL_MASK_HI)) == 0u) return false;   // no head joint in this skin
    if (2u * hlHeadWeight(wbase + local, m) < 65535u) return false;    // below one half
    const uint hj = (m.w >> 16u) - 1u;
    const uint hslot = cel_skin_meta[mb];
    if (hj >= 216u || hslot >= uint(avatar_joints.length())) return false;   // (JOINT-ROW-GUARD)
    const mat3x4 W = avatar_joints[hslot].world[hj];
    target = vec3(W[0].w, W[1].w, W[2].w) - eye;
    return true;
}

#endif
