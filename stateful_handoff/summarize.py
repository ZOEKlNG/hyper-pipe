#!/usr/bin/env python3
"""Validate raw counts and aggregate actual runs. Never executes a benchmark."""
from pathlib import Path
from collections import defaultdict
import csv,hashlib,json,statistics
root=Path(__file__).resolve().parent
def read(p):return list(csv.DictReader((root/p).open()))
def write(p,rows):
 with (root/p).open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def median(a):return statistics.median(a)
def stats(a):return dict(median=median(a),minimum=min(a),maximum=max(a))
configs=json.loads((root/'matrix.json').read_text());pipe=read('results/pipeline.csv');micro=read('results/micro.csv');proto=read('results/proto.csv');alloc=read('results/proto_alloc.csv');store=read('results/storage_alloc.csv');scaling=read('results/allocator_scaling.csv')
assert len(pipe)==len(configs)*5
pg=defaultdict(list);mg=defaultdict(list)
microkeys=['storage','handoff','field_length','modules','demand','stage']
for row in pipe:
 assert int(row['errors'])==0 and int(row['accepted'])==int(row['received'])==int(row['offered']) and int(row['dropped'])==0 and float(row['wall_seconds'])>=.3
 pg[int(row['config_id'])].append(row)
for row in micro:
 assert int(row['wall_ns'])>=80000000 and int(row['iterations'])>0
 assert abs(int(row['cpu_ns'])/int(row['iterations'])-float(row['cpu_ns_per_log']))<1e-6
 mg[tuple(row[k] for k in microkeys)].append(row)
assert len(pg)==len(configs) and all(len(v)==5 and set(int(x['repeat']) for x in v)==set(range(5)) for v in pg.values())
assert all(len(v)==5 and set(int(x['repeat']) for x in v)==set(range(5)) for v in mg.values())
expected=set()
for c in configs:
 for stage in ['worker_assembly','logger_encode','worker_lifecycle_local']+(['worker_descriptor_only_frozen'] if c['handoff']=='H6_versioned_ptr' else []):
  expected.add(tuple(str(c[k]) for k in microkeys[:-1])+(stage,))
assert set(mg)==expected
before=json.loads((root/'results/snapshot_before.json').read_text());after=json.loads((root/'results/snapshot_after.json').read_text());assert before==after
for name,sha in before.items():
 if not name.startswith('build/'):
  assert hashlib.sha256((root/name).read_bytes()).hexdigest()==sha,name
pipe_summary=[]
for c in configs:
 values=pg[c['config_id']];r={k:v for k,v in c.items() if k!='args'};a=stats([float(x['logs_per_s'])/1e6 for x in values]);r.update({f'Mlog_s_{k}':v for k,v in a.items()})
 for field in ['worker_total_cpu_ns','logger_total_cpu_ns']:
  r[field+'_per_log_including_spin']=median(int(x[field])/int(x['accepted']) for x in values)
 r['wire_bytes_per_log']=median(int(x['wire_bytes'])/int(x['accepted']) for x in values)
 for field in ['pool_reserved_bytes','session_metadata_bytes','retained_string_heap_bytes','ring_bytes','return_ring_bytes','process_maxrss_kib','retired_peak','free_low','pool_page_growth']:
  r[field]=median(int(x[field]) for x in values)
 r['shortest_run_s']=min(float(x['wall_seconds']) for x in values);pipe_summary.append(r)
write('results/pipeline_summary.csv',pipe_summary)
micro_summary=[]
for key,values in sorted(mg.items()):
 a=stats([float(x['cpu_ns_per_log']) for x in values]);micro_summary.append(dict(zip(microkeys,key),**{f'cpu_ns_{k}':v for k,v in a.items()},wall_ns_median=median(float(x['wall_ns_per_log']) for x in values)))
