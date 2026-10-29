# JSON 库选型与实测口径审查

核查日期：2026-10-29。本文件只做官方版本、API 和语义核查，没有运行基准或编译代码。实验应固定实际依赖版本、源码提交或归档校验值；滚动文档中的新功能不自动属于所测版本。

## 选型结论

按用户新增要求，采用一项常用通用基线和两项性能候选：

| 库 | 本次建议固定版本 | 官方稳定版发布日期 | 比较中的角色 |
|---|---|---|---|
| nlohmann/json | v3.12.0 | 2025-04-11 | 常用通用 C++ JSON 基线，默认使用拥有式 DOM（文档对象模型） |
| yyjson | 0.13.0 | 2026-09-08 | 可从 C++ 使用的 ANSI C 高性能 DOM 读写库 |
| Glaze | v9.1.0 | 2026-10-28 | 固定 C++ 类型直接映射的高性能读写库 |

三者的官方 latest release 均已核实为上述版本且非 prerelease（预发布版）。[N1,Y1,G1] nlohmann/json 在这里代表“常用通用库”，不应称为 C++ 标准库官方 JSON 实现。yyjson 和 Glaze 是合理的强候选，覆盖不同的 API 与内存模型；本次选择不能证明它们是绝对“全球最强”，也不能由上游项目自己的基准排名推导出本机、本字段结构或跨机链路的赢家。

不宜用“simdjson 没有 JSON 写入能力”作为排除理由：当前官方文档已经提供 builder（构建器）与 `to_json` 等写入接口。此次限制为两个高性能候选是实验范围选择，而非对其他库能力作绝对判断。[S1]

Glaze v9.1.0 要求 C++23，官方列出的编译器范围包括 GCC 13+、Clang 18+；不能把旧版的 C++20 说明套用到此版。应记录实际编译器和优化参数。若环境不满足而改用旧版，必须修改版本声明并重新检查该版默认选项。[G1–G2]

## 必须先固定的协议

### 整数：JSON 数字与 ProtoJSON 数字字符串是两个配置

本次字段若是 `uint64_t` / `uint32_t`，普通 C++ JSON 对照可以统一输出十进制 JSON 数字，解析时保留精确的无符号整数类型。不要经 `double` 中转；不能因为某些消费者使用二进制浮点数，就断言 JSON 语法本身只能表示到某个固定整数上限。RFC 8259 允许实现限定数字范围和精度，并指出常见二进制浮点实现的互操作限制。[J1]

若配置声称与 **ProtoJSON** 兼容，则官方映射要求 64 位整数输出为十进制字符串，32 位整数为数字，`bytes` 为 Base64 字符串；普通 JSON 数字模式应另外命名，不能冒称完整 ProtoJSON。[P1] 与 Java/Flink 等下游互操作还需验证实际解析器使用的数值类型，不能仅凭 C++ 往返成功保证全部消费者保留完整 `uint64_t`。

功能核验至少覆盖零、各整数边界、超过常见浮点精确范围的合法数、最大 `uint64_t`，并确认负数、越界数、浮点数和错误类型不会被业务解码器静默接受。重复键、缺失键与未知键也要有明确策略；库默认行为不完全一致。

### payload：任意二进制必须有明确文本表示

随机二进制 payload 不保证是有效 UTF-8，不能直接塞进 JSON 字符串后与二进制协议比较。建议三库共用同一套标准 Base64 编解码实现、同一字母表和 padding（补齐符号）规则。标准带 padding Base64 的字符数为 `4 * ceil(payload_bytes / 3)`；这是协议尺寸关系，不是性能预测。[B1]

nlohmann/json 的 `binary(...).dump()` 不是 Base64：它输出包含整数数组 `bytes` 与 `subtype` 的对象。拿这个默认表示与另两库的 Base64 字符串比较，会同时改变协议和工作量。[N3]

建议分别报告：

1. **JSON 本身成本**：输入已经是 Base64 文本，编码/解析 JSON；明确 Base64 转换未计入。
2. **原始 bytes 端到端成本**：编码包括 bytes → Base64 → JSON；完整解码包括 JSON → Base64 → 原始 bytes，并验证恢复内容。

