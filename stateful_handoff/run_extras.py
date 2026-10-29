#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,subprocess,time
root=Path(__file__).resolve().parent;out=root/'results'
paths=[root/x for x in ['bench.cpp','types.hpp','storage.hpp','slots.hpp','encoding.hpp','events.proto','events.pb.cc','events.pb.h','proto_bench.cpp','extras/mixed_bench.cpp','extras/proto_mixed.cpp','extras/CMakeLists.txt','build-extras/mixed_bench','build-extras/proto_mixed','build-extras/proto_mixed_alloc']]
def snapshot():return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
before=snapshot();(out/'extras_snapshot_before.json').write_text(json.dumps(before,indent=2)+'\n')
records=[]
for binary,file,expected_rows in [('mixed_bench','mixed_lengths.csv',60),('proto_mixed','proto_mixed.csv',120),('proto_mixed_alloc','proto_mixed_alloc.csv',120)]:
 start=time.monotonic()
 with (out/file).open('w') as stdout,(out/(binary+'.stderr')).open('w') as stderr:
  p=subprocess.run(['./build-extras/'+binary],cwd=root,stdout=stdout,stderr=stderr)
 records.append({'binary':binary,'command':['./build-extras/'+binary],'wall_s':time.monotonic()-start,'return_code':p.returncode,'expected_rows':expected_rows})
 (out/'extras_runs.json').write_text(json.dumps(records,indent=2)+'\n')
 if p.returncode:raise SystemExit(p.returncode)
 print(binary,'PASS',flush=True)
after=snapshot();(out/'extras_snapshot_after.json').write_text(json.dumps(after,indent=2)+'\n');assert before==after
(out/'extras_metadata.json').write_text(json.dumps({'ended_utc':datetime.now(timezone.utc).isoformat(),'snapshots_match':True,'mixed_lengths':[12,32,64,128,256,512],'active_sessions':4096,'mixed_full_field_checks':6*5*4096,'corrected_mixed_proto_checks':2*4*3*5*64,'note':'Mixed storage is single-CPU only, H5, 4096 stable sessions; corrected proto supplement fills TLS once and HTTP once.'},indent=2)+'\n')
