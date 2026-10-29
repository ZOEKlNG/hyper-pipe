#!/usr/bin/env python3
"""Aggregate run-level capacity samples; no message-latency inference."""
import argparse,csv,statistics
from collections import defaultdict
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('input',nargs='?',default='results/handoff_results.csv');a=p.parse_args()
path=Path(a.input);rows=list(csv.DictReader(path.open()));groups=defaultdict(list)
keys=['mode','schema','tail_bytes','batch','queue_slots']
for row in rows:groups[tuple(row[k] for k in keys)].append(row)
summary=[]
for key,rs in sorted(groups.items()):
 r=dict(zip(keys,key));rates=[float(x['msg_per_s'])/1e6 for x in rs]
 r.update(repeats=len(rs),median_Mmsg_s=statistics.median(rates),min_Mmsg_s=min(rates),max_Mmsg_s=max(rates),
  median_worker_total_cpu_ns_per_msg=statistics.median(float(x['worker_total_cpu_ns_per_msg']) for x in rs),
  median_logger_total_cpu_ns_per_msg=statistics.median(float(x['logger_total_cpu_ns_per_msg']) for x in rs),
  worker_spin_polls_per_msg=statistics.median(int(x['worker_spin_polls'])/int(x['messages']) for x in rs),
  logger_spin_polls_per_msg=statistics.median(int(x['logger_spin_polls'])/int(x['messages']) for x in rs),
  wire_bytes_mean=statistics.mean(float(x['wire_bytes_mean']) for x in rs),
  min_run_s=min(float(x['elapsed_s']) for x in rs),errors=sum(int(x['errors']) for x in rs),dropped=sum(int(x['dropped']) for x in rs),
  ring_allocated_bytes=int(rs[0]['ring_allocated_bytes']),pb_object_space_bytes=int(rs[0]['pb_object_space_bytes']))
 summary.append(r)
out=path.with_name('handoff_summary.csv')
with out.open('w',newline='') as f:
 writer=csv.DictWriter(f,fieldnames=list(summary[0]));writer.writeheader();writer.writerows(summary)
print(out)
print('rows',len(rows),'groups',len(summary),'minimum_duration_s',min(float(x['elapsed_s']) for x in rows),'errors',sum(int(x['errors']) for x in rows))
for r in summary:
 if r['tail_bytes']=='256' and r['queue_slots']=='256':
  print(r['schema'],r['batch'],r['mode'],'median',round(r['median_Mmsg_s'],3),'range',[round(r['min_Mmsg_s'],3),round(r['max_Mmsg_s'],3)],'workerCPUtotal',round(r['median_worker_total_cpu_ns_per_msg'],1),'loggerCPUtotal',round(r['median_logger_total_cpu_ns_per_msg'],1))
