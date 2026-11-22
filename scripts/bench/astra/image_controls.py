"""CPU-only A/A and A/B still controls. Keep outputs private until images are reviewed.

Usage: image_controls.py ROOT A1 A2 B OUTPUT_DIRECTORY
Inputs are private NAME.still*.bin files; no log content is read.
"""
import argparse,importlib.util,json,hashlib
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
repo=Path(__file__).resolve().parents[3]
s=importlib.util.spec_from_file_location('still',repo/'scripts/bench/still_detail.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('a1');p.add_argument('a2');p.add_argument('b');p.add_argument('out',type=Path);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
result={'runs':[a.a1,a.a2,a.b],'units':'display luminance 0..255','keys':[]}
stills=sorted(a.root.glob(a.a1+'.still*.bin'))
assert stills, 'No stills for A1; refusing an empty comparison'
for f in stills:
 suffix=f.name[len(a.a1):];paths=[a.root/(n+suffix) for n in result['runs']]
 imgs=[m.load(str(x)) for x in paths]
 assert len({(x['w'],x['h'],x['frames']) for x in imgs})==1
 assert imgs[0]['frames']>=200
 edge=m.edge_mask(imgs[0]['lum'])
 row={'key':suffix,'extent':[imgs[0]['w'],imgs[0]['h']],'frames':imgs[0]['frames'],'sha256':[], 'noise':[m.noise_figs(x,edge) for x in imgs],'aa':m.compare(imgs[0],imgs[1]),'ab':m.compare(imgs[0],imgs[2])}
 for x in paths:
  with x.open('rb') as src:row['sha256'].append(hashlib.file_digest(src,'sha256').hexdigest())
 result['keys'].append(row)
 # Mean images are complete synthetic scenes, but keep sheets private until visually reviewed.
 cells=[]
 for label,x in zip(['A1 mean','A2 mean','B mean'],imgs):cells.append((label,np.clip(x['mean']*255,0,255).astype('uint8')))
 for label,i in [('A/A absolute luminance difference x8',1),('A/B absolute luminance difference x8',2)]:
  v=np.clip(np.abs(imgs[0]['lum']-imgs[i]['lum'])*8,0,255).astype('uint8');cells.append((label,np.repeat(v[:,:,None],3,axis=2)))
 v=np.clip(imgs[2]['std']*16,0,255).astype('uint8');cells.append(('B temporal std x16',np.repeat(v[:,:,None],3,axis=2)))
 width=640;height=round(imgs[0]['h']*width/imgs[0]['w']);sheet=Image.new('RGB',(width*3,(height+32)*2),(25,25,25));draw=ImageDraw.Draw(sheet)
 for i,(label,pixels) in enumerate(cells):
  x=(i%3)*width;y=(i//3)*(height+32);draw.text((x+8,y+8),label,fill='white');sheet.paste(Image.fromarray(pixels).resize((width,height),Image.Resampling.LANCZOS),(x,y+32))
 sheet.save(a.out/(suffix.removesuffix('.bin').lstrip('.')+'.png'))
 # Full-resolution worst-tile context: the same crop for A1/A2/B, no per-image normalization.
 selected=[next(t for t in row['ab']['worst_tiles'] if t['kind']==kind) for kind in ['err','lost','added']]
 crops=Image.new('RGB',(1152,1248),(25,25,25));cd=ImageDraw.Draw(crops)
 for ri,t in enumerate(selected):
  x0=min(max(t['x']+8-48,0),imgs[0]['w']-96);y0=min(max(t['y']+8-48,0),imgs[0]['h']-96)
  for ci,x in enumerate(imgs):
   pix=np.clip(x['mean'][y0:y0+96,x0:x0+96]*255,0,255).astype('uint8')
   cd.text((ci*384+5,ri*416+8),['A1','A2','B'][ci]+' '+t['kind']+f' ({x0},{y0})',fill='white')
   crops.paste(Image.fromarray(pix).resize((384,384),Image.Resampling.NEAREST),(ci*384,ri*416+32))
 crops.save(a.out/(suffix.removesuffix('.bin').lstrip('.')+'-crops.png'))
 print(suffix,'AAerr',row['aa']['err_mean'],'ABerr',row['ab']['err_mean'],'noise',[x['all']['mean_std'] for x in row['noise']],flush=True)
(a.out/'metrics.json').write_text(json.dumps(result,indent=2))
