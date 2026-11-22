#!/usr/bin/env python3
"""THE CONVERGENCE RULE (clean_dn_temporal.comp, radiance mode): a replay of the denoiser's temporal step on a row of
texels, the old fixed-history rule against the new one, plus the source wiring. No Vulkan device or viewer required.

The model follows the shader line for line where it matters: the same-surface 3-tap neighbourhood (the 3x3 in 1D), the
clamp box mean +- gamma sigma with sigma floored at the history's per-sample spread (dnRadSpread), the clamp's length cut
(dnClamp), the caps (the setting as a HARD cap, THE MOVER COUNTDOWN, the exact cap), the 60 Hz accumulation (htAdvance /
htAlpha at one reference step), and the variance recurrence -- the old one with its young-history 3x3 floor stored in
HIST, the new one temporal-only over a >= DN_ADAPT_VAR_LEN window (a disocclusion starts it at 0).

2026-11-14: the variance-driven lengthening (dnAdaptiveCap, toward 32 frames) is gone -- the owner saw specular smear
at RenderVKDenoiseSpecHistory 1 and shadow noise grow with RenderVKDenoiseHistory: the settings are hard caps now.
The tests that asserted the lengthening (noisy input / reuse break lengthen) now assert the cap instead. The caster evidence tests and THE MOVER RECORD are
left out on both sides: the fixtures show what the history rule alone does.
2026-11-15: THE COUNTDOWN'S NOISE (RenderVKShadowFixMoverVariance, A/B) -- MoverCountdownNoise: a still self-shadow whose
rays meet the avatar every frame starves the a-trous of its noise estimate. The first fix (bee5efbe96, reverted: worse
noise, a smeared walking-avatar shadow) floored the stop with one frame's deviation behind the caster evidence's 1-sigma
gate, which a partial (one light of several) moving shadow passes; the replay shows it softening that shadow. The new
rule gates in luminance too and caps the frame's deviation; modes 1 (OUT) and 2 (HIST) both filter the still case and
leave the moving partial shadow's edges as they were.
"""
import math
import random
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SH = ROOT / 'indra/vkclean/shaders'

GAMMA = 1.5
VAR_LEN = 8.0       # DN_ADAPT_VAR_LEN
EXACT_HIST = 1.0    # DN_EXACT_HIST
CD_LEN = 4          # max(RenderVKRTShadowMoverPersist 4, RenderVKRTShadowDynamicMaxHistory 4)
CD_CAP = 4.0        # RenderVKRTShadowDynamicMaxHistory


def hard_cap(base):
    """The setting (RenderVKDenoiseHistory / the roughness's specular one): a hard cap, never lengthened."""
    return base


class Texel:
    __slots__ = ('h', 'length', 'var', 'cd', 'have')

    def __init__(self):
        self.h, self.length, self.var, self.cd, self.have = 0.0, 0.0, 0.0, 0, False


def step(row, raw, exact, mover, rule, base):
    """One temporal step over the row (a still camera: the history reprojects onto itself). Returns the outputs."""
    n = len(row)
    out = [0.0] * n
    new_state = []
    for i, t in enumerate(row):
        nb = [raw[j] for j in range(max(i - 1, 0), min(i + 2, n))]
        m = sum(nb) / len(nb)
        s = math.sqrt(max(sum(x * x for x in nb) / len(nb) - m * m, 0.0))
        r = raw[i]
        nt = Texel()
        if not t.have:
            nt.h, nt.length, nt.var, nt.have = r, 1.0, s * s, True
            nt.cd = CD_LEN if (rule == 'new' and mover[i]) else 0
            if rule == 'new':
                nt.var = 0.0   # (a disocclusion: HIST starts at 0 -- the 3x3 spread reaches OUT only)
            out[i] = r
            new_state.append(nt)
            continue
        h, length, hv = t.h, t.length, t.var
        # the clamp, its sigma floored at the history's spread (dnRadSpread), and its length cut (dnClamp)
        sig = max(s, math.sqrt(max(hv, 0.0)))
        lo, hi = max(m - GAMMA * sig, 0.0), m + GAMMA * sig
        hc = min(max(h, lo), hi)
        cut = abs(hc - h) / max(h + m, 1e-4)
        if cut > 0.05:
            length = min(length, 8.0 + (1.0 - 8.0) * min(max((cut - 0.05) * 3.0, 0.0), 1.0))
        if rule == 'old':
            max_len = 32.0
            cd = 0
        else:
            max_len = hard_cap(base)
            cd = CD_LEN if mover[i] else max(t.cd - 1, 0)
            if cd > 0:
                max_len = max(min(max_len, CD_CAP), 1.0)
            if exact[i]:
                max_len = min(max_len, EXACT_HIST)
        nd = min(length + 1.0, max_len)
        a = 1.0 / max(nd, 1.0) if length > 0.0 else 1.0
        acc = hc + a * (r - hc)
        if rule == 'old':
            av = 1.0 / max(nd, 1.0)
            var = (1.0 - av) * (hv + av * (r - hc) ** 2)
            if nd < 4.0:
                var = max(var, s * s)   # (stored in HIST: the old rule)
        else:
            av = 1.0 / max(nd, VAR_LEN)
            dev = 0.0 if (exact[i] or cd > 0) else length / (length + 1.0)
            var = (1.0 - av) * (hv + av * dev * (r - hc) ** 2)
        nt.h, nt.length, nt.var, nt.cd, nt.have = acc, nd, var, cd, True
        out[i] = acc
        new_state.append(nt)
    row[:] = new_state
    return out


