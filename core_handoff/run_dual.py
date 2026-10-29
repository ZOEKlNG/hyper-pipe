#!/usr/bin/env python3
"""Run the frozen two-core capacity matrix, retaining build and cgroup evidence."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,subprocess,time
root=Path(__file__).resolve().parent
out=root/'results';out.mkdir(exist_ok=True)
paths=['handoff.cpp','common.hpp','log.proto','log.pb.h','log.pb.cc','handoff','build.sh']
def snapshot():return {p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in paths}
before=snapshot();(out/'dual_snapshot_before.json').write_text(json.dumps(before,indent=2)+'\n')
cgroup=Path('/sys/fs/cgroup/cpu.stat');(out/'dual_cpu_before.txt').write_text(cgroup.read_text())
command=['./handoff','--output','results/handoff_results.csv','--repeats','5','--min-seconds','0.3']
start=time.monotonic();started=datetime.now(timezone.utc).isoformat()
with (out/'dual.stdout').open('w') as stdout,(out/'dual.stderr').open('w') as stderr:
 proc=subprocess.Popen(command,cwd=root,stdout=stdout,stderr=stderr)
 pid,status,usage=os.wait4(proc.pid,0);proc.returncode=os.waitstatus_to_exitcode(status)
after=snapshot();(out/'dual_snapshot_after.json').write_text(json.dumps(after,indent=2)+'\n')
(out/'dual_cpu_after.txt').write_text(cgroup.read_text())
meta=dict(command=command,started_utc=started,ended_utc=datetime.now(timezone.utc).isoformat(),wall_s=time.monotonic()-start,
 return_code=proc.returncode,snapshot_consistent=before==after,user_cpu_s=usage.ru_utime,system_cpu_s=usage.ru_stime,max_rss_kib=usage.ru_maxrss,
 voluntary_context_switches=usage.ru_nvcsw,involuntary_context_switches=usage.ru_nivcsw,
 cpu_note='Worker CPU0 and Logger CPU1; per-thread CPU columns include busy polling; spin counters are polls/episodes, not spin duration. Service CPU is measured separately by construction microbenchmark.')
(out/'dual_run_timing.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta))
raise SystemExit(proc.returncode if before==after else 3)
