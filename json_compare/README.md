# 三种 JSON 库与官方固定宽度 Protobuf

本轮选择 nlohmann/json 3.12.0 作为常见通用基线，yyjson 0.13.0 和 Glaze 9.1.0 作为高性能候选。C++ 标准库没有官方 JSON 实现；本轮不宣称它们在所有负载下是全球最快的库。

先读 [实测报告](RESULTS.zh-CN.md)。原始数据在 [results/benchmark.csv](results/benchmark.csv)，所有纳秒数均为批平均服务成本，不是消息延迟百分位。

## 相同逻辑数据

沿用 [官方 PB schema](../core_handoff/log.proto)：5 个 fixed64、4 个 fixed32；平铺结构带一个变长字段，嵌套结构是 common 与 http，带 url/host/cookie 三个字段。每种结构使用 4096 个变化记录，尾部总长度 0/64/256/1024 字节；两种结构的区别同时包括嵌套和字符串字段数。

两类输入：

- `utf8_escaped`：合法 UTF-8，混合 ASCII、中文、引号、反斜线、换行、制表符和 NUL。JSON 库必须正确转义及还原，PB bytes 原样存储。
- `binary_base64`：任意二进制字节；三种 JSON 共用标准带 padding 的 Base64，PB 原样存储。分别测已转换内容和包含实际 Base64 转换的完整路径。共用 Base64 为本测试的标量实现，不代表最快的向量化实现。

JSON 的 64 位字段都是精确十进制数字，没有经过 double。它不是官方 ProtoJSON 映射（后者将 64 位整数映射为字符串）。C++ 往返成功不意味着 JavaScript 的普通 Number 类型也能精确保留这些整数。

三种 JSON 解析器接收**完全相同的规范输入字节**：用 nlohmann::ordered_json 按 schema 声明顺序在计时外生成，键名和字段内容一致。各自编码器可用自身自然键顺序；输出尺寸单独记录。测试没有启用“假定输入键顺序固定”的解析选项；另有重排字段功能验证。

## 六个阶段

| stage | 工作边界 |
|---|---|
| encode_prepared | 编码已经填好的对象，轮转 4096 个拥有式对象；不含对象填充和 Base64 |
| encode_fields | 从变化业务记录复制全部字段到可复用对象，再输出编码字节；二进制输入已预转 Base64 |
| parse_only | 解析完整编码，保留拥有式库对象；DOM 库尚未逐项读取业务字段，Glaze 已映射到静态类型 |
| decode_fields | parse_only 后读取全部九个标量，复制所有字符串到统一拥有式 Record；不含 Base64 还原 |
| encode_raw | 仅二进制档：包含实时 Base64 转换、字段填充、JSON 输出；PB 直接填原始字节并编码 |
| decode_raw | 仅二进制档：包含完整字段提取、字符串复制，以及 Base64 严格校验与还原；PB 直接提取字节 |

`decode_fields`/`decode_raw` 的统一 Record 拷出是本测试接口的工作，业务若直接使用已解析的 PB/Glaze 对象或 DOM 引用，可避免这次拷贝；`parse_only` 另报，但 DOM 构建不能直接当作已完成所有业务字段访问。六个阶段不是可相加的端到端延迟分解；特别是 prepared 的 4096 对象工作集与 fields 的单个复用编码器不同。

## API 和内存复用

- nlohmann/json：使用默认 `json` 类型的拥有式 DOM（文档对象模型）。固定编码结构的节点指针预先定位，字符串容量重用，避免把已知 schema 的节点创建或键查找强加给每次 load。输出使用公开 `dump()`，该接口返回新字符串。解析使用严格 `json::parse`，每次建立新 DOM。
- yyjson：编码 DOM 的九个标量节点预先定位，字符串复制到适配器自有 std::string 后由 DOM 引用，应用输入可以立即释放；不是借用应用输入。输出用公开 `yyjson_mut_write_buf` 复用缓冲。解析用非 insitu（原地）严格模式和官方池化分配器，池按 `yyjson_read_max_memory_usage` 扩容并复用，解析字符串归库对象所有。
- Glaze：静态 C++ 类型、拥有式 std::string，编码和解析对象及输出容量重用；读取前清除旧字段值并保留字符串容量。显式启用 UTF-8、跳过字段和尾部校验；关闭 null_terminated、is_padded、partial_read 等输入假设。未知字段允许但仍严格检查语法。
- Protobuf：官方 3.21.12 生成类，重用消息及 oneof 子对象；全部 setter 后执行 ByteSizeLong，再 SerializeWithCachedSizesToArray。解码使用官方 ParseFromArray。

所有库都使用各自合理的公开 API；这不是只改变库名、其余分配行为完全相同的单变量实验。JSON 的 owning（拥有式）指内容不依赖输入缓冲区；yyjson 的池和 Glaze 的 std::string 对象都必须活到字段使用结束。仅提供完整声明字段且无重复键的生产消息；不同库对缺失键、重复键的默认语义并不统一。

## 构建与复现

需要 Linux x86_64、逻辑 CPU0、GCC 14（Glaze 9.1 要求 C++23）或兼容工具链、Python3 标准库，以及 Protobuf 3.21.12 的兼容开发头文件和运行库。yyjson 源码与 Glaze 头文件已保留，nlohmann 单头复用仓库 deps 目录；来源和下载 SHA 在 [dependencies.json](dependencies.json)。

```bash
# 从 json_compare 目录执行；如使用系统 PB 包可省略 DEP_ROOT。
DEP_ROOT="$PWD/../deps/root" bash build.sh
./bench --smoke
# 会覆盖已有计时结果，建议在独立 checkout 中运行。
python3 run.py
python3 summarize.py
```

先完成编译与功能检查，再独占计时 CPU，避免其他重负载。`run.py` 记录源码/依赖/二进制 SHA、cgroup CPU 配额与节流、UTC 时间和子进程资源。主矩阵 64 种配置，每种 4 个基本阶段，二进制档另外 2 个阶段，全部 5 次重复；320 组共 1600 行，每条保留测量至少 80ms。配置和阶段顺序使用固定随机种子打散。

没有真实网络发送、DPDK、跨核队列或 Flink；也没有真实抓包解析。当前数字不应与前轮不同工作集的纳秒数拼接成链路延迟。

## 功能核验

独立 `--smoke` 路径逐字段比较全部内容，而非只检查首尾：8192 条自身往返、18432 条三 JSON 库交叉往返，覆盖完整宽度整数、零值、两种结构和所有尾部档位；另有 128 次输入生命周期检查、120 个错误 JSON/类型/越界输入拒绝、16 个 Base64 正反例。数字均来自实际运行计数。

DOM 的语法解析可以先接受合法 JSON 数字，再由业务读取阶段拒绝类型/范围不合的字段；错误类型测试检查 parse + read_back 整体。性能热循环不执行完整内容比较，以免把验证成本混入库成本。
