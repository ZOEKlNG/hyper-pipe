# 跨语言互操作功能结果

状态：**PASS**。本次只运行少量功能测试，没有运行Java性能基准，也没有从这些测试推算吞吐或延迟。

| 校验范围 | 数量 | 结果 |
|---|---:|---|
| C++真实导出消息 → Java逐字段比对 | 168条、15种codec | 全通过 |
| 其中标准Protobuf → 官方Java生成类 | 156条 | 全通过 |
| 其中Hybrid → 独立Java checked decoder | 12条 | 全通过 |
| 追加未知字段、Java重序列化后的派生检查 | 324项 | 全通过 |
| 明确的畸形输入 | 13项 | 全部拒绝 |
| 重复字段、65535B bytes边界、空消息缺省值 | 4组 | 全通过 |

所有消息检查全部9个标量及完整payload字节。样本覆盖uint64最高位、uint64最大值、uint32最高位及最大值、0缺省值、含0x00/0xff/0x80的bytes，以及full/subset/no_payload与恢复full的连续plan切换。full有90条、subset39条、no_payload39条；静态full基线只导出full，其余实现复用同codec实例切换plan。

ProtoEmitOp的两种模式分别用官方 `Event` 与 `FixedEvent` 解析；真实upb v29.3输出也用相同官方Java生成类解析。Java Protobuf运行时为3.21.12。Hybrid未知字段的三种类型均能跳过，重复字段最后值生效；检查输入边界的实现拒绝不完整字段与不匹配的已知类型。

结果说明编码器输出在这些schema和样本下可被对应Java端正确读取。它不证明任意`.proto`兼容，不覆盖嵌套/repeated/map/groups或动态schema发现，也不证明upb alias解码缓冲的实际生命周期。Java Hybrid功能辅助代码主动复制payload为独立byte数组，不能将其作为Java零拷贝性能实现。

可复核文件：`results/summary.json`、`results/fixtures.input.jsonl`、`results/normalized_fixtures.tsv`、`results/java.stdout`、`results/java.stderr`、`results/compile.stdout`、`results/compile.stderr`。`summary.json`保存fixture、生成源码、Java运行库及测试源码的SHA-256，以及精确编译/运行命令。实现与边界在 `METHOD.md`，测试源码在 `testsrc/InteropCheck.java`。
