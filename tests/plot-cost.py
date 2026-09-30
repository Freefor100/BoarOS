#!/usr/bin/env python3
"""Render measured three-boot cost curves from the verified evidence archive."""
from pathlib import Path
from statistics import median
import importlib.util,json,sys
ROOT=Path(__file__).resolve().parents[1]
if len(sys.argv)!=3:raise SystemExit('usage: plot-cost.py archive.json output.png')
sys.path.insert(0,str(ROOT/'tests'))
spec=importlib.util.spec_from_file_location('evidence',ROOT/'tests/cost-evidence.py')
evidence=importlib.util.module_from_spec(spec);spec.loader.exec_module(evidence)
rows=evidence.unpack(json.loads(Path(sys.argv[1]).read_text()))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
def selected(case,on=1):
 return [r for r in rows if r['case']==case and r['cost_diagnostics']==on and r.get('platform','boaros')=='boaros' and not r.get('two_disks',False)]
def distribution(rs,name,metric=None):
 if metric:
  return [next(s['values'] for s in r['snapshots'] if s['name']==name)[metric] for r in rs]
 return [r['timings_ns'][name]/1e6 for r in rs]
def curve(ax,x,groups,label,color):
 y=[median(g) for g in groups];low=[m-min(g) for m,g in zip(y,groups)];high=[max(g)-m for m,g in zip(y,groups)]
 ax.errorbar(x,y,yerr=[low,high],label=label,color=color,marker='o',capsize=3,lw=1.4)
fig,axs=plt.subplots(2,2,figsize=(11,7),constrained_layout=True)
for resident,color in [(0,'#205493'),(16,'#b55613'),(64,'#158267')]:
 x=[16,64,256];curve(axs[0,0],x,[distribution(selected('mprotect',0),f'mprotect-{n}-{resident}') for n in x],f'{resident} MiB resident',color)
axs[0,0].set(title='Single-page permission window: diagnostics OFF',xlabel='Unrelated VMAs',ylabel='Guest elapsed (ms)')
x=[0,16,64];curve(axs[0,1],x,[distribution(selected('mprotect'),f'mprotect-16-{n}','foreground.mprotect_resident_visits.value') for n in x],'Resident visits','#205493')
curve(axs[0,1],x,[distribution(selected('mprotect'),f'mprotect-16-{n}','foreground.mprotect_pte_visits.value') for n in x],'PTE visits','#b55613')
axs[0,1].text(.04,.88,'Target PTE visits remain 6',transform=axs[0,1].transAxes,fontsize=9)
axs[0,1].set(title='Two permission calls on one target page',xlabel='Unrelated file resident data (MiB)',ylabel='Visits')
x=[0,32,128,256];groups=[]
for n in x:
 vals=[]
 for r in selected('deadline'):
  s=next(s['values'] for s in r['snapshots'] if s['name']==f'deadline-{n}-0-0')
  vals.append(max(s[l+'.deadline_visits.max'] for l in ('foreground','background')))
 groups.append(vals)
curve(axs[1,0],x,groups,'Four deadline tasks fixed','#205493')
axs[1,0].set(title='Largest blocked-queue pass in each window',xlabel='Additional tasks without deadlines',ylabel='Visited tasks')
x=[1,8,32]
for relation,color,label in [(1,'#b55613','Independent OFDs, same inode'),(2,'#158267','Different inodes')]:
 curve(axs[1,1],x,[distribution(selected('locking'),f'locking-{relation}-0-{n}','foreground.lock15_reblocks.value') for n in x],label,color)
axs[1,1].set(title='Actual write-operation lock reblocking',xlabel='Waiters',ylabel='Reblocks')
for ax in axs.flat:
 ax.grid(alpha=.2);ax.legend(fontsize=8);ax.spines[['right','top']].set_visible(False)
fig.suptitle('Three serial boots per point; median with min/max range. QEMU evidence.',fontsize=12)
fig.savefig(sys.argv[2],dpi=150)
print('matplotlib',matplotlib.__version__)
