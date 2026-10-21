# 独立代码与指标复核

复核日期：2026-10-21。范围为当前可用的 `/workspace/work/ipc_sim`、`transport`、`java_flink`、`component` 源码、README及已输出数据。只静态阅读与轻量CSV检查；未运行重负载、TSan或跨物理网卡实验，不能据此宣称已证明不存在所有并发问题。

## 总体结论

当前没有发现足以推翻这些实验既定用途的重大数据竞争或计数错误。代码与README整体匹配：IPC用于饱和容量，transport是loopback传输与独立闭环RTT，Java是热语料解码，Flink是预加载语料源产生投影tuple的本地MiniCluster，component是有界best-effort UDP PoC。四种实验不能直接拼接为一个已经测量的DPDK→远端Flink端到端结果。

轻量验证记录在 `review_counter_checks.json`：正式IPC120行、3个单配置复核文件各5行、队列模拟75行、transport当前已完整写出的58行通过所检查的计数与公式。transport此时仍在运行，后续行不在这一快照范围内。其他CSV也检查了可用字段；没有相应字段的表只记录行数，不代表进行了额外语义验证。

## 已确认的关键边界

1. **IPC不测消息时延。** `ipc_bench.cpp` 的满队列分支pause/spin，实际没有drop。CSV `ns_per_delivered` 是wall/count，即吞吐倒数；`total_cpu_ns_per_msg`是两端忙轮询CPU时间合计/count。摘要p10/p90是5次完整运行的吞吐分布。不得把这些指标写作逐消息p99、无阻塞丢弃性能或DPDK `rte_ring`实测。
2. **IPC发布/归还顺序合理。** producer填slot+descriptor后release发布head；consumer acquire读取head、读完内容后release发布tail；producer acquire读取tail后才覆盖slot。SPSC ownership没有发现同slot并发读写。进程模型依赖Linux共享映射的lock-free64位atomics，README已明确这不是任意系统的C++标准保证。
3. **IPC内容负载与codec不同。** IPC producer构造并consumer扫描整条64/256/1024B内容；codec/Java `decode_all`读取全部业务scalar、payload长度与首尾字节，未逐字节scan。codec的“payload64B”总wire约128B，transport/IPC的“message64B”是总body64B。表格中必须保留这些标签。
4. **队列模拟是设定服务能力。** 它采用确定性5µs处理、60万/s总输入、4×20万/s消费者等假设；暂停与热点结果不能称硬件预测。容量含in-service项；accepted记录在输入结束后drain。检查每consumer和ALL满足offered=accepted+dropped、drained=accepted，ALL计数也等于consumer之和。accepted-only的低p99不代表低丢失。
5. **批量模拟只计算组批等待。** 无网络、无CPU服务，Poisson输入与理想deadline。DPDK burst/sendmmsg32是32个独立报文；compression batch32是一个内存块，不能据此建议把整块塞入MTU1500下的单UDP数据报。
6. **transport通过有效接收计吞吐。** TCP记录分帧及partial read/write处理合理；接收计数、顺序与checksum在可靠模式下校验。UDP missing=sent-received；sequence_discontinuities是跳变次数，不能当丢失条数。ZeroMQ processCPU包含额外I/O线程，避免仅比较两个app线程的CPU。
7. **RTT是独立闭环低在途请求实验。** RTT样本真实逐exchange计时，p50/p99的表头合理；它不是单向时延，不除2，也不是饱和吞吐下的尾延迟。缺失/timeout应与成功样本一起报告。
8. **Java fixed与protobuf不是同一种所有权API。** fixed复用预分配heap/direct ByteBuffer；protobuf真实parseFrom创建消息。direct allocation/文件读取不在计时内，不能把direct的读取数值视作NIC DMA或JNI性能。输入4096记录已逐条比较fixed与protobuf checksum。
9. **Flink计时与payload范围清楚。** SourceReader从预加载corpus解码，输出4个Long的投影Tuple；payload字节不穿过Flink shuffle。`data_window_ms`为首source emit到最后sink.close，包含处理/背压/drain/关闭边界，未包含部署前启动；`wall_ms`与`job_runtime_ms`另列。不能当生产单record时延或远端吞吐。
10. **Flink running reduce的checksum不是总和校验。** 每个输入产生一次running aggregation输出，sink记录数仍应等于n；把所有running aggregate再相加的checksum会随跨source到达顺序变化。不能拿fixed/PB或keyBy/nokeyBy的checksum不同当丢包。checksum用于保证消费，计数守恒用于验证n。
11. **component的ownership和统计设计合理。** 每个DirectByteBuffer在接收线程/pool/queue/consumer之间单一归属；consumer完成后归还。MAX_DATAGRAM+1接收buffer可识别超过1472字节的截断报文。sendmmsg返回后普通socket用户buffer可以复用，仍有内核复制。收到但软件队列满继续drain并drop-new，统计rx=invalid+queued+queue_drop、queued=processed。
12. **component尚未实现全路径丢失账本。** kernel_accepted只是本机接受；kernel_accepted-java_received含网络/socket/结束边界等，不能全部算软件queue_drop。epoch仅上线格式，未fence；seq无乱序窗口/去重/尾损账本；README已明确，不应宣称完整动态迁移或可观测所有丢失。

## 不影响当前结果但应保留的限制

- transport UDP RTT出现timeout后，下一次request未先清理迟到reply，旧reply可能导致memcmp断言终止；当前若所有timeout=0则不影响已记录分布。timeouts变量包含warmup交换，不能称纯测量区间timeout。已通知transport作者。
- transport whole-processCPU从主线程到达gate前开始计量，到两个线程join之后结束，可能包括少量准备以及UDP末尾idle timeout；delivery_seconds排除了idle timeout。由二者相除的核数是实验口径近似，不是严格同一active span的CPU占用。600ms运行下影响较小。
- Flink实验Source的snapshotState对mutable Split浅复制，future完成后不为后续空期重置，addSplitsBack仅放pending；当前关闭checkpoint且固定bounded splits/noMore流程不触发其恢复边界。不要将该演示Source直接包装成已通过故障恢复验证的生产connector。已通知作者。
- component --rate是基于绝对时间表的尽力pacing，暂停后可追赶形成burst，sleep50µs也不意味着20k/s准确服务能力。README已说明。
- component接收关闭时会丢弃尚在网络/socket中的尾部数据；load测试必须留drain时间，且使用“Java未观测”而非未经验证的网络丢包归因。
- 当前矩阵没有真实DPDK/AF_XDP/RDMA/AF_PACKET、多生产者争用、NUMA、物理网卡、双机单向时钟或线上Flink业务负载。所有推荐需维持这一外推边界。

## Codec adaptive-count上限补查

另一个reviewer发现codec共用measure函数和compression同形循环有潜在cap边界错误：在8M operations仍不够20ms时，旧循环先翻成16M再退出，而elapsed仍属于8M。已逐行核对codec1240、FlatDirect120、compression300、ZstdReuse120，共1780行，**没有任何iterations达到16,777,216（2^24）**，现有数据全部未触发这个边界，不需重测。源码已修复为计时当前count后，达到20ms或cap才break，并轻量验证count与实际计时count一致；未来更快机器也不会低报一半。SBE另有受影响cell，由其作者单独处理，不在本文数据判断内。
