#!/usr/bin/env python3
"""Run all primary matrices serially. No compilation during timing."""
from pathlib import Path
from datetime import datetime,timezone
from collections import defaultdict
import csv,hashlib,json,os,random,subprocess,time,platform
root=Path(__file__).resolve().parent;out=root/'results';out.mkdir(exist_ok=True)
S=['M1_string_new','M2_string_reuse','M3_slab','M4_arena256','M4_arena512','M4_arena2048']
H=['H1_deep','H2_tape','H3_selective','H4_bulk','H5_modules_copy','H6_versioned_ptr']
L=['L0_owned','L1_shared_lease','L2_return_reset','L3_piggyback']
matrix=defaultdict(set)
def add(group,s,h,length=64,mods=3,sparse=0,life=3,cpu=1,zero=0):
    if h==0:life=0
    matrix[(s,h,life,length,mods,sparse,cpu,zero)].add(group)
for s in range(6):
 for length in [12,32,64,128,512]:add('storage',s,4,length)
for s in [1,4]:
 for h in range(6):
  for length in [12,64,512]:
   for sparse in [0,1]:add('handoff',s,h,length,sparse=sparse)
for life in [1,2,3]:
 for length in [12,128,512]:add('lifecycle',1,4,length,life=life)
for h in [4,5]:
 for mods in [1,2,3,5,8]:
  for length in [12,128]:add('scaling',1,h,length,mods)
for zero in [0,1]:add('reset_control',1,4,64,zero=zero)
for length in [15,16,48]:
 for s in [0,1,2,4]:add('SSO_and_midlength',s,4,length)
for h in range(6):add('cpu02_sensitivity',1,h,64,cpu=2)
for h in [2,3]:add('cpu02_sensitivity',1,h,64,sparse=1,cpu=2)
for life in [1,2]:add('cpu02_sensitivity',1,4,128,life=life,cpu=2)
configs=[]
for i,(cfg,groups) in enumerate(sorted(matrix.items())):
 s,h,l,length,mods,sparse,cpu,zero=cfg
 configs.append({'config_id':i,'storage':S[s],'handoff':H[h],'lifecycle':L[l],'field_length':length,'modules':mods,'demand':'sparse' if sparse else 'workflow_dense','logger_cpu':cpu,'logger_zero':zero,'groups':';'.join(sorted(groups)),'args':list(cfg)})
(root/'matrix.json').write_text(json.dumps(configs,indent=2)+'\n')
paths=[*root.glob('*.cpp'),*root.glob('*.hpp'),root/'events.proto',root/'CMakeLists.txt',root/'build.sh',root/'run.py',root/'build/bench',root/'build/proto_bench',root/'build/proto_alloc',root/'build/store_audit',root/'build/allocator_scaling']
def snapshot():return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
def cmd(a):return subprocess.run(a,cwd=root,capture_output=True,text=True,check=True).stdout.strip()
environment={'recorded_utc':datetime.now(timezone.utc).isoformat(),'compiler':cmd(['g++','--version']),'platform':platform.platform(),'lscpu':cmd(['lscpu']),'affinity':sorted(os.sched_getaffinity(0)),'cpu_max':Path('/sys/fs/cgroup/cpu.max').read_text().strip(),'memory_max':Path('/sys/fs/cgroup/memory.max').read_text().strip(),'case_order_seed':20261029,'pipeline_repeat_order':'five consecutive runs per randomized configuration','micro_repeat_order':'five consecutive runs per unique storage/handoff/length/module/demand configuration','scope':'same-process single Worker/Logger on logical CPUs, simulated protocol extraction, real threads and official Protobuf; no NIC/network/DPDK/Flink'}
(root/'environment.json').write_text(json.dumps(environment,indent=2)+'\n')
before=snapshot();(out/'snapshot_before.json').write_text(json.dumps(before,indent=2)+'\n');(out/'cpu_before.txt').write_text(Path('/sys/fs/cgroup/cpu.stat').read_text())
prefix=[k for k in configs[0] if k!='args']
microcols=['stage','repeat','iterations','wall_ns','cpu_ns','cpu_ns_per_log','wall_ns_per_log','checksum']
pipecols=['stage','repeat','offered','accepted','dropped','received','errors','wall_seconds','logs_per_s','worker_total_cpu_ns','logger_total_cpu_ns','worker_spin_polls','logger_spin_polls','wire_bytes','checksum','reclaimed','hooked','immediate','retired_peak','free_low','pool_reserved_bytes','pool_page_growth','session_metadata_bytes','retained_string_heap_bytes','ring_bytes','return_ring_bytes','process_maxrss_kib']
order=configs.copy();random.Random(20261029).shuffle(order)
seen_micro=set();logs=[];start=time.monotonic()
with (out/'micro.csv').open('w',newline='') as mf,(out/'pipeline.csv').open('w',newline='') as pf:
 mw=csv.DictWriter(mf,fieldnames=prefix+microcols,lineterminator='\n');mw.writeheader();pw=csv.DictWriter(pf,fieldnames=prefix+pipecols,lineterminator='\n');pw.writeheader()
 for oi,c in enumerate(order):
  meta={k:c[k] for k in prefix};s,h,l,length,mods,sparse,cpu,zero=c['args'];args=list(map(str,c['args']))
  key=(s,h,length,mods,sparse)
  if key not in seen_micro:
   for repeat in range(5):
    a=['./build/bench','micro',*args];p=subprocess.run(a,cwd=root,capture_output=True,text=True,check=True)
    for values in csv.reader(p.stdout.splitlines()):
     r=dict(zip(microcols,values));r['repeat']=repeat;mw.writerow(meta|r)
    mf.flush()
   seen_micro.add(key)
  started=time.monotonic();a=['./build/bench','pipeline',*args];p=subprocess.run(a,cwd=root,capture_output=True,text=True,check=True)
  lines=list(csv.reader(p.stdout.splitlines()));assert len(lines)==5
  for values in lines:
   r=dict(zip(pipecols,values));assert float(r['wall_seconds'])>=.3;pw.writerow(meta|r)
  pf.flush();logs.append({'order':oi,'config_id':c['config_id'],'command':a,'wall_s':time.monotonic()-started,'stderr':p.stderr})
  (out/'case_order.json').write_text(json.dumps(logs,indent=2)+'\n')
  print('completed',oi+1,'/',len(order),'config',c['config_id'],flush=True)
for binary,file in [('proto_bench','proto.csv'),('proto_alloc','proto_alloc.csv'),('store_audit','storage_alloc.csv'),('allocator_scaling','allocator_scaling.csv')]:
 with (out/file).open('w') as f:subprocess.run(['./build/'+binary],cwd=root,stdout=f,check=True)
after=snapshot();(out/'snapshot_after.json').write_text(json.dumps(after,indent=2)+'\n');(out/'cpu_after.txt').write_text(Path('/sys/fs/cgroup/cpu.stat').read_text())
timing={'wall_seconds':time.monotonic()-start,'ended_utc':datetime.now(timezone.utc).isoformat(),'configurations':len(configs),'unique_micro_configurations':len(seen_micro),'snapshots_match':before==after}
(out/'timing.json').write_text(json.dumps(timing,indent=2)+'\n');assert before==after;print(json.dumps(timing))
