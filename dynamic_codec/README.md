# 动态字段编码基准

[阅读本轮结果](RESULTS.zh-CN.md)。新增方案：自定义 Hybrid TV/TLV、预编译字段指令表直接输出标准 Protobuf、官方 Google upb。主计时矩阵15个适配器、296组×5轮=1480行；另有90行精确长度编码补充。这里只测局部编码/字段读取，不含网络和 Flink。

## 构建与验证

需要Linux x86-64小端、CPU0可用、GCC14/C++20、Python3.12+、Java17+、curl、ar、Protobuf C++3.21兼容运行库及开发头。精确依赖提取脚本位于仓库 `deps/fetch.py`，它不替换系统运行库。

从仓库根开始：

```bash
python3 deps/fetch.py              # 已有匹配开发包可跳过
cd dynamic_codec
python3 upb/build_runtime.py        # 官方v29.3源码下载+SHA验证+实际C编译
bash build.sh
./bench --self-test --output smoke_final
bash interop/prepare.sh
python3 interop/run_interop.py --fixtures smoke_final/fixtures.jsonl
bash custom/test.sh
```

`upb/link_flags.sh`按自身目录定位源码与静态库。原始下载包、源树、可执行文件、classes和依赖缓存不进Git。upb用私有静态库，与系统C++ libprotobuf版本独立；C API/ABI不稳定，需固定版本。

## 串行计时

确认其它编译与负载停止后：

```bash
python3 run_benchmark.py
# 补充：同一程序内比较原检查版、精确长度优化版、官方动态选字段基线
DEP_ROOT="${DEP_ROOT:-$PWD/../deps/root}"
g++ -O3 -DNDEBUG -std=c++20 -march=native -DHAVE_CUSTOM \
  -I"$DEP_ROOT/usr/include" optimized_emit.cpp ../codec/event.pb.cc \
  -L"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -lprotobuf -pthread -o optimized_emit
./optimized_emit
python3 summarize.py
```

每组至少100ms×5轮，打乱次序。主脚本记录实际源码/二进制SHA256前后快照、CPU配额统计和进程时间。`summary.csv`为5个批平均耗时的median/min/max，不是单条消息p99。

## 不重新编译地换字段

编辑或另存 `plans.json`；配置只引用已有10字段注册表，不接受任意offset。`full`仍应保留10字段，作为静态全量对照；`subset`可任意选择或重排既有字段，例如仓库中的 `alternate_plans.json`：

```bash
./bench --self-test --config alternate_plans.json --output alternate_validation
```

它把subset改成value1、producer、payload，不重新构建二进制。记录在 `results/config_without_recompile.json`。`mixed`只模拟三个不可变计划引用逐消息交替，不是并发配置发布与回收的生产实现。

## 文件

- `common.hpp`、`plan_compiler.hpp`、`pb_adapters.hpp`、`bench.cpp`：统一语料、JSON计划、官方生成类及DynamicMessage基线。
- `custom/`：两个手写编码原型、checked视图解析、功能测试和语义限制。
- `upb/`：官方34个C源构建脚本、固定commit/SHA、MiniTable/Arena适配器与所有权测试。
- `interop/`：独立Java官方PB/自定义Hybrid互操作验证。
- `results/benchmark.csv`、`summary.csv`、`optimized_encode.csv`、`plan_compile.csv`：原始测量和摘要。
- `results/build_snapshot_*.json`、`run_timing.json`、`validation.json`：计数与运行证据。
- `research_audit.md`、`sources.md`、`review.md`：参考核查和独立评审。

本轮upb主矩阵使用预配64KiB Arena并显式拒绝扩容；其编码输入payload上限8192B。自定义Hybrid的单bytes字段上限65535B。这些是不同的实现/实验限制，不能混称格式限制。

所有编码输出为独立std::string；解码的自定义视图与upb别名可能借用原始wire，不能异步保留后马上回收原缓冲。源码注明的安全检查/类型子集属于合同，不能把原型称为完整Protobuf替代库。