write('results/micro_summary.csv',micro_summary)
assert len(proto)==660 and len(alloc)==660 and len(store)==30 and len(scaling)==90
corrected_proto=read('results/proto_mixed.csv');corrected_alloc=read('results/proto_mixed_alloc.csv')
assert len(corrected_proto)==len(corrected_alloc)==120
proto=[x|{'source_sample_file':'proto.csv'} for x in proto if x['pattern']!='mixed_tls_http_P2_only']+[x|{'source_sample_file':'proto_mixed.csv'} for x in corrected_proto]
alloc=[x|{'source_sample_file':'proto_alloc.csv'} for x in alloc if x['pattern']!='mixed_tls_http_P2_only']+[x|{'source_sample_file':'proto_mixed_alloc.csv'} for x in corrected_alloc]
assert len(proto)==len(alloc)==660
write('results/proto_canonical.csv',proto);write('results/proto_alloc_canonical.csv',alloc)
pr=defaultdict(list);ar=defaultdict(list);sr=defaultdict(list)
for row in proto:
 assert int(row['wall_ns'])>=80000000
 pr[tuple(row[k] for k in ['scheme','api','pattern','length'])].append(row)
for row in alloc:ar[tuple(row[k] for k in ['scheme','api','pattern','length'])].append(row)
proto_summary=[]
for key,values in sorted(pr.items()):
 assert len(values)==5 and len(ar[key])==5
 a=stats([float(x['cpu_ns_per_event']) for x in values]);r=dict(zip(['scheme','api','pattern','length'],key),**{f'cpu_ns_{k}':v for k,v in a.items()})
 for field in ['alloc_calls','free_calls','alloc_bytes']:r[field+'_per_event']=median(int(x[field])/int(x['iterations']) for x in ar[key])
 proto_summary.append(r)
write('results/proto_summary.csv',proto_summary)
for row in scaling:
 assert int(row['wall_ns'])>=300000000
 sr[tuple(row[k] for k in ['storage','length','threads'])].append(row)
scaling_summary=[]
for key,values in sorted(sr.items()):
 assert len(values)==5;a=stats([float(x['cycles_per_s'])/1e6 for x in values]);scaling_summary.append(dict(zip(['storage','length','threads'],key),**{f'Msession_s_{k}':v for k,v in a.items()}))
write('results/allocator_scaling_summary.csv',scaling_summary)
def find(s,h,length=64,mods=3,sparse=False,life=3,cpu=1,zero=0):
 if h==0:life=0
 c=next(c for c in configs if c['args']==[s,h,life,length,mods,int(sparse),cpu,zero]);return next(x for x in pipe_summary if x['config_id']==c['config_id'])
def mrow(p,stage):
 return next(x for x in micro_summary if all(x[k]==str(p[k]) for k in microkeys[:-1]) and x['stage']==stage)
def fmt(v):return f'{v:.2f}'
def interval(r,prefix='cpu_ns'):
 return f"{r[prefix+'_median']:.2f}（{r[prefix+'_minimum']:.2f}–{r[prefix+'_maximum']:.2f}）"
