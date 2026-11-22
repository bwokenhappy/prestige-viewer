"""Regression guard for THE CONVERGENCE RULE (clean_dn_temporal.comp, local-light radiance denoiser)."""
import importlib.util
import io
import unittest

NAME = 'pdn-history-exact'
STAGE = 'source'
DESCRIPTION = ('radiance denoiser history follows the input: exact texels take this frame, moving casters the sun\'s '
               'cap + countdown in every mover mode, base 4 lengthened only where the temporal variance is high')
GUARDS = ('A moving avatar shadow trailing / smearing behind its motion under RenderVKShadowMoverMask 0, exact shadow '
          'edges drifting from the raw truth, floor-gap specular filling in over time, and static noisy light no '
          'longer converging.')


def run(ctx, result):
    spec = importlib.util.spec_from_file_location('pdn_history_exact_fixtures',
                                                  ctx.path('scripts/check/test_pdn_history_exact.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    output = io.StringIO()
    checked = unittest.TextTestRunner(stream=output).run(unittest.defaultTestLoader.loadTestsFromModule(mod))
    if not checked.wasSuccessful():
        result.fail(output.getvalue())
    result.summary = f'{checked.testsRun} replay/source fixtures'
