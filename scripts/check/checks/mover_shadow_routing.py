"""Regression guard for the MOVER SHADOW ROUTING (RenderVKShadowMoverMask, 2026-11-09).

Under the stochastic lighting path every temporal fix for moving-caster shadows traded smear / trailing against
reveal points / speckle (RenderVKShadowCasterHistory 0-3, the mover record, the radiance light-drop test): the
owner kept seeing speckle on an idle avatar's hair self-shadow and a trail behind a walking avatar's shadow. The fix
is structural: the stochastic histories only ever hold STATIC occlusion and the movers' shadows come from the RT
shadow mask pipeline (the non-stochastic path's per-frame trace + its moving-caster denoiser):
  * the TLAS instance pass puts every mover in the mover classes (RT_MASK_MOVER = rigged | attachment: rigged /
    worn as ever, a moving static opaque / alpha-mask instance re-classed into the attachment class for a hold) and
    writes the movers' bounds (clean_rt_instances.comp moverClass / moverAvatar / moverObject);
  * pass 15's walks (every stochastic shadow ray) AND their TLAS mask with ~RT_MASK_MOVER and pass blend-class
    movers over per candidate (clean_vis_walk.glsl RT_WALK_MASK_AND / RT_WALK_MOVER_FILTER, PDN_OPT_MOVER);
  * the mask traces MOVER-ONLY rays (mover classes + blend, blend non-movers passed over) for the merged sun and
    the stochastic lights, only where a mover sphere can lie between the texel and the light (THE MOVER CULL), and
    the lighting multiplies the static stochastic result by them (clean_shadow_mask_read.glsl rtMover*).
This check fails when the routing is broken on either side -- the stochastic rays seeing a mover again (the history
smear comes back), the mask's mover rays not seeing them (mover shadows vanish), the cull dropping a mover that
blocks a light (holes) -- or when the wiring between the TLAS pass, pass 15, the mask, the host and the lighting is
cut:
  (a) WIRING: the walk hook in front of both of the walk's ray queries and the candidate filter in its loop; pass
      15's hook defined before the walk library is included; the mask's caster sets and mover traces; the host's
      PDN_OPT_MOVER bit and caster-policy bypass; the three consumers; the nonpersistent setting; the layout twins
      (clean_rt_movers.glsl MV_* = llcleanrtvk.h kMv*, compute binding 29 = kCompBindings - 1).
  (b) CLASSES: the production mask macros evaluated over every instance class -- the stochastic rays (every ray
      mask the shadow cores use, ANDed with pass 15's hook) hit no mover class and every static one; the mask's
      mover rays hit every mover class and no static non-blend one; the blend filter's two senses.
  (c) CULL: the production sphere tests (clean_rt_movers.glsl mvSegHit / mvRayHit) and the mask's subgroup
      predicates (mvCullBuild's hs / hl lines), compiled into C++: over random tiles of receivers, lights, sun
      directions and movers, every receiver -> light segment / sun ray a mover sphere blocks (brute force) must be
      kept by the tile predicate AND the per-texel test. A cull that drops one is a missing shadow.
2026-11-09 follow-up (the owner, in-world): two artifacts with the routing on, each guarded here:
  (d) FACETED SELF-SHADOW on a smooth-shaded avatar limb (triangle-shaped spikes): the mask's point-light mover
      rays left the facet with no terminator lift (the probe ran for projectors only) through a copy of the
      origin rule. Now every light kind takes the shared probe (mvLightWeight: rtTerminatorCached, the smooth
      normal deciding the facing gate) and the shared origin (clean_shadow_rt.glsl rtPosOriginSetup /
      rtPosOriginAt, the projector core's and the mask's).
  (e) LIT HOLES inside an avatar's shadow:
      * the own-fixture rule tested a SKINNED caster's mesh box in its group record's (pelvis / eye) space -- a
        limb, ear or tail whose box "held" the lamp (RenderVKReSTIRFixtureSize, default 1.5 m) was passed over as
        the lamp's fixture: rtFixtureBoxAB never takes a skinned caster now (every walk: mask, pass 15, GI);
      * the radiance packing's per-frame ratio sum w Vs Vm / sum w Vs jumped to the other lights' value on every
        frame whose static sample of the dominant light came back blocked (and a texel with no static light wrote
        a lit 1): the REST is now TWO filtered sums (N, D) and the reader divides them -- the replay below (a
        model of the 2-frame mover history) bounds the new estimator and shows the old one's spikes;
      * a soft projector's rays aim anywhere on its disc: the cull's segment test and the tile reach grow by the
        light's disc radius (mvLightRadius).
2026-11-10 (the owner, in-world, after (d)): hard dark TRIANGLES on a smooth-shaded low-poly skirt under several
point lights, inside lit areas:
  (f) the mover rays met the receiver's OWN neighbouring facets wherever the flat facet's plane runs under them --
      concave folds / pleats at any light angle, every facet outside the terminator band (no lift), and inside it
      the lift is outward only -- and the REST ratio sum w Vs Vm / sum w Vs turned those triangles dark (Vs never
      sees a mover). Now the mover rays pass over the receiver's own mesh within its ONE-RING (clean_vis_walk.glsl
      RT_WALK_SELF_NEAR: rtWalkNearSelf, the walk NoOpaque while armed), the reach 2 x the receiver triangle's
      longest edge + the probe window (clean_rt_shadow_mask.comp mvSelfProbe, armed by mvCastMovers, cleared by
      the other caster sets). Guarded: the wiring, and a model -- a pleated low-poly skirt under random point lights:
      its own-mesh blocks of rays the smooth shading lights exist (the artifact), and all lie within that reach.
2026-11-10, later (the owner, with (f) in): still flat, straight-edged light / dark patches and contour bands over
a smooth body, splotches on the ground:
  (g) the RATIO's inputs were facet-constant and badly scaled: mvLightWeight weighed each light (and gated it as
      turned) by the depth-plane normal outside the terminator band -- one value per flat facet, so the ratio
      stepped at every facet edge wherever any light was mover-blocked -- and the two sums were normalised by every
      light's UNSHADOWED w: indoors (most lights behind walls) N and D were a few percent of full scale in 8-bit raw
      slots (contour bands), beside the 1 / 1 of texels with no mover (the lit texels dominated the filters' and
      the upsample's ratio of means along the cull's tile edges). Now the smooth normal (the shared per-texel
      triangle) weighs every light at every orientation, and the sums are normalised by the STATIC light (hard
      lights w Vs; a soft projector's w, its Vs being a 4-ray estimate): point lights only, D = 1 and N = the ratio.
2026-11-10, later still (the owner): the avatar shadow is a perfect HARD, opaque shadow beside the soft static
one (they never blend), and saw-toothed facet streaks remain over the body:
  (h) every mover ray was one hard ray at the light's centre, and the mover REST slots carried no penumbra radius
      (-1: the spatial filter never touched them), and the filter's depth-SLOPE stop (one per flat facet) split
      any blur at every facet edge. Now the mover (and static weight) rays sample each light as pass 15 does --
      a point light's RenderVKReSTIRPointRadius disc facing the receiver, a record-less projector's
      RenderVKRTShadowSoftness disc, pass 15's near clip and fixture limit (host: history_time.w bits,
      clean_frame_info.glsl moverLightBits), 4 stratified rays -- the REST slots carry the widest penumbra radius of
      their lights, and the spatial filter blurs the mover REST on the plane-predicted depth only. The normaliser
      is the light of the lights any static disc ray reaches.
  (i) (h) first traced 4 disc rays per light everywhere: c_rtmask 11 -> 23 ms, FPS halved. Now ONE ray per light and
      pixel (mover and static weight, the same disc point: stratified per pixel, rotated every frame) and the mover
      temporal + spatial filters integrate the disc into the penumbra, as pass 15's denoiser does; the log's ray
      count is the rays traced (g_mv_ray_n), no longer the calls. Extra rays only where they matter: the static
      weight takes 4 stratified rays only where the light's mover ray was BLOCKED (inside a mover shadow; where it
      is clear the light adds w Vs to N and D alike and Vs's noise cancels) -- the ratio replay below is that path
      (a 1-ray static weight there spikes ~15% of frames without the spatial footprint). (g)'s static-light
      normaliser is gone: with stochastic rays it is not frame-stable; restW is every light's w, but a light the
      receiver's visible facet turns away from.
  (j) (the owner, fa4c4ba4e1: blurred but POLYGONAL patches over a smooth body) the one-ring reach came from the
      receiver TRIANGLE's longest edge: on an irregular tessellation it jumped at every triangle boundary, so an
      own-mesh hit between two triangles' reaches was passed over from one and blocked from the other -- the
      visibility changed exactly along the triangle edges. The reach is now one value per face: 2 x its mean edge
      sqrt(2 A / n) (A its world box's area, n its triangles) + the probe window; the model above uses it.
  (l) (the owner: the mover path cost ~8 ms with 8-10 avatars, and it must stay DETERMINISTIC -- a stochastic light
      pick was rejected: it brings back the smear-vs-noise trade-off the movers left the stochastic path for) every
      eligible light of a mover texel took a static-weight ray: most of the ~14 rays a texel. Now a static ray only
      where the light's mover ray was blocked (elsewhere the light adds w Vs to N and D alike: taken as 1, no ray --
      a statically blocked light then only weakens the other lights' mover shadow, never darkens), mover rays only
      for the 4 strongest lights with a mover on their way (a fixed set while the scene holds), and the point-light
      disc sampled as the static (non-stochastic) RT shadows sample theirs: the shared ADAPTIVE early-out
      (clean_shadow_rt.glsl rtAdaptiveDone: the outer pair first; agreeing = settled, exact and noise-free; the whole
      disc only in penumbrae), the same TAA-phase Vogel pattern, the same mask temporal + spatial passes.
  (m) (the owner: with (l) the avatar shadow was much lighter than with the mover mask off) (l) took the static
      visibility of every light whose mover ray was clear as 1: a texel lit by A only, B behind a wall, the avatar
      blocking A read wB / (wA + wB) instead of 0. Now the radiance packing traces NO static ray while none of the
      texel's mover rays is blocked (N = D: the ratio is exactly 1) and EVERY light's where one is -- exact, paid
      only under a mover shadow.
  (n) DEVICE_LOST on RenderVKShadowMoverMask 0 -> 1 (2026-11-11, a freed 1 MB 'rt.buf'): the mask's binding 23 was
      cached on the VkBuffer value; with the setting off on merged frames the mask did not run, the binding kept the
      retired buffer, and the re-created one came back with the same handle. Keyed on the buffer's generation now.
  (o) (the owner: a worn animesh cast dark, straight-edged boxes, gone with RT sun off or the mover mask off) on a
      merged frame the mask's sun is the MOVERS alone, and the RT sun shadow cache (#771) reused tiles' history class
      outside the host-projected footprints of the casters it knows to move -- a mover with no footprint left its old
      shadow behind as stale blocked tiles. The cache is off on mover-sun frames (host) and the trace never reuses a
      mover sun (shader); the mover cull already skips every texel whose sun ray passes no mover.
  (p) (the boxes again, the #771 cache ruled out) a subgroup tile with more than MV_CAND candidate movers (a worn
      animesh's separately moving prims) answered "a mover is on the way" for EVERY light, so the radiance packing's
      top 4 in that tile were just the 4 strongest lights -- a mover-blocked weaker light lost its shadow there and
      kept it in the neighbouring tiles: tile-shaped patches. The overflowing tile now tests every mover per texel.
  (q) (the boxes are BRIGHTER: light leaking through the mover shadow) the radiance packing wrote a texel no mover
      blocks as 1 / 1 and a blocked one at the two-sum scale (D = sum w Vs / sum w: a few percent indoors) -- in the
      spatial blur and the lighting's 4-tap upsample (ratios of means) the 1 / 1 neighbours won. With deterministic
      static rays the two-sum form (for noisy static samples) is not needed: every texel writes its ratio, D = 1.
  (u) (view-dependent patches on a glossy floor; gone in ratio mode) one diffuse-weighted ratio multiplied the
      stochastic SPECULAR radiance too, whose share per light follows the view. The packing now also writes two
      specular ratios (the same Vs / Vm, weighed by each light's GGX lobe toward the camera at two roughnesses, in the
      two free slots), the 4 mover lights are ranked by diffuse OR lobe share, and the lighting multiplies its
      specular radiance by the ratio at the surface's roughness (rtMoverSpec).
  (v) (avatar-shaped glints / a faint mover shadow inside a projector's static shadow, radiance mode only) the mask
      counted EVERY point light and record-less projector as a stochastic light; pass 15 carries only its light set
      (the ReSTIR header's rs_set). A light outside it -- shaded by the lit pass on its own, not in the radiance --
      weighed into the fold's pooled ratio, so a mover blocking it darkened (and through the specular ratio, re-lit
      the edges of) the radiance of the other lights. The host copies rs_set into LightInfo and mvLightEligible tests
      it. (RenderVKLitDebugMask 16384: the lighting's mover factors off -- an A/B for what the fold changes.)
  (w) (bright glints in a worn animesh's shape inside the avatar's shadow, radiance mode only) the fold's slots
      (REST / DEN / SPEC) were blurred with NO surface stop (h removed the facet-slope one), so an object resting on
      the floor -- the animesh's base -- carried its own ratio into the floor's around it; ratio mode's own slots kept
      the sharp stop. The fold's slots now take the relative slope test (the temporal pass's slopeMatch): facets of one
      surface pass, another surface does not. RenderVKLitDebugMask 32768 shows the fold at the combine: R the diffuse
      ratio, G the specular one, B the share of upsample taps carrying a REST pair.
  (x) (straight-edged self-shadow wedges over the own avatar's body up close) the own-mesh skip's reach was per FACE
      (legacy rows: the face's mean edge; cluster-traced rows: the probed triangle's own edge), so it stepped at face
      and triangle boundaries. The instance pass writes one value per skin (MV_SKIN: the largest per-face mean edge of
      the skinned mesh, legacy and cluster-traced faces alike) and mvSelfProbe takes it for a rigged receiver. And a
      self-shadowing avatar's cluster RT cut is the raster's cut always: rigTolMul gives the near band's multiplier
      (RenderVKClusterRTRigTolNear, persisted) only to a tagged (no-self-shadow) avatar.
  (y) (RenderVKMoverProbePixel, the glint: the fold read 1 inside the avatar's shadow) a light's STATIC ray through
      the agent's own alpha-blend layers (flags 0x2003: rigged | blend | own -- blend class, passed over per candidate
      by the mover filter) still counted each one toward the walk's 64-candidate budget, and the over-budget rule
      confirmed the 65th as a solid blocker (a different face every frame): Vs 0 zeroed that light in both sums.
      Passed-over candidates now count on their own (the NoOpaque bound) and past it end the walk unblocked.
  (z) (RenderVKMoverProbePixel, the wedge: the mover ray blocked at 0.30 m by the receiver's own skin, past its
      0.25 m reach) at the body's terminator the ray leaves the facet into the closed body and meets its far wall from
      inside; a face of the receiver's own skinned mesh met from behind (the raster's back-face rule) is passed over.
  (aa) (the owner: alpha-ing the arms UNDER a suit changed the shadows on the suit's outside) a mover ray from the
      suit that started inside its shell met the body underneath -- another skin of the agent's avatar, on its
      front faces -- before leaving the suit. One short query before the walk finds the receiver skin's nearest face
      beyond the one-ring; met from behind, the ray started inside and every own-avatar candidate nearer than that
      exit is passed over. Met from the front (the ray started outside): nothing is. Inclusive by RT_SHELL_EPS: a
      coincident layer AT the exit lies on the shell.
  (ab) (the owner: a body built from coincident copies of one mesh, all but the chosen size set 100% transparent;
      popping at a joint) the self probe took the nearest candidate -- coincident copies tie -- and alternated
      between the visible copy and a hidden one; the own-mesh rules then skipped the wrong skin and the visible
      copy blocked. A blend face of face alpha <= 1/255 (rtFaceHidden; the raster discards it) is never the
      receiver nor a shell layer, and the rigged plan leaves it out of the TLAS (rigRowHidden, in the row signature).
  (ac) (RenderVKShadowMoverMask 1 vs 0 -- pass 15 the reference: the inner sides of the legs lit, leg-on-leg shadow
      lost) the own-mesh skip passed over every candidate of the receiver's mesh within the reach (~0.25 m on a body),
      the other leg among them. It now passes over only the triangles sharing a vertex with the receiver's triangle
      (rtWalkOneRing: world positions within RT_NEAR_VERT_EPS); the reach is that test's bound. The shell query skips
      the same one-ring, not everything within the reach.
  (ad) (2026-11-12, the owner: with (ac) the insides of the legs still unshadowed and the multi-copy body hard-edged,
      where the path that shadows avatars correctly -- the non-stochastic RT shadow rays -- has none of these rules)
      THE MOVER RAYS ARE THE MASK'S OWN RT SHADOW RAYS: the routing changes WHICH casters a ray sees, never HOW it is
      set up. (f), (j), (x)'s reach, (z), (aa) and (ac) are DELETED -- the receiver probe, the per-skin reach, the
      one-ring skip, own-mesh exits, the outfit shell; every mover ray goes through clean_shadow_rt.glsl
      projShadowRayCore (a point light as that core's light: apex = centre, disc facing the receiver, near plane at its
      prim's bound, own-fixture re-trace in shadowDiscRay), with the terminator lift / post, back-face self-shadow and
      adaptive disc that path has. The check asserts the deleted rules stay gone. Kept: (y), (ab) (the fold: see (an)).
  (ae) (the owner's A/B with (ad): vertical brush-stroke streaks and a ragged band in a wide, soft penumbra on a trouser
      leg) the fold's slots were blurred under a depth-SLOPE stop -- anisotropic on a curved receiver, the blur ran
      along the leg only -- and a luminance stop that kept the deterministic dither's steps apart. They now take a
      plane-predicted depth test only (4 x the tolerance: a leg's curvature over the kernel), and the mover rays' disc
      pattern is fixed per pixel (the frame's R2 rotation off: the short mover history showed it as shimmer). (w)'s
      relative-slope stop is gone with it: the animesh glints it was meant for were (y), the candidate budget.
  (af) (the owner's A/B with (ae): a thigh above the other leg's shadow lighter than pass 15, an extra dark lobe) the
      fold's per-light weights were not pass 15's diffuse terms: a projector took no N.l, a point light 0.25 + N.l, so
      lights grazing or behind the surface (no diffuse in pass 15) diluted or darkened the ratio. w = light x
      max(N.l, 0) now, for both kinds (projDirectTerms' share).
  (ag) (the owner, with (ae): a blocky, layered FALSE shadow spreading around a correct one) the mover slots' history
      is almost always young, and the young rule gave a texel the widest penumbra radius of its 3 x 3: a LIT texel
      beside a mover shadow took that shadow's radius and, under (ae)'s depth-only weights, averaged the shadow in --
      a halo one blur radius wide, in a-trous steps. A fold-slot texel keeps its own radius (none: at most 1 texel).
      And the mover rays' fixed disc pattern is the IGN plus a per-pixel hash (mvPixelNoise): the frozen IGN's
      structure put the penumbra's few levels on neighbouring texels alike (stepped bands).
  (al) (2026-11-12, the owner's mover probe at a hair / skin contact, one lamp) the raw fold value was 0 -- both mover
      rays blocked by the hair, a firm umbra -- but the spatial pass lifted it to 0.31: the radius was the 12-texel cap
      (the penumbra estimate R t / (D - t) from a blocker 0.8 m up the ray) and the lit surface above weighed ~1 (the
      fold's depth test never under 8 cm). A settled fold texel (REST 0 or 1) now blurs at most 1 texel, every fold slot
      alike, and the fold's depth scale is the receiver's footprint + a limb's curvature (capped at the old scale), with
      a soft normal stop only between folded surfaces.
  (am) (the owner on (al): a thin lit line, 1-2 texels, along hair lying on skin) a settled fold texel is not blurred
      at all (the 1-texel blur took the lit hair texel beside it in), and the mover reads' upsample measures its taps
      against the receiver's footprint instead of the 3 cm floor whenever a tap is this surface by it.
  (an) (2026-11-12, the owner, a third time: REUSE the proven RT shadow path -- "the same denoiser we're using there"
      -- after (ae)..(am) left avatar shadows noisy, ragged and dithered) the mover lights are now the RT projector
      loop's lights end to end: the same ray core with the frame's own disc rotation (the frozen per-pixel pattern
      mvPixelNoise is gone), an own slot's hint from that light's history (the fold hint is gone), the same slots
      (own slots + ONE REST ratio slot; the two-sum N / D slot PROJ_MASK_MOVER_DEN_ID is gone), the same temporal
      pass (its moving-caster rules via rt_block_dyn), the PROVEN spatial pass (every mover-only branch -- mvRest,
      mvSurfTol, mvSoftNormalW, mvSettled, the 3 x 3 radius cap -- is gone: atrousProj is the projector slots'
      filter again) and the PROVEN reader (rtMaskTapsMv's footprint tolerance is gone; rtMover* call
      rtMaskProjTapsS on rtMaskTapsEx's taps). The radiance packing writes the REST ratio
      sum w Vs Vm / sum w Vs (+ the two specular ratios) from the per-frame per-light visibilities, as the projector
      loop's REST, and traces EVERY light with a mover on its way (no top-4 cut: f3eb75c757's contact pocket).
      Superseded and no longer asserted: (e)'s two-sum REST and its replay, (i)'s one-ray rule, (l)'s top 4 and fixed
      disc point, (m)'s phase-2 shape, (q), (ae), (ag), (ah), (aj), (al), (am). Kept: (ai)'s adaptive static weight,
      (m)'s rule (no static ray while no mover ray is blocked; every REST light's where one is), (u), (v), (af).
  (ao) (2026-11-13, the owner: a lit band around an avatar's shadow on a striped strap, a projector on the left and a
      point light on the right; RenderVKShadowMoverMask 0 correct) the radiance packing folded the lights IN THE MASK,
      per mask texel, by mvLightWeight's proxy weights -- the texel's smooth normal (no normal map), no projector
      ambiance, the specular by a two-roughness GGX lobe -- while pass 15's split between the lights at the pixel
      follows the normal-mapped normal and the full BRDF; and the spatial pass filtered that cross-light ratio as one
      signal over texels whose light mix differs. One light: the weights cancel (R = Vm). Two from opposite sides: the
      facets / stripes facing the blocked light kept the other light's share -- a lit ring around the both-blocked
      core. THE PIXEL FOLD: with <= 4 lights at the texel (pixFold) the radiance packing writes the ratio mode's OWN
      slots (per-light Vm, the proven per-light filter) + a static-weight slot per light in a static shadow
      (MV_VS_TAG | light + 1; Vs ~0: the light gives up its Vm slot), and the lighting folds them at the pixel with
      pass 15's own per-light terms (deferMoverPixelFold: projDirectTerms, R = sum t Vs Vm / sum t Vs, mvPixelRatio).
      More lights: the REST fold as before, multiplied on top. Asserted: the slot policy, the per-id read without the
      REST fallback, the fold's terms, and mvPixelRatio's algebra (one light = its Vm; a wall-blocked light never
      dilutes; two lights by their pixel terms).
  (ap) (2026-11-13, the owner: mover shadows cost ~25 ms -- 6.8 rays per mover texel, 142 lights in range through the
      walls of a house with two lights of its own; must stay DETERMINISTIC, no random light pick) three cuts, each
      guarded: the per-texel cull tests each avatar's world BOX (the same joints + pad and attachment spheres as its
      sphere) as well as the sphere -- the replay above shows no blocking caster is lost; pass 3 (the REST
      denominator's static rays) skips (i) every remaining light once every blocked light is statically dark (Vs
      exactly 0: the avatar only "shadows" lights a wall already blocks) and the denominator is past mvRestRatio's
      fallback -- the ratio is then exactly 1, the replay shows it bit-exact -- and (ii) lights within MV_CUT_F of the
      static light already traced, per channel, their Vs taken as 1/2 -- the replay shows every ratio within
      (1 - R) x (f / 2) / (1 - f / 2) of the full pass. No random number, frame index or stats-frame flag may enter the
      cull, the light walk or these cuts; the stats frame (frame info moverBits bit 4: the VKMoverShadow "mover cost"
      lines, header words 10-23 and the per-light block) only counts.
  (aq) (2026-11-13, the owner: the striped strap's lit halo STILL there after (ao) and (ap)) the pass-1 gate was the
      light's WEIGHT on the mask texel's smooth normal (w = light x max(N.l, 0) > 0.002). Where that normal grazes or
      just turns from a light while the normal map's stripes still face it, pass 15 shades the light (projDirectTerms:
      the mapped normal, N.l > 0) but the mask dropped it -- no slot, no mover ray -- and the lighting reads a light no
      tap lists as unshadowed (the pixel fold: factor 1 for it; the REST: weight 0): a lit, striped band along the
      shadow wherever the blocked light grazes the receiver. Now the gate is the light's reach (its base light x
      attenuation, the frustum) and the facing gate (turned, the normal-map slack); w (maybe 0) only weighs the REST,
      and the pixel fold weighs every listed light by pass 15's own term. The probe logs the gated-out lights (phase 4,
      their smooth N.l) and every light event names the light's kind (word 8: mvLightInfo); the cost log names each
      light's object (local id, worn by the agent / another avatar) and how many of its static traces saw light.
  (ar) (2026-11-13, the owner: a HARD SEAM with a LIT SLIVER between two overlapping avatar shadows, scalloped edges,
      >= 7 lights in reach; RenderVKShadowMoverMask 0 correct) (ao)'s pixel fold only took texels with <= 4 ELIGIBLE
      lights; every other texel folded all its lights into one REST ratio by mvLightWeight's proxy weights (no normal
      map, no ambiance, a GGX proxy for the specular) -- the blocked lights' share against the free ones was the
      proxy's, not pass 15's (the sliver along an overlap) -- and the <= 4 / 5+ switch, a light's reach / frustum /
      facing edge crossing the receiver, flipped a texel between the two estimates (the seam; per facet on a curved
      receiver). THE EXACT PIXEL FOLD at every light count: explicit Vm / Vs slots for the HIT lights (a mover in front:
      the cull, never this frame's Vm -- a slot that came and went with the penumbra noise would never build a history)
      by LIGHT INDEX (the same set whatever the cluster walk's order), Vs-only slots for other lights in a static
      shadow, the U shares N / D for the rest, and where nothing folds the hit lights' Vm 1 slots all the same; the lighting walks pass 15's light set at the pixel (the mask's mvLightEligible) and
      adds every light by its own projDirectTerms. Asserted: the slots, the order key, the pixel walk, the same
      eligibility bodies, no REST / blend in the fold; replayed: the plan never overfills the 4 slots, the explicit set
      is walk-order invariant, the fold is EXACT wherever no N share is needed and the U set is statically alike (any
      light count), and past the slots it is closer to pass 15 than the old REST.
"""
import math
import os


