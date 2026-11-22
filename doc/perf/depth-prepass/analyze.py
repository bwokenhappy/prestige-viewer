#!/usr/bin/env python3
"""Validate private replays and emit numeric, scrubbed depth-prepass evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

PRE_RASTER = {'g_prepass', 'g_prepass_p2', 'g_pre_static', 'g_pre_rigged',
              'g_pre_cl_begin', 'g_pre_cl_end', 'g_pre_pre_snow', 'g_pre_snow', 'g_vr_vismask'}
PRE_SETUP = {'g_pass1_begin', 'c_occl_split', 'c_occl_seed', 'c_hiz', 'c_occl_p2'}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def read_run(root, name):
    path = root / (name + '.json')
    j = json.loads(path.read_text(encoding='utf-8-sig'))
    ident = json.loads((root / (name + '.identity.json')).read_text(encoding='utf-8-sig'))
    log = (root / (name + '.log')).read_text(encoding='utf-8-sig', errors='replace')
    assert re.fullmatch(r'[0-9a-fA-F]{40}', ident['source']), name + ': exact source SHA missing'
    assert j.get('ok') and j.get('mode') == 'replay', name + ': incomplete replay'
    assert not j['overall'].get('unfocused_frames', 0), name + ': unfocused frames'
    assert j.get('pipelines', {}).get('settled'), name + ': unsettled pipelines'
    assert 'replay: pipelines ready' in log, name + ': missing readiness'
    assert 'starting anyway (this run may time a fallback variant)' not in log, name + ': fallback timed'
    for bad in ('VK_ERROR_DEVICE_LOST', 'GRAPHICS DEVICE LOST', 'Validation Error', 'VUID-', 'SYNC-HAZARD', 'Segmentation fault'):
        assert bad not in log, name + ': failure marker ' + bad
    ci = j.get('capture_info', {})
    assert ci.get('slots_failed') == 0 and ci.get('textures_failed') == 0, name + ': capture injection failure'
    if ci.get('rigged_rows', 0):
        skins = re.findall(r'replay: injected skins (\d+) \((\d+) failed\)', log)
        assert skins and int(skins[-1][1]) == 0, name + ': rigged injection failed'
    assert not sum(x.get('failed', 0) for x in j.get('pipelines', {}).get('batches', [])), name + ': pipeline compilation failed'
    cfg = ident['settings']
    assert int(cfg['RenderVKFrameGen']) == 0
    assert int(cfg['RenderVKOcclusion2Phase']) == 2
    assert int(cfg['RenderVKBlendOpaqueGBuffer']) == 1
    n = j['overall']['gpu_ms']['n']
    assert n > 100, name + ': insufficient GPU samples'
    passes = j['overall']['passes']
    def count(key):
        return sum(v['n'] for k, v in passes.items() if k.split('@')[0] == key)
    def weighted_where(predicate):
        return sum(v.get('avg', 0) * v['n'] / n for k, v in passes.items() if predicate(k))
    def weighted(keys):
        return weighted_where(lambda k: k.split('@')[0] in keys)
    assert count('g_prepass') == n, name + ': prepass sample coverage differs'
    ao_coverage = sum(count(k) for k in ('g_ssao','g_gtao','g_gtao_half','c_ssao','c_gtao','c_gtao_half')) / n
    if int(cfg.get('RenderDeferredSSAO',0)):
        assert ao_coverage > .95, name + ': requested AO did not run through measured interval'
    rt = int(cfg['RenderVKRayTracing'])
    assert int(j['settings']['RenderVKRayTracing']) == rt
    if rt:
        assert re.search(r'ray-traced GI \(RenderVKGI 3\): [^\r\n]*ACTIVE', log), name + ': GI3 inactive'
        assert re.search(r'ReSTIR DI: [^\r\n]*ACTIVE', log), name + ': ReSTIR inactive'
        for suffix in ('gi_trace', 'defer_shadowvis'):
            assert count('c_' + suffix) + count('g_' + suffix) == n, name + ': partial RT coverage ' + suffix
    else:
        assert not re.search(r'ReSTIR DI: [^\r\n]*ACTIVE|ray-traced GI \(RenderVKGI 3\): [^\r\n]*ACTIVE', log)
        assert not any(count(k) for k in ('c_gi_trace', 'c_defer_shadowvis', 'c_defer_rtrefl_trace'))
    assert 'FSR 4 upscaler made:' not in log, name + ': unexpected FSR4'
    dims = re.findall(r'VKDeferSplit: intermediates (\d+)x(\d+)', log)
    if not dims:
        dims = re.findall(r'FSR 3\.1: resources (\d+)x(\d+) ->', log)
    assert dims, name + ': actual extent absent'
    routes = re.findall(r'VKDeferSplit: deferred lighting runs ([^\r\n]+)', log)
    assert routes, name + ': lighting route absent'
    lighting_path = 'FEATURE_SPLIT' if routes[-1].startswith('SPLIT + FEATURE') else 'SPLIT' if routes[-1].startswith('SPLIT') else 'UBER'
    if int(cfg.get('RenderVKDeferredSplit',0)) == 1:
        assert lighting_path != 'UBER', name + ': requested split timed the startup uber fallback'
    if lighting_path == 'UBER':
        assert count('g_defer_light') == n, name + ': mixed or incomplete uber route'
    else:
        suffix = 'defer_base' if lighting_path == 'FEATURE_SPLIT' else 'defer_refl'
        assert count('g_'+suffix)+count('c_'+suffix) == n, name + ': mixed or incomplete split route'
    groups = {
        'pre_raster_ms': weighted(PRE_RASTER),
        'pre_setup_ms': weighted(PRE_SETUP),
        'pre_chain_ms': weighted(PRE_RASTER | PRE_SETUP),
        'gbuffer_ms': weighted({k.split('@')[0] for k in passes if k.startswith('g_gbuf')}),
        'probes_ms': weighted({k.split('@')[0] for k in passes if k.startswith(('pr_','g_probe','c_probe'))}),
        'skin_ms': weighted({k.split('@')[0] for k in passes if 'preskin' in k or 'rigweld' in k}),
        'deferred_graphics_ms': weighted_where(lambda k: k.startswith(('g_defer','c_defer')) and not k.endswith('@async')),
        'deferred_async_ms': weighted_where(lambda k: k.startswith(('g_defer','c_defer')) and k.endswith('@async')),
        'alpha_ms': weighted({k.split('@')[0] for k in passes if k.startswith('g_p2') or k.startswith('g_alpha')}),
        'ao_graphics_ms': weighted_where(lambda k: k.startswith(('g_ao','c_ao','g_gtao','c_gtao','g_ssao','c_ssao')) and not k.endswith('@async')),
        'ao_async_ms': weighted_where(lambda k: k.startswith(('g_ao','c_ao','g_gtao','c_gtao','g_ssao','c_ssao')) and k.endswith('@async')),
    }
    return dict(name=name, source=ident['source'], executable_sha256=ident['executable_sha256'].lower(),
                report_sha256=sha(path), capture_sha256=ident['capture_manifest_sha256'].lower(),
                path_sha256=ident['path_sha256'].lower(), template_sha256=ident['settings_template_sha256'].lower(),
                settings={k:v for k,v in cfg.items() if k not in ('CacheLocation','NewCacheLocation')},
                capture_counts={k:ci[k] for k in ('rows','slots','slots_failed','materials','textures','textures_builtin',
                                'textures_failed','skins','armatures','rigged_rows','pose_samples') if k in ci},
                output=j['window'], render=list(map(int,dims[-1])), seed=j['seed'], fixed_dt=j['fixed_dt'],
                gpu=j['gpu'], driver=j['driver'], lighting_path=lighting_path, gpu_ms=j['overall']['gpu_ms'], frame_ms=j['overall']['frame_ms'],
                groups=groups, passes=passes,
                phase2_coverage=count('g_prepass_p2')/n, ao_coverage=ao_coverage,
                adaptive_stand_down='two-phase occlusion (adaptive): standing down' in log,
                adaptive_resume='two-phase occlusion (adaptive): re-enabled' in log,
                pipeline_failures=sum(x.get('failed',0) for x in j.get('pipelines',{}).get('batches',[])),
                vram={k:j.get('vram',{}).get(k) for k in ('valid','usage_mb_end','usage_mb_peak','budget_mb','capacity_mb','ladder_rung')})

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('output',type=Path)
    p.add_argument('--require-complete',action='store_true')
    a=p.parse_args()
    accepted=[]; rejected=[]
    for f in sorted(a.directory.glob('*.identity.json')):
        name=f.name.removesuffix('.identity.json')
        if not (a.directory/(name+'.json')).exists():
            continue
        try:
            accepted.append(read_run(a.directory,name))
        except (AssertionError,KeyError,ValueError) as exc:
            rejected.append({'name':name,'reason':str(exc)})
    cases={}
    for r in accepted:
        key=r['name'].rsplit('_',1)[0]
        cases.setdefault(key,[]).append(r)
    summary={}
    for key,rows in cases.items():
        ref=rows[0]
        for field in ('capture_sha256','path_sha256','template_sha256','output','render','seed','fixed_dt','settings','gpu','driver','lighting_path'):
            assert all(x[field]==ref[field] for x in rows), key+': mismatch '+field
        arms={arm:[r for r in rows if r['name'].rsplit('_',1)[1][0]==arm] for arm in ('a','b')}
        if a.require_complete:
            assert all(len(v)>=3 for v in arms.values()), key + ': fewer than 3 runs/arm'
            for arm, rs in arms.items():
                for field in ('source','executable_sha256'):
                    assert len({r[field] for r in rs})==1, key + ': mixed '+arm+' '+field
        case={}
        metrics=['gpu_ms','frame_ms',*ref['groups']]
        for metric in metrics:
            values={arm:[r[metric]['avg'] if metric in ('gpu_ms','frame_ms') else r['groups'][metric] for r in rs] for arm,rs in arms.items()}
            case[metric]={arm:dict(runs=len(v),means=v,mean=statistics.mean(v),minimum=min(v),maximum=max(v),spread=max(v)-min(v)) for arm,v in values.items() if v}
        summary[key]=case
    if a.require_complete:
        assert accepted and not rejected, 'missing accepted data or rejected runs: ' + str(rejected)
    a.output.write_text(json.dumps(dict(accepted=accepted,rejected=rejected,summary=summary),indent=2))
    print('accepted',len(accepted),'rejected',len(rejected),'cases',len(cases))
