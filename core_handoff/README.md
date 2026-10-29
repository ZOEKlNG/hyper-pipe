# Worker 与日志线程交接：官方固定宽度 Protobuf 实测

**后续核查修正（2026-10-29）：** 本轮3.21.12的`set_bytes(ptr,len)`会构造临时`std::string`，旧计时保留，但载荷复制账本须多计这一层；`mutable_bytes()->assign(...)`可避免它。详见[生成代码/API核查与重测](../stateful_handoff/PROTOBUF_3_21_NOTES.zh-CN.md)。


先读 **[服务成本、完整接力与丢弃实验报告](RESULTS_SERVICE.zh-CN.md)**。本轮重点回答“Worker 已经把字段收集到一块内存，是否与直接编码 PB 成本相近”。Worker 指数据生产线程；Logger 指后台日志线程；slot 是队列槽位；wire 是编码后的线格式；tail 是变长尾部；schema 是消息结构定义；payload 是业务载荷。所有网络发送均为内存 sink（模拟消费端）。

## 复现

```bash
DEP_ROOT="$PWD/../deps/root" bash build_all.sh
./handoff --smoke
./handoff_cpu2 --smoke
# 以下会覆盖随仓库保存的原始结果；请先备份或在独立 checkout 中执行。
python3 run_dual.py
python3 summarize.py results/handoff_results.csv
python3 run_service.py
python3 run_cpu2.py
python3 summarize.py results_cpu2/handoff_results.csv
python3 summarize_service.py
```

需要 Linux x86_64、可用逻辑 CPU0/CPU1/CPU2、Python3 标准库，以及官方 Protobuf 3.21.12 的 C++ headers/library。精确依赖可用仓库根目录的 `python3 deps/fetch.py` 提取，但仍需匹配的运行库；也可安装兼容的系统开发包。`build_all.sh` 默认使用 `../deps/root`，没有该目录时使用系统头文件/库。生成文件已提交，默认不运行 protoc；更换 Protobuf 版本需设置 `REGENERATE_PROTO=1` 并提供匹配的 protoc，此时是新的版本测量。

`build.sh`、`build_cpu2.sh` 是原始计时所用脚本，保留其绝对默认依赖路径以匹配历史快照；新 checkout 优先用 `build_all.sh`。`run_service.py` 是测量完成后整理的复现包装器，不追称为原始驱动。原始测量的命令、日志、源码/二进制 SHA、资源记录仍原样保存；二进制未上传，重新构建的哈希可能因路径/工具链不同而变化。

`run_dual.py` 冻结并比较测量源码/生成代码/二进制 SHA-256，记录完整命令、cgroup CPU 前后、UTC 墙钟、子进程资源使用。主矩阵期间应暂停其他重负载。`prepare_cpu2.py` 可从未修改的原始 `handoff.cpp` 重建已提交的 CPU0/2 变体；详见 [亲和性补测](CPU2_RESULTS.md)。

## 隔离服务成本与有界丢弃

`micro.cpp` 单线程绑定 CPU0，分别测字段提取、直接填原生槽位、临时结构转存、完整 PB 编码、只填池化 PB 对象及日志侧阶段。共 2 种结构×4 档尾部，9 个无压力阶段、5 个带压力前台阶段，每组 5 轮，共 560 行。每轮至少 80ms，用整批线程 CPU 时钟计时，不逐条读时钟；没有真实入队、忙等或跨核一致性成本。每种上下文使用 4096 个预热目标槽位/对象，不能将这些成本直接相加预测 queue256 双核容量。

合成压力为 32MiB 表上每事件 8 次依赖读取，不代表真实深度包检测（DPI）业务；没有硬件缓存失效计数。各轮配置随机排序；`results/micro.csv`、`micro_summary.csv` 保存原始值与五轮范围。

`drop_demo.cpp` 在 CPU0/CPU1 上以计划 25 万条/s 尝试 50000 条输入，测试 256/4096 槽与日志线程起始暂停 0/10ms，满时丢弃新记录（drop-new）。共 36 行，检查输入守恒、最终排空和容量上限。源调度落后会追赶，实际持续时间与最大迟到均已记录，不是生产丢失率预测；也不用于服务 CPU 测量。它只对队列协议作运行验证，完整字段正确性由下述 smoke 路径独立检查。

## 五种交接方式

| 模式 | Worker 做什么 | 日志核做什么 | 变长字节经过的拷贝层数 |
|---|---|---|---:|
| A_native_slot | 在最终原生队列 slot 中写 scalar 和 3 个 tail span | 从 slot 调官方 PB setter，再序列化到最终 frame | 4 |
| A_scratch_then_slot | 写私有 scratch native，再复制到最终 slot | 同 A | 5 |
| B_worker_pb_batch_copy | 官方 PB setter + 序列化到 queue wire slot | 复制已编码 wire 到最终 frame | 4 |
| B_direct_worker_pb | 同 B | 直接消费 queue 中 wire，不建最终 frame | 3 |
| D_shared_pb_object | 填当前槽位独占的官方 PB 对象，不序列化 | ByteSizeLong + 序列化该 PB 对象到最终 frame | 3 |

