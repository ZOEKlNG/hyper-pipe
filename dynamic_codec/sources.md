# 动态编码核查的官方来源

核查日期：2026-10-29。所有“已核查”链接均在本轮实际打开；源码固定Protobuf v36.2。这里只摘取支持结论的接口/契约，不把来源中的概括性能文字当本任务实测。未运行构建或性能测试。

## upb

| ID | 精确来源 | 支持的结论 |
|---|---|---|
| U01 | [upb README，v36.2](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/README.md#L1-L39) | C API/ABI不稳定、无独立稳定C库release；可使用生成API或反射；支持运行期加载模式 |
| U02 | [MiniDescriptor decode.h，v36.2](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/mini_descriptor/decode.h) | 公开upb_MiniTable_Build(data,len,arena,status)四参；platform在下划线入口；复杂字段要链接子表；返回NULL/Status须检查 |
| U03 | [Arena公开头，v36.2](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/mem/arena.h) | Init外部块+NULL allocator为固定容量；有初始外部块时不可任意融合/延长；Free结束Arena生命周期 |
| U04 | [Arena实现，v36.2](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/mem/arena.c#L505-L613) | Init在外部块写状态并对齐；Free的引用与块回收；不是带live对象的reset指针。固定外部块不被系统free也不等于可覆盖其中有效wire |
| U05 | [wire/decode.h，v36.2](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/wire/decode.h) | kUpb_DecodeOption_AliasString引用string及unknown输入而非复制；decode status选项与失败处理 |
| U06 | [String setter实现，v36.2](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/message/internal/accessors.h#L757-L768) / [公开accessors.h](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/message/accessors.h) | SetString不复制；调用者负责value数据在msg生命周期内有效；scalar/string setter返回bool |
| U07 | [MiniTable内部结构约束](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/mini_table/internal/message.h#L61-L76) / [公共字段查找接口](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/mini_table/message.h#L23-L29) | 用户不得直接访问成员；FindFieldByNumber/GetFieldByIndex/FieldCount |
| U08 | [wire/encode.c](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/wire/encode.c#L7-L26) / [编码实现](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/wire/internal/encoder.c#L719-L805) | 倒序写入避免预计算长度；encoder输出来自其分配器并返回指针，失败状态要检查 |
| U09 | [back_alloc实现](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/wire/internal/back_alloc.c) / [头文件](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/wire/internal/back_alloc.h) | 编码buffer从Arena管理的空间取得，可增长/保留；生命周期不能独立假设 |
| U10 | [官方upb包装指南](https://github.com/protocolbuffers/protobuf/blob/v36.2/docs/upb/wrapping-upb.md#L87-L200) / [MiniDescriptor编码入口](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/mini_descriptor/internal/encode.h) | MiniTable不是完整descriptor，运行期构表真实；内部MiniDescriptor encoder应固定版本，不假装稳定公共协议 |
| U11 | [DefPool加载描述符](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/reflection/def_pool.h#L78-L82) / [MessageDef取得MiniTable](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/reflection/message_def.h#L105-L110) | upb_DefPool_AddFile与upb_MessageDef_MiniTable路径；用反射描述信息初始化后缓存热路径字段 |
| U12 | [动态MiniTable fasttable构建](https://github.com/protocolbuffers/protobuf/blob/v36.2/upb/mini_descriptor/decode.c#L747-L775) | UPB_FASTTABLE条件下运行期也会BuildTable，具体编译配置必须披露 |
| U13 | [官方releases](https://github.com/protocolbuffers/protobuf/releases) / [v36.2](https://github.com/protocolbuffers/protobuf/releases/tag/v36.2) | 本轮查到稳定v36.2与预发布v37.0-rc1；upb随Protobuf版本固定，不另造upb稳定release编号 |
| U14 | [顶层CMake选项](https://github.com/protocolbuffers/protobuf/blob/v36.2/CMakeLists.txt#L26-L40) / [libupb CMake](https://github.com/protocolbuffers/protobuf/blob/v36.2/cmake/libupb.cmake) | protobuf_BUILD_LIBUPB、静态libupb、protobuf::libupb、utf8_range链接依赖；C API/ABI应私有封装 |

## 官方C++ Protobuf与线格式

| ID | 来源 | 支持的结论 |
|---|---|---|
| P01 | [Protobuf encoding](https://protobuf.dev/programming-guides/encoding/) | `(field_number<<3)|wire_type`、varint、fixed32/64、bytes长度、字段顺序与线格式大小；自研写出器要遵守有限范围的标准语义 |
| P02 | [Field Presence](https://protobuf.dev/programming-guides/field_presence/) | 隐式/显式presence、零默认值省略；动态字段配置应明确“未选/缺失/零值” |
| P03 | [Message / Reflection官方C++ API](https://protobuf.dev/reference/cpp/api-docs/google.protobuf.message/) | generated Message也能反射设置/清除字段；完整runtime与MessageLite不同 |
| P04 | [FieldMaskUtil](https://protobuf.dev/reference/cpp/api-docs/google.protobuf.util.field_mask_util/) | MergeMessageTo、TrimMessage是配置化字段投影工具；不等于ParseFromArray自动选择性解析 |
| P05 | [现代C++表驱动解析实现，v36.2](https://github.com/protocolbuffers/protobuf/blob/v36.2/src/google/protobuf/generated_message_tctable_lite.cc) | C++也有TcParser表驱动实现，不能把“查表”描述成upb独享机制 |
| P06 | [DynamicMessageFactory官方C++ API](https://protobuf.dev/reference/cpp/api-docs/google.protobuf.dynamic_message/) / [descriptor使用技巧](https://protobuf.dev/programming-guides/techniques/) | 无重新生成C++类的运行期描述符消息；DescriptorPool/factory/message生命周期要求 |
| P07 | [MessageLite序列化API](https://protobuf.dev/reference/cpp/api-docs/google.protobuf.message_lite/#MessageLite.SerializeWithCachedSizes.details) | 已缓存大小的序列化入口存在；使用前消息不可变化，否则违约；不能泛称每次必完整双遍 |
| P08 | [C++ String View APIs](https://protobuf.dev/reference/cpp/string-view/) | view getter接口与消息存储/输入alias不是同一承诺，不能凭返回类型声称零拷贝parse |

检索限制：GitHub部分HTML视图最初只展示页面导航，随后均使用同一官方仓库的固定tag raw源文件核对实际声明/注释/实现。未依赖第三方upb教程或附件中的预估ns数字。链接中的行号用于定位，落地若选不同Protobuf tag应重新检查API签名、生成器、宏与依赖。