import random
import re
import shutil
import subprocess
import tempfile

NAME = "mover-shadow-routing"
STAGE = "source"
DESCRIPTION = "under the stochastic path, movers' shadows come from the RT shadow mask: stochastic rays see static " \
              "casters only, mover rays see movers only, the mover cull never drops a blocker"
GUARDS = ("The moving-caster smear / trailing / speckle of the stochastic histories (idle hair self-shadow, a walking "
          "avatar's shadow): RenderVKShadowMoverMask routes movers to the RT shadow mask; this guards the routing.")

SH = "indra/vkclean/shaders/"


def _function(src, name):
    m = re.search(r"(?:bool|float|uint|void|vec3|vec4)\s+" + name + r"\([^)]*\)\s*\{", src)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[m.start():i + 1]
        i += 1
    return None


def _defines(src):
    out = {}
    for m in re.finditer(r"^#define\s+(RT_MASK_\w+)\s+(.+?)(?://.*)?$", src, re.M):
        out[m.group(1)] = m.group(2).strip()
    return out


def _eval_mask(expr, defs, depth=0):
    if depth > 8:
        raise ValueError("define recursion: " + expr)
    e = re.sub(r"(\d+|0x[0-9a-fA-F]+)u\b", r"\1", expr)
    for name in sorted(defs, key=len, reverse=True):
        if re.search(r"\b" + name + r"\b", e):
            e = re.sub(r"\b" + name + r"\b", "(" + str(_eval_mask(defs[name], defs, depth + 1)) + ")", e)
    if not re.fullmatch(r"[\s0-9a-fA-Fx()|&~]+", e):
        raise ValueError("not a mask expression: " + expr)
    return eval(e) & 0xFF   # (digits, hex, | & ~ and parentheses only: checked above)


