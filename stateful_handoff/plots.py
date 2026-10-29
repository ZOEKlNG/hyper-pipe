#!/usr/bin/env python3
from pathlib import Path
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parent;out=root/'figures';out.mkdir(exist_ok=True)
def read(p):return list(csv.DictReader((root/p).open()))
micro=read('results/micro_summary.csv');proto=read('results/proto_summary.csv')
fig,ax=plt.subplots(figsize=(10,5))
labels={'M1_string_new':'M1 new strings','M2_string_reuse':'M2 string reuse','M3_slab':'M3 size classes','M4_arena256':'M4 arena 256B','M4_arena512':'M4 arena 512B','M4_arena2048':'M4 arena 2048B'}
lengths=[12,32,64,128,512]
for mode,label in labels.items():
 r=[next(x for x in micro if x['storage']==mode and x['handoff']=='H5_modules_copy' and x['field_length']==str(n) and x['modules']=='3' and x['demand']=='workflow_dense' and x['stage']=='worker_lifecycle_local') for n in lengths]
 y=[float(x['cpu_ns_median']) for x in r];low=[a-float(x['cpu_ns_minimum']) for a,x in zip(y,r)];high=[float(x['cpu_ns_maximum'])-a for a,x in zip(y,r)]
 ax.errorbar(lengths,y,yerr=[low,high],marker='o',capsize=3,label=label)
ax.set_xscale('log',base=2);ax.set_xticks(lengths,[str(n) for n in lengths]);ax.set_xlabel('Bytes per persistent field');ax.set_ylabel('Worker local lifecycle CPU ns/log');ax.set_title('H5, 4096 active sessions; median and observed five-run range');ax.grid(alpha=.2);ax.legend(ncol=2,fontsize=9);fig.tight_layout()
for ext in ['png','svg']:fig.savefig(out/f'storage_worker.{ext}',dpi=160)
plt.close(fig)
specs=[('P0_implicit_parent_clear','set_ptr_len','Implicit\nsetter'),('P0_implicit_parent_clear','mutable_assign','Implicit\nmutable'),('P2_explicit_parent_clear','set_ptr_len','Optional\nsetter'),('P2_explicit_parent_clear','mutable_assign','Optional\nmutable')]
r=[next(x for x in proto if x['scheme']==s and x['api']==a and x['pattern']=='alternate_tls_http_dns' and x['length']=='64') for s,a,_ in specs]
y=[float(x['cpu_ns_median']) for x in r];low=[a-float(x['cpu_ns_minimum']) for a,x in zip(y,r)];high=[float(x['cpu_ns_maximum'])-a for a,x in zip(y,r)]
fig,ax=plt.subplots(figsize=(8,4.8));bars=ax.bar([x[2] for x in specs],y,color=['#c46b61','#c69852','#6697ab','#438b74'],yerr=[low,high],capsize=5)
for bar,row in zip(bars,r):ax.text(bar.get_x()+bar.get_width()/2,bar.get_height()+9,f"{float(row['cpu_ns_median']):.1f} ns\n{float(row['alloc_calls_per_event']):.0f} new/event",ha='center',va='bottom',fontsize=10)
ax.set_ylim(0,max(y)*1.35);ax.set_ylabel('CPU ns/event');ax.set_title('Protobuf 3.21.12: presence and bytes API\n4 x 64B strings; alternate TLS/HTTP/DNS; parent Clear');ax.grid(axis='y',alpha=.2);fig.tight_layout()
for ext in ['png','svg']:fig.savefig(out/f'protobuf_api.{ext}',dpi=160)
plt.close(fig)
print('Wrote two PNG/SVG figures from summary CSV.')
