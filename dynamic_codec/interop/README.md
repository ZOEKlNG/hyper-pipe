# Java互操作功能验证

本轮168条消息、15种codec逐字段比对全部通过；详见 [RESULTS.md](RESULTS.md) 与 [完整方法及边界](METHOD.md)。这是功能测试，不是Java性能基准。

```bash
bash prepare.sh
python3 run_interop.py --fixtures results/fixtures.input.jsonl
```

使用仓库已有官方Java生成源码与ECJ；缺少Protobuf Java运行库时，准备脚本下载固定版本3.21.12至本目录`.deps/`。生成的`classes/`及缓存均被忽略，不纳入Git。原始导出fixture、日志、SHA-256和精确历史命令保留在`results/`。
