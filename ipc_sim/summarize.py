#!/usr/bin/env python3
import csv,statistics,argparse
from collections import defaultdict
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('csv');a=p.parse_args()
rows=list(csv.DictReader(open(a.csv)))
groups=defaultdict(list)
for r in rows:groups[tuple(r[k] for k in ['model','construction','message_bytes','batch'])].append(r)
out=[]
for k,rs in groups.items():
 vals=[float(r['msg_per_s']) for r in rs]
 cpus=[float(r['total_cpu_ns_per_msg']) for r in rs]
 out.append(dict(model=k[0],construction=k[1],message_bytes=k[2],batch=k[3],repeats=len(vals),
  median_Mmsg_s=statistics.median(vals)/1e6,min_Mmsg_s=min(vals)/1e6,max_Mmsg_s=max(vals)/1e6,
  run_throughput_p10_Mmsg_s=float(np.percentile(vals,10))/1e6,
  run_throughput_p90_Mmsg_s=float(np.percentile(vals,90))/1e6,
  median_total_cpu_ns_per_msg=statistics.median(cpus),
  shortest_run_s=min(float(r['elapsed_s']) for r in rs),errors=sum(int(r['errors']) for r in rs)))
f=Path(a.csv).with_name('ipc_summary.csv')
with f.open('w',newline='') as h:
 w=csv.DictWriter(h,fieldnames=list(out[0]));w.writeheader();w.writerows(out)
print(f)
for r in out:print(r)
