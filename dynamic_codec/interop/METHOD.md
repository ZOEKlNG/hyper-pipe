# C++ → Java 编码互操作功能验证

本目录只验证编码互操作与输入校验，不收集编解码性能，不产生 Java 吞吐或延迟结论。

## 验证对象

- `ProtoEmitOp` 的变长整数线格式使用官方生成的 `bench.Event` Java解析器。
- `ProtoEmitOp` 的固定宽度线格式使用官方生成的 `bench.FixedEvent` Java解析器。
- `realupb` 导出的线格式按相同规则交给官方 Java 解析器。
- `HybridTVTLV` 使用本目录独立实现的、检查输入边界的 Java 解析器。

Java生成代码来自已有 `event.proto` 与官方 Protobuf 3.21.12 代码生成器；运行时为 `protobuf-java-3.21.12.jar`，使用 Java21 JRE。实验环境只有JRE，没有javac，因此借用既有ECJ编译器编译本目录源码。工具、生成源码及fixture的SHA-256保存在结果JSON，方便核查实际输入。

`Event` 与 `FixedEvent` 是两种不同的Protobuf字段类型约定，测试为每条消息选择对应类型；不能把本测试理解成两者在任意代码里可互换。Java不支持无符号long原语，但getter保留64位原始位模式；期望值以十进制字符串传递，用 `Long.parseUnsignedLong` 读入后逐位比较。uint32则转为0至2^32−1的long后比较。

## 线格式与比较方式

每条消息均比较9个标量字段：`id/seq/timestamp/value0/value1/producer/stream/type/flags`，以及完整payload字节。没有用一个求和校验和代替字段检查，以免字段互换等错误相互抵消。fixture由统一C++基准程序导出，以JSONL（每行一个JSON对象）保存：

```json
{"case":"edge","codec":"ProtoEmitOp_varint","plan":"full","protobuf_type":"Event","expected":{"id":"18446744073709551615","seq":"0","timestamp":"0","value0":"0","value1":"0","producer":"0","stream":"0","type":"0","flags":"0","payload_base64":"/wA="},"wire_base64":"..."}
```

Python驱动只负责无损读取JSON、检查数值范围与Base64、转换为Java易于读取的TSV（制表符分隔）文件。所有uint64仍按字符串传递，避免经浮点数丢失精度。真正解析wire与逐字段比较由Java进行。未被plan选择的字段期望为0，payload期望为空。

Hybrid中的TV表示标签与值，TLV表示标签、长度与值。每字段起始是2字节小端标签：高2位为类型、低14位为字段号；类型0为小端u32，类型1为小端u64，类型2为2字节小端长度和字节串。字段1–5要求u64，6–9要求u32，10要求bytes；字段号0与类型3拒绝。未知字段按其声明类型跳过，重复已知字段采用最后一个值。帧最大1MiB，单bytes长度最多65535。

## 派生校验与畸形输入

- 每条Protobuf消息追加未知字段127，再确认所有已知字段不变，并确认官方Java对象保留该未知字段；随后由Java重新序列化，再次确认已知字段。
- 每条Hybrid消息追加未知u32、u64、bytes字段，确认完整消息的已知字段不变。
- 13个明确的畸形输入拒绝案例：不完整Protobuf varint、截断bytes、非法0标签、截断fixed64；Hybrid半截标签、字段号0、保留类型、已知字段类型不符、截断标量、缺少长度、截断bytes、未知bytes截断、超过帧长上限。
- 另检查Hybrid重复标量、重复payload的最后值生效、65535字节payload长度边界，以及空Hybrid/Event/FixedEvent消息使用缺省值。

Java的Hybrid功能辅助解码器将payload复制为独立byte数组；它是正确性对照实现，不能当成Java零拷贝性能实现。

这些是有限的跨语言功能样本与定向边界测试，不是全面模糊测试、所有Protobuf特性兼容证明或内存安全认证。尤其不覆盖嵌套消息、repeated/map字段、groups、未知schema动态发现，也不证明自定义编码器适用于任意`.proto`。

## 运行

```bash
bash prepare.sh  # 缺少运行库时下载官方固定版本到本目录.deps缓存
python3 run_interop.py --fixtures results/fixtures.input.jsonl
```

源码在 `testsrc/InteropCheck.java`，驱动在 `run_interop.py`。驱动相对于仓库根目录查找已有Java生成源码与依赖；也支持 `--protobuf-jar`、`--ecj-jar`、`--generated-java` 指定本机路径。编译输出仅写入被Git忽略的 `classes/`，下载缓存仅在被忽略的 `.deps/`，不提交class文件或classpath缓存文件。结果写入 `results/summary.json`、`java.stdout/java.stderr`，编译输出和归一化fixture亦保留在结果目录。该驱动只写本目录，仅读取其它目录的官方依赖和生成源码。
