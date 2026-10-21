# 高性能专用通信组件：实测、模拟与可组合架构

研究日期：2026-10-21。本文面向 C++/DPDK 上游，兼容普通 C++、AF_PACKET/AF_XDP 输入与 Java/Flink 下游。目标是让不同负载选择不同实现，不预设固定 QPS 分界。

## 先给结论

你的核心方向是成立的：**控制面只管理分片与目标，生产者本地查表，数据点对点；缓冲有界，拥塞时按策略丢弃；尽量在最终发送缓冲里构造数据。** 但不宜把“裸结构体 + 全链路 DPDK + 零拷贝”设为所有部署的唯一方案。

建议把组件分成相互独立的五层：版本化消息封装、可替换编码、虚拟分片路由、有界缓冲与所有权、传输及接入适配器。先提供 Protobuf + 普通 socket 的可维护基线，同时提供 SBE/直接访问格式 + 批量 UDP 的高性能配置；上游已有 DPDK 时再接入其 mbuf/port/queue。是否启用内核旁路，以真实双机端到端测量判断。

本次**实际跑了** C++ 编解码、真实官方生成 SBE/FlatBuffers/Protobuf、复制与压缩、跨线程/跨进程共享池、Linux loopback TCP/UDP/ZeroMQ、按计划速率发送的 UDP、Java 解码、真实 Flink 本地 MiniCluster，以及 C++→Java UDP 原型。另跑了热点、突发、暂停、队列和批量形成的模拟。**没有跑物理双机 NIC/DPDK/AF_XDP/RDMA 的端到端测试**；下文将实测、模拟和理论预算分开。

## 1. 实验边界与可信度

主测量环境：Linux 6.18.55，x86-64，KVM，AMD EPYC 9V74；可见逻辑 CPU0–4，cgroup 配额为 4 CPU，内存 16 GiB，单 NUMA 节点。编译器 GCC14.2.0，C++20/O3。Java21，Flink2.1.0，Protobuf3.21.12，FlatBuffers23.5.26，SBE1.35.0，ZeroMQ4.3.5，LZ41.10.0，Zstd1.5.7。完整环境与版本见 environment.json 和各子实验 README。

实验按阶段串行执行并绑核，减少不同实验相互争抢 CPU；它仍然是共享虚拟机，不保证宿主机隔离、CPU 恒频或冷缓存行为。编解码一般每组 5 次，报告 **5 次批平均值的中位数和范围**，不把它标成单条消息 p99。传输 RTT 才是每次请求的样本分位数，但属于轻载 closed-loop，不是高负载尾延迟。

独立复核检查了代码、计数守恒和吞吐计算。SBE首轮暴露出极快操作触及校准上限时分母错误，已修正并完整重跑；其他1780行codec/复制/压缩数据没有触发该边界。一次可能与编译重叠的Java/Flink轮次也已隔离，采用干净重跑及3000万条长窗口验证。无效/受干扰数据不纳入结果包。

4096 条变化事件构成编解码 corpus：5 个 u64、4 个 u32，加 0/64/256/1024B 可变尾部。所有语言共用同一批 wire corpus。所谓“读取全部字段”是读取全部固定字段和尾部长度/首尾字节，不遍历或处理整个尾部。消息格式的协议元数据略有差异，wire bytes 单独列出。

**不同实验的尺寸口径不能混用：** codec 的 256B 指尾部，固定布局总长320B；transport 的 256B 指完整应用 body；原型使用72B头，256B尾部总长328B。不同表也使用不同 CPU 数量，不能据此声称某种网络库拥有某个“单核上限”。

## 2. 编码：Protobuf 到底是不是值得去掉？

{{codec_table}}

上表为256B随机尾部，单位ns/消息，单核热缓存微基准。“编码+解码”是在同一线程立即读取刚编码的缓冲，可能受益于缓存，因此不必等于独立两列之和。