def _wiring(read):
    missing = []
    walk = read(SH + "clean_vis_walk.glsl")
    fp = _function(walk, "rtVisWalkFP") or ""
    i_and = fp.find("mask &= RT_WALK_MASK_AND")
    q = [m.start() for m in re.finditer(r"rayQueryInitializeEXT", fp)]
    if i_and < 0 or len(q) < 2 or any(p < i_and for p in q):
        missing.append("clean_vis_walk.glsl rtVisWalkFP: `mask &= RT_WALK_MASK_AND` must precede both ray queries "
                       "(the opaque-first pre-ray and the walk) -- else a walk sees the other pipeline's casters")
    if "rtWalkMoverSkip(id)" not in fp:
        missing.append("clean_vis_walk.glsl rtVisWalkFP: the candidate loop no longer passes blend-class movers over "
                       "(rtWalkMoverSkip)")
    # (y) a passed-over blend caster of the other pipeline never counts toward the candidate budget, and its own
    #     bound ends the walk WITHOUT confirming (the over-budget rule made the 65th own-hair candidate a solid
    #     static blocker: Vs 0, the fold read lit)
    i_skip = fp.find("if (rtWalkMoverSkip(id))")
    i_bud = fp.find("if (++n_cand > budget)")
    if i_skip < 0 or i_bud < 0 or i_skip > i_bud \
            or "if (++n_skip > RT_CANDIDATE_BUDGET_NOOPAQUE) { rayQueryTerminateEXT(rq); break; }" not in fp \
            or fp.count("rtWalkMoverSkip(id)") != 1:
        missing.append("clean_vis_walk.glsl rtVisWalkFP: the mover filter's passed-over candidates count toward the "
                       "candidate budget again (or confirm a blocker past their own bound) -- an own-avatar blend "
                       "layer becomes a static blocker -- (y)")
    # (ad) THE MOVER RAYS ARE THE MASK'S OWN RT SHADOW RAYS (2026-11-12, the owner): the routing changes WHICH casters a
    #      ray sees, never HOW it is set up. The mover-only self-shadow machinery -- the receiver probe (mvSelfProbe), the
    #      per-skin reach (MV_SKIN), the one-ring skip (RT_WALK_SELF_NEAR / rtWalkNearSelf / rtWalkOneRing), own-mesh
    #      exits (rtWalkOwnExit), the outfit shell (rtWalkShellExit / rtWalkInnerLayer) -- stays gone; every mover ray
    #      goes through the RT projector core (projShadowRayCore), a point light as that core's light.
    mask_src = read(SH + "clean_rt_shadow_mask.comp")
    gone = ["mvSelfProbe", "RT_WALK_SELF_NEAR", "rtWalkNearSelf", "rtWalkOneRing", "rtWalkOwnExit", "rtWalkShellExit",
            "rtWalkInnerLayer", "rt_near_", "MV_SKIN", "moverSkinEdge", "g_mv_self"]
    srcs = {"clean_vis_walk.glsl": walk, "clean_rt_shadow_mask.comp": mask_src,
            "clean_rt_instances.comp": read(SH + "clean_rt_instances.comp"),
            "clean_rt_movers.glsl": read(SH + "clean_rt_movers.glsl"),
            "llcleanrtvk.h": read("indra/newview/vkgfx/llcleanrtvk.h")}
    back = sorted({"%s in %s" % (g, n) for n, s in srcs.items() for g in gone
                   if re.search(r"\b" + re.escape(g) + (r"" if g.endswith("_") else r"\b"),
                                re.sub(r"//[^\n]*", "", s))})
    if back:
        missing.append("mover-only self-shadow rules are back (the mover rays must be the mask's own RT shadow rays, "
                       "rule (ad)): " + ", ".join(back))
    lvis0 = _function(mask_src, "mvLightVis") or ""
    if "projShadowRayCore(Lc, ps, P, Ng, cam, time, npx, max_n)" not in lvis0 or "rtVisTraceK(" in lvis0 \
            or "rtVisWalk" in lvis0:
        missing.append("clean_rt_shadow_mask.comp mvLightVis: the mover rays left the RT projector core "
                       "(projShadowRayCore) -- a parallel ray setup again -- (ad)")
    if "rtVisTraceK(origin, dir, tmax," not in (_function(read(SH + "clean_shadow_rt.glsl"), "shadowDiscRay") or ""):
        missing.append("clean_shadow_rt.glsl shadowDiscRay: the disc rays no longer take the own-fixture re-trace "
                       "(a point light's mover rays through the core: its own lamp shade shadows it) -- (ad)")
    # (ab) a face the raster never shows (blend, face alpha <= 1/255: a 100% transparent coincident copy) stays out of
    #      the RT scene: the rigged plan leaves it out of the TLAS (rigRowHidden, in the row signature); rtFaceHidden is
    #      the GPU twin of the rule
    rtc = read(SH + "clean_rt_common.glsl")
    hid = _function(rtc, "rtFaceHidden") or ""
    rtcpp = read("indra/newview/vkgfx/llcleanrtvk.cpp")
    if "a <= (1.0 / 255.0)" not in hid or "base_color_factor.a" not in hid \
            or "if (rigRowHidden(in)) continue;" not in rtcpp or "rigRowHidden(in) ? 0x5A5Au" not in rtcpp \
            or "kInstTintA = 108" not in rtcpp:
        missing.append("a 100% transparent face (rigRowHidden / rtFaceHidden) enters the RT scene again: the hidden "
                       "copies of a body cast and receive -- (ab)")
    skip = _function(walk, "rtWalkMoverSkip") or ""
    if "m == 1u ? mover : !mover" not in skip.replace("  ", " "):
        missing.append("clean_vis_walk.glsl rtWalkMoverSkip: the filter's two senses (1 = pass movers over, 2 = pass "
                       "non-movers over) changed")

    dl = read(SH + "clean_deferred_light.comp")
    m = re.search(r"#if DEFER_PSTOCH\n(?:(?!#endif).)*?#define RT_WALK_MASK_AND ([^\n]+)\n#define RT_WALK_MOVER_FILTER ([^\n]+)",
                  dl, re.S)
    inc = dl.find('#include "clean_draw_common.glsl"')
    if not m or inc < 0 or m.start() > inc:
        missing.append("clean_deferred_light.comp: pass 15's RT_WALK_MASK_AND / RT_WALK_MOVER_FILTER (PDN_OPT_MOVER) "
                       "are not defined under DEFER_PSTOCH before the walk library is included")
        stoch_and = None
    else:
        stoch_and = m.group(1)
        if "~RT_MASK_MOVER" not in stoch_and or "PDN_OPT_MOVER" not in stoch_and or "1u" not in m.group(2):
            missing.append("clean_deferred_light.comp: pass 15's hook no longer clears RT_MASK_MOVER / filters blend "
                           "movers under PDN_OPT_MOVER")
    if not re.search(r"#define PDN_OPT_MOVER\s+128u", dl):
        missing.append("clean_deferred_light.comp: PDN_OPT_MOVER is not 128u (the host sets rtr2.z bit 128)")
    if "* rtMoverSun(P)" not in (_function(dl, "deferMergedSunT") or ""):
        missing.append("clean_deferred_light.comp deferMergedSunT: the merged (static) sun is no longer multiplied by "
                       "the mask's mover sun")
    if "fD = mvPixelRatio(numD, denD);" not in (_function(dl, "deferMoverPixelFold") or "") \
            or "deferMoverPixelFold(" not in (_function(dl, "deferEmitterLight") or ""):
        missing.append("clean_deferred_light.comp deferEmitterLight: the radiance is no longer multiplied by the "
                       "mask's mover ratio")
    if "rtMoverLight(li, P)" not in (_function(dl, "deferPdnVisLight") or ""):
        missing.append("clean_deferred_light.comp deferPdnVisLight: the ratio mode's lights no longer take their "
                       "mover visibility")

    # (the shared cull bodies -- mvSphere / mvCullBuild / mvSegHitAny / mvBoxSeg / mvLightRadius -- live in
    #  clean_rt_movers.glsl since RenderVKShadowMoverMask 2: the mask and pass 15 include ONE body; the rules below
    #  read the mask with them appended)
    mask = read(SH + "clean_rt_shadow_mask.comp") + "\n" + read(SH + "clean_rt_movers.glsl")
    d_and = mask.find("#define RT_WALK_MASK_AND g_mv_and")
    d_inc = mask.find('#include "clean_rt_common.glsl"')
    if d_and < 0 or d_inc < 0 or d_and > d_inc:
        missing.append("clean_rt_shadow_mask.comp: the walk hook (g_mv_and) is not defined before clean_rt_common.glsl")
    main = _function(mask, "main") or ""
    if not re.search(r"if \(mvSun\) \{ mvCastMovers\(\);[^\n]*\n\s*sun = mix\(sunShadowRayCore", main):
        missing.append("clean_rt_shadow_mask.comp: the merged frame's sun trace is no longer mover-only")
    if not re.search(r"mvCastMovers\(\);\s*\n\s*vm = mvLightVis\(", main):
        missing.append("clean_rt_shadow_mask.comp: the stochastic lights' mover rays are gone")
    if not re.search(r"mvCastStatic\(\);\s*\n(?:\s*rt_block_t = 1e30;\s*\n)?(?:\s*if \(g_mpr\)[^\n]*\n)?\s*(?:const vec3 )?vs = mvLightVis\(", main):
        missing.append("clean_rt_shadow_mask.comp: the REST ratio's static weight rays are gone")
    if "mvCullBuild(" not in main or "mvSegHitAny(P, X, mvLightRadius(L))" not in main \
            or "mvSunHit(P, Lsun, sunFar)" not in main:
        missing.append("clean_rt_shadow_mask.comp: the mover cull (mvCullBuild + the per-texel tests) is not wired")
    # (ae) 2026-11-12, the owner: an avatar lost its point-light shadow ENTIRELY past ~5-10 m of camera distance.
    # The mover lights walked the CAPPED light cluster list (CLUSTER_CAP, camera-importance order): a light the cap
    # dropped kept its light and static shadow (pass 15's own set, uncapped tile lists) but was never mover-traced.
    # Every mover light walk (the cull's reach and the three light passes) goes through mvNextLight, which walks every
    # light when the cluster overflowed; no walk of the mover block reads cluster_index directly.
    nxt = _function(mask, "mvNextLight") or ""
    ovf = _function(mask, "mvClusterOverflow") or ""
    if "clusterCandidates(cluster_count[c]) > CLUSTER_CAP" not in ovf or "ovf ? nt" not in nxt \
            or "length(pr.xyz - P) < pr.w" not in nxt:
        missing.append("clean_rt_shadow_mask.comp mvNextLight / mvClusterOverflow: an overflowed light cluster no "
                       "longer walks every light in range (lights past CLUSTER_CAP lose their avatar shadows)")
    mv_beg = main.find("mvOvf = mvClusterOverflow(mvCl);")
    mv_blk = main.find("MOVER ROUTING: the stochastic path's local lights")
    mv_end = main.find("if (g_mpr)\n    {\n        // (RenderVKMoverProbePixel: the texel itself)")
    if mv_beg < 0 or mv_blk < 0 or mv_end < 0 or not (mv_beg < mv_blk < mv_end):
        missing.append("clean_rt_shadow_mask.comp: the mover block's light walks (mvOvf, the local-light block) moved")
    else:
        reach = main[mv_beg:main.find("mvCullBuild(", mv_beg)]
        blk = main[mv_blk:mv_end]
        if "cluster_index[" in reach or "cluster_index[" in blk:
            missing.append("clean_rt_shadow_mask.comp: a mover light walk reads the capped cluster list directly "
                           "(cluster_index) -- lights past CLUSTER_CAP lose their avatar shadows; use mvNextLight")
        if reach.count("mvNextLight(") != 1 or blk.count("mvNextLight(mvCl, mvOvf") + blk.count("mvNextLight(c, mvOvf") != 3:
            missing.append("clean_rt_shadow_mask.comp: the cull's reach and the three mover light passes must each "
                           "walk mvNextLight (one order: the passes' bitsets index its positions)")

    ps = read("indra/newview/vkgfx/llcleanprojstochvk.cpp")
    if "mMoverRouteNow ? 128u" not in ps:
        missing.append("llcleanprojstochvk.cpp: pass 15's PDN_OPT_MOVER bit is not set from mMoverRouteNow")
    if "if (mMoverRouteNow) dp.casterHistoryPolicy = 0u;" not in ps:
        missing.append("llcleanprojstochvk.cpp: the caster-history policies are not bypassed under the routing")
    rv = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if "mRtMaskOn = mask && (!mShadowMergeTry || mMoverWant);" not in rv:
        missing.append("llcleanrenderervk.cpp selectLitVariant: the mask no longer runs on merged frames for the movers")
    if "offsetof(VKSSRInfo, history_time) + 2 * sizeof(float)" not in rv:
        missing.append("llcleanrenderervk.cpp rtMaskFrameInfo: the frame info's mover word is no longer written")

    st = read("indra/newview/app_settings/settings.xml")
    sm = re.search(r"<key>RenderVKShadowMoverMask</key>\s*<map>(.*?)</map>", st, re.S)
    if not sm or not re.search(r"<key>Persist</key>\s*<integer>0</integer>", sm.group(1)) \
            or not re.search(r"<key>Value</key>\s*<integer>0</integer>", sm.group(1)):
        missing.append("settings.xml: RenderVKShadowMoverMask must be nonpersistent and default 0 (2026-11-14: mode 0 ships)")

    mv = read(SH + "clean_rt_movers.glsl")
    h = read("indra/newview/vkgfx/llcleanrtvk.h")
    g = {k: int(v) for k, v in re.findall(r"#define (MV_HDR|MV_AV_CAP|MV_OBJ_CAP)\s+(\d+)u", mv)}
    c = {k: int(v) for k, v in re.findall(r"\b(kMvHdr|kMvAvCap|kMvObjCap) = (\d+)", h)}
    if g.get("MV_HDR") != c.get("kMvHdr") or g.get("MV_AV_CAP") != c.get("kMvAvCap") \
            or g.get("MV_OBJ_CAP") != c.get("kMvObjCap"):
        missing.append("the mover buffer layout's twins differ: clean_rt_movers.glsl %s vs llcleanrtvk.h %s" % (g, c))
    lg = {k: int(v) for k, v in re.findall(r"#define (MV_LST_N|MV_LST_W)\s+(\d+)u", mv)}
    lc = {k: int(v) for k, v in re.findall(r"\b(kMvLstN|kMvLstW) = (\d+)", h)}
    if not re.search(r"#define MV_ZERO_WORDS MV_OBJ_SPH", mv) or \
            not re.search(r"#define MV_LST\s+MV_HDR\b", mv) or \
            not re.search(r"#define MV_AV_LIST\s+\(MV_LST \+ MV_LST_N \* MV_LST_W\)", mv) or \
            not re.search(r"#define MV_AV_BOX\s+\(MV_AV_SPH \+ 4u \* MV_AV_CAP\)", mv) or \
            not re.search(r"#define MV_AV_CLN\s+\(MV_AV_BOX \+ 6u \* MV_AV_CAP\)", mv) or \
            not re.search(r"#define MV_HIT\s+\(MV_AV_CLN \+ MV_AV_CAP\)", mv) or \
            not re.search(r"#define MV_OBJ_SPH\s+\(MV_HIT \+ MV_AV_CAP \+ MV_OBJ_CAP\)", mv) or \
            lg.get("MV_LST_N") != lc.get("kMvLstN") or lg.get("MV_LST_W") != lc.get("kMvLstW") or \
            "kMvZeroWords = kMvHdr + kMvLstN * kMvLstW + kMvAvCap + kMvAvCap + 4 * kMvAvCap\n" \
            "                                           + 6 * kMvAvCap + kMvAvCap + kMvAvCap + kMvObjCap;" not in h or \
            "kMvRbWords = kMvHdr + kMvLstN * kMvLstW;" not in h:
        missing.append("the mover buffer's per-frame part (MV_ZERO_WORDS / kMvZeroWords: header, per-light stats, avatar "
                       "lists, spheres, attachment boxes, cluster counts, per-mover hits) or its readback (kMvRbWords) "
                       "changed on one side")
    # the carried part past the object spheres: the joint clusters, pass 15's demand histogram, then the motion hold
    kc = {k: int(v) for k, v in re.findall(r"\b(kMvClN|kMvClW|kMvXBins) = (\d+)", h)}
    gc = {k: int(v) for k, v in re.findall(r"#define (MV_CL_N|MV_CL_W|MV_X_BINS)\s+(\d+)u", mv)}
    if kc.get("kMvClN") != gc.get("MV_CL_N") or kc.get("kMvClW") != gc.get("MV_CL_W") \
            or kc.get("kMvXBins") != gc.get("MV_X_BINS") \
            or not re.search(r"#define MV_AV_CL\s+\(MV_OBJ_SPH \+ 4u \* MV_OBJ_CAP\)", mv) \
            or not re.search(r"#define MV_X_HIST\s+\(MV_AV_CL \+ MV_CL_N \* MV_CL_W \* MV_AV_CAP\)", mv) \
            or not re.search(r"#define MV_HOLD\s+\(MV_X_HIST \+ 2u \* MV_X_BINS\)", mv) \
            or "kMvHold = kMvZeroWords + 4 * kMvObjCap + kMvClN * kMvClW * kMvAvCap\n" \
               "                                      + 2 * kMvXBins;" not in h:
        missing.append("the mover buffer's carried part (MV_AV_CL / MV_X_HIST / MV_HOLD vs kMvHold) changed on one side")
    # the mover probe (RenderVKMoverProbePixel): its guard-buffer region's twins, right after the candidate diagnostic
    pr = read(SH + "clean_rt_mover_probe.glsl")
    # (+ RenderVKShadowMoverMask 2: pass 15's mover-exact record RTP_X / kRtpX, 512 words, after the spatial region)
    pg = {k: int(v) for k, v in re.findall(r"#define (RTP_BASE|RTP_HDR|RTP_MASK_N|RTP_SP_N|RTP_X_N|RTP_X_L|RTP_X_LW|RTP_X_LIGHTS)\s+(\d+)u", pr)}
    if pg.get("RTP_BASE") != 64 + 4 + 512 + 8192 or pg.get("RTP_HDR") != 8 or pg.get("RTP_MASK_N") != 2560 \
            or pg.get("RTP_SP_N") != 256 or "kRtpHdr = 8, kRtpMaskN = 2560, kRtpSpN = 256" not in h \
            or pg.get("RTP_X_N") != 512 or pg.get("RTP_X_L") != 16 or pg.get("RTP_X_LW") != 6 \
            or pg.get("RTP_X_LIGHTS") != 64 or "kRtpXN = 512, kRtpXL = 16, kRtpXLW = 6, kRtpXLights = 64" not in h \
            or "#define RTP_X         (RTP_SP + RTP_SP_N)" not in pr or "#define RTP_END       (RTP_X + RTP_X_N)" not in pr \
            or "kRtpX = kRtpSp + kRtpSpN" not in h or "kRtpEnd = kRtpX + kRtpXN" not in h \
            or "kGuardWords = 64 + 4 + 512 + 8192 + 3336" not in h:
        missing.append("clean_rt_mover_probe.glsl RTP_* and llcleanrtvk.h kRtp* / kGuardWords differ (the mover probe's "
                       "guard-buffer region)")
    kb = re.search(r"kCompBindings = (\d+)", h)
    inst = read(SH + "clean_rt_instances.comp")
    # (x) a self-shadowing avatar traces the raster's cluster cut
    if "if (!rigNoSelf(a)) return 1.0;" not in (_function(read(SH + "clean_cluster_rt.comp"), "rigTolMul") or ""):
        missing.append("clean_cluster_rt.comp rigTolMul: a self-shadowing avatar may trace a coarser cluster cut than "
                       "the raster's (its own coarser facets shadow it) -- (x)")
    if not kb or not re.search(r"binding = %d\) coherent buffer MoverBuf" % (int(kb.group(1)) - 1), inst):
        missing.append("clean_rt_instances.comp: the mover buffer is not the compute set's last binding (kCompBindings - 1)")
    # (d) the shared terminator / origin rules for every light kind
    srt = read(SH + "clean_shadow_rt.glsl")
    core = _function(srt, "projShadowRayCoreImpl") or ""
    if "rtPosOriginSetup(" not in core or "rtPosOriginAt(" not in core:
        missing.append("clean_shadow_rt.glsl projShadowRayCoreImpl: no longer the shared rtPosOriginSetup / rtPosOriginAt")
    lvis = _function(mask, "mvLightVis") or ""
    if "rtPosOriginSetup(" not in lvis or "projShadowRayCore(" not in lvis:
        missing.append("clean_rt_shadow_mask.comp mvLightVis: the point-light rays left the shared origin rules "
                       "(faceted self-shadow on smooth limbs)")
    lwf = _function(mask, "mvLightWeight") or ""
    if "rtTerminatorCached(" not in lwf or re.search(r"pterm\s*=\s*L\.proj_origin\.w", lwf):
        missing.append("clean_rt_shadow_mask.comp mvLightWeight: the terminator probe must run for every light kind")
    if not re.search(r"rt_term_on = pterm;[^\n]*\n(?:[^\n]*\n){0,20}?[^\n]*vm = mvLightVis\(", main):
        missing.append("clean_rt_shadow_mask.comp: the mover rays no longer take the light's terminator lift (rt_term_*)")
    # (e) lit holes
    fbx = _function(walk, "rtFixtureBoxAB") or ""
    if "skin_id != 0xFFFFFFFFu) return false;" not in fbx:
        missing.append("clean_vis_walk.glsl rtFixtureBoxAB: a skinned caster can be taken for a lamp's fixture again "
                       "(its mesh box tested in its group record's space: limbs passed over, lit holes)")
    lwf2 = _function(mask, "mvLightWeight") or ""
    # (af) the fold's weight of a light is its diffuse term in pass 15 (projDirectTerms: light x attenuation x N.l, N.l > 0
    #      only) -- for projectors and point lights alike
    if "w = g_mv_base * max(nl, 0.0);" not in lwf2 or "0.25 + max(nl" in lwf2 \
            or "if (nl_raw <= 0.0) return false;" not in read(SH + "clean_draw_common.glsl"):
        missing.append("clean_rt_shadow_mask.comp mvLightWeight: the fold weighs a light other than by its pass-15 diffuse "
                       "term (light x N.l, nothing behind the surface): lighter mover shadows and stray dark lobes -- (af)")
    # (aq) the GATE is the light, not its weight: a light the smooth normal grazes / just turns from (w = 0) while the
    #      normal-mapped pixel still faces it is lit by pass 15 (projDirectTerms on the mapped normal) -- dropped from
    #      the mask it had no slot and no ray, and the lighting read it as unshadowed (the striped halo)
    mainsrc = _function(mask, "main") or ""
    if "return max(max(g_mv_base.r, g_mv_base.g), g_mv_base.b) > 0.002;" not in lwf2 \
            or "return max(max(w.r, w.g), w.b) > 0.002;" in lwf2 \
            or "const bool reach = mvLightWeight(L, P, Ng, cam, rp, w, X, turned, pterm, plift, pns, png);" not in mainsrc \
            or "if (!reach || turned)" not in mainsrc \
            or "if (g_mpr) rtpLight(4u, (turned ? 1u : 0u) | (reach ? 0u : 2u), li, w, vec3(g_mv_nl), 1e30, 0u, -1.0);" not in mainsrc:
        missing.append("clean_rt_shadow_mask.comp mvLightWeight / pass 1: a light is gated by its smooth-normal WEIGHT again "
                       "(w > 0.002) instead of its reach + the facing gate -- a light the stripes of a normal-mapped "
                       "receiver face gets no slot, no ray and reads unshadowed: the lit striped halo -- (aq)")
    if "const float nl = tri ? dot(tns, l) : dot(Ng, l);" not in lwf2 \
            or "pterm = tri && rtTerminatorBand(" not in lwf2:
        missing.append("clean_rt_shadow_mask.comp mvLightWeight: the weight / facing gate must take the smooth normal "
                       "at every orientation (facet-constant weights step the ratio at facet edges) -- (g)")
    # (h) the mover rays sample the light as pass 15 does, and their shadow is filtered like a penumbra
    mlr = _function(mask, "mvLightRadius") or ""
    if "return moverPointRadius();" not in mlr or "return moverProjRadius();" not in mlr:
        missing.append("clean_rt_shadow_mask.comp mvLightRadius: point lights / record-less projectors are no longer "
                       "the discs pass 15 samples (a hard mover shadow beside a soft static one) -- (h)")
    if "rt_fix_max = moverFixtureMax();" not in lvis or "ps.texel = vec4(moverPointRadius()," not in lvis \
            or "Lc.proj_axis = vec4(A, 0.0);" not in lvis:
        missing.append("clean_rt_shadow_mask.comp mvLightVis: the point light is no longer pass 15's disc (Vogel rays, "
                       "its fixture limit) -- (h)")
    if "slotV[k] = vec4(rv, rpr);" not in main or "slotV[k] = vec4(vm, prad);" not in main:
        missing.append("clean_rt_shadow_mask.comp: the mover slots carry no penumbra radius (never blurred) -- (h)")
    fi = read(SH + "clean_frame_info.glsl")
    if "floatBitsToUint(ssr.history_time.w)" not in (_function(fi, "moverLightBits") or ""):
        missing.append("clean_frame_info.glsl moverLightBits: history_time.w is no longer the light-size bits -- (h)")
    if "| (q(projR, 1000.f, 4095u) << 11)" not in rv or "sizeof(zw), zw);" not in rv:
        missing.append("llcleanrenderervk.cpp rtMaskFrameInfo: the mover light-size bits (history_time.w) are not "
                       "written -- (h)")
    # (n) the mask's binding of the mover buffer is keyed on its GENERATION (a re-created buffer can come back with the
    #     same VkBuffer value: DEVICE_LOST on RenderVKShadowMoverMask 0 -> 1)
    rtc = read("indra/newview/vkgfx/llcleanrtvk.cpp")
    if "mvg != mRtMaskMoverGen" not in rv or "mRtMaskMoverGen = mvg;" not in rv or rtc.count("++mMoverGen") < 3:
        missing.append("the RT shadow mask's mover-buffer binding is cached on the VkBuffer value again (a re-created "
                       "buffer with the same handle leaves binding 23 on freed memory: DEVICE_LOST) -- (n)")
    # (p) a subgroup tile with more candidate movers than MV_CAND tests every mover per texel, not "yes for every light"
    sha = _function(mask, "mvSegHitFirst") or ""
    if "if (mv[2] != 0u) return MV_SEG_LIST;" not in sha or "for (uint m = 0u; m < n; ++m)" not in sha \
            or "return mvSegHitFirst(P, X, rl) != MV_SEG_NONE;" not in (_function(mask, "mvSegHitAny") or ""):
        missing.append("clean_rt_shadow_mask.comp mvSegHitAny: a tile with more candidate movers than MV_CAND answers "
                       "yes for every light again (the top 4 change per tile: tile-shaped mover-shadow patches) -- (p)")
    # (r) the mask's STATIC-WEIGHT rays see exactly pass 15's casters: the same AND mask (every class but the mover
    #     classes) and the same blend filter sense (1: pass over blend movers). A caster in one set and not the other
    #     drops a light from the ratio (Vs = 0) while the static radiance still holds it -- a bright, caster-shaped
    #     hole in a mover's shadow.
    if stoch_and is not None:
        on = re.search(r"\?\s*\(([^?:]+)\)\s*:", stoch_and)
        cs = _function(mask, "mvCastStatic") or ""
        b = re.search(r"g_mv_and = ([^;]+);\s*g_mv_filter = (\d)u", cs)
        try:
            same = bool(on and b) and _eval_mask(on.group(1), _defines(read(SH + "clean_rt_common.glsl"))) \
                == _eval_mask(b.group(1), _defines(read(SH + "clean_rt_common.glsl"))) and b.group(2) == "1"
        except ValueError:
            same = False
        if not same:
            missing.append("clean_rt_shadow_mask.comp mvCastStatic: the static-weight rays' caster set differs from pass "
                           "15's (RT_WALK_MASK_AND under PDN_OPT_MOVER / blend filter 1) -- (r)")
    # (s) the emissive light build never lists a RIGGED face (by its instance flag): a rigged row on a pass-A slot BLAS
    #     is its bind-pose mesh (a worn animesh without a skin slot) -- its emitters lit the floor in the animesh's shape
    if dl.count("if ((fl & (1u | 512u | 8192u | 32u | 2048u)) != 0u)") < 2:
        missing.append("clean_deferred_light.comp: the emissive light build lists rigged-flag faces again (a worn "
                       "animesh's bind-pose emitters: a bright animesh-shaped patch in the avatar's shadow) -- (s)")
    # (t) a rigged row without a skin never enters the TLAS (its pass-A slot BLAS: the bind-pose mesh at the root)
    pa = _function(inst, "passA") or ""
    if "if ((f & 1u) != 0u) { writeInactive(id); return; }" not in pa:
        missing.append("clean_rt_instances.comp passA: a rigged row without a skin enters the TLAS again (a static "
                       "bind-pose copy of a worn animesh at its root) -- (t)")
    # (v) the fold weighs exactly pass 15's light set (LightInfo.rs_set), not every point light / record-less projector
    if "&& mvRsInSet(li);" not in (_function(mask, "mvLightEligible") or "") \
            or "std::memcpy(mLightInfoCPU.rs_set, mRsSet, sizeof(mRsSet));" not in rv \
            or "uvec4 rs_set[2];" not in read(SH + "clean_lights_common.glsl"):
        missing.append("clean_rt_shadow_mask.comp mvLightEligible: the mover fold weighs lights pass 15 does not carry "
                       "again (LightInfo.rs_set): a mover blocking one darkens the radiance of the others -- (v)")
    # (o) the merged frame's MOVER sun never takes the RT sun shadow cache's reused class (stale boxes)
    rcc = read("indra/newview/vkgfx/llcleanrtcachevk.cpp")
    if "const bool mover_sun = (moverBits() & 5u) == 5u;" not in rcc or "rtMaskCacheWanted() && !mover_sun" not in rcc \
            or "const bool sun_reuse = rc_cls != 0u && !moverSunOnly();" not in main:
        missing.append("the RT sun shadow cache runs on the merged frame's mover-only sun again (a mover without a host "
                       "footprint leaves stale blocked tiles: the worn animesh's boxes) -- (o)")
    if "if (rtAdaptiveDone(k, n, vis, first)) { rays = 2.0; break; }" not in core \
            or "const uint i = n - 1u - k;" not in core:
        missing.append("clean_shadow_rt.glsl projShadowRayCoreImpl: the RT shadow rays (the mover rays' core) no longer "
                       "take the adaptive sampling (outer pair first, the whole disc only in penumbrae) -- (l)")
    if "g_mv_ray_n = ps.texel.x > 0.0" not in lvis:
        missing.append("clean_rt_shadow_mask.comp mvLightVis: the mover disc rays are not counted -- (i)")
    missing += _reuse(read, mask, main, lvis, dl)
    missing += _cost(read, mask, main, inst, mv)
    if "mvSegHit(P, X, s.xyz, s.w + rl)" not in (_function(mask, "mvSegHitFirst") or "") \
            or "+ mvLightRadius(lights[li])" not in main:
        missing.append("clean_rt_shadow_mask.comp: the cull no longer grows by the light's disc radius")
    # (f, revised 2026-11-12) the mover caster set only changes the casters: mvCastMovers arms nothing else, and the
    #     mover rays follow it straight into the RT core (mvLightVis), the merged sun's into sunShadowRayCore
    if (_function(mask, "mvCastMovers") or "").count(";") != 2 \
            or not re.search(r"mvCastMovers\(\);\s*\n\s*vm = mvLightVis\(", main) \
            or not re.search(r"if \(mvSun\) \{ mvCastMovers\(\);", main):
        missing.append("clean_rt_shadow_mask.comp: the mover caster set does more than pick the casters, or the mover "
                       "rays are no longer traced right after it (rule (ad))")
    return missing, stoch_and, mask, read(SH + "clean_rt_common.glsl"), inst


