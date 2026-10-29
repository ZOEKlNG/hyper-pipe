#!/usr/bin/env python3
"""Run the authorized benchmark serially and retain cgroup/time evidence."""
from pathlib import Path
from datetime import datetime,timezone
import json,subprocess,time,resource,hashlib
root=Path(__file__).resolve().parent
out=root/'results';out.mkdir(exist_ok=True)
snapshot_paths=['bench','bench.cpp','common.hpp','plan_compiler.hpp','pb_adapters.hpp','plans.json','custom/codecs.hpp','upb/adapter.cpp','upb/adapter.hpp','upb/build/libofficial_upb_v29_3.a','../codec/event.pb.cc','../codec/event.pb.h']
def snapshot():
 return {p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in snapshot_paths}
before=snapshot();(out/'build_snapshot_before.json').write_text(json.dumps(before,indent=2)+'\n')
stat=Path('/sys/fs/cgroup/cpu.stat')
(out/'cpu_before.txt').write_text(stat.read_text())
started=datetime.now(timezone.utc).isoformat();start=time.monotonic()
with (root/'run.stdout').open('w') as stdout,(root/'run.stderr').open('w') as stderr:
 result=subprocess.run(['./bench','--output','results','--repeats','5','--min-ms','100'],cwd=root,stdout=stdout,stderr=stderr)
(out/'cpu_after.txt').write_text(stat.read_text())
after=snapshot();(out/'build_snapshot_after.json').write_text(json.dumps(after,indent=2)+'\n')
meta={'snapshot_consistent':before==after,'started_utc':started,'ended_utc':datetime.now(timezone.utc).isoformat(),'wall_seconds':time.monotonic()-start,'return_code':result.returncode}
usage=resource.getrusage(resource.RUSAGE_CHILDREN)
meta.update(user_cpu_s=usage.ru_utime,system_cpu_s=usage.ru_stime,max_rss_kib=usage.ru_maxrss,voluntary_context_switches=usage.ru_nvcsw,involuntary_context_switches=usage.ru_nivcsw)
(out/'run_timing.json').write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps(meta));raise SystemExit(result.returncode)
