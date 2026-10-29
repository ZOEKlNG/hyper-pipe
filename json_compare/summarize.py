#!/usr/bin/env python3
"""Validate saved measurements and generate the Chinese report; no performance run."""
from pathlib import Path
from collections import defaultdict
import csv,hashlib,json,statistics
root=Path(__file__).resolve().parent
rows=list(csv.DictReader((root/'results/benchmark.csv').open()))
keys=['library','schema','profile','tail_bytes','stage'];groups=defaultdict(list)
for row in rows:groups[tuple(row[k] for k in keys)].append(row)
assert len(rows)==1600 and len(groups)==320
assert all(len(v)==5 and {int(x['repeat']) for x in v}==set(range(5)) for v in groups.values())
assert all(int(x['wall_ns'])>=80_000_000 and int(x['iterations'])>0 and 0<int(x['cpu_ns']) for x in rows)
assert sorted(int(x['order_index']) for x in rows)==list(range(1600))
for x in rows:
    assert abs(int(x['cpu_ns'])/int(x['iterations'])-float(x['cpu_ns_per_event']))<1e-6
    assert abs(int(x['wall_ns'])/int(x['iterations'])-float(x['wall_ns_per_event']))<1e-6
before=json.loads((root/'results/snapshot_before.json').read_text());after=json.loads((root/'results/snapshot_after.json').read_text())
assert before==after
source_matches={}
for name,sha in before.items():
    if name=='bench':continue # A relocated rebuild may legitimately differ.
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==sha,name
    source_matches[name]=True
timing=json.loads((root/'results/run_timing.json').read_text());assert timing['return_code']==0 and timing['snapshot_consistent']
deps=json.loads((root/'dependencies.json').read_text())
nlohmann=next(x for x in deps if x['name']=='nlohmann/json')
external_header_sha=hashlib.sha256((root/nlohmann['repo_path']).read_bytes()).hexdigest()
assert external_header_sha==nlohmann['sha256']
external_check={'header':nlohmann['repo_path'],'before_run_dependency_manifest_sha256':nlohmann['sha256'],'post_run_current_sha256':external_header_sha,'match':True,'note':'This external single header was recorded in dependencies.json before the run and checked separately afterward; it is outside the vendor snapshot map.'}
(root/'results/external_header_validation.json').write_text(json.dumps(external_check,indent=2)+'\n')
def stat(path):return {k:int(v) for k,v in (x.split() for x in path.read_text().splitlines())}
a,b=stat(root/'results/cpu_before.txt'),stat(root/'results/cpu_after.txt')
quota={k:b[k]-a[k] for k in ['nr_throttled','throttled_usec']}
functional=json.loads((root/'results/functional_validation.json').read_text());assert functional['status']=='PASS'
summary=[]
for key,values in sorted(groups.items()):
    ns=[float(x['cpu_ns_per_event']) for x in values]
    summary.append(dict(zip(keys,key),repeats=5,cpu_ns_median=statistics.median(ns),cpu_ns_min=min(ns),cpu_ns_max=max(ns),wall_ns_median=statistics.median(float(x['wall_ns_per_event']) for x in values),encoded_bytes_mean=statistics.mean(float(x['encoded_bytes_mean']) for x in values),decode_input_bytes_mean=statistics.mean(float(x['decode_input_bytes_mean']) for x in values)))
