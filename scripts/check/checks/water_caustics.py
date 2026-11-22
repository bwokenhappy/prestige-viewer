"""Caustic energy/bounds, translated stereo footprint and lighting/ABI contracts."""
import math
import random
import re
from types import SimpleNamespace as NS
import xml.etree.ElementTree as ET

NAME = "water-caustics"
STAGE = "source"
DESCRIPTION = "Water lens bounds, zero-mean symmetric compression, per-eye footprint and diffuse-only gates"
GUARDS = "Unbounded focus, origin-dependent VR filtering, dry/shaded glow and caustic work in non-base variants."
SHADERS = "indra/vkclean/shaders/"


class V:
    def __init__(self, *v):
        self.v = tuple(float(x) for a in v for x in (a.v if isinstance(a, V) else (a,)))

    def __getattr__(self, name):
        values = tuple(self.v["xyzw".index(x)] for x in name)
        return values[0] if len(values) == 1 else V(*values)

    def _op(self, other, op):
        return V(*(op(a, b) for a, b in zip(self.v, other.v if isinstance(other, V) else (other,) * len(self.v))))

    def __add__(self, other):
        return self._op(other, lambda a, b: a + b)

    def __sub__(self, other):
        return self._op(other, lambda a, b: a - b)

    def __mul__(self, other):
        return self._op(other, lambda a, b: a * b)

    __rmul__ = __mul__

    def __truediv__(self, other):
        return self._op(other, lambda a, b: a / b)


class Matrix:
    def __init__(self, columns):
        self.columns = columns

    def __getitem__(self, i):
        return self.columns[i]

    def __mul__(self, vector):
        return sum((self.columns[i] * vector.v[i] for i in range(1, 4)), self.columns[0] * vector.v[0])


def dot(a, b):
    return sum(x * y for x, y in zip(a.v, b.v))


def evaluate(src, fn, inputs):
    """Execute these straight-line production GLSL scalar/vector expressions, not a copied formula."""
    match = re.search(r"float " + fn + r"\([^)]*\)\s*\{([^}]+)\}", src)
    if not match:
        raise ValueError("missing production math function " + fn)
    env = dict(inputs, vec2=V, vec4=V, abs=abs, max=max, float=float, dot=dot,
               length=lambda v: math.sqrt(dot(v, v)))
    body = re.sub(r"//[^\n]*", "", match[1])
    for statement in body.split(";"):
        statement = statement.strip()
        if not statement:
            continue
        if statement.startswith("return "):
            return eval(statement[7:], {"__builtins__": {}}, env)
        decl = re.fullmatch(r"const (?:float|vec[234]) (\w+) = (.*)", statement, flags=re.S)
        if not decl:
            raise ValueError("unsupported production math: " + statement)
        expr = decl[2]
        if "?" in expr:
            condition, rest = expr.split("?", 1)
            yes, no = rest.split(":", 1)
            expr = f"({yes} if {condition} else {no})"
        env[decl[1]] = eval(expr, {"__builtins__": {}}, env)
    raise ValueError("no return in " + fn)


