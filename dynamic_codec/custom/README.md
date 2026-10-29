# 自定义 Hybrid TV/TLV 与 ProtoEmitOp 编码原型

这里是本轮新写的 C++ 基准实现，**不是现成通用库**。目标是测量：固定业务结构体可以在运行时选择字段，配置预编译为操作表，逐消息按操作表编码；编码和网络传输解耦。它不实现生产级 schema registry、动态迁移、任意 Protobuf 反射或所有数据类型。

## 集成接口

`codecs.hpp` 是 header-only（仅头文件）实现，依赖上级 `common.hpp`，namespace 为 `dynbench`。

```cpp
validate_custom_plan(plan); // 每次加载/替换配置时调用，放在计时区外
HybridCodec hybrid;
ProtoEmitCodec proto_varint(false);
ProtoEmitCodec proto_fixed(true);
codec.encode(event, plan, out);   // size_t，复用调用方 std::string 容量
codec.decode(wire, false);       // 全部已出现业务字段的 checksum
codec.decode(wire, true);        // 只消费 id + seq + type 的 checksum
```

公共 Event 与既有基准一致：64B Header 含5个u64（id/seq/timestamp/value0/value1）、4个u32（producer/stream/type/flags）及header/version/payload长度元数据，另有 `std::string payload`。字段号1..9依次对应上述标量，10是payload。Header元数据不参与本轮wire字段和checksum。

Plan 中逻辑标量类型为 U32/U64，payload 为 Bytes；`ProtoEmitCodec(false)` 输出全部数值为 varint 的 Event schema，`true` 输出u64→fixed64、u32→fixed32的 FixedEvent schema。这两种整份 schema 分开测试，**不承诺任意混合字段类型 schema**。实现没有逐消息构造或解析动态 JSON；根目录配置编译器产生 source offset、字段选择mask、tag bytes 后传入。切换Plan不重新编译C++，也不依赖上一条消息状态。Plan必须不可变且经校验后才能交给并发调用者。

`validate_custom_plan` 检查重复fid、fid范围、mask、业务字段offset/宽度、payload getter以及预编译的两种PB tag。编码输入是本进程 Header 的数字值，读取用memcpy；输出明确为LE或标准PB wire，不把native struct的padding当线格式。

两种编码器都省略数值0和空payload，与本次proto3默认值语义对应。缺失字段解码为0/empty；显式0/empty也等价。这个优化不能泛化到带显式presence的optional、oneof或任意schema。用户生成的Protobuf代码本来也支持动态选择设置哪些字段，本实验不声称它缺少该能力。

## Hybrid TV/TLV 线格式

每个字段独立编码，消息边界由外层frame长度提供，没有字段总数或结束tag。

| 内容 | 编码 |
|---|---|
| tag | u16 little endian，高2bit为type，低14bit为fid |
| type0 | tag后紧跟4B little endian u32 |
| type1 | tag后紧跟8B little endian u64 |
| type2 | tag后u16 little endian字节长度，再跟payload字节 |
| type3 | 保留；拒绝 |

fid0拒绝；未知fid1..16383按其type校验并跳过，不参与checksum。已知fid1..5只接受type1，6..9只接受type0，10只接受type2。奇数字节地址也使用显式安全读取，不做未对齐的类型指针强转。重复已知singular字段采用last-wins（后值覆盖前值），bytes也如此。

单个bytes长度上限65535，65536必须拒绝，不能缩窄到u16后静默截断。解码frame上限1MiB；编码还采用保守的最大输出空间预算，因此临近1MiB边界可能比实际wire长度更早拒绝。该预算不影响性能语料的0/64/256/1024B payload。

## ProtoEmitOp 与受限PB视图解码

ProtoEmitOp编码器依次执行Plan中的field操作：从已知source offset获取值，复制预编译tag bytes，写varint或fixed32/fixed64，payload写标准length-delimited长度和字节。**输出是真实标准Protobuf wire**；功能测试用官方3.21.12生成的Event/FixedEvent验证，而不是用另一个手写parser自证。

它只支持此Event模型、无符号u32/u64、fixed32/fixed64、bytes和字段子集。没有嵌套message、signed/ZigZag、repeated/packed、map、oneof、未知字段保留等。标准PB字段顺序可以不同，因此比较按值而非强求字节序列相同。

手写解码器另名为 `pb_wire_view_checked`，不等同于官方Protobuf parser。Codec实例按Varint或Fixed模式检查已知字段类型；自由函数还提供显式 `PBScalarWire::Either` 模式用于统一查看两种数字表示，默认参数是Either。未知字段只允许wire0/1/2/5并完成边界检查后skip；groups与wire6/7拒绝。fid0、过长tag、u64溢出、unterminated varint、截断长度/数据、超过1MiB均拒绝。

两个刻意更严格的边界：已知字段wire类型不匹配时本实现拒绝，而特定schema的官方parser通常把它视为未知字段；u32值超过UINT32_MAX时本实现拒绝，不仿照某些官方路径的缩窄行为。因此它不是全PB兼容解码器。有效测试语料只使用计划所定义的schema类型与取值范围。

## “零拷贝”与计量边界

`CustomDecodedView` 在栈上保存9个标量和一个指向输入wire的 `string_view`；payload不复制、不为其分配内存。该view只在原输入buffer保持存活且不改写期间有效。它不是由完整业务对象组成的拥有型反序列化结果。

`decode(..., true)`仍扫描并验证完整消息，然后仅消费id/seq/type；不是跳过其余报文字节的可信短路parser。all-fields checksum读取所有业务标量和payload长度/首/尾，不扫描整段payload；**功能测试**另外逐字段核对并比较完整payload字节，防止checksum掩盖字段交换或内部字节错误。

编码器仍将Header数字写入输出，并复制payload；本实现没有把网络、共享内存、DMA或IPC整合进来。输出std::string采用最大长度预留/resize再缩短，包含该策略的开销，不能称完全没有编码/复制。与官方PB拥有型parse对比时，必须注明输出对象所有权和payload拷贝不同。

所有性能均由根harness在CPU隔离时串行运行；此目录只提供功能测试，不在测试脚本中偷偷跑benchmark。批量ns/op不能标成逐消息p99。

## 已执行功能验证

```bash
bash custom/test.sh  # 从dynamic_codec目录，或进入custom后 ./test.sh
```

需要C++20、官方Protobuf3.21.12生成的 `event.pb.h/.cc` 和兼容runtime。默认从相邻原始codec与deps目录读取，可用 `DEP_ROOT`、`PROTO_GEN_DIR`、`CXX`覆盖。

本轮功能执行结果：**138575 checks，512个变化事件，PASS**。另外用AddressSanitizer+UndefinedBehaviorSanitizer（地址/未定义行为检查）编译并运行相同用例，PASS，无诊断。

覆盖：4个payload尺寸、full/sparse/reordered/empty计划、同实例Plan连续切换、zero/empty、UINT64_MAX/高位u64、0xff与嵌入NUL payload、官方PB逐字段及完整payload双向交叉验证、未知字段跳过、重复字段last-wins、每个单字段内部截断、fid0/type3/错误wire、varint溢出、Hybrid65535/65536边界、frame上限、无效Plan。

检查结果记录在 `functional_validation.json`。这些用例增加可信度，不代表经过生产级模糊测试、所有schema兼容性或安全审计。
