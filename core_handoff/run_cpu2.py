#!/usr/bin/env python3
"""Run only after the parent releases CPUs and authorizes the sensitivity pass."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,subprocess,time
root=Path(__file__).resolve().parent;out=root/'results_cpu2';out.mkdir(exist_ok=True)
paths=['handoff.cpp','handoff_cpu2.cpp','common.hpp','log.proto','log.pb.h','log.pb.cc','handoff_cpu2','build_cpu2.sh','prepare_cpu2.py','cpu2_generation.json']
def snapshot():return {p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in paths}
before=snapshot();(out/'snapshot_before.json').write_text(json.dumps(before,indent=2)+'\n')
cgroup=Path('/sys/fs/cgroup/cpu.stat');(out/'cpu_before.txt').write_text(cgroup.read_text())
try:
 topology=subprocess.run(['lscpu','-e=CPU,CORE,CACHE'],capture_output=True,text=True,check=True).stdout
except Exception as e:topology='topology command failed: '+str(e)
(out/'topology.txt').write_text(topology)
cpus=[]
for cpu in [0,1,2]:
 base=Path('/sys/devices/system/cpu')/('cpu'+str(cpu));item={'logical_cpu':cpu,'thread_siblings_list':(base/'topology/thread_siblings_list').read_text().strip(),'caches':[]}
 for index in sorted((base/'cache').glob('index*')):
  item['caches'].append({name:(index/name).read_text().strip() for name in ['level','type','id','shared_cpu_list']})
 cpus.append(item)
(out/'topology.json').write_text(json.dumps({'lscpu_e':topology,'selected_cpus':[0,2],'comparison_cpus':[0,1],'cpu_details':cpus,'caveat':'VM cache topology does not establish independent physical cores.'},indent=2)+'\n')
command=['./handoff_cpu2','--output','results_cpu2/handoff_results.csv','--repeats','5','--min-seconds','0.3']
start=time.monotonic();started=datetime.now(timezone.utc).isoformat()
with (out/'run.stdout').open('w') as stdout,(out/'run.stderr').open('w') as stderr:
 proc=subprocess.Popen(command,cwd=root,stdout=stdout,stderr=stderr)
 pid,status,usage=os.wait4(proc.pid,0);proc.returncode=os.waitstatus_to_exitcode(status)
after=snapshot();(out/'snapshot_after.json').write_text(json.dumps(after,indent=2)+'\n');(out/'cpu_after.txt').write_text(cgroup.read_text())
meta=dict(command=command,started_utc=started,ended_utc=datetime.now(timezone.utc).isoformat(),wall_s=time.monotonic()-start,
 return_code=proc.returncode,snapshot_consistent=before==after,user_cpu_s=usage.ru_utime,system_cpu_s=usage.ru_stime,max_rss_kib=usage.ru_maxrss,
 voluntary_context_switches=usage.ru_nvcsw,involuntary_context_switches=usage.ru_nivcsw,
 cpu_note='Worker logical CPU0 and Logger logical CPU2. Different reported L1/L2 cache ids do not prove independent physical cores in a VM. CPU columns include busy polling.')
(out/'run_timing.json').write_text(json.dumps(meta,indent=2)+'\n');print(json.dumps(meta));raise SystemExit(proc.returncode if before==after else 3)
