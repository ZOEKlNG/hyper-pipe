from collections import defaultdict
from pathlib import Path
import csv,json,statistics,hashlib

def enrich(root,doc,pipeline,micro,proto):
 def read(path):return list(csv.DictReader((root/path).open()))
 def table(headers,rows):return '\n'.join(['| '+' | '.join(headers)+' |','|'+'|'.join('---' for _ in headers)+'|']+['| '+' | '.join(map(str,x))+' |' for x in rows])
 def write(path,rows):
  with (root/path).open('w',newline='') as f:
   w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
 def interval(a):return f'{statistics.median(a):.2f}（{min(a):.2f}–{max(a):.2f}）'
 recheck=read('results/recheck.csv');mixed=read('results/mixed_lengths.csv')
 assert len(recheck)==110 and len(mixed)==60
 rg=defaultdict(list);mg=defaultdict(list)
 for x in recheck:
  assert int(x['errors'])==0 and int(x['accepted'])==int(x['offered']) and float(x['wall_seconds'])>=.3
  rg[int(x['config_id'])].append(x)
 rs=[]
 for key,values in sorted(rg.items()):
  assert len(values)==5 and {int(x['repeat']) for x in values}==set(range(5))
  a=[float(x['Mlog_s']) for x in values];old=next(x for x in pipeline if int(x['config_id'])==key)
  rs.append(dict(config_id=key,storage=old['storage'],handoff=old['handoff'],lifecycle=old['lifecycle'],length=old['field_length'],modules=old['modules'],demand=old['demand'],logger_cpu=old['logger_cpu'],logger_zero=old['logger_zero'],main_median=old['Mlog_s_median'],recheck_median=statistics.median(a),recheck_minimum=min(a),recheck_maximum=max(a)))
 write('results/recheck_summary.csv',rs)
 for x in mixed:
  assert int(x['wall_ns'])>=80000000
  mg[(x['storage'],x['stage'])].append(x)
 ms=[]
 for key,values in sorted(mg.items()):
  assert len(values)==5
  a=[float(x['cpu_ns_per_log']) for x in values]
  ms.append(dict(storage=key[0],stage=key[1],cpu_ns_median=statistics.median(a),cpu_ns_minimum=min(a),cpu_ns_maximum=max(a),logical_session_bytes=int(values[0]['logical_session_bytes']),session_capacity_bytes=int(values[0]['session_capacity_bytes']),pool_reserved_bytes=int(values[0]['pool_reserved_bytes'])))
 write('results/mixed_lengths_summary.csv',ms)
 for path in ['extras_snapshot','snapshot']:
  assert json.loads((root/f'results/{path}_before.json').read_text())==json.loads((root/f'results/{path}_after.json').read_text())
 for name,sha in json.loads((root/'results/extras_snapshot_before.json').read_text()).items():
  if not name.startswith('build-'):assert hashlib.sha256((root/name).read_bytes()).hexdigest()==sha,name
 main=json.loads((root/'results/snapshot_before.json').read_text());rmeta=json.loads((root/'results/recheck_metadata.json').read_text())
 assert main['build/bench']==rmeta['binary_sha256_before_after']
 rt=table(['方案','规则/生命周期','Logger CPU','主矩阵Mlog/s中位','独立进程交错复查Mlog/s（范围）'],[[r['handoff'],r['demand']+'/'+r['lifecycle']+('/extra_zero' if r['logger_zero'] else ''),r['logger_cpu'],f"{r['main_median']:.2f}",f"{r['recheck_median']:.2f}（{r['recheck_minimum']:.2f}–{r['recheck_maximum']:.2f}）"] for r in rs if r['storage']=='M2_string_reuse' and r['length']==64])
 mt=[]
 for mode in ['M1_string_new','M2_string_reuse','M3_slab','M4_arena256','M4_arena512','M4_arena2048']:
  a=next(x for x in ms if x['storage']==mode and x['stage']=='worker_lifecycle_local');b=next(x for x in ms if x['storage']==mode and x['stage']=='logger_encode')
  mt.append([mode,f"{a['cpu_ns_median']:.2f}（{a['cpu_ns_minimum']:.2f}–{a['cpu_ns_maximum']:.2f}）",f"{b['cpu_ns_median']:.2f}（{b['cpu_ns_minimum']:.2f}–{b['cpu_ns_maximum']:.2f}）",a['logical_session_bytes'],a['session_capacity_bytes']])
 mt=table(['混合长度存储','Worker本地生命周期 ns/log','Logger ns/log','完整载荷B','槽/块/string容量B'],mt)
 selected=[]
 for scheme,api in [('P0_implicit_parent_clear','set_ptr_len'),('P0_implicit_parent_clear','mutable_assign'),('P2_explicit_parent_clear','set_ptr_len'),('P2_explicit_parent_clear','mutable_assign')]:
  p=next(x for x in proto if x['scheme']==scheme and x['api']==api and x['length']=='64' and x['pattern']=='alternate_tls_http_dns')
  selected.append([scheme,api,f"{p['cpu_ns_median']:.2f}",f"{p['alloc_calls_per_event']:.2f}"])
 api_table=table(['字段存在性','官方bytes写入API','编码ns/event','C++ new/event'],selected)
 p2=float(selected[-1][2]);p0=float(selected[0][2])
 cases=json.loads((root/'results/case_order.json').read_text());phases=json.loads((root/'results/phases.json').read_text())
 pipe_throttle=sum(x['cpu_stat_delta']['nr_throttled'] for x in cases)
 phase_text='；'.join(f"{x['binary']}新增{x['cpu_stat_delta']['nr_throttled']}次/{x['cpu_stat_delta']['throttled_usec']}微秒" for x in phases)
 summary=f'''## 用结果选默认组合

先固定PB后端的对象与容量复用，再比较交接。相同64B字段、逐条协议切换、父Clear的同二进制交叉实测：

{api_table}

最终显式optional＋mutable assign约为{p2:.2f}ns，隐式＋旧setter约{p0:.2f}ns；不能把这两项隐藏成本归因于SPSC或存储池。mutable assign仍复制字节，零分配不等于零拷贝。

优先评估M2字符串池化复用作为存储基线；本轮没有证据支持“自定义arena必然更快”。H5模块快照在完整字段的前台组装较轻，H2/H3在稀疏字段更省动作；H4整块复制可以让发日志动作变快，却不自动让包括状态更新的完整会话路径更快。H6只写指针需要源已冻结，变化源的四版本成本已另计。L1/L2/L3要按所有权、丢弃控制和池预算选择，不能用“没有额外原子计数”直接推导L3稳定更快。

**吞吐暂不用于给出唯一赢家或生产容量承诺。** 主矩阵部分配置出现很大的高吞吐簇；独立进程交错复查中有些中位数明显回落。已保留全部数据，不挑最高值：

{rt}

22个配置、5次外层随机交错，共110条，均为新启动进程内至少300ms样本；追加复查的节流计数合计{sum(int(x['nr_throttled']) for x in recheck)}。例如H5稀疏主矩阵的高吞吐不能直接当作稳定优势。没有足够硬件计数来定位这些吞吐状态，不能武断归因于预取、原子操作或虚拟核心关系；隔离CPU成本与正确性结论可独立评估。

## 不同字段跨大小级的补测

原主矩阵各字段在同档长度相同。补充六个常驻字段分别12/32/64/128/256/512B，共1004B，使slab真实跨多个大小级。固定H5、单CPU、4096活跃Session，完整字段在计时外核对122,880次；本补测没有双CPU吞吐：

{mt}

容量包含inline SSO或池块有效容量，元数据另计。它不包括所有分配器元数据；池页总量按4096个Session预留，不能直接与主矩阵8193对象预算比较。混合分布依然不能证明某种硬件预取因果。

## 原始记录与修正的界线

原泛型proto fixture的mixed分支重复刷新了TLS；其660行原始文件不删除。最终proto摘要的单detail540行仍来自主文件，mixed120行改用每个detail只填一次的独立补测；该补测另做120行分配审计及每条件64次wire一致核对。规范分析输入保存在[proto_canonical.csv](results/proto_canonical.csv)，source_sample_file列明确出处。

核心pipeline每个case均记录资源增量，新增节流合计{pipe_throttle}次。后续阶段定位为：{phase_text}。4线程分配器扩展性阶段的小额节流不应被默认为所有双CPU样本的状态。

'''
 doc=doc.replace('## 先回答哪些想法成立',summary+'## 先回答哪些想法成立',1)
 doc=doc.replace('P2混合日志另列，不用P1丢弃一半信息来制造更低耗时。','P2混合日志另列，不用P1丢弃一半信息来制造更低耗时；最终mixed计时采用每个子消息只填一次的补测。')
 doc=doc.replace('没有将计数拦截器放进性能二进制。','没有将计数拦截器放进性能二进制；mixed行来自独立修正补测，单detail各schema/API在同一主二进制内比较。')
 doc=doc.replace('## H1～H6：','![不同字符串长度的Worker本地成本](figures/storage_worker.png)\n\n## H1～H6：',1)
 doc=doc.replace('## P1 oneof / P2 并列子消息','![PB存在性与bytes API交叉成本](figures/protobuf_api.png)\n\n## P1 oneof / P2 并列子消息',1)
 extra={'recheck_rows':len(recheck),'recheck_configurations':len(rg),'recheck_throttle_count':sum(int(x['nr_throttled']) for x in recheck),'mixed_length_rows':len(mixed),'mixed_full_content_checks':122880,'corrected_mixed_proto_rows':120,'corrected_mixed_allocation_rows':120,'pipeline_case_throttles':pipe_throttle,'phase_resource_deltas':phases,'generated_schema_check':json.loads((root/'results/generated_validation.json').read_text())}
 return doc,extra
