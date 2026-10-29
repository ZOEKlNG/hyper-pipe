#!/usr/bin/env python3
"""Independently launched, interleaved repeat check for decision-critical pairs."""
from pathlib import Path
from datetime import datetime,timezone
import csv,hashlib,json,random,subprocess,time
root=Path(__file__).resolve().parent;out=root/'results';configs=json.loads((root/'matrix.json').read_text())
wanted=[]
def add(s,h,length=64,mods=3,sparse=0,life=3,cpu=1,zero=0):
 if h==0:life=0
 args=[s,h,life,length,mods,sparse,cpu,zero]
 c=next(x for x in configs if x['args']==args)
 if c not in wanted:wanted.append(c)
for h in range(6):
 for sparse in [0,1]:add(1,h,sparse=sparse)
for life in [1,2,3]:add(1,4,length=128,life=life)
add(1,4,zero=1)
for s in [0,1,2,4]:add(s,4,length=32)
add(1,4,cpu=2);add(1,3,sparse=1,cpu=2)
names=['config_id','repeat','order','storage','handoff','lifecycle','field_length','modules','demand','logger_cpu','logger_zero','offered','accepted','wall_seconds','Mlog_s','errors','nr_throttled','throttled_usec']
def stat():return {k:int(v) for k,v in (x.split() for x in Path('/sys/fs/cgroup/cpu.stat').read_text().splitlines())}
binary=root/'build/bench';sha=hashlib.sha256(binary.read_bytes()).hexdigest();rng=random.Random(20261030);oi=0;start=time.monotonic()
with (out/'recheck.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=names,lineterminator='\n');w.writeheader()
 for repeat in range(5):
  order=wanted.copy();rng.shuffle(order)
  for c in order:
   a=stat();p=subprocess.run(['./build/bench','pipeline_once',*map(str,c['args'])],cwd=root,capture_output=True,text=True,check=True);b=stat();v=next(csv.reader(p.stdout.splitlines()));assert len(v)==27
   row={k:c[k] for k in names if k in c};row.update(repeat=repeat,order=oi,offered=v[2],accepted=v[3],wall_seconds=v[7],Mlog_s=float(v[8])/1e6,errors=v[6],nr_throttled=b['nr_throttled']-a['nr_throttled'],throttled_usec=b['throttled_usec']-a['throttled_usec'])
   assert float(v[7])>=.3 and v[6]=='0';w.writerow(row);f.flush();oi+=1
  print('interleaved recheck repeat',repeat,'rows',oi,flush=True)
assert hashlib.sha256(binary.read_bytes()).hexdigest()==sha
(out/'recheck_metadata.json').write_text(json.dumps({'rows':oi,'configurations':len(wanted),'repeats':5,'seed':20261030,'ended_utc':datetime.now(timezone.utc).isoformat(),'wall_seconds':time.monotonic()-start,'binary_sha256_before_after':sha,'order':'outer repeat shuffles all paired configurations; each retained sample is in an independently launched process'},indent=2)+'\n')
