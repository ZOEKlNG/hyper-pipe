# 逻辑 CPU0/2 亲和性敏感性补测

原始 `handoff.cpp` 保持主实验 SHA。`prepare_cpu2.py` 生成独立 `handoff_cpu2.cpp`，仅将日志线程从CPU1改为CPU2，并把矩阵缩为两schema、tail256、queue256、batch32、五模式；`build_cpu2.sh` 复用冻结的生成PB代码，不运行protoc。

执行 `./build_cpu2.sh`，`./handoff_cpu2 --smoke`，`python3 run_cpu2.py`。功能检查100030条完整字段/payload+10次复用切换全部通过。正式50样本（10配置各5次），最短0.30787064秒，错误0，源文件/二进制SHA一致，墙钟26.363秒。

此补测选择reported cache id不同的逻辑CPU0/2：主测试CPU0/1被VM报告为共享L1/L2标识。该拓扑无法证明底层独立物理核，因此不把任意一对称为“两个独立物理核”。

| 模式 | CPU0/1 flat | CPU0/2 flat | CPU0/1 nested | CPU0/2 nested |
|---|---:|---:|---:|---:|
| A native | 2.298 | 2.346 | 2.312 | 2.336 |
| A scratch | 2.743 | 2.742 | 2.738 | 2.717 |
| B聚合复制 | 2.962 | 2.941 | 1.728 | 1.751 |
| B直接wire | 3.133 | 3.110 | 2.660 | 2.694 |
| D共享PB对象 | 2.532 | 2.518 | 1.509 | 1.489 |

单位为5次运行中位百万条/秒。两组中位数差约−1.3%到+2.1%，这部分比较没有改变主实验排序；若干配置仍有高吞吐离群。主矩阵与敏感性矩阵的运行时间、完整配置次序不同，本表是敏感性对照，不是对底层拓扑物理原因的证明。两端CPU均含自旋，不是主动服务成本。

完整新数据、每次范围、拓扑JSON、源文件/二进制快照、CPU前后证据与运行命令在 `results_cpu2/`。逐项对照为 `affinity_comparison.csv`；原source与生成source SHA、编辑列表在 `cpu2_generation.json`。
