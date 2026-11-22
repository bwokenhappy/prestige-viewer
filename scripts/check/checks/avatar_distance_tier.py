"""Distance simplification must not allocate nearby avatars' places by pixel rank."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

NAME = 'avatar-distance-tier'
STAGE = 'source'
DESCRIPTION = 'per-avatar distance tier and default shadow retention'
GUARDS = '#1617: a nearby visible avatar loses detail and its cast shadow on camera zoom'


def run(ctx, result):
    avatar = Path(ctx.path('indra/newview/llvoavatar.cpp')).read_text()
    tune = Path(ctx.path('indra/newview/llvkautotune.cpp')).read_text()
    renderer = Path(ctx.path('indra/newview/vkgfx/llcleanrenderervk.cpp')).read_text()
    body = avatar.split('void LLVOAvatar::vkUpdateReducedTier', 1)[1].split('S32 LLVOAvatar::vkReducedRiggedLOD', 1)[0]
    caps = tune.split('void LLVKAutoTune::applyLLSideCaps()', 1)[1].split('void LLVKAutoTune::logDecision', 1)[0]
    if 'getAvatars' in caps or 'AutoTuneImpostorByDistEnabled' in caps:
        result.fail('Distance preference became a global count cap again')
    if 'const U32 cap = mNonImpCapTuner;' not in caps or 'cap < eff' not in caps:
        result.fail('Separate user / FPS count caps no longer apply')
    if 's_auto_dist_on' not in body or 'AV_RENDER_ONLY_SHOW_FRIENDS' not in body or 'STATE_STARTED' not in body:
        result.fail('Distance preference lost its enable / mode / session gates')
    match = re.search(r'want = (dist_vec_squared\(getPositionGlobal\(\), gAgentCamera.getCameraPositionGlobal\(\)\) > radius \* radius);', body)
    if not match:
        result.fail('Distance tier must use the avatar and camera in the same global space')
        return
    expression = match.group(1).replace('getPositionGlobal()', 'position').replace('gAgentCamera.getCameraPositionGlobal()', 'camera')
    code = compile(expression, 'production avatar-distance predicate', 'eval')
    def reduced(position, camera, radius):
        return eval(code, {'__builtins__': {}}, dict(position=position, camera=camera, radius=radius,
                    dist_vec_squared=lambda a,b:sum((x-y)**2 for x,y in zip(a,b))))
    # The old distance->count->rank mapping: three within range (self included),
    # four large distant avatars outrank the small nearby wearer. Even the rank
    # hysteresis (cap+1.5) sends that wearer to reduced. Direct distance cannot.
    areas = [(1e6, 0), (2e5, 90), (1.8e5, 100), (1.6e5, 110), (1.4e5, 120), (1000, 2), (900, 3)]
    count = sum(distance <= 64 for _,distance in areas)
    rank = next(i+1 for i,(_,distance) in enumerate(areas) if distance == 2)
    if not rank > count + 1.5:
        result.fail('Regression scenario no longer exposes the old rank error')
    for distance, expected in ((0,False),(2,False),(63.99,False),(64,False),(64.01,True),(120,True)):
        if reduced((1000+distance,2000,30),(1000,2000,30),64) != expected:
            result.fail('Wrong per-avatar tier at distance %s' % distance)
    settings = list(ET.parse(ctx.path('indra/newview/app_settings/settings.xml')).getroot().find('map'))
    for index in range(0,len(settings),2):
        if settings[index].text == 'RenderVKReducedAvatarShadows':
            row = list(settings[index+1])
            values = {row[k].text:row[k+1].text for k in range(0,len(row),2)}
            if values.get('Value') != '1' or values.get('Persist') != '1':
                result.fail('Reduced-avatar shadow default must stay enabled and user-controlled')
            break
    else:
        result.fail('Missing reduced-avatar shadow setting')
    if 's_shadows(gSavedSettings, "RenderVKReducedAvatarShadows", true)' not in renderer:
        result.fail('Renderer fallback differs from the enabled shadow default')
    result.summary = 'old nearby-rank failure reproduced; 6 global-distance cases; count caps and shadow default retained'
