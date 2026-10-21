# Java / Flink 调研与测量说明

资料核验日期：2026-10-21。实测依赖固定为 Flink 2.1.0、Protobuf Java 3.21.12、Java 21，编译输出 Java17兼容字节码（ECJ 3.40.0）。官方2.1分支文档当前页面标识2.1.1，API页面为2.1-SNAPSHOT；不要将文档版本误写成所测binary版本。

## 官方证据与设计判断

1. SourceReader.pollNext 必须非阻塞；空时 isAvailable 不应永远completed；复杂source推荐SourceReaderBase的handover。
   https://nightlies.apache.org/flink/flink-docs-release-2.1/api/java/org/apache/flink/api/connector/source/SourceReader.html
2. checkpoint无法为无持久化、无回放的UDP源补回丢失数据。上游best effort可以与Flink内部状态checkpoint并存，但不能承诺所有源日志端到端exactly once。
   https://nightlies.apache.org/flink/flink-docs-release-2.1/docs/dev/datastream/fault-tolerance/checkpointing/
3. 上游按id路由到SourceReader不自动消除Flink keyBy的重新分区；业务虚拟分片不自动等于Flink key groups/rescale分配。
   https://nightlies.apache.org/flink/flink-docs-release-2.1/zh/docs/learn-flink/etl/
4. native receiver/Java I/O worker批量收包→有界block pool→SourceReader逐条emit，是建议架构；Flink背压到边缘后必须明确drop策略，不能把主机环溢出当作唯一受控丢弃点。JNI按批次跨越边界。缓冲block的生命周期覆盖全部读取者，不能emit flyweight后立刻归还mbuf。
5. Java DirectByteBuffer/MemorySegment允许读取native内存，但不会自动解决外部mbuf复用。FFM arena决定其关联内存生存期；JDK版本需与Flink部署矩阵匹配。
   https://docs.oracle.com/en/java/javase/22/core/memory-segments-and-arenas.html
   https://docs.oracle.com/en/java/javase/26/docs/api/java.base/java/lang/foreign/MemorySegment.html
6. libzmq官方transport列表没有DPDK。zmq_msg_init_data在该API边界不复制并接管buffer，callback可能由任意线程调用；这不证明TCP链路端到端无复制。UDP仅RADIO/DISH，不是PUSH/PULL改URL即可。
   https://libzmq.readthedocs.io/en/latest/zmq.html
   https://libzmq.readthedocs.io/en/latest/zmq_msg_init_data.html
   https://libzmq.readthedocs.io/en/latest/zmq_udp.html
7. Aeron开源UDP/IPC可纳入比较，offer/tryClaim非阻塞；offer入log，MediaDriver异步发送。reliable=false表示允许gap fill，并非取消所有flow control。当前另有付费Aeron Premium DPDK组件；其限制包含IPv4、unicast、每driver单DPDK口、Dedicated模式、绑核。这里的单口是该产品限制，非DPDK原生限制。
   https://aeron.io/docs/aeron/publications-subscriptions/
   https://github.com/aeron-io/aeron/blob/master/aeron-client/src/main/java/io/aeron/CommonContext.java
   https://aeron.io/premium-docs/aeron-dpdk/dpdk-overview.html
8. Protobuf是序列化，不能将parse称作必然解压；fixed32/fixed64仍有field tags和parse。SBE flyweight更接近固定布局直接访问，支持尾部多个var-data，演进需actingVersion/blockLength/sinceVersion等规则。FlatBuffers Java通过ByteBuffer访问，table支持受约束演进，struct不能任意加字段。
   https://protobuf.dev/programming-guides/encoding/
   https://flatbuffers.dev/languages/java/
   https://flatbuffers.dev/evolution/
   https://github.com/aeron-io/simple-binary-encoding/wiki/Java-Users-Guide
   https://github.com/aeron-io/simple-binary-encoding/wiki/Message-Versioning

## JavaCodecBench 范围

与C++使用同一4096条binary corpus，每条文件记录u32LE length+wire。fixed布局64字节头+payload；protobuf是真实protoc生成代码。消费全部业务scalar与payload长度、首字节、尾字节，与C++ all-field workload一致；不逐字节扫描payload。fixed同时比较复用heap ByteBuffer和预分配direct ByteBuffer。direct allocation与文件读取在计时区外；这不是实测NIC DMA或JNI收包。Protobuf parseFrom会构造实际Java消息及payload表示，不能声称采用了C++可变消息复用API。5轮短warmup后重复测量，volatile checksum防止死代码消除，结果非JMH。

