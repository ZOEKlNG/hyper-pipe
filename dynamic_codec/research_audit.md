# 动态可配编码参考：独立技术核查

核查日期：2026-10-29。本次完整阅读用户新增附件，只读核查官方源码与文档；未修改组件仓库、未编译、未运行性能测试。英文技术词首次出现采用中文释义，以便区分概念与具体API。

## 可直接采用的结论

新增的三种思路都值得实测：Hybrid TV/TLV（混合“标签—值”与“标签—长度—值”格式）、ProtoEmitOp（预编译字段写出指令表，输出标准Protobuf）、upb动态MiniTable（精简消息布局表）+Arena（区域内存分配器）。但附件中的13.5/19.2/27.5ns等新增数字没有可追溯原始数据、完整可运行基准、环境或重复测量，不能视为实测或据此宣布优胜者。

建议主对比沿用已有4096条变动记录与相同逻辑schema（模式定义），分别测“固定全集、动态字段子集、配置切换”；同时分清复制解析与alias（别名引用；字段切片直接借用输入内存）的所有权差异。upb必须修正Arena生命周期和API用法后再进入计时。[U01–U09]

## 最严重的代码问题

| 优先级 | 参考代码/主张 | 问题及可实施修正 |
|---|---|---|
| P0 | `on_u64(fid,*reinterpret_cast<const uint32_t*>(p))` | **确定的数据错误**：64位字段仅加载低32位，上半部分丢失。改成64位安全load，并用高32位非零样本校验；仅id/时间戳值很小的测试会掩盖bug。 |
| P0 | `reinterpret_cast<const uint32_t*>(p)`/原计划uint64指针解引用 | 两字节tag使后续值通常不满足4/8字节对齐；还涉及类型别名/对象生命周期。即使x86能执行非对齐load，也不能让C++未定义行为变正确。使用`memcpy`到相应宽度标量，再按显式little-endian（小端字节序）读取；编译器可优化为高效load。 |
| P0 | `EncodeCustomTlv(...,uint8_t*out)` | 没有输出容量参数，配置字段数或bytes长度变化即可越界写。改成输出span（带长度的连续内存视图）/`out,capacity`，每次写前检查剩余容量；冷路径预计算有上界的最大编码尺寸，但动态bytes仍须检查。 |
| P0 | `uint16_t len=static_cast<uint16_t>(sv.size())` | 大于65535的字段被静默截断，数据不完整却可能被当作成功。必须明确拒绝/分块/升级长度类型，禁止静默窄化。 |
| P0 | `upb_Arena_Free(arena)`之后继续使用`wire_buf`；随后在同一`scratch_mem`初始化decoder arena | 编码输出由编码Arena管理；先结束Arena再把输出当稳定输入不符合所有权契约。若使用堆扩展会释放内存；固定外部块虽物理上仍在栈中，但重初始化/后续分配可覆盖其内容，不能据此保证安全。**编码Arena与只读wire输入必须持续存活，decoder scratch必须独立且不重叠**；或先把wire复制到独立拥有的buffer再free。是否当前小样本刚好未覆盖，不改变错误的生命周期设计。[U03,U04,U08,U09] |
| P1 | 忽略`upb_Arena_Init`、`upb_Message_New`、setter、`upb_Encode`、`upb_Decode`返回值 | 固定Arena耗尽不能自动扩容；分配失败/非法wire可能让后续代码读NULL或半解码结果。逐项处理状态并计数，正确性阶段必须覆盖资源耗尽与损坏输入。[U03,U05,U06] |
| P1 | `upb_MiniTable_Build(data,len,platform,arena,status)` | 固定核查的v36.2公开函数只有四参`(data,len,arena,status)`；带platform的是下划线版本`_upb_MiniTable_Build`，不能混淆。优先用Native公开入口并按固定版本编译。[U02] |
| P1 | `mt->UPB_PRIVATE(fields)[i]` | 官方MiniTable内部结构注释明确要求用户不得直接读写成员。用`upb_MiniTable_FindFieldByNumber`在初始化时按field number（字段编号）查找并缓存字段指针，检查类型与存在性。数组下标不是业务field id，布局/版本也可能变化。[U07] |
| P1 | `else`把type2和type3都当bytes | 高两位共四类，但协议只定义0/1/2。保留类型3应明确拒绝或定义可跳过编码；不能在新版本赋予新意义时被旧解析器误当bytes。 |
| P1 | `field_id & 0x3FFF` | 越界编号会折叠到已有编号。配置验证时检查范围、重复编号、字段类型与getter映射，禁止静默mask。自定义协议若允许id0需明确，Protobuf字段号0无效。[P01] |
| P1 | `p+8>end`、`p+blen>end`、`while(p+2<=end)` | C++形成超过同一数组one-past-end（尾后一个位置）的指针本身就可能未定义；不能先构造坏指针再比较。用`remaining=end-p`后判断`remaining>=needed`，再推进指针。对外入口也需保证`buf,len`指向真实可读区域。 |
| P1 | 原始`memcpy`写tag、length、scalar | 实际编码用了主机字节序，未定义跨架构线格式。要求固定字节序并显式转换；C++与Java双方共同测试，不把同为x86的小端自测当跨平台保证。 |
| P2 | parser（解析器）对out_view逐字段回调后才发现尾部错误 | 失败时可能已对外产生部分副作用。应在临时view/索引中解析，校验成功再commit（提交），或明确回调只写可丢弃临时状态。重复字段、未知字段、必需字段、缺失值/零值语义还需定义。 |