def band(n, centre, half):
    return [abs(i + 0.5 - centre) < half for i in range(n)]


class MovingShadow(unittest.TestCase):
    N = 96

    def run_band(self, rule, exact_input, noise, seed=7, base=4.0):
        rng = random.Random(seed)
        row = [Texel() for _ in range(self.N)]
        out = raw = shadow = None
        # settle the lit wall first, then sweep an avatar's shadow (16 texels) right at half a texel per frame
        for f in range(160):
            centre = -20.0 if f < 60 else 10.0 + 0.5 * (f - 60)
            shadow = band(self.N, centre, 8.0)
            raw = [0.0 if sh else max(1.0 + (rng.gauss(0.0, noise) if noise > 0.0 else 0.0), 0.0) for sh in shadow]
            out = step(row, raw, [exact_input] * self.N, shadow, rule, base)
        return out, shadow, row

    def trail(self, out, shadow, tol):
        """Texels behind (left of) the shadow that the shadow has left and that are still off the lit value by > tol."""
        first = shadow.index(True)
        return sum(1 for i in range(first) if abs(out[i] - 1.0) > tol)

    def test_exact_edge_has_no_trail(self):
        out, shadow, _ = self.run_band('new', True, 0.0)
        for i, sh in enumerate(shadow):
            self.assertAlmostEqual(out[i], 0.0 if sh else 1.0, places=6,
                                   msg='an exact texel must show this frame\'s value (texel %d)' % i)
        old, shadow_o, _ = self.run_band('old', True, 0.0)
        self.assertGreater(self.trail(old, shadow_o, 0.05), 3,
                           'the fixture no longer shows the old rule\'s trail: it tests nothing')

    def test_exact_edge_leading_side_snaps_dark(self):
        out, shadow, _ = self.run_band('new', True, 0.0)
        last = len(shadow) - 1 - shadow[::-1].index(True)
        self.assertAlmostEqual(out[last], 0.0, places=6, msg='a texel entering the shadow must snap dark')

    def test_stochastic_moving_shadow_trail_bounded(self):
        new, shadow, _ = self.run_band('new', False, 0.05)
        old, shadow_o, _ = self.run_band('old', False, 0.05)
        tn, to = self.trail(new, shadow, 0.15), self.trail(old, shadow_o, 0.15)
        # the countdown (4 frames) + the 4-frame cap: a few texels at half a texel per frame
        self.assertLessEqual(tn, 4, 'the moving-caster cap no longer bounds the trail (%d texels)' % tn)
        self.assertGreater(to, tn, 'the old rule should trail longer (old %d, new %d)' % (to, tn))


class StaticConvergence(unittest.TestCase):
    N = 256

    def run_static(self, sampler, frames=200, seed=3, base=4.0):
        rng = random.Random(seed)
        row = [Texel() for _ in range(self.N)]
        out = None
        for _ in range(frames):
            raw = [sampler(rng) for _ in range(self.N)]
            out = step(row, raw, [False] * self.N, [False] * self.N, 'new', base)
        return out, row

    def test_noisy_input_keeps_the_cap(self):
        # one pick of two equal lights, the other blocked: 0 or 2 -- genuinely noisy; the setting is still a hard cap
        for base in (1.0, 4.0, 24.0):
            _, row = self.run_static(lambda rng: 2.0 if rng.random() < 0.5 else 0.0, base=base)
            self.assertTrue(all(t.length <= base + 1e-6 for t in row),
                            'history above the setting %g (max %.2f)' % (base, max(t.length for t in row)))

    def test_settled_input_keeps_base(self):
        # ReSTIR-converged input: a few percent of noise -- the base history (RenderVKDenoiseHistory 4) is enough
        _, row = self.run_static(lambda rng: 1.0 + rng.gauss(0.0, 0.03))
        self.assertTrue(all(t.length <= 4.0 + 1e-6 for t in row),
                        'settled input must keep the base history (max %.2f)' % max(t.length for t in row))

    def test_reuse_break_keeps_the_cap(self):
        # camera motion breaks the reuse: the same texels go from settled to noisy -- the history stays at the cap
        rng = random.Random(11)
        row = [Texel() for _ in range(self.N)]
        for f in range(120):
            noisy = f >= 60
            raw = [(2.0 if rng.random() < 0.5 else 0.0) if noisy else 1.0 + rng.gauss(0.0, 0.03)
                   for _ in range(self.N)]
            step(row, raw, [False] * self.N, [False] * self.N, 'new', 4.0)
            if f == 59:
                self.assertLessEqual(max(t.length for t in row), 4.0 + 1e-6)
        self.assertLessEqual(max(t.length for t in row), 4.0 + 1e-6)