三库统一 Base64 尤其重要：Glaze v9.1.0 自带 `read_base64()` 遇到 padding 或非法字符会停止并返回已解出的前缀，没有独立错误结果，不能直接把它当严格 Base64 校验器。[G8] 正确性校验应核对完整恢复内容；只比较长度及首尾不足以发现中间内容损坏。

## nlohmann/json v3.12.0：保留自然的通用基线

- 读：使用有明确边界的 `json::parse(first, last, ...)`。可令 `allow_exceptions=false`，但必须检查 `is_discarded()`；这只是改变报错方式，不表示允许跳过语法校验。保持不接受注释等扩展。[N2]
- 写：`dump(-1, ' ', false, error_handler_t::strict)` 是紧凑 JSON、保留合法非 ASCII 字符、严格处理错误 UTF-8 的公开接口。不要用 pretty print（美化输出）增加其不必要工作，也不要用替换/忽略坏字节模式宣称完整无损往返。[N3]
- 默认数字存储能精确保留合法 64 位有符号/无符号整数，但 `get<T>()` 允许 C++ 数值转换，不能把 `get<uint64_t>()` 当成完整的类型和范围验证。读取业务无符号整数应先核对类型；32 位字段还需上界检查，避免负数或过大的整数被转换后接受。[N4]
- 默认基线包括从业务记录构造 DOM，再序列化，以及解析 DOM 后读取字段的工作。若只对预建且不变的 DOM 调 `dump()`，应标明“仅输出既有 DOM”；不得把它与另库的完整记录编码混称同一操作。
- `dump()` 返回新的字符串；若其他库使用复用输出缓冲区，应如实记录这种公开 API 差异。可分别保留普通生命周期配置与复用配置，不应把容量增长、分配或回收费用默默挪到计时外。

注意线上滚动文档已出现 3.13.0 unreleased（未发布）功能，如新的字符串视图或错误处理选项；本次 v3.12.0 应以固定版本源码/文档为准。[N2–N4]

## yyjson 0.13.0：DOM、分配器与字符串借用

建议读入使用 `yyjson_read_opts(..., YYJSON_READ_NOFLAG, allocator, &error)`，检查 document 与错误结果；释放 document 的必要工作应有明确归属。默认模式执行标准 JSON 与 UTF-8 校验。不要给它开启 `NUMBER_AS_RAW`、`STOP_WHEN_DONE`、允许无效 Unicode 等选项，同时让别的库完整验证，却把结果放在同一严格配置里；同时记录 `YYJSON_DISABLE_UTF8_VALIDATION` 等编译宏。[Y2–Y3]

数值方面，`yyjson_get_uint()` 本身不是校验入口：有符号整数也可能被转换，非整数会得到零。应先 `yyjson_is_uint()`，然后读取；`uint32_t` 再核对范围。超出无符号整数范围的输入可能退化为 double，必须由业务类型检查拒绝，不能认为解析为某种 number 就有效。[Y2–Y3]

写入采用 mutable document（可变文档）与 `yyjson_mut_write_opts`，或 0.13.0 提供的 `*_write_buf` 复用输出缓冲区。后者避免输出分配，但需要额外临时空间，不能仅按预先测到的最终 JSON 字节数加终止符分配；必须遵守官方容量说明并检查失败结果。[Y2–Y3]

`yyjson_mut_strn` 引用调用者提供的字符串，并不复制；`yyjson_mut_strncpy` 才复制到文档。引用字符串在 document 使用期间必须持续有效且不变。如果以原生业务记录为编码输入，使用借用字符串是正常 API 优化，但应标出 DOM 借用这一条件，不能声称与拥有式 DOM 完成了完全相同的内存工作。

`YYJSON_READ_INSITU`（原地解析）会修改并借用输入，至少需要官方 `YYJSON_PADDING_SIZE` 的额外空间，该版为 4 字节；输入活到 document 释放。它应是单独变体，包含恢复/取得可修改输入所需的成本。尤其不能对同一份已解转义内容无限重复解析，并把首次输入恢复费用省掉。[Y2–Y3]

可复用池分配器；每条 document 的释放与复用顺序必须正确，避免累积占用。已知有序字段可用对象迭代查找优化字段读取，但它仍先完成了 DOM 解析，不能称为只解析了所取字段。

