#!/usr/bin/env python3
from pathlib import Path
import json,subprocess,hashlib,concurrent.futures
root=Path(__file__).resolve().parent
items=json.loads((root/'download_manifest.json').read_text())
def fetch(x):
 p=root/x['filename']
 if not p.exists():subprocess.run(['curl','-fsSL','--retry','2',x['url'],'-o',str(p)],check=True)
 if hashlib.sha256(p.read_bytes()).hexdigest()!=x['sha256']:raise RuntimeError('SHA mismatch: '+p.name)
 return p
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:paths=list(ex.map(fetch,items))
for p in paths:subprocess.run(['dpkg-deb','-x',str(p),str(root/'root')],check=True)
print('Pinned C++ development headers/tools extracted; matching runtime libraries are still required.')
