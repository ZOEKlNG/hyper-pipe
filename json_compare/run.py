#!/usr/bin/env python3
"""Run the CPU benchmark serially, retaining hashes, timing and quota evidence."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,platform,subprocess,time
root=Path(__file__).resolve().parent
out=root/'results';out.mkdir(exist_ok=True)
paths=[*root.glob('*.cpp'),*root.glob('*.hpp'),root/'bench',root/'build.sh',root/'run.py',
       *[p for p in (root/'vendor').rglob('*') if p.is_file()],
       root/'../core_handoff/common.hpp',root/'../core_handoff/log.proto',
       root/'../core_handoff/log.pb.cc',root/'../core_handoff/log.pb.h']
def snapshot():return {os.path.relpath(p,root):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}
def command(a):return subprocess.run(a,cwd=root,capture_output=True,text=True,check=True).stdout.strip()
meta={'recorded_utc':datetime.now(timezone.utc).isoformat(),'compiler':command(['g++','--version']),
      'kernel':platform.platform(),'lscpu':command(['lscpu']),
      'available_affinity':sorted(os.sched_getaffinity(0)),'measurement_logical_cpu':0,
      'cpu_max':Path('/sys/fs/cgroup/cpu.max').read_text().strip(),
      'build_flags':'-O3 -DNDEBUG -std=c++23 -march=native -pthread; yyjson compiled as C11',
      'corpus_records':4096,'minimum_retained_wall_ns':80000000,'repeats':5,
      'scope':'Isolated local CPU service; no queue, cross-core handoff, NIC, DPDK, network, Flink or message latency percentiles.'}
(root/'environment.json').write_text(json.dumps(meta,indent=2)+'\n')
before=snapshot();(out/'snapshot_before.json').write_text(json.dumps(before,indent=2)+'\n')
stat=Path('/sys/fs/cgroup/cpu.stat');(out/'cpu_before.txt').write_text(stat.read_text())
cmd=['./bench','--output','results/benchmark.csv','--repeats','5']
start=time.monotonic();started=datetime.now(timezone.utc).isoformat()
with (out/'run.stdout').open('w') as stdout,(out/'run.stderr').open('w') as stderr:
    proc=subprocess.Popen(cmd,cwd=root,stdout=stdout,stderr=stderr)
    _,status,usage=os.wait4(proc.pid,0);proc.returncode=os.waitstatus_to_exitcode(status)
after=snapshot();(out/'snapshot_after.json').write_text(json.dumps(after,indent=2)+'\n')
(out/'cpu_after.txt').write_text(stat.read_text())
timing={'command':cmd,'started_utc':started,'ended_utc':datetime.now(timezone.utc).isoformat(),
        'wall_seconds':time.monotonic()-start,'return_code':proc.returncode,'snapshot_consistent':before==after,
        'user_cpu_s':usage.ru_utime,'system_cpu_s':usage.ru_stime,'max_rss_kib':usage.ru_maxrss,
        'voluntary_context_switches':usage.ru_nvcsw,'involuntary_context_switches':usage.ru_nivcsw}
(out/'run_timing.json').write_text(json.dumps(timing,indent=2)+'\n');print(json.dumps(timing))
raise SystemExit(proc.returncode if before==after else 3)