def _nocomment(src):
    return re.sub(r"//[^\n]*", "", src)


# tokens of a per-frame / random choice: none may decide which lights take mover or static rays (ap)
_FRAME_TOKENS = ("mp.p0", "rt_noise_shift", "projIGN", "occMix", "occKeyTexel", "g_occ_u", "frame", "rnd", "rand",
                 "hash", "time", "moverStatsFrame", "g_mv_stats")


def _cost(read, mask, main, inst, mv):
    """(ap) the mover-ray cost cuts (2026-11-13): the avatar box (exact), pass 3's walled rule (exact) and contribution
    cut (bounded), all deterministic; the stats frame only counts."""
    out = []
    nm = _nocomment(mask)
    # THE AVATAR BOUND (2026-11-14): every REAL joint (mvJointReal) joins a cluster box, each box grown by MV_JOINT_PAD;
    # the sphere covers the same joints + pad and every attachment sphere; the attachment box takes every contributor's
    # own sphere -- read as sphere AND (a cluster box OR the attachment box)
    ma = _nocomment(_function(inst, "moverAvatar") or "")
    if "if (!mvJointReal(W, C)) continue;" not in ma or "r = max(r, length(p - C) + MV_JOINT_PAD);" not in ma \
            or "clo[best] = min(clo[best], p); chi[best] = max(chi[best], p);" not in ma \
            or "if (best == MV_CL_N) best = near1;" not in ma \
            or "const vec3 lo = clo[k] - vec3(MV_JOINT_PAD), hi = chi[k] + vec3(MV_JOINT_PAD);" not in ma \
            or "mv[MV_AV_CLN + av] = nc;" not in ma \
            or "moverAvatarBox(av, s.xyz - vec3(s.w), s.xyz + vec3(s.w));" not in ma \
            or "atomicMax(mv[x + k], ~mvOrd(lo[k]));" not in (_function(inst, "moverAvatarBox") or ""):
        out.append("clean_rt_instances.comp moverAvatar: the avatar bound no longer covers every real joint + "
                   "MV_JOINT_PAD (a joint cluster) and every contributor's sphere (the attachment box) -- a mover the "
                   "bound misses loses its shadow -- (ap)")
    jr = _nocomment(_function(inst, "mvJointReal") or "")
    if "if (W == mat4(1.0)) return false;" not in jr or "length(p) < MV_HUD_R && length(C) > 4.0 * MV_HUD_R" not in jr:
        out.append("clean_rt_instances.comp mvJointReal: absent rows (identity / unwritten) and the HUD attachment points "
                   "at the world origin count as joints again -- one avatar's bound spans from the region's corner -- (ap)")
    sha = _nocomment(_function(mask, "mvSegHitFirst") or "")
    if sha.count("&& mvBoxSeg(m, P, X, rl)") != 2 or sha.count("mvSegHit(P, X, s.xyz, s.w + rl)") != 2:
        out.append("clean_rt_shadow_mask.comp mvSegHitFirst: the per-texel cull must test the sphere AND the avatar's "
                   "fine bound (each bounds the caster) -- (ap)")
    bs = _nocomment(_function(mask, "mvBoxSeg") or "")
    if "if (nc == 0u) return true;" not in bs or "if (mvSegHitBox(P, X, clo, chi, rl)) return true;" not in bs \
            or "return mvAvBox(m, lo, hi) && mvSegHitBox(P, X, lo, hi, rl);" not in bs:
        out.append("clean_rt_shadow_mask.comp mvBoxSeg: an avatar's fine bound is its cluster boxes OR its attachment "
                   "box; a mover without clusters falls back to its sphere -- (ap)")
    # the selection reads no per-frame / random input (the stats frame only counts)
    sel = {fn: _nocomment(_function(mask, fn) or "") for fn in
           ("mvSegHitAny", "mvSegHitFirst", "mvBoxSeg", "mvAvBox", "mvAvClusters", "mvAvCluster", "mvRestSettled",
            "mvNextLight", "mvLightEligible", "mvCullBuild",
            "mvFoldKeepC", "mvFoldKeepQ", "mvFoldTraced", "mvFoldDNeed", "mvFoldPlan", "mvFoldWrite")}
    sel["mvSegHitBox"] = _nocomment(_function(mv, "mvSegHitBox") or "")
    blk = main[main.find("// PASS 1: the eligible lights"):main.find("if (restSeen && k < 4u)")]
    conds = re.findall(r"if \((?:!visBlk|visBlk)[^{]*", _nocomment(blk), re.S)
    sel["pass 3 cuts"] = "\n".join(conds)
    bad = sorted({"%s in %s" % (t, fn) for fn, src in sel.items() for t in _FRAME_TOKENS
                  if re.search(r"\b" + re.escape(t) + r"\b", src)})
    if bad or len(conds) != 2 or any(not src for src in sel.values()):
        out.append("the mover light selection must be DETERMINISTIC -- no random number, frame index or stats-frame "
                   "flag in the cull, the light walk or pass 3's cuts: " + (", ".join(bad) or "a function moved") + " -- (ap)")
    # the stats frame: every use of the flag only counts (atomics into the stats words), never selects
    # (g_mv_sphere_only: mvBoxSeg's "sphere alone" switch -- set exactly once, around the stats count, and reset)
    if nm.count("g_mv_sphere_only = true;") != 1 or nm.count("g_mv_sphere_only = false;") != 2 or not re.search(
            r"g_mv_sphere_only = true;\s*\n\s*if \(segHit \|\| mvSegHitAny\(P, X, mvLightRadius\(L\)\)\) \+\+stCandS;"
            r"\s*\n\s*g_mv_sphere_only = false;", nm):
        out.append("clean_rt_shadow_mask.comp: the sphere-only cull (stats) does more than count -- (ap)")
    # pass 3: the walled rule only where every blocked light is statically dark (visBlk from Vs > 0 in any channel),
    # mirroring mvRestRatio (the fold: no slot at all there -- R exactly 1); the cut within its budget, an untraced
    # light's Vs taken as 1/2 (the fold: in the U sums with Vs 1/2, never a slot)
    if "visBlk = visBlk || any(greaterThan(vs, vec3(0.0)));" not in nm \
            or "if (!visBlk && (fold || mvRestSettled(dBlk + dFree, wSum)))" not in nm \
            or "if (visBlk && !(fold && hit) && mvRestSettled(trD, wSum) && all(lessThanEqual(cutW + w, MV_CUT_F * trD)))" not in nm \
            or "trD += w * vs;" not in nm or "dFree += 0.5 * w;" not in nm \
            or "if (fold) { g_fu_w += w; g_fu_ws += 0.5 * w; g_fu_wsm += 0.5 * w; }" not in nm \
            or "if (anyBlk && visBlk)" not in nm \
            or not re.search(r"#define MV_CUT_F 0\.03125\b", mask):
        out.append("clean_rt_shadow_mask.comp pass 3: the walled rule / contribution cut changed -- they must skip a "
                   "static ray only when the ratio is provably 1 (every blocked light Vs 0, the denominator past "
                   "mvRestRatio's fallback) or within MV_CUT_F of the traced static light (Vs taken as 1/2) -- (ap)")
    rs = _nocomment(_function(mask, "mvRestSettled") or "")
    rr = _nocomment(_function(mask, "mvRestRatio") or "")
    if "dot(d, lw) > 1e-3 * dot(dFb, lw)" not in rs or "!(dot(dFb, lw) > 1e-8)" not in rs \
            or "if (!(dot(d, lw) > 1e-3 * dot(dFb, lw)))" not in rr or "if (!(dl > 1e-8)) return vec3(1.0);" not in rr:
        out.append("clean_rt_shadow_mask.comp mvRestSettled no longer mirrors mvRestRatio's fallback tests (the walled "
                   "rule would skip lights whose static weight changes the ratio) -- (ap)")
    # the stats frame (frame info bit 4) and the host's breakdown line
    fi = read(SH + "clean_frame_info.glsl")
    rv = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    rtc = read("indra/newview/vkgfx/llcleanrtvk.cpp")
    if "return (moverBits() & 17u) == 17u;" not in fi or "(mMoverStatsReq ? 16u : 0u)" not in rv \
            or "moverCostLog();" not in rv or "if (mMoverStats.w[10] == 1u)" not in rtc \
            or "g_mv_stats = mvRoute && moverStatsFrame() && uint(mv.length()) > MV_HOLD;" not in nm:
        out.append("the mover cost breakdown (stats frame bit 4, words 10-23, the per-light block, the VKMoverShadow "
                   "'mover cost' lines) is no longer wired -- (ap)")
    return out


