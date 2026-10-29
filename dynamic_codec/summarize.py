#!/usr/bin/env python3
"""Produce inspectable summaries and a Chinese result note from completed trials."""
from pathlib import Path
import csv,json,statistics,collections,hashlib

ROOT=Path(__file__).resolve().parent
OUT=ROOT/'results'
def rows(path):return list(csv.DictReader(path.open()))
raw=rows(OUT/'benchmark.csv')
assert len(raw)==1480 and all(int(x['elapsed_ns'])>=100_000_000 for x in raw)
assert [int(x['order_index']) for x in raw]==list(range(len(raw)))
timing=json.loads((OUT/'run_timing.json').read_text())
assert timing['return_code']==0 and timing['snapshot_consistent']
group=collections.defaultdict(list)
for x in raw:group[(x['encoder'],x['profile'],x['payload_bytes'],x['operation'])].append(x)
summary=[]
for key,a in sorted(group.items()):
    assert len(a)==5 and sorted(int(x['repeat']) for x in a)==list(range(5))
    v=[float(x['ns_per_op']) for x in a]
    summary.append(dict(zip(['encoder','profile','payload_bytes','operation'],key),repeats=5,ns_median=statistics.median(v),ns_min=min(v),ns_max=max(v),wire_bytes_mean=float(a[0]['wire_bytes_mean'])))
