# 参考方案逐项覆盖

本轮只把已有实测作为索引，不把不同消息、所有权、工作集或版本的纳秒数混在一个排行榜。当前工作目录最终交付为仓库 `stateful_handoff/`。

| 参考编号 | 覆盖方式 | 证据 |
|---|---|---|
| S1 固定C布局 / SBE | 已有实测；本轮不重复测无变化的纯codec | [首轮报告](../研究报告.md)、[SBE](../sbe/README.md) |
| S2 Hybrid TV/TLV | 已有运行期字段选择、借用解码和正确性实测 | [动态编码报告](../dynamic_codec/RESULTS.zh-CN.md) |
| S3 ProtoEmitOp | 已有检查版、优化版和官方PB互操作实测 | [动态编码报告](../dynamic_codec/RESULTS.zh-CN.md) |
| S4 Google upb | 已有运行期MiniTable、别名/复制两条解析路径 | [动态编码报告](../dynamic_codec/RESULTS.zh-CN.md) |
| S5 官方fixed32/fixed64 PB | 已有动态掩码实测；本轮继续用官方生成代码输出多子消息 | [旧报告](../dynamic_codec/RESULTS.zh-CN.md)、[新schema](events.proto) |
| S6 DynamicMessage | 已有反射编解码实测 | [动态编码报告](../dynamic_codec/RESULTS.zh-CN.md) |
| P1 oneof / P2 并列子消息 | 新增稳定协议、逐条交替、64条成块、P2混合；实际C++ new/delete计数另跑 | [proto.csv](results/proto.csv)、[分配计数](results/proto_alloc.csv) |
| M1 新建销毁string | 新增完整会话四日志/即时关闭；真实字符串销毁重建 | [pipeline.csv](results/pipeline.csv)、[分配计数](results/storage_alloc.csv) |
| M2 池化string容量复用 | 同样生命周期及同样业务字节；预热所有池对象 | 同上 |
| M3 大小分级slab | 新增64KiB页、32/64/128/256/512/1024B分级空闲槽 | 同上；[实现](storage.hpp) |
| M4 链式会话arena | 新增256/512/2048B块；超长单字段采用连续大块例外 | 同上；[实现](storage.hpp) |
| H1 深拷贝 | 新增有状态/多日志基线；载荷自包含，可立即回收会话，实际用L0 | [micro.csv](results/micro.csv)、[pipeline.csv](results/pipeline.csv) |
| H2 混合紧凑带 | 新增tag＋标量/借用指针/临时内联字节；两端实际组装/解析 | 同上 |
| H3 选择填字段 | 新增countr_zero掩码扫描，直接写最终槽位选中成员 | 同上 |
| H4 整块镜像快照 | 新增固定8模块镜像整体复制；源字段直接维护在镜像布局中 | 同上 |
| H5 模块快照 | 新增导出基类子集快照、模块描述符、私有引擎字段分离 | 同上；[槽位代码](slots.hpp) |
| H6 模块指针 | 原始可变指针有功能反例；主测为每次输出保留不可变版本的安全实现，计入版本构造成本；另测来源已冻结时只填描述符 | 同上；[反例](validation/consistency.json) |
| 上轮D：池化PB对象移交 | 已有不同消息/旧bytes API实测。本轮没有将旧D数字与新H1～H6拼成同条件排名；API额外复制成本已作修正 | [旧交接报告](../core_handoff/RESULTS_SERVICE.zh-CN.md)、[API修正](PROTOBUF_3_21_NOTES.zh-CN.md) |
| 1/2/3/5/8模块 | 新增H5/H6完整组装、Logger编码与双CPU接力 | 同上，筛选groups=scaling |
| L1 shared_ptr | 新增池化Session lease（租约）的原子引用计数，最后引用归还；存储固定M2 | 同上，筛选groups=lifecycle |
| L2 日志侧重置＋返回 | 新增真实反向SPSC，Logger重置并归还；主测为不丢弃的饱和容量。最后关流记录可丢时需要额外控制协议，本原型不声称支持 | 同上 |
| L3 Worker前置重置＋顺风车回收 | 新增last_seq检查、槽位退休钩子、最终排空/停机扫描、满队列丢新记录；实际ASan/UBSan和投毒压力验证 | [验证目录](validation/) |
| 单独比较Logger多写元数据 | 新增相同L3回收下的logger_zero=0/1控制组，避免把L2/L3整体差值全归因于缓存行写入 | 同上，筛选groups=reset_control |
| M1分配器锁争用假说 | 追加M1/M2在1/2/4逻辑CPU下的独立会话构建/回收速率；没有锁等待硬件/运行时归因 | [allocator_scaling.csv](results/allocator_scaling.csv) |
| 不同字段跨大小级 | 追加12/32/64/128/256/512B混合的单CPU本地Worker/Logger与完整内容核验 | [mixed_lengths.csv](results/mixed_lengths.csv) |
| 隐式/显式presence与bytes API | 追加P0隐式、P1 oneof、P2显式optional，交叉两种公开API；计数与计时分别运行 | [proto_canonical.csv](results/proto_canonical.csv) |
| 吞吐稳定性 | 追加22配置外层随机交错、独立进程5重复，保留主矩阵与复查全部范围 | [recheck.csv](results/recheck.csv) |
| JSON三库 | 上轮已完成，不混入本轮状态槽位排名 | [JSON报告](../json_compare/RESULTS.zh-CN.md) |

参考中的“upb已淘汰”“某种做法彻底排除”属于其建议，不是跨机器、版本、消息类型和功能语义都成立的实验结论。用户已选择官方固定宽度PB，因此本轮固定该编码后端来研究其他维度。

参考摘取的旧数值也有口径混用：主动态掩码PB满字段为45.40ns，后续优化补充二进制的官方对照才约42.8ns；不得与另一轮Hybrid数字拼成严格单变量差值。旧报告保留了各轮原始值及所有权差别。