FEED_LEN = 8.0   # DN_RAD_FEED_LEN
B3 = (1 / 16, 1 / 4, 3 / 8, 1 / 4, 1 / 16)


def halton(i, b):
    f, r = 1.0, 0.0
    while i > 0:
        f /= b
        r += f * (i % b)
        i //= b
    return r


def highlight(x):
    """A static projector highlight on a floor: a soft lobe, a sharp bright board strip and a dark gap."""
    v = 0.1 + 3.0 * math.exp(-((x - 20.0) / 3.0) ** 2)
    if 40.0 <= x < 44.0:
        v += 2.0
    if 30.0 <= x < 31.0:
        v = 0.02
    return v


def crawl_replay(rule, exact, frames=240, n=64, base=4.0, hist_fix=4.0, iters=3, phi_l=4.0, resample=0.0, jitter_input=True):
    """A still camera, a static noise-free input sampled with a per-frame sub-pixel jitter (TAA / DLSS), the temporal
    pass (integer reprojection: clean_pdn_common.glsl pdnSampleGridHc / pdnExactHc land a static receiver on its own
    texel) and the variance-guided a-trous with its youth rules and YOUNG-HISTORY FEEDBACK. Returns (the output's mean
    frame-to-frame change, the input's) over the settled frames: the denoiser must add none of its own."""
    h, ln, hv = [0.0] * n, [0.0] * n, [0.0] * n
    prev_out = prev_raw = None
    d_out = d_raw = 0.0
    cnt = 0

    def youth(length, cap):
        nn = hist_fix if rule == 'old' else min(hist_fix, max(cap, 1.0))
        return min(max(1.0 - length / nn, 0.0), 1.0)

    for t in range(frames):
        j = halton(t % 8 + 1, 2) - 0.5
        raw = [highlight(m + 0.5 + (j if jitter_input else 0.0)) for m in range(n)]
        # (resample > 0: the history fetched bilinearly resample x the jitter off the texel -- an inexact inversion)
        if resample > 0.0:
            off = resample * j
            src_h, src_l, src_v = h[:], ln[:], hv[:]
            for i in range(n):
                x = min(max(i + off, 0.0), n - 1.0)
                b = min(int(math.floor(x)), n - 2)
                fx = x - b
                h[i] = src_h[b] * (1 - fx) + src_h[b + 1] * fx
                ln[i] = src_l[b] * (1 - fx) + src_l[b + 1] * fx
                hv[i] = src_v[b] * (1 - fx) + src_v[b + 1] * fx
        out_c, out_v, caps = [0.0] * n, [0.0] * n, [base] * n
        for i in range(n):
            nb = [raw[q] for q in range(max(i - 1, 0), min(i + 2, n))]
            mm = sum(nb) / len(nb)
            sp = max(sum(x * x for x in nb) / len(nb) - mm * mm, 0.0)
            r = raw[i]
            have = ln[i] > 0.0
            length, hh, var0 = ln[i], h[i], hv[i]
            if have:
                sig = max(math.sqrt(sp), math.sqrt(var0))
                lo, hi = max(mm - GAMMA * sig, 0.0), mm + GAMMA * sig
                hc = min(max(hh, lo), hi)
                cut = abs(hc - hh) / max(hh + mm, 1e-4)
                if cut > 0.05:
                    length = min(length, 8.0 - 7.0 * min(max((cut - 0.05) * 3.0, 0.0), 1.0))
            else:
                hc, length = r, 0.0
            cap = base if rule == 'old' else hard_cap(base)
            if rule == 'new' and exact:
                cap = min(cap, EXACT_HIST)
            caps[i] = cap
            nd = min(length + 1.0, cap)
            a = 1.0 / max(nd, 1.0) if length > 0.0 else 1.0
            acc = hc + a * (r - hc)
            if rule == 'old':
                av = 1.0 / max(nd, 1.0) if have else 1.0
                var = (1.0 - av) * (var0 + av * (r - hc) ** 2)
                if nd < 4.0:
                    var = max(var, sp)
                hist_var = out_var = var
            else:
                av = 1.0 / max(nd, VAR_LEN) if have else 1.0
                dev = 0.0 if exact else length / (length + 1.0)
                var = (1.0 - av) * (var0 + av * dev * (r - hc) ** 2) if have else (0.0 if exact else sp)
                hist_var = var if have else 0.0
                out_var = max(var, sp) if nd < 4.0 else var
            y = youth(nd, cap) if not (rule == 'new' and exact) else 0.0
            if y > 0.0:
                l = max(acc, mm)
                out_var = max(out_var, y * l * l / nd)
            if rule == 'new' and exact:
                out_var = 0.0
            h[i], ln[i], hv[i] = acc, nd, hist_var
            out_c[i], out_v[i] = acc, out_var
        # ---- the a-trous ----
        c, v = out_c[:], out_v[:]
        for k in range(iters):
            step_k = 1 << k
            gv = [(0.25 * v[max(i - 1, 0)] + 0.5 * v[i] + 0.25 * v[min(i + 1, n - 1)]) if k == 0 else v[i]
                  for i in range(n)]
            nc, nv = c[:], v[:]
            for i in range(n):
                if gv[i] < 1e-6:
                    continue   # (idle: nothing to filter)
                ex = rule == 'new' and exact
                y = 0.0 if ex else youth(ln[i], caps[i])
                phi = (phi_l * math.sqrt(gv[i]) + 1e-4) * (1.0 + 2.0 * y)
                s = w_sum = vs = 0.0
                for o in range(-2, 3):
                    q = i + o * step_k
                    if q < 0 or q >= n:
                        continue
                    w = B3[o + 2]
                    if o != 0:
                        w *= math.exp(-abs(c[q] - c[i]) / phi)
                        if y > 0.0:
                            w *= min(max(ln[q] / max(ln[i], 1.0), 1.0), 8.0)
                        else:
                            w *= max(min(ln[q] / hist_fix, 1.0), 1.0 / 8.0)
                    s += w * c[q]
                    vs += w * w * v[q]
                    w_sum += w
                nc[i], nv[i] = s / w_sum, vs / (w_sum * w_sum)
                if k == 0 and not ex:
                    feed = ln[i] < FEED_LEN if rule == 'old' else ln[i] < min(FEED_LEN, caps[i] - 1.0)
                    if feed:
                        h[i] = nc[i]
            c, v = nc, nv
        if t >= frames // 2 and prev_out is not None:
            d_out += sum(abs(a - b) for a, b in zip(c, prev_out)) / n
            d_raw += sum(abs(a - b) for a, b in zip(raw, prev_raw)) / n
            cnt += 1
        prev_out, prev_raw = c, raw
    return d_out / cnt, d_raw / cnt