def _exact_fold(read, mask, mb, dl, lc):
    """(ar) THE EXACT PIXEL FOLD (2026-11-13, the owner: a hard seam with a lit sliver between two overlapping avatar
    shadows, >= 7 lights in reach): the radiance packing folds EVERY light count at the pixel with pass 15's own terms
    -- explicit slots by light index, the U shares for the rest -- never the texel-level REST ratio."""
    out = []
    nlc = _nocomment(lc)
    if not re.search(r"#define MV_FOLD_N_ID 0xFFFBu", nlc) or not re.search(r"#define MV_FOLD_D_ID 0xFFFAu", nlc):
        out.append("clean_lights_common.glsl: the exact fold's U share ids (MV_FOLD_N_ID / MV_FOLD_D_ID) are gone -- (ar)")
    nmb = _nocomment(mb)
    # the mask: every traced light gathered, the slots planned once all are known, no REST / own slot in the fold
    if nmb.count("mvFoldTraced(") != 2 or "k = mvFoldWrite(slotV, ids, dyn, rpr, restDyn);" not in nmb \
            or not re.search(r"if \(fold\)\s*\{[^}]*if \(anyBlk && visBlk\)\s*\{[^}]*k = mvFoldWrite\([^}]*\}\s*"
                             r"else k = mvFoldWriteHit\(slotV, ids\);\s*\}\s*else if \(restSeen && k < 4u\)", nmb) \
            or "else if (fold && hit) mvFoldKeepH(li);" not in nmb \
            or nmb.count("if (fold) w = max(w, MV_FOLD_W_FLOOR * g_mv_base);") != 2:
        out.append("clean_rt_shadow_mask.comp: the radiance packing no longer gathers every traced light into the exact "
                   "fold (mvFoldTraced in passes 2 and 3) and plans its slots once (mvFoldWrite), or writes the "
                   "texel-level REST ratio again (the lit sliver / the <= 4 vs 5+ seam) -- (ar)")
    ft = _nocomment(_function(mask, "mvFoldTraced") or "")
    if "if (hit && vl > MV_VS_OUT) mvFoldKeepC(li, vm, vs, w, dyn, any(lessThan(vm.rgb, vec3(1.0))));" not in ft \
            or nmb.count(", hit);") < 2:
        out.append("clean_rt_shadow_mask.comp mvFoldTraced: an explicit slot must key on the hit cull (a mover in front of "
                   "the light), never on this frame's Vm < 1 -- the outer penumbra flips between 1 and partial frame to "
                   "frame: a slot that comes and goes never builds a history (per-frame flicker) -- (ar)")
    if "else k = mvFoldWriteHit(slotV, ids);" not in nmb:
        out.append("clean_rt_shadow_mask.comp: the unblocked hit lights' Vm slots are not written where nothing folds "
                   "(their histories break every frame the penumbra noise blocks nothing) -- (ar)")
    kc = _nocomment(_function(mask, "mvFoldKeepC") or "")
    kq = _nocomment(_function(mask, "mvFoldKeepQ") or "")
    if "while (j > 0u && g_fc_li[j - 1u] > li)" not in kc \
            or "if (li > g_fc_li[3]) { g_fc_blkOver = g_fc_blkOver || blk; return; }" not in kc \
            or "g_fc_blkOver = g_fc_blkOver || g_fc_blk[3];" not in kc \
            or "while (j > 0u && g_fq_li[j - 1u] > li)" not in kq or "if (li > g_fq_li[3]) return;" not in kq:
        out.append("clean_rt_shadow_mask.comp mvFoldKeepC / Q: the explicit lights are no longer the lowest LIGHT "
                   "INDICES (a per-texel order changes the slot set between neighbours: a seam) -- (ar)")
    fw = _nocomment(_function(mask, "mvFoldWrite") or "")
    for line in ("ids[k] = g_fc_li[i] + 1u;", "ids[k] = MV_VS_TAG | (g_fc_li[i] + 1u);",
                 "ids[k] = MV_VS_TAG | (g_fq_li[i] + 1u);", "ids[k] = MV_FOLD_N_ID;", "ids[k] = MV_FOLD_D_ID;",
                 "slotV[k] = vec4(mvPixelRatio(rwsm, rw), rpr);", "rwsm -= cWs[i] * g_fc_vm[i].rgb;"):
        if line not in fw:
            out.append("clean_rt_shadow_mask.comp mvFoldWrite: `%s` missing -- (ar)" % line)
    # the lighting: explicit lights by their slots, EVERY other light pass 15 carries here by its own term x the U shares
    pf = _nocomment(_function(dl, "deferMoverPixelFold") or "")
    for line in ("for (uint li = 0u; li < count; ++li)", "if ((lm[li >> 5u] & (1u << (li & 31u))) != 0u) continue;",
                 "if (!mvLightEligible(lights[li], li)) continue;", "tU += dt; sU += st;",
                 "numD += nU * tU; denD += dU * tU;", "numS += nU * sU; denS += dU * sU;",
                 "const vec3 dU = hasD ? rtMaskIdTapsS(MV_FOLD_D_ID, t, slot1) : vec3(1.0);",
                 "const vec3 nU = hasN ? min(rtMaskIdTapsS(MV_FOLD_N_ID, t, slot1), dU) : dU;"):
        if line not in pf:
            out.append("clean_deferred_light.comp deferMoverPixelFold: `%s` missing -- every light pass 15 carries at "
                       "the pixel must enter the fold by its own term (the U set by the mask's shares) -- (ar)" % line)
    if "PROJ_MASK_REST_ID" in pf or "mix(" in pf:
        out.append("clean_deferred_light.comp deferMoverPixelFold: a REST read / tap-weight blend is back -- (ar)")
    # the pixel walks the mask's light set: the same eligibility bodies
    for fn in ("mvRsInSet", "mvLightEligible"):
        a = re.sub(r"\s+", " ", _nocomment(_function(mask, fn) or "")).strip()
        b = re.sub(r"\s+", " ", _nocomment(_function(dl, fn) or "")).strip()
        if not a or a != b:
            out.append("%s differs between clean_rt_shadow_mask.comp and clean_deferred_light.comp: the pixel's U set "
                       "is not the mask's light set -- (ar)" % fn)
    # instrumentation: the fold kind reaches the debug view and the probes
    if dl.count("g_mv_fold_info") < 4 or "RTP_M_FOLD" not in mask \
            or "#define RTP_M_FOLD    16u" not in read(SH + "clean_rt_mover_probe.glsl"):
        out.append("the exact fold's kind no longer reaches RenderVKLitDebugMask 32768 / the mover probes -- (ar)")
    return out


