# 本次双核容量结果

2026-10-29 UTC 运行，400个保留样本（80配置各5次），最短0.302390202秒，功能错误0，测量源文件与二进制SHA前后一致。墙钟212.611秒，cgroup未发生CPU配额节流（nr_throttled/throttled_usec增量0）。CPU0为Worker、CPU1为Logger。

下表是tail256B、queue256、队列batch32时的5次运行中位数，单位百万条/秒。它是完整双核容量，不能当Worker服务成本或网络性能。

| 模式 | flat | nested HTTP |
|---|---:|---:|
| A_native_slot | 2.298 | 2.312 |
| A_scratch_then_slot | 2.743 | 2.738 |
| B_worker_pb_batch_copy | 2.962 | 1.728 |
| B_direct_worker_pb | 3.133 | 2.660 |
| D_shared_pb_object | 2.532 | 1.509 |

- 256B指输入tail；完整PB wire均值为flat323.6875B、nested332.6875B。nested lengths仍使用varint，fixed32/64不代表整条消息定长。
- 部分配置存在较大高吞吐离群，完整min/max与每次样本已保留。例如nested A范围2.269–8.420 Mmsg/s，nested A_scratch为2.637–7.972 Mmsg/s；nested B_direct范围2.642–2.747 Mmsg/s。不能只引用最快一次。
- A_scratch在本环境可以比A更快；D虽然少一次payload拷贝，却可能更慢。复制层数不能单独推导饱和容量。生产者节奏、共享索引轮询、跨核对象/元数据访问都可能影响结果，本次未采硬件计数器，不确定归因。
- 两端CPU列都包含忙轮询，约各占满一个核；spin polls/episodes是计数，不是时长。Worker主动填充/编码成本请看独立construction microbenchmark，不能用这里的total CPU/条代替。
- 所有模式ring slot1344B，queue256分配344064B。D额外对象空间估计flat139264B、nested155648B，且只适用于同进程线程间移交，不支持把PB对象指针发到其他进程。

原始数据 `results/handoff_results.csv`（400行），汇总 `results/handoff_summary.csv`（80行），运行命令与资源 `results/dual_run_timing.json`，源码快照 `results/dual_snapshot_before.json` / `dual_snapshot_after.json`，CPU证据 `results/dual_cpu_before.txt` / `dual_cpu_after.txt`。模式、copy账本、输入、所有权和复现说明见README。
