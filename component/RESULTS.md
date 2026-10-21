# 组件原型实测结果

这是socket backend跨语言功能与过载验证，不是双机DPDK/Flink端到端测量，也不是优化后的Java性能上限。

C++20/GCC14.2发送器使用sendmmsg，Java21 receiver使用DatagramChannel、固定direct-buffer pool和带锁ArrayBlockingQueue。单VM loopback；发送器CPU1，Java CPU2/3；tail256B、wire328B；batch最多32包且deadline10µs。每轮新启JVM，无单独JIT warmup；发送约2秒、接收约4秒以排空；3轮重复。SO_RCVBUF实际4194304B。

| 负载 | 每轮offered | 每轮Java received | 每轮软件queue drop | 每轮processed | drop比例中位数（范围） |
|---|---:|---|---|---|---|
| low_20k | 40000 | 40000,40000,40000 | 0,0,0 | 40000,40000,40000 | 0.0000% (0.0000–0.0000%) |
| normal_100k | 200000 | 200000,200000,200000 | 1907,1916,1526 | 198093,198084,198474 | 0.9535% (0.7630–0.9580%) |
| overload_100k_delay50us | 200000 | 200000,200000,200000 | 182324,182485,182042 | 17676,17515,17958 | 91.1620% (91.0210–91.2425%) |

20k/s的3轮未观测到丢失；100k/s、无人工延迟也观测到约0.8–1.0%的软件queue丢失。这里包含冷JVM编译、操作系统调度及pacing追赶突发，不能把这个数字换算成“Java只能处理多少条”的固定阈值。

给工作线程每条请求sleep50µs后，消费者明显跟不上100k/s offered，队列256持续drop-new，丢失约91.2%；生产者没有等待慢消费者。Thread.sleep实际等待可能远大于50µs，不能把50µs直接当成实测服务时间或推导消费者20k/s能力。

9轮均满足 `offered=kernel_accepted`、`kernel_accepted=Java received`、`received=invalid+queued+queue_drop`、`queued=processed`。所以本轮接收后的缺口可定位到有界软件队列；这不意味着任意机器、网络或更高负载都不会在socket/NIC/链路丢包。

协议测试全部通过：C++ fixture + Java跨语言读取；正常100条与畸形3条UDP smoke得到received103、processed100、invalid3、queue_drop0。

原始记录：`results/workloads.csv`；每轮完整命令/stdout/校验数据：`results/*-rep*.json`；汇总：`results/summary.json`。`build-test.log`、`smoke.log`、`load.log`保留本次执行输出。
