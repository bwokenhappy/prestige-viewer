#!/usr/bin/env python3
"""Derive private constant-camera controls without publishing scene coordinates."""
import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('platform', choices=['linux', 'amd'])
p.add_argument('inputs', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--candidate', default='pending')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = '/home/trash/slipstream/sol-depth-prepass-ae1f063eb7' if a.platform == 'linux' else 'C:/sltest/sol-depth-prepass'
captures = ({s: '/home/trash/slipstream/astra/capture-' + s for s in ('house', 'club', 'street')}
            if a.platform == 'linux' else {'house': 'C:/sltest/rtperf/cap/owner_now',
                                          'club': 'C:/sltest/rtperf/cap/nightclub-skins4'})
settings = dict(AutoTuneLock=1, ProbeHardwareOnStartup=0, RenderVKGpuTimers=1, RenderVKGpuSubMarks=1,
                BenchOverlay=0, BenchFreezeStills=1, RenderVKNoiseStats=0, RenderVKBench=0,
                RenderVKFrameGen=0, RenderVSyncEnable=0, RenderFPSLimit=0,
                FullScreen=0, WindowMaximized=0, WindowWidth=1920, WindowHeight=1080,
                RenderVKUpscaler=1, RenderVKUpscalerFsr4=0, RenderVKAntiLag=0,
                RenderVKOcclusion2Phase=2, RenderVKBlendOpaqueGBuffer=1,
                RenderVKDepthOnlyVS=1, RenderVKPrepassDepthOnlyDeferred=1,
                RenderFarClip=512, RenderVKDeferredSplit=1, AudioAcoustics=0,
                RenderDeferredSSAO=1, RenderVKProjShadowMax=16, RenderVKRiggedScreenLOD=0)
settings.update(CacheLocation=root + '/cache', NewCacheLocation=root + '/cache')
paths = {}
for scene in captures:
    original = json.loads((a.inputs / (scene + '.json')).read_text(encoding='utf-8-sig'))
    pose = dict(original['keys'][0])
    pose.update(t=0, segment='held', still=240 if scene != 'house' else 0)
    tail = dict(pose, t=4, still=0)
    path = {'format': 'slipstream-bench-path', 'version': 1, 'keys': [pose, tail]}
    name = scene + '-held.json'
    (a.output / name).write_text(json.dumps(path))
    paths[scene] = root + '/paths/' + name
cases = [(s, rt) for s in captures for rt in ([False] if a.platform == 'linux' else [False, True])]
runs = []
# Begin with A/A controls on all cases while the candidate is being built. Then
# each B has adjacent A references in its own case, and >=3 runs of each arm.
for arms in [('a1', 'a2'), ('b1', 'a3', 'b2', 'a4', 'b3', 'a5')]:
    for scene, rt in cases:
        for arm in arms:
            cfg = dict(settings, BenchStillRaw=int(scene != 'house'),
                       RenderVKRayTracing=int(rt), RenderVKGI=3 if rt else 0,
                       RenderVKRTReflections=int(rt), RenderVKSunShadows=2 if rt else 1,
                       RenderVKProjectorShadows=2 if rt else 1)
            runs.append(dict(name=f'{scene}_{"rt" if rt else "nrt"}_{arm}',
                             package=root + '/package-' + arm[0],
                             source='ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a' if arm[0] == 'a' else a.candidate,
                             capture=captures[scene], path=paths[scene], settings=cfg))
(a.output / 'runs.json').write_text(json.dumps(runs, indent=2))
print('runs', len(runs), 'private paths derived', len(paths))