## Glaze v9.1.0：结构体直接映射与校验选项

正常接口是 `glz::read_json(object, input)`、`glz::write_json(object, output)`；返回的 `error_ctx` 为错误上下文，`if (ec)` 表示存在错误。可复用 `std::string` 输出，也可用检查容量的固定 span/array（连续区域视图/数组）；裸 `char*` 输出没有同样的容量检查，不宜与其他有边界检查配置混用。[G3–G4]

建议严格主配置显式确认下列条件：[G5]

| 选项/条件 | 本次基线建议 | 原因 |
|---|---|---|
| `validate_utf8` | 保持 `true` | v9.1.0 默认已开启；不能据旧资料声称默认不验证 |
| `validate_skipped` | `true` | 默认 `false`；只取部分字段或忽略未知字段时，跳过内容仍需语法校验 |
| `validate_trailing_whitespace` | `true` | 默认 `false`；需要检出根对象后面的非法尾随内容 |
| `error_on_unknown_keys` | 按协议统一 | 默认 `true`；部分字段投影一般需允许其余合法字段，再严格验证被跳过内容 |
| `error_on_missing_keys` | 按协议统一；必需字段应检查 | 默认 `false`，复用对象时缺失字段可能留下上条记录的值 |
| `null_terminated` | 对任意网络切片设 `false`，或明确提供尾零 | 默认 `true` 承诺 `data()[size()]` 为零；普通切片通常不能保证 |
| `is_padded` | 只有确实提供额外空间才开启 | 与尾零是不同条件；该版要求额外 16 字节可读空间 |

`minified=true` 假设无多余空白，`raw_string=true` 改变转义处理，`assume_sufficient_buffer=true` 跳过固定输出容量检查。这些都属于带前提的性能变体，应单独标明，不能与普通安全输入接口混成一个“最佳结果”。主线保持正常优化级别，不使用为减小代码而设的 `size` 模式。[G5,G9]

固定结构体的 `uint64_t` 能保留完整无符号整数，官方有全范围往返测试。不要为了动态 JSON 使用一个以 double 储存所有数字的类型；若测 Glaze 动态对象，需要明确是否选择了保留整数的类型。`quoted_num` 会改变线协议，三库必须统一。[G7]

**拥有式 `std::string` 与借用式 `std::string_view` 的读取结果不是无条件相同。** v9.1.0 的字符串视图直接引用 JSON 输入片段，保留 `\"`、`\\` 等转义文本；官方测试明确检查这个行为。主线采用 `std::string` 更容易与完整反转义语义对齐。无转义 Base64 内容可以另设借用变体，但输入必须保活，而且该结果不能推广到任意转义字符串。[G6]

## 建议交给实测代码的比较矩阵

| 操作 | 必须计入/确认 | 不应混淆 |
|---|---|---|
| encode | 从同一业务记录生成 JSON；所需 DOM/结构映射、字符串处理和输出写入 | 预建 DOM 输出 vs 完整记录编码 |
| decode_all | 解析完整输入、取得全部标量、正确读取/反转义字符串；bytes 模式包含 Base64 还原 | 完整 owning 解码 vs 引用文本片段 |
| decode_few | 从同一完整 JSON 取相同几个字段；明确其余内容是否完整验证 | 少取字段 vs 停在中途、不验证其余输入 |
| roundtrip | 每次都使用本轮编码结果，再完整读取和检查 | 只读不变旧缓冲区、使用预先算好的摘要 |
| reusable/insitu 扩展 | 报告缓冲区容量、输入修改/恢复、池重用和寿命 | 少分配模式 vs 普通分配模式未标记 |

正确性检查应置于性能测量前，覆盖库间互读、所有标量、完整 payload、转义字符、非法尾部、错误数值、未知/缺失字段及重复键策略。循环中的结果要通过可观测摘要防止优化，但性能摘要不是完整正确性验证的替代品。

数据组应包括实际业务文本和统一 Base64 的随机二进制两类，分别报告原始 payload 大小与 JSON wire bytes（线上字节数）。字段名、是否输出默认值、紧凑格式、转义策略、数值表示、键顺序和对象生命期均需说明。固定短字段名或完全无转义 ASCII 可以是有效业务模型，但不能据它泛化所有 JSON 文本。