## FlinkLocalBench 范围

真实local MiniCluster、FLIP27 bounded source、map、可选keyBy+running reduce、count/checksum discard sink。source重复读取预加载的真实wire corpus并解码，消费所有scalar及payload长/首/尾，然后只把4个Long的投影tuple送入Flink。**完整原始payload未穿过Flink网络路径；没有真实DPDK、UDP收包、远程主机、checkpoint、state backend磁盘I/O、event-time窗口或线上业务。**

默认对象复用关闭，keyBy用1024个key；p=2能覆盖本地Flink shuffle/serialization，p=1不能据此推出跨TaskManager开销。每组先完整跑一次不计结果的warmup job，再统计重复。

这里的Source只覆盖固定one-split-per-reader、bounded、无checkpoint/recovery的基准流程；不作为生产connector或故障恢复实现。动态重分配、rescale、checkpoint状态拷贝、idle availability future重置等生命周期还需生产实现和验证。

- wall_ms：env.execute前后主进程单调时钟，含调度和启动等。
- job_runtime_ms：Flink JobExecutionResult.getNetRuntime。
- data_window_ms：最早SourceReader第一次emit的单调时间到最晚sink.close的时间；排除部署前启动，但含处理、背压、drain及很短的sink关闭边界，不能称无尾部成本的单record latency。
- data_window_mps：records / data_window。与微基准ns/op完全不同口径，不能直接相加。
- 记录数由Flink聚合accumulator验证必须精确等于设定值；checksum用于确认消费，keyBy running aggregation的checksum会随分区次序变化，不是最终总和校验。

调度要求：其他代理基准独占运行期间，只构建准备/写源码，不跑性能；正式运行须等主代理RUN。

## 实际结果（有效轮次）

Java microbench：每条计时2,097,152次，5次repeat，4种payload×3模式=60行。64B payload中位数heap固定布局16.3921ns、direct固定布局16.3696ns、generated Protobuf114.3666ns。这里DirectByteBuffer直接读字段并未显著快于heap；不能把I/O边界减少复制的收益等同于每次getLong更快。所有payload下，三种模式的checksum相同。

Flink长窗使用30,000,000条/轮，3次repeat，每组另有1次同样大小的warmup job。所有12条正式记录的计数精确，data window均大于1秒。结果是本地共享JVM MiniCluster吞吐，不是单核指标、双机吞吐或单条尾延迟。

| 输入解码 | Flink路径 | 数据窗口吞吐中位 Mrec/s | 3次范围 | 数据窗中位秒 | env.execute wall中位秒 |
|---|---|---:|---|---:|---:|
| 固定布局trusted | map→sink | 23.1637 | 22.7739–24.3440 | 1.2951 | 1.9019 |
| 固定布局trusted | map→keyBy→running reduce→sink | 11.0697 | 10.4820–12.8662 | 2.7101 | 3.3080 |
| generated Protobuf | map→sink | 11.1474 | 10.9488–11.1629 | 2.6912 | 3.3318 |
| generated Protobuf | map→keyBy→running reduce→sink | 7.1534 | 7.0585–7.9451 | 4.1938 | 4.8210 |

并行度2，keyBy 1024keys。map→sink可operator chaining；keyBy在本地引入重新分区与聚合，二者处理路径、活跃task线程数也不同，不是只测一个hash函数。相同算子图中，Protobuf与固定布局的解码选择在本实验中确实影响吞吐；加入keyBy/reduce后收益比例变小，说明需要把优化放在实际图中评价。不能把该比例移植到包含完整payload、RocksDB、窗口、checkpoint或sink I/O的生产图。

主表采用results/flink_long.csv（原始results/long_window/flink_local.csv的相同拷贝）；短窗results/flink_short.csv只是较短的探索实验，统计单独保留summary_short.json。summary_long.json只含长窗。最初一次可能与另一代理2.5秒编译重叠的轮次完全隔离到results/contaminated_then_rerun，**不应交付或用于结论**。有效Java microbench、短窗与长窗均在基准交接后顺序独占运行。
