# 有状态会话、多模块SPSC和生命周期实测

先读 [实测结论](RESULTS.zh-CN.md)；[方案覆盖表](COVERAGE.zh-CN.md) 将用户参考的每个编号对应到已有或新增证据。没有把上游模型预测的0.8/1.5/2.5ns写成实测。

最终主表采用**显式optional子消息＋官方mutable string assign**，区别于[第一版基线](baseline_implicit/README.md)的隐式子消息与bytes指针长度setter。两项区别及对旧报告复制账本的修正见[API核查](PROTOBUF_3_21_NOTES.zh-CN.md)。

所有布局是归一化原型，不是各方案元数据体积的理论最小值；共用控制结构中有未压缩的成员。不能以本原型RSS宣称某种生产布局必然最省内存。

## 工作负载与公平边界

这是C++20、同进程真实双线程与官方Protobuf 3.21.12的可执行原型，不是DPDK、HTTP/TLS协议栈、真实网卡或跨进程通信测试。Worker是生产线程，Logger是日志线程，SPSC是单生产者单消费者队列；主测4096个活跃会话位置、4096个队列槽、逻辑CPU0/1，另有CPU0/2敏感性对照。

每会话经过TLS提取、HTTP提取、风险日志、关闭汇总四阶段，输出四次且标量在各阶段变化；6个会话常驻字符串只写一次，后续输出借用不可变字节。HTTP阶段另有32B栈上临时XFF，所有方案都须复制以便输入立即覆写。第三条日志和汇总只选择子集。每轮按4096个活跃位置轮转，不是一个会话连续输出完才处理下一个。

主三模块归一化schema为Common两个固定64位标量、TLS两个标量与两个字符串、HTTP两个标量与四个字符串，共12个常驻字段；全量HTTP日志另加XFF为13字段。稀疏配置选择4字段。扩展模块各两个标量、两个字符串；每个导出基类用统一最多四字符串的88B布局，不把48B等示意尺寸当本实现实际尺寸。模块状态继承导出基类，另含64B引擎私有状态。每case预先配置1/2/3/5/8个协议状态节点，阶段掩码模拟协议识别/字段就绪；节点从池预留，不把真实解析器的识别与动态节点分配算入测量。

因此本轮回答给定归一化字段布局的存储/交接/回收问题，不是任意真实.proto的性能保证。Common/TLS/HTTP都用同一生成Module类型，有助于固定后端工作量；生产可改成不同大小的类型后复跑。

## 三种计量

- `worker_assembly`：已有字段→最终槽位，包含本次临时字段复制；H6包括不可变版本构造。没有队列等待、存储提取和生命周期引用协议。
- `worker_descriptor_only_frozen`：仅H6，源导出版本已在计时外冻结，实际只组装描述符和公共元数据。不能拿它替代会变化的源所需快照成本。
- `worker_lifecycle_local`：单CPU轮转4096会话，包含字符串提取、四阶段更新、组装、关闭清理和重用；消费视为本地立即完成。**不包含L1/L2/L3跨核生命周期协议**。微基准按存储/槽位/长度/模块/规则去重，不能用其比较引用计数的跨核成本。
- `logger_encode`：稳定槽位→读取描述符/字段→官方setter→ByteSizeLong→最终wire（编码字节），没有网络发送或ParseFromArray验证。
- `pipeline`：完整有状态双CPU服务容量，包含真实队列、跨核生命周期、关闭、最后排空和回收。队列满自旋，drop=0；Worker/Logger总CPU列**包含忙等**，不可冒充上述隔离服务时间。L3另有满队列drop-new（丢新记录）的独立安全压力测试。

微基准每轮至少80ms；pipeline每轮至少300ms；每组5次，报告批平均中位数与范围，没有测消息p99。配置随机打乱，同一case的五次重复连续执行；不声称全矩阵外层重复随机化。

## 所有权、回收与一致性

H1完整复制业务字节，队列记录自包含，所以用L0立即回收；其余方式借用会话字符串。H5只冻结标量和字符串指针，**不会冻结指针所指字节**；若真实业务会重复改写URL等字段，必须使用不可变字符串版本或深拷贝。

H6原始可变导出指针不能给出发出时快照。主实现为每个会话保留四阶段的不可变导出版本，未来更新只改当前状态；版本构造计入前台，版本内存计入元数据。只读指针的另一种适用条件是源导出对象确实不再改变。

四版本只适用于本模型恰好四次输出；生产中同阶段重复发日志，需要每次输出独立版本或其他正确的回收协议。扩展性case统一保留根消息的临时字段控制项，模块数量不包含该项。源码View是显式ptr/len POD（普通数据结构），不假定std::string_view内部ABI布局。

L1管理池对象的shared_ptr租约，每个会话创建控制块；槽位复制租约，消费后释放。最后引用在Worker时直接本地重置；在Logger时先重置M2字符串/状态，再通过反向环归还，控制块可在Logger侧析构。不是每次new/delete整个Session的另一个存储模型。

