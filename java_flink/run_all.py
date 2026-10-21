#!/usr/bin/env python3
"""Serialize CPU-heavy Java runs; do not overlap other benchmark processes."""
import csv,json,os,pathlib,subprocess,time
root=pathlib.Path(__file__).resolve().parent
out=root/'results'/os.environ.get('RESULT_LABEL','');out.mkdir(parents=True,exist_ok=True)
corpus=os.environ.get('CORPUS_DIR',str(root.parent/'codec'/'corpus'))
flink_records=os.environ.get('FLINK_RECORDS','3000000')
def run(name,args):
    t=time.time()
    with (out/(name+'.stdout')).open('w') as stdout,(out/(name+'.stderr')).open('w') as stderr:
        p=subprocess.run(['bash','run.sh']+args,cwd=root,stdout=stdout,stderr=stderr,timeout=240)
    print(json.dumps({'step':name,'returncode':p.returncode,'seconds':time.time()-t}),flush=True)
    if p.returncode:raise RuntimeError((out/(name+'.stderr')).read_text()[-8000:])
    return [line for line in (out/(name+'.stdout')).read_text().splitlines() if line.startswith(('runtime,','java21,','flink2.1.0,'))]
if os.environ.get('SKIP_CODEC','0')!='1':
    (out/'java_codec.csv').write_text('\n'.join(run('java_codec',['JavaCodecBench',corpus,'5','2097152']))+'\n')
rows=[]
for codec in ['fixed','protobuf']:
    for mode in ['nokeyby','keyby']:
        result=run('flink_'+codec+'_'+mode,['FlinkLocalBench',corpus,codec,mode,'2',flink_records,'3','64'])
        rows+=result if not rows else result[1:]
(out/'flink_local.csv').write_text('\n'.join(rows)+'\n')
print('All runs completed',flush=True)
