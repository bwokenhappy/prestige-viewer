"""Check actual AMD replay paths, emitting no raw log/scene metadata."""
import json,re,sys
from pathlib import Path
for arg in sys.argv[1:]:
 p=Path(arg);d=json.loads(p.read_text(encoding='utf-8-sig'));s=p.with_suffix('.log').read_text(encoding='utf-8-sig',errors='replace')
 assert d['ok'] and d['pipelines']['settled'] and d['overall']['unfocused_frames']==0
 assert d['capture_info']['textures_failed']==d['capture_info']['slots_failed']==0
 assert d['capture_info']['armatures']==81
 skins=re.findall(r'replay: injected skins (\d+) \((\d+) failed\)',s);assert skins and int(skins[-1][1])==0
 assert d['settings']['RenderVKRayTracing']==1
 assert 'starting anyway (this run may time a fallback variant)' not in s
 assert not re.search(r'GRAPHICS DEVICE LOST|VK_ERROR_DEVICE_LOST',s)
 gi=re.findall(r'ray-traced GI \(RenderVKGI 3\): ([^\r\n]+)',s);assert gi and re.search(r'\bACTIVE$',gi[-1])
 rs=re.findall(r'ReSTIR DI: ([^\r\n]+)',s);assert rs and re.search(r'; ACTIVE \(',rs[-1])
 n=d['overall']['gpu_ms']['n'];passes=d['overall']['passes'];coverage={}
 for name in ['c_gi_trace','c_defer_shadowvis','c_defer_rtrefl','c_defer_rtrefl_trace','c_mirror']:
  coverage[name]=sum(v['n'] for k,v in passes.items() if k.split('@')[0]==name);assert coverage[name]==n
 assert any('pstochxvis' in k for k in passes)
 assert max(b['at_s'] for b in d['pipelines']['batches'])<=d['pipelines']['timed_start_s']
 print(json.dumps({'run':p.stem,'timed_frames':n,'rt_coverage':coverage,'gi3_active':True,'restir_active':True,'capture_injection_failed':0}))