def _reuse(read, mask, main, lvis, dl):
    """(an) the mover slots go through the PROVEN RT shadow path: its ray core with the frame's disc rotation, its
    slots and REST ratio, its temporal / spatial filters and its reader -- no mover-only variant of any of them."""
    out = []
    sp = read(SH + "clean_rt_mask_spatial.comp")
    rd = read(SH + "clean_shadow_mask_read.glsl")
    lc = read(SH + "clean_lights_common.glsl")
    # deleted mechanisms stay deleted (code, not comments)
    gone = {"clean_rt_mask_spatial.comp": (sp, ["mvRest", "mvSurfTol", "mvSoftNormalW", "mvSettled", "mvPxAngle",
                                                "mvPxWorld", "MV_SP_CURV_R", "mvOwn", "moverRouteOn"]),
            "clean_shadow_mask_read.glsl": (rd, ["rtMaskTapsMv", "rtMaskPxWorld", "rtMoverProjTaps", "fpOn",
                                                 "rtMoverAll", "rtMoverSpec"]),
            "clean_rt_shadow_mask.comp": (mask, ["mvPixelNoise", "g_mv_hint", "mvHint", "mvKeep", "mvAnyBlocked",
                                                 "mvSpecPeak", "inTop", "pixFold", "mvSpecLobe",
                                                 "PROJ_MASK_MOVER_SPEC_A_ID", "PROJ_MASK_MOVER_SPEC_B_ID"]),
            "clean_deferred_light.comp": (dl, ["rtMoverAll", "rtMoverSpec", "PROJ_MASK_MOVER_SPEC_A_ID",
                                               "PROJ_MASK_MOVER_SPEC_B_ID"]),
            "clean_lights_common.glsl": (lc, ["PROJ_MASK_MOVER_DEN_ID", "PROJ_MASK_MOVER_SPEC_A_ID",
                                              "PROJ_MASK_MOVER_SPEC_B_ID", "MV_SPEC_ROUGH_A", "MV_SPEC_ROUGH_B"])}
    back = ["%s in %s" % (g, n) for n, (src, gs) in gone.items() for g in gs
            if re.search(r"\b" + g + r"\b", _nocomment(src))]
    for n in ("clean_rt_mask_spatial.comp", "clean_shadow_mask_read.glsl", "clean_rt_shadow_mask.comp"):
        if "PROJ_MASK_MOVER_DEN_ID" in _nocomment(read(SH + n)):
            back.append("PROJ_MASK_MOVER_DEN_ID in " + n)
    if back:
        out.append("a mover-only filter / reader / sampling variant is back (the mover slots must use the proven RT "
                   "shadow path, rule (an)): " + ", ".join(sorted(set(back))))
    # the spatial pass: the projector slots' a-trous weights, the same for every slot id (the probe aside)
    ap = _nocomment(_function(sp, "atrousProj") or "")
    for line in ("const float wz = exp(-abs(dt.x - zp) / tol);",
                 "const float wn = 1.0 / (1.0 + 200.0 * length(dt.yz / max(dt.x, 1e-3) - sc));",
                 "const float wl = exp(-abs(lum3(v.rgb) - lc) / sl);",
                 "const bool myoung = young && rtMoverYoungFix() && rtProjMover(dc.w, s);",
                 "if (young && !myoung)",
                 "const bool want = partial(c.rgb) || young || c.a > 1e-5;"):
        if line not in ap:
            out.append("clean_rt_mask_spatial.comp atrousProj: the projector slots' filter changed (`%s` missing) -- "
                       "the mover slots must take the proven weights, rule (an)" % line)
    if re.search(r"PROJ_MASK_\w+", ap.replace("prbT && id == PROJ_MASK_REST_ID", "")):
        out.append("clean_rt_mask_spatial.comp atrousProj: a slot id is special-cased in the filter (the probe aside) -- (an)")
    # the reader: the mover reads are the proven upsample + per-light lookup
    if "g_mv_taps = rtMaskTapsEx(P, true, moverMaskSlot1());" not in rd:
        out.append("clean_shadow_mask_read.glsl rtMoverTaps: not the proven upsample (rtMaskTapsEx) -- (an)")
    if "rtMaskProjTapsS(" not in (_function(rd, "rtMoverLight") or ""):
        out.append("clean_shadow_mask_read.glsl rtMoverLight: not the proven per-light lookup (rtMaskProjTapsS) -- (an)")
    em = _nocomment(_function(dl, "deferEmitterLight") or "")
    pf = _nocomment(_function(dl, "deferMoverPixelFold") or "")
    if "g_mv_fold_d = fD;" not in em or "g_mv_fold_s = fS;" not in em \
            or "radD *= g_mv_fold_d;" not in em or "radS *= g_mv_fold_s;" not in em \
            or "deferMoverPixelFold(P, N, V, pbr, base, diffuseColor, specularColor, rough, ls, fD, fS);" not in em:
        out.append("clean_deferred_light.comp deferEmitterLight: the radiance no longer takes the mover factors (the "
                   "exact pixel fold) -- (an) / (ao) / (ar)")
    # (ao) THE PIXEL FOLD: per-light slots read by id (no REST fallback), weighed by pass 15's own terms at the pixel
    for line in ("const vec3 vm = rtMaskIdTapsS(lid[q], t, slot1);",
                 "const vec3 vs = rtMaskIdTapsS(MV_VS_TAG | lid[q], t, slot1);",
                 "if (!projDirectTerms(li, P, N, V, pbr, base, diffuseColor, specularColor, rough, ls, false, dt, st, pd))",
                 "numD += dt * vs * vm; denD += dt * vs;", "numS += st * vs * vm; denS += st * vs;",
                 "fD = mvPixelRatio(numD, denD);", "fS = mvPixelRatio(numS, denS);"):
        if line not in pf:
            out.append("clean_deferred_light.comp deferMoverPixelFold: `%s` missing -- the pixel fold must weigh each "
                       "light by pass 15's own term here and read its slots by id -- (ao)" % line)
    if not re.search(r"#if DEFER_PSTOCH \|\| CLEAN_DEFER_PASS == 3 \|\| CLEAN_DEFER_PASS == 7\s*\n(//[^\n]*\n)*#define CLEAN_PROJ_STOCH_TERMS", dl):
        out.append("clean_deferred_light.comp: projDirectTerms (CLEAN_PROJ_STOCH_TERMS) is not compiled into BASE / the "
                   "combine -- the pixel fold has no per-light terms -- (ao)")
    it = _nocomment(_function(rd, "rtMaskIdTapsS") or "")
    if not it or "PROJ_MASK_REST_ID" in it or "rtMaskSlotOf" in it:
        out.append("clean_shadow_mask_read.glsl rtMaskIdTapsS: missing or falls back to the REST slot (the pixel fold's "
                   "per-id read must not) -- (ao)")
    # the trace: the core's own disc rotation (one frame shift in main), the hint from the caller, the proven slot rules
    nm = _nocomment(mask)
    if nm.count("rt_noise_shift =") != 1 or "rt_noise_shift = fract(mp.p0.x * vec2(0.7548776662, 0.5698402910));" not in nm:
        out.append("clean_rt_shadow_mask.comp: the RT rays' disc pattern is not the frame's R2 rotation everywhere (a "
                   "fixed / per-pixel mover pattern again) -- (an)")
    if "rt_vis_hint" in _nocomment(lvis) or "rt_noise_shift" in _nocomment(lvis):
        out.append("clean_rt_shadow_mask.comp mvLightVis: the mover core call overrides the hint or the pattern -- (an)")
    # (RenderVKShadowMoverMask 2: the local mover block is skipped while pass 15 evaluates the local lights exactly)
    mb = main[main.find("else if (mvRoute && !mvLocalExact && count > 0u"):]
    if main.count("if (hids[h] == li + 1u) { hslot = h; break; }") < 2 \
            or "vm = mvLightVis(L, P, Nl, cam, time, npx, (own || fold) ? 16u : 1u, llift, camd, 1u);" not in mb \
            or "if (mdyn) dyn |= 2u << k;" not in mb or "if (restDyn) dyn |= 2u << k;" not in mb \
            or "const bool own = !fold && (allOwn || (hit && kHit < ownHitCap));" not in mb \
            or "const bool allOwn = nElig <= 4u;" not in mb \
            or "const uint ownHitCap = allOwn ? 4u : 3u;" not in mb \
            or "prad = mvLightRadius(L) * bt / max(length(X - P) - bt, 0.05) / pxw * s;" not in mb:
        out.append("clean_rt_shadow_mask.comp: the mover lights left the projector loop's slot sequence (own-slot hint, "
                   "own 16 / REST 1 rays -- radiance: every mover light at 16 --, the moving-caster bit, the slot policy, "
                   "the penumbra radius) -- (an)")
    # the REST (ratio mode): ONE ratio slot from the per-light visibilities; static weights only where a mover blocks (m)
    if "const vec3 rv = anyBlk ? mvRestRatio(lost, dBlk + dFree, lostW, wSum) : vec3(1.0);" not in mb \
            or "ids[k] = PROJ_MASK_REST_ID;" not in mb or "if (any(lessThan(vm, vec3(1.0))))" not in mb \
            or "if (anyBlk)" not in mb \
            or mb.count("const vec3 vs = mvLightVis(L, P, Nl, cam, time, npx, 4u, llift, camd, 1u);") != 2:
        out.append("clean_rt_shadow_mask.comp: the mover REST is no longer ONE ratio slot sum w Vs Vm / sum w Vs from the "
                   "per-light visibilities, or its static weights are traced where no mover blocks / skipped where one "
                   "does -- (an) / (m) / (ai) / (u)")
    out += _exact_fold(read, mask, mb, dl, lc)
    if mb.count("mvRays += g_mv_ray_n;") < 3 or "++mvRays" in mb:
        out.append("clean_rt_shadow_mask.comp: the log no longer counts the rays traced (g_mv_ray_n) -- (i)")
    return out


def _classes(stoch_and, mask_src, common, inst):
    """(b): the production macros over every instance class."""
    out = []
    defs = _defines(common)
    try:
        M = {k: _eval_mask(v, defs) for k, v in defs.items()}
    except ValueError as e:
        return ["clean_rt_common.glsl mask macros not evaluable: %s" % e]
    mover = M.get("RT_MASK_MOVER")
    if mover is None:
        return ["clean_rt_common.glsl: RT_MASK_MOVER is gone"]
    # the class each kind of instance carries (clean_rt_instances.comp maskClass + moverClass)
    mc = _function(inst, "moverClass") or ""
    if "return RT_MASK_ATTACH;" not in mc or "cls != RT_MASK_OPAQUE && cls != RT_MASK_SMALL && cls != RT_MASK_ALPHAMASK" not in mc:
        out.append("clean_rt_instances.comp moverClass: a moving static no longer moves into a mover class "
                   "(or blend / terrain / water now do)")
    static = {"opaque": 0x01, "small opaque": M["RT_MASK_SMALL"], "alpha-mask": M["RT_MASK_ALPHAMASK"],
              "terrain": M["RT_MASK_TERRAIN"]}
    movers = {"rigged / own avatar": M["RT_MASK_RIGGED"], "worn attachment": M["RT_MASK_ATTACH"],
              "moving static (re-classed)": M["RT_MASK_ATTACH"]}
    blend = M["RT_MASK_BLEND"]
    for name, cls in movers.items():
        if not cls & mover:
            out.append("%s (class 0x%02x) is not in RT_MASK_MOVER 0x%02x" % (name, cls, mover))
    for name, cls in static.items():
        if cls & mover:
            out.append("%s (class 0x%02x) is in RT_MASK_MOVER 0x%02x: the stochastic rays would lose it" % (name, cls, mover))
    # pass 15's AND (the "on" branch of its ternary)
    m = re.search(r"\?\s*\(([^?:]+)\)\s*:", stoch_and or "")
    if not m:
        return out + ["clean_deferred_light.comp: pass 15's RT_WALK_MASK_AND on-branch not parsed: %s" % stoch_and]
    s_and = _eval_mask(m.group(1), defs)
    cm = _function(mask_src, "mvCastMovers") or ""
    cs = _function(mask_src, "mvCastStatic") or ""
    a = re.search(r"g_mv_and = ([^;]+);\s*g_mv_filter = (\d)u", cm)
    b = re.search(r"g_mv_and = ([^;]+);\s*g_mv_filter = (\d)u", cs)
    if not a or not b or a.group(2) != "2" or b.group(2) != "1":
        return out + ["clean_rt_shadow_mask.comp: mvCastMovers / mvCastStatic changed (caster set or filter sense)"]
    m_and, st_and = _eval_mask(a.group(1), defs), _eval_mask(b.group(1), defs)
    ray_masks = {"disc / mixed (RT_MASK_SHADOW)": M["RT_MASK_SHADOW"],
                 "opaque set (SHADOW & ~BLEND)": M["RT_MASK_SHADOW"] & ~blend & 0xFF,
                 "translucent ray (BLEND)": blend, "foliage ray (ALPHAMASK)": M["RT_MASK_ALPHAMASK"]}
    for rname, rm in ray_masks.items():
        for name, cls in movers.items():
            if cls & rm & s_and:
                out.append("pass 15's %s ray still hits a %s (0x%02x): the stochastic history sees a mover" % (rname, name, cls))
            if cls & rm & st_and:
                out.append("the mask's static weight %s ray hits a %s" % (rname, name))
        for name, cls in static.items():
            if cls & rm & m_and:
                out.append("the mask's mover %s ray hits a static %s (0x%02x): static shadows twice" % (rname, name, cls))
    for name, cls in movers.items():
        if not cls & M["RT_MASK_SHADOW"] & m_and:
            out.append("the mask's mover rays miss a %s (0x%02x): its shadow is gone" % (name, cls))
    for name, cls in static.items():
        if not cls & M["RT_MASK_SHADOW"] & s_and:
            out.append("pass 15's rays miss a static %s (0x%02x)" % (name, cls))
    # blend casters reach both pipelines' masks; the filter splits them
    if not (blend & s_and and blend & m_and):
        out.append("a blend-class caster no longer reaches both pipelines (the filter cannot split it)")
    return out


