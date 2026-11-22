"""Private-input, sanitized-output regression evidence. Never emits raw logs."""
import argparse,hashlib,json,re,statistics
from pathlib import Path

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
def read_run(root,name):
    rp=root/(name+'.json');j=json.loads(rp.read_text(encoding='utf-8-sig'))
    identity=json.loads((root/(name+'.identity.json')).read_text(encoding='utf-8-sig'))
    log=(root/(name+'.log')).read_text(encoding='utf-8-sig',errors='replace')
    assert j.get('ok') and j.get('mode')=='replay',name+': failed replay'
    ci=j.get('capture_info',{})
    assert ci.get('slots_failed')==0 and ci.get('textures_failed')==0,name+': failed capture injection'
    if ci.get('rigged_rows',0):
        skins=re.findall(r'replay: injected skins (\d+) \((\d+) failed\)',log)
        assert skins and int(skins[-1][1])==0,name+': rigged injection missing or failed'
    assert not j['overall'].get('unfocused_frames',0),name+': unfocused frames'
    assert 'starting anyway (this run may time a fallback variant)' not in log,name+': pipeline deadline'
    assert 'replay: pipelines ready' in log,name+': readiness absent'
    assert not re.search(r'GRAPHICS DEVICE LOST|VK_ERROR_DEVICE_LOST',log),name+': device loss'
    modes=re.findall(r'VKDeferSplit: deferred lighting runs ([^\r\n]+)',log)
    assert modes and modes[-1].startswith('SPLIT'),name+': split path absent'
    if int(identity['settings']['RenderVKRayTracing']):
        gi=re.findall(r'ray-traced GI \(RenderVKGI 3\): ([^\r\n]+)',log)
        assert gi and gi[-1].endswith('ACTIVE'),name+': GI inactive'
        rs=re.findall(r'ReSTIR DI: ([^\r\n]+)',log)
        assert rs and 'ACTIVE' in rs[-1],name+': stochastic/RT DI inactive'
        ad=re.findall(r'RenderVKProjStochasticAdaptive (\d+): ([^\r\n]+)',log)
        requested=int(identity['settings']['RenderVKProjStochasticAdaptive'])
        if requested:
            assert ad and int(ad[-1][0])==requested and ad[-1][1].startswith('running'),name+': adaptive path differs'
            assert any('pstochxvis' in k for k in j['overall']['passes']),name+': extra VIS timer absent'
        else:
            assert 'RenderVKProjStochasticAdaptive=0' in log,name+': effective adaptive0 absent'
            assert not any('pstoch' in k for k in j['overall']['passes']),name+': extra passes unexpectedly ran'
        assert any('shadowvis' in k for k in j['overall']['passes']),name+': base pass15 VIS absent'
        frame_n=j['overall']['gpu_ms']['n']
        for marker in ('c_gi_trace','c_defer_shadowvis'):
            coverage=sum(v['n'] for k,v in j['overall']['passes'].items() if k.split('@')[0]==marker)
            assert coverage==frame_n,name+': partial path '+marker+' '+str(coverage)+'/'+str(frame_n)
    else:
        assert int(j.get('settings',{}).get('RenderVKRayTracing',-1))==0,name+': report RT setting differs'
        # Linux omits default-valued GI=0 from its non-default settings line.
        # Require the explicit startup application as well as no active GI/trace work.
        assert 'RenderVKGI=0' in log or 'Setting Global.RenderVKGI to 0' in log,name+': GI0 application absent'
        assert not re.search(r'ReSTIR DI: [^\r\n]*ACTIVE|ray-traced GI \(RenderVKGI 3\): [^\r\n]*ACTIVE',log),name+': unexpected active RT signal'
        assert not any(k.split('@')[0] in ('c_gi_trace','c_defer_shadowvis','c_defer_rtrefl_trace') for k in j['overall']['passes']),name+': RT trace unexpectedly ran'
        marker='defer_base' if 'FEATURE SPLIT' in modes[-1] else 'defer_refl'
        coverage=sum(v['n'] for k,v in j['overall']['passes'].items() if k.split('@')[0][2:]==marker)
        assert coverage==j['overall']['gpu_ms']['n'],name+': incomplete non-RT split coverage'
    if int(identity['settings'].get('RenderVKUpscalerFsr4',0)):
        assert 'FSR 4 upscaler made: 4.1.2' in log,name+': FSR4 runtime absent'
    else:
        assert 'FSR 4 upscaler made:' not in log,name+': unexpected FSR4 runtime'
    assert j.get('pipelines',{}).get('settled',True),name+': unsettled pipelines'
    dims=re.findall(r'VKDeferSplit: intermediates (\d+)x(\d+)',log)
    assert dims,name+': extent absent'
    assert int(identity['settings']['RenderVKFrameGen'])==0,name+': FG requested'
    # Retain numeric activity only, excluding object/local identifiers and log paths.
    activity=[]
    for row in re.findall(r'GI delta totals: ([^\r\n]+)',log):
        metrics={k:int(v) for k,v in re.findall(r'\b(unknown_pose|bad_bounds|material_residency|geometry_overflow|store_reset|agent_shift|dropped_frame|cache_restart_headers|faint|moved|emission|texture|appear|release|full_queue|local_spheres|header_hard|header_soft)=(\d+)',row)}
        h=re.search(r'headers_with_spheres=(\d+)/(\d+)',row)
        s=re.search(r'spheres (\d+) in (\d+) frames',row)
        radius=re.search(r'largest header sphere: hard ([\d.]+) m, soft ([\d.]+) m',row)
        if h:metrics['headers_with_spheres'],metrics['headers']=map(int,h.groups())
        if s:metrics['spheres'],metrics['frames_with_emitter_spheres']=map(int,s.groups())
        if radius:metrics['hard_radius_max'],metrics['soft_radius_max']=map(float,radius.groups())
        activity.append(metrics)
    return dict(name=name,source=identity['source'],executable_sha256=identity['executable_sha256'],
        report_sha256=digest(rp),capture_sha256=identity['capture_manifest_sha256'],path_sha256=identity['path_sha256'],
        template_sha256=identity['settings_template_sha256'],settings=identity['settings'],
        capture_counts={k:ci[k] for k in ('rows','slots','slots_failed','materials','textures','textures_builtin',
            'textures_failed','skins','armatures','rigged_rows','pose_samples','track_seconds') if k in ci},
        window=j['window'],render=[int(x) for x in dims[-1]],seed=j['seed'],fixed_dt=j['fixed_dt'],loops=j['loops'],
        path=modes[-1],gpu=j['gpu'],driver=j['driver'],
        device_info={k:v.strip() for k,v in re.findall(r'\bGPU_(VENDOR|RENDERER|API|DRIVER):[ \t]*([^\r\n]+)',log)},
        vram={k:j.get('vram',{}).get(k) for k in ('valid','usage_mb_end','usage_mb_peak','budget_mb','capacity_mb','ladder_rung')},
        radiance_point_mask_counts=[int(n) for n in re.findall(r'ASTRA radiance-owned point mask populated: points=(\d+)',log)],
        timing=j['overall']['gpu_ms'],cpu_frame=j['overall']['frame_ms'],
        segments={x['name']:{'gpu_ms':x['gpu_ms'],'passes':x['passes']} for x in j['segments']},
        passes=j['overall']['passes'],gi_activity=activity,
        adaptive=re.findall(r'RenderVKProjStochasticAdaptive \d+: (?:running|refused|off)[^\r\n]*',log),
        lag_active='one-frame-latency GI (#978, RenderVKGILatencyFrame): depth snapshot' in log,
        stats_lvc_present='LVC stats (' in log,
        stochastic_route=re.findall(r'stochastic signal: [^\r\n]*',log)[-1:],
        gi_extent=re.findall(r'ray-traced GI: [^\r\n]*',log)[-1:],
        lvc_aggregate=[line.split('; INVARIANT BREAKS')[0].split('; top lights')[0]
            for line in re.findall(r'LVC stats \(RenderVKReSTIROcclusionCull 3\)[^\r\n]*',log)],
        avatar_empty=[dict(option=int(a),skip=int(b),faces=int(c),published_empty=int(d),pending_upload=int(e))
            for a,b,c,d,e in re.findall(r'empty-set option=(\d+) skip=(\d+) conservative_faces=(\d+) published_empty=(\d+) pending_full_upload=(\d+)',log)])

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('names',nargs='+');a=p.parse_args()
    runs=[read_run(a.directory,n) for n in a.names]
    for key in ('capture_sha256','path_sha256','template_sha256','window','render','seed','fixed_dt','loops'):
        assert all(r[key]==runs[0][key] for r in runs),'mismatched '+key
    print(json.dumps(runs,indent=2))
