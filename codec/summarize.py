#!/usr/bin/env python3
import csv, statistics, pathlib, sys
from collections import defaultdict
root=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else '/workspace/work/results')
for stem,key,value in [('flat_direct',('codec','payload_bytes','pattern','operation'),'ns_per_op'),('compression_reuse',('codec','record_bytes','batch','pattern','operation'),'ns_per_record'),('codec',('codec','payload_bytes','pattern','operation'),'ns_per_op'),('compression',('codec','record_bytes','batch','pattern','operation'),'ns_per_record')]:
    rows=list(csv.DictReader((root/(stem+'_raw.csv')).open()))
    groups=defaultdict(list)
    for r in rows:groups[tuple(r[k] for k in key)].append(r)
    with (root/(stem+'_summary.csv')).open('w',newline='') as f:
        fields=list(key)+['repeats','ns_median','ns_min','ns_max']
        if stem in ('codec','flat_direct'): fields+=['wire_bytes_mean','Mops_per_s_from_median']
        else:fields+=['input_bytes_batch','output_bytes_batch_mean','output_input_ratio']
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader()
        for k,rs in groups.items():
            vals=[float(r[value]) for r in rs];med=statistics.median(vals)
            o=dict(zip(key,k));o.update(repeats=len(vals),ns_median=med,ns_min=min(vals),ns_max=max(vals))
            if stem in ('codec','flat_direct'):o.update(wire_bytes_mean=rs[0]['wire_bytes_mean'],Mops_per_s_from_median=1000/med)
            else:o.update(input_bytes_batch=rs[0]['input_bytes_batch'],output_bytes_batch_mean=rs[0]['output_bytes_batch_mean'],output_input_ratio=float(rs[0]['output_bytes_batch_mean'])/float(rs[0]['input_bytes_batch']))
            w.writerow(o)
    print(stem,len(rows),'raw rows',len(groups),'summary rows')
