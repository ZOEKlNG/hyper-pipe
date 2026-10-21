# 高性能通信组件研究：报告、证据与源码

先打开 `研究报告.html`（离线、自带4幅图、可筛选汇总表），或阅读 `研究报告.md`。实测主机、消息定义和所有边界见报告第1节。没有物理双机DPDK/AF_XDP/RDMA测量。

## 直接查看

- **[完整研究报告](研究报告.md)**：GitHub 可直接阅读，含实测表、架构建议、模拟与局限。
- [HTML 报告](研究报告.html)：下载后可离线打开，含图表与可筛选结果表。
- [C++ → Java UDP 组件原型](component/README.md)：协议、路由、有界队列、编译与双机启动命令。
- [C++ 编解码与压缩基准](codec/README.md) · [SBE 基准](sbe/README.md) · [IPC 与模拟](ipc_sim/README.md) · [传输基准](transport/README.md) · [Java/Flink 基准](java_flink/README.md)。
- [C++/SBE 原始数据](results/) · [传输原始数据](transport/) · [Flink 长窗 CSV](java_flink/results/flink_long.csv) · [原型负载 CSV](component/results/workloads.csv)。

这些数据来自单机微基准、loopback、真实本地 Flink 及显式参数模拟；没有物理双机 DPDK/AF_XDP/RDMA 实测。完整 payload 未经过本地 Flink 实验的 shuffle。详细边界见报告。

## 内容

- `results/`：C++ codec/复制/压缩/SBE原始CSV与5轮摘要、环境、cgroup记录；SBE未记录cgroup一项如实标记。
- `codec/`：真实Protobuf/FlatBuffers/JSON及固定布局代码与4096条共同corpus。
- `sbe/`：官方SBE1.35生成器、schema和生成代码，边界校验/可信输入分开。
- `ipc_sim/`：实际SPSC共享池线程/fork测试；显式假设的队列/批量/路由模拟。
- `transport/`：TCP/UDP/ZeroMQ真实loopback、RTT与paced UDP，原始计数和进程CPU。
- `java_flink/`：真实Java decoder和Flink2.1 MiniCluster，主报告采用 `results/flink_long.csv`。完整payload没有穿过Flink shuffle。
- `component/`：可两机分别启动的C++→Java UDP原型；本次实测在同机。72B线格式、静态路由、有界队列、功能测试和9轮负载验证。它没有DPDK后端/生产Flink网络connector/迁移fence。
- `research/`：两份粘贴参考的核查、官方来源、独立代码和计数复核。

## 复现前提

Linux x86-64、小端，至少4个可用逻辑CPU（当前harness使用0/1/2/3），GCC支持C++20，Java21，Python3及numpy/matplotlib/markdown，curl、tar、dpkg-deb。CPU affinity写在各实验源码/参数中，cpuset不包含这些CPU时应先调整。不能把这个测量主机的数值直接当成另一个主机的容量。

Debian13可使用系统开发包：build-essential、libprotobuf-dev/protobuf-compiler、libflatbuffers-dev/flatbuffers-compiler、libzmq3-dev、liblz4-dev、libzstd-dev；nlohmann单头3.12已保留。Java工具由脚本bootstrap，不要求额外安装JDK。C++精确版本也可用 `python3 deps/fetch.py` 提取会话所用Debian开发包；**仍需兼容的Protobuf3.21/ZeroMQ4.3/LZ4/Zstd运行库**，不适合在其他发行版盲目替换系统库。manifest含上游URL与会话下载文件SHA256。没有提供构建容器，也没有声称全离线依赖齐备。

先构建全部程序，再串行计时，避免编译干扰。以下命令从本目录运行，耗时因硬件而异，正式运行会写各results目录：

```bash
bash codec/build.sh
(cd codec && mkdir -p corpus && ./bench --export-only >/dev/null)
bash sbe/build.sh
g++ -std=c++20 -O3 -march=native -pthread ipc_sim/ipc_bench.cpp -o ipc_sim/ipc_bench
g++ -std=c++20 -O3 -pthread -Ideps/root/usr/include transport/transport.cpp -Wl,-l:libzmq.so.5 -o transport/transport
bash java_flink/build.sh
ECJ_JAR="$PWD/java_flink/ecj.jar" bash component/build.sh

bash codec/run.sh
bash sbe/run.sh "$PWD/results"
./ipc_sim/ipc_bench ipc_sim/results/ipc_results.csv 0 5 0.35
python3 ipc_sim/summarize.py ipc_sim/results/ipc_results.csv
python3 ipc_sim/simulate.py --output ipc_sim/results
python3 transport/run_measurements.py
(cd java_flink && CORPUS_DIR="$OLDPWD/codec/corpus" python3 run_all.py)
(cd java_flink && CORPUS_DIR="$OLDPWD/codec/corpus" FLINK_RECORDS=30000000 SKIP_CODEC=1 RESULT_LABEL=long_window python3 run_all.py)
python3 component/smoke.py
python3 component/load_runner.py
```

Java长窗会输出到 `java_flink/results/long_window/flink_local.csv`。报告组装程序使用canonical `java_flink/results/flink_long.csv`；复跑后请复制长窗文件到该位置，再依次运行assemble_report.py、render_report.py。报告生成需Python markdown/matplotlib/numpy；`verify_report.py`是本会话用于浏览器验证的可选脚本（还需Playwright/Chromium），不属于基准运行必需项。

所有吞吐数据保留范围。codec/SBE的ns/op是批平均；IPC满队列自旋而不是drop；RTT是轻载往返；paced高档位有生成器未执行记录；Flink只有投影tuple走内部链路。不要把这些不同口径相加。

原始首轮SBE计时上限错误数据和可能与编译重叠的Java轮次已剔除。本包保留有效结果及修正说明；manifest列出文件SHA256，便于审计。
