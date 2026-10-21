import json, pathlib, subprocess, time, datetime
p=pathlib.Path(__file__).resolve().parent
metadata={}
def state():
 return {'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(), 'cpu_stat':pathlib.Path('/sys/fs/cgroup/cpu.stat').read_text(), 'cpu_max':pathlib.Path('/sys/fs/cgroup/cpu.max').read_text()}
for phase,args,timeout in [('baseline',['--out',str(p/'results'),'--seconds','0.6','--repeats','5','--rounds','5000'],240),('paced',['--out',str(p/'results'),'--paced','--seconds','1','--repeats','3'],60)]:
 metadata[phase]={'start':state(),'argv':[str(p/'transport')]+args}
 started=time.monotonic()
 with open(p/(phase+'.log'),'w') as out:
  proc=subprocess.run([str(p/'transport')]+args,stdout=out,stderr=subprocess.STDOUT,timeout=timeout)
 metadata[phase].update(end=state(),wall_seconds=time.monotonic()-started,exit_code=proc.returncode)
 (p/'measurement_metadata.json').write_text(json.dumps(metadata,indent=2))
 if proc.returncode:raise SystemExit(proc.returncode)
subprocess.run(['python3',str(p/'summarize.py'),str(p/'results')],check=True)
