#!/usr/bin/env python3
"""Compile actual official protobuf v29.3 upb C sources, without system protobuf."""
import hashlib,json,os,pathlib,shlex,subprocess,tarfile,urllib.request
root=pathlib.Path(__file__).resolve().parent
tag='v29.3';commit='b407e8416e3893036aee5af9a12bd9b6a0e2b2e6'
url='https://codeload.github.com/protocolbuffers/protobuf/tar.gz/refs/tags/v29.3'
digest='008a11cc56f9b96679b4c285fd05f46d317d685be3ab524b2a310be0fbad987e'
archive=root/'protobuf-v29.3.tar.gz';src=root/'protobuf-29.3';build=root/'build';build.mkdir(exist_ok=True)
if not archive.exists():subprocess.run(['curl','-fL','--retry','2',url,'-o',str(archive)],check=True)
if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest:raise SystemExit('Official archive SHA256 mismatch')
if not src.exists():
    with tarfile.open(archive) as t:t.extractall(root,filter='data')
# Each directory's *.c are official runtime targets or their internal dependencies.
# C++ tests, reflection/json/compiler tooling, generated Event code and system libprotobuf are absent.
dirs=['base','mem','hash','mini_table','mini_descriptor','message','wire']
sources=sorted({p for d in dirs for p in (src/'upb'/d).rglob('*.c')})
sources.append(src/'third_party/utf8_range/utf8_range.c')
flags=shlex.split(os.environ.get('UPB_BENCH_CFLAGS','-O3 -DNDEBUG -march=native -std=gnu99 -fPIC -Wall -Wextra -Wno-unused-parameter'))
compiler=os.environ.get('UPB_BENCH_CC','gcc')
objects=[]
for p in sources:
    o=build/(str(p.relative_to(src)).replace('/','_')+'.o')
    subprocess.run([compiler,*flags,'-I'+str(src),'-I'+str(src/'third_party/utf8_range'),'-c',str(p),'-o',str(o)],check=True)
    objects.append(o)
lib=build/'libofficial_upb_v29_3.a'
if lib.exists():lib.unlink()
subprocess.run(['ar','rcs',str(lib),*(str(p) for p in objects)],check=True)
version=subprocess.check_output([compiler,'--version'],text=True).splitlines()[0]
metadata={'tag':tag,'commit':commit,'archive_url':url,'archive_sha256':digest,'compiler':version,'cflags':flags,'source_files':[str(p.relative_to(src)) for p in sources],'source_sha256':{str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},'dependencies':['C standard library','vendored third_party/utf8_range/utf8_range.c from the same official archive'],'not_linked':['system libprotobuf','Abseil','generated Event MiniTable or field accessor code'],'upb_fasttable':'default upstream configuration: disabled; runtime MiniTable path','library':str(lib),'library_sha256':hashlib.sha256(lib.read_bytes()).hexdigest()}
(root/'build_metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
print(json.dumps({'library':str(lib),'official_c_sources':len(sources),'commit':commit}))