def feeds(length, base, rule):
    """The a-trous YOUNG-HISTORY FEEDBACK predicate (clean_dn_atrous.comp fbD / fbS)."""
    return length < FEED_LEN if rule == 'old' else length < min(FEED_LEN, base - 1.0)


class FeedbackSettled(unittest.TestCase):
    def test_settled_at_short_base_is_not_fed(self):
        # a gap texel settled at base 4 (RenderVKDenoiseSpecHistory 4): the old rule replaced its history with the
        # a-trous mean every frame -- a blur that compounds: the gap fills with the boards' specular
        gap, board = 0.05, 1.0
        for rule, expect_filled in (('old', True), ('new', False)):
            h = gap
            for _ in range(60):
                length = 4.0
                if feeds(length, 4.0, rule):
                    h = 0.6 * h + 0.4 * board     # (the a-trous mean of a gap beside two boards)
                h = h + (gap - h) / length        # (next frame's temporal step toward the exact raw)
            self.assertEqual(h > 0.3, expect_filled, '%s rule: settled gap value %.3f' % (rule, h))
        # a real restart (length 1 or 2 toward base 4, or anything young toward base 32) still takes the feedback
        self.assertTrue(feeds(1.0, 4.0, 'new') and feeds(2.0, 4.0, 'new') and feeds(7.0, 32.0, 'new'))
        self.assertFalse(feeds(1.0, 1.0, 'new'))


class StaticNoCrawl(unittest.TestCase):
    """The owner's static projector highlight (2026-11-13): SpecHistory 1 clean, 4 the worst crawl, 24 mostly fixed --
    not monotonic: the denoiser added the crawl. The replay reproduces that ordering with a jittered camera whose
    history fetch is a sub-texel bilinear resample (the jittered sample grid's own reprojection), and nothing else."""

    def test_old_rule_reproduces_the_report(self):
        c1 = crawl_replay('old', False, base=1.0, jitter_input=False, resample=1.0)[0]
        c4 = crawl_replay('old', False, base=4.0, jitter_input=False, resample=1.0)[0]
        c24 = crawl_replay('old', False, base=24.0, jitter_input=False, resample=1.0)[0]
        self.assertLess(c1, 1e-9)
        self.assertGreater(c4, c24)
        self.assertGreater(c24, 0.005, 'the fixture no longer crawls under the old rule: it tests nothing')

    def test_exact_static_input_is_unchanged(self):
        for base in (1.0, 4.0, 24.0):
            out, inp = crawl_replay('new', True, base=base, jitter_input=False, resample=1.0)
            self.assertLess(out, 1e-9, 'a static exact input crawls (base %g: %.5f per frame)' % (base, out))
            # a jittered input: the output changes exactly as the input does (no change of the denoiser's own)
            out, inp = crawl_replay('new', True, base=base)
            self.assertAlmostEqual(out, inp, places=9)

    def test_settled_feedback_adds_nothing(self):
        # an integer reprojection (a still camera, the inversion exact): the young-history feedback at base 4 must
        # not move a settled image
        out, _ = crawl_replay('new', False, base=4.0, jitter_input=False, resample=0.0)
        self.assertLess(out, 1e-6)