with (root/'results/summary.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(summary[0]),lineterminator='\n');w.writeheader();w.writerows(summary)
libs=['protobuf_fixed','nlohmann_json','yyjson','glaze_9_1_0_typed_strict_owning_reuse']
labels=dict(zip(libs,['官方固定宽度 Protobuf','nlohmann/json 3.12','yyjson 0.13','Glaze 9.1']))
def g(lib,profile,stage,shape='nested_http',tail=256):
    return next(x for x in summary if tuple(x[k] for k in keys)==(lib,shape,profile,str(tail),stage))
def ns(lib,profile,stage,shape='nested_http',tail=256):return g(lib,profile,stage,shape,tail)['cpu_ns_median']
def val(lib,profile,stage,shape='nested_http',tail=256):
    x=g(lib,profile,stage,shape,tail);return f"{x['cpu_ns_median']:.1f}（{x['cpu_ns_min']:.1f}–{x['cpu_ns_max']:.1f}）"
def table(headers,data):return '\n'.join(['| '+' | '.join(headers)+' |','|'+'|'.join('---' for _ in headers)+'|']+['| '+' | '.join(map(str,x))+' |' for x in data])
text_table=table(['库','字段→编码 ns','仅解析 ns','解析并完整取字段 ns','编码字节'],[[labels[l],val(l,'utf8_escaped','encode_fields'),val(l,'utf8_escaped','parse_only'),val(l,'utf8_escaped','decode_fields'),f"{g(l,'utf8_escaped','encode_fields')['encoded_bytes_mean']:.1f}"] for l in libs])
binary_table=table(['库','原始 bytes→编码 ns','解析→原始 bytes ns','编码字节'],[[labels[l],val(l,'binary_base64','encode_raw'),val(l,'binary_base64','decode_raw'),f"{g(l,'binary_base64','encode_raw')['encoded_bytes_mean']:.1f}"] for l in libs])
json_only=table(['库','已备文本→编码 ns','仅解析 ns','解析并取全部文本字段 ns'],[[labels[l],val(l,'binary_base64','encode_fields'),val(l,'binary_base64','parse_only'),val(l,'binary_base64','decode_fields')] for l in libs])
prepared_table=table(['库','UTF-8/转义档仅输出预备对象 ns','ASCII/Base64档仅输出预备对象 ns'],[[labels[l],val(l,'utf8_escaped','encode_prepared'),val(l,'binary_base64','encode_prepared')] for l in libs])
sizes=table(['尾部 B','库','平铺完整编码 ns','平铺完整解码 ns','嵌套完整编码 ns','嵌套完整解码 ns'],[[tail,labels[l],f"{ns(l,'binary_base64','encode_raw','flat',tail):.1f}",f"{ns(l,'binary_base64','decode_raw','flat',tail):.1f}",f"{ns(l,'binary_base64','encode_raw','nested_http',tail):.1f}",f"{ns(l,'binary_base64','decode_raw','nested_http',tail):.1f}"] for tail in [0,64,256,1024] for l in libs])
pb_enc=ns(libs[0],'binary_base64','encode_raw');gl_enc=ns(libs[3],'binary_base64','encode_raw')
pb_dec=ns(libs[0],'binary_base64','decode_raw');yy_dec=ns(libs[2],'binary_base64','decode_raw')
doc=f'''# 三种 JSON 库与固定宽度 Protobuf：本机实测

本轮采用 nlohmann/json 3.12.0 通用基线、yyjson 0.13.0 与 Glaze 9.1.0 两个高性能候选，并重新运行官方 Protobuf 3.21.12 对照。**C++ 标准库没有官方 JSON 实现；这里的“标准库”按常见通用库理解。** 选择覆盖通用 DOM（文档对象模型）、高性能 DOM、静态类型直接映射三种 API，不是全球库排名。

## 结论先看

在本机示例嵌套结构、256B 原始二进制尾部下：

- 完整编码：PB约 **{pb_enc:.1f}ns**，Glaze＋共用 Base64约 **{gl_enc:.1f}ns**，是PB的 **{gl_enc/pb_enc:.2f}倍耗时**。Glaze比本轮另两种JSON编码快，但仍需考虑载荷转换。
- 完整解码并复制所有字段到统一业务记录：PB约 **{pb_dec:.1f}ns**，yyjson＋Base64还原约 **{yy_dec:.1f}ns**。yyjson与Glaze在此档位很接近，五轮范围重叠，不宜认定所有解码场景的固定赢家。
- JSON自身成本与原始二进制成本差别明显：若输入已经是ASCII/Base64文本，Glaze字段填充＋JSON输出约 **{ns(libs[3],'binary_base64','encode_fields'):.1f}ns**；不能把这个数字当作原始bytes完整编码成本。
- nlohmann/json与两个高性能候选的成本差距明显。不能用旧报告中一个通用JSON库的结果概括全部JSON方案，也不能从这组整数/字符串消息推导浮点、超深嵌套、动态大对象的排名。

下面全部为**五轮批平均 CPU 服务成本的中位数**；括号是五轮最小到最大值，单位ns（纳秒）。不是消息p99，也不是跨核/跨机端到端延迟。

## 含转义与UTF-8文本：256B尾部

结构为5个64位无符号整数、4个32位无符号整数，以及common/http下三个字符串；变长尾部分为128B、64B、64B。文本合成分布混合ASCII、中文、引号、反斜线、换行、制表符和NUL，需要正常转义与还原。它是转义压力样例，不代表真实URL/日志的统计分布。

{text_table}

DOM库的“仅解析”还未完成全部字段查找；Glaze直接解析到静态C++类型，已完成字段映射。右侧完整取字段路径统一读取全部九个标量，并将所有字符串复制进拥有式业务Record。业务若直接使用已解析对象或字符串引用，可省掉统一Record拷出，因此两列都保留。

## 任意二进制：包含Base64的完整路径

相同逻辑原始字节：PB bytes直接携带，JSON每个字符串分别做标准带padding Base64。下表包含实时转换、对象填充、编码；解码包含解析、全部字段提取和Base64严格还原。

{binary_table}

这里Base64使用三JSON库共用的标量实现，**不是向量化Base64库的性能上限**。库本身成本另列，避免将此实现的转换成本归因于JSON库。256B分为三个字段时，Base64文本共348B，而单一256B字段为344B，来自各字段独立padding；最终JSON还含键名、数字等。

## JSON本身：已备ASCII/Base64文本

原始256B在计时外已转换为348B的三个ASCII字段；下表不含Base64。这也提供一个没有字符串转义的ASCII读写档位，区别于上面的UTF-8/转义压力档。

{json_only}

本表中的PB仍存原始256B二进制，不先转换Base64；比较的是相同逻辑载荷、各自合理的格式表示，不是将相同长度的文本强塞给所有格式。

## 只输出已经填好的对象

{prepared_table}

此阶段轮转4096个预填好的对象；“字段→编码”阶段使用一个复用编码器和4096条变化输入。**不同工作集会让仅输出阶段反而更慢，不能相减当作负的字段填充成本。** 每个阶段独立有意义，不能据此分解或相加成链路延迟。

## 消息大小和结构敏感性

以下为原始二进制完整路径，JSON包含共用Base64；表中只列中位数，全部五轮范围见CSV摘要：

{sizes}

平铺是一个变长字段，嵌套是三个字段；因此两者同时改变嵌套与字段数。固定宽度PB只固定标量值编码宽度，proto3零值省略、bytes与子消息长度前缀仍按官方生成代码执行。JSON始终输出声明的全部字段。

## 对worker/日志线程的意义

本轮是编码/解码CPU实验，没有运行新的异步队列测试。若数据本来就是二进制、下游可用PB，固定宽度PB继续适合作为默认数据面格式；若需要JSON兼容，静态结构优先评估Glaze，动态DOM需求可评估yyjson。通用nlohmann/json适合功能与便利性优先、CPU预算足够的路径。

每条日志的服务成本可作粗预算：所需CPU份额约为 `日志条数/秒 × ns/条 ÷ 10^9`。例如本轮256B嵌套二进制编码，按100万日志/秒计，PB约{pb_enc/10:.1f}%、Glaze含共用Base64约{gl_enc/10:.1f}%的一个逻辑CPU；它不包括真实业务、队列、路由、网络和停顿，也不是抓包速率。是否放worker上还要结合上一轮[交接实验](../core_handoff/RESULTS_SERVICE.zh-CN.md)。

JSON的64位字段用精确十进制数字，C++三库完整往返成功；**不是官方ProtoJSON**，后者将64位整数映射为字符串。下游若经JavaScript普通Number处理，会有额外精度约束；本轮没有测Java/Flink JSON接入。

## 方法、正确性与边界

- GCC14.2，O3、C++23、march=native；AMD EPYC 9V74/KVM，单逻辑CPU0。可见5个逻辑CPU、cgroup配额4CPU；不能证明底层物理核独占。[环境](environment.json)
- 2种结构×4档尾部×2类输入×4种codec（编解码器）；4个基础阶段，二进制档多2个阶段，每组5次，**320组共1600行**。每条保留样本至少80ms；配置与阶段固定种子随机打散。
- 所有JSON解析器收到相同规范文档字节和声明键序；功能检查另覆盖重排键。无预设键序、借用输入、原地解析或跳过语法/UTF-8校验。JSON编码只接受本实验生成的合法UTF-8字符串。
- 使用各库合理的公开API：nlohmann固定编码节点已预定位，但dump返回新字符串、parse构建新DOM；yyjson使用自有字符串、缓冲输出及池化解析；Glaze使用可复用的拥有式静态类型。分配模型不同，不能把差值全归因于某一条指令或SIMD（单指令多数据）。[API与复现说明](README.md)
- 编译、4096条输入生成、prepared对象建立、预热和初次扩容在计时外；热路径内必要的分配/回收、字段复制、校验与转换按阶段计入。每批读线程CPU时钟，不逐消息读时钟。
- 独立功能路径：8192条自身完整往返、18432条三库交叉完整往返、128次输入生命周期、120个错误输入拒绝、16个Base64正反例，全部通过。性能循环只保留轻量结果消费与编译器屏障，未逐条重复完整内容比较。[验证结果](results/functional_validation.json)
- 主测墙钟{timing['wall_seconds']:.2f}秒；源码/依赖/二进制SHA前后一致；记录的CPU节流增量为{quota['nr_throttled']}次、{quota['throttled_usec']}微秒。没有采集硬件缓存或分配次数计数，不能推断精确因果。[运行证据](results/run_timing.json)
- DOM parse_only只做通用语法解析，字段类型/范围在read_back检查；Glaze已知字段的类型验证在解析时完成。完整取字段才具有本适配器共同的拥有式业务记录输出。
- 生产语义限定为完整声明字段且无重复键；库对缺失键、重复键并非完全相同，本基准不是通用不可信消息协议的完整安全验证。没有真实队列、网卡、DPDK、双机发送、Flink或延迟百分位测量。

## 查看证据与复现

[全部原始CSV](results/benchmark.csv) · [全部五轮摘要](results/summary.csv) · [综合校验](results/validation.json) · [源码与构建](README.md) · [上游依赖与SHA](dependencies.json) · [官方资料核查](research_audit.md)
'''
(root/'RESULTS.zh-CN.md').write_text(doc)
validation={'status':'PASS','raw_rows':len(rows),'groups':len(groups),'repeats':5,'minimum_wall_ns':min(int(x['wall_ns']) for x in rows),'source_dependency_binary_unchanged_during_run':before==after,'current_source_dependency_hashes_match':len(source_matches),'external_nlohmann_header_match':external_check,'cpu_quota_delta':quota,'functional':functional,'notes':'Measurements are isolated batch CPU means; no p99, network or DPDK. Relocated binary hashes need not match original build.'}
(root/'results/validation.json').write_text(json.dumps(validation,indent=2)+'\n')
print(json.dumps(validation))