本次结果不支持“Protobuf 一定是数微秒、某个固定 QPS 以上必须换掉”的说法。这组简单 schema 的重用对象 Protobuf 已经很快。fixed32/fixed64 在这里更省 CPU，但 wire bytes 也变化；真实业务的小整数分布、嵌套 message、repeated/string、Arena、内存分配、缓存和异常输入，都会改变结论。这里的“重用”是 C++ message/buffer 重用，**没有声称实测了 Arena**。

SBE 与自定义固定布局允许按偏移直接读取；FlatBuffers table 允许通过 vtable 访问少数字段。它们省掉的主要是接收端对象化和通用 parser 工作，数据写入、长度验证、访存以及跨机器搬运不会消失。Protobuf 是序列化格式，varint 是整数编码，不能把 ParseFromArray 直接称为解压。[Protobuf 编码说明](https://protobuf.dev/programming-guides/encoding/)、[SBE 版本演进](https://github.com/aeron-io/simple-binary-encoding/wiki/Message-Versioning)、[FlatBuffers 演进规则](https://flatbuffers.dev/evolution/)

本次还专门控制了 API 差异：FlatBuffers 主矩阵包含 builder 到 owning string 的一次复制，补充矩阵保留 builder 自身的缓冲；验证器开启和关闭分别报告。不能把 trusted 输入的直接访问与完整验证的 parser 不加说明地比较；各格式验证器的校验范围也不完全相同。JSON 使用 nlohmann DOM，二进制尾部编码为 hex，体积翻倍；它不能代表 simdjson、RapidJSON 或所有 JSON 工作负载。

{{flat_direct_note}}

### 用 CPU 预算而不是固定 QPS 分界

以实测编码时间 t_tx、解码时间 t_rx 和目标速率 λ 估算：发送侧CPU核数≈λ×t_tx/10^9，接收侧≈λ×t_rx/10^9；再除以希望的利用率上限，加入队列、网络和业务工作。两端分开配置，不能把两端相加后称为一个进程的吞吐。

{{cpu_budget_note}}

选择建议：

- **Protobuf**：复杂 schema、多人维护、版本演进优先，且实测 CPU/延迟有足够余量。先用对象/缓冲重用、合理字段类型和批量边界。
- **SBE**：字段相对稳定、C++/Java 都要高效 flyweight、可接受受约束的 schema 演进。通常比手写一整套结构体版本协议更值得维护。
- **FlatBuffers table**：可选字段较多、需要只读少量字段、希望生成跨语言访问器。构造成本、vtable 访问和 verifier 都要测。
- **自定义固定头+offset/TLV**：业务高度受控，性能收益足够大，团队愿意承担规范、兼容和测试成本。不要直接把编译器的 C++ 对象布局当网络协议。

多个变长字段完全可行：一组 offset/length 表、尾部 TLV、或 SBE 多个按顺序编码的 var-data。跨机器发送的是内容和相对位置，绝对指针没有意义。字段读取前验证 `offset <= size`、`length <= size-offset`，用减法避免长度相加溢出；字节序、空值、保留字段和版本规则都应写入协议。

## 3. 复制与压缩：优化哪一个更划算？

{{compression_table}}

这是独立的合成压缩实验：64B随机头+256B尾部，和业务 codec corpus 不完全相同。单条与32条连续内存批次分开测量；随机内容与重复日志文字分开测量。Zstd 同时比较 stateless API 与复用 CCtx/DCtx，不能用每次创建 context 的数字评价最佳实践。

带宽充裕、消息小且不可压缩时，默认不压缩通常合理：复制几百字节可能比压缩便宜很多。内容重复、带宽/跨地域费用成为瓶颈时，批量压缩才可能值得。理论近似收益条件是 `压缩CPU时间+解压CPU时间 < 节省字节×8/瓶颈带宽`，但多核流水线中还需分别核算 CPU、吞吐和尾延迟；不能只用这条串行公式决定生产配置。

一次“零拷贝”可能引入引用计数、跨核 cache line 争用、远端 NUMA、长期占用 mbuf 和更复杂回收。少一次复制不保证整体更快。尤其 native→Flink 边界，先保留一处可计算、生命周期清楚的复制，往往是更好的默认实现。

## 4. 线程与进程：同一套共享池描述符

{{ipc_table}}

这里消息总长256B，producer CPU0、consumer CPU1，4096-slot池；包括每条完整构造、descriptor交接、消费全部字节和checksum。线程共享地址空间，无须额外OS共享内存；进程使用fork+mmap共享池，descriptor为offset+seq+len。不是在重复传递预构造指针。fork-copy的256B/batch32出现8.77–34.14Mmsg/s宽范围且高于原地构造，原因未被本次实验隔离；保留该异常，不把它解释为“多一次复制一定更快”，也不隐藏它来支持零拷贝叙事。

**这是饱和服务能力实验，队列满时自旋，drop=0。** 它不测业务要求的过载丢弃率；后文模拟及跨语言原型单独测试drop-new。ns/record只是吞吐倒数，不是端到端消息延迟。部分配置范围较宽，不能据此得出“进程总比线程快/慢”。

生产实现建议一个producer独占一个SPSC发布端，consumer处理完再release归还slot。多个producer共用一条ring时必须用MPSC/MPMC协议；不能因为接收端只有一个就把全队列配置为SPSC。跨进程推荐 `{pool_id, offset, length, generation}`；DPDK primary/secondary 在其共享hugepage及相同映射契约下可传mbuf指针，但不是通用跨地址空间接口。[DPDK ring](https://doc.dpdk.org/guides/prog_guide/ring_lib.html)、[DPDK 多进程](https://doc.dpdk.org/guides/prog_guide/multi_proc_support.html)

## 5. 传输：TCP、UDP、ZeroMQ 和批量

{{transport_table}}

以上是**同机loopback**，完整应用body256B。发送和接收分别绑定两个核，ZeroMQ还使用额外I/O线程；CPU列包含整个进程，不能称单核吞吐。TCP含每条4B长度头并正确处理拆包/粘包；TCP batch32是一批write32条frame；UDP mmsg32是一次syscall提交多个独立datagram；不是把32条拼成超过MTU的大UDP报文。

{{rtt_table}}

RTT是每次只有一个未完成请求的应用往返，不能除以2当成真实跨机单向时延。队列配置、loopback优化、GSO/GRO、共享缓存和无物理网卡，使这些数字不能直接外推到双机、DPDK或Flink。

{{paced_table}}

paced UDP 使用同机单调时钟。每条有计划发送时刻，统计接收时刻减计划时刻，以及发送调度落后；如果发送端到窗口结束仍来不及尝试，单列source_schedule_missed。延迟分位只覆盖收到的消息，必须与received/offered、发送本地失败和网络/接收丢失一起看，不能靠丢掉慢消息证明低延迟。

**ZeroMQ 可做无broker的P2P，但没有原生DPDK transport。** `zmq_msg_init_data`只避免该API边界的复制，后续TCP和接收端不自动零拷贝；free callback可能来自任意线程。`udp://`只适用于RADIO/DISH，不能把PUSH/PULL地址简单改成UDP。[libzmq transport](https://libzmq.readthedocs.io/en/latest/zmq.html)、[zmq_msg_init_data](https://libzmq.readthedocs.io/en/latest/zmq_msg_init_data.html)、[UDP transport](https://libzmq.readthedocs.io/en/latest/zmq_udp.html)

若用一个PUSH连接多个PULL，负载分发不等于按业务id路由。生产者仍应本地选择目标，例如每个endpoint对应一个socket/连接。TCP/ZeroMQ适合需要可靠流或维护简便的配置；遇到慢消费者时必须用bounded应用队列和try-send隔离DPDK采集主循环。可靠传输不等于业务线程必须阻塞，但已经入TCP流的旧字节难以跳过，队头阻塞仍然存在。

UDP适合明确容忍丢失、希望控制积压的部署，仍需 pacing、MTU管理、报文验证、可观测丢弃和热点保护。允许丢失不等于可以不管拥塞，也不等于send成功就是远端已收到。

Aeron值得作为现成候选：开源版支持C/C++/Java、UDP和IPC，offer/tryClaim及Media Driver可以减少自研生命周期工作；reliable=false不代表关闭全部flow control。当前还有付费Premium DPDK driver，其限制属于该产品而非DPDK本身。本次未实测Aeron，不能在表里给它编造排名。[Aeron publication](https://aeron.io/docs/aeron/publications-subscriptions/)、[Aeron DPDK](https://aeron.io/premium-docs/aeron-dpdk/dpdk-overview.html)

## 6. 路由、迁移与观测：控制中心不转发数据

```text
控制中心：{virtual_partition → endpoint, route_epoch}、心跳、负载、schema协商
                                ↓ 版本化快照
生产者：mix64(id) & (V-1) → 本地route_table → 直接目标endpoint
```

V应固定且参数化，例如4096或16384；不是每次随consumer数量变化。只有验证业务id低位分布后，才把mix64简化成位操作或区间路由。控制面更新用不可变快照+原子切换，热路径不请求中心，不给每条消息加锁。

{{route_note}}

“同id到同consumer”需要限定为同一路由epoch；它不保证多producer全局有序，也不能消除网络乱序。一个热id超过一个consumer的处理能力时，迁移其所在分片无法解决持续过载。要提高单consumer能力、拆业务语义、聚合、采样或丢弃。

迁移至少需要epoch和fence策略。允许丢失的简化实现可以在切换后拒绝旧epoch并统计切换窗口丢弃；若旧目标和新目标接受期重叠，仍可能同时处理同id。要严格避免重叠，需要协调租约/截止条件，接受短暂停顿/丢失；要连续且有序，还要producer barrier、drain与必要状态迁移。单纯广播新route table不够。

序号建议按 `(producer_id, boot_id, partition, epoch)` 分配，避免每id维护状态或多个producer争抢一个原子序列。发现gap时先留有限乱序窗口；最后一批全部丢失时没有后继序号，仍须控制面发送high-watermark/计数快照。`tx_burst`和UDP send只表示本地接受，不能当delivery ack。

指标至少包含 offered、pool allocation fail、sender queue drop、Tx not accepted、kernel accepted、receiver received/invalid/epoch drop/queue drop、delivered to Flink、reordered、duplicate、confirmed gap，以及queue occupancy、oldest age、batch size和flush cause。不同层drop和端到端gap不能无区别相加。

## 7. 缓冲与批量：容量是业务取舍

{{queue_table}}

**上表是模拟，不是硬件/Flink实测。** 参数：4个producer合计60万条/s，4个consumer各20万条/s，1秒输入后完整drain；其中一个consumer可暂停，或者30%输入固定到同一个id；各consumer独立drop-new队列。全部等待分位只统计已接受消息，容量包含正在服务的记录。

这说明两点：小队列可以用高丢失换取低已接收p99；大队列能覆盖短停顿，但会增加排队和数据陈旧时间。总算力还有余量时，热点id照样能压垮一个consumer。缓冲只能吸收有限突发，不能治持续过载。容量上限和最大消息年龄应该一起配置，而不是只配“最多多少条”。

硬件Rx ring是descriptor环，不是Flink停顿缓冲。理论除法：4096 descriptors / 10Mpps = 409.6µs；若每条单独成包，10M records/s、停顿10ms需要10万条，256B仅payload就25.6MB。还须预算mbuf头、对齐、软件队列、socket、native池和Flink自身缓冲；单层有限不代表总内存小。

{{batch_note}}

batch应同时设置 `max_records`、`max_bytes` 和 `max_wait`，低负载时允许不满批发出。DPDK burst/sendmmsg是批量提交多个包；应用聚合是一个包放多个记录，两者应分开测。IPv4/UDP常见MTU1500时应用datagram最多1472B，仍需减去组件头；封装、隧道、IPv6会改变预算。压缩32条连续内存不等于可以把整个块不分段地作为一个UDP包。

## 8. DPDK、AF_XDP、AF_PACKET 与 RDMA 的落点

一个DPDK进程可以同时采集port0、从port1发送日志。每个Rx queue优先由独立lcore轮询，每个发送核独占Tx queue，按PMD能力确认共享队列是否可行。SDK应复用上游已有EAL、mempool与port配置，不擅自再初始化EAL或绑定网卡。[DPDK PMD](https://doc.dpdk.org/guides/prog_guide/ethdev/ethdev.html)

推荐快路径：从最终Tx池reserve → 直接构造有效网络头/消息头/tail → 本核Tx或SPSC交给Tx核 → `rte_eth_tx_burst`。其返回前n个mbuf已被驱动接管，不得立即修改/释放；未接受部分仍归应用，按短重试或drop策略处理。接收buffer在所有读取者完成前不能复用。[DPDK mbuf](https://doc.dpdk.org/guides/prog_guide/mbuf_lib.html)、[ethdev API](https://doc.dpdk.org/api/rte__ethdev_8h.html)

同进程直接Tx可以省队列，但会让解析核承担编码、路由、组包与Tx工作；独立Tx核能隔离采集，却增加cache迁移和队列开销。哪个更优要按端口NUMA、核心预算和业务处理抖动测量。输入原包与新生成的日志不是同一份内容，“零拷贝”不代表不需要写日志字节。

RSS通常对IP/UDP/TCP等协议头做hash，再映射**目标主机内**的Rx queue，不替代跨主机endpoint路由。可以用稳定的UDP端口增加可验证的流分散，但端口与queue并非天然一一对应；每个NIC的RETA/flow能力和规则都要验证。同组同优先级重叠rte_flow结果未定义，不要推广“L4永远优先”或“先创建者优先”。[Linux RSS](https://docs.kernel.org/networking/scaling.html)、[DPDK rte_flow](https://doc.dpdk.org/guides/prog_guide/rte_flow.html)

| 后端 | 通用组件中的定位 | 必须说明的限制 |
|---|---|---|
| 普通UDP/TCP | portable基线、普通C++/Java接入 | 系统调用、socket缓冲、可靠性/拥塞语义 |
| AF_PACKET/PACKET_MMAP | 原始二层收发、兼容已有采集程序 | mmap ring减少开销，不等价于NIC直接DMA零拷贝 |
| AF_XDP | 保留Linux设备管理的优化后端 | 零拷贝依赖驱动；不支持时必须区分COPY与ZC；UMEM rings有SPSC约束 |
| DPDK | 上游已使用DPDK、专核/NIC管理可接受 | PMD能力、NUMA、队列、DMA内存、部署成本；本次未实测NIC |
| RDMA | 有匹配网卡/网络、注册内存与运维能力 | MR/rkey/completion/credit和回收协议；仍需跨机传输数据 |

[AF_XDP 官方说明](https://docs.kernel.org/networking/af_xdp.html)、[PACKET_MMAP](https://docs.kernel.org/networking/packet_mmap.html)、[RDMA memory registration](https://man7.org/linux/man-pages/man3/ibv_reg_mr.3.html)

DPDK sender可以发送标准Ethernet/IPv4/UDP，让接收侧先用Linux UDP/Java。无需为了“对称”一开始就两端DPDK。AF_PACKET如果只是上游采集方式，应实现成PacketSource适配器，而不与数据传输后端混为一谈。

## 9. Java/Flink：让数据活到最后一个读取者结束

{{java_table}}

以上Java实验读取与C++相同的corpus，direct buffer预分配，复制和文件加载在计时外；Protobuf Java使用实际generated parseFrom，会构造Java消息。它不是JMH，也不包含网络/JNI，所以只用于局部比较。

{{flink_table}}

Flink实测是真实本地MiniCluster和FLIP-27 source。source解码完整业务字段后，**只把4个Long的投影tuple送进Flink**，没有让完整payload通过shuffle。keyBy实验有running reduce，和无keyBy路径的工作量也不同，因此只能展示本地路径成本，不能把差值全归因于网络shuffle。记录数通过accumulator校验。

wall含启动部署；data window从首条Source emit到最后sink关闭，含处理、背压、drain和关闭边界。单机p=2覆盖本地Flink序列化/分区，不能代替跨TaskManager双机测试。没有checkpoint、落盘state backend、线上窗口/外部sink，这些都应在部署前补测。

推荐接入：native/Java专门接收线程持续drain → 固定block pool+有界队列 → SourceReader非阻塞pollNext逐条emit；空队列用isAvailable通知，不能在Flink主执行线程阻塞收包或持续返回可用造成忙等。[SourceReader](https://nightlies.apache.org/flink/flink-docs-release-2.1/api/java/org/apache/flink/api/connector/source/SourceReader.html)

Flink背压到边缘时，按配置丢弃并记录；不能让上游DPDK采集循环跟着阻塞。原生块交给Java后可用DirectByteBuffer或MemorySegment view，但必须pin/release直到最后读取者结束；进入异步、state或shuffle前，要复制到Flink拥有的内存或实现经过验证的序列化/生命周期方案。默认允许这一次复制，比“拿着mbuf视图emit后立即free”可靠。

不建议把BinaryRowData等内部布局直接设为通用跨版本网络协议；它标注Internal，Flink MemorySegment也不是FFM MemorySegment。FFM/JNI优劣需批量实测，FFM不会自动防止错误native调用让JVM崩溃；DPDK很多热函数是static inline，jextract绑定通常仍需导出的C batch shim。

上游按id直达SourceReader不自动消除后续keyBy；Flink key groups、state和rescale规则需要独立处理。UDP无回放源可以配合Flink内部状态checkpoint，但不能由此宣称对所有源日志端到端exactly-once。[Checkpoint要求](https://nightlies.apache.org/flink/flink-docs-release-2.1/docs/dev/datastream/fault-tolerance/checkpointing/)

## 10. 已实现的组件原型

原型提供C++生产者、Java接收器、72B明确线格式、静态CSV虚拟桶路由、sendmmsg批量+deadline、固定接收buffer池、有界drop-new队列及计数守恒。可在两台Linux机器分别启动，具体命令在component/README.md。Java接收队列使用有锁ArrayBlockingQueue，以便验证正确性；不把它宣传成极限性能实现。

{{component_table}}

这些是同机跨语言实际运行，尾部256B、总wire328B，收到数据排空后核对统计。消费者故意延迟用来验证有界丢弃，并非承诺调度精确到给定微秒。内核/网络丢失不包含在queue_drop中。

**原型尚未实现** DPDK/AF_XDP后端、完整网络Flink connector、控制中心共识、动态迁移fence、乱序窗口/尾部对账、持久化和重传。epoch写入wire不等于已经完成迁移协议。它是可运行起点，不能当成生产完成度承诺。

建议生产API围绕所有权，而不是限定某种消息对象：

```cpp
BufferLease reserve(size_t bytes);                 // 取得独占可写buffer
MutableBytes writable(BufferLease&);               // 在最终布局里写字段
PublishResult try_publish(RouteKey, BufferLease&&);// 本地接受/明确丢弃原因
BorrowedMessage poll();                            // 有界只读视图
void release(BorrowedMessage&&);                   // 最后读取者归还
```

`ACCEPTED_LOCAL`只意味着本地组件接管；传输后端DMA/异步发送完成前不可复用。不同后端支持的最大消息、批量、zero-copy、可靠性和queue配置作为capability暴露，不静默把AF_XDP COPY冒充ZC，也不为了伪统一隐藏一次复制。

## 11. 按负载选择方案，而不是记一个阈值

| 配置 | 建议组合 | 使用条件 | 换挡依据 |
|---|---|---|---|
| 易维护 | Protobuf+TCP或ZeroMQ/P2P，有界try-send | schema复杂、可靠流有价值、资源有余量 | 实际profile出现parser/alloc热点，或尾延迟/CPU超预算 |
| 可丢失高吞吐 | Protobuf或SBE+批量UDP | 不重传、受控积压、需要普通Java接入 | 各目标负载、socket丢失、syscall/softirq占比 |
| 字段直读 | SBE或FlatBuffers+UDP/Aeron | parser或对象化明显占CPU、schema可管理 | 验证后端到端收益是否抵过维护成本 |
| DPDK集成 | 原地构造SBE/固定布局+mbuf+多port Tx | 上游已有DPDK、专核/PMD可接受 | 真双机NIC实测确认网络路径为瓶颈 |
| 异构部署 | 同一envelope与路由，按节点协商socket/AF_XDP/DPDK | 节点能力不同、逐步升级 | 固定路由流的能力协商和可观测回退 |

不要每条消息临时选择编码或传输；按stream/schema/endpoint协商，控制面分发配置。相同链路里的producer/consumer必须知道实际codec和版本。可在启动或低频控制更新时换档，避免对热路径引入动态分支和隐藏状态。

容量要同时看 `records/s`、`bytes/s`、`packets/s`、两端CPU、JVM分配/GC、Flink业务处理和可接受丢失/陈旧度。CPU与链路任一成为瓶颈，都不是换一个序列化库就能解决。

端到端延迟预算可拆为构造、软件队列、凑批、Tx、链路、Rx、解码、Flink队列和业务处理。这里不同实验只给出各自workload的局部证据，不能把不同表的吞吐倒数相加冒充整链路延迟，也不能直接相加各段p99得到端到端p99。下一轮应在完整目标路径上统一采样。

{{wire_budget}}

## 12. 下一轮真实双机/DPDK验收清单

本次交付包含可复现源码、raw CSV、运行日志、模拟参数和参考核查。下一轮只需把相同workload搬到目标硬件，不应换schema后继续沿用这些数字。

1. 两台同交换机机器，记录CPU/NUMA/NIC/固件/PMD/MTU/链路速率、时钟同步误差、offloads、IRQ和队列。先跑socket基线，再逐项换编码、批量和I/O后端，避免同时改变三项后无法归因。
2. 真实NIC的DPDK Rx/Tx、AF_XDP COPY/ZC分别确认，统计IMISSED/RXNOBUF、software ring、PMD未接受、接收队列和最终交付；rte_ring软件数值不得当DMA吞吐。
3. 各消息大小/producer数/consumer数，均匀、热点、突发；按饱和能力10/25/50/75/90/100/120% offered load逐档运行，重复、报告范围。
4. 延迟采用open-loop或带计划时刻的测量，记录所有计划输入、发送调度落后、各层drop和成功接收分位。单向时延须有PTP等同步及误差预算；否则报告RTT。
5. 消费者10/50/100ms暂停、Flink GC/背压/checkpoint、控制面断连、producer重启、路由迁移，检查bounded memory、fence、重号/乱序/尾部缺失对账和池回收。
6. Flink网络Source+真实keyBy/window/state/backend/sink运行，完整payload、真实对象生命周期；吞吐、p99/p99.9、CPU、allocation、GC、掉数据比例、恢复行为共同验收。

不能用这份单机报告承诺跨机器5µs、100Gb线速或某固定QPS下“一定过剩”。可以据此确定实验变量、先实施的路径和避免的语义错误。

## 附件与复现

raw目录按codec、sbe、ipc_sim、transport、java_flink、component分开保存；各README给出编译、依赖版本和运行命令。研究附件reference_audit_notes.md对两份粘贴参考逐项核查，official_sources.md提供官方原始资料。

本报告中的实测来自随包CSV，模拟来自随包源码和固定随机种子，理论数字来自明确公式。未测的DPDK NIC、AF_XDP、RDMA、Aeron不填写虚构性能。
