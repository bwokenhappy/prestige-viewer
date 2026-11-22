"""Regression guard for the LIGHT CAP BLINK + the last capped cluster readers (2026-11-13).

The owner, a scene with more lights than the 256-light list (the list full every frame, ~150 of it 220-400 m
away, ~90-99 lights ranked past CLUSTER_CAP 127 in overflowed clusters):
  * lights BLINKING: llappviewer.cpp keeps the top RenderLocalLightCount (<= 256) frustum-visible lights by
    camera importance, re-ranked every frame with no hysteresis -- the lights at the cut swapped in and out as
    the camera moved. Now a light kept last frame scores kKeptLightBonus x (LightCache::kept, no per-frame
    allocation), ties break on the light key, and a #VKLitGuard "light cut:" line every 60 s (10 s over the cap) gives candidates /
    kept / cap and how many swapped in / out.
  * lights still UNSHADOWED: the light-shadow-dropout fix (b4ebf54352) made the deferred / pass-15 shadow walks
    lossless, but the RT projector mask (clean_rt_shadow_mask.comp), the RT hit lights (GI / reflections,
    clean_rt_hit_lights.glsl) and the particle-fog / volumetric light loops still walked the capped list: a
    projector past the cap was lit by the resolve but never traced by the mask (the REST ratio of the others:
    lit unshadowed), and the hit / fog / volume lights past it vanished -- which ones changed with the frame's
    order. Each now walks the whole light table where its cluster overflowed (clusterWalkAll /
    clusterShadowedWalkAll).
This check fails when any of that is undone:
  (a) the hysteresis: the kept flag, the bonus (> 1), its use on last frame's kept lights, the re-mark of this
      frame's cut, the key tie-break, the churn log line;
  (b) the four readers back to the capped list alone;
  (c) any other main-view cluster-list walk (clusterLen / clusterShadowedLen, comments stripped) in the shaders
      without a WalkAll fallback in reach, except the listed diagnostics / capture lists;
  (d) a model: noisy importance around the cut -- with the bonus the kept set stops churning, without it it churns.
  (e) THE CUT'S FADE (2026-11-15, lights flicking on and off over a whole sim with the hysteresis in: the owner's
      log at 15a81dcb6e showed 520-535 candidates, 256 kept and ~1 light a frame still crossing the cut): while
      the candidates exceed the cap, the cut keeps light_cap - light_cap / 8 at full, a light it pushes out fades
      out in the reserved slots and one it takes back fades in (RenderVKLightCutFade seconds; 0 = the hard cut);
      a light that leaves the light set and comes back (object re-rezzed, script) keeps its cut state. A model of
      that cut under drifting importance: no drawn light's colour scale moves more than one frame's step.
  (f) THE STICKY CUT (2026-11-15, the cut still churning on a still camera: ~50-70 lights a second in, half as
      many out -- in - out = kept lights whose drawable left the light set): RenderVKLightCutSticky 1 keys the
      cut's membership by the light (s_sticky), holds an absent member's slot, lets an outsider displace a member
      only by beating it kStickyMargin x for kStickyFrames frames in a row, caches importance per camera epoch,
      and logs "light cut why:" every 10 s over the cap. A model: a still camera, +-5% noise and the light
      set churning -> no kept-set change; a 2x newcomer gets its slot after exactly kStickyFrames frames; the
      mode-0 cut churns under the same input.
"""
import os
import random
import re

NAME = "light-cap-hysteresis"
STAGE = "source"
DESCRIPTION = "the 256-light cut has hysteresis (no blinking) and no cluster reader drops lights past CLUSTER_CAP"
GUARDS = ("Lights blinking on and off in a crowded scene (the light list's cut re-ranked every frame) and lights lit "
          "but unshadowed / missing in GI, fog and volumetrics past the cluster cap.")

SH = "indra/vkclean/shaders/"

# (file, a code snippet near the capped walk) -- readers that may walk the capped list alone
ALLOWED_CAPPED = (
    ("clean_surface_light.glsl", "const uint ns  = clusterShadowedLen(raw);"),      # debug view 26 (display)
    ("clean_surface_light.glsl", "const uint n   = clusterLen(raw);"),              # debug view 26 (display)
    ("clean_deferred_light.comp", "if (lid < clusterLen(raw) && cluster_index"),     # the lit guard's stale-entry scan
    ("clean_rt_shadow_mask.comp", "bool mvInClusterList(uint c, uint li)"),          # the mover probe's diagnostics
    ("clean_rt_hit_lights.glsl", "const uint c = CLUSTER_COUNT + list;"),           # a probe capture's own list
)


