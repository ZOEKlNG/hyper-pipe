#!/usr/bin/env python3
"""Functional cross-language UDP checks; not a performance benchmark."""
import json,pathlib,subprocess
root=pathlib.Path(__file__).resolve().parent
rx=subprocess.Popen(['java','-cp',str(root/'build'),'UdpReceiver','--port','19091','--queue','16','--seconds','2'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
ready=rx.stdout.readline().strip()
if not ready.startswith('READY'):raise RuntimeError(ready+rx.stderr.read())
base=[str(root/'build/producer'),'--port','19091','--batch','8','--flush-us','10','--rate','1000']
good=subprocess.run(base+['--count','100','--payload','256','--session','123'],capture_output=True,text=True,check=True)
bad=subprocess.run(base+['--count','3','--payload','256','--malformed'],capture_output=True,text=True,check=True)
out,err=rx.communicate(timeout=10)
if rx.returncode:raise RuntimeError(out+err)
received=json.loads(out.strip().splitlines()[-1]);good_json=json.loads(good.stdout);bad_json=json.loads(bad.stdout)
assert good_json['offered']==good_json['kernel_accepted']==100
assert received['received']==103 and received['processed']==100 and received['invalid']==3 and received['queue_drop']==0,received
result={'kind':'functional_smoke_not_benchmark','ready':ready,'normal_sender':good_json,'malformed_sender':bad_json,'receiver':received}
(root/'smoke-results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
