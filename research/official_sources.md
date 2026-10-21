# 官方来源与可支持的结论

访问日期：2026-10-21。仅列实际检索并打开的官方文档/项目源码/维护者手册；不把任何页面上的外部性能数字作为当前环境实测。DPDK当前在线指南标为26.11.0-rc1；Flink stable当前页面标为2.3.0但部分兼容说明引用2.0，部署必须固定release版本。OpenJDK JEP 454/471直连返回403，相关事实改用可读的Oracle官方API/迁移指南，不声称已读JEP全文。

| ID | 来源 | 本核查采用的事实 |
|---|---|---|
| S01 | [Protobuf wire encoding](https://protobuf.dev/programming-guides/encoding/) | wire types包含varint、fixed32/64、长度字段等；不应把序列化视为通用压缩 |
| S02 | [SBE Message Versioning](https://github.com/aeron-io/simple-binary-encoding/wiki/Message-Versioning) | block末尾追加、sinceVersion/optional、actingVersion/actingBlockLength；不保证所有模式改动兼容 |
| S03 | [ZeroMQ PUSH/PULL socket semantics](https://libzmq.readthedocs.io/en/latest/zmq_socket.html) | PUSH round-robin，PULL fair queue；非业务id一致路由；HWM和发送模式需按socket类型读契约 |
| S04 | [Linux RSS/network scaling](https://docs.kernel.org/networking/scaling.html) | RSS通常hash网络/传输头，经indirection table映射本机queue |
| S05 | [AF_XDP](https://docs.kernel.org/networking/af_xdp.html) | UMEM所有权、四种SPSC ring、XDP_COPY/ZEROCOPY、XSK queue约束；不支持ZC时默认可回退copy |
| S06 | [DPDK ethdev API](https://doc.dpdk.org/api/rte__ethdev_8h.html) | tx_burst返回写入Tx descriptors数量，未接受部分应用处理；不是远端delivery证明 |
| S07 | [DPDK multi-process](https://doc.dpdk.org/guides/prog_guide/multi_proc_support.html) | primary/secondary共享映射、相同版本及部署约束；共享指针的成立范围 |
| S08 | [DPDK Ring Library](https://doc.dpdk.org/guides/prog_guide/ring_lib.html) | 固定容量FIFO，SP/MP及SC/MC，bulk/burst；多生产者不可错误使用SP |
| S09 | [Flink KeyGroupRangeAssignment源码](https://github.com/apache/flink/blob/master/flink-runtime/src/main/java/org/apache/flink/runtime/state/KeyGroupRangeAssignment.java) | key.hashCode经MurmurHash后%maxParallelism得key-group，operatorIndex=keyGroupId*parallelism/maxParallelism；字节hash不自动等价 |
| S10 | [JDK22 java.lang.foreign API](https://docs.oracle.com/en/java/javase/22/docs/api/java.base/java/lang/foreign/package-summary.html) | MemorySegment/Arena范围与线程约束；restricted绑定错误仍可VM crash或silent corruption |
| S11 | [JNI Functions](https://docs.oracle.com/en/java/javase/22/docs/specs/jni/functions.html) | GetStringUTFChars的isCopy可报告copy/no-copy，规范不要求每次复制 |
| S12 | [JDK23 Removed/Deprecated Components](https://docs.oracle.com/en/java/javase/23/migrate/removed-tools-and-components.html) | Unsafe内存访问方法deprecated for removal，非JDK23整个Unsafe已移除 |
| S13 | [Flink BinaryRowData API](https://nightlies.apache.org/flink/flink-docs-stable/api/java/org/apache/flink/table/data/binary/BinaryRowData.html) / [项目源码](https://github.com/apache/flink/blob/master/flink-table/flink-table-common/src/main/java/org/apache/flink/table/data/binary/BinaryRowData.java) | @Internal；1字节header+null bits，8字节槽；Flink MemorySegment与FFM不是同一类型 |
| S14 | [Flink BinaryArrayData API](https://nightlies.apache.org/flink/flink-docs-stable/api/java/org/apache/flink/table/data/binary/BinaryArrayData.html) | size(int)+4-byte边界null bitmap描述对应array而不是row |
| S15 | [Flink Java Compatibility](https://nightlies.apache.org/flink/flink-docs-stable/docs/deployment/java_compatibility/) | 应按Flink版本核对JDK；当前页面推荐17并标21 experimental，不能因JDK22+存在就默认Flink支持 |
| S16 | [DPDK Generic Flow API](https://doc.dpdk.org/guides/prog_guide/ethdev/flow_offload.html) / [flow attrs](https://doc.dpdk.org/api/structrte__flow__attr.html) | 同group同priority多匹配undefined；小priority数值更高；规则支持需validate；持久化查询FLOW_RULE_KEEP |
| S17 | [DPDK PMD architecture](https://doc.dpdk.org/guides/prog_guide/ethdev/ethdev.html) | 多port、run-to-completion/pipeline、每Rx queue单lcore、Tx MT_LOCKFREE能力、NUMA与burst |
| S18 | [DPDK mbuf](https://doc.dpdk.org/guides/prog_guide/mbuf_lib.html) / [mbuf API](https://doc.dpdk.org/api/rte__mbuf_8h.html) | mempool、headroom、prepend/append、链式/外部buffer和生命周期 |
| S19 | [PACKET_MMAP](https://docs.kernel.org/networking/packet_mmap.html) | mmap circular ring减少syscall/复制，不等于保证NIC直DMA用户buffer |
| S20 | [ibv_reg_mr](https://man7.org/linux/man-pages/man3/ibv_reg_mr.3.html) / [ibv_post_send](https://man7.org/linux/man-pages/man3/ibv_post_send.3.html) | RDMA注册权限、远程地址/rkey、本地sge/lkey；仍需completion/credits/回收协议 |
| S21 | [Linux UDP manual](https://man7.org/linux/man-pages/man7/udp.7.html) | UDP不可靠，包可reordered/duplicated；PMTU和大包分片限制 |

来源使用边界：以上链接已在本轮打开，关键段已检索。未能核验主参考里的[cite:N]，因为原文没有与其匹配的完整参考书目；不能把这些占位标记当可追溯引用。GitHub master和stable文档是动态入口，项目落地还应改成所用版本的tag/commit permalink。