def _strip(src):
    return re.sub(r"//[^\n]*", "", src)


def _model(bonus, rng):
    """Frames of noisy importance around the cut; returns the swaps after the first frame."""
    n, cap, frames = 400, 256, 200
    base = [rng.lognormvariate(0.0, 0.3) for _ in range(n)]
    kept = set()
    swaps = 0
    for f in range(frames):
        sc = []
        for i in range(n):
            s = base[i] * (1.0 + rng.uniform(-0.05, 0.05))   # the camera moving: +-5% per frame
            if i in kept:
                s *= bonus
            sc.append((-s, i))   # (ties: the key -- here the index)
        sc.sort()
        now = {i for _, i in sc[:cap]}
        if f > 0:
            swaps += len(now - kept)
        kept = now
    return swaps


def _fade_model(fade_secs, rng):
    """The cut with its fade (llappviewer.cpp THE CUT'S FADE), lights always in view, importance drifting
    (a random walk: the camera flying over the sim). Returns (swaps into the kept set after the first frame,
    the largest one-frame change of any light's drawn colour scale after the first frame)."""
    n, cap, frames, bonus, dt = 520, 256, 600, 1.5, 1.0 / 60.0
    imp = [rng.lognormvariate(0.0, 0.5) for _ in range(n)]
    fade = [1.0] * n
    kept = [False] * n
    listed = [False] * n
    drawn = [0.0] * n
    cut_fade = fade_secs > 0.0
    step = min(max(dt, 0.001), 0.1) / fade_secs if cut_fade else 1.0
    swaps, jump = 0, 0.0
    for f in range(frames):
        imp = [v * (1.0 + rng.uniform(-0.03, 0.03)) for v in imp]
        cands = sorted(range(n), key=lambda i: (-(imp[i] * (bonus if kept[i] else 1.0)), i))
        was_kept, was_listed = kept[:], listed[:]
        kept = [False] * n
        listed = [False] * n
        reserve = cap // 8 if (cut_fade and n > cap) else 0
        primary = min(n, cap - reserve)
        for i in cands[:primary]:
            kept[i] = True
            fade[i] = min(fade[i] + step, 1.0) if cut_fade else 1.0
            if f > 0 and not was_kept[i]:
                swaps += 1
        fading = []
        for i in cands[primary:]:
            fade[i] = max(fade[i] - step, 0.0) if (cut_fade and was_listed[i]) else 0.0
            if fade[i] > 0.0:
                fading.append(i)
        fading.sort(key=lambda i: (-fade[i], -imp[i], i))
        for i in fading[reserve:]:
            fade[i] = 0.0
        for i in cands[:primary] + fading[:reserve]:
            listed[i] = True
        now = [fade[i] if listed[i] else 0.0 for i in range(n)]
        if f > 0:
            jump = max(jump, max(abs(a - b) for a, b in zip(now, drawn)))
        drawn = now
    return swaps, jump


def _churn_present(rng, away, n, skip):
    """The lights in the set this frame: each present one leaves with a small chance for 1-60 frames (an object
    re-rezzed, a script toggling the light -- always back within 1 s, under the sticky hold)."""
    present = []
    for i in range(n):
        if away[i] > 0:
            away[i] -= 1
            continue
        if i != skip and rng.random() < 0.004:
            away[i] = rng.randint(1, 60)
            continue
        present.append(i)
    return present


