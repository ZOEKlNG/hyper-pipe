# IPC 实测与有界队列/批量模拟

## 复现

```bash
g++ -std=c++20 -O3 -march=native -pthread -Wall -Wextra ipc_bench.cpp -o ipc_bench
./ipc_bench results/ipc_results.csv 0 5 0.35
python3 summarize.py results/ipc_results.csv
python3 simulate.py --output results
```

需要 Linux x86_64、可用 CPU 0/1，模拟和摘要依赖 numpy。`0 5 0.35` 表示每个配置先用 20 万条校准，再选择预计约 0.35 秒的消息量，实测 5 次；消息量上限 1 亿。各行记录实际运行时间、条数，摘要的 p10/p90 是 **5 次运行的吞吐分布**，不是消息延迟。测试期间不应同时跑其他重任务。

## IPC 测量边界

- 模型：一个 producer 独占 CPU0，一个 consumer 独占 CPU1。分别使用 `std::thread` 与 `fork + mmap(MAP_SHARED|MAP_ANONYMOUS)`。本程序只对 Linux 的跨进程 lock-free 64-bit atomics 作出部署假设，不宣称 C++ 标准为任意操作系统提供跨进程 `std::atomic` 保证。
- 固定 4096 个 slot，pool stride 按 64 字节对齐。每条 descriptor 为 offset + seq + length + reserved（24B）。描述符保存相对映射基址的偏移；没有跨机器/跨地址空间传裸指针。共享内存映射位置与消息 bytes 均不经过网络。
- 每条消息总长 64/256/1024B，内容是连续 `uint64_t(seq+i)`。producer 每条重新构造全部字节；consumer 读取全部字节、验证描述符与内容 checksum。`inplace_pool` 直接写最终 pool；`scratch_then_copy` 先写同等消息至私有 scratch，再 memcpy 到 pool。两者都包含构造、descriptor 交接和全部字段消费，**不是** 预构造 pointer-only 微基准。
- SPSC，release/acquire 发布 head/tail；producer 在发布之前独占 slot，consumer 归还 tail 后 slot 才可复用。head/tail/start 等位于独立的 128B cache line。batch=1/32 指索引发布和消费批次；并不等待墙钟 batching timer，也不是网络包批量。
- **本实测是饱和服务能力实验：队列满时 producer 自旋等待，dropped=0。** 不代表生产组件必须反压、不代表给定 offered rate 的丢失率。生产环境的 drop-new、热点/暂停取舍由单独模拟探索。
- 分配、page prefault、进程/线程启动和准备 barrier 在计时外；计时从 producer 开始发布到 consumer 全部消费完成。输入工作集循环复用，适合热池，不覆盖冷缓存、NUMA、进程崩溃恢复、MPMC 原子争用、DPDK rte_ring 或网卡 DMA。
- `ns_per_delivered` = 完整运行 wall time / delivered count，只是吞吐倒数；`total_cpu_ns_per_msg` = 两端 CPU 时间总和 / delivered count（包含忙轮询）。二者都不能称为消息端到端延迟。
- `smoke_only_not_benchmark.csv` 是 10003 条、单次功能检查，验证非整 batch 尾部，不纳入性能结论；正式矩阵另存 `ipc_results.csv`。

## 模拟边界

`simulate.py` 使用固定随机种子 20261021。所有服务速率、暂停时长和输入分布都是人为假设，**不是对硬件/Flink 的预测**。完整参数保存 `simulation_parameters.json`。

队列模型：4 个 producer 的错相等间隔输入合计 60 万条/秒，4 个 consumer 各自确定性服务 20 万条/秒，平均总利用率 75%；运行输入 1 秒，之后完整 drain 所有已接收消息。采用 4096 虚拟桶 `id & 4095`，桶 round-robin 分配至 4 个 consumer；各 consumer 独立 drop-new queue，容量包含正在服务的记录。容量 256、4096、65536。分别模拟 uniform、30% 流量强制为同一个 id、[0.4,0.42)s 的全局 2x 突发、consumer0 在 0.4s 暂停 10ms/50ms（暂停内不服务）。对于每个消费者和总体都断言 offered=accepted+dropped；drain 后 completed=accepted。

队列百分位只统计成功入队记录；丢弃记录没有伪造延迟。**低 p99 与高丢失可以同时发生**。总体 p99 也可能掩盖仅一个消费者的停顿，因此同时提供 consumer 级行；`last_finish_s` 可看输入结束后的 backlog。`wait_p99_us` 是服务开始前等待，`latency_p99_us` 包含 5us 服务（服务跨暂停时还包含暂停）。

批量模型：Poisson 到达率为 1000、10 万、100 万条/秒，batch size 为 1/8/32，分别仅按数量 flush 或最多等待 10us deadline，10 万条样本。只测 **batch 形成等待**，不包含队列、编码、syscall、传输与处理，timer 假设精确且无调度抖动。记录 mean batch size 与 formation p50/p99/max。小 batch 不等于 batching 失败，低负载 timer 就应及时发出不满批次。

路由扩容模型：连续 100 万个 id，直接 `id % N` 从 N=4 扩至 5，与固定 4096 桶只迁移 819 桶比较，输出移动比例。桶迁移比例不是实际迁移数据量或停顿时间，热点桶有不同权重，也不提供迁移顺序协议。

## 本次测量审计说明

正式 raw 为 `results/ipc_results.csv`，24 个配置各 5 次（120 行），内容校验错误总数为 0，最终最短 run 0.314813172 秒。第一次校准后，有 3 个配置出现少于 0.30 秒的 run，因此整组重测；重测选择规则只依据时长，整组新的 5 行全部采用，未依据速度择优。首轮数据完整保留于 `ipc_initial_results.csv`，3 组补测 raw 以 `ipc_validation_*.csv` 保存。适应性校准现在会对低于目标时长 85% 的 run 增加消息量后重试。`ipc_measurement_notes.json` 记录规则与最终时长。

观察到部分配置即使时间足够也有很大吞吐波动，例如 fork 的 scratch+copy 256B batch32 在 8.77–34.14 Mmsg/s 之间。这里不把其原因确定归于某个机制，且不据此宣称 fork 比线程更快或 memcpy 总会降低吞吐。需要进一步固定生产者/消费者队列占用、硬件拓扑与裸机环境才能解释这类差异。