def run(ctx, result):
    def read(path):
        with open(ctx.path(path), encoding="utf-8") as f:
            return f.read()

    src = read(SHADERS + "clean_water_caustics.glsl")
    surface = read(SHADERS + "clean_surface_light.glsl")
    deferred = read(SHADERS + "clean_deferred_light.comp")
    sea = read(SHADERS + "clean_sea_state.glsl")
    renderer = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    try:
        rng = random.Random(1636)
        for _ in range(2048):
            lens = V(*(rng.uniform(-1, 1) * 10 ** rng.uniform(-5, 5) for i in range(3)))
            a = evaluate(src, "waterCausticFocus", {"lens": lens})
            b = evaluate(src, "waterCausticFocus", {"lens": lens * -1})
            assert math.isfinite(a) and abs(a) < 1.0, "focus escaped finite unit bound"
            assert abs(a + b) < 1e-10, "symmetric wave lens no longer approximately conserves mean energy"
        assert evaluate(src, "waterCausticFocus", {"lens": V(0, 0, 0)}) == 0.0
        # The inverse of a simple infinite/reverse perspective projection, then an
        # orthographic projection. h is eye relative; P includes a translated camera.
        for ortho in (False, True):
            inverse = Matrix([V(0.8, 0, 0, 0), V(0, 0.5, 0, 0),
                              V(0, 0, 0, 1), V(0, 0, -1, 0)] if not ortho else
                             [V(12, 0, 0, 0), V(0, 8, 0, 0), V(0, 0, -100, 0), V(0, 0, 0, 1)])
            for eye_x in (-0.032, 0.032):
                for depth in (2.0, 80.0, 800.0):
                    values = []
                    for origin in (V(0, 0, 0), V(4096, -8192, 2048)):
                        eye = origin + V(eye_x, 0, 1.7)
                        point = eye + V(0.24 * depth, -0.1 * depth, -depth)
                        value = evaluate(src, "waterCausticFootprint", {
                            "relative": point - eye, "CLEAN_FRAG_XY": V(2496, 864),
                            "deferInvVP": inverse, "g_draw_push": NS(width=3840, height=2160),
                            "linfo": NS(cam_fwd=NS(w=float(ortho)))})
                        expected = max(0.02, (24 / 3840 if ortho else 1.6 * depth / 3840),
                                       (16 / 2160 if ortho else depth / 2160))
                        assert abs(value - expected) < 1e-10, "incorrect projection footprint"
                        values.append(value)
                    assert abs(values[0] - values[1]) < 1e-10, "camera translation changed caustic filtering"
        # The Hessian is the derivative of the same filtered cosine height wave.
        curvature = re.search(r"s.curvature -= ([^;]+);", sea)[1]
        for _ in range(64):
            w = V(rng.uniform(-2, 2), rng.uniform(-2, 2), 0, 0)
            amplitude, phase, nf = rng.uniform(0, 2), rng.uniform(-math.pi, math.pi), rng.random()
            env = {"a": amplitude, "cs": math.cos(phase), "nf": nf, "w0": w, "vec3": V}
            analytical = eval(curvature, {"__builtins__": {}}, env) * -1
            step = 1e-3
            height = lambda x, y: amplitude * nf * math.cos(phase + w.x * x + w.y * y)
            xx = (height(step, 0) - 2 * height(0, 0) + height(-step, 0)) / step ** 2
            yy = (height(0, step) - 2 * height(0, 0) + height(0, -step)) / step ** 2
            xy = (height(step, step) - height(step, -step) - height(-step, step) + height(-step, -step)) / (4 * step ** 2)
            assert max(abs(x-y) for x,y in zip(analytical.v, (xx,xy,yy))) < 3e-5, "wave Hessian sign/scale drift"
        # No shader ABI growth; CPU upstream reserves floats 26/27 in both main and
        # water-options-1636. Renderer publishes this lane after the sea copy/flat clear.
        assert renderer.index('fillSeaInfo(si);') < renderer.index('si.sea[6][2] = s_caustics')
        assert 'ssr.sea[6].z < 0.5 || L.z <= 0.02 || !specSunOn()' in src
        assert 'litDbg(LDBG_NO_SUN_SHADOW) || ssr.rt_mask.x < -1.5' in src
        assert 'shadowDepth >= 0.8 * ssr.shadow_clip.w' in src
        assert src.index('ssr.sea[6].z < 0.5') < src.index('const SeaTexel field')
        assert src.index('if (depth <= 0.0 || depth >= 24.0)') < src.index('const vec2 n = waterCausticDetail')
        assert 'P.z + receiverError < ssr.sea[6].w' in src
        assert src.index('P.z + receiverError < ssr.sea[6].w') < src.index('const SeaTexel field')
        assert 'seaSurface(P.xy' not in src and 'heightBound' not in src
        assert 'si.sea[6][3] = si.sea[6][2] > 0.5f ? causticWetLower(si)' in renderer
        assert renderer.index('mCausticBounds.publishPlanes') < renderer.index('mInstRuns.clear()')
        assert 'if (!fullbright && sunWant && sunVis > 0.0)' in surface
        assert '#if DEFER_UBER || CLEAN_DEFER_PASS == 3 || CLEAN_DEFER_PASS == 7\n#include "clean_water_caustics.glsl"' in deferred
        assert 'WATER_CAUSTIC_DIFFUSE(diffPunc) + specPunc' in surface
        assert '#else\n#define WATER_CAUSTIC_DIFFUSE(value) value' in surface
        assert 'causticGain * specPunc' not in surface and 'sunlit *= causticGain' not in surface
        assert 's.curvature -= a * cs * nf * vec3(w0.x * w0.x, w0.x * w0.y, w0.y * w0.y)' in sea
        # Published toggle must be default on, persisted and a boolean.
        root = ET.fromstring(read("indra/newview/app_settings/settings.xml"))
        children = list(root.find("map"))
        index = next(i for i, e in enumerate(children) if e.tag == "key" and e.text == "RenderVKWaterCaustics")
        pairs = list(children[index + 1])
        config = {pairs[i].text: pairs[i + 1].text for i in range(0, len(pairs), 2)}
        assert config["Persist"] == "1" and config["Value"] == "1" and config["Type"] == "Boolean"
    except (AssertionError, ValueError, TypeError, NameError, AttributeError, StopIteration) as e:
        result.fail(str(e))
        return
    result.summary = "2048 signed lens pairs finite/bounded/zero mean; 64 wave Hessians; 24 translated-eye projection cases; diffuse/off/dry/ABI contracts"