_FIXTURE = r"""
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <cstring>
struct vec3 { float x, y, z; vec3(float a = 0.f) : x(a), y(a), z(a) {} vec3(float a, float b, float c) : x(a), y(b), z(c) {}
               float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
               float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); } };
static vec3 operator+(vec3 a, vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 operator-(vec3 a, vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static vec3 operator*(float s, vec3 a) { return a * s; }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length(vec3 a) { return std::sqrt(dot(a, a)); }
static float clamp(float v, float a, float b) { return std::min(std::max(v, a), b); }
struct vec4 { vec3 xyz; float w; };
struct uvec4 { uint32_t v[4]; uint32_t operator[](uint32_t i) const { return v[i]; } };
using uint = uint32_t;
static uint bitCount(uint x) { return (uint)__builtin_popcount(x); }
static vec3& operator-=(vec3& a, vec3 b) { a = a - b; return a; }
static vec3& operator+=(vec3& a, vec3 b) { a = a + b; return a; }
static vec3 operator*(vec3 a, vec3 b) { return vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
static float max(float a, float b) { return std::max(a, b); }
static float min(float a, float b) { return std::min(a, b); }
static float abs(float a) { return std::fabs(a); }
static uint floatBitsToUint(float f) { uint u; std::memcpy(&u, &f, 4); return u; }
static float uintBitsToFloat(uint u) { float f; std::memcpy(&f, &u, 4); return f; }
// (the exact fold's candidate lists: clean_rt_shadow_mask.comp's globals)
static uint g_fc_n, g_fq_n, g_fc_tot, g_fq_tot;
static uint g_fc_li[4], g_fq_li[4];
static vec4 g_fc_vm[4], g_fc_vs[4], g_fq_vs[4];
static vec3 g_fc_w[4], g_fq_w[4], g_fq_vm[4];
static bool g_fc_dyn[4], g_fc_blk[4], g_fc_blkOver;
HELPERS
static uint32_t rng = 12345u;
static float rnd() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) * (1.0f / 16777216.0f); }
static vec3 rv(float s) { return vec3((rnd() - .5f) * s, (rnd() - .5f) * s, (rnd() - .5f) * s); }
// brute force: does any point of segment a -> b lie within r of c?
static bool blocks(vec3 a, vec3 b, vec3 c, float r)
{
    for (int i = 0; i <= 512; ++i) { const vec3 p = a + (b - a) * (i / 512.f); if (length(p - c) < r * 0.999f) return true; }
    return false;
}
int main()
{
    // THE LANE -> MOVER MAP (mvRankBelow): the k-th active lane of a ballot took mover base + k (its
    // subgroupBallotExclusiveBitCount rank); a ballot bit must map back to exactly that rank, for any active set
    // of a subgroup up to 128 lanes (inactive lanes: texels that returned early).
    long rankBad = 0;
    for (int t = 0; t < 20000; ++t)
    {
        uvec4 act;
        for (int w = 0; w < 4; ++w) act.v[w] = (uint32_t)(rnd() * 4294967295.0);
        if (t % 4 == 0) { act.v[1] = act.v[2] = act.v[3] = 0u; }   // a 32-lane subgroup
        uint rank = 0u;
        for (uint lane = 0u; lane < 128u; ++lane)
        {
            if (!((act.v[lane >> 5] >> (lane & 31u)) & 1u)) continue;
            if (mvRankBelow(act, lane) != rank) ++rankBad;
            ++rank;
        }
    }
    std::printf("lane map: %ld wrong ranks\n", rankBad);
    if (rankBad) return 1;
    // THE REST RATIO (mvRestRatio, production code): one light is exactly its Vm whatever its static visibility (0
    // included: the fallback); a light a wall blocks (Vs 0) never dilutes another light's mover shadow; no mover
    // blocked -> 1; two equal lights, one blocked by the mover -> 1/2.
    {
        int bad = 0;
        const vec3 one(1.f), zero(0.f);
        for (float vm : {0.f, 0.3f, 0.7f, 1.f})
            for (float vs : {0.f, 0.25f, 1.f})
            {
                const vec3 r = mvRestRatio(vec3(vs * (1.f - vm)), vec3(vs), vec3(1.f - vm), one);
                if (std::fabs(r[0] - vm) > 1e-5f || std::fabs(r[1] - vm) > 1e-5f) ++bad;
            }
        // A: w 1, Vs 1, Vm 0 (the mover); B: w 1, Vs 0 (a wall), Vm 1 -> pass 15 lights the texel by A only: R = 0
        if (std::fabs(mvRestRatio(vec3(1.f), vec3(1.f), vec3(1.f), vec3(2.f))[1]) > 1e-5f) ++bad;
        if (std::fabs(mvRestRatio(zero, vec3(2.f), zero, vec3(2.f))[1] - 1.f) > 1e-5f) ++bad;
        if (std::fabs(mvRestRatio(vec3(1.f), vec3(2.f), vec3(1.f), vec3(2.f))[1] - 0.5f) > 1e-5f) ++bad;
        std::printf("rest ratio: %d wrong\n", bad);
        if (bad) return 1;
    }
    // (ao) THE PIXEL FOLD (mvPixelRatio over sum t Vs Vm / sum t Vs, production code): one light is exactly its Vm
    // whatever its term and static visibility; a wall-blocked light (Vs 0) never dilutes another's mover shadow; two
    // lights from opposite sides split by THIS pixel's terms (a stripe facing the blocked projector: t 0.9 / 0.1 ->
    // 0.1, the one facing the free point light: t 0.1 / 0.9 -> 0.9); no light -> 1.
    {
        int bad = 0;
        for (float t : {0.01f, 0.5f, 3.f})
            for (float vm : {0.f, 0.3f, 1.f})
                for (float vs : {0.25f, 1.f})
                {
                    const vec3 r = mvPixelRatio(vec3(t * vs * vm), vec3(t * vs));
                    if (std::fabs(r[0] - vm) > 1e-5f || std::fabs(r[2] - vm) > 1e-5f) ++bad;
                }
        if (std::fabs(mvPixelRatio(vec3(1.f * 1.f * 0.f + 1.f * 0.f * 1.f), vec3(1.f * 1.f + 1.f * 0.f))[1]) > 1e-5f) ++bad;
        if (std::fabs(mvPixelRatio(vec3(0.9f * 0.f + 0.1f), vec3(1.f))[1] - 0.1f) > 1e-5f) ++bad;
        if (std::fabs(mvPixelRatio(vec3(0.1f * 0.f + 0.9f), vec3(1.f))[1] - 0.9f) > 1e-5f) ++bad;
        if (std::fabs(mvPixelRatio(vec3(0.f), vec3(0.f))[1] - 1.f) > 1e-5f) ++bad;
        std::printf("pixel fold: %d wrong\n", bad);
        if (bad) return 1;
    }
    // (ap) THE AVATAR BOX (mvOrd / mvUnord / mvSegHitBox, production code): the ordered bits round-trip and keep the
    // float order; a caster within MV_JOINT_PAD of a joint (the bound the sphere assumes) blocking a ray from P to any
    // point of the light's disc is never culled by a box of joints +- the pad grown by the disc radius (each joint
    // cluster of THE AVATAR BOUND is one; mover_exact_mode2.py replays the clustering itself).
    {
        long bad = 0, boxBlocked = 0, boxTests = 0, boxCulled = 0;
        for (int t = 0; t < 200000; ++t)
        {
            const float a = (rnd() - 0.5f) * std::pow(10.f, rnd() * 8.f - 3.f), b = (rnd() - 0.5f) * std::pow(10.f, rnd() * 8.f - 3.f);
            if (mvUnord(mvOrd(a)) != a || ((a < b) != (mvOrd(a) < mvOrd(b)) && a != b)) ++bad;
            if (((a < b) != ((~mvOrd(a)) > (~mvOrd(b)))) && a != b) ++bad;
        }
        const float pad = MV_AV_PAD_V;
        for (int trial = 0; trial < 6000; ++trial)
        {
            const vec3 pel = rv(30.f);
            vec3 J[24];
            vec3 lo = pel - vec3(pad), hi = pel + vec3(pad);
            for (int j = 0; j < 24; ++j)
            {
                J[j] = pel + vec3((rnd() - .5f) * 1.2f, (rnd() - .5f) * 0.6f, (rnd() - .55f) * 2.0f);
                lo = vec3(std::min(lo.x, J[j].x - pad), std::min(lo.y, J[j].y - pad), std::min(lo.z, J[j].z - pad));
                hi = vec3(std::max(hi.x, J[j].x + pad), std::max(hi.y, J[j].y + pad), std::max(hi.z, J[j].z + pad));
            }
            // the box as the GPU stores and reads it (ordered bits)
            for (int k = 0; k < 3; ++k) { lo[k] = mvUnord(~(~mvOrd(lo[k]))); hi[k] = mvUnord(mvOrd(hi[k])); }
            for (int c = 0; c < 6; ++c)
            {
                const float cr = 0.02f + rnd() * 0.25f;
                vec3 dir = rv(2.f); dir = dir * (1.f / std::max(length(dir), 1e-3f));
                const vec3 cc = J[(int)(rnd() * 23.99f)] + dir * (rnd() * (pad - cr));
                for (int r = 0; r < 24; ++r)
                {
                    const vec3 P = pel + rv(5.f), X = pel + rv(10.f);
                    const float rl = (r & 1) ? rnd() * 0.6f : 0.f;
                    const vec3 T = X + rv(2.f * rl) * 0.57f;
                    ++boxTests;
                    const bool hitBox = mvSegHitBox(P, X, lo, hi, rl);
                    if (!hitBox) ++boxCulled;
                    if (length(T - X) > rl || !blocks(P, T, cc, cr)) continue;
                    ++boxBlocked;
                    if (!hitBox) ++bad;
                }
            }
        }
        std::printf("avatar box: %ld segments, %ld blocked by a caster, %ld culled by the box, %ld wrong\n",
                    boxTests, boxBlocked, boxCulled, bad);
        if (bad || boxBlocked < 1000) return 1;
    }
    // (ap) PASS 3's CUTS (mvRestRatio / mvRestSettled production code; the loop is the shader's, replayed): over random
    // texels -- REST lights with coloured weights, some walled (Vs exactly 0), some blocked by the mover -- the walled
    // rule's ratio is BIT-EXACT the full pass's, and the contribution cut's (grey lights) is within its bound
    // (1 - R) x (f / 2) / (1 - f / 2) of the full one.
    {
        long bad = 0, exactSkips = 0, cutSkips = 0, cutCases = 0;
        double worst = 0.0;
        const float f = MV_CUT_F_V;
        for (int t = 0; t < 200000; ++t)
        {
            const int n = 1 + (int)(rnd() * 14.f);
            const bool grey = (t & 1) != 0;
            vec3 w[16], vs[16], vm[16];
            for (int i = 0; i < n; ++i)
            {
                const float g = std::pow(10.f, rnd() * 4.f - 3.f);
                w[i] = grey ? vec3(g) : vec3(g * rnd(), g * rnd(), g * rnd());
                const float u = rnd();
                vs[i] = u < 0.45f ? vec3(0.f) : (u < 0.8f ? vec3(1.f) : vec3(rnd(), rnd(), rnd()));
                const float v = rnd();
                vm[i] = v < 0.6f ? vec3(1.f) : (v < 0.8f ? vec3(0.f) : vec3(rnd()));
            }
            // pass 2: the blocked lights' sums
            vec3 lost(0.f), dBlk(0.f), lostW(0.f), wSum(0.f);
            bool anyBlk = false, visBlk = false;
            for (int i = 0; i < n; ++i)
            {
                wSum += w[i];
                const bool blk = vm[i].x < 1.f || vm[i].y < 1.f || vm[i].z < 1.f;
                if (!blk) continue;
                anyBlk = true;
                const vec3 om = vec3(1.f) - vm[i];
                lostW += w[i] * om; lost += w[i] * vs[i] * om; dBlk += w[i] * vs[i];
                visBlk = visBlk || vs[i].x > 0.f || vs[i].y > 0.f || vs[i].z > 0.f;
            }
            if (!anyBlk) continue;
            const vec3 lw(0.2126f, 0.7152f, 0.0722f);
            vec3 dFull = dBlk, dLazy = dBlk, dCut = dBlk, trD = dBlk;
            vec3 cutW(0.f);
            for (int i = 0; i < n; ++i)
            {
                const bool blk = vm[i].x < 1.f || vm[i].y < 1.f || vm[i].z < 1.f;
                if (blk) continue;
                dFull += w[i] * vs[i];
                if (!visBlk && mvRestSettled(dLazy, wSum)) ++exactSkips; else dLazy += w[i] * vs[i];
                const vec3 cw = cutW + w[i];
                if (visBlk && mvRestSettled(trD, wSum) && cw.x <= f * trD.x && cw.y <= f * trD.y && cw.z <= f * trD.z)
                { cutW += w[i]; dCut += w[i] * 0.5f; ++cutSkips; }
                else { dCut += w[i] * vs[i]; trD += w[i] * vs[i]; }
            }
            const vec3 rFull = mvRestRatio(lost, dFull, lostW, wSum);
            const vec3 rLazy = mvRestRatio(lost, dLazy, lostW, wSum);
            for (int c = 0; c < 3; ++c) if (rFull[c] != rLazy[c]) ++bad;
            if (visBlk)
            {
                // every channel's ratio (those well clear of mvRestRatio's per-channel 1e-3 switch) and the luminance
                ++cutCases;
                const vec3 rCut = mvRestRatio(lost, dCut, lostW, wSum);
                const float dl = dot(dFull, lw);
                for (int c = 0; c < 4; ++c)
                {
                    if (c < 3 && !(dFull[c] > 2e-3f * dl)) continue;
                    const double rf = c < 3 ? rFull[c] : dot(rFull * dFull, lw) / std::max(dl, 1e-30f);
                    const double rc = c < 3 ? rCut[c] : dot(rCut * dFull, lw) / std::max(dl, 1e-30f);
                    const double bound = (1.0 - rf) * (f * 0.5) / (1.0 - f * 0.5) + 1e-5;
                    const double err = std::fabs(rc - rf);
                    worst = std::max(worst, err / std::max(bound, 1e-9));
                    if (err > bound) ++bad;
                }
            }
        }
        std::printf("pass 3 cuts: walled rule skipped %ld static rays (bit-exact), contribution cut %ld over %ld texels "
                    "(worst %.2f of its bound), %ld wrong\n", exactSkips, cutSkips, cutCases, worst, bad);
        if (bad || exactSkips < 1000 || cutSkips < 1000) return 1;
    }
    // (ar) THE EXACT PIXEL FOLD (mvFoldKeepC / mvFoldKeepQ / mvFoldDNeed / mvFoldPlan production code; mvFoldTraced's
    // sums, mvFoldWrite's slots and deferMoverPixelFold's sums replayed): random texels of 1-12 lights -- pixel terms t,
    // mask weights w = t x a proxy error up to 4x either way (the smooth normal / no normal map / no ambiance), static
    // and mover visibilities 0 / 1 / partial. (1) The explicit set is the lowest light indices whatever the walk order
    // (two walks: identical slots -- no seam from the cluster order). (2) Wherever the plan needs no N share and the U
    // set's lights are statically alike, the pixel's R is EXACT (pass 15's terms: sum t Vs Vm / sum t Vs), at ANY light
    // count -- including the old REST texels (> 4 lights), whose proxy ratio is wrong there (the lit sliver).
    {
        const uint32_t rngSave = rng;   // (the cull replay below keeps its own sequence)
        long bad = 0, orderBad = 0, exactN = 0, exactBig = 0, approxN = 0;
        double worstExact = 0.0, errNew = 0.0, errOld = 0.0;
        const vec3 lw(0.2126f, 0.7152f, 0.0722f);
        for (int tr = 0; tr < 200000; ++tr)
        {
            const int n = 1 + (int)(rnd() * 12.f);
            uint li[12]; vec3 t[12], w[12], vs[12], vm[12]; bool hit[12];
            for (int i = 0; i < n; ++i)
            {
                bool dup;
                do { li[i] = (uint)(rnd() * 255.99f); dup = false; for (int j = 0; j < i; ++j) dup = dup || li[j] == li[i]; } while (dup);
                const float g = std::pow(10.f, rnd() * 3.f - 2.f);
                t[i] = (tr & 1) ? vec3(g) : vec3(g * (0.2f + rnd()), g * (0.2f + rnd()), g * (0.2f + rnd()));
                w[i] = t[i] * std::pow(4.f, rnd() * 2.f - 1.f);
                const float u = rnd(), v = rnd();
                vs[i] = u < 0.55f ? vec3(1.f) : (u < 0.7f ? vec3(0.f) : vec3(0.1f + 0.8f * rnd()));
                vm[i] = v < 0.5f ? vec3(1.f) : (v < 0.75f ? vec3(0.f) : vec3(0.1f + 0.8f * rnd()));
                hit[i] = vm[i].x < 1.f || rnd() < 0.5f;   // (the cull: a superset of the blocked lights)
            }
            const auto blocked = [&](int i) { return vm[i].x < 1.f; };
            bool anyBlk = false, visBlk = false;
            for (int i = 0; i < n; ++i) if (blocked(i)) { anyBlk = true; visBlk = visBlk || vs[i].x > 0.f; }
            // the reference: pass 15's own fold at the pixel
            vec3 en(0.f), ed(0.f);
            for (int i = 0; i < n; ++i) { en += t[i] * vs[i] * vm[i]; ed += t[i] * vs[i]; }
            const vec3 rRef = mvPixelRatio(en, ed);
            if (!(anyBlk && visBlk))
            {   // the mask writes no slot: R must be 1 (num = den)
                for (int c = 0; c < 3; ++c) if (std::fabs(rRef[c] - 1.f) > 1e-5f) ++bad;
                continue;
            }
            // the mask: two walks in different orders (mvFoldTraced's sums + candidate lists)
            uint keptC[2][6], keptQ[2][5];
            vec3 uW(0.f), uWs(0.f), uWsm(0.f);
            for (int pass = 0; pass < 2; ++pass)
            {
                g_fc_n = g_fq_n = g_fc_tot = g_fq_tot = 0u; g_fc_blkOver = false;
                int ord[12];
                for (int i = 0; i < n; ++i) ord[i] = i;
                for (int i = n - 1; i > 0; --i) { const int j = (int)(rnd() * (i + 0.999f)); std::swap(ord[i], ord[j]); }
                uW = uWs = uWsm = vec3(0.f);
                for (int o = 0; o < n; ++o)
                {
                    const int i = ord[o];
                    uW += w[i]; uWs += w[i] * vs[i]; uWsm += w[i] * vs[i] * vm[i];
                    const float vl = dot(vs[i], lw);
                    vec4 a; a.xyz = vm[i]; a.w = -1.f;
                    vec4 b; b.xyz = vs[i]; b.w = -1.f;
                    if (hit[i] && vl > 0.02f) mvFoldKeepC(li[i], a, b, w[i], false, blocked(i));
                    else if (vl < 0.98f) mvFoldKeepQ(li[i], b, w[i], vm[i]);
                }
                for (uint j = 0; j < 4u; ++j) { keptC[pass][j] = j < g_fc_n ? g_fc_li[j] : 999u; keptQ[pass][j] = j < g_fq_n ? g_fq_li[j] : 999u; }
                keptC[pass][4] = g_fc_tot; keptQ[pass][4] = g_fq_tot; keptC[pass][5] = g_fc_blkOver ? 1u : 0u;
            }
            for (int j = 0; j < 5; ++j) if (keptC[0][j] != keptC[1][j] || keptQ[0][j] != keptQ[1][j]) ++orderBad;
            if (keptC[0][5] != keptC[1][5]) ++orderBad;
            // the lowest indices, ascending
            for (uint j = 1; j < g_fc_n; ++j) if (g_fc_li[j - 1] >= g_fc_li[j]) ++orderBad;
            // mvFoldWrite: the plan (production) and its slots
            uint cBits = 0u, cBlk = 0u;
            vec3 cW[4], cWs[4], qW[4], qWs[4];
            for (uint i = 0; i < 4u; ++i)
            {
                const bool ci = i < g_fc_n, qi = i < g_fq_n;
                cW[i] = ci ? g_fc_w[i] : vec3(0.f); cWs[i] = ci ? g_fc_w[i] * g_fc_vs[i].xyz : vec3(0.f);
                qW[i] = qi ? g_fq_w[i] : vec3(0.f); qWs[i] = qi ? g_fq_w[i] * g_fq_vs[i].xyz : vec3(0.f);
                if (ci && dot(g_fc_vs[i].xyz, lw) < 0.98f) cBits |= 1u << i;
                if (ci && g_fc_blk[i]) cBlk |= 1u << i;
            }
            const uint plan = mvFoldPlan(g_fc_n, cBlk, g_fc_blkOver, cBits, g_fq_n, uW, uWs, cW, cWs, qW, qWs);
            const uint p = plan & 15u, q = (plan >> 4) & 15u;
            uint slots = 0u;
            vec3 rw = uW, rws = uWs, rwsm = uWsm;
            bool expl[256] = {}; vec3 sVm[256], sVs[256];
            for (int i = 0; i < 256; ++i) { sVm[i] = vec3(1.f); sVs[i] = vec3(1.f); }
            for (uint i = 0; i < p; ++i)
            {
                const uint l = g_fc_li[i];
                expl[l] = true; sVm[l] = g_fc_vm[i].xyz; ++slots;
                if ((cBits >> i) & 1u) { sVs[l] = g_fc_vs[i].xyz; ++slots; }
                rw -= cW[i]; rws -= cWs[i]; rwsm -= cWs[i] * g_fc_vm[i].xyz;
            }
            for (uint i = 0; i < q; ++i)
            {
                const uint l = g_fq_li[i];
                expl[l] = true; sVs[l] = g_fq_vs[i].xyz; ++slots;
                rw -= qW[i]; rws -= qWs[i]; rwsm -= qWs[i] * g_fq_vm[i];
            }
            for (int c = 0; c < 3; ++c) { rw[c] = std::max(rw[c], 0.f); rws[c] = clamp(rws[c], 0.f, rw[c]); rwsm[c] = clamp(rwsm[c], 0.f, rws[c]); }
            const bool hasN = (plan & 256u) != 0u, hasD = (plan & 512u) != 0u;
            slots += (hasN ? 1u : 0u) + (hasD ? 1u : 0u);
            if (slots > 4u) ++bad;
            const vec3 dU = hasD ? mvPixelRatio(rws, rw) : vec3(1.f);
            vec3 nU = hasN ? mvPixelRatio(rwsm, rw) : dU;
            for (int c = 0; c < 3; ++c) nU[c] = std::min(nU[c], dU[c]);
            // the pixel (deferMoverPixelFold): explicit lights by their slots, every other light by t x the U shares
            vec3 pn(0.f), pd(0.f), tU(0.f);
            bool uAlike = true; float uVs = -1.f;
            for (int i = 0; i < n; ++i)
            {
                if (expl[li[i]]) { pn += t[i] * sVs[li[i]] * sVm[li[i]]; pd += t[i] * sVs[li[i]]; continue; }
                tU += t[i];
                if (uVs < 0.f) uVs = vs[i].x;
                uAlike = uAlike && vs[i].x == uVs && vs[i].y == uVs && vs[i].z == uVs && !blocked(i);
            }
            pn += nU * tU; pd += dU * tU;
            const vec3 r = mvPixelRatio(pn, pd);
            // the old radiance REST (every light in one proxy-weighted ratio) at the same texel, for the comparison
            vec3 lost(0.f), dd(0.f), lostW(0.f), wS(0.f);
            for (int i = 0; i < n; ++i)
            {
                wS += w[i]; dd += w[i] * vs[i];
                lost += w[i] * vs[i] * (vec3(1.f) - vm[i]); lostW += w[i] * (vec3(1.f) - vm[i]);
            }
            const vec3 rOld = mvRestRatio(lost, dd, lostW, wS);
            const double eNew = std::fabs(dot(r - rRef, lw)), eOld = std::fabs(dot(rOld - rRef, lw));
            // explicit lights without a Vs slot read Vs 1: exact only where their Vs is 1 (lit) or 0.98+ (none here)
            bool fit = !hasN && (uAlike || tU.x + tU.y + tU.z == 0.f);
            for (uint i = 0; i < p && fit; ++i) fit = ((cBits >> i) & 1u) || g_fc_vs[i].xyz.x == 1.f;
            if (fit)
            {
                ++exactN;
                if (n > 4) ++exactBig;
                for (int c = 0; c < 3; ++c)
                {
                    const double e = std::fabs(r[c] - rRef[c]);
                    worstExact = std::max(worstExact, e);
                    if (e > 2e-4) ++bad;
                }
            }
            else { ++approxN; errNew += eNew; errOld += eOld; }
            for (int c = 0; c < 3; ++c) if (!(r[c] >= 0.f && r[c] <= 1.f)) ++bad;
        }
        std::printf("exact fold: %ld texels exact (%ld of them > 4 lights; worst %.1e), %ld past the slots (mean |dR| "
                    "%.4f vs the old REST's %.4f), %ld walk-order differences, %ld wrong\n", exactN, exactBig, worstExact,
                    approxN, approxN ? errNew / approxN : 0.0, approxN ? errOld / approxN : 0.0, orderBad, bad);
        if (bad || orderBad || exactBig < 1000 || (approxN && errNew > errOld)) return 1;
        rng = rngSave;
    }
    long tests = 0, blocked = 0, dropTile = 0, dropTex = 0, sunTests = 0, sunBlocked = 0, sunDropTile = 0, sunDropTex = 0;
    for (int trial = 0; trial < 4000; ++trial)
    {
        // a tile of 32 receivers: a small patch (a few cm to a metre), anywhere in a 40 m box
        const vec3 base = rv(40.f);
        const float span = 0.05f + rnd() * 1.0f;
        vec3 P[32];
        vec3 tmn(1e30f), tmx(-1e30f);
        for (int i = 0; i < 32; ++i)
        {
            P[i] = base + rv(span);
            tmn = vec3(std::min(tmn.x, P[i].x), std::min(tmn.y, P[i].y), std::min(tmn.z, P[i].z));
            tmx = vec3(std::max(tmx.x, P[i].x), std::max(tmx.y, P[i].y), std::max(tmx.z, P[i].z));
        }
        // the lights the tile's texels take (in range of each), the farthest = the reach (as the mask computes it)
        // (+ each light's disc radius rl: a soft projector's rays aim anywhere on it -- mvLightRadius)
        vec3 X[4];
        float rl[4];
        float reach = 0.f;
        for (int l = 0; l < 4; ++l) { X[l] = base + rv(16.f); rl[l] = (l & 1) ? rnd() * 0.6f : 0.f; }
        for (int i = 0; i < 32; ++i) for (int l = 0; l < 4; ++l) reach = std::max(reach, length(X[l] - P[i]) + rl[l]);
        vec3 L = rv(2.f); L = L * (1.f / std::max(length(L), 1e-3f));
        const float sunFar = 512.f;
        const vec3 tc = 0.5f * (tmn + tmx);
        const float tr = 0.5f * length(tmx - tmn) + 0.05f;
        const float rch = reach;
        const bool sunWant = true, locWant = true;
        for (int k = 0; k < 6; ++k)
        {
            vec4 s;
            s.xyz = base + rv(12.f);
            s.w = 0.1f + rnd() * 2.5f;
            bool hs = false, hl = false;
            TILE_PREDICATES
            for (int i = 0; i < 32; ++i)
            {
                for (int l = 0; l < 4; ++l)
                {
                    ++tests;
                    // a ray to a point of the light's disc (the centre for a point light)
                    const vec3 T = X[l] + rv(2.f * rl[l]) * 0.57f;
                    if (length(T - X[l]) > rl[l] || !blocks(P[i], T, s.xyz, s.w)) continue;
                    ++blocked;
                    if (!hl) ++dropTile;
                    if (!mvSegHit(P[i], X[l], s.xyz, s.w + rl[l])) ++dropTex;   // (mvSegHitAny's test)
                }
                ++sunTests;
                if (!blocks(P[i], P[i] + L * 64.f, s.xyz, s.w)) continue;
                ++sunBlocked;
                if (!hs) ++sunDropTile;
                if (!mvRayHit(P[i], L, sunFar, s.xyz, s.w)) ++sunDropTex;
            }
        }
    }
    std::printf("cull: %ld light segments (%ld blocked by a mover: dropped by the tile %ld, by the texel %ld), "
                "%ld sun rays (%ld blocked: dropped %ld / %ld)\n",
                tests, blocked, dropTile, dropTex, sunTests, sunBlocked, sunDropTile, sunDropTex);
    if (blocked < 1000 || sunBlocked < 1000) { std::printf("the fixture's movers block too little: it tests nothing\n"); return 1; }
    return (dropTile || dropTex || sunDropTile || sunDropTex) ? 1 : 0;
}
"""


