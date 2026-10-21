import csv, sys, statistics
from pathlib import Path
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).parent
files=[root/'sbe_validated_raw.csv',root/'sbe_trusted_raw.csv']
rows=[]
for path in files:
    rows.extend(csv.DictReader(path.open()))
groups={}
for row in rows:
    key=tuple(row[x] for x in ['codec','payload_bytes','pattern','operation','wire_bytes_mean'])
    groups.setdefault(key,[]).append(float(row['ns_per_op']))
out=root/'sbe_summary.csv'
with out.open('w') as f:
    w=csv.writer(f); w.writerow(['codec','payload_bytes','pattern','operation','wire_bytes_mean','repeats','ns_median','ns_min','ns_max','million_ops_s'])
    for key,vals in sorted(groups.items()):
        assert len(vals)==5
        med=statistics.median(vals)
        w.writerow([*key,len(vals),med,min(vals),max(vals),1000/med])
print(out)