with (OUT/'summary.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(summary[0]));w.writeheader();w.writerows(summary)

opt=rows(OUT/'optimized_encode.csv');assert len(opt)==90
og=collections.defaultdict(list)
for x in opt:og[(x['encoder'],x['profile'])].append(float(x['ns_per_op']))
osummary=[dict(encoder=k[0],profile=k[1],repeats=len(v),ns_median=statistics.median(v),ns_min=min(v),ns_max=max(v)) for k,v in sorted(og.items())]
assert all(x['repeats']==5 for x in osummary)
with (OUT/'optimized_summary.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(osummary[0]));w.writeheader();w.writerows(osummary)
def get(name,profile='full',op='encode',payload='256'):
    return next(x for x in summary if x['encoder']==name and x['profile']==profile and x['operation']==op and x['payload_bytes']==payload)
def ogt(name,profile='full'):
    return next(x for x in osummary if x['encoder']==name and x['profile']==profile)
def fmt(n):return f'{float(n):.2f}'
def tab(head,body):return '\n'.join(['| '+' | '.join(head)+' |','|'+'|'.join(['---']*len(head))+'|']+['| '+' | '.join(map(str,r))+' |' for r in body])
fixed='pb_generated_fixed_plan_selective';static='pb_generated_fixed_static_full';varint='pb_generated_varint_plan_selective'
hybrid='custom_hybrid_tv_tlv';emit='custom_proto_emit_op_fixed'
alias='upb_v29_3_dynamic_minitable_fixed_viewinput_decodealias_prealloc_arena'
copy='upb_v29_3_dynamic_minitable_fixed_viewinput_decodecopy_prealloc_arena'
labels=[('官方PB固定宽度，静态写满',static),('官方PB固定宽度，动态选字段',fixed),('官方PB变长整数，动态选字段',varint),('自定义Hybrid TV/TLV',hybrid),('ProtoEmitOp固定宽度，检查版',emit),('upb固定宽度，别名解析',alias),('upb固定宽度，复制解析',copy),('C++ DynamicMessage固定宽度／反射','pb_dynamicmessage_fixed_reflection_reuse')]
fulltable=tab(['实现','编码 ns','解码 ns','同线程编码后立即解码 ns','编码后字节'],[[label,fmt(get(n)['ns_median']),fmt(get(n,op='decode_all')['ns_median']),fmt(get(n,op='roundtrip')['ns_median']),fmt(get(n)['wire_bytes_mean'])] for label,n in labels])
subtable=tab(['实现','动态四字段编码 ns','动态四字段解码 ns','三计划切换编码 ns','三计划切换解码 ns'],[[label,fmt(get(n,'subset')['ns_median']),fmt(get(n,'subset','decode_all')['ns_median']),fmt(get(n,'mixed')['ns_median']),fmt(get(n,'mixed','decode_all')['ns_median'])] for label,n in labels if n!=static])
optlabels=[('官方PB动态选字段',fixed),('ProtoEmitOp检查版',emit),('ProtoEmitOp精确长度、已验证计划','custom_proto_emit_exactsize_fixed')]
opttable=tab(['同一补充二进制内重新比较，编码ns','满字段','四字段','三计划切换'],[[label]+[fmt(ogt(n,p)['ns_median']) for p in ['full','subset','mixed']] for label,n in optlabels])
plan=rows(OUT/'plan_compile.csv')
cold=statistics.median(float(x['ns_per_op']) for x in plan if x['operation'].startswith('json_parse'))
hot=statistics.median(float(x['ns_per_op']) for x in plan if x['operation'].startswith('hot_mixed'))
def cpustat(p):return {k:int(v) for k,v in (x.split() for x in p.read_text().splitlines())}
a,b=cpustat(OUT/'cpu_before.txt'),cpustat(OUT/'cpu_after.txt')
validation={'main_rows':len(raw),'groups':len(summary),'repeats_per_group':5,'all_main_trials_at_least_100ms':True,'main_source_and_binary_snapshot_unchanged':True,'main_cgroup_added_throttled_periods':b['nr_throttled']-a['nr_throttled'],'optimized_rows':len(opt),'optimized_validation':json.loads((OUT/'optimized_validation.json').read_text()),'java_interop':json.loads((ROOT/'interop/results/summary.json').read_text()),'raw_sha256':hashlib.sha256((OUT/'benchmark.csv').read_bytes()).hexdigest()}
(OUT/'validation.json').write_text(json.dumps(validation,ensure_ascii=False,indent=2)+'\n')

doc=f'''# 动态字段编码实测：Hybrid TV/TLV、ProtoEmitOp 与 Google upb

日期：2026-10-29。本报告来自本次实际执行的源码与CSV，不采用粘贴参考中的预测数字。主矩阵1480条、296组，每组5次；另有90条编码优化补充测量。正文时间均是5次批平均耗时的中位数，不是单消息p99或网络时延。

## 结论

三种方案都已实现或接入并实测，但参考给出的13.5/19.2/27.5ns等数字未被直接复现。Hybrid在这组字段直读任务中较快；Protobuf指令表直写的收益明显依赖实现与字段数量；upb运行期MiniTable（消息布局小表）配置在此测试中慢于官方生成的C++代码。upb仍提供比手写原型完整得多的Protobuf运行时能力。

“不重新编译就选择已有字段”并非这三个方案独占：官方生成的Protobuf也可以按配置set/clear字段。真正运行期创建未知消息类型，则可比较upb与C++ DynamicMessage（动态消息）的反射路径；本轮两条路径都执行了。新输入值从哪里提取，仍需业务入口提供，动态格式不能凭空生成新业务字段。

## 测量口径

- Linux/KVM、AMD EPYC 9V74，逻辑CPU0绑核；可见5逻辑CPU，cgroup配额4 CPU、内存16GiB；GCC14.2、O3、C++20、march=native。正式主矩阵约{timing['wall_seconds']:.1f}秒；新增CPU限流次数为{validation['main_cgroup_added_throttled_periods']}。
- Protobuf C++3.21.12与上次基线一致，本次重新运行；upb固定为官方protobuf v29.3源码，commit `b407e8416e3893036aee5af9a12bd9b6a0e2b2e6`，实际编译34个C源及同包utf8_range。版本不同，因此不把差异全归结为实现机制。
- 4096条变化事件，随机种子与旧基准相同：5个u64、4个u32，加0/64/256/1024B尾部。表中256B指尾部，整条wire（线上字节）大于256B。
- 编码包括字段填充／源字段读取、格式写入、输出到拥有独立内存的std::string；缓冲容量重用。upb编码时可借用调用期内有效的源payload，但在释放Arena前复制编码结果到独立输出。
- 解码“全部字段”读取9个标量及payload长度、首尾字节；不遍历整个payload。功能校验另行比较所有字段与完整payload。
- `full`为10字段；`subset`为id、seq、type、payload四字段；`no_payload`为9个标量；`mixed`逐消息交替引用上述三个已编译、不可变计划。所有编码器都使用预先选好的Plan引用，不在消息热循环解析JSON或比较配置名称。
- 每组先预热，正式每轮至少100ms，外层5次随机打乱所有任务次序。每轮实际迭代数、墙钟时间、线程CPU时间和校验和均在CSV；已确认源码与二进制快照前后不变。

## 满字段、256B尾部

{fulltable}

**解码所有权不同：** Hybrid和手写PB视图会借用输入payload；官方C++ ParseFromArray构造自己的消息内容；upb分别测了别名和复制两条路径。不能把收益全说成“删除varint更快”。同线程编码后立即解码可能受缓存等因素影响，不要求等于独立两列相加。

## 动态选择字段和切换计划

{subtable}

切换前清空复用消息，已检查full→subset→no_payload→full不会泄漏旧字段。这里证明JSON能选择既有字段、计划可无重编译地切换；没有证明多线程并发热更新和旧计划回收的生产级安全性。

JSON解析、验证并编译三个计划的中位耗时约{cold/1000:.2f}µs；单独选择既有计划引用约{hot:.2f}ns。后者只是这个小操作的局部开销，不是整套热更新成本。

## 指令表编码的实现优化补充

第一版按上界扩大std::string，再缩到实际长度，并逐字段重复安全检查。补充版本信任已验证的10字段输入计划，缓存本条标量，计算精确输出长度后直接写入，固定宽度字段采用小端安全memcpy；两版和官方PB在同一个补充二进制内重新比较：

{opttable}

补充版本73728条语义与字节等价检查通过，输出与检查版逐字节相同。它面向本实验已验证的U32/U64/bytes入口子集，不是通用Protobuf库；varint补充数据也完整保存在[optimized_encode.csv](results/optimized_encode.csv)。不要把检查版结果当作该方案的性能上限，也不要把优化版推广到未实现的类型。

## upb的真实路径与边界

MiniDescriptor（紧凑模式描述）在初始化阶段通过官方API构建MiniTable，字段指针按field number缓存，没有使用生成的Event MiniTable。该模式没有启用生成式FastTable专用快表，不代表upb所有部署的最快路径。

每条操作都包括预配64KiB Arena（区域分配器）的初始化、消息建立、解析／编码和释放；只在构造时预配scratch，不每次清零64KiB，显式拒绝扩容分配器防止堆回退。另测了编码输入payload先复制到Arena的敏感性对照，见CSV的copyinput行；encode借用源数据和decode别名读取是两个独立选择。

别名路径已实测检查StringView（字符串切片）指针落在输入缓冲内，并通过输入修改验证与复制路径的差别。输入缓冲必须活到所有读取结束；小输入的内部回退机制也意味着不能宣称设置AliasString就总是完全无复制。

本适配器编码payload上限8192B、Arena64KiB属于实验资源限制，不是upb格式的固有限制。已通过大字段耗尽Arena的安全失败测试。upb C API/ABI并非稳定接口，集成应固定版本并封装在私有适配层。[官方upb说明](https://github.com/protocolbuffers/protobuf/blob/v29.3/upb/README.md)

## 参考讨论中必须纠正的内容

1. **数字缺少可复现实验依据。** 新表只使用本轮CSV；上轮SBE/FlatBuffers等结果未冒充本轮新测。
2. **消息大小不同。** 附件示例6×u64＋4×u32＋160B尾部仅224B逻辑值，并非我们使用的256B尾部消息。它给出的整条PB256B等长度不能混用。
3. **TLV代码的u64分支用uint32_t读取。** 会丢高32位；非对齐reinterpret_cast还涉及C++规则。新实现使用安全读取。
4. **长度与容量。** uint16_t长度不能直接截断大payload，type3保留值不能当bytes；所有游标、类型和容量都要验证。“无varint”也不等于没有分支或版本治理。
5. **upb生命周期。** 参考先Free编码Arena，再使用wire指针并复用同一scratch解码，可能访问已失效或已覆写内存。新实现分清模式内存、临时消息和独立wire输出。
6. **兼容范围。** 自定义ProtoEmitOp输出已被官方C++和Java解析，但只覆盖这里的无符号标量和bytes子集；没有实现嵌套、map、oneof、完整presence及所有未知字段行为。手写checked视图比官方parser更严格的拒绝规则也单独记录，不能宣称“100%支持全部Protobuf功能”。
7. **动态能力。** 官方C++既可按配置给生成类选择字段，也可用DescriptorPool＋DynamicMessage在运行期建立类型；所谓“只有upb能动态”不成立。[官方DynamicMessage文档](https://protobuf.dev/reference/cpp/api-docs/google.protobuf.dynamic_message/)

## 正确性、复现与原始证据

主harness对91个配置×4096条消息检查逻辑结果；标准PB输出由官方C++逐字段及完整payload校验。Java独立互操作覆盖168条消息、324项派生检查和17项定向边界检查；自定义实现另通过138575项功能检查及地址／未定义行为检测。upb包含别名、输入保持、最大整数、二进制payload及内存耗尽检查。

- [主原始数据](results/benchmark.csv) · [主摘要](results/summary.csv) · [补充优化原始数据](results/optimized_encode.csv)
- [计时与资源记录](results/run_timing.json) · [验证记录](results/validation.json) · [构建版本](upb/build_metadata.json)
- [独立代码核查](research_audit.md) · [测试方法审查](review.md) · [Java互操作结果](interop/RESULTS.md)
- [如何构建和运行](README.md)

本轮只测编码和读取，没有网络、DPDK网卡或Flink整链路测量。自定义实现是可运行基准原型；旧的中止轮次未纳入正式数据。
'''
(ROOT/'RESULTS.zh-CN.md').write_text(doc)
print(json.dumps({'main_rows':len(raw),'groups':len(summary),'optimized_rows':len(opt),'full_hybrid_encode_ns':get(hybrid)['ns_median'],'full_emit_optimized_ns':ogt('custom_proto_emit_exactsize_fixed')['ns_median'],'plan_compile_us':cold/1000},ensure_ascii=False))