def _sticky_model(c, rng, churn):
    """The sticky cut (llappviewer.cpp THE STICKY CUT): a still camera (+-5% importance noise every frame, worse
    than the cached importance ever sees), the light set churning; at frame 300 a newcomer 2x the weakest member
    appears. Returns the kept-set changes other than the newcomer's, the frame it got its slot, the members it
    displaced."""
    n, cap, frames, fps, t_in = 520, 224, 600, 60, 300
    margin, need = c["margin"], int(c["frames"])
    hold = int(c["hold_secs"] * fps)
    base = [rng.lognormvariate(0.0, 0.5) for _ in range(n)]
    away = [0] * n
    members = {}   # key -> last frame it was a candidate
    streak = {}    # key -> (streak, frame)
    swaps, entrant, entrant_frame, evicted = 0, -1, None, 0
    for f in range(frames):
        if f == t_in:
            base.append(2.0 * min(base[k] for k in members))
            away.append(0)
            entrant = len(base) - 1
        present = _churn_present(rng, away, len(base), entrant) if churn else list(range(len(base)))
        score = {i: base[i] * (1.0 + rng.uniform(-0.05, 0.05)) for i in present}
        before = set(members)
        mem = [i for i in present if i in members]
        out = [i for i in present if i not in members]
        for i in mem:
            members[i] = f
        n_held = 0
        for k in list(members):
            if members[k] == f:
                continue
            if f - members[k] > hold:
                del members[k]
            else:
                n_held += 1
        mem.sort(key=lambda i: (score[i], -i))    # the weakest first
        out.sort(key=lambda i: (-score[i], i))    # the strongest first
        n_admit = min(cap - len(mem) - n_held, len(out))
        kept = set(mem) | set(out[:n_admit])
        for k_i, o_i in zip(mem, out[n_admit:n_admit + len(mem)]):
            if not score[o_i] > margin * score[k_i]:
                break
            s, sf = streak.get(o_i, (0, -10))
            s = s + 1 if sf + 1 == f else 1
            streak[o_i] = (s, f)
            if s < need:
                continue
            streak[o_i] = (0, f)
            kept.discard(k_i)
            kept.add(o_i)
            del members[k_i]
            evicted += 1
        for i in kept:
            members[i] = f
        if f > 0:
            for i in set(members) - before:
                if i == entrant:
                    entrant_frame = f
                else:
                    swaps += 1
    return {"swaps": swaps, "entrant_frame": entrant_frame, "want_entrant_frame": t_in + need - 1,
            "evicted": evicted}