def _cull_source(read):
    mv = read(SH + "clean_rt_movers.glsl")
    mask = read(SH + "clean_rt_shadow_mask.comp") + "\n" + mv   # (the shared cull bodies: clean_rt_movers.glsl)
    parts = []
    for fn in ("mvSegHit", "mvRayHit", "mvOrd", "mvUnord", "mvSegHitBox"):
        f = _function(mv, fn)
        if not f:
            return None, "clean_rt_movers.glsl " + fn
        parts.append("static " + f)
    for fn in ("mvRestRatio", "mvRestSettled", "mvFoldKeepC", "mvFoldKeepQ", "mvFoldDNeed", "mvFoldPlan"):
        rr = _function(mask, fn)
        if not rr:
            return None, "clean_rt_shadow_mask.comp " + fn
        parts.append("static " + rr)
    pad = re.search(r"#define MV_JOINT_PAD\s+([\d.]+)", mv)
    cut = re.search(r"#define MV_CUT_F\s+([\d.]+)", mask)
    if not pad or not cut:
        return None, "MV_JOINT_PAD / MV_CUT_F"
    parts.append("#define MV_AV_PAD_V %sf\n#define MV_CUT_F_V %sf" % (pad.group(1), cut.group(1)))
    pr = _function(read(SH + "clean_lights_common.glsl"), "mvPixelRatio")
    if not pr:
        return None, "clean_lights_common.glsl mvPixelRatio"
    parts.append("static " + pr)
    rk = _function(mask, "mvRankBelow")
    if not rk:
        return None, "clean_rt_shadow_mask.comp mvRankBelow"
    parts.append("static " + rk.replace("[[unroll]]", ""))
    cb = _function(mask, "mvCullBuild") or ""
    hs = re.search(r"\n\s*(hs = sunWant && [^;]+;)", cb)
    hl = re.search(r"\n\s*(hl = locWant && [^;]+;)", cb)
    if not hs or not hl:
        return None, "clean_rt_shadow_mask.comp mvCullBuild's hs / hl predicates"
    helpers = re.sub(r"//[^\n]*", "", "\n".join(parts))
    helpers = re.sub(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?)(?![\w.f])", r"\1f", helpers)
    return _FIXTURE.replace("HELPERS", helpers).replace("TILE_PREDICATES", hs.group(1) + "\n            " + hl.group(1)), None


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    missing, stoch_and, mask, common, inst = _wiring(read)
    missing += _classes(stoch_and, mask, common, inst)
    source, name = _cull_source(read)
    if source is None:
        result.fail("production code %s not found (moved or renamed?): the cull fixture covers nothing" % name)
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        if missing:
            result.fail("\n".join(missing))
        else:
            result.warn("no C++ compiler: the cull replay not run")
        return
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "mover_cull.cpp")
        with open(src, "w") as o:
            o.write(source)
        exe = os.path.join(tmp, "t")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-w", src, "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("the cull fixture does not compile against the production tests:\n" + c.stderr[:3000])
            return
        r = subprocess.run([exe], capture_output=True, text=True)
    out = r.stdout.strip()
    if r.returncode or missing:
        result.fail("\n".join(([out] if out and r.returncode else []) + missing))
        return
    result.summary = " | ".join(out.splitlines() if out else [])
