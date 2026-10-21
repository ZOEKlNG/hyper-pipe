# IPC 与模拟结果摘录

所有速率为本环境测量，不能推广为 DPDK、网络或 Flink 性能。IPC 为两核饱和 SPSC 服务能力，构造与消费完整消息、校验全部字段，满队列 spin，无丢弃；单条 end-to-end latency 未测量。以下吞吐为 5 次运行中位数，单位百万条/秒（Mmsg/s）。

| 模型 | 256B 构造方式 | batch1 | batch32 |
|---|---|---:|---:|
| threads | 直接构造 pool | 4.048 | 6.880 |
| threads | scratch 构造后 memcpy | 3.288 | 3.852 |
| fork+mmap | 直接构造 pool | 4.907 | 6.889 |
| fork+mmap | scratch 构造后 memcpy | 3.681 | 16.649* |

*最后一组 5 次范围 8.77–34.14 Mmsg/s，有很大波动，不能作为稳定优势结论。完整矩阵/范围见 `results/ipc_summary.csv`。256B 的 threads+pool batch32 为 6.647–7.008 Mmsg/s，fork+pool batch32 为 6.866–7.126 Mmsg/s。这两个配置接近，支持“同机跨进程共享池不必复制消息”可行，但不是普适性能保证。线程直接构造1024B batch32 中位数2.149 Mmsg/s，scratch+copy 为1.743 Mmsg/s。

模拟参数：4 producer 合计60万条/s，4 consumer各自20万条/s，1秒输入之后完全排空，drop-new，queue容量包含在服务的记录。速率是人为假设，模拟数字是容量与语义演示，不是实测硬件上限。

| 场景 | queue槽数/consumer | 总体丢失率 | 已接收记录的等待p99 |
|---|---:|---:|---:|
| uniform | 256 | 0% | 26.67us |
| 30%单热点id | 256 | 14.04% | 1.275ms |
| 30%单热点id | 65536 | 3.16% | 327.67ms |
| 2x突发20ms | 256 | 1.14% | 1.273ms |
| 2x突发20ms | 4096 | 0% | 8.740ms |
| consumer0暂停50ms | 256 | 1.22% | 28.33us |
| consumer0暂停50ms | 4096 | 0.58% | 17.10ms |
| consumer0暂停50ms | 65536 | 0% | 40.55ms |

- 更大缓冲可以吸收短暂停顿，但会延长排队；持续热点过载不靠扩队列解决。
- 只统计成功入队记录会掩盖被丢掉的慢记录：暂停50ms、256槽的低p99与丢失同时发生。总体p99又会掩盖只影响一个消费者的停顿，所以CSV保留consumer级行。
- 仅按数量凑32条，Poisson1000条/s时形成批次的等待p99=38.34ms；加10us deadline后p99<=10us，但平均batch只有1.01。10万条/s时仅计数p99=380.91us，10us deadline平均batch约2；100万条/s时仅计数p99=38.58us，10us deadline平均batch10.94。这只包含批次形成，没有网络与调度抖动。
- 100万个连续id，`id%N` 从4扩5，80% id目标变化；固定4096桶仅移819桶，模拟约20.04% id目标变化。移动比例不提供实际迁移协议或状态迁移成本。

源码 `ipc_bench.cpp` / `simulate.py` / `summarize.py`，所有结果CSV及参数说明位于 `results/`，复现命令与边界见README。
