#!/usr/bin/env python3
"""Independent Java interop function checks. Does not collect performance results."""
import argparse,base64,collections,hashlib,json,pathlib,subprocess,sys,shutil
ROOT=pathlib.Path(__file__).resolve().parent
WORK=ROOT.parent.parent
FIELDS=['id','seq','timestamp','value0','value1','producer','stream','type','flags']

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--fixtures',type=pathlib.Path,required=True);ap.add_argument('--compile-only',action='store_true')
    ap.add_argument('--protobuf-jar',type=pathlib.Path);ap.add_argument('--ecj-jar',type=pathlib.Path);ap.add_argument('--generated-java',type=pathlib.Path);args=ap.parse_args()
    result=ROOT/'results';result.mkdir(exist_ok=True);classes=ROOT/'classes';classes.mkdir(exist_ok=True)
    pb=args.protobuf_jar or WORK/'java_flink/m2/com/google/protobuf/protobuf-java/3.21.12/protobuf-java-3.21.12.jar'
    if args.protobuf_jar is None and not pb.is_file():pb=ROOT/'.deps/protobuf-java-3.21.12.jar'
    ecj=args.ecj_jar or WORK/'java_flink/ecj.jar'
    if args.ecj_jar is None and not ecj.is_file():ecj=ROOT/'.deps/ecj-3.40.0.jar'
    generated=args.generated_java or WORK/'java_flink/generated/bench/EventOuterClass.java'
    for dep in [pb,ecj,generated]:
        if not dep.is_file():raise FileNotFoundError(str(dep)+' missing; run bash prepare.sh or supply explicit dependency paths')
    test=ROOT/'testsrc/InteropCheck.java'
    cmd=['java','-jar',str(ecj),'-17','-nowarn','-cp',str(pb),'-d',str(classes),str(generated),str(test)]
    built=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
    (result/'compile.stdout').write_text(built.stdout);(result/'compile.stderr').write_text(built.stderr)
    if built.returncode:raise RuntimeError(built.stderr)
    if args.compile_only:return
    saved_fixture=result/'fixtures.input.jsonl'
    if args.fixtures.resolve()!=saved_fixture.resolve():shutil.copyfile(args.fixtures,saved_fixture)
    fixtures=[]
    for ln,line in enumerate(args.fixtures.read_text().splitlines(),1):
        if not line.strip():continue
        row=json.loads(line);expected=row['expected']
        if set(FIELDS)-set(expected):raise ValueError(f'fixture line {ln} missing expected fields: {set(FIELDS)-set(expected)}')
        payload=expected.get('payload_base64','');base64.b64decode(payload,validate=True)
        wire=row['wire_base64'];base64.b64decode(wire,validate=True)
        codec=str(row['codec']);type_=str(row.get('protobuf_type','Hybrid' if 'hybrid' in codec.lower() else 'Event'))
        vals=[str(row.get('case',f'line{ln}')),codec,str(row['plan']),type_]
        for f in FIELDS:
            val=int(expected.get(f,0));limit=(1<<64)-1 if f in FIELDS[:5] else (1<<32)-1
            if not 0<=val<=limit:raise ValueError(f'{f}: {val}')
            vals.append(str(val))
        vals += [payload,wire]
        if any('\t' in v or '\n' in v for v in vals):raise ValueError('bad TSV metadata')
        fixtures.append((row,vals))
    tsv=result/'normalized_fixtures.tsv'
    header=['case','codec','plan','protobuf_type']+FIELDS+['payload_base64','wire_base64']
    tsv.write_text('\t'.join(header)+'\n'+''.join('\t'.join(vals)+'\n' for _,vals in fixtures))
    run=['java','-cp',str(classes)+':'+str(pb),'interop.InteropCheck',str(tsv)]
    checked=subprocess.run(run,capture_output=True,text=True,timeout=60)
    (result/'java.stdout').write_text(checked.stdout);(result/'java.stderr').write_text(checked.stderr)
    if checked.returncode:raise RuntimeError(checked.stderr)
    summaries=[json.loads(x.split('\t',1)[1]) for x in checked.stdout.splitlines() if x.startswith('SUMMARY\t')]
    if len(summaries)!=1:raise RuntimeError('missing Java summary')
    summary=summaries[0]
    expected_rows=[r['expected'] for r,_ in fixtures]
    payloads=[base64.b64decode(e.get('payload_base64','')) for e in expected_rows]
    coverage={'all_zero_defaults':any(all(int(e[f])==0 for f in FIELDS) and not p for e,p in zip(expected_rows,payloads)), 'u64_highbit':any(int(e[f]) >= (1<<63) for e in expected_rows for f in FIELDS[:5]), 'u64_max':any(int(e[f]) == (1<<64)-1 for e in expected_rows for f in FIELDS[:5]), 'u32_highbit':any(int(e[f]) >= (1<<31) for e in expected_rows for f in FIELDS[5:]), 'u32_max':any(int(e[f]) == (1<<32)-1 for e in expected_rows for f in FIELDS[5:]), 'payload_contains_ff':any(255 in p for p in payloads), 'payload_contains_zero':any(0 in p for p in payloads), 'payload_contains_highbit':any(any(b>=128 for b in p) for p in payloads), 'empty_payload':any(not p for p in payloads), 'nonempty_payload':any(bool(p) for p in payloads)}
    if not all(coverage[k] for k in ['all_zero_defaults','u64_highbit','u64_max','payload_contains_ff','empty_payload','nonempty_payload']):raise AssertionError('required fixture coverage missing: '+str(coverage))
    summary.update({'coverage':coverage})
    summary.update({'source_fixture':str(args.fixtures),'saved_fixture':str(result/'fixtures.input.jsonl'),'source_fixture_sha256':digest(args.fixtures),'generated_java_sha256':digest(generated),'protobuf_jar_sha256':digest(pb),'test_java_sha256':digest(test),'compile_command':cmd,'run_command':run,'codec_counts':dict(collections.Counter(r['codec'] for r,_ in fixtures)),'plan_counts':dict(collections.Counter(r['plan'] for r,_ in fixtures)),'measurement':'functional_only_no_performance'})
    (result/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