def _legacy_churn_model(rng):
    """Mode 0 (the per-frame re-rank, kept lights x1.5) under the same light set churn: the swaps into the cut."""
    n, cap, frames, bonus = 520, 224, 600, 1.5
    base = [rng.lognormvariate(0.0, 0.5) for _ in range(n)]
    away = [0] * n
    kept = set()
    swaps = 0
    for f in range(frames):
        present = _churn_present(rng, away, n, -1)
        sc = sorted(present, key=lambda i: (-(base[i] * (1.0 + rng.uniform(-0.05, 0.05))
                                              * (bonus if i in kept else 1.0)), i))
        now = set(sc[:cap])
        if f > 0:
            swaps += len(now - kept)
        kept = now
    return swaps


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    fail = []
    app = read("indra/newview/llappviewer.cpp")

    # (a) the hysteresis
    i = app.find("// ---- Local lights (point + projector) ----")
    blk = app[i:i + 60000] if i >= 0 else ""
    m = re.search(r"static const F32 kKeptLightBonus = ([0-9.]+)f;", blk)
    if not m:
        fail.append("llappviewer.cpp: kKeptLightBonus (the light cut's hysteresis) is gone")
    elif not float(m.group(1)) > 1.0:
        fail.append("llappviewer.cpp: kKeptLightBonus %s is not a bonus (> 1)" % m.group(1))
    for need, why in (("on = false, kept = false;", "LightCache::kept (last frame's cut membership)"),
                      ("const bool was_kept = lc.kept;\n            lc.kept = false;", "the per-frame read + reset"),
                      ("if (was_kept) score *= kKeptLightBonus;", "the bonus on last frame's kept lights"),
                      ("cands.push_back({ score, dr, &lc, member, was_listed,", "the candidate carries its score + membership"),
                      ("cd.lc->kept = true;", "this frame's cut marked for the next"),
                      ("return a.lc->key < b.lc->key;", "the key tie-break (a deterministic cut)"),
                      ('LL_INFOS("VKLitGuard") << "light cut: "', "the churn log line")):
        if need not in blk:
            fail.append("llappviewer.cpp light gather: %s missing (%s)" % (need.split("\n")[0], why))
    # the reset happens before any early-out of the loop (a skipped light must lose its kept flag)
    loop = blk.find("for (LightCache& lc : lflat)")
    if loop >= 0:
        first_continue = blk.find("continue;", loop)
        reset = blk.find("lc.kept = false;", loop)
        if reset < 0 or (first_continue >= 0 and reset > first_continue):
            fail.append("llappviewer.cpp: lc.kept is reset after the loop's first continue (a light that left the "
                        "view / stopped being a light keeps last frame's bonus)")
        lreset = blk.find("lc.listed = false;", loop)
        if lreset < 0 or (first_continue >= 0 and lreset > first_continue):
            fail.append("llappviewer.cpp: lc.listed is reset after the loop's first continue (a light not drawn last "
                        "frame would fade out instead of being cut)")

    # (e) the cut's fade (2026-11-15): a light crossing the cut fades instead of switching in one frame
    for need, why in (('LLCachedControl<F32> s_cut_fade(gSavedSettings, "RenderVKLightCutFade", 0.4f);',
                       "the fade setting"),
                      ("const size_t reserve = s_cut_reserve ? light_cap / 8 : 0;",
                       "the reserved slots for the lights fading out"),
                      ("if (!cut_fade || n_cands <= light_cap - light_cap / 8) s_cut_reserve = false;",
                       "the reserve's latch (no flip at a count hovering at the cap)"),
                      ("lc.fade = (cut_fade && cands[i].was_listed) ? llmax(lc.fade - fade_step, 0.f) : 0.f;",
                       "the fade out past the kept set"),
                      ("cd.lc->fade = cut_fade ? llmin(cd.lc->fade + fade_step, 1.f) : 1.f;",
                       "the fade in of the kept set"),
                      ("const LLColor3 col = lc.fade < 1.f ? lc.col * lc.fade : lc.col;",
                       "the fade on the light's colour"),
                      ("s_dropped_cut[old.key] = { rnow, old.fade, old.kept };",
                       "a light that leaves and re-enters the light set keeps its cut state")):
        if need not in blk:
            fail.append("llappviewer.cpp light gather: %s missing (%s)" % (need, why))
    settings = read("indra/newview/app_settings/settings.xml")
    if '<key>RenderVKLightCutFade</key>' not in settings:
        fail.append("settings.xml: RenderVKLightCutFade is not declared")

    # (f) THE STICKY CUT (2026-11-15, the cut churning on a still camera): membership keyed by the light (survives
    # the light set churning), held slots, displacement only by a margin held for N frames, cached importance,
    # and the "light cut why:" diagnostics line
    for need, why in (('LLCachedControl<U32> s_cut_sticky(gSavedSettings, "RenderVKLightCutSticky", 1U);',
                       "the sticky cut's A/B setting"),
                      ("static std::unordered_map<U64, StickySlot> s_sticky;",
                       "the cut's membership keyed by the light's key, not the drawable"),
                      ("if (!member && !s_sticky.empty()) member = s_sticky.find(lc.key) != s_sticky.end();",
                       "a light back in the set / in view resumes its held slot"),
                      ("if (!(challenger.score > kStickyMargin * member.score)) break;",
                       "displacement only by the margin"),
                      ("if (clc.streak < kStickyFrames) continue;",
                       "... held for kStickyFrames frames in a row"),
                      ("const bool expired = st.why == kWhyFrustum ? lframe - st.seen_frame > kStickyHoldFrames",
                       "an absent member holds its slot"),
                      ("if (lc.sc_epoch != s_sticky_epoch || rel_changed(s, lc.sc_s) || rel_changed(lc.c2, lc.sc_c2)",
                       "the importance cached per light and camera epoch"),
                      ("lc.kept = sticky ? false : ret->second.kept;",
                       "the key map's kept flag only in mode 0 (sticky: s_sticky)"),
                      ('LL_INFOS("VKLitGuard") << "light cut why: sticky "', "the diagnostics line"),
                      ("const U32 swapped_out = sticky ? sticky_out : prev_kept - stayed;",
                       "the churn line's out count per mode")):
        if need not in blk:
            fail.append("llappviewer.cpp light gather: %s missing (%s)" % (need, why))
    if '<key>RenderVKLightCutSticky</key>' not in settings:
        fail.append("settings.xml: RenderVKLightCutSticky is not declared")
    consts = {}
    for name, rx in (("margin", r"static const F32 kStickyMargin = ([0-9.]+)f;"),
                     ("frames", r"static const U32 kStickyFrames = ([0-9]+);"),
                     ("hold_frames", r"static const U32 kStickyHoldFrames = ([0-9]+);"),
                     ("hold_secs", r"static const F64 kStickyHoldSecs = ([0-9.]+);")):
        mm = re.search(rx, blk)
        if not mm:
            fail.append("llappviewer.cpp: the sticky cut's %s constant is gone" % name)
        else:
            consts[name] = float(mm.group(1))
    if len(consts) == 4:
        if not consts["margin"] > 1.0 or consts["frames"] < 2:
            fail.append("llappviewer.cpp: the sticky cut's margin %.2f / streak %d no longer resist a single-frame "
                        "swap" % (consts["margin"], consts["frames"]))
        st = _sticky_model(consts, random.Random(1116), churn=True)
        if st["swaps"] != 0:
            fail.append("sticky model: %d kept-set changes on a still camera with +-5%% importance noise and the "
                        "light set churning (want 0)" % st["swaps"])
        if st["entrant_frame"] != st["want_entrant_frame"]:
            fail.append("sticky model: a 2x brighter newcomer took a slot at frame %s (want %d: after %d frames)"
                        % (st["entrant_frame"], st["want_entrant_frame"], int(consts["frames"])))
        if st["evicted"] != 1:
            fail.append("sticky model: the newcomer displaced %d members (want 1)" % st["evicted"])
        legacy = _legacy_churn_model(random.Random(1116))
        if legacy == 0:
            fail.append("sticky model: the mode-0 cut does not churn under the same set churn -- the model no longer "
                        "exercises what the sticky cut fixes")

    # (b) the four readers
    mask = _strip(read(SH + "clean_rt_shadow_mask.comp"))
    for need in ("const bool sAll = clusterShadowedWalkAll(craw);",
                 "const uint n = sAll ? min(count, uint(lights.length())) : clusterShadowedLen(craw);",
                 "const uint li = sAll ? i : cluster_index[ci];"):
        if need not in mask:
            fail.append("clean_rt_shadow_mask.comp: the RT projector loop lost its lossless walk (%s)" % need)
    hit = _strip(read(SH + "clean_rt_hit_lights.glsl"))
    if "walkAll = c == RT_SHADE_ALL_LIGHTS || clusterWalkAll(cluster_count[c]);" not in hit:
        fail.append("clean_rt_hit_lights.glsl: the hit's main-view cluster walk has no clusterWalkAll fallback")
    for f in ("clean_pfog_light.comp", "clean_vol_light.comp"):
        src = _strip(read(SH + f))
        if "const bool walkAll = clusterWalkAll(craw);" not in src \
                or "const uint li = walkAll ? i : cluster_index[c * CLUSTER_CAP + i];" not in src:
            fail.append("%s: the light loop walks the capped cluster list alone (no clusterWalkAll)" % f)
    if "if (++taken > takeMax)" not in _strip(read(SH + "clean_vol_light.comp")):
        fail.append("clean_vol_light.comp: the medium's 32-light budget no longer counts reaching lights on a "
                    "walk-all cluster (it would take the first 32 table entries)")

    # (c) every other capped walk
    shdir = ctx.path(SH)
    for fn in sorted(os.listdir(shdir)):
        if not fn.endswith((".glsl", ".comp", ".frag", ".vert", ".rgen", ".rchit", ".rahit", ".rmiss")):
            continue
        with open(os.path.join(shdir, fn), encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
        code = _strip(raw)
        for mm in re.finditer(r"\bcluster(?:Shadowed)?Len\(", code):
            if fn == "clean_lights_common.glsl":
                break   # (the definitions)
            win = code[max(0, mm.start() - 900):mm.start() + 400]
            if "WalkAll(" in win or "mvClusterOverflow" in win or "ovf ?" in win:
                continue
            if any(fn == af and snip in win for af, snip in ALLOWED_CAPPED):
                continue
            line = code.count("\n", 0, mm.start()) + 1
            fail.append("%s ~line %d (comments stripped): a cluster list walk with no WalkAll fallback -- the lights "
                        "past CLUSTER_CAP are dropped there (add the lossless walk, or list it in ALLOWED_CAPPED "
                        "with the reason)" % (fn, line))

    # (d) the model
    m_on = _model(float(m.group(1)) if m else 1.5, random.Random(1113))
    m_off = _model(1.0, random.Random(1113))
    if m_on != 0:
        fail.append("model: with the bonus, %d swaps over 199 frames of +-5%% importance noise (want 0)" % m_on)
    if m_off == 0:
        fail.append("model: without the bonus no swaps either -- the model no longer exercises the cut")
    swaps, jump = _fade_model(0.4, random.Random(1115))
    step = (1.0 / 60.0) / 0.4
    if swaps == 0:
        fail.append("fade model: no light crossed the cut -- the model no longer exercises the fade")
    if jump > step + 1e-6:
        fail.append("fade model: a listed light's colour scale jumped %.3f in one frame (want <= %.3f)" % (jump, step))
    _, jump_off = _fade_model(0.0, random.Random(1115))
    if jump_off < 1.0:
        fail.append("fade model: with the fade off no light switched in one frame -- the model is not the cut")

    for f in fail:
        result.fail(f)
