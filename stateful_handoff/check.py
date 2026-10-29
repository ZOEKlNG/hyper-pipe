#!/usr/bin/env python3
"""Actual functional runs, independent of performance hot loops."""
from pathlib import Path
import subprocess,json,sys,time
root=Path(__file__).resolve().parent
binary=root/(sys.argv[1] if len(sys.argv)>1 else 'build/bench')
out=root/'validation'/('asan' if 'asan' in str(binary) else 'normal');out.mkdir(parents=True,exist_ok=True)
cases=[]
# H1 owns bytes; reference variants use real deferred retirement. Test mixed,
# sparse, multiple wraps, partial tail and all requested lengths/store models.
for storage in range(6):
 for length in [12,32,64,128,512]:cases.append((storage,4,3,length,3,0,1))
for h in range(6):
 for sparse in [0,1]:
  for mods in [1,2,3,5,8]:cases.append((1,h,0 if h==0 else 3,64,mods,sparse,1))
for life in [1,2,3]:
 for h in [2,4,5]:cases.append((1,h,life,128,3,0,1))
seen=set();results=[];started=time.monotonic()
for cfg in cases:
 if cfg in seen:continue
 seen.add(cfg);cmd=[str(binary),'smoke',*map(str,cfg)]
 p=subprocess.run(cmd,cwd=root,capture_output=True,text=True)
 record={'config':cfg,'return_code':p.returncode,'stdout':p.stdout.strip(),'stderr':p.stderr}
 results.append(record)
 if p.returncode:
  (out/'functional.json').write_text(json.dumps(results,indent=2)+'\n');raise SystemExit('Functional failure: '+str(record))
 if len(results)%20==0:print('functional cases',len(results),flush=True)
(out/'functional.json').write_text(json.dumps(results,indent=2)+'\n')
summary={'status':'PASS','cases':len(results),'offered':sum(int(x['stdout'].split(',')[2]) for x in results),'full_field_payload_errors':0,'elapsed_s':time.monotonic()-started,'binary':str(binary),'note':'Real two-thread FIFO, 4096 active flow slots, four changing emissions/session, forced Logger startup pause, partial final cycle and poison-on-recycle. Empirical safety checks, not a proof for all schedules.'}
(out/'functional_summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
