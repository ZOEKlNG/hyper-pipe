#!/usr/bin/env python3
from pathlib import Path
import hashlib,json,os,subprocess,time
root=Path(__file__).resolve().parent;out=root/'validation/stress';out.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1'
binary=root/'build-asan/bench';digest=hashlib.sha256(binary.read_bytes()).hexdigest()
cases=[('L1_shared_lossless','stress_lossless',1,4,1,128,3,0,1),('L2_return_lossless','stress_lossless',1,4,2,128,3,0,1),('L3_piggyback_lossless','stress_lossless',1,4,3,128,3,0,1),('L3_piggyback_drop','stress',1,4,3,128,3,0,1),('H6_versioned_lossless','stress_lossless',1,5,3,128,3,0,1)]
records=[]
for label,*args in cases:
 start=time.monotonic();command=[str(binary),*map(str,args)]
 p=subprocess.run(command,cwd=root,env=env,capture_output=True,text=True)
 r={'label':label,'command':command,'wall_s':time.monotonic()-start,'return_code':p.returncode,'stdout':p.stdout.strip(),'stderr':p.stderr,'sanitized_binary_sha256':digest}
 records.append(r);(out/'runs.json').write_text(json.dumps(records,indent=2)+'\n');print(label,'return',p.returncode,'seconds',round(r['wall_s'],2),flush=True)
 if p.returncode:raise SystemExit(p.returncode)
assert hashlib.sha256(binary.read_bytes()).hexdigest()==digest
summary={'status':'PASS','runs':len(records),'offered':sum(int(r['stdout'].split(',')[2]) for r in records),'accepted_and_fully_verified':sum(int(r['stdout'].split(',')[3]) for r in records),'dropped':sum(int(r['stdout'].split(',')[4]) for r in records),'full_payload_errors':sum(int(r['stdout'].split(',')[6]) for r in records),'asan_options':env['ASAN_OPTIONS'],'ubsan_options':env['UBSAN_OPTIONS'],'sanitized_binary_sha256':digest,'note':'Poisoned recycle, complete decoded field/content checks, one real Worker and Logger. Finite stress is empirical evidence, not universal proof; ASan/UBSan are not race detectors.'}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