L2在最终关闭记录被消费后重置Session并通过反向环归还，也重置槽位元数据。本容量路径保证关闭记录被接受；**没有实现最后关闭记录被丢时的独立控制通知协议**，不能直接为L2开启drop-new。

L3 Logger不写槽位载荷/元数据，仍须release写SPSC消费计数；它不是“零原子操作”。Worker检查`tail>last_seq`，否则在该最后记录槽位的Worker私有retiring字段挂钩，下一次复用槽位时回收；Logger不读取这个字段。停机时必须在排空后扫描全部钩子并归还部分活跃会话。每个会话仅发向同一个FIFO队列、计数无64位回绕是本原型前提。

池预留`4096活跃位置 + 队列容量 + 1`个Session，覆盖延迟释放预算；所有方案用相同数量控制对象。引用计数、反向环和顺风车的额外开销体现在完整pipeline，不能仅按拷贝字节数排名。

## 存储、容量和分配

M1在关闭时真正析构并重新构造string；M2只clear保留capacity。M3按32/64/128/256/512/1024B大小级从64KiB、64B对齐页切槽。M4从同类页池拿256/512/2048B块，按会话顺序追加；块列表用复用vector维护。单字段超过块尺寸时使用向上整倍数的连续大块，不把跨块字节伪装成连续string_view。

Slab/arena池由Worker拥有、跨其所有Session共享，**不是多个Worker无同步并发访问的全局池**。池预热时所有Session同时持有数据，再归还，以预留全部真实容量。M1也预热堆分配器，但关闭仍释放字串。

`pool_reserved_bytes`是实际申请并保留的页字节，`retained_string_heap_bytes`不把SSO（短字符串内嵌存储）空间重复算成堆；`session_metadata_bytes`统计Session及几类主要容器容量，未冒称全部malloc开销。`ring_bytes`是固定槽位实际分配。`process_maxrss_kib`是该case子进程峰值，包含输入、预热、PB、元数据和分配器保留，不等于载荷占用。

`storage_alloc.csv`另列单个完整会话的逻辑请求、实际槽/块/字符串capacity，以及地址跨度和覆盖缓存行；这是布局观察，不能证明硬件预取的因果。分配审计用独立二进制拦截C++ operator new/delete，**不是所有malloc调用**；不把审计计数开销混入主性能。

## 构建、运行

需要Linux x86_64、GCC14或支持C++20的兼容编译器、Python3标准库、CMake>=3.20，以及Protobuf3.21.12开发包/兼容库。生成PB文件已提交。支持仓库deps提取头文件/库，也支持通过DEP_ROOT指向另一份兼容安装。

```bash
DEP_ROOT="$PWD/../deps/root" bash build.sh
python3 check.py
./build/proto_bench --check
./build/semantics_demo
# 会覆盖结果，建议在独立checkout运行；计时前先结束编译与其他CPU负载。
python3 run.py
python3 recheck.py
cmake -S extras -B build-extras -DDEP_ROOT="$PWD/../deps/root"
cmake --build build-extras -j 1
python3 run_extras.py
# 下述消毒器验证可单独执行；修改生命周期源码后应重做。
python3 summarize.py
python3 plots.py # 可选图形生成，需要matplotlib。
```

没有CMake时可安装系统CMake或`python3 -m pip install --user cmake`，并通过`CMAKE=/path/to/cmake`指定。改变PB版本需用匹配protoc重新生成并将其视为新版本实验。

```bash
cmake -S . -B build-asan -DSANITIZE=ON -DDEP_ROOT="$PWD/../deps/root"
cmake --build build-asan --target bench -j 1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 check.py build-asan/bench
python3 stress.py
```

ASan（地址消毒器）/UBSan（未定义行为消毒器）与投毒检查是有限执行证据，不是“100%无悬垂指针”的形式化证明，也不是数据竞争检测器。性能热循环关闭投毒和完整字段比较；验证模式实际解析PB并比较所有选中/未选中字段、完整字节、代际标量、计数守恒和最终池唯一归还。

完整运行顺序是先编译/普通检查，串行完成主计时、独立进程复查、extras计时，再运行消毒器并生成报告；避免把编译或消毒器负载与性能测量重叠。报告脚本会读取已保存的验证记录；这些记录的二进制与源码版本必须对应，不能拿旧验证为修改后的代码背书。

`proto.csv`保留原泛型fixture，mixed分支有重复TLS刷新；最终分析只用其单detail行，mixed以`extras/proto_mixed.cpp`每detail填一次的补测替代。`proto_canonical.csv`逐行标明源文件。补测同时验证wire内容一致，未悄悄删除原样本。

原始snapshot枚举了schema、核心C++源和二进制，但未单列生成的.pb.cc/.pb.h。`results/generated_validation.json`是事后用固定protoc再生成的SHA一致性证据，不追称为原始测量前后捕获。

`results/assembly_build.txt`从实测二进制提取组装函数及编译器专门化版本，能看到掩码扫描的tzcnt和批量向量访存；它不是缓存命中、硬件预取或某条指令耗时的计数证据。编译器可因版本、ISA（指令集）和布局改变输出。