## “256B”的口径不一致

附件写“与第一轮完全同构”，随后给出的示例却是**6个u64+4个u32+160B payload（载荷）**，这与旧基准的**5个u64+4个u32+256B载荷**不同，不能直接套旧数比较。

在字段编号1..11、scalar全部非零、不含外层帧的条件下，参考示例可直接算出：

- 裸字段值：`6*8+4*4+160=224B`。
- 自定义2B tag TV/TLV：`6*(2+8)+4*(2+4)+(2+2+160)=248B`。
- Protobuf fixed64/fixed32：`6*(1+8)+4*(1+4)+(1+2+160)=237B`。

因此表里的“PB编码后256B、TLV280–296B”没有被示例支持。以上只是线格式大小的条件算术，不是性能实测。Protobuf隐式presence（存在性）下零默认值可省略，真实均值也可能更小；应报告实际编码长度分布。[P01,P02]

## upb：哪些机制真实，哪些表述过强

**真实机制。** `kUpb_DecodeOption_AliasString`确实可使string/bytes及unknown fields（未知字段）借用输入，而非把其内容复制到消息Arena。`upb_Message_SetString`也不复制传入StringView，调用者负责其数据在消息使用期间保持有效。数值字段仍要解析、转换并写入消息布局，message对象及某些结构仍要分配，不能称为“完全不解码”。[U05,U06]

**真实机制。** upb编码器从后向前写，避免预先计算嵌套长度；源码有明确注释。这是算法选择，不保证比C++快固定百分比，也不自动省去预先填充upb_Message或最后复制进业务batch buffer的成本。[U08]

**Arena的真实边界。** `upb_Arena_Init(mem,n,NULL)`是固定容量Arena：够用时可以不触发系统堆分配，不够时失败。初始化状态、对齐和所有对象都占空间；4096B不是保证足够所有schema/输入的通用常数。`upb_Arena_Free`是结束生命周期和引用管理，不是可携带live对象的“仅回收游标”操作。有初始外部块的Arena也不能任意通过Fuse/IncRef延长生命周期。[U03,U04]

**API/ABI（调用接口/二进制接口）不稳定。** upb官方README明确如此，upb也没有独立稳定C库release；应固定其所属Protobuf版本、私有静态链接并隔离适配层，不把内部MiniTable二进制结构暴露成组件公共协议。[U01]