拷贝数按每个变长业务字节完整经过的存储层计，不是 `memcpy` 调用次数，不含 scalar、长度前缀、队列元数据或网络。A 的3.21.12 bytes setter经过临时string，再复制到PB自有字符串；完整路径为四层。D 是同进程内移交对象所有权，不是 DPDK mbuf 引用方案 C，也不能跨进程直接传 PB 对象指针。

`B_direct` 同时绕过 64KiB 聚合 frame 和其中的 4B 记录长度前缀，表示“不聚合、直接移交原 wire”的完整变体，不能作为只改变一条 memcpy 的单变量实验。最终 sink 只消费长度/首尾字节和 compiler escape，没有网络或异步 DMA；若真实网卡/网络线程持有 wire，slot 归还必须延后到发送完成。

## 等价输入与 Schema

- 输入为 4096 个变化、不可变的模拟 packet bytebuffer，64B 前缀 + 64B native scalar header + 16B 长度表 + tail。Worker 每条从该 bytebuffer 提取 Header 与 URL/host/cookie 的 source spans。它模拟“字段已经定位”的提取，不模拟真实 Ethernet/IP/HTTP 解析、不运行 DPDK，也没有真实 mbuf 生命周期。
- flat 使用旧 `FixedEvent` 的 5 个 fixed64 +4 个 fixed32 +1 个 bytes；nested 将相同 9 个逻辑 scalar 分为 CommonHeader 的 7 个与 HttpLog 的 status/method（对应 flat type/flags），tail 拆成 url/host/cookie 3 个 bytes，并嵌入 `GatewayLogEvent.common + oneof http`。
- tail 总长64/256/1024B，三个 span 分别占1/2、1/4、剩余1/4，随机内容包含任意二进制字节。三个字段在模拟 packet 中连续，原生槽位逐 span 复制。
- fixed32/64 只固定标量的 wire 宽度；proto3 零值省略、bytes 长度与嵌套长度仍会改变 wire 长度。`wire_bytes_mean` 是实际完整 PB 消息长度，不是输入 tail 长度，且不含聚合层4B前缀。
- PB 子对象/字符串复用：主路径不 Clear 父消息，固定 HTTP oneof 使用同一个 mutable_http 子对象；每次覆盖全部 scalar 和全部 bytes，包括0/empty。每条在设置字段之后重新 ByteSizeLong，更新嵌套 cached sizes，再 SerializeWithCachedSizesToArray。
- D 在计时外为每槽位创建 PBState，并用当前 tail 长度预热所有字符串容量；Worker 与 Logger 通过 release/acquire 交接该对象，二者不会并发访问同一槽位对象。`pb_object_space_bytes` 是两个 PB 子对象 SpaceUsedLong 的总和估计，`ring_allocated_bytes` 单列。

## 队列、计时与限制

- 一个 Worker（CPU0）与一个 Logger（CPU1），单生产者单消费者（SPSC）队列，固定容量，head/tail 独立 cache line；release 发布、acquire 接收，Logger 归还 tail 后 Worker 才能复用 slot。未使用 MPSC/多生产者竞争。
- batch1/32 是队列发布/消费的批次；Logger 的64KiB frame 是另一个聚合层，二者不同。
- 主 queue256：2种schema×3种tail×2种batch×5种mode；queue4096 仅tail256补充，合计80配置，每配置5次、每次实测≥0.3秒。配置次序在每个 outer repeat 随机打散，计时外单独校准条数，短于0.3秒会增加条数重试，保留的条数确实执行。
- 这是饱和容量实验：队列满时 Worker 自旋，Logger 空时自旋；`dropped=0`。它不代表生产系统必须反压，也不提供开环 offered rate 下的丢失率。
- **Worker/Logger CPU 列都是线程总 CPU，包含忙轮询与 Worker 最终 drain 等待，不能用它们推断填充/编码服务 ns/op。** 另列 spin polls 与 stall episodes（不是等待时长）。为避免每事件时钟读取掩盖短操作，本实验不测 active service CPU；无等待的前台服务成本由独立 construction microbenchmark 测量。
- 分配、预触页、PB 容量预热、线程启动及 ready barrier 在计时外。主 wall time 从开始发布到最后一槽消费/归还；sink、队列协作与完整填充/编码均在内。消息延迟百分位未测量，吞吐倒数不能当单条端到端延迟。
- 所有模式共用固定上限 wire/native 存储槽位，每 slot1344B；D 额外对象池另计。当前布局重在相同队列协议的可比较性，不是最小内存占用实现。冷缓存、NUMA、真实网卡收发、业务解析缓存污染和生产环境排队/丢失均不在容量数字中。

## 功能验证

`--smoke` 对80配置分别传10003条（非整批尾部），在独立功能路径使用官方 PB 完整解析并逐项核对9个 scalar 与全部 payload 字节，共800240条验证。另测复用对象在非空→全0/empty→最大整数/1024B→empty→非空间切换，并确认 HTTP oneof 子对象地址保持不变。性能模板 `Verify=false` 不执行 ParseFromArray。