测量报告应保留每轮原始结果、重复统计、预热、线程/CPU 绑定、编译器/编译宏、分配复用条件和验证选项。批量循环均值不是单消息延迟分位数；内存内编解码结果也不能直接变成端到端网络或 DPDK/Flink 性能结论。没有实测前不填写胜负、倍数或固定纳秒值。

## 官方来源

- **[N1] nlohmann/json v3.12.0 release**：<https://github.com/nlohmann/json/releases/tag/v3.12.0>；最新稳定版元数据：<https://api.github.com/repos/nlohmann/json/releases/latest>
- **[N2] nlohmann/json v3.12.0 parse API**：<https://github.com/nlohmann/json/blob/v3.12.0/docs/mkdocs/docs/api/basic_json/parse.md>
- **[N3] nlohmann/json v3.12.0 dump API**：<https://github.com/nlohmann/json/blob/v3.12.0/docs/mkdocs/docs/api/basic_json/dump.md>
- **[N4] nlohmann/json v3.12.0 Number Handling**：<https://github.com/nlohmann/json/blob/v3.12.0/docs/mkdocs/docs/features/types/number_handling.md>
- **[Y1] yyjson 0.13.0 release**：<https://github.com/ibireme/yyjson/releases/tag/0.13.0>
- **[Y2] yyjson 0.13.0 API 文档**：<https://github.com/ibireme/yyjson/blob/0.13.0/doc/API.md>
- **[Y3] yyjson 0.13.0 公共头文件与完整契约**：<https://github.com/ibireme/yyjson/blob/0.13.0/src/yyjson.h>
- **[G1] Glaze v9.1.0 release**：<https://github.com/stephenberry/glaze/releases/tag/v9.1.0>
- **[G2] Glaze v9.1.0 README / 编译要求**：<https://github.com/stephenberry/glaze/blob/v9.1.0/README.md>
- **[G3] Glaze v9.1.0 Reading**：<https://github.com/stephenberry/glaze/blob/v9.1.0/docs/reading.md>
- **[G4] Glaze v9.1.0 Writing**：<https://github.com/stephenberry/glaze/blob/v9.1.0/docs/writing.md>
- **[G5] Glaze v9.1.0 Options / JSON conformance**：<https://github.com/stephenberry/glaze/blob/v9.1.0/docs/options.md>；<https://github.com/stephenberry/glaze/blob/v9.1.0/docs/json.md#json-conformance>
- **[G6] Glaze v9.1.0 string_view 实现与官方转义测试**：<https://github.com/stephenberry/glaze/blob/v9.1.0/include/glaze/json/read.hpp#L1603>；<https://github.com/stephenberry/glaze/blob/v9.1.0/tests/json_test/json_test.cpp#L1490>
- **[G7] Glaze v9.1.0 uint64 / generic_u64 测试**：<https://github.com/stephenberry/glaze/blob/v9.1.0/tests/json_test/json_test.cpp#L1536>；<https://github.com/stephenberry/glaze/blob/v9.1.0/tests/json_test/generic_int_test.cpp#L264>
- **[G8] Glaze v9.1.0 Base64 解码实现**：<https://github.com/stephenberry/glaze/blob/v9.1.0/include/glaze/base64/base64.hpp#L24>
- **[G9] Glaze v9.1.0 Optimization levels**：<https://github.com/stephenberry/glaze/blob/v9.1.0/docs/optimization-levels.md>
- **[J1] RFC 8259：JSON 数据格式，尤其数字、对象与 UTF-8 互操作约束**：<https://datatracker.ietf.org/doc/html/rfc8259>
- **[B1] RFC 4648：Base64 字母表、补齐与输入合法性**：<https://www.rfc-editor.org/rfc/rfc4648>
- **[P1] Protobuf 官方 ProtoJSON 映射**：<https://protobuf.dev/programming-guides/json/>
- **[S1] simdjson 官方 Builder 文档**（滚动主分支，仅用于纠正“没有写入能力”的绝对说法）：<https://github.com/simdjson/simdjson/blob/master/doc/builder.md>