**动态能力真实。** MiniDescriptor（精简模式描述串）可以运行期构建MiniTable，但它不是随便写一段字符串，也不是完整`.proto`源码。MiniTable省掉名称、选项等很多语义信息；业务配置仍需字段编号、类型、单位、缺省值/存在性、版本和来源字段映射。嵌套消息和proto2枚举要链接子表；只有平坦数值+bytes的基准不能代表复杂模式演进。[U02,U10,U11]

可选两条初始化路径：一是在冷路径用固定版本的MiniDescriptor生成器或已有MiniDescriptor构表；另一条是加载FileDescriptorProto到`upb_DefPool`，查MessageDef，再取得`upb_MessageDef_MiniTable()`。第二条更便于保留字段名称/完整描述信息；两条都不应在每条消息热路径重新创建schema。[U02,U10,U11]

**快速解析表需要记录构建条件。** v36.2的动态MiniTable builder在`UPB_FASTTABLE`开启时也能建立快速表；关闭时走另一条路径。不能默认“动态一定慢”，也不能不报编译器/宏/架构就宣称动态与预生成表一定同速。现代C++ Protobuf也有表驱动解析实现，不是upb独享“查表”这一概念。[U12,P05]

## 官方C++ Protobuf也能动态选择字段

应区分三种需求：

1. **固定已知字段全集，配置选择输出子集。** 已生成的Message可只给选中字段赋值；完整C++ runtime（运行库）的Reflection（反射接口）能按FieldDescriptor设置/清除字段，冷路径先把名字解析成字段指针。`FieldMaskUtil::MergeMessageTo/TrimMessage`还能按配置做字段投影。并非只能手写bitmask（位掩码）开关；但“先填满再Trim”是更重的API路径，不应默认它代表最优基线。[P03,P04]
2. **新schema在运行期加载，避免重新生成C++类。** `DescriptorPool + DynamicMessageFactory + Reflection`支持基于描述符创建消息。描述符池、factory与消息对象有明确的存活顺序要求；其开销应单列为dynamic-reflection基线，不能与generated-setter基线混成一个“Protobuf”。[P03,P06]
3. **新业务字段根本不存在于当前IngressRecord或提取器中。** 换任何线格式都不会凭配置生成该值。必须有可扩展数据来源、通用字段字典或插件；“不重编”只对预先支持的字段/类型/提取能力成立。

还需区分输出投影与输入选择解析：给Protobuf配置FieldMask不代表`ParseFromArray`自动只解析选中字段；若专门写wire scanner（线格式扫描器）跳过未选字段，应作为独立实现，与通用对象解析分开命名。

## ProtoEmitOp的定位与兼容范围

预先把配置编译成`{preencoded_tag,source_offset/getter,wire_type}`指令数组，按指令直接向目标buffer写标准Protobuf，是合理优化：省中间Message对象、字段名查找和部分重复大小计算，但仍需校验容量、source字段范围、长度/varint、presence（存在性）、字节序和配置计划生命周期。

它是**自研Protobuf写入器的一个有限子集**，不是只写几十行便自动具备完整Protobuf语义。当前只实现fixed32/64与bytes可以声明“这些schema产生标准兼容wire”，不能笼统承诺所有嵌套、map、repeated/packed、oneof、枚举、必需字段、扩展字段都支持。ProtoEmitOp本身是编码策略，不提供独立的标准解码器；“其decode耗时”必须说明实际使用哪个解析器（官方C++、upb或自研scanner）。[P01,P02]

## 对参考性能解释的修正

- 变长整数编码需要额外操作，但不表示每个字段都会发生分支预测失败。字段号/长度分布稳定时分支可预测，库实现也有优化快路径。“超过128B或field id15就必然罚3–5ns”没有普遍依据；长度从128起即需两字节前缀，而不是必须大于128。[P01]
- 两字节tag的自定义格式也有type判断、长度/边界判断和字段分派；参考代码自身有分支，不能标注“零分支、未知字段1时钟周期”。
- upb alias相较标准拥有型字符串解析省拷贝，但生命周期更严格，输出对象语义并不相同。应同时测alias与copy（复制）路径，报告额外buffer占用和复用时机，避免把借用与拥有的不同契约包装成纯算法胜负。[U05,U06]
- 官方C++某些序列化入口先算大小再写，但有已缓存大小的公开API；其前提是消息自上次大小计算后未变化，不能修改后仍沿用旧值。不能普遍声称所有C++序列化每次不可避免完整两遍。[P07]
- C++字符串view访问器本身不等于`ParseFromArray`零拷贝借用网络输入。比较时要看真实field存储类型与解析入口，不能仅凭API返回view就推断alias。[P08]