def table(headers,rows):return '\n'.join(['| '+' | '.join(headers)+' |','|'+'|'.join('---' for _ in headers)+'|']+['| '+' | '.join(map(str,x))+' |' for x in rows])
handoff=[]
for sparse in [False,True]:
 for h in range(6):
  p=find(1,h,sparse=sparse);handoff.append(['四字段' if sparse else '完整阶段规则',p['handoff'],interval(mrow(p,'worker_assembly')),interval(mrow(p,'worker_lifecycle_local')),interval(mrow(p,'logger_encode')),interval(p,'Mlog_s'),int(p['ring_bytes'])//4096])
ht=table(['规则','方案','Worker仅组装ns','Worker本地全生命周期ns/log','Logger编码ns/log','完整Mlog/s','每槽实际B'],handoff)
storage=[]
for length in [12,32,64,128,512]:
 for s in range(6):
  p=find(s,4,length);storage.append([length,p['storage'],fmt(mrow(p,'worker_lifecycle_local')['cpu_ns_median']),fmt(mrow(p,'logger_encode')['cpu_ns_median']),fmt(p['Mlog_s_median']),fmt(p['pool_reserved_bytes']/2**20),fmt(p['retained_string_heap_bytes']/2**20),fmt(p['process_maxrss_kib']/1024)])
st=table(['每字段B','存储','Worker本地ns/log','Logger ns/log','完整Mlog/s','池页MiB','保留字符串堆MiB','进程峰值MiB'],storage)
life=[]
for length in [12,128,512]:
 for l in [1,2,3]:
  p=find(1,4,length,life=l);life.append([length,p['lifecycle'],interval(p,'Mlog_s'),fmt(p['worker_total_cpu_ns_per_log_including_spin']),fmt(p['logger_total_cpu_ns_per_log_including_spin']),int(p['retired_peak']),int(p['free_low'])])
lt=table(['字段B','生命周期','完整Mlog/s','Worker总CPU/log含忙等ns','Logger总CPU/log含忙等ns','退休钩子峰值','空闲Session最低'],life)
scale=[]
for length in [12,128]:
 for mods in [1,2,3,5,8]:
  for h in [4,5]:
   p=find(1,h,length,mods);scale.append([length,mods,p['handoff'],fmt(mrow(p,'worker_assembly')['cpu_ns_median']),fmt(mrow(p,'worker_descriptor_only_frozen')['cpu_ns_median']) if h==5 else '—',fmt(mrow(p,'logger_encode')['cpu_ns_median']),interval(p,'Mlog_s')])
sct=table(['字段B','模块数','方案','完整组装ns','已有冻结版本仅描述符ns','Logger ns','完整Mlog/s'],scale)
pt=table(['方案','bytes API','模式','字段B','CPU ns/event','C++ new/event','C++ delete/event'],[[r['scheme'],r['api'],r['pattern'],r['length'],interval(r),fmt(r['alloc_calls_per_event']),fmt(r['free_calls_per_event'])] for r in proto_summary if r['length']=='64'])
memory=table(['存储','字段B','完整载荷B','字符串/槽/块容量B','热10000会话new次数','池保留B','载荷地址跨度B','覆盖缓存行'],[[x['mode'],x['length'],x['logical_payload_bytes'],x['session_payload_capacity'],x['allocations'],x['pooled_reserved_bytes'],x['address_span_bytes'],x['payload_cache_lines']] for x in store])
at=table(['存储','字段B','1逻辑CPU Msession/s','2逻辑CPU','4逻辑CPU'],[[s,l,*[fmt(next(x['Msession_s_median'] for x in scaling_summary if x['storage']==s and x['length']==str(l) and x['threads']==str(n))) for n in [1,2,4]]] for s in ['M1_string_new','M2_string_reuse'] for l in [12,64,512]])
aff=[]
for c in configs:
 if c['logger_cpu']!=2:continue
 args=c['args'].copy();args[6]=1;other=next(o for o in configs if o['args']==args);p=next(x for x in pipe_summary if x['config_id']==c['config_id']);q=next(x for x in pipe_summary if x['config_id']==other['config_id']);aff.append(dict(handoff=p['handoff'],length=p['field_length'],demand=p['demand'],lifecycle=p['lifecycle'],cpu01=q['Mlog_s_median'],cpu02=p['Mlog_s_median'],delta_pct=(p['Mlog_s_median']/q['Mlog_s_median']-1)*100))
write('results/affinity_comparison.csv',aff)
zero0=find(1,4,zero=0);zero1=find(1,4,zero=1);zerodelta=(zero1['Mlog_s_median']/zero0['Mlog_s_median']-1)*100
stress=json.loads((root/'validation/stress/summary.json').read_text());assert stress['status']=='PASS' and stress['full_payload_errors']==0
normal=json.loads((root/'validation/normal/functional_summary.json').read_text());asan=json.loads((root/'validation/asan/functional_summary.json').read_text());assert normal['status']==asan['status']=='PASS'
timing=json.loads((root/'results/timing.json').read_text());assert timing['snapshots_match']
def cpustat(p):return {k:int(v) for k,v in (x.split() for x in p.read_text().splitlines())}
a,b=cpustat(root/'results/cpu_before.txt'),cpustat(root/'results/cpu_after.txt');quota={k:b[k]-a[k] for k in ['nr_throttled','throttled_usec']}
front=[]
for sparse in [False,True]:
 candidates=[find(1,h,sparse=sparse) for h in range(6)];best=min(candidates,key=lambda p:mrow(p,'worker_assembly')['cpu_ns_median']);front.append(('四字段' if sparse else '完整阶段规则',best['handoff'],mrow(best,'worker_assembly')['cpu_ns_median']))
header=f'''# 有状态通信架构：存储、六种槽位、生命周期与Protobuf组织实测

用户参考中的S1～S6格式比较已有证据，本轮保留索引；新增M1～M4、H1～H6、L1～L3、P1/P2及模块扩展性。**{len(configs)}个pipeline配置、{len(micro)}条隔离CPU样本、{len(pipe)}条双CPU样本、{len(proto)}条proto时间、{len(alloc)}条proto分配审计、30条存储审计、90条分配器扩展性记录**。所有数字来自实际执行，非模型预测。

第一版隐式子消息/旧setter的完整142配置数据没有删除，见[基线说明](baseline_implicit/README.md)。本轮发现并修正两项隐藏成本：[3.21.12生成代码与API核查](PROTOBUF_3_21_NOTES.zh-CN.md)。

## 先回答哪些想法成立

1. H6直接指向会变化的会话导出结构不是日志快照。单线程可重现：发出时值1926，随后更新，指针读到2935，而H5快照保持1926。主测H6使用每会话四阶段不可变版本，版本构造和内存都计入；另列源本来就冻结时的描述符成本。[反例](validation/consistency.json)
2. H5复制string_view也没有复制其字节。已实际验证原数据随后修改时，浅快照跟着变化，深拷贝保持旧值。本轮借用模式要求会话字符串写入后不再改变；真实多请求URL/重写字段需版本或深拷贝。[语义验证](validation/semantics.json)
3. `oneof`无法同时保留TLS与HTTP。P1/P2只在每条一个detail时作等信息比较；2313条fixture逐字节一致；P2混合日志另列，不用P1丢弃一半信息来制造更低耗时。
4. 结构体整块复制、按需填、模块快照的排名随字段数和布局变化。默认M2/64B字段中，完整规则下最小组装中位数为{front[0][1]}的{front[0][2]:.2f}ns；四字段规则下为{front[1][1]}的{front[1][2]:.2f}ns。它们包括公共元数据和临时字段，不是单条CPU store耗时，也不是整个流水线的最佳排名。
5. L3没有被当作无需维护的“魔法回收”：包含尾部排空、停机扫描、无接受记录、最后一条被丢弃的处理。ASan/UBSan模式尝试{stress['offered']:,}条，实际消费并完整核对{stress['accepted_and_fully_verified']:,}条，字段错误0；这是有限执行证据，不是100%安全的形式化证明。

## 统一工作负载与口径

GCC14.2/O3/C++20/march=native，官方Protobuf3.21.12；AMD EPYC9V74/KVM，5逻辑CPU可见，cgroup4CPU。主表使用显式optional子消息与mutable assign复用官方PB；主测CPU0/1，另10组CPU0/2；不能称为独占物理双核。

4096个活跃会话位置，四阶段轮转：TLS提取与日志→HTTP提取与完整日志→四字段风险日志→汇总日志后立即关闭。主三模块共12常驻字段，完整HTTP另加32B一次性XFF；标准完整阶段规则四条日志字段数分别6/13/4/4。每字段12/32/64/128/512B，另测15/16/48B；本轮256/512/2048B指arena块尺寸，不是消息大小。

归一化schema每模块最多2标量＋4字符串；导出基类实际88B，私有运行态另64B。每case预配置1～8个模块，池预留状态节点，阶段掩码模拟识别和字段就绪，不测真实协议解析和动态节点分配。所有实际结构尺寸和实现边界见[README](README.md)。

Worker隔离：仅组装和本地完整生命周期分列；Logger隔离：槽位读取＋官方PB编码；pipeline：真实两个线程、4096槽、真实关闭/回收/最终排空，满时自旋。**pipeline线程总CPU含忙等，不能当worker服务时间；吞吐倒数不是消息延迟。** 每组5次，micro样本至少80ms，pipeline至少300ms。以下括号为五次批平均最小～最大，不是置信区间或p99。

## H1～H6：同样M2存储、每字段64B

{ht}

H1是数据自包含记录，实际使用L0立即回收；其他借用方案用L3，不能强加H1不需要的引用回收。H4固定8模块镜像704B；H5最多8个16B描述符＋8个88B快照；H6指针描述符实际24B，引用的不可变版本在Session另占内存。固定容量槽位仍为未用模块预留空间，“没选协议”不等于物理槽位占0B。

H6的完整组装已将当前导出值复制进不可变版本，再写指针。只写指针的优势仅适用于源已不可变的情形，不能把必要的快照工作移出计时后声称适用于任意可变计数器。

## M1～M4：H5、L3、完整阶段规则

{st}

M1在关流时真正销毁字符串；M2 clear保留capacity；M3为64KiB页上的分级槽；M4为会话块列表，同一块内追加。字段512B而arena块256B时使用连续512B专用大块例外，未伪装成跨块连续string_view。

池页和保留字符串堆分别列，不能把M1最终堆容量0当作峰值内存0。进程峰值包含输入、预热、Session、PB、固定槽位和分配器保留；不是单独载荷RSS，也不能以RSS差反推精确碎片。

## 内存/分配审计与SSO

{memory}

SSO是短字符串内嵌存储，本机默认容量15B；对象可在堆上，不等于“走栈”。M2预热后没有每会话C++ new；M3/M4热池也不需要再扩页。但这并不证明某一方的硬件预取更强。本表是实际地址布局和容量观察，没有缓存失效计数；Slab同规格字段可能在同一页，不能先假定必定分散。

审计在独立二进制拦截operator new/delete，不宣称统计了所有malloc。每个完整会话6个常驻字符串，容量表中的SSO内容已经包含在string对象中，不能再重复加到元数据。

## 1、2、3、5、8模块

{sct}

扩展模块同时增加Logger实际编码字段数，不能把完整pipeline增长全归因于几个描述符。纯描述符列也包括公共头、掩码和临时字段组装，不采用参考的0.8/1.5/2.5ns估计。全量范围见原始与摘要CSV。

## L1、L2、L3：固定M2＋H5

{lt}

L1管理池化Session租约，含共享控制块和逐日志引用计数；最后引用在Logger时先重置M2再经反向环归还。L2用最终接受的关闭记录触发重置和返回，主测不会丢该标记；若要容忍最后关闭记录丢弃，需要独立关闭控制协议，本原型没有声称实现。

L3仍需SPSC原子消费计数，只省掉额外会话引用计数。顺风车可能继续持有已消费完的Session直到槽位复用，因此需池预算和最终扫描。三者给相同8193个Session预算；最低空闲数与退休峰值保留，不能只看平均CPU。

额外控制组保持L3其他条件相同，仅让Logger多写槽位重置字段：不额外写为{interval(zero0,'Mlog_s')} Mlog/s，额外写为{interval(zero1,'Mlog_s')} Mlog/s，中位变化{zerodelta:+.2f}%。这测量指令与缓存一致性的整体效果，不能把L2/L3协议整体差异都说成某个缓存行状态的后果。

## P1 oneof / P2 并列子消息

{pt}

本表字段64B；12/512B完整结果见[proto摘要](results/proto_summary.csv)。固定协议时正确重用当前oneof子对象，与每条Clear父消息是不同操作；交替协议会切换oneof活动成员。显式optional子消息的Clear保留子对象/字符串容量；隐式子消息的Clear在本版会delete。旧bytes指针长度setter还会创建临时string；最终主表使用官方mutable assign。schema/API交叉行的实际new/delete列分别验证这些成本。[API核查与旧账本修正](PROTOBUF_3_21_NOTES.zh-CN.md)没有将计数拦截器放进性能二进制。

相同字段号、类型且恰有一个detail时，本次2313条对照逐字节兼容；Protobuf一般并不承诺跨序列化器的唯一字段顺序。“混合TLS＋HTTP”改变oneof语义，P1只保留最后设置的detail，因此仅列P2等信息结果。

## 分配器扩展性补测

{at}

单位为百万完整会话构建/回收次每秒，没有日志编码或SPSC。每线程独立状态与字符串，分别绑定0..N-1；4线程在4CPU配额内。非线性可能来自分配器、频率、访存和虚拟CPU调度；没有锁等待归因，不能只看到M1分配就声称存在全局malloc锁瓶颈。

## 安全性、亲和性和证据

普通版97场景、ASan/UBSan版97场景，各实际尝试4,769,587条，包含部分尾部、多次绕环、瞬时停顿、所有模型和模块数。另{stress['runs']}组各1000万尝试，实际接受总计{stress['accepted_and_fully_verified']:,}条并完整验证；唯一丢弃组丢{stress['dropped']:,}条，是消毒器下尽快提供输入的压力场景，**不是生产丢失率估计**。回收时投毒，验证完整PB字段/载荷、计数守恒与最终池唯一归还；性能运行关闭这些检查。

CPU0/1被VM报告共享L1/L2，CPU2在另一组；这不能证明底层物理核心。10组0/2中位吞吐相对变化为{min(x['delta_pct'] for x in aff):+.2f}%～{max(x['delta_pct'] for x in aff):+.2f}%，逐项保留[亲和性对照](results/affinity_comparison.csv)，不把任意一对称为物理独占。

正式驱动总墙钟{timing['wall_seconds']:.2f}秒，驱动捕获的schema、C++源码和二进制SHA前后一致；原快照没有单列.pb.cc/.pb.h，另有固定protoc再生成一致性[核验](results/generated_validation.json)；全驱动记录的新增CPU节流{quota['nr_throttled']}次、{quota['throttled_usec']}微秒。分配器4线程阶段也包含在该总记录中。没有测真实DPDK、网卡、跨进程指针、双机、Flink、排队延迟百分位或硬件预取因果。

本轮适用于线程模型。跨进程借用应改为共享段＋offset并另测；已有[fork共享池实验](../ipc_sim/README.md)不代表本轮raw pointer槽位自动支持跨地址空间。

## 复现与全量数据

[源码/CMake/运行方法](README.md) · [逐项方案覆盖](COVERAGE.zh-CN.md) · [micro原始](results/micro.csv) · [pipeline原始](results/pipeline.csv) · [micro摘要](results/micro_summary.csv) · [pipeline摘要](results/pipeline_summary.csv) · [验证汇总](results/validation.json) · [消毒器压力](validation/stress/summary.json) · [分配审计](results/proto_alloc.csv) · [上轮格式比较](../dynamic_codec/RESULTS.zh-CN.md)
'''
from report_supplements import enrich
header,extra_validation=enrich(root,header,pipe_summary,micro_summary,proto_summary)
(root/'RESULTS.zh-CN.md').write_text(header)
validation={'status':'PASS','configurations':len(configs),'pipeline_rows':len(pipe),'micro_rows':len(micro),'micro_groups':len(mg),'proto_rows':len(proto),'proto_allocation_rows':len(alloc),'storage_allocation_rows':len(store),'allocator_scaling_rows':len(scaling),'all_pipeline_recorded_errors_zero':True,'pipeline_min_wall_s':min(float(x['wall_seconds']) for x in pipe),'micro_min_wall_ns':min(int(x['wall_ns']) for x in micro),'snapshots_unchanged':before==after,'cpu_throttle_delta':quota,'stress':stress,'functional_normal':normal,'functional_asan':asan,'supplementary':extra_validation}
(root/'results/validation.json').write_text(json.dumps(validation,indent=2)+'\n');print(json.dumps(validation))
