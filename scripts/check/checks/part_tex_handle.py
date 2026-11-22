"""Regression guard for worn fire / any particle system flashing as a stack of white quads.

prepareParticles writes each GPU emitter's bindless texture index; clean_particle_sim.comp copies it onto EVERY
particle the emitter owns, every frame. It used to hand out index 0 (the built-in white) for any emitter slot it
had no texture record for, and handed out whatever slot the texture map named even when that slot had no
committed image yet (a fresh slot waiting for its upload ticket, a released one): writeTextureDescriptors writes
such a slot as white. One such frame turned the whole system white (white tint, alpha only from the texture).
The rule now lives in indra/newview/llcleanparttexhandle.h: the resolved slot once its image is committed, else
the emitter's last good slot for the same texture (still holding it), else 0xFFFFFFFF (not drawn) -- but only
for kPartTexStuckFrames: a slot that never gets its image is handed out as before (88272f5f6c hid ~10 emitters
for good), and a null-texture emitter keeps the white default (LL's default image). The sim keeps a particle's
own texture when its emitter has none to hand out this frame.

  * the fixture (scripts/check/test_part_tex_handle.cpp, includes the header): no input state yields the white
    slot or a slot without a committed image; transients keep the last good slot; another texture's or a
    recycled slot is never borrowed;
  * source wiring: prepareParticles clears handles to kPartTexNone (not 0), decides every handle with
    cleanPartTexHandle against the committed-image test, no longer copies the cached index straight into the
    table; the sim only refreshes a particle's texture from a handle that is not 0xFFFFFFFF.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "part-tex-handle"
STAGE = "source"
DESCRIPTION = ("GPU particle emitters never hand their particles the white slot or a slot without a committed "
               "image (a transient keeps the last good texture; unresolved = not drawn)")
GUARDS = ("A particle system (worn fire) intermittently drawn as a stack of untextured white quads for a few frames "
          "-- every live particle of it at once.")


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _wiring(ctx):
    missing = []
    r = _read(ctx, "indra/newview/vkgfx/llcleanrenderervk.cpp")
    i = r.find("void LLCleanRendererVK::prepareParticles()")
    body = r[i:r.find("\n}\n", i)] if i >= 0 else ""
    if not body:
        return ["prepareParticles not found in llcleanrenderervk.cpp"]
    # Emitters without a texture record (null texture) keep the white default (LL's default image);
    # every textured emitter is decided by cleanPartTexHandle, and its cache rows must not skip them.
    if not re.search(r"tex_handle\[0\]\s*=\s*kPartTexWhite\s*;", body):
        missing.append("prepareParticles does not default null-texture emitters to kPartTexWhite (LL's default image)")
    if "PartTexWhy::Stuck" not in body or "partTexSlotWhy(" not in body:
        missing.append("prepareParticles does not count / log stuck slots (VKParticleTex: why a slot has no image)")
    if "cleanPartTexHandle(" not in body:
        missing.append("prepareParticles does not decide handles with cleanPartTexHandle (llcleanparttexhandle.h)")
    if re.search(r"tex_handle\[0\]\s*=\s*sh\.second", body):
        missing.append("prepareParticles still copies the cached slot straight into the emitter table")
    ready = re.search(r"auto slotReady = \[this\]\(uint32_t idx, uint64_t& key\) -> bool\s*\{(.*?)\};", body, re.S)
    if not ready or "!mTextures[idx].view" not in ready.group(1) or "mVkTexKey[idx]" not in ready.group(1):
        missing.append("prepareParticles' slot test no longer requires a committed view and reports the slot's key")
    h = _read(ctx, "indra/newview/vkgfx/llcleanrenderervk.h")
    if '#include "../llcleanparttexhandle.h"' not in h:
        missing.append("llcleanrenderervk.h does not include llcleanparttexhandle.h")
    sim = _read(ctx, "indra/vkclean/shaders/clean_particle_sim.comp")
    if not re.search(r"if \(own && em\.tex_handle\.x != 0xFFFFFFFFu\) parts\[i\]\.tex_handle\.x = em\.tex_handle\.x;", sim):
        missing.append("clean_particle_sim.comp refreshes a particle's texture from a 0xFFFFFFFF emitter handle "
                       "(the whole system vanishes for the frame)")
    return missing


def run(ctx, result):
    hdr = ctx.path("indra/newview/llcleanparttexhandle.h")
    if not os.path.isfile(hdr):
        result.fail("indra/newview/llcleanparttexhandle.h is missing: particle emitters can be handed the white slot again")
        return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: the handle fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    src = ctx.path("scripts/check/test_part_tex_handle.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_part_tex_handle")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(hdr), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "no emitter handle reads as white (fixture) + prepareParticles / sim wiring"