## 给主基准的最小可复现实验建议

基准应固定同一逻辑记录、4096条变化输入、相同读取结果和输出边界策略。建议至少列：自定义TV/TLV_checked、ProtoEmitOp_fixed_pb_checked、官方generated-C++_config-setters、官方dynamic-C++_reflection、upb_dynamic_copy与upb_dynamic_alias；若测预生成MiniTable再单列，不用它冒充dynamic。

每种对照配置分开报告：全部字段、去掉payload、少量scalar、稀疏字段、若干确定的配置计划按批次切换。配置构建/发布成本另测，不塞进逐条热路径，也不能把配置选择完全编译掉后声称已经测试动态计划。类型/字段源映射在冷路径验证，热切换以不可变计划和安全回收保证旧批次仍可访问。

计时操作至少区分：encode（包含从原记录取值和填Message）、decode_all（读所有字段和载荷长度/首尾）、decode_few、roundtrip；upb direct-output与复制进独立输出buffer分开列。源Arena、输出buffer、解码Arena必须互不破坏；alias输出只在源活着时消费。检查返回状态、checksum（校验和）与未选字段缺失/存在性，不仅比较单个id。

非空载荷的alias对照还应在正确性阶段检查StringView指针是否确实落在本次输入buffer范围内，不能仅凭设置了flag便宣布省掉复制。复用Message时，配置从全集切到稀疏子集必须清除上次已设置而此次未选中的字段，避免旧值继续被序列化；“只按新计划写选中字段”不自动清除既有对象的字段。测试`all→sparse→all`并检查未选字段缺失，清理工作的热路径成本也必须计入。

正确性在计时前覆盖高32位非零u64、0默认值、长度0/127/128/16383/16384/65535/65536、field id15/16及16383/越界、未知/重复字段、未知wire type、截断报文、容量不足、Arena耗尽、配置切换与跨标准解析器互通。性能可沿用单线程绑核、预热、5次独立批均值和原始CSV；这些是建议，并非本核查已经运行的结果。

## 本轮版本与构建核查

官方release列表当前显示Protobuf v36.2为稳定版，v37.0-rc1为预发布版；本核查固定v36.2头文件，不把main分支当稳定API。之前3.21.12基线与新v36.2 upb若并列，须标明运行库代际也是变量，最好新增同代generated-C++对照。[U13]

v36.2的CMake定义`protobuf_BUILD_LIBUPB`、`libupb`静态target和`protobuf::libupb`别名，且链接`utf8_range`。构建时应遵循固定tag顶层CMake配置/依赖，而非假定系统已有一个通用稳定`libupb.a`。生成器、反射或protoc需要的依赖比仅运行时更多；可用CMake target传播所需链接，不手工漏依赖。详细定位与链接见同目录`sources.md`。[U14]

以下仅是供实现者使用的配置示意，本核查**没有执行**：

```sh
cmake -S <protobuf-v36.2-source> -B <build-dir> \
  -DCMAKE_BUILD_TYPE=Release \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
  -Dprotobuf_BUILD_LIBUPB=ON
cmake --build <build-dir> --target libupb
```

具体依赖要按该tag的顶层CMake解决。不要把`protobuf_BUILD_LIBPROTOBUF=OFF`顺手加入上述配置：v36.2的选项校验明确会要求同时关闭LIBUPB。上例关闭protoc生成器，只面向动态MiniTable的运行时对照；若需要同版generated-C++基线，另启用并使用匹配版本protoc生成`.pb.cc`，不能用旧生成代码直接与新运行库拼接而假定兼容。[U14]