class HardCaps(unittest.TestCase):
    """2026-11-14 owner report: specular smeared at RenderVKDenoiseSpecHistory 1, a revealed highlight stayed noisy for
    many frames, shadow noise grew with RenderVKDenoiseHistory. The settings are hard caps; a reveal fills at once."""

    def test_reveal_converges_within_the_cap(self):
        # a settled dim floor; at frame 60 a highlight (3.0, small noise) is revealed: no history there (a disocclusion)
        for base in (1.0, 4.0):
            rng = random.Random(5)
            row = [Texel() for _ in range(64)]
            for f in range(60 + int(base) + 1):
                rev = f >= 60
                if f == 60:
                    for i in range(20, 44):
                        row[i] = Texel()
                raw = [(3.0 if (rev and 20 <= i < 44) else 0.2) + rng.gauss(0.0, 0.02) for i in range(64)]
                out = step(row, raw, [False] * 64, [False] * 64, 'new', base)
                if rev:
                    self.assertTrue(all(row[i].length <= base + 1e-6 for i in range(64)))
            err = max(abs(out[i] - 3.0) for i in range(24, 40))
            self.assertLess(err, 0.15, 'base %g: the reveal is %.3f off after %d frames' % (base, err, int(base) + 1))
            self.assertTrue(all(row[i].var < 0.01 for i in range(24, 40)),
                            'a reveal seeded HIST with the spatial edge variance (holds the clamp box open)')

    def test_jittered_input_not_noisier_with_longer_history(self):
        # a static scene seen through a per-frame sub-pixel jitter, the history reprojected onto its own texel (the
        # sample-grid inversion exact): a longer history must never change the output more than history 1 does
        c1 = crawl_replay('new', False, base=1.0)[0]
        for base in (2.0, 4.0, 8.0, 24.0):
            cb = crawl_replay('new', False, base=base)[0]
            self.assertLessEqual(cb, c1 + 1e-9, 'base %g: %.5f per frame against %.5f at 1' % (base, cb, c1))

    def test_spec_settings_are_caps(self):
        dnc = (ROOT / 'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
        # SpecMinHistory never exceeds SpecHistory (a mirror at SpecHistory 1 keeps 1, not SpecMinHistory's 4)
        self.assertIn('q.prm1[3] = std::clamp(p.minHistSpecular, 1.f, q.prm0[1]);', dnc)
        tmp = (SH / 'clean_dn_temporal.comp').read_text()
        i = tmp.index('float maxS = max(mix(dp.prm1.w, dp.prm0.y, smoothstep(0.05, 0.6, g.rough)), 1.0);')
        j = tmp.index('float nS = htAdvance(lenS, maxS);', i)
        # between the setting and the accumulation maxS only ever shrinks (min / mix toward a shorter cap)
        for line in tmp[i:j].splitlines()[1:]:
            if 'maxS =' in line and not any(k in line for k in ('maxS = max(min(maxS', 'maxS = min(maxS', 'dnCasterHistoryCap(maxS')):
                self.fail('maxS is set other than by a shortening cap: ' + line.strip())


LUM_K = 0.5         # DN_MV_VAR_K (dnMoverLumSteady: the 3x3 mean within half its standard error of the history)
LUM_REL = 0.02      # DN_MV_VAR_REL
VAR_CAP = 9.0       # DN_MV_VAR_CAP


def mover_replay(rule, moving, frames=400, n=64, noise=0.35, seed=3, iters=3, phi_l=4.0, base=4.0, shade=0.0):
    """THE COUNTDOWN'S NOISE (clean_dn_temporal.comp, RenderVKShadowFixMoverVariance). A row whose rays meet a mover:
    moving=False, a STILL self-shadow (a lit patch, a hard edge, a penumbra ramp) whose rays meet the avatar every frame --
    the countdown never ends; moving=True, an avatar's shadow (16 texels, level `shade`: 0 an umbra, 0.6 one light of
    several) sweeping at half a texel per frame over a lit floor (the countdown where the shadow is). The temporal step
    (the countdown withholds the deviation from HIST) and the variance-guided a-trous. The caster-evidence gate is taken
    to PASS everywhere -- its worst case, a moving shadow of one light of several within its 1 sigma. rule: 'off' (mode 0),
    'bee5' (the reverted floor: this frame's deviation into OUT behind that gate alone), 'out' (mode 1), 'hist' (mode 2).
    Returns (rms error vs the truth over the still row's penumbra, its mean frame-to-frame change, the a-trous's mean
    prefiltered variance there over the settled frames, the moving row's mean |output - truth| within 2 texels of its
    edges over the sweep)."""
    rng = random.Random(seed)
    frame = [0]

    def truth(i):
        if moving:
            centre = 10.0 + 0.5 * (frame[0] - 60) if frame[0] >= 60 else -40.0
            return shade if abs(i + 0.5 - centre) < 8.0 else 1.0
        if i < 32:
            return 1.0
        if i < 40:
            return 0.2
        return 0.2 + 0.6 * min(max((i - 40) / 16.0, 0.0), 1.0)

    def edge(i):
        centre = 10.0 + 0.5 * (frame[0] - 60)
        return frame[0] >= 60 and min(abs(i + 0.5 - (centre - 8.0)), abs(i + 0.5 - (centre + 8.0))) <= 2.0

    h, ln, hv, cd = [0.0] * n, [0.0] * n, [0.0] * n, [0] * n
    err2 = flick = 0.0
    cnt = 0
    sum_gv = 0.0
    edge_err, edge_n = 0.0, 0
    prev = None
    for t in range(frames):
        frame[0] = t
        tr = [truth(i) for i in range(n)]
        raw = [max(x * (1.0 + rng.gauss(0.0, noise)), 0.0) for x in tr]
        oc, ov = [0.0] * n, [0.0] * n
        for i in range(n):
            nb = [raw[q] for q in range(max(i - 1, 0), min(i + 2, n))]
            mm = sum(nb) / len(nb)
            sp = max(sum(x * x for x in nb) / len(nb) - mm * mm, 0.0)
            r = raw[i]
            have = ln[i] > 0.0
            length, hh, var0 = ln[i], h[i], hv[i]
            clamped = False
            if have:
                sig = max(math.sqrt(sp), math.sqrt(var0))
                lo, hi = max(mm - GAMMA * sig, 0.0), mm + GAMMA * sig
                hc = min(max(hh, lo), hi)
                clamped = hc != hh
                cut = abs(hc - hh) / max(hh + mm, 1e-4)
                if cut > 0.05:
                    length = min(length, 8.0 - 7.0 * min(max((cut - 0.05) * 3.0, 0.0), 1.0))
            else:
                hc, length = r, 0.0
            cd[i] = CD_LEN if (not moving or tr[i] < 0.99) else max(cd[i] - 1, 0)
            cap = max(min(hard_cap(base), CD_CAP), 1.0) if cd[i] > 0 else hard_cap(base)
            nd = min(length + 1.0, cap)
            a = 1.0 / max(nd, 1.0) if length > 0.0 else 1.0
            acc = hc + a * (r - hc)
            av = 1.0 / max(nd, VAR_LEN) if have else 1.0
            dev = 0.0 if cd[i] > 0 else length / (length + 1.0)
            # (dnMoverLumSteady + the clamp + the cap: modes 1 / 2)
            mvdev = 0.0
            if rule in ('out', 'hist') and cd[i] > 0 and have and length > 0.0 and not clamped \
                    and abs(mm - hh) <= max(LUM_K * math.sqrt(sp / len(nb)), LUM_REL * max(mm, hh)):
                mvdev = min(length / (length + 1.0) * (r - hc) ** 2, VAR_CAP * max(sp, var0))
            hist_dev = mvdev if rule == 'hist' else 0.0
            var = (1.0 - av) * (var0 + av * dev * (r - hc) ** 2 + av * hist_dev) if have else sp
            out_var = max(var, sp) if nd < 4.0 else var
            if rule == 'out':
                out_var = max(out_var, mvdev)
            if rule == 'bee5' and cd[i] > 0 and have and length > 0.0:
                out_var = max(out_var, length / (length + 1.0) * (r - hc) ** 2)
            h[i], ln[i], hv[i] = acc, nd, (var if have else 0.0)
            oc[i], ov[i] = acc, out_var
        c, v = oc[:], ov[:]
        for k in range(iters):
            sk = 1 << k
            gv = [(0.25 * v[max(i - 1, 0)] + 0.5 * v[i] + 0.25 * v[min(i + 1, n - 1)]) if k == 0 else v[i]
                  for i in range(n)]
            if k == 0 and t > frames // 2:
                sum_gv += sum(gv[i] for i in range(44, 56)) / 12.0
            nc, nv = c[:], v[:]
            for i in range(n):
                if gv[i] < 1e-6:
                    continue   # (idle: nothing to filter)
                phi = phi_l * math.sqrt(gv[i]) + 1e-4
                s = ws = vs = 0.0
                for o in range(-2, 3):
                    q = i + o * sk
                    if q < 0 or q >= n:
                        continue
                    w = B3[o + 2]
                    if o != 0:
                        w *= math.exp(-abs(c[q] - c[i]) / phi)
                    s += w * c[q]
                    vs += w * w * v[q]
                    ws += w
                nc[i], nv[i] = s / ws, vs / (ws * ws)
            c, v = nc, nv
        if moving:
            for i in range(n):
                if edge(i):
                    edge_err += abs(c[i] - tr[i])
                    edge_n += 1
        elif t > frames // 2:
            err2 += sum((c[i] - tr[i]) ** 2 for i in range(44, 56)) / 12.0
            if prev is not None:
                flick += sum(abs(c[i] - prev[i]) for i in range(44, 56)) / 12.0
            cnt += 1
        prev = c
    if moving:
        return 0.0, 0.0, 0.0, edge_err / max(edge_n, 1)
    return math.sqrt(err2 / cnt), flick / cnt, sum_gv / cnt, 0.0


class MoverCountdownNoise(unittest.TestCase):
    def test_still_self_shadow_was_unfiltered(self):
        # the bug: under a permanent countdown the a-trous's noise estimate over the penumbra collapses
        _, _, gv, _ = mover_replay('off', False)
        self.assertLess(gv, 1e-4, 'the fixture no longer starves the a-trous: it tests nothing (%.2e)' % gv)

    def test_still_self_shadow_is_filtered(self):
        # (measured: off 0.087 / 0.057; mode 1 0.074 / 0.052; mode 2 0.054 / 0.033 -- mode 1's one-frame estimate filters
        #  less than mode 2's smoothed one)
        e0, f0, _, _ = mover_replay('off', False)
        for rule, ke, kf in (('out', 0.9, 0.95), ('hist', 0.7, 0.65)):
            e1, f1, gv, _ = mover_replay(rule, False)
            self.assertGreater(gv, 1e-3, '%s: the countdown still starves the a-trous (%.2e)' % (rule, gv))
            self.assertLess(e1, ke * e0, '%s: penumbra error %.4f against %.4f off' % (rule, e1, e0))
            self.assertLess(f1, kf * f0, '%s: penumbra crawl %.4f against %.4f off' % (rule, f1, f0))

    def test_reverted_floor_smears_a_partial_moving_shadow(self):
        # WHY bee5efbe96 HURT: a moving shadow of one light of several passes the caster evidence's 1-sigma gate, and
        # the frame's deviation (the shadow arriving) opened the a-trous's stop on its edges -- they spread
        worst = 0.0
        for seed in (3, 7, 11):
            _, _, _, d0 = mover_replay('off', True, frames=151, noise=0.05, seed=seed, shade=0.6)
            _, _, _, d1 = mover_replay('bee5', True, frames=151, noise=0.05, seed=seed, shade=0.6)
            worst = max(worst, d1 / max(d0, 1e-9))
        self.assertGreater(worst, 1.1, 'the replay no longer shows the reverted floor softening the edge (%.3f)' % worst)

    def test_moving_shadow_edges_kept(self):
        for shade in (0.0, 0.6):
            for seed in (3, 7, 11):
                _, _, _, d0 = mover_replay('off', True, frames=151, noise=0.05, seed=seed, shade=shade)
                for rule in ('out', 'hist'):
                    _, _, _, d1 = mover_replay(rule, True, frames=151, noise=0.05, seed=seed, shade=shade)
                    # (~1 % from the noise a steady interior learned, read at the edge later; the reverted floor: +27 %)
                    self.assertLess(d1, 1.03 * d0 + 1e-6, 'shade %.1f seed %d %s: moving edge error %.4f against %.4f off'
                                    % (shade, seed, rule, d1, d0))


class SourceWiring(unittest.TestCase):
    def test_shader_rules(self):
        tmp = (SH / 'clean_dn_temporal.comp').read_text()
        atr = (SH / 'clean_dn_atrous.comp').read_text()
        com = (SH / 'clean_dn_common.glsl').read_text()
        self.assertNotIn('dnAdaptiveCap', tmp, 'the variance-driven lengthening is back: the settings are hard caps')
        self.assertNotIn('DN_ADAPT_CEIL', tmp)
        for needle in ('#define DN_EXACT_HIST 1.0', '#define DN_ADAPT_VAR_LEN 8.0',
                       'const float histVarD = (dnRadiance() && !have) ? 0.0 : varD,',
                       'if (moverCd > 0u && dnMoverCdCap() > 0.0)', 'maxD = min(maxD, DN_EXACT_HIST);',
                       'const bool exactNow = dnExactMot(mo.w);',
                       'const float storeVarD = dnRadiance() ? max(histVarD, recoveredD) : varD;',
                       '(min(moverCd, 15u) << DN_REC_CD_SHIFT)', 't.mvCd = max(t.mvCd, dnRecCd(g.rec));',
                       'if (exactNow) { varDo = 0.0; varSo = 0.0; }'):
            self.assertIn(needle, tmp)
        for needle in ('if (exactC && !dnExactMot(imageLoad(uDnMot, DN_MOT(q)).w)) continue;',
                       '&& !exactC && !moverC)', '(yD > 0.0 && !moverC)',
                       'const bool fbD = diffOn && lcD < min(DN_RAD_FEED_LEN, baseD - 1.0);',
                       'const bool fbS = specOn && wS > 0.0 && lcS < min(DN_RAD_FEED_LEN, baseS - 1.0);',
                       'dnYouthTo(lcD, baseD)'):
            self.assertIn(needle, atr)
        for needle in ('#define DN_T_MOVER_CD_SHIFT 14u', '#define DN_T_MOVER_CAP_SHIFT 18u',
                       '#define DN_REC_CD_SHIFT 28u',
                       'bool dnExactMot(uint w) { return dnRadiance() && (w & PDN_MOT_EVIDENCE_EXACT) != 0u; }'):
            self.assertIn(needle, com)
        # THE COUNTDOWN'S NOISE (RenderVKShadowFixMoverVariance): both spaces steady, the frame's deviation capped; mode 1
        # OUT only (after histVarD), mode 2 the recurrence -- added after the old expression (off: exactly as before)
        for needle in ('#define DN_MV_VAR_K 0.5', '#define DN_MV_VAR_REL 0.02', '#define DN_MV_VAR_CAP 9.0',
                       'const float hLumD0 = pdnLum(hD), hLumS0 = pdnLum(hS);',
                       'if (mvVarFix != 0u && moverCd > 0u && have && !exactNow && testV && pendV == 0u && !edgeV && !mvReveal',
                       '&& !(mvEvidence > 0.0) && !dnCasterDeviating(visMean, receiverHistory.visibility, noiseV))',
                       'if (!casterChangeD && !clampedD && infoD && lenD > 0.0 && nnD > 1.5',
                       '&& dnMoverLumSteady(pdnLum(mD), hLumD0, spVarD, nnD))',
                       'float varD = (1.0 - aVD) * (hvD + aVD * devD * (lD - muD) * (lD - muD) + aVD * histDevD);',
                       'if (mvVarFix == 1u) { varD = max(varD, mvDevD); varS = max(varS, mvDevS); }'):
            self.assertIn(needle, tmp)
        self.assertLess(tmp.index('const float histVarD = (dnRadiance() && !have) ? 0.0 : varD,'),
                        tmp.index('if (mvVarFix == 1u) { varD = max(varD, mvDevD); varS = max(varS, mvDevS); }'),
                        'mode 1 must reach OUT only: after HIST took its variance')
        self.assertIn('uint dnMoverVarFix() { return dnRadiance() ? (dp.ctl.y >> DN_T_MOVER_VAR_SHIFT) & 3u : 0u; }', com)
        # THE MOVER RECORD stays below bit 28 (its evidence: bits 24-27)
        self.assertIn('(e << 24u)', tmp)
        self.assertIn('min(uint(max(evidence, 0.0) / DN_MOVER_EV_UNIT + 0.5), 15u)', tmp)

    def test_host(self):
        dnc = (ROOT / 'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
        ps = (ROOT / 'indra/newview/vkgfx/llcleanprojstochvk.cpp').read_text()
        st = (ROOT / 'indra/newview/app_settings/settings.xml').read_text()
        self.assertIn('(p.radiance ? (std::min(p.moverCdLen, 15u) << 14u) | (std::min(p.moverCdCap, 15u) << 18u) : 0u)', dnc)
        self.assertIn('dp.moverCdLen = moverCountdownLen();', ps)
        self.assertIn('dp.moverCdCap = moverDynMaxHistory();', ps)
        self.assertIn('| (p.radiance ? (std::min(p.moverVarFix, 2u) << 22u) : 0u);', dnc)
        self.assertIn('"RenderVKShadowFixMoverVariance", 0U', ps)
        i = st.index('<key>RenderVKShadowFixMoverVariance</key>')
        blk = st[i:st.index('</map>', i)]
        self.assertIn('<key>Persist</key><integer>0</integer>', blk)
        self.assertIn('<key>Value</key><integer>0</integer>', blk)
        self.assertIn('"RenderVKDenoiseHistory", 4.f', ps)
        i = st.index('<key>RenderVKDenoiseHistory</key>')
        self.assertIn('<real>4.0</real>', st[i:st.index('</map>', i)])


if __name__ == '__main__':
    unittest.main()
