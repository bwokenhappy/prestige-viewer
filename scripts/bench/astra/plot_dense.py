#!/usr/bin/env python3
"""CPU-only total-frame chart; whiskers show range of run means, not confidence intervals."""
import argparse
import json
import statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('evidence',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
runs={r['name']:r for r in map(json.loads,a.evidence.read_text().splitlines())}
# The mask figure deliberately uses one binary only; map-policy tables remain separate.
fig,ax=plt.subplots(figsize=(10,5.8),layout='constrained')
colors=['#687b89','#227ca3']
rows=[]
for scene in ['house','club','closed','visible']:
 off=[runs['mask_'+scene+'_'+n] for n in ['a1','a2']]
 on=[runs['mask_'+scene+'_b1']]
 if scene=='house':on.append(runs['mask_house_b2'])
 if scene=='visible':
  off.append(runs['mask_visible_a3']);on.append(runs['mask_visible_b2'])
 for key in ['source','capture_sha256','path_sha256','template_sha256','window','render']:
  assert all(r[key]==off[0][key] for r in off+on),(scene,key)
 avgs=[[r['timing']['avg'] for r in group] for group in [off,on]]
 means=list(map(statistics.mean,avgs));rows.append((scene,means[1]-means[0]))
 y=len(rows)-1
 for index,(values,mean) in enumerate(zip(avgs,means)):
  pos=y+(-.18 if index==0 else .18)
  ax.barh(pos,mean,height=.29,color=colors[index],label=['Mask off','Mask on'][index] if y==0 else None)
  ax.errorbar(mean,pos,xerr=[[mean-min(values)],[max(values)-mean]],fmt='none',ecolor='black',capsize=3)
  ax.text(mean+.15,pos,f'{mean:.3f} ms (n={len(values)})',va='center',fontsize=9)
ax.set_yticks(range(len(rows)),[{'house':'HOUSE*','club':'Club','closed':'128 lights, wall closed','visible':'128 lights, wall open'}[s]+f'\nΔ {d:+.3f} ms' for s,d in rows])
ax.invert_yaxis();ax.set_xlim(0,max(r['timing']['avg'] for r in runs.values() if r['name'].startswith('mask_') and 'warm' not in r['name'])+5)
ax.set_xlabel('Total GPU frame mean (ms); lower is faster')
ax.grid(axis='x',alpha=.2);ax.set_axisbelow(True);ax.legend(loc='lower right')
ax.set_title('RX7900XTX • optional residual point-light mask • renderer 57f164d27d\nSame-scene A/B, GI3 / ReSTIR / pass15 active, no frame generation')
fig.supxlabel('*HOUSE is truncated: timing only. Whiskers = range of run means, not confidence intervals.\nSynthetic controls have zero captured probes. NVIDIA DLSS/RR is not measured.',fontsize=9)
a.output.mkdir(parents=True,exist_ok=True)
fig.savefig(a.output/'amd-residual-mask.png',dpi=160)
svg=a.output/'amd-residual-mask.svg';fig.savefig(svg);plt.close(fig)
svg.write_text('\n'.join(line.rstrip() for line in svg.read_text().splitlines())+'\n')
